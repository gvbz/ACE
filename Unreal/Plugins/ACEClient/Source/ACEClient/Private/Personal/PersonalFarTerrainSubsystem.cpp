#include "Personal/PersonalFarTerrainSubsystem.h"
#include "Personal/PersonalFarTerrain.h"
#include "Personal/PersonalFarTerrainMesh.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACETerrainPresenterComponent.h"
#include "Async/Async.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "RHI.h"
#include "TextureResource.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"

namespace
{
	TAutoConsoleVariable<int32> CVarFarTerrainWireframe(TEXT("personal.FarTerrain.Wireframe"), 0,
		TEXT("1 draws the far terrain shell as wireframe (editor builds)."));
	TAutoConsoleVariable<int32> CVarFarTerrainFreeze(TEXT("personal.FarTerrain.Freeze"), 0,
		TEXT("1 stops the far terrain shell from following the player."));
}

bool UPersonalFarTerrainSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UPersonalFarTerrainSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	InWorld.GetTimerManager().SetTimer(UpdateTimer, this, &UPersonalFarTerrainSubsystem::Update, UpdateSeconds, true);
}

void UPersonalFarTerrainSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimer);
	}
	if (ShellActor)
	{
		ShellActor->Destroy();
	}
	ShellActor = nullptr;
	ShellMesh = nullptr;
	Super::Deinitialize();
}

void UPersonalFarTerrainSubsystem::SetShown(bool bShown)
{
	if (ShellMesh && ShellMesh->IsVisible() != bShown)
	{
		ShellMesh->SetVisibility(bShown);
	}
}

bool UPersonalFarTerrainSubsystem::EnsureBake(const FString& DatDirectory)
{
	if (Bake)
	{
		return true;
	}
	if (!bBakeRequested)
	{
		bBakeRequested = true;
		PendingBake = PersonalFarTerrainBake::LoadOrBuildAsync(DatDirectory);
	}
	else if (PendingBake.IsValid() && PendingBake.IsReady())
	{
		// A failed bake leaves this null for good: the shell stays off.
		Bake = PendingBake.Get();
		PendingBake = TFuture<PersonalFarTerrainBake::FBakePtr>();
	}
	return Bake.IsValid();
}

bool UPersonalFarTerrainSubsystem::EnsureMesh(UACEDatSubsystem& Dat)
{
	if (ShellMesh && ShellMaterial)
	{
		return true;
	}
	UWorld* World = GetWorld();
	// The land material gives the shell the terrain's fog and day and night lighting.
	UMaterialInterface* Base = Dat.EnsureAceLandMaterialBase();
	if (!World || !Base || !Bake)
	{
		return false;
	}
	if (!ColorTexture && Bake->ColorPixels.Num() > 0)
	{
		// One texel per 24 m cell. Clamped, so everything outside the world takes the edge color.
		constexpr int32 Size = FPersonalFarTerrainBake::ColorTextureSize;
		ColorTexture = FACEDatTextureResolver::CreateTransientRgbaWithMips(Size, Size, Bake->ColorPixels, false,
			TA_Clamp, TA_Clamp, false, false, 0, TF_Trilinear, false);
		// Workers only read the heights. The pixels are not needed again.
		Bake->ColorPixels.Empty();
	}
	ShellMaterial = UMaterialInstanceDynamic::Create(Base, this);
	if (!ColorTexture || !ShellMaterial)
	{
		ShellMaterial = nullptr;
		return false;
	}
	ShellMaterial->SetTextureParameterValue(TEXT("ACETexture"), ColorTexture);
	ShellMaterial->SetScalarParameterValue(TEXT("LandDepthBias"), DepthBiasCm);
	AppliedEmissive = -1.f;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	ShellActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (!ShellActor)
	{
		return false;
	}
	ShellMesh = NewObject<UProceduralMeshComponent>(ShellActor);
	ShellMesh->SetMobility(EComponentMobility::Movable);
	ShellActor->SetRootComponent(ShellMesh);
	ShellMesh->RegisterComponent();
	ShellMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ShellMesh->SetCanEverAffectNavigation(false);
	ShellMesh->SetCastShadow(false);
	ShellMesh->bCastDynamicShadow = false;
	ShellMesh->bUseAsOccluder = false;
	ShellMesh->bNeverDistanceCull = true;
	ShellMesh->SetReceivesDecals(false);
	ShellMesh->SetVisibility(false);
	BuiltRadius = 0;
	return true;
}

void UPersonalFarTerrainSubsystem::ApplyMesh(const TSharedRef<FPersonalFarTerrainMesh, ESPMode::ThreadSafe>& Mesh,
	int32 Radius, TSet<FIntPoint>&& Holes, float WorldScale, TOptional<FIntPoint> InDetailCenter)
{
	bBuilding = false;
	if (!ShellActor || !ShellMesh || !ShellMaterial)
	{
		return;
	}
	ShellActor->SetActorLocation(PersonalFarTerrainMesh::Origin(Mesh->Center, WorldScale));
	// Section 0: flat colors from the color map. Section 1: the part with the detail texture.
	if (Mesh->Triangles.Num() > 0)
	{
		ShellMesh->CreateMeshSection_LinearColor(0, Mesh->Vertices, Mesh->Triangles, Mesh->Normals, Mesh->UVs,
			TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
		ShellMesh->SetMaterial(0, ShellMaterial);
	}
	else
	{
		ShellMesh->ClearMeshSection(0);
	}
	if (DetailMaterial && Mesh->DetailTriangles.Num() > 0)
	{
		ShellMesh->CreateMeshSection_LinearColor(1, Mesh->Vertices, Mesh->DetailTriangles, Mesh->Normals, Mesh->DetailUVs,
			TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
		ShellMesh->SetMaterial(1, DetailMaterial);
	}
	else
	{
		ShellMesh->ClearMeshSection(1);
	}
	BuiltDetailCenter = InDetailCenter;
	bWireframe = false;
	BuiltCenter = Mesh->Center;
	BuiltRadius = Radius;
	BuiltHoles = MoveTemp(Holes);
	// The loaded terrain may have changed while the worker ran.
	Update();
}

bool UPersonalFarTerrainSubsystem::EnsureDetailTexture()
{
	if (DetailTexture && DetailMaterial)
	{
		return true;
	}
	UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = Game ? Game->GetSubsystem<UACEDatSubsystem>() : nullptr;
	UMaterialInterface* Base = Dat ? Dat->EnsureAceLandMaterialBase() : nullptr;
	if (!Base)
	{
		return false;
	}
	if (!DetailTexture)
	{
		// Every mip is there from the start, empty. The worker's uploads fill them.
		constexpr int32 Size = PersonalFarTerrainDetail::TextureSize;
		UTexture2D* Texture = UTexture2D::CreateTransient(Size, Size, PF_B8G8R8A8);
		FTexturePlatformData* PlatformData = Texture ? Texture->GetPlatformData() : nullptr;
		if (!PlatformData || PlatformData->Mips.Num() != 1)
		{
			return false;
		}
		Texture->SRGB = true;
		Texture->CompressionSettings = TC_Default;
		Texture->LODGroup = TEXTUREGROUP_World;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
		Texture->NeverStream = true;
		for (int32 Mip = 0, MipSize = Size; MipSize >= 1; ++Mip, MipSize /= 2)
		{
			if (Mip > 0)
			{
				FTexture2DMipMap* Added = new FTexture2DMipMap();
				Added->SizeX = MipSize;
				Added->SizeY = MipSize;
				PlatformData->Mips.Add(Added);
			}
			FByteBulkData& Bulk = PlatformData->Mips[Mip].BulkData;
			Bulk.Lock(LOCK_READ_WRITE);
			FMemory::Memzero(Bulk.Realloc(int64(MipSize) * MipSize * sizeof(FColor)), int64(MipSize) * MipSize * sizeof(FColor));
			Bulk.Unlock();
		}
		Texture->UpdateResource();
		DetailTexture = Texture;
	}
	DetailMaterial = UMaterialInstanceDynamic::Create(Base, this);
	if (!DetailMaterial)
	{
		return false;
	}
	DetailMaterial->SetTextureParameterValue(TEXT("ACETexture"), DetailTexture);
	DetailMaterial->SetScalarParameterValue(TEXT("LandDepthBias"), DepthBiasCm);
	AppliedEmissive = -1.f;
	return true;
}

void UPersonalFarTerrainSubsystem::UpdateDetail(FIntPoint Center, const FString& DatDirectory)
{
	if (!PersonalFarTerrainDetail::Enabled())
	{
		// The mesh goes back to flat colors. The texture keeps what it has.
		DetailCenter.Reset();
		return;
	}
	if (bDetailBusy || !Bake || (DetailCenter.IsSet() && *DetailCenter == Center))
	{
		return;
	}
	const bool bFirst = !Detail.IsValid();
	if (bFirst)
	{
		Detail = MakeShared<FPersonalFarTerrainDetail, ESPMode::ThreadSafe>();
		DetailTiles = PersonalFarTerrainDetail::MakeDatTileSource(DatDirectory);
	}
	bDetailBusy = true;
	Async(EAsyncExecution::ThreadPool,
		[Weak = TWeakObjectPtr<UPersonalFarTerrainSubsystem>(this), State = Detail, Tiles = DetailTiles, Data = Bake, Center, DatDirectory, bFirst]()
	{
		const double Start = FPlatformTime::Seconds();
		const uint64 Key = PersonalFarTerrainBake::SourceKey(DatDirectory);
		if (bFirst && Key != 0)
		{
			State->LoadTiles(FPersonalFarTerrainDetail::TileCachePath(Key), Key);
		}
		const int32 TilesBefore = State->TileCount();
		auto Uploads = MakeShared<TArray<FPersonalFarTerrainDetail::FUpload>, ESPMode::ThreadSafe>();
		State->Update(*Data, Center, Tiles, *Uploads);
		if (State->UnsavedTiles() > 0 && Key != 0)
		{
			// A failed save only means these tiles are made again next time.
			State->SaveTiles(FPersonalFarTerrainDetail::TileCachePath(Key), Key);
		}
		const double Seconds = FPlatformTime::Seconds() - Start;
		if (bFirst || Seconds > 0.25)
		{
			UE_LOG(LogTemp, Log, TEXT("PersonalFarTerrain: detail texture %.2fs (%d parts, %d new tiles, %d tiles in all)"),
				Seconds, Uploads->Num(), State->TileCount() - TilesBefore, State->TileCount());
		}
		AsyncTask(ENamedThreads::GameThread, [Weak, Center, Uploads]()
		{
			if (UPersonalFarTerrainSubsystem* Self = Weak.Get())
			{
				Self->ApplyDetail(Center, Uploads);
			}
		});
	});
}

void UPersonalFarTerrainSubsystem::ApplyDetail(FIntPoint Center, const TSharedRef<TArray<FPersonalFarTerrainDetail::FUpload>, ESPMode::ThreadSafe>& Uploads)
{
	if (!EnsureDetailTexture())
	{
		// Stays busy, so it is not tried again: the far terrain keeps its flat colors.
		UE_LOG(LogTemp, Warning, TEXT("PersonalFarTerrain: no detail texture; far terrain keeps flat colors"));
		return;
	}
	bDetailBusy = false;
	for (FPersonalFarTerrainDetail::FUpload& Upload : *Uploads)
	{
		// The render thread copies the pixels later. The uploads live until the last copy is done.
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(Upload.X, Upload.Y, 0, 0, Upload.Width, Upload.Height);
		DetailTexture->UpdateTextureRegions(Upload.Mip, 1, Region, Upload.Width * sizeof(FColor), sizeof(FColor),
			reinterpret_cast<uint8*>(Upload.Pixels.GetData()),
			[Uploads](uint8*, const FUpdateTextureRegion2D* Done) { delete Done; });
	}
	DetailCenter = Center;
	Update();
}

void UPersonalFarTerrainSubsystem::Update()
{
	if (CVarFarTerrainFreeze.GetValueOnGameThread() != 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = Game ? Game->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
	const int32 Radius = PersonalFarTerrain::IsOn() ? PersonalFarTerrain::Radius() : 0;
	if (Radius <= 0 || !Dat || !Client)
	{
		SetShown(false);
		return;
	}

	if (!Presenter.IsValid())
	{
		for (TObjectIterator<UACETerrainPresenterComponent> It; It; ++It)
		{
			if (IsValid(*It) && It->GetWorld() == World)
			{
				Presenter = *It;
				break;
			}
		}
	}
	// Outdoors only: not at the login screen, in portal space, or inside a building or dungeon.
	const uint32 Cell = uint32(Client->GetPlayerPosition().CellId);
	const uint32 CellIndex = Cell & 0xFFFFu;
	const UACETerrainPresenterComponent* Terrain = Presenter.Get();
	if (!Terrain || Terrain->LoadRadius <= 0 || CellIndex == 0 || CellIndex >= 0x100u
		|| Dat->IsInPortalSpace() || !Terrain->IsLandblockSpawned(Cell))
	{
		SetShown(false);
		return;
	}
	if (!EnsureBake(Dat->GetDatDirectory()) || !EnsureMesh(*Dat))
	{
		return;
	}

	// Leave out every landblock the client draws itself.
	const FIntPoint Center(int32(Cell >> 24), int32((Cell >> 16) & 0xFFu));
	const int32 HoleRadius = Terrain->LoadRadius + 2;
	TSet<FIntPoint> Holes;
	for (int32 X = FMath::Max(0, Center.X - HoleRadius); X <= FMath::Min(254, Center.X + HoleRadius); ++X)
	{
		for (int32 Y = FMath::Max(0, Center.Y - HoleRadius); Y <= FMath::Min(254, Center.Y + HoleRadius); ++Y)
		{
			if (Terrain->IsLandblockSpawned((uint32(X) << 24) | (uint32(Y) << 16)))
			{
				Holes.Add(FIntPoint(X, Y));
			}
		}
	}
	UpdateDetail(Center, Dat->GetDatDirectory());
	// The mesh uses the detail texture only while the texture has everything the mesh would draw with it.
	TOptional<FIntPoint> WantDetail;
	if (DetailCenter.IsSet() && PersonalFarTerrainDetail::Covers(*DetailCenter, Center))
	{
		WantDetail = DetailCenter;
	}
	const bool bChanged = Center != BuiltCenter || Radius != BuiltRadius || Holes.Num() != BuiltHoles.Num() || !Holes.Includes(BuiltHoles)
		|| WantDetail != BuiltDetailCenter;
	if (bChanged && !bBuilding)
	{
		// The mesh has about 100,000 triangles: build it on a worker, apply it here.
		bBuilding = true;
		const float WorldScale = Terrain->WorldScale;
		Async(EAsyncExecution::ThreadPool,
			[Weak = TWeakObjectPtr<UPersonalFarTerrainSubsystem>(this), Data = Bake, Center, Radius, HoleRadius, WorldScale, WantDetail, Holes = MoveTemp(Holes)]() mutable
		{
			auto Mesh = MakeShared<FPersonalFarTerrainMesh, ESPMode::ThreadSafe>();
			FIntRect DetailCells;
			if (WantDetail.IsSet())
			{
				DetailCells = PersonalFarTerrainDetail::MeshCells(*WantDetail);
			}
			PersonalFarTerrainMesh::Build(*Data, Center, Radius, Holes, HoleRadius, WorldScale, *Mesh, WantDetail.IsSet() ? &DetailCells : nullptr);
			AsyncTask(ENamedThreads::GameThread, [Weak, Mesh, Radius, WorldScale, WantDetail, Holes = MoveTemp(Holes)]() mutable
			{
				if (UPersonalFarTerrainSubsystem* Self = Weak.Get())
				{
					Self->ApplyMesh(Mesh, Radius, MoveTemp(Holes), WorldScale, WantDetail);
				}
			});
		});
	}
	if (BuiltRadius <= 0)
	{
		// Nothing to show until the first mesh arrives.
		return;
	}

	const float Emissive = Dat->GetWorldEmissiveScale();
	if (!FMath::IsNearlyEqual(Emissive, AppliedEmissive, 0.004f))
	{
		AppliedEmissive = Emissive;
		ShellMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"), Emissive);
		if (DetailMaterial)
		{
			DetailMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"), Emissive);
		}
	}
	const bool bWantWireframe = CVarFarTerrainWireframe.GetValueOnGameThread() != 0 && GEngine && GEngine->WireframeMaterial;
	if (bWantWireframe != bWireframe)
	{
		bWireframe = bWantWireframe;
		ShellMesh->SetMaterial(0, bWireframe ? static_cast<UMaterialInterface*>(GEngine->WireframeMaterial) : ShellMaterial);
		if (DetailMaterial)
		{
			ShellMesh->SetMaterial(1, bWireframe ? static_cast<UMaterialInterface*>(GEngine->WireframeMaterial) : DetailMaterial);
		}
	}
	SetShown(true);
}
