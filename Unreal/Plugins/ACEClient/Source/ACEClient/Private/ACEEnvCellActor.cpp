#include "ACEEnvCellActor.h"
#include "ACEEffectLightSubsystem.h"
#include "ACERuntimeOptions.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "ACEDatSubsystem.h"
#include "ACEPlayerController.h"
#include "ACERegionSceneryActor.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEScriptComponent.h"
#include "ACETypes.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACEEnvCellMeshBuilder.h"
#include "ProceduralMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"

namespace
{
	uint64 GEnvStabBudgetFrame = MAX_uint64;
	double GEnvStabBudgetUsedSec = 0.0;
	constexpr double GEnvStabGlobalBudgetSec = 0.002;
	TMap<TWeakObjectPtr<UWorld>, TMap<int32, TWeakObjectPtr<AACEEnvCellActor>>> GEnvCollisionCells;
	void UnregisterCollisionCell(AACEEnvCellActor* Actor)
	{
		const TWeakObjectPtr<UWorld> World(Actor->GetWorld());
		if (auto* Cells = GEnvCollisionCells.Find(World))
		{
			if (Cells->FindRef(Actor->EnvCellId).Get() == Actor) Cells->Remove(Actor->EnvCellId);
			if (Cells->IsEmpty()) GEnvCollisionCells.Remove(World);
		}
	}
}

bool AACEEnvCellActor::HasActiveCollision(UWorld* World, int32 CellId)
{
	const auto* Cells = GEnvCollisionCells.Find(World);
	const auto* Actor = Cells ? Cells->FindRef(CellId).Get() : nullptr;
	return Actor && !Actor->IsActorBeingDestroyed() && Actor->bAppliedCollisionActive && Actor->bCollisionCooked;
}

AACEEnvCellActor::AACEEnvCellActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("EnvCellRoot"));
	SetRootComponent(Root);

	CellMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("CellMesh"));
	CellMesh->bPreferCachedDraws = true;
	// PView already selects room shells from this frame's eye and portal apertures.
	// Reusing a previous GPU occlusion result can hide a newly admitted room for
	// a frame, exposing the sky through its doorway while the camera turns.
	CellMesh->bDisableOcclusionQueries = true;
	CellMesh->SetupAttachment(Root);
	CellMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CellMesh->SetCastShadow(false);
	CellMesh->bCastDynamicShadow = false;
	CellMesh->bCastFarShadow = false;
	CellMesh->bCastInsetShadow = false;
	CellMesh->bUseAsOccluder = false;
	CellMesh->bNeverDistanceCull = true;
	// PView draws EnvCells with useSunlight=0. Keep the outdoor directional light
	// on channel 0; channel 1 admits dynamic interior/particle lights.
	CellMesh->SetLightingChannels(false, true, false);

	CellCollisionMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("CellCollisionMesh"));
	CellCollisionMesh->SetupAttachment(Root);
	CellCollisionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CellCollisionMesh->SetCollisionObjectType(ECC_WorldStatic);
	CellCollisionMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	CellCollisionMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	CellCollisionMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
	CellCollisionMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	CellCollisionMesh->CanCharacterStepUpOn = ECB_Yes;
	CellCollisionMesh->SetVisibility(false);
	CellCollisionMesh->SetHiddenInGame(true);
	CellCollisionMesh->SetCastShadow(false);

	PortalStencilMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PortalStencilMesh"));
	PortalStencilMesh->SetupAttachment(Root);
	PortalStencilMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PortalStencilMesh->SetCastShadow(false);
	PortalStencilMesh->SetVisibility(false);
	PortalStencilMesh->SetHiddenInGame(true);
	PortalStencilMesh->bRenderInMainPass = false;
	PortalStencilMesh->bRenderInDepthPass = false;
	PortalStencilMesh->SetRenderCustomDepth(true);
	PortalStencilMesh->SetCustomDepthStencilValue(1);
}

bool AACEEnvCellActor::EnsureCollisionCooked(bool bWalkableDrawMesh, bool bAllowAsync)
{
	(void)bAllowAsync;
	if (bCollisionCooked)
	{
		return true;
	}

	UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		if (UWorld* World = GetWorld())
		{
			GI = World->GetGameInstance();
		}
	}
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat)
	{
		return false;
	}

	const FACEBuiltEnvCellMesh* MeshPtr = Dat->FindEnvCellMesh(static_cast<uint32>(EnvCellId), WorldScale);
	if (!MeshPtr)
	{
		Dat->RequestEnvCellMesh(static_cast<uint32>(EnvCellId), WorldScale);
		return false;
	}
	const FACEBuiltEnvCellMesh& Mesh = *MeshPtr;

	int32 CollSection = 0;
	if (CellCollisionMesh)
	{
		CellCollisionMesh->ClearAllMeshSections();
		CellCollisionMesh->bUseComplexAsSimpleCollision = true;
		// Async cook used to set bCollisionCooked before PhysicsBSP existed, so the
		// occupancy path skipped a real cook and the pawn walked through walls.
		CellCollisionMesh->bUseAsyncCooking = false;
		FACEBuiltMeshSection Combined;
		for (const FACEBuiltMeshSection& Sec : Mesh.CollisionSections)
		{
			if (Sec.IsEmpty() || (Sec.bFullyTransparent && !Sec.bClipMap))
			{
				continue;
			}
			bool bBadIdx = false;
			for (int32 Ti = 0; Ti < Sec.Triangles.Num(); ++Ti)
			{
				if (!Sec.Vertices.IsValidIndex(Sec.Triangles[Ti]))
				{
					bBadIdx = true;
					break;
				}
			}
			if (bBadIdx)
			{
				continue;
			}
			const int32 VertexBase = Combined.Vertices.Num();
			Combined.Vertices.Append(Sec.Vertices);
			for (int32 Index : Sec.Triangles) Combined.Triangles.Add(VertexBase + Index);
		}
		// PhysicsBSP has no visible material sections. Cook it once, rather than
		// recooking the growing triangle mesh for each draw-surface group on entry.
		if (!Combined.IsEmpty())
		{
			// CEnvCell keeps resident geometry when the player changes cells. Actor
			// collision filters gate participation; the cooked body survives visits.
			CellCollisionMesh->bAlwaysCreatePhysicsState = true;
			CellCollisionMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			CellCollisionMesh->CreateMeshSection_LinearColor(CollSection++,
				Combined.Vertices, Combined.Triangles, {}, {}, {}, {}, true);
		}
		if (CellMesh)
		{
			CellCollisionMesh->SetRelativeTransform(CellMesh->GetRelativeTransform());
		}
	}

	// Empty connector cells are valid after the DAT build completes. Their support
	// comes from adjacent transit cells; never invent collision from their portal art.
	bCollisionCooked = CollSection > 0 || Mesh.CollisionSections.IsEmpty();

	return bCollisionCooked;
}

void AACEEnvCellActor::PrefetchCollisionCookAsync()
{
	EnsureCollisionCooked(true, true);
}

void AACEEnvCellActor::SetEnvCellCollisionActive(bool bActive, bool bWalkableDrawMesh, bool bAllowAsync)
{
	if (bActive)
	{
		if (!EnsureCollisionCooked(bWalkableDrawMesh, bAllowAsync))
		{
			return;
		}
	}
	if (bCollisionStateApplied && bAppliedCollisionActive == bActive
		&& bAppliedWalkableDrawMesh == bWalkableDrawMesh)
	{
		return;
	}
	bCollisionStateApplied = true;
	bAppliedCollisionActive = bActive;
	bAppliedWalkableDrawMesh = bWalkableDrawMesh;
	SetActorEnableCollision(bActive);
	// SetActorEnableCollision updates the filters for every resident body, including
	// ISM instances. Switching component modes here destroys/recreates those bodies;
	// ConfigurePartCollision(false) also discards and recooks scripted stab sections.
	// Draw geometry and empty PhysicsBSP decorations remain non-colliding.
	ApplyStabPawnCollision(bActive);
}

void AACEEnvCellActor::ApplyStabPawnCollision(bool bEnable)
{
	// Attached actors do not inherit their owner's collision-enabled flag.
	if (StaticSceneryActor) StaticSceneryActor->SetActorEnableCollision(bEnable);
	for (AACERegionSceneryActor* Scenery : AnimatedStabs)
	{
		if (Scenery) Scenery->SetActorEnableCollision(bEnable);
	}
}

void AACEEnvCellActor::SetEnvCellHiddenInGame(bool bHide)
{
	if (IsHidden()==bHide) return;
	if (IsHidden() != bHide) SetActorHiddenInGame(bHide);
	if (StaticSceneryActor) StaticSceneryActor->SetActorHiddenInGame(bHide);
	for (AACERegionSceneryActor* Scenery : AnimatedStabs)
	{
		if (!Scenery)
		{
			continue;
		}
		Scenery->SetActorHiddenInGame(bHide);
		if (UACEScriptComponent* Scripts = Scenery->ScriptComponent)
		{
			Scripts->SetComponentTickEnabled(!bHide);
			if (bHide)
			{
				Scripts->StopAllSounds();
			}
		}
	}
}

USceneComponent* AACEEnvCellActor::GetStaticSceneryRoot()
{
	if (!StaticSceneryActor && GetWorld())
	{
		FActorSpawnParameters Params; Params.Owner=this;
		Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		StaticSceneryActor=GetWorld()->SpawnActor<AActor>(AActor::StaticClass(),GetActorTransform(),Params);
		if (StaticSceneryActor)
		{
			auto* Root=NewObject<USceneComponent>(StaticSceneryActor);
			StaticSceneryActor->SetRootComponent(Root); Root->RegisterComponent();
			StaticSceneryActor->SetActorTransform(GetActorTransform());
			StaticSceneryActor->AttachToActor(this,FAttachmentTransformRules::KeepWorldTransform);
			StaticSceneryActor->SetActorHiddenInGame(IsHidden());
			StaticSceneryActor->SetActorEnableCollision(GetActorEnableCollision());
		}
	}
	return StaticSceneryActor ? StaticSceneryActor->GetRootComponent() : nullptr;
}

void AACEEnvCellActor::RefreshStaticVisibility(TFunctionRef<bool(const FBox&)> IsVisible)
{
	// Retail adds scenery to every intersected cell. Room polygons retain their
	// portal visibility; the object's origin alone must not hide a long stair.
	bool Any=false;
	auto Show=[&](UPrimitiveComponent* Part)
	{
		if (!Part) return;
		const bool Visible=!IsHidden() || IsVisible(Part->Bounds.GetBox());
		Part->SetVisibility(Visible); Any|=Visible;
	};
	for (const auto& Pair:StaticObjectHisms) Show(Pair.Value);
	for (UProceduralMeshComponent* Part:StaticObjectMeshes) Show(Part);
	if (StaticSceneryActor) StaticSceneryActor->SetActorHiddenInGame(!Any);
	for (AACERegionSceneryActor* Scenery:AnimatedStabs)
	{
		if (!Scenery) continue;
		FBox Bounds(ForceInit);
		if (Scenery->Appearance) Scenery->Appearance->GetVisualWorldBounds(Bounds);
		const bool Visible=!IsHidden() || (Bounds.IsValid && IsVisible(Bounds));
		if (Scenery->IsHidden()==Visible)
		{
			Scenery->SetActorHiddenInGame(!Visible);
			if (UACEScriptComponent* Scripts=Scenery->ScriptComponent)
			{
				Scripts->SetComponentTickEnabled(Visible);
				if (!Visible) Scripts->StopAllSounds();
			}
		}
	}
}

void AACEEnvCellActor::SetSunCastShadow(bool bCast)
{
	bRequestedSunCastShadow = bCast;
	if (!CellMesh)
	{
		return;
	}
	// The room also occludes its local entrance light when PView hides its draw.
	const bool bDoorwayShadows = ACERuntimeOptions::DynamicLightingQuality() == 2 && !DoorwayLights.IsEmpty();
	const bool bLocalShadows = bDoorwayShadows || (EnvCellId && !bSeenOutside && !ACERuntimeOptions::UniformDungeonLighting() &&
		GetWorld()->GetFeatureLevel() >= ERHIFeatureLevel::SM5 && ACERuntimeOptions::DynamicLightingQuality()==2);
	bCast = bCast || bLocalShadows;
	if (static_cast<bool>(CellMesh->CastShadow) != bCast || CellMesh->bCastDynamicShadow != bCast
		|| CellMesh->bCastHiddenShadow != bLocalShadows)
	{
		CellMesh->bCastHiddenShadow = bLocalShadows;
		CellMesh->SetCastShadow(bCast);
		CellMesh->bCastDynamicShadow = bCast;
		CellMesh->bCastShadowAsTwoSided = bCast;
		CellMesh->bCastInsetShadow = false;
		CellMesh->bCastFarShadow = false;
		CellMesh->bCastContactShadow = false;
		CellMesh->MarkRenderStateDirty();
	}
}

void AACEEnvCellActor::RefreshLightingOptions()
{
	for (UPointLightComponent* Light : DoorwayLights)
		if (IsValid(Light)) Light->SetVisibility(ACERuntimeOptions::DynamicLightingQuality() == 2);
	SetSunCastShadow(bRequestedSunCastShadow);
}

void AACEEnvCellActor::SetLookInDepthBias(float DepthBiasCm)
{
	if (!CellMesh)
	{
		return;
	}
	// PView::DrawCells draws room BSP first, then DrawObjCellForDummies;
	// retail's LESS_EQUAL depth gives coplanar furniture the final pixel.
	// UE sorts opaque draws independently. Recede only room depth by 0.0025
	// AC units to preserve that priority (e.g. Yaraq bench faces 15/16).
	// Drawing and PhysicsBSP vertices remain at their authored positions.
	const float Bias = FMath::Max(DepthBiasCm, WorldScale * 0.0025f);
	if (FMath::IsNearlyEqual(AppliedLookInDepthBias, Bias, 0.001f))
	{
		return;
	}
	AppliedLookInDepthBias = Bias;
	CellMesh->SetDefaultCustomPrimitiveDataFloat(0, Bias);
	CellMesh->MarkRenderStateDirty();
}

bool AACEEnvCellActor::HasPhysicsCollisionMesh() const
{
	return CellCollisionMesh && CellCollisionMesh->GetNumSections() > 0;
}

void AACEEnvCellActor::SetLookInDrawLift(bool bLift)
{
	if (!CellMesh || bLookInDrawLift == bLift)
	{
		return;
	}
	bLookInDrawLift = bLift;
	FVector Loc = CellMeshAuthoredLoc;
	if (bLift)
	{
		Loc.Z += 12.f;
	}
	CellMesh->SetRelativeLocation(Loc);
}

void AACEEnvCellActor::SetCeilingDrawSuppressed(bool bSuppress)
{
	if (!CellMesh || bCeilingDrawSuppressed == bSuppress)
	{
		return;
	}
	bCeilingDrawSuppressed = bSuppress;
	const int32 NumSections = CellMesh->GetNumSections();
	for (int32 i = 0; i < NumSections; ++i)
	{
		// SectionIsCeiling is set only for face-split ceiling buckets — floors that share
		// a SurfaceId with a ceiling stay in a separate section and remain visible.
		const bool bCeiling = SectionIsCeiling.IsValidIndex(i) && SectionIsCeiling[i];
		const bool bVisible = !(bSuppress && bCeiling);
		CellMesh->SetMeshSectionVisible(i, bVisible);
	}
}

void AACEEnvCellActor::SetPortalStencilActive(bool bActive)
{
	const bool bWant = bActive && bHasOutsidePortalStencil;
	if (bPortalStencilActive == bWant) return;
	bPortalStencilActive = bWant;
	if (!PortalStencilMesh)
	{
		return;
	}
	const bool bOn = bPortalStencilActive;
	PortalStencilMesh->SetHiddenInGame(!bOn);
	PortalStencilMesh->SetVisibility(bOn);
	PortalStencilMesh->bRenderInMainPass = false;
	PortalStencilMesh->SetRenderCustomDepth(bOn);
	PortalStencilMesh->SetCustomDepthStencilValue(1);
	PortalStencilMesh->MarkRenderStateDirty();
}

namespace
{
	/** Split a CellStruct surface section into floor/wall vs ceiling by triangle face normal.
	 *  Outdoor peeks hide only the ceiling bucket so floors keep occluding land under shops. */
	void SplitEnvCellSectionByFacing(const FACEBuiltMeshSection& Src, FACEBuiltMeshSection& OutFloorWall,
		FACEBuiltMeshSection& OutCeiling)
	{
		OutFloorWall = FACEBuiltMeshSection();
		OutCeiling = FACEBuiltMeshSection();
		OutFloorWall.SurfaceId = Src.SurfaceId;
		OutFloorWall.bClipMap = Src.bClipMap;
		OutFloorWall.bWrapTexture = Src.bWrapTexture;
		OutFloorWall.bFullyTransparent = Src.bFullyTransparent;
		OutFloorWall.bCollisionOnly = Src.bCollisionOnly;
		OutCeiling.SurfaceId = Src.SurfaceId;
		OutCeiling.bClipMap = Src.bClipMap;
		OutCeiling.bWrapTexture = Src.bWrapTexture;
		OutCeiling.bFullyTransparent = Src.bFullyTransparent;
		OutCeiling.bCollisionOnly = Src.bCollisionOnly;

		auto AppendVert = [](FACEBuiltMeshSection& Dst, const FACEBuiltMeshSection& S, int32 Idx) -> int32
		{
			const int32 NewIdx = Dst.Vertices.Num();
			Dst.Vertices.Add(S.Vertices[Idx]);
			Dst.Normals.Add(S.Normals.IsValidIndex(Idx) ? S.Normals[Idx] : FVector::UpVector);
			Dst.UVs.Add(S.UVs.IsValidIndex(Idx) ? S.UVs[Idx] : FVector2D::ZeroVector);
			Dst.VertexColors.Add(S.VertexColors.IsValidIndex(Idx) ? S.VertexColors[Idx] : FLinearColor::White);
			return NewIdx;
		};

		const int32 TriCount = Src.Triangles.Num() / 3;
		for (int32 Ti = 0; Ti < TriCount; ++Ti)
		{
			const int32 I0 = Src.Triangles[Ti * 3 + 0];
			const int32 I1 = Src.Triangles[Ti * 3 + 1];
			const int32 I2 = Src.Triangles[Ti * 3 + 2];
			if (!Src.Vertices.IsValidIndex(I0) || !Src.Vertices.IsValidIndex(I1) || !Src.Vertices.IsValidIndex(I2))
			{
				continue;
			}
			const FVector FaceN = FVector::CrossProduct(
				Src.Vertices[I1] - Src.Vertices[I0],
				Src.Vertices[I2] - Src.Vertices[I0]).GetSafeNormal();
			// Prefer stored vertex normals: AceVectorToUnreal flips X and reverses winding, so
			// geometric FaceN.Z is inverted vs ACE while transformed normals keep the retail
			// Z sign. Outdoor ceiling suppress must not hide floors as ceilings.
			FVector AvgN = FVector::ZeroVector;
			int32 NCount = 0;
			if (Src.Normals.IsValidIndex(I0)) { AvgN += Src.Normals[I0]; ++NCount; }
			if (Src.Normals.IsValidIndex(I1)) { AvgN += Src.Normals[I1]; ++NCount; }
			if (Src.Normals.IsValidIndex(I2)) { AvgN += Src.Normals[I2]; ++NCount; }
			const float CeilingZ = (NCount > 0) ? (AvgN / static_cast<float>(NCount)).Z : FaceN.Z;
			FACEBuiltMeshSection& Dst = (CeilingZ < -0.35f) ? OutCeiling : OutFloorWall;
			const int32 N0 = AppendVert(Dst, Src, I0);
			const int32 N1 = AppendVert(Dst, Src, I1);
			const int32 N2 = AppendVert(Dst, Src, I2);
			Dst.Triangles.Add(N0);
			Dst.Triangles.Add(N1);
			Dst.Triangles.Add(N2);
		}
	}
}

void AACEEnvCellActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (PendingStaticObjects.Num() == 0)
	{
		SetActorTickEnabled(false);
		return;
	}

	UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		if (UWorld* World = GetWorld())
		{
			GI = World->GetGameInstance();
		}
	}
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat)
	{
		PendingStaticObjects.Reset();
		SetActorTickEnabled(false);
		return;
	}

	bool bOccupant = false;
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (const AACEPlayerController* AcePC = Cast<AACEPlayerController>(PC))
			{
				const uint32 Eff = static_cast<uint32>(AcePC->GetEffectiveCellId());
				bOccupant = Eff != 0 && Eff == static_cast<uint32>(EnvCellId);
			}
		}
	}

	if (GEnvStabBudgetFrame != GFrameCounter)
	{
		GEnvStabBudgetFrame = GFrameCounter;
		GEnvStabBudgetUsedSec = 0.0;
	}
	const double GlobalRemaining = GEnvStabGlobalBudgetSec - GEnvStabBudgetUsedSec;
	if (!bOccupant && GlobalRemaining <= 0.0)
	{
		return;
	}

	const int32 Budget = FMath::Max(1, bOccupant ? 8 : StaticObjectsPerTick);
	const double OwnBudgetSec = FMath::Max(0.5f, StaticObjectTimeBudgetMs) * (bOccupant ? 0.004 : 0.001);
	const double BudgetSec = bOccupant ? OwnBudgetSec : FMath::Min(OwnBudgetSec, GlobalRemaining);
	const double StartSec = FPlatformTime::Seconds();
	int32 Spawned = 0;
	int32 Stalled = 0;
	while (PendingStaticObjects.Num() > 0 && Spawned < Budget)
	{
		const FACEDatStab Item = PendingStaticObjects[0];
		PendingStaticObjects.RemoveAt(0, 1, EAllowShrinking::No);
		if (!TrySpawnOneStaticObject(Dat, Item))
		{
			PendingStaticObjects.Add(Item);
			++Stalled;
			if (Stalled >= PendingStaticObjects.Num())
			{
				break;
			}
			continue;
		}
		++Spawned;
		Stalled = 0;
		if (Spawned > 0 && (FPlatformTime::Seconds() - StartSec) >= BudgetSec)
		{
			break;
		}
	}
	if (!bOccupant)
	{
		GEnvStabBudgetUsedSec += FPlatformTime::Seconds() - StartSec;
	}

	if (PendingStaticObjects.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("ACE: EnvCell 0x%08X static objects finished — %d HISM pools, %d proc"),
			EnvCellId, StaticObjectHisms.Num(), StaticObjectMeshes.Num());
		SetActorTickEnabled(false);
	}
}

void AACEEnvCellActor::EnsureStaticObjectsQueued()
{
	if (bStaticsQueued || DeferredStaticObjects.Num() == 0)
	{
		return;
	}
	UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		if (UWorld* World = GetWorld())
		{
			GI = World->GetGameInstance();
		}
	}
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat)
	{
		return;
	}
	QueueStaticObjects(DeferredStaticObjects, Dat);
	DeferredStaticObjects.Reset();
	bStaticsQueued = true;
}

// Actor ownership/attachment does not destroy separately spawned actors in UE.
// Retail releases these objects with their owning cell/landblock.
void AACEEnvCellActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnregisterCollisionCell(this);
    ClearDoorwayLights();
    ClearStaticObjects();
    Super::EndPlay(EndPlayReason);
}

void AACEEnvCellActor::Destroyed()
{
    UnregisterCollisionCell(this);
    ClearDoorwayLights();
    ClearStaticObjects();
    Super::Destroyed();
}

void AACEEnvCellActor::ClearDoorwayLights()
{
	if (auto* Pool=GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
		Pool->RemoveSource(GetRootComponent());
	DoorwayLights.Reset();
	if (IsValid(DoorwayLightActor)) DoorwayLightActor->Destroy();
	DoorwayLightActor = nullptr;
}

void AACEEnvCellActor::ClearStaticObjects()
{
	PendingStaticObjects.Reset();
	DeferredStaticObjects.Reset();
	bStaticsQueued = false;
	for (UProceduralMeshComponent* Mesh : StaticObjectMeshes)
	{
		if (Mesh)
		{
			Mesh->DestroyComponent();
		}
	}
	StaticObjectMeshes.Reset();
	for (auto& Pair : StaticObjectHisms)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearInstances();
			Pair.Value->DestroyComponent();
		}
	}
	StaticObjectHisms.Reset();
	if (StaticSceneryActor) { StaticSceneryActor->Destroy(); StaticSceneryActor=nullptr; }
	for (AACERegionSceneryActor* Scenery : AnimatedStabs)
	{
		if (Scenery)
		{
			Scenery->Destroy();
		}
	}
	AnimatedStabs.Reset();
	SetActorTickEnabled(false);
}

void AACEEnvCellActor::ConfigureSceneryLighting(UMeshComponent* Part,UACEDatSubsystem* Dat) const
{
	if (!Part || !Part->bRenderInMainPass) return;
	for (int32 I=0; I<Part->GetNumMaterials(); ++I)
	{
		auto* Source=Part->GetMaterial(I);
		Part->SetMaterial(I,bSeenOutside ? Dat->GetUniformInteriorMaterial(Source) : Dat->GetDungeonObjectMaterial(Source));
	}
	Part->SetLightingChannels(bSeenOutside,!bSeenOutside,false);
	Part->bCastDynamicShadow=!bSeenOutside;
	// Portal culling can hide this prop while its shadow still falls into the next room.
	if (!bSeenOutside && Part->IsVisible()) Part->SetCastHiddenShadow(true);
	Part->SetCastShadow(!bSeenOutside);
}

UInstancedStaticMeshComponent* AACEEnvCellActor::GetOrCreateStaticHism(
	UACEDatSubsystem* Dat, uint32 SetupId)
{
	if (TObjectPtr<UInstancedStaticMeshComponent>* Existing = StaticObjectHisms.Find(SetupId))
	{
		return Existing->Get();
	}
	UStaticMesh* StaticMesh = Dat->GetOrCreateSetupStaticMesh(
		SetupId, WorldScale, /*bEnableCollision*/ true, UACEDatSubsystem::ACEPlacementResting);
	if (!StaticMesh)
	{
		return nullptr;
	}
	const FName CompName = *FString::Printf(TEXT("EnvStabISM_%08X"), SetupId);
	auto* SceneryRoot=GetStaticSceneryRoot(); if (!SceneryRoot) return nullptr;
	UInstancedStaticMeshComponent* Hism = NewObject<UInstancedStaticMeshComponent>(StaticSceneryActor, CompName);
	if (!Hism)
	{
		return nullptr;
	}
	Hism->SetupAttachment(SceneryRoot);
	Hism->SetStaticMesh(StaticMesh);
	// Root is Movable (actors relocate with streaming) — Static ISM cannot attach to it.
	Hism->SetMobility(EComponentMobility::Movable);
	Dat->BindSetupStaticMeshMaterials(
		Hism, SetupId, WorldScale, /*bEnableCollision*/ true, UACEDatSubsystem::ACEPlacementResting);
	ConfigureSceneryLighting(Hism,Dat);
	Hism->bNeverDistanceCull = true;
	Hism->SetCullDistances(0.f, 0.f);
	Hism->LDMaxDrawDistance = 0.f;
	Hism->SetCachedMaxDrawDistance(0.f);
	Hism->InstanceStartCullDistance = 0.f;
	Hism->InstanceEndCullDistance = 0.f;
	Hism->bUseAsOccluder = false;
	Hism->bUseDefaultCollision = false;
	// Empty PhysicsBSP must never create an ISM physics state (no instance-body
	// array would exist for subsequent AddInstance calls).
	Hism->bAlwaysCreatePhysicsState = StaticMesh->GetBodySetup() != nullptr;
	Hism->SetCollisionEnabled(StaticMesh->GetBodySetup()
		? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	Hism->SetCollisionObjectType(ECC_WorldStatic);
	Hism->SetCollisionResponseToAllChannels(ECR_Ignore);
	Hism->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Hism->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Hism->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Hism->CanCharacterStepUpOn = ECB_No;
	Hism->RegisterComponent();
	StaticObjectHisms.Add(SetupId, Hism);
	return Hism;
}

bool AACEEnvCellActor::TrySpawnOneStaticObject(UACEDatSubsystem* Dat, const FACEDatStab& Stab)
{
	if (!Dat || Stab.Id == 0)
	{
		return true;
	}

	float IgnoredStep = 0.f, IgnoredH = 0.f, IgnoredR = 0.f;
	uint32 DefaultAnim = 0;
	Dat->TryGetSetupPhysics(Stab.Id, IgnoredStep, IgnoredH, IgnoredR, DefaultAnim);
	uint32 DefaultScript = 0, ScriptTableId = 0, SoundTableId = 0;
	Dat->TryGetSetupRuntimeMetadata(Stab.Id, DefaultScript, ScriptTableId, SoundTableId);
	const bool bPhysicsScript = DefaultScript != 0 && (DefaultScript & 0xFF000000u) == 0x33000000u;
	const bool bNeedsLiveActor = DefaultAnim != 0 || bPhysicsScript;

	const UACEDatSubsystem::EACESetupMeshStatus Status = Dat->RequestSetupMesh(
		Stab.Id, WorldScale, UACEDatSubsystem::ACEPlacementResting);
	if (Status == UACEDatSubsystem::EACESetupMeshStatus::Pending
		|| Status == UACEDatSubsystem::EACESetupMeshStatus::NotReady)
	{
		return false;
	}
	if (Status == UACEDatSubsystem::EACESetupMeshStatus::Failed && !bNeedsLiveActor)
	{
		return true;
	}

	const FQuat WorldQuat = FACEPosition::AceQuatToUnreal(
		Stab.Orientation.W, FVector(Stab.Orientation.X, Stab.Orientation.Y, Stab.Orientation.Z));
	const FVector RelLoc = FACEPosition::AceVectorToUnreal(
		FVector(Stab.Origin.X, Stab.Origin.Y, Stab.Origin.Z), WorldScale);
	const FTransform RelXform(WorldQuat, RelLoc);

	if (bNeedsLiveActor)
	{
		UWorld* World = GetWorld();
		if (!World)
		{
			return true;
		}
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AACERegionSceneryActor* Scenery = World->SpawnActor<AACERegionSceneryActor>(
			AACERegionSceneryActor::StaticClass(), GetActorTransform(), Params);
		if (!Scenery)
		{
			return true;
		}
		Scenery->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
		// Residency can create scripted stabs before their room becomes occupied.
		// Retain the authored PhysicsBSP at creation, just as for static ISMs:
		// ConfigurePartCollision can toggle existing sections, but cannot restore
		// sections omitted by ApplySetupParts(false). Empyrean Rescue's scripted
		// bridge spans otherwise have a floor on login but none after portal entry.
		// Occupancy still gates the actor's collision below (retail CEnvCell objects).
		if (!Scenery->InitializeFromSetup(static_cast<int32>(Stab.Id), 1.f, WorldScale, /*bEnableCollision*/ true))
		{
			Scenery->Destroy();
			return false;
		}
		Scenery->SetActorRelativeTransform(RelXform);
		// Portal loading builds collision stabs while their owning room is hidden.
		Scenery->SetActorHiddenInGame(IsHidden());
		if (IsHidden() && Scenery->ScriptComponent)
		{
			Scenery->ScriptComponent->SetComponentTickEnabled(false);
			Scenery->ScriptComponent->StopAllSounds();
		}
		if (Scenery->Appearance) Scenery->Appearance->ConfigurePartCollision(true, false);
		Scenery->SetActorEnableCollision(bAppliedCollisionActive);
		TArray<UProceduralMeshComponent*> Parts;
		Scenery->GetComponents(Parts);
		for (auto* Part : Parts) ConfigureSceneryLighting(Part,Dat);
		AnimatedStabs.Add(Scenery);
		return true;
	}

	// Shared MeshBuffer analogue: ISM of a cached UStaticMesh (MIC, not MID).
	if (UInstancedStaticMeshComponent* Hism = GetOrCreateStaticHism(Dat, Stab.Id))
	{
		// AddInstance updates rendering and creates just the new body. Collision
		// mode was set before registration; rebuilding every body here is quadratic.
		Hism->AddInstance(RelXform, /*bWorldSpace*/ false);
		return true;
	}

	// ProceduralMesh fallback when StaticMesh cook fails.
	const FName CompName = *FString::Printf(TEXT("EnvStab_%08X_%d"), Stab.Id, StaticObjectMeshes.Num());
	auto* SceneryRoot=GetStaticSceneryRoot(); if (!SceneryRoot) return false;
	UProceduralMeshComponent* Proc = NewObject<UProceduralMeshComponent>(StaticSceneryActor, CompName);
	if (!Proc)
	{
		return true;
	}
	Proc->SetupAttachment(SceneryRoot);
	Proc->bAlwaysCreatePhysicsState = true;
	Proc->RegisterComponent();
	Proc->bUseAsyncCooking = !bAppliedCollisionActive;
	Proc->SetCastShadow(false);
	Proc->bCastDynamicShadow = false;
	Proc->bCastFarShadow = false;
	Proc->bCastInsetShadow = false;
	// Retail CEnvCell::init_static_objects adds stabs as cell objects and FindObjCollisions
	// walks their part PhysicsBSP — furniture / statues / stairs are solid. PhysicsBSP alone
	// often omits stair treads/ramp tops (same gap as EnvCell structs), which left indoor
	// stairs unclimbable: step-up hit the risers with no landable top. Force draw collision
	// like the outdoor building path so visible treads collide too.
	if (!Dat->ApplySetupToProceduralMesh(Proc, static_cast<int32>(Stab.Id), WorldScale, /*bEnableCollision*/ true,
		UACEDatSubsystem::ACEPlacementResting, /*bForceDrawCollision*/ true, /*bIndoorStairCollision*/ true))
	{
		Proc->DestroyComponent();
		return true;
	}
	ConfigureSceneryLighting(Proc,Dat);
	Proc->bUseComplexAsSimpleCollision = true;
	Proc->CanCharacterStepUpOn = ECB_No;
	Proc->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Proc->SetCollisionObjectType(ECC_WorldStatic);
	Proc->SetCollisionResponseToAllChannels(ECR_Ignore);
	Proc->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Proc->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Proc->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Proc->SetRelativeTransform(RelXform);
	Proc->LDMaxDrawDistance = 0.f;
	Proc->SetCachedMaxDrawDistance(0.f);
	StaticObjectMeshes.Add(Proc);
	UE_LOG(LogTemp, Verbose, TEXT("ACEEnvStab: cell=0x%08X setup=0x%08X collide=0 sections=%d"),
		EnvCellId, Stab.Id, Proc->GetNumSections());
	return true;
}

void AACEEnvCellActor::QueueStaticObjects(const TArray<FACEDatStab>& StaticObjects, UACEDatSubsystem* Dat)
{
	// Copy first — ClearStaticObjects resets DeferredStaticObjects, which is often *this* array.
	TArray<FACEDatStab> ToQueue = StaticObjects;
	ClearStaticObjects();
	PendingStaticObjects = MoveTemp(ToQueue);
	bStaticsQueued = true;
	if (Dat)
	{
		for (const FACEDatStab& Stab : PendingStaticObjects)
		{
			if (Stab.Id != 0)
			{
				Dat->RequestSetupMesh(Stab.Id, WorldScale, UACEDatSubsystem::ACEPlacementResting);
			}
		}
	}
	if (PendingStaticObjects.Num() > 0)
	{
		SetActorTickEnabled(true);
	}
}

bool AACEEnvCellActor::LoadEnvCell(int32 InEnvCellId, const FVector& LandblockOrigin, float InWorldScale)
{
	UnregisterCollisionCell(this);
	EnvCellId = InEnvCellId;
	if (!GEnvCollisionCells.Contains(GetWorld()))
	{
		// Editor/test worlds can be collected without ever entering BeginPlay.
		for (auto It = GEnvCollisionCells.CreateIterator(); It; ++It)
			if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	GEnvCollisionCells.FindOrAdd(GetWorld()).Add(EnvCellId, this);
	WorldScale = InWorldScale;
	SetLookInDepthBias(0.f);

	UGameInstance* GI = GetGameInstance();
	if (!GI)
	{
		if (UWorld* World = GetWorld())
		{
			GI = World->GetGameInstance();
		}
	}
	if (!GI)
	{
		UE_LOG(LogTemp, Error, TEXT("ACE: LoadEnvCell 0x%08X — no GameInstance"), EnvCellId);
		return false;
	}

	UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>();
	if (!Dat)
	{
		return false;
	}

	// Footprint build can GetOrBuildEnvCellMesh and rehash EnvCellMeshCache — do it BEFORE
	// taking any cache pointer, then deep-copy the mesh so later Dat calls cannot dangle.
	const FACEBuiltEnvCellMesh* MeshPtr = Dat->FindEnvCellMesh(static_cast<uint32>(EnvCellId), WorldScale);
	if (!MeshPtr || (MeshPtr->IsEmpty() && !MeshPtr->bPortalConnector))
	{
		UE_LOG(LogTemp, Verbose, TEXT("ACE: LoadEnvCell 0x%08X — mesh not ready yet"), EnvCellId);
		return false;
	}
	const FACEBuiltEnvCellMesh Mesh = *MeshPtr; // deep copy — MeshPtr may be invalidated next

	if (!CellMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("ACE: LoadEnvCell 0x%08X — CellMesh null"), EnvCellId);
		return false;
	}

	CellMesh->ClearAllMeshSections();
	CellMesh->bUseAsyncCooking = true;
	CellMesh->bUseComplexAsSimpleCollision = true;
	if (PortalStencilMesh)
	{
		PortalStencilMesh->ClearAllMeshSections();
	}
	bHasOutsidePortalStencil = false;
	bPortalStencilActive = false;
	SectionIsCeiling.Reset();
	bCeilingDrawSuppressed = false;
	int32 SectionIndex = 0;
	int32 MidCount = 0;
	int32 VcCount = 0;
	uint32 SampleSurf = 0;
	TArray<UMaterialInterface*> SectionMaterials;

	auto EmitDrawSection = [&](const FACEBuiltMeshSection& Sec, bool bCeilingBucket)
	{
		if (Sec.IsEmpty() || Sec.bFullyTransparent)
		{
			return;
		}
		if (Sec.Triangles.Num() < 3 || Sec.Vertices.Num() == 0)
		{
			return;
		}
		for (int32 Ti = 0; Ti < Sec.Triangles.Num(); ++Ti)
		{
			if (!Sec.Vertices.IsValidIndex(Sec.Triangles[Ti]))
			{
				return;
			}
		}
		CellMesh->CreateMeshSection_LinearColor(
			SectionIndex,
			Sec.Vertices,
			Sec.Triangles,
			Sec.Normals,
			Sec.UVs,
			Sec.VertexColors,
			TArray<FProcMeshTangent>(),
			/*bCreateCollision*/ false);
		SectionIsCeiling.Add(bCeilingBucket);
		++SectionIndex;
		UMaterialInterface* SecMat = Dat->GetOrCreateEnvCellMaterial(Sec.SurfaceId, Sec.bWrapTexture, !Mesh.CanSeeOutside());
		if (SecMat)
		{
			++MidCount;
			if (SampleSurf == 0)
			{
				SampleSurf = Sec.SurfaceId;
			}
		}
		else
		{
			++VcCount;
		}
		SectionMaterials.Add(SecMat ? SecMat : Dat->GetVertexColorMaterial());
	};

	for (const FACEBuiltMeshSection& Sec : Mesh.Sections)
	{
		if (Sec.IsEmpty() || Sec.bFullyTransparent)
		{
			continue;
		}
		FACEBuiltMeshSection FloorWall;
		FACEBuiltMeshSection Ceiling;
		SplitEnvCellSectionByFacing(Sec, FloorWall, Ceiling);
		EmitDrawSection(FloorWall, /*bCeilingBucket*/ false);
		EmitDrawSection(Ceiling, /*bCeilingBucket*/ true);
	}
	{
		static int32 EnvMatLogCount = 0;
		if (EnvMatLogCount < 8 || VcCount > 0)
		{
			++EnvMatLogCount;
			UE_LOG(LogTemp, Verbose,
				TEXT("ACEEnvMat: cell=0x%08X sections=%d mid=%d vcFallback=%d sampleSurf=0x%08X"),
				EnvCellId, SectionIndex, MidCount, VcCount, SampleSurf);
		}
	}
	for (int32 i = 0; i < SectionMaterials.Num(); ++i)
	{
		CellMesh->SetMaterial(i, SectionMaterials[i]);
	}
	CellMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// Actor sits at the landblock corner. A 120m cap culled shops on the far side
	// of the same 192m landblock — pop-out while standing in the doorway.
	CellMesh->LDMaxDrawDistance = 0.f;
	CellMesh->SetCachedMaxDrawDistance(0.f);
	CellMesh->bNeverDistanceCull = true;
	CellMesh->bUseAsOccluder = false;
	CellMesh->SetCastShadow(false);
	CellMesh->bCastDynamicShadow = false;

	CellMesh->CanCharacterStepUpOn = ECB_No;

	if (CellCollisionMesh)
	{
		CellCollisionMesh->ClearAllMeshSections();
		CellCollisionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		CellCollisionMesh->SetVisibility(false);
		CellCollisionMesh->SetHiddenInGame(true);
	}
	bCollisionStateApplied = false;
	bCollisionCooked = false;

	SetActorLocation(LandblockOrigin);
	const FQuat CellQuat = FACEPosition::AceQuatToUnreal(
		Mesh.Orientation.W, FVector(Mesh.Orientation.X, Mesh.Orientation.Y, Mesh.Orientation.Z));
	const FVector CellLoc = FACEPosition::AceVectorToUnreal(
		FVector(Mesh.Origin.X, Mesh.Origin.Y, Mesh.Origin.Z), WorldScale);
	// Draw and collide share the authored cell frame. A +12–24cm draw-only lift put
	// visual floors above PhysicsBSP and, when the draw mesh was walkable, made the
	// pawn skip at every doorway. The gap under lifted floors also showed LScape
	// through shop interiors from the street.
	const FTransform CellXform(CellQuat, CellLoc);
	CellMeshAuthoredLoc = CellLoc;
	bLookInDrawLift = false;
	CellMesh->SetRelativeTransform(CellXform);
	if (CellCollisionMesh)
	{
		CellCollisionMesh->SetRelativeTransform(CellXform);
	}

	// CustomStencil look-out abandoned — never upload OutsidePortalMesh (dangling Mesh*
	// previously crashed CreateMeshSection here). Keep component empty/hidden.
	if (PortalStencilMesh)
	{
		PortalStencilMesh->SetVisibility(false);
		PortalStencilMesh->SetHiddenInGame(true);
		PortalStencilMesh->SetRenderCustomDepth(false);
	}
	bHasOutsidePortalStencil = false;

	VisibleCells.Reset();
	const uint32 LandblockKey = static_cast<uint32>(EnvCellId) & 0xFFFF0000u;
	for (uint16 Vc : Mesh.VisibleCells)
	{
		VisibleCells.Add(static_cast<int32>(LandblockKey | Vc));
	}
	bSeenOutside = Mesh.CanSeeOutside();
	bHasOutsidePortal = Mesh.HasOutsidePortal();
	ClearDoorwayLights();
	if (!bSeenOutside)
	{
		if (auto* Pool=GetWorld()->GetSubsystem<UACEEffectLightSubsystem>())
		{
			TArray<FACEEffectLightRequest> Fixtures;
			Fixtures.Reserve(Mesh.PointLights.Num());
			for (const auto& Point : Mesh.PointLights)
			{
				auto& Light = Fixtures.AddDefaulted_GetRef();
				Light.Position = Point.BlockPosition;
				Light.Color = Point.Color;
				Light.Intensity = Point.Intensity;
				Light.Radius = Point.Radius;
			}
			Pool->SetCellLights(GetRootComponent(),MoveTemp(Fixtures));
		}
	}
	TArray<FLinearColor> DoorCenters, DoorNormals;
	// Light starts inside the aperture. Closed door meshes and the room walls
	// occlude it; a light outside the door illuminates the street even when shut.
	for (int32 I = 0; I < Mesh.CellPortals.Num() && DoorwayLights.Num() < 2; ++I)
	{
		if (!Mesh.CellPortals[I].IsOutsidePortal() || !Mesh.PortalApertureLocalVerts.IsValidIndex(I)
			|| Mesh.PortalApertureLocalVerts[I].IsEmpty()) continue;
		FVector Center = FVector::ZeroVector;
		for (const FVector& Vertex : Mesh.PortalApertureLocalVerts[I]) Center += Vertex;
		Center /= Mesh.PortalApertureLocalVerts[I].Num();
		if (Mesh.PortalApertureLocalNormals.IsValidIndex(I))
		{
			const auto& X=CellMesh->GetComponentTransform();
			const FVector C=X.TransformPosition(Center);
			const FVector N=X.TransformVectorNoScale(Mesh.PortalApertureLocalNormals[I]).GetSafeNormal();
			const FVector Side(-N.Y,N.X,0);
			float HalfWidth=0, HalfHeight=0;
			for (const FVector& V:Mesh.PortalApertureLocalVerts[I])
			{
				const FVector D=X.TransformPosition(V)-C;
				HalfWidth=FMath::Max(HalfWidth,float(FMath::Abs(FVector::DotProduct(D,Side))));
				HalfHeight=FMath::Max(HalfHeight,float(FMath::Abs(D.Z)));
			}
			DoorCenters.Add(FLinearColor(C.X,C.Y,C.Z,HalfWidth));
			DoorNormals.Add(FLinearColor(N.X,N.Y,N.Z,HalfHeight));
		}
		if (!Mesh.PortalApertureLocalNormals.IsValidIndex(I)) continue;
		const FVector Outward=Mesh.PortalApertureLocalNormals[I].GetSafeNormal();
		Center -= Outward * 100.f;
		if (!DoorwayLightActor)
		{
			FActorSpawnParameters Params;
			Params.Owner = this;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			DoorwayLightActor = GetWorld()->SpawnActor<AActor>(Params);
			if (!DoorwayLightActor) break;
			DoorwayLightActor->Tags.Add(TEXT("ACEDoorwaySpill"));
			auto* LightRoot = NewObject<USceneComponent>(DoorwayLightActor);
			LightRoot->SetMobility(EComponentMobility::Movable);
			DoorwayLightActor->SetRootComponent(LightRoot);
			DoorwayLightActor->AddInstanceComponent(LightRoot);
			LightRoot->RegisterComponent();
			DoorwayLightActor->SetActorTransform(CellMesh->GetComponentTransform());
			DoorwayLightActor->AttachToComponent(CellMesh, FAttachmentTransformRules::KeepWorldTransform);
		}
		// PView can hide the room while its glow is still visible on the street.
		// Actor ownership controls lifetime, not visibility; native light bounds
		// and MaxDrawDistance handle culling independently of room polygons.
		auto* Light = NewObject<USpotLightComponent>(DoorwayLightActor);
		Light->SetupAttachment(DoorwayLightActor->GetRootComponent());
		Light->SetRelativeLocation(Center);
		Light->SetRelativeRotation(Outward.Rotation());
		Light->SetInnerConeAngle(50.f);
		Light->SetOuterConeAngle(70.f);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetCastShadows(true);
		Light->ShadowResolutionScale=.5f;
		Light->SetShadowBias(.15f);
		Light->SetUseInverseSquaredFalloff(false);
		Light->SetIntensityUnits(ELightUnits::Unitless);
		Light->SetLightFalloffExponent(2.f);
		// The aperture centre is above the threshold. At 1.25 / 250cm the
		// falloff left almost no light on the outdoor ground, even at night.
		Light->SetIntensity(18.f);
		Light->SetLightColor(FLinearColor(1.f, .88f, .68f));
		Light->SetAttenuationRadius(550.f);
		Light->SetSourceRadius(8.f);
		Light->SetSoftSourceRadius(16.f);
		Light->SetSpecularScale(0.f);
		Light->SetAffectTranslucentLighting(false);
		Light->SetVolumetricScatteringIntensity(0.f);
		Light->SetLightingChannels(true, true, false);
		Light->MaxDrawDistance = 4000.f;
		Light->MaxDistanceFadeRange = 2000.f;
		Light->SetVisibility(ACERuntimeOptions::DynamicLightingQuality() == 2);
		DoorwayLightActor->AddInstanceComponent(Light);
		Light->RegisterComponent();
		DoorwayLights.Add(Light);
	}
	SetSunCastShadow(false);
	// Blend the final 1.8 m of room illumination into the warm entrance glow.
	// Materials are per cell so one doorway cannot alter other rooms sharing a DAT surface.
	if (!DoorCenters.IsEmpty()) for (int32 I=0; I<CellMesh->GetNumMaterials(); ++I)
	{
		auto* Source=CellMesh->GetMaterial(I);
		if (!Source) continue;
		auto* Material=UMaterialInstanceDynamic::Create(Source->GetMaterial(),this);
		Material->CopyMaterialUniformParameters(Source);
		for (int32 D=0; D<FMath::Min(2,DoorCenters.Num()); ++D)
		{
			Material->SetVectorParameterValue(FName(*FString::Printf(TEXT("Door%d"),D)),DoorCenters[D]);
			Material->SetVectorParameterValue(FName(*FString::Printf(TEXT("Normal%d"),D)),DoorNormals[D]);
		}
		CellMesh->SetMaterial(I,Material);
	}

	// Retail CEnvCell::init_static_objects runs when the cell is in the visible table,
	// not when it is merely streamed. Queueing 20+ shops on outdoor load was the indoor
	// hitch (HISMs + Setup cooks) and left furniture in rooms you had not entered.
	DeferredStaticObjects = Mesh.StaticObjects;
	bStaticsQueued = false;
	SetEnvCellHiddenInGame(true);

	UE_LOG(LogTemp, Log, TEXT("ACE: EnvCell 0x%08X applied — %d draw section(s), %d physics collision section(s), %d static object(s) queued"),
		EnvCellId, SectionIndex, Mesh.CollisionSections.Num(), Mesh.StaticObjects.Num());
	return SectionIndex > 0 || Mesh.CollisionSections.Num() > 0 || Mesh.StaticObjects.Num() > 0
		|| !Mesh.OutsidePortalMesh.IsEmpty() || Mesh.bPortalConnector;
}
