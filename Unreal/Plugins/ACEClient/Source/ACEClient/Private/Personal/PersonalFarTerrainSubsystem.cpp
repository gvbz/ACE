#include "Personal/PersonalFarTerrainSubsystem.h"
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
	int32 Radius, TSet<FIntPoint>&& Holes, float WorldScale)
{
	bBuilding = false;
	if (!ShellActor || !ShellMesh || !ShellMaterial)
	{
		return;
	}
	ShellActor->SetActorLocation(PersonalFarTerrainMesh::Origin(Mesh->Center, WorldScale));
	ShellMesh->CreateMeshSection_LinearColor(0, Mesh->Vertices, Mesh->Triangles, Mesh->Normals, Mesh->UVs,
		TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
	ShellMesh->SetMaterial(0, ShellMaterial);
	bWireframe = false;
	BuiltCenter = Mesh->Center;
	BuiltRadius = Radius;
	BuiltHoles = MoveTemp(Holes);
	// The loaded terrain may have changed while the worker ran.
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
	const int32 Radius = PersonalFarTerrainMesh::RadiusSetting();
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
	const bool bChanged = Center != BuiltCenter || Radius != BuiltRadius || Holes.Num() != BuiltHoles.Num() || !Holes.Includes(BuiltHoles);
	if (bChanged && !bBuilding)
	{
		// The mesh has about 100,000 triangles: build it on a worker, apply it here.
		bBuilding = true;
		const float WorldScale = Terrain->WorldScale;
		Async(EAsyncExecution::ThreadPool,
			[Weak = TWeakObjectPtr<UPersonalFarTerrainSubsystem>(this), Data = Bake, Center, Radius, HoleRadius, WorldScale, Holes = MoveTemp(Holes)]() mutable
		{
			auto Mesh = MakeShared<FPersonalFarTerrainMesh, ESPMode::ThreadSafe>();
			PersonalFarTerrainMesh::Build(*Data, Center, Radius, Holes, HoleRadius, WorldScale, *Mesh);
			AsyncTask(ENamedThreads::GameThread, [Weak, Mesh, Radius, WorldScale, Holes = MoveTemp(Holes)]() mutable
			{
				if (UPersonalFarTerrainSubsystem* Self = Weak.Get())
				{
					Self->ApplyMesh(Mesh, Radius, MoveTemp(Holes), WorldScale);
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
	}
	const bool bWantWireframe = CVarFarTerrainWireframe.GetValueOnGameThread() != 0 && GEngine && GEngine->WireframeMaterial;
	if (bWantWireframe != bWireframe)
	{
		bWireframe = bWantWireframe;
		ShellMesh->SetMaterial(0, bWireframe ? static_cast<UMaterialInterface*>(GEngine->WireframeMaterial) : ShellMaterial);
	}
	SetShown(true);
}
