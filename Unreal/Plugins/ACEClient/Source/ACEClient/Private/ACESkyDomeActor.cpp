#include "ACESkyDomeActor.h"
#include "ACEProfiling.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACEScriptComponent.h"
#include "ACETypes.h"
#include "Dat/ACERetailGameTime.h"
#include "Dat/ACEDatTextureResolver.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponent.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "SceneInterface.h"
#include "UObject/UObjectIterator.h"
#include "Personal/PersonalSkyHorizon.h" // personal: sky-horizon

namespace
{
	TAutoConsoleVariable<float> CVarSkyFogUpdateHz(TEXT("ace.Sky.FogUpdateHz"),10.f,
		TEXT("World fog parameter update rate. 0 updates every frame; sky motion is always frame-rate independent."));
	/** Retail SkyObject.Properties bits (GameSky). */
	constexpr uint32 SkyProp_After = 0x1u;
	constexpr uint32 SkyProp_Weather = 0x4u;
	constexpr uint32 SkyProp_NoWeatherZ = 0x8u;

	/** Retail weather objects sit at Z = -120 AC unless Properties&8. */
	constexpr float RetailWeatherZAc = -120.f;

	FQuat AcAxisAngleToUnreal(const FVector& AcAxis, float AngleRad)
	{
		const float Half = AngleRad * 0.5f;
		const FVector A = AcAxis.GetSafeNormal() * FMath::Sin(Half);
		return FACEPosition::AceQuatToUnreal(FMath::Cos(Half), A);
	}

	/** GameSky::CalcFrame — heading about Z, then rotation about −Y (BeginAngle→EndAngle). */
	FQuat CalcFrameQuat(float HeadingDeg, float RotationDeg)
	{
		const FQuat HeadingQ = AcAxisAngleToUnreal(FVector(0.f, 0.f, 1.f), FMath::DegreesToRadians(HeadingDeg));
		const FQuat RotQ = AcAxisAngleToUnreal(FVector(0.f, -1.f, 0.f), FMath::DegreesToRadians(RotationDeg));
		// Frame::grotate pre-multiplies the heading by a world-axis rotation.
		return RotQ * HeadingQ;
	}

	bool IsRetailSkyShellGfx(uint32 GfxId)
	{
		return GfxId == 0x010015EEu || GfxId == 0x010015EFu
			|| GfxId == 0x010015F0u || GfxId == 0x010015F1u || GfxId == 0x010015F2u;
	}


}

int32 AACESkyDomeActor::SlotDrawOrder(const FSkySlot& Slot)
{
	if (Slot.bWeather || Slot.bAfterPass) return 2000+Slot.ObjectIndex;
	// Keep the authored order within each layer, but atmospheric sheets must
	// composite over celestial cards. A planet's later DAT index otherwise
	// draws it in front of nearer moving clouds in Unreal's translucent pass.
	const bool Clouds=!FMath::IsNearlyZero(Slot.Def.TexVelocityX) || !FMath::IsNearlyZero(Slot.Def.TexVelocityY);
	return (Clouds ? -1000 : -2000)+Slot.ObjectIndex;
}

AACESkyDomeActor::AACESkyDomeActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	WeatherScripts = CreateDefaultSubobject<UACEScriptComponent>(TEXT("WeatherScripts"));
}

void AACESkyDomeActor::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			DatSubsystem = GI->GetSubsystem<UACEDatSubsystem>();
			ClientSubsystem = GI->GetSubsystem<UACEClientSubsystem>();
		}
	}
	SnapToView();
}

void AACESkyDomeActor::SnapToView()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	FVector CamLoc = FVector::ZeroVector;
	bool bHaveCam = false;
	if (PC->PlayerCameraManager)
	{
		CamLoc = PC->PlayerCameraManager->GetCameraLocation();
		bHaveCam = !CamLoc.IsNearlyZero();
	}
	if (!bHaveCam)
	{
		FRotator Ignored;
		PC->GetPlayerViewPoint(CamLoc, Ignored);
		bHaveCam = !CamLoc.IsNearlyZero();
	}
	if (!bHaveCam)
	{
		if (AActor* View = PC->GetViewTarget())
		{
			CamLoc = View->GetActorLocation();
			bHaveCam = !CamLoc.IsNearlyZero();
		}
	}
	if (!bHaveCam)
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			CamLoc = Pawn->GetActorLocation();
			bHaveCam = !CamLoc.IsNearlyZero();
		}
	}
	if (bHaveCam)
	{
		SetActorLocation(CamLoc);
	}
}

FLinearColor AACESkyDomeActor::ArgbToLinear(uint32 Argb)
{
	const FColor C(
		static_cast<uint8>((Argb >> 16) & 0xFF),
		static_cast<uint8>((Argb >> 8) & 0xFF),
		static_cast<uint8>(Argb & 0xFF),
		static_cast<uint8>((Argb >> 24) & 0xFF));
	return FLinearColor::FromSRGBColor(C);
}

int32 AACESkyDomeActor::PickDayGroup(const FACEDatRegionSky& Sky, double Ticks) const
{
	const int32 NumGroups = Sky.DayGroups.Num();
	if (NumGroups <= 0)
	{
		return INDEX_NONE;
	}
	// SkyDesc::CalcPresentDayGroup hashes GameTime's day and absolute portal year.
	const uint32 Seed = ACERetailGameTime::DaySeed(Ticks, Sky);
	const uint32 H = 0x6A42FDB2u * Seed - 0x7541E9AEu;
	const int32 Group = static_cast<int32>((static_cast<uint64>(H) * static_cast<uint64>(NumGroups)) >> 32);
	if (Group < 0 || Group >= NumGroups)
	{
		return 0;
	}
	return Group;
}

bool AACESkyDomeActor::BracketTimeOfDay(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction,
	int32& OutIdxA, int32& OutIdxB, float& OutAlpha)
{
	OutIdxA = INDEX_NONE;
	OutIdxB = INDEX_NONE;
	OutAlpha = 0.f;
	if (Keys.Num() == 0)
	{
		return false;
	}

	for (int32 i = 0; i < Keys.Num(); ++i)
	{
		if (Keys[i].Begin <= DayFraction)
		{
			OutIdxA = i;
		}
	}

	if (OutIdxA == INDEX_NONE)
	{
		// Before first key — wrap from previous day's last key (retail midnight wrap).
		OutIdxA = Keys.Num() - 1;
		OutIdxB = 0;
		const float SpanStart = Keys[OutIdxA].Begin - 1.f;
		const float Span = FMath::Max(KINDA_SMALL_NUMBER, Keys[OutIdxB].Begin - SpanStart);
		OutAlpha = FMath::Clamp((DayFraction - SpanStart) / Span, 0.f, 1.f);
		return true;
	}

	OutIdxB = (OutIdxA + 1) % Keys.Num();
	const float SpanEnd = (OutIdxB == 0) ? Keys[OutIdxB].Begin + 1.f : Keys[OutIdxB].Begin;
	const float Span = FMath::Max(KINDA_SMALL_NUMBER, SpanEnd - Keys[OutIdxA].Begin);
	OutAlpha = FMath::Clamp((DayFraction - Keys[OutIdxA].Begin) / Span, 0.f, 1.f);
	return true;
}

void AACESkyDomeActor::ClearSky()
{
	for (FSkySlot& Slot : Slots)
	{
		if (Slot.bPesPlaying && WeatherScripts && Slot.PesScriptId != 0)
		{
			WeatherScripts->StopScriptId(Slot.PesScriptId);
			Slot.bPesPlaying = false;
		}
		DestroySlotMesh(Slot);
	}
	Slots.Reset();
	SlotMeshes.Reset();
	SlotMids.Reset();
	ActiveGroup = FACEDatSkyDayGroup();
	bBuilt = false;
	BuiltSkyMeshFormat = 0;
	FogUpdateAccumulator = 0.f;
	LastFogDayFraction = -1.f;
	if (WeatherScripts)
	{
		WeatherScripts->StopAllEffects();
	}
	DestroyColorFillMesh();
}

void AACESkyDomeActor::DestroySlotMesh(FSkySlot& Slot)
{
	if (Slot.Mesh)
	{
		SlotMeshes.Remove(Slot.Mesh);
		Slot.Mesh->DestroyComponent();
		Slot.Mesh = nullptr;
	}
	for (UMaterialInstanceDynamic* Mid : Slot.Mids)
	{
		SlotMids.Remove(Mid);
	}
	Slot.Mids.Reset();
	Slot.SurfaceLighting.Reset();
	Slot.ActiveGfxId = 0;
}

void AACESkyDomeActor::DestroyColorFillMesh()
{
	if (ColorFillMesh)
	{
		ColorFillMesh->DestroyComponent();
		ColorFillMesh = nullptr;
	}
	ColorFillMid = nullptr;
	LastFillColor = FLinearColor::Transparent;
}

void AACESkyDomeActor::EnsureColorFillMesh()
{
	if (ColorFillMesh || !DatSubsystem)
	{
		return;
	}
	UMaterialInterface* Base = DatSubsystem->EnsureAceSkyColorFillMaterialBase();
	if (!Base)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACESky: color-fill material missing — sky backdrop skipped"));
		return;
	}

	ColorFillMesh = NewObject<UProceduralMeshComponent>(this);
	ColorFillMesh->SetupAttachment(GetRootComponent());
	ColorFillMesh->RegisterComponent();
	ColorFillMesh->SetCastShadow(false);
	ColorFillMesh->bCastDynamicShadow = false;
	ColorFillMesh->bCastStaticShadow = false;
	ColorFillMesh->bCastFarShadow = false;
	ColorFillMesh->bCastInsetShadow = false;
	ColorFillMesh->bCastContactShadow = false;
	ColorFillMesh->bUseAsOccluder = false;
	ColorFillMesh->bNeverDistanceCull = true;
	ColorFillMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ColorFillMesh->SetCanEverAffectNavigation(false);
	ColorFillMesh->SetReceivesDecals(false);
	ColorFillMesh->SetBoundsScale(8.f);
	ColorFillMesh->SetTranslucentSortPriority(-3000);
	ColorFillMesh->SetHiddenInGame(false);
	ColorFillMesh->SetVisibility(true);

	const float Half = WorldScale * FMath::Max(0.5f, SkyDistanceScale) * 1080.f;
	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FLinearColor> Colors;
	TArray<FVector> Norms;
	TArray<FVector2D> UVs;
	const FLinearColor Seed(0.7f, 0.75f, 0.9f, 1.f);
	constexpr int32 LatSegments = 10;
	constexpr int32 LonSegments = 20;
	for (int32 Lat = 0; Lat <= LatSegments; ++Lat)
	{
		const float V = static_cast<float>(Lat) / static_cast<float>(LatSegments);
		const float Theta = V * PI;
		const float SinTheta = FMath::Sin(Theta);
		const float CosTheta = FMath::Cos(Theta);
		for (int32 Lon = 0; Lon <= LonSegments; ++Lon)
		{
			const float U = static_cast<float>(Lon) / static_cast<float>(LonSegments);
			const float Phi = U * 2.f * PI;
			const FVector Dir(
				FMath::Sin(Phi) * SinTheta,
				FMath::Cos(Phi) * SinTheta,
				CosTheta);
			Verts.Add(Dir * Half);
			Norms.Add(Dir);
			Colors.Add(Seed);
			UVs.Add(FVector2D(U, V));
		}
	}
	for (int32 Lat = 0; Lat < LatSegments; ++Lat)
	{
		for (int32 Lon = 0; Lon < LonSegments; ++Lon)
		{
			const int32 I0 = Lat * (LonSegments + 1) + Lon;
			const int32 I1 = I0 + 1;
			const int32 I2 = I0 + LonSegments + 1;
			const int32 I3 = I2 + 1;
			Tris.Add(I0);
			Tris.Add(I2);
			Tris.Add(I1);
			Tris.Add(I1);
			Tris.Add(I2);
			Tris.Add(I3);
		}
	}
	ColorFillMesh->CreateMeshSection_LinearColor(0, Verts, Tris, Norms, UVs, Colors,
		TArray<FProcMeshTangent>(), false);
	ColorFillMid = UMaterialInstanceDynamic::Create(Base, this);
	ColorFillMid->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	ColorFillMid->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
	ColorFillMid->SetVectorParameterValue(TEXT("FillColor"), Seed);
	ColorFillMesh->SetMaterial(0, ColorFillMid);
	UE_LOG(LogTemp, Log,
			TEXT("ACESky: color-fill sphere verts=%d half=%.0f (fog-color backdrop)"),
		Verts.Num(), Half);
}

void AACESkyDomeActor::UpdateColorFill(const FLinearColor& Color)
{
	if (!ColorFillMesh)
	{
		EnsureColorFillMesh();
	}
	if (!ColorFillMesh || !ColorFillMid)
	{
		return;
	}
	FLinearColor Fill = Color;
	Fill.A = 1.f;
	if (LastFillColor.Equals(Fill, 0.01f))
	{
		return;
	}
	LastFillColor = Fill;
	ColorFillMid->SetVectorParameterValue(TEXT("FillColor"), Fill);
	ColorFillMid->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	ColorFillMid->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
	static double LastTintLogSec = 0.0;
	const double NowSec = FPlatformTime::Seconds();
	if (NowSec - LastTintLogSec >= 30.0)
	{
		LastTintLogSec = NowSec;
		UE_LOG(LogTemp, Verbose,
			TEXT("ACESky: color-fill tint R=%.2f G=%.2f B=%.2f"), Fill.R, Fill.G, Fill.B);
	}
}

void AACESkyDomeActor::RebuildSlotMesh(FSkySlot& Slot, uint32 GfxId)
{
	if (GfxId == 0 || !DatSubsystem)
	{
		DestroySlotMesh(Slot);
		return;
	}

	const float ShellScale = WorldScale * FMath::Max(0.5f, SkyDistanceScale);
	// Weather/after curtains stay camera-local; before-pass shells push far so depth test
	// lets buildings occlude the cube (DEPTHTEST_ALWAYS is not usable as "draw on top" in UE).
	const float BuildScale = (Slot.bWeather || Slot.bAfterPass) ? WorldScale : ShellScale;
	const auto Built = DatSubsystem->GetOrBuildSetupMeshShared(GfxId, BuildScale);
	if (!Built || Built->IsEmpty())
	{
		// DAT placeholder Setups (e.g. 0x02000714 → null GfxObj 0x010001EC) are intentional
		// empty sky slots. Still stamp ActiveGfxId so UpdateSky does not rebuild every tick.
		if (Slot.ActiveGfxId != GfxId)
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("ACESky: gfx 0x%08X has no drawable geometry (placeholder — skipped)"), GfxId);
		}
		DestroySlotMesh(Slot);
		Slot.ActiveGfxId = GfxId;
		Slot.AuthoredOpacity = 1.f;
		return;
	}

	DestroySlotMesh(Slot);

	FACEDatTextureResolver* Resolver = DatSubsystem->GetTextureResolver();
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(this);
	Mesh->SetupAttachment(GetRootComponent());
	Mesh->RegisterComponent();
	Mesh->SetCastShadow(false);
	Mesh->bCastDynamicShadow = false;
	Mesh->bCastStaticShadow = false;
	Mesh->bCastFarShadow = false;
	Mesh->bCastInsetShadow = false;
	Mesh->bCastContactShadow = false;
	Mesh->bCastVolumetricTranslucentShadow = false;
	Mesh->bUseAsOccluder = false;
	Mesh->bNeverDistanceCull = true;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCanEverAffectNavigation(false);
	Mesh->bUseAsyncCooking = false;
	Mesh->SetReceivesDecals(false);
	Mesh->bAffectDistanceFieldLighting = false;
	Mesh->bAffectDynamicIndirectLighting = false;
	Mesh->SetBoundsScale(Slot.bWeather ? 4.f : 8.f);

	// Before-pass: behind other translucents; depth test vs opaque world.
	// After/weather: composite last, but opaque walls and ground still occlude rain.
	Mesh->SetTranslucentSortPriority(SlotDrawOrder(Slot));

	if (Slot.bForceWeatherZ)
	{
		FVector Location = GetActorLocation();
		Location.Z = RetailWeatherZAc * WorldScale - GetWorld()->OriginLocation.Z;
		Mesh->SetWorldLocation(Location);
	}

	int32 SectionIndex = 0;
	float SlotSurfOpac = 1.f;

	for (const FACEBuiltSetupPart& Part : Built->Parts)
	{
		for (const FACEBuiltMeshSection& Sec : Part.Sections)
		{
			if (Sec.IsEmpty() || Sec.bFullyTransparent || Sec.bCollisionOnly)
			{
				continue;
			}

			bool bAdditive = false;
			bool bSoftAlpha = false;
			bool bClipMap = false;
			UTexture2D* Tex = nullptr;
			const bool bScrolls = !FMath::IsNearlyZero(Slot.Def.TexVelocityX)
				|| !FMath::IsNearlyZero(Slot.Def.TexVelocityY);

			TArray<FVector> Verts = Sec.Vertices;
			TArray<FVector> Norms = Sec.Normals;
			TArray<FVector2D> UVs = Sec.UVs;
			TArray<FLinearColor> Colors = Sec.VertexColors;
			TArray<int32> Triangles = Sec.Triangles;
			for (int32 v = 0; v < Verts.Num(); ++v)
			{
				Verts[v] = Part.BindTransform.TransformPosition(Verts[v]);
				Norms[v] = Part.BindTransform.TransformVectorNoScale(Norms[v]).GetSafeNormal();
			}
			PersonalSkyHorizon::ExtendSection(GfxId, Verts, Triangles, Norms, UVs, Colors); // personal: sky-horizon

			// GameSky draws the authored polygons in DAT order without depth writes.
			// The shared builder already converts winding. Reversing it here exposes
			// the far walls; projecting the cloud cards onto a sphere bends the horizon.
			const bool bSkyShell = !Slot.bWeather && IsRetailSkyShellGfx(GfxId);
			const bool bCloudLayer = bScrolls && !bSkyShell && !Slot.bWeather;
			const bool bNeedWrap = bCloudLayer || GfxId == 0x010015EFu;

			FACEDatDecodedSurface Decoded;
			if (Resolver && Sec.SurfaceId != 0)
			{
				if (Resolver->ResolveSurface(Sec.SurfaceId, Decoded))
				{
					bAdditive = Decoded.bAdditive && !(Decoded.bClipMap && Decoded.bSurfaceTranslucent);
					bSoftAlpha = Decoded.bUsesAlpha || Decoded.bSurfaceTranslucent;
					bClipMap = Decoded.bClipMap;
					if (Slot.bWeather || Slot.bAfterPass)
					{
						// DAT weather surfaces often carry Translucency≈1; using that as
						// OpacityMul made rain curtains invisible.
						SlotSurfOpac = 1.f;
					}
				}
				Tex = Resolver->GetOrCreateSkyUTexture(Sec.SurfaceId, DatSubsystem, bNeedWrap);
			}
			// ACEPolygonMeshBuilder already converts DAT winding and emits authored backs.
			// Reorienting those triangles toward the origin reverses Unreal's front faces
			// and culls the rain cylinder when viewed from inside.

			// Starfield is authored SrcAlpha/InvSrcAlpha: its black background replaces
			// the day shell at night. An additive override leaked the multicolor dome.

			if (!Tex)
			{
				// Vertex colors were already sampled from the DAT surface in the setup
				// cook. Draw the authored cube — do not tessellate/spherize (that made
				// the aurora-curtain sky). Skip empty geometry.
				if (Triangles.Num() < 3 || Verts.Num() < 3)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("ACESky: gfx 0x%08X surface 0x%08X has no texture and no verts — skipped"),
						GfxId, Sec.SurfaceId);
					continue;
				}
				UE_LOG(LogTemp, Warning,
					TEXT("ACESky: gfx 0x%08X surface 0x%08X using vertex-color cube (no UTexture)"),
					GfxId, Sec.SurfaceId);
				if (Colors.Num() != Verts.Num())
				{
					Colors.SetNum(Verts.Num());
					for (FLinearColor& C : Colors)
					{
						C = FLinearColor::White;
					}
				}
				Mesh->CreateMeshSection_LinearColor(SectionIndex, Verts, Triangles, Norms, UVs,
					Colors, TArray<FProcMeshTangent>(), /*bCreateCollision*/ false);
				if (UMaterialInterface* VcBase = DatSubsystem->EnsureAceSkyVertexColorMaterialBase())
				{
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(VcBase, this);
					Mid->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
					Mid->SetScalarParameterValue(TEXT("DiffuseStrength"), 1.f);
					Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
					Mid->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
					Mesh->SetMaterial(SectionIndex, Mid);
					Slot.Mids.Add(Mid);
					Slot.SurfaceLighting.Add(FVector3f(Decoded.Luminosity, Decoded.Diffuse,
						Decoded.bAdditive && !bAdditive ? 1.f - Decoded.Translucency : 1.f));
					SlotMids.Add(Mid);
					++SectionIndex;
				}
				else
				{
					UE_LOG(LogTemp, Warning,
						TEXT("ACESky: gfx 0x%08X vertex-color material missing — skipped section (avoids pink PMC)"),
						GfxId);
					Mesh->ClearMeshSection(SectionIndex);
				}
				continue;
			}

			// Weather keeps its own wrapping/blending; all sky passes test world depth.
			UMaterialInterface* Base = nullptr;
			const bool bDayCube = bSkyShell && GfxId != 0x010015EFu && !bAdditive;

			if (Slot.bWeather && !bDayCube)
			{
				Base = bAdditive
					? DatSubsystem->EnsureAceWeatherAdditiveMaterialBase()
					: DatSubsystem->EnsureAceWeatherTranslucentMaterialBase();
			}
			else if (bAdditive)
			{
				Base = DatSubsystem->EnsureAceSkyAdditiveMaterialBase();
			}
			else if (bNeedWrap)
			{
				Base = DatSubsystem->EnsureAceSkyTranslucentWrapMaterialBase();
			}
			else
			{
				Base = DatSubsystem->EnsureAceSkyTranslucentMaterialBase();
			}
			if (!Base)
			{
				Base = DatSubsystem->EnsureAceSkyVertexColorMaterialBase();
			}
			if (!Base)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("ACESky: gfx 0x%08X surface 0x%08X has no sky material — skipped (avoids pink PMC)"),
					GfxId, Sec.SurfaceId);
				continue;
			}
			Mesh->CreateMeshSection_LinearColor(SectionIndex, Verts, Triangles, Norms, UVs,
				Colors, TArray<FProcMeshTangent>(), /*bCreateCollision*/ false);
			{
				UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
				Mid->SetTextureParameterValue(TEXT("ACETexture"), Tex);
				// A few retail glow images have faint nonblack edge texels. Fade
				// only their outer border; tiled stars/cloud layers keep wrapping.
				Mid->SetScalarParameterValue(TEXT("SkyCardEdgeFade"),bAdditive && !bNeedWrap ? 1.f : 0.f);
				Mid->SetScalarParameterValue(TEXT("SkyAlphaTestReference"), bClipMap && !Decoded.bSurfaceTranslucent ? Decoded.AlphaTestReference : 0.f);
				Mid->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
				Mid->SetScalarParameterValue(TEXT("DiffuseStrength"), 1.f);
				Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
				Mid->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
				Mesh->SetMaterial(SectionIndex, Mid);
				Slot.Mids.Add(Mid);
				Slot.SurfaceLighting.Add(FVector3f(Decoded.Luminosity, Decoded.Diffuse,
					Decoded.bAdditive && !bAdditive ? 1.f - Decoded.Translucency : 1.f));
				SlotMids.Add(Mid);
			}
			if (bDayCube && SectionIndex == 0)
			{
				int32 MeanR = 0, MeanG = 0, MeanB = 0, MeanA = 0;
				const int32 NPix = Decoded.Pixels.Num();
				if (NPix > 0)
				{
					int64 SumR = 0, SumG = 0, SumB = 0, SumA = 0;
					for (const FColor& C : Decoded.Pixels)
					{
						SumR += C.R; SumG += C.G; SumB += C.B; SumA += C.A;
					}
					MeanR = static_cast<int32>(SumR / NPix);
					MeanG = static_cast<int32>(SumG / NPix);
					MeanB = static_cast<int32>(SumB / NPix);
					MeanA = static_cast<int32>(SumA / NPix);
				}
				UE_LOG(LogTemp, Log,
					TEXT("ACESky: day-dome gfx=0x%08X surf=0x%08X %s tex=%dx%d softA=%d meanRGBA=%d,%d,%d,%d"),
					GfxId, Sec.SurfaceId,
					TEXT("authored sky"),
					Tex->GetSizeX(), Tex->GetSizeY(), bSoftAlpha ? 1 : 0,
					MeanR, MeanG, MeanB, MeanA);
			}
			++SectionIndex;
		}
	}

	Slot.Mesh = Mesh;
	Slot.ActiveGfxId = GfxId;
	Slot.AuthoredOpacity = SlotSurfOpac;
	SlotMeshes.Add(Mesh);
}

bool AACESkyDomeActor::TryBuild()
{
	SnapToView();
	if ((ClientSubsystem && !ClientSubsystem->HasGameTime()) || !DatSubsystem || !DatSubsystem->IsDatReady())
	{
		return false;
	}

	const FACEDatRegionSky* Sky = DatSubsystem->GetRegionSkyInfo();
	if (!Sky || !Sky->bValid)
	{
		return false;
	}

	const double Ticks = ClientSubsystem ? ClientSubsystem->GetGameTimeTicks() : 0.0;
	const int32 DayNumber = ClientSubsystem ? ClientSubsystem->GetGameDayNumber() : 0;
	const int32 GroupIdx = PickDayGroup(*Sky, Ticks);
	if (GroupIdx == INDEX_NONE)
	{
		return false;
	}

	ActiveGroup = Sky->DayGroups[GroupIdx];
	BuiltDayNumber = DayNumber;

	UE_LOG(LogTemp, Log,
		TEXT("ACESky: day=%d group=%d/%d name='%s' chance=%.3f ticks=%.0f frac=%.3f"),
		DayNumber, GroupIdx, Sky->DayGroups.Num(), *ActiveGroup.DayName, ActiveGroup.ChanceOfOccur,
		Ticks,
		ClientSubsystem ? ClientSubsystem->GetGameDayFraction(0.5f) : 0.5f);

	if (WeatherScripts)
	{
		WeatherScripts->InitializeForEnvironment(WorldScale);
	}

	for (int32 ObjIdx = 0; ObjIdx < ActiveGroup.Objects.Num(); ++ObjIdx)
	{
		const FACEDatSkyObject& Def = ActiveGroup.Objects[ObjIdx];
		if (Def.DefaultGfxObjectId == 0 && Def.DefaultPesObjectId == 0)
		{
			continue;
		}

		FSkySlot Slot;
		Slot.Def = Def;
		Slot.ObjectIndex = ObjIdx;
		Slot.PesScriptId = Def.DefaultPesObjectId;
		Slot.bAfterPass = (Def.Properties & SkyProp_After) != 0;
		Slot.bWeather = (Def.Properties & SkyProp_Weather) != 0;
		Slot.bForceWeatherZ = Slot.bWeather && (Def.Properties & SkyProp_NoWeatherZ) == 0;

		if (Def.DefaultGfxObjectId != 0)
		{
			RebuildSlotMesh(Slot, Def.DefaultGfxObjectId);
		}

		UE_LOG(LogTemp, Verbose,
			TEXT("ACESky: obj[%d] gfx=0x%08X pes=0x%08X props=0x%X after=%d weather=%d zfix=%d"),
			ObjIdx, Def.DefaultGfxObjectId, Def.DefaultPesObjectId, Def.Properties,
			Slot.bAfterPass ? 1 : 0, Slot.bWeather ? 1 : 0, Slot.bForceWeatherZ ? 1 : 0);

		Slots.Add(MoveTemp(Slot));
	}

	bBuilt = Slots.Num() > 0;
	if (bBuilt)
	{
		BuiltSkyMeshFormat = SkyMeshFormatVersion;
		AppliedSkyMaterialGeneration = DatSubsystem ? DatSubsystem->GetSkyMaterialGeneration() : AppliedSkyMaterialGeneration;
		EnsureColorFillMesh();
		UE_LOG(LogTemp, Log,
			TEXT("ACESky: retail GameSky build '%s' — %d slots, day %d, fmt %d, scale=%.1f"),
			*ActiveGroup.DayName, Slots.Num(), BuiltDayNumber, BuiltSkyMeshFormat,
			WorldScale * SkyDistanceScale);
	}
	return bBuilt;
}

void AACESkyDomeActor::ResolveCelestialState(float DayFraction, int32 IdxA, int32 IdxB, float Alpha,
	int32 ObjIndex, const FACEDatSkyObject& Def, FCelestialState& Out) const
{
	Out = FCelestialState();
	Out.GfxId = Def.DefaultGfxObjectId;

	const bool bTimed = Def.BeginTime != Def.EndTime;
	// SkyDesc::GetSky uses an exclusive interval, or an always-visible object
	// when both times match. Replacement records can supply a different mesh.
	Out.bVisible = !bTimed || (DayFraction > Def.BeginTime && DayFraction < Def.EndTime);
	Out.RotationDeg = bTimed
		? FMath::Lerp(Def.BeginAngle,Def.EndAngle,(DayFraction-Def.BeginTime)/(Def.EndTime-Def.BeginTime))
		: Def.BeginAngle;

	const TArray<FACEDatSkyTimeOfDay>& Keys = ActiveGroup.TimesOfDay;
	if (!Keys.IsValidIndex(IdxA) || !Keys.IsValidIndex(IdxB)) return;
	auto FindReplace = [](const FACEDatSkyTimeOfDay& Key, int32 Index) -> const FACEDatSkyObjectReplace*
	{
		return Key.Replaces.FindByPredicate([Index](const FACEDatSkyObjectReplace& R) { return R.ObjectIndex == Index; });
	};

	// SkyDesc::GetSky applies discrete mesh/heading replacements from the
	// preceding key. Only luminosity, diffusion, and opacity interpolate.
	const FACEDatSkyObjectReplace* A = FindReplace(Keys[IdxA], ObjIndex);
	const FACEDatSkyObjectReplace* B = FindReplace(Keys[IdxB], ObjIndex);
	if (!A) return;
	if (A->GfxObjId != 0) { Out.GfxId = A->GfxObjId; Out.bVisible = true; }
	Out.HeadingDeg = A->Rotate;
	if (B)
	{
		if (A->Transparent >= 0.f && B->Transparent >= 0.f)
			Out.Transparent = FMath::Lerp(A->Transparent, B->Transparent, Alpha);
		if (A->Luminosity > 0.f && B->Luminosity > 0.f)
			Out.Luminosity = FMath::Lerp(A->Luminosity, B->Luminosity, Alpha);
		if (A->MaxBright > 0.f && B->MaxBright > 0.f)
			Out.MaxBright = FMath::Lerp(A->MaxBright, B->MaxBright, Alpha);
	}
	if (Out.Transparent >= 100.f) Out.bVisible = false;
}

void AACESkyDomeActor::ApplySlotFrame(FSkySlot& Slot, const FCelestialState& State)
{
	if (!Slot.Mesh)
	{
		return;
	}
	// GameSky::Update applies CalcFrame to every celestial object. Properties
	// select render pass/position behavior; they do not disable the DAT rotation.
	Slot.Mesh->SetRelativeRotation(CalcFrameQuat(State.HeadingDeg, State.RotationDeg));
	if (Slot.bForceWeatherZ)
	{
		FVector Location = GetActorLocation();
		Location.Z = RetailWeatherZAc * WorldScale - GetWorld()->OriginLocation.Z;
		Slot.Mesh->SetWorldLocation(Location);
	}
}

void AACESkyDomeActor::Tick(float DeltaSeconds)
{
	ACE_PROFILE_SCOPE(Sky);
	Super::Tick(DeltaSeconds);
	// A disconnected account must not keep displaying the previous server's rain.
	// Wait for the new connection's authenticated clock before choosing a day group.
	if (ClientSubsystem && !ClientSubsystem->HasGameTime())
	{
		if (bBuilt) ClearSky();
		return;
	}

	// Retail GameSky is player-centered. Building the fill sphere at world origin paints
	// a horizon band until the first camera snap.
	SnapToView();

	if (!bBuilt)
	{
		RetryAccumulator += DeltaSeconds;
		if (RetryAccumulator >= 0.5f)
		{
			RetryAccumulator = 0.f;
			TryBuild();
		}
		if (!bBuilt)
		{
			return;
		}
	}

	const int32 DayNumber = ClientSubsystem ? ClientSubsystem->GetGameDayNumber() : 0;
	const int32 SkyMatGen = DatSubsystem ? DatSubsystem->GetSkyMaterialGeneration() : AppliedSkyMaterialGeneration;
	if (DayNumber != BuiltDayNumber
		|| BuiltSkyMeshFormat != SkyMeshFormatVersion
		|| SkyMatGen != AppliedSkyMaterialGeneration)
	{
		ClearSky();
		if (!TryBuild())
		{
			return;
		}
	}

	const float DayFraction = ClientSubsystem ? ClientSubsystem->GetGameDayFraction(0.5f) : 0.5f;
	UpdateSky(DayFraction, DeltaSeconds);
}

void AACESkyDomeActor::UpdateSky(float DayFraction, float DeltaSeconds)
{
	const TArray<FACEDatSkyTimeOfDay>& Keys = ActiveGroup.TimesOfDay;
	int32 IdxA = INDEX_NONE;
	int32 IdxB = INDEX_NONE;
	float Alpha = 0.f;
	const bool bHaveTod = BracketTimeOfDay(Keys, DayFraction, IdxA, IdxB, Alpha);

	// Fog broadcasts touch all loaded material caches. Time-of-day changes are
	// slow; keep that work at 10 Hz without throttling head tracking, sky motion,
	// weather simulation or shadow updates. Clock jumps and rebuilds apply now.
	FogUpdateAccumulator += FMath::Max(0.f,DeltaSeconds);
	const float FogHz=CVarSkyFogUpdateHz.GetValueOnGameThread();
	if (bDriveWorldFog && bHaveTod && (FogHz<=0.f || DeltaSeconds<=0.f
		|| LastFogDayFraction<0.f || FMath::Abs(DayFraction-LastFogDayFraction)>.005f
		|| FogUpdateAccumulator>=1.f/FMath::Max(FogHz,1.f)))
	{
		ACE_PROFILE_SCOPE(SkyFog);
		UpdateFog(Keys[IdxA], Keys[IdxB], Alpha);
		FogUpdateAccumulator=0.f;
		LastFogDayFraction=DayFraction;
	}
	if (bDriveWorldLighting && bHaveTod)
	{
		ACE_PROFILE_SCOPE(SkyLighting);
		UpdateWorldLighting(Keys[IdxA], Keys[IdxB], Alpha, DayFraction);
	}

	for (FSkySlot& Slot : Slots)
	{
		FCelestialState State;
		ResolveCelestialState(DayFraction, IdxA, IdxB, Alpha, Slot.ObjectIndex, Slot.Def, State);

		// Outdoor weather / after-pass gated by LScape::weather_enabled equivalent.
		if ((Slot.bWeather || Slot.bAfterPass) && !bWeatherEnabled)
		{
			State.bVisible = false;
		}

		float Opacity = 1.f;
		if (State.Transparent >= 0.f)
		{
			Opacity = 1.f - FMath::Clamp(State.Transparent / 100.f, 0.f, 1.f);
		}

		const bool bDraw = State.bVisible && Opacity > KINDA_SMALL_NUMBER;
		if (Slot.Def.DefaultGfxObjectId == 0x010015EFu)
		{
			static float StarLogAcc = 0.f;
			StarLogAcc += DeltaSeconds;
			if (StarLogAcc >= 5.f)
			{
				StarLogAcc = 0.f;
				UE_LOG(LogTemp, Log,
					TEXT("ACESky: stars %s frac=%.3f trans=%.1f opac=%.2f '%s'"),
					bDraw ? TEXT("ON") : TEXT("OFF"), DayFraction, State.Transparent, Opacity,
					*ActiveGroup.DayName);
			}
		}

		if (!bDraw)
		{
			if (Slot.Mesh)
			{
				Slot.Mesh->SetVisibility(false, true);
				Slot.Mesh->SetHiddenInGame(true, true);
			}
			SyncSlotPes(Slot, false);
			continue;
		}

		if (State.GfxId != 0 && State.GfxId != Slot.ActiveGfxId)
		{
			RebuildSlotMesh(Slot, State.GfxId);
		}

		if (Slot.Mesh)
		{
			Slot.Mesh->SetHiddenInGame(false, true);
			Slot.Mesh->SetVisibility(true, true);
			ApplySlotFrame(Slot, State);
		}
		SyncSlotPes(Slot, true);

		const bool bScrolls = !FMath::IsNearlyZero(Slot.Def.TexVelocityX)
			|| !FMath::IsNearlyZero(Slot.Def.TexVelocityY);
		if (bScrolls)
		{
			Slot.UvOffset.X = FMath::Fmod(Slot.UvOffset.X + Slot.Def.TexVelocityX * DeltaSeconds, 1.f);
			Slot.UvOffset.Y = FMath::Fmod(Slot.UvOffset.Y + Slot.Def.TexVelocityY * DeltaSeconds, 1.f);
			if (Slot.UvOffset.X < 0.f)
			{
				Slot.UvOffset.X += 1.f;
			}
			if (Slot.UvOffset.Y < 0.f)
			{
				Slot.UvOffset.Y += 1.f;
			}
		}

		for (int32 MaterialIndex = 0; MaterialIndex < Slot.Mids.Num(); ++MaterialIndex)
		{
			UMaterialInstanceDynamic* Mid = Slot.Mids[MaterialIndex];
			const FVector3f Surface = Slot.SurfaceLighting[MaterialIndex];
			if (!Mid)
			{
				continue;
			}
			const bool bBackground = !Slot.bWeather;
			Mid->SetScalarParameterValue(TEXT("OpacityMul"),
				Opacity * Slot.AuthoredOpacity * (bBackground ? Surface.Z : 1.f));
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), bBackground
				? (State.Luminosity > 0.f ? State.Luminosity * .01f : Surface.X) : 1.f);
			Mid->SetScalarParameterValue(TEXT("DiffuseStrength"), bBackground
				? (State.MaxBright > 0.f ? State.MaxBright * .01f : Surface.Y) : 1.f);
			if (bHaveTod && bBackground)
			{
				const auto& A = Keys[IdxA];
				const auto& B = Keys[IdxB];
				// These are fixed-function lighting coefficients, not sRGB samples.
				const auto Color = [](uint32 C)
				{
					return FLinearColor(float((C >> 16) & 255) / 255.f,
						float((C >> 8) & 255) / 255.f, float(C & 255) / 255.f);
				};
				const float Bright = FMath::Lerp(A.DirBright, B.DirBright, Alpha);
				// LScape::calc_object_light adds one fifth of sun intensity to ambient.
				const FLinearColor Ambient = FMath::Lerp(Color(A.AmbColor), Color(B.AmbColor), Alpha)
					* (FMath::Lerp(A.AmbBright, B.AmbBright, Alpha) + FMath::Abs(Bright) * .2f);
				const float Heading = FMath::DegreesToRadians(FMath::Lerp(A.DirHeading, B.DirHeading, Alpha));
				const float Pitch = FMath::DegreesToRadians(FMath::Lerp(A.DirPitch, B.DirPitch, Alpha));
				const FVector Direction = FACEPosition::AceVectorToUnreal(FVector(
					FMath::Cos(Pitch) * FMath::Sin(Heading), FMath::Cos(Pitch) * FMath::Cos(Heading),
					FMath::Sin(Pitch)), 1.f);
				Mid->SetVectorParameterValue(TEXT("SkyAmbient"), Ambient);
				Mid->SetVectorParameterValue(TEXT("SkySunColor"),
					FMath::Lerp(Color(A.DirColor), Color(B.DirColor), Alpha) * FMath::Abs(Bright));
				Mid->SetVectorParameterValue(TEXT("SkySunDirection"),
					FLinearColor(Direction.X, Direction.Y, Direction.Z));
                if (WeatherScripts) WeatherScripts->SetEnvironmentLighting(Ambient,
                    FMath::Lerp(Color(A.DirColor),Color(B.DirColor),Alpha)*FMath::Abs(Bright),
                    FLinearColor(Direction.X,Direction.Y,Direction.Z));
			}
			if (bScrolls)
			{
				Mid->SetVectorParameterValue(TEXT("UVOffset"),
					FLinearColor(Slot.UvOffset.X, Slot.UvOffset.Y, 0.f, 0.f));
			}
		}
	}
}

void AACESkyDomeActor::SyncSlotPes(FSkySlot& Slot, bool bVisible)
{
	if (Slot.PesScriptId == 0 || !WeatherScripts)
	{
		return;
	}
	if (bVisible && !Slot.bPesPlaying)
	{
		// GameSky::Draw recursively draws particle children with their owning
		// celestial slot. Later cloud/star layers may cover these particles.
		WeatherScripts->SetEnvironmentScriptDrawOrder(Slot.PesScriptId,SlotDrawOrder(Slot));
		WeatherScripts->PlayScriptId(static_cast<int32>(Slot.PesScriptId), 1.f);
		Slot.bPesPlaying = true;
	}
	else if (!bVisible && Slot.bPesPlaying)
	{
		WeatherScripts->StopScriptId(Slot.PesScriptId);
		Slot.bPesPlaying = false;
	}
}

void AACESkyDomeActor::InvalidateAndRebuild()
{
	ClearSky();
	BuiltDayNumber = INDEX_NONE;
	bDirShadowSettingsApplied = false;
	AppliedShadowSettingsRev = -1;
	LastDirShadowIndoor = -1;
	bHaveAppliedSunRotation = false;
	AppliedDirIntensity = -1.f;
	AppliedSkyIntensity = -1.f;
	TryBuild();
}

void AACESkyDomeActor::SetWeatherEnabled(bool bEnabled)
{
	if (bWeatherEnabled == bEnabled)
	{
		return;
	}
	bWeatherEnabled = bEnabled;
	if (!bWeatherEnabled)
	{
		for (FSkySlot& Slot : Slots)
		{
			if (!Slot.bWeather && !Slot.bAfterPass)
			{
				continue;
			}
			if (Slot.Mesh)
			{
				Slot.Mesh->SetVisibility(false, true);
			}
			SyncSlotPes(Slot, false);
		}
		// Before-pass celestial particles remain visible indoors and when weather
		// is disabled. SyncSlotPes stopped only the gated slots above.
	}
	// Re-enabling is applied by UpdateSky with the current DAT time window.

}

void AACESkyDomeActor::UpdateFog(const FACEDatSkyTimeOfDay& A, const FACEDatSkyTimeOfDay& B, float Alpha)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float MinFog = FMath::Lerp(A.MinWorldFog, B.MinWorldFog, Alpha);
	const float MaxFog = FMath::Lerp(A.MaxWorldFog, B.MaxWorldFog, Alpha);
	const FLinearColor FogColor = FMath::Lerp(ArgbToLinear(A.WorldFogColor), ArgbToLinear(B.WorldFogColor), Alpha);
	const float MinCm = FMath::Max(MinFog, 0.f) * WorldScale;
	const float DatMaxCm = FMath::Max(MinCm + 1.f, MaxFog * WorldScale);
	float MaxCm = DatMaxCm;
	if (FogClampLandblocks > 0.f)
	{
		MaxCm = FMath::Min(DatMaxCm, FogClampLandblocks * 192.f * WorldScale);
	}

	bool bIndoor = false;
	UACEDatSubsystem* Dat = DatSubsystem;
	if (UGameInstance* GI = World->GetGameInstance())
	{
		Dat = GI->GetSubsystem<UACEDatSubsystem>();
		bIndoor = Dat && Dat->IsIndoorEnvironment();
	}

	if (!FogActor)
	{
		if (TActorIterator<AExponentialHeightFog> It(World); It)
		{
			FogActor = *It;
		}
	}
	if (FogActor)
	{
		if (UExponentialHeightFogComponent* Fog = FogActor->FindComponentByClass<UExponentialHeightFogComponent>())
		{
			// Retail fog is linear distance fog on world geometry (MinWorldFog→MaxWorldFog).
			// ExponentialHeightFog painted a lighting gradient across the sky and never reached
			// emissive DAT materials — so the horizon stayed sharp while the sky washed out.
			Fog->SetVisibility(false);
			Fog->SetFogDensity(0.f);
			Fog->SetVolumetricFog(false);
			Fog->SetDirectionalInscatteringColor(FLinearColor::Black);
		}
	}
	if (Dat)
	{
		Dat->SetWorldDistanceFog(MinCm, MaxCm, FogColor, 1.f);
	}
	UpdateColorFill(FogColor);

	static double LastFogLog = 0.0;
	const double Now = FPlatformTime::Seconds();
	if (Now - LastFogLog >= 30.0)
	{
		LastFogLog = Now;
		UE_LOG(LogTemp, Verbose,
			TEXT("ACESky: world-fog min=%.0f max=%.0f amount=%.2f col=(%.2f,%.2f,%.2f) indoor=%d"),
			MinCm, MaxCm, 1.f, FogColor.R, FogColor.G, FogColor.B, bIndoor ? 1 : 0);
	}
}

void AACESkyDomeActor::UpdateWorldLighting(const FACEDatSkyTimeOfDay& A, const FACEDatSkyTimeOfDay& B, float Alpha, float DayFraction)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FACEDatSkyTimeOfDay La = A;
	FACEDatSkyTimeOfDay Lb = B;
	float LerpAlpha = Alpha;
	if (bPersistentAtDay || (ClientSubsystem && (ClientSubsystem->GetCharacterOptions2() & 1u) != 0))
	{
		const TArray<FACEDatSkyTimeOfDay>& Keys = ActiveGroup.TimesOfDay;
		if (Keys.Num() > 0)
		{
			int32 NoonA = 0;
			for (int32 i = 0; i < Keys.Num(); ++i)
			{
				if (Keys[i].Begin <= 0.5f)
				{
					NoonA = i;
				}
			}
			const int32 NoonB = (NoonA + 1) % Keys.Num();
			La = Keys[NoonA];
			Lb = Keys[NoonB];
			const float SpanEnd = (NoonB == 0) ? Keys[NoonB].Begin + 1.f : Keys[NoonB].Begin;
			const float Span = FMath::Max(KINDA_SMALL_NUMBER, SpanEnd - La.Begin);
			LerpAlpha = FMath::Clamp((0.5f - La.Begin) / Span, 0.f, 1.f);
			(void)DayFraction;
		}
	}

	const float DirBright = FMath::Lerp(La.DirBright, Lb.DirBright, LerpAlpha);
	const float HeadingDeg = FMath::Lerp(La.DirHeading, Lb.DirHeading, LerpAlpha);
	const float PitchDeg = FMath::Lerp(La.DirPitch, Lb.DirPitch, LerpAlpha);
	auto Coefficient = [](uint32 C)
    { return FLinearColor(float((C>>16)&255)/255.f,float((C>>8)&255)/255.f,float(C&255)/255.f); };
    const FLinearColor DirColor = FMath::Lerp(Coefficient(La.DirColor),Coefficient(Lb.DirColor),LerpAlpha);
    const FLinearColor AmbColor = FMath::Lerp(Coefficient(La.AmbColor),Coefficient(Lb.AmbColor),LerpAlpha);
    // LScape::calc_object_light: ambient_level + length(sunlight)*0.2.
    const float ObjectAmbient = FMath::Lerp(La.AmbBright,Lb.AmbBright,LerpAlpha)+FMath::Abs(DirBright)*0.2f;

	UACEDatSubsystem* Dat = nullptr;
	if (UGameInstance* GI = World->GetGameInstance())
	{
		Dat = GI->GetSubsystem<UACEDatSubsystem>();
	}
	const bool bIndoor = Dat && Dat->IsIndoorEnvironment();
	if (Dat)
	{
        Dat->SetInteriorAmbient(AmbColor * ObjectAmbient);
	}
	// DirBright is ~0.25 night … 0.80 noon. Do not square — that crushed night to near-black.
	const float Dayness = FMath::Clamp((DirBright - 0.20f) / 0.62f, 0.f, 1.f);
	const float DirIntensity = FMath::Lerp(5.5f, 8.5f, Dayness);
	const float SkyIntensityOut = FMath::Lerp(1.05f, 1.35f, Dayness);
	const float SkyIntensity = SkyIntensityOut;
	const float LandEmis = FMath::Lerp(0.14f, 0.09f, Dayness);
	// Outdoor shells/plants are DefaultLit — keep emissive low so CSM shows on walls/foliage.
	// EnvCells use authored static vertex lights and the retail world ambient separately.
	const float SceneryEmis = ObjectAmbient;

	if (!SunLightActor || !IsValid(SunLightActor))
	{
		SunLightActor = nullptr;
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			if (*It && (*It)->ActorHasTag(FName(TEXT("ACERuntimeLight"))))
			{
				SunLightActor = *It;
				break;
			}
		}
		if (!SunLightActor)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SunLightActor = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		}
	}
	if (SunLightActor)
	{
		SunLightActor->Tags.AddUnique(FName(TEXT("ACERuntimeLight")));
		if (UDirectionalLightComponent* DirEarly = SunLightActor->FindComponentByClass<UDirectionalLightComponent>())
		{
			if (DirEarly->Mobility != EComponentMobility::Movable)
			{
				DirEarly->SetMobility(EComponentMobility::Movable);
			}
		}
		const float HeadingRad = FMath::DegreesToRadians(HeadingDeg);
		const float PitchRad = FMath::DegreesToRadians(PitchDeg);
		const float Cp = FMath::Cos(PitchRad);
		const FVector AcTowardSun(
			Cp * FMath::Sin(HeadingRad),
			Cp * FMath::Cos(HeadingRad),
			FMath::Sin(PitchRad));
		const FVector UnrealTowardSun = FACEPosition::AceVectorToUnreal(AcTowardSun, 1.f).GetSafeNormal();
		FVector LightDir = (-UnrealTowardSun).GetSafeNormal();
		// Night DAT pitch can put the moon under the horizon so upward land faces get N·L≤0
		// and read as fully shadowed. Keep moonlight steep enough for CSM (~25°) — 9°
		// (Z=-0.16) peter-panned large Yaraq casters while character specks still showed.
		if (LightDir.Z > -0.35f)
		{
			LightDir.Z = -0.42f;
			LightDir.Normalize();
		}
		const FRotator SunRot = LightDir.Rotation();
		const bool bSunRotChanged = !bHaveAppliedSunRotation || !SunRot.Equals(AppliedSunRotation, 0.12f);
		if (bSunRotChanged)
		{
			SunLightActor->SetActorRotation(SunRot);
			AppliedSunRotation = SunRot;
			bHaveAppliedSunRotation = true;
		}
		if (UDirectionalLightComponent* Dir = SunLightActor->FindComponentByClass<UDirectionalLightComponent>())
		{
			if (Dir->Mobility != EComponentMobility::Movable)
			{
				Dir->SetMobility(EComponentMobility::Movable);
			}
			const FLinearColor SunColor = DirColor;
			if (!FMath::IsNearlyEqual(AppliedDirIntensity, DirIntensity, 0.04f))
			{
				Dir->SetIntensity(DirIntensity);
				AppliedDirIntensity = DirIntensity;
			}
			if (!AppliedDirColor.Equals(SunColor, 0.008f))
			{
				Dir->SetLightColor(SunColor);
				AppliedDirColor = SunColor;
			}
			Dir->SetVisibility(true);
			static constexpr int32 ShadowSettingsRev = 22;
			static const auto* ShadowQualityCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("sg.ShadowQuality"));
			const int32 ShadowQuality = ShadowQualityCVar ? FMath::Clamp(ShadowQualityCVar->GetInt(), 0, 4) : 3;
			const bool bWantDirShadows = ShadowQuality > 0;
			const int8 DirShadowIndoor = 0;
			if (!bDirShadowSettingsApplied || AppliedShadowSettingsRev != ShadowSettingsRev
				|| LastDirShadowIndoor != DirShadowIndoor || AppliedShadowQuality != ShadowQuality)
			{
				if (AppliedShadowQuality == 0 && bWantDirShadows && World->Scene)
				{
					// UE omits cached shadow draw commands for meshes created or
					// updated at r.ShadowQuality=0. Re-enable those passes once,
					// without recreating geometry buffers or physics components.
					TArray<FPrimitiveSceneProxy*> Casters;
					for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
						if (IsValid(*It) && It->GetWorld() == World && It->IsRegistered() && It->CastShadow && It->SceneProxy)
							Casters.Add(It->SceneProxy);
					ENQUEUE_RENDER_COMMAND(ACERefreshEnabledShadowPasses)(
						[Scene=World->Scene, Casters=MoveTemp(Casters)](FRHICommandListImmediate&)
						{
							for (auto* Proxy : Casters) Scene->UpdateCachedRenderStates(Proxy);
						});
				}
				Dir->bAllowMegaLights = false;
				Dir->SetCastRaytracedShadows(ECastRayTracedShadow::Disabled);
				Dir->bUseRayTracedDistanceFieldShadows = false;
				Dir->SetForceCachedShadowsForMovablePrimitives(false);
				// All casters share the same shadow projection and density.
				Dir->bUseInsetShadowsForMovableObjects = false;
				Dir->SetAtmosphereSunLight(false);
				Dir->SetSpecularScale(0.f);
				Dir->bCastShadowsOnClouds = false;
				if (bWantDirShadows)
				{
					Dir->SetCastShadows(true);
					Dir->SetShadowBias(0.22f);
					Dir->SetShadowSlopeBias(0.35f);
					const bool Mobile=World->GetFeatureLevel()==ERHIFeatureLevel::ES3_1;
					// Mobile switches cascades abruptly by view depth (no desktop
					// blend), so pitching the headset swept a quality line across
					// the world. One 2048 map over 40m retains nearby definition
					// with no internal quality boundary and one shadow pass.
					// Desktop Low/Medium reduce both coverage and shadow passes.
					// High retains the full four cascades and 600m coverage.
					Dir->DynamicShadowDistanceMovableLight = Mobile ? 4000.f : ShadowQuality == 1 ? 10000.f : ShadowQuality == 2 ? 30000.f : 60000.f;
					Dir->DynamicShadowCascades = Mobile || ShadowQuality == 1 ? 1 : ShadowQuality == 2 ? 2 : 4;
					Dir->CascadeDistributionExponent = Mobile ? 1.f : 4.f;
					Dir->CascadeTransitionFraction = 0.1f;
					Dir->ShadowDistanceFadeoutFraction = 0.12f;
					Dir->ShadowAmount = 0.88f;
				}
				else
				{
					Dir->SetCastShadows(false);
				}
				Dir->MarkRenderStateDirty();
				AppliedShadowSettingsRev = ShadowSettingsRev;
				AppliedShadowQuality = ShadowQuality;
				bDirShadowSettingsApplied = true;
				LastDirShadowIndoor = DirShadowIndoor;
			}
		}
	}

	if (!AmbientSkyLightActor || !IsValid(AmbientSkyLightActor))
	{
		AmbientSkyLightActor = nullptr;
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			if (*It && (*It)->ActorHasTag(FName(TEXT("ACERuntimeLight"))))
			{
				AmbientSkyLightActor = *It;
				break;
			}
		}
		if (!AmbientSkyLightActor)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AmbientSkyLightActor = World->SpawnActor<ASkyLight>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		}
	}
	if (AmbientSkyLightActor)
	{
		AmbientSkyLightActor->Tags.AddUnique(FName(TEXT("ACERuntimeLight")));
		if (USkyLightComponent* Sky = AmbientSkyLightActor->FindComponentByClass<USkyLightComponent>())
		{
			// UE's setter queues a six-face sky capture even if the value is
			// already false. Calling it every lighting tick stalls both threads.
			if (Sky->bRealTimeCapture) Sky->SetRealTimeCaptureEnabled(false);
			if (Sky->Mobility != EComponentMobility::Stationary)
			{
				Sky->SetMobility(EComponentMobility::Stationary);
			}
			Sky->bLowerHemisphereIsBlack = false;
			Sky->SetLowerHemisphereColor(AmbColor * 0.55f);
			if (!FMath::IsNearlyEqual(AppliedSkyIntensity, SkyIntensity, 0.03f))
			{
				Sky->SetIntensity(SkyIntensity);
				AppliedSkyIntensity = SkyIntensity;
			}
			if (!AppliedSkyColor.Equals(AmbColor, 0.008f))
			{
				Sky->SetLightColor(AmbColor);
				AppliedSkyColor = AmbColor;
			}
			Sky->SetVisibility(true);
		}
	}

	if (Dat)
	{
		Dat->SetWorldEmissiveScale(LandEmis, SceneryEmis, AmbColor);
	}

	static double LastSkyDiagSec = 0.0;
	const double NowSec = FPlatformTime::Seconds();
	if (NowSec - LastSkyDiagSec >= 30.0)
	{
		LastSkyDiagSec = NowSec;
		UE_LOG(LogTemp, Verbose,
			TEXT("ACESky: dayFrac=%.3f indoor=%d dirI=%.2f skyI=%.2f land=%.2f scenery=%.2f (day=%d group='%s')"),
			DayFraction, bIndoor ? 1 : 0, DirIntensity, SkyIntensity, LandEmis, SceneryEmis,
			BuiltDayNumber, *ActiveGroup.DayName);
	}
}
