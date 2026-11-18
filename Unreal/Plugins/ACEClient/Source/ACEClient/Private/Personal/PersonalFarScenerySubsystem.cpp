#include "Personal/PersonalFarScenerySubsystem.h"
#include "Personal/PersonalFarTerrain.h"
#include "Personal/PersonalFarTerrainSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACELandblockActor.h"
#include "ACETerrainPresenterComponent.h"
#include "ACETypes.h"
#include "Async/Async.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dat/ACESetupMeshBuilder.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RenderTimer.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"

namespace
{
	TAutoConsoleVariable<float> CVarFarSceneryBudgetMs(TEXT("personal.FarScenery.BudgetMs"), 1.f,
		TEXT("Game thread time per update (10 per second) the far scenery may use to add and remove landblocks, in ms."));

	FAutoConsoleCommandWithWorld FarSceneryLogFrameRate(TEXT("personal.FarScenery.LogFrameRate"),
		TEXT("Writes the frame rate and the game, render and GPU times of the last 10 seconds to the log."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (const UPersonalFarScenerySubsystem* Far = World ? World->GetSubsystem<UPersonalFarScenerySubsystem>() : nullptr)
			{
				Far->LogFrameRate(TEXT("frame rate"));
			}
		}));

	/** For measuring only: sets r.ScreenPercentage and remembers what it was; 0 puts it back. */
	FAutoConsoleCommand FarSceneryScreenPercentage(TEXT("personal.FarScenery.ScreenPercentage"),
		TEXT("Sets the render resolution in percent for a measurement; 0 puts the earlier value back."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			static float Saved = -1.f;
			IConsoleVariable* Percentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
			const float Wanted = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.f;
			if (!Percentage)
			{
				return;
			}
			if (Wanted > 0.f)
			{
				if (Saved < 0.f)
				{
					Saved = Percentage->GetFloat();
				}
				Percentage->Set(FMath::Clamp(Wanted, 10.f, 200.f), ECVF_SetByConsole);
			}
			else if (Saved >= 0.f)
			{
				Percentage->Set(Saved, ECVF_SetByConsole);
				Saved = -1.f;
			}
		}));

	/** Side of a landblock in AC units. */
	constexpr float BlockLength = 192.f;

	int32 DistanceSquared(FIntPoint A, FIntPoint B)
	{
		return (A.X - B.X) * (A.X - B.X) + (A.Y - B.Y) * (A.Y - B.Y);
	}

	TAutoConsoleVariable<int32> CVarFarSceneryGroup(TEXT("personal.FarScenery.Group"), 1,
		TEXT("Landblocks per side that share one component per object type, past the client's own rings. 1 is one component per type per landblock."));

	TAutoConsoleVariable<int32> CVarFarSceneryDetail(TEXT("personal.FarScenery.Detail"), 1,
		TEXT("1 draws far objects with the simpler models of the DAT further out and past the fog end, 0 with the full model everywhere."));
	TAutoConsoleVariable<int32> CVarFarSceneryMiddleRing(TEXT("personal.FarScenery.MiddleRing"), 4,
		TEXT("First ring of landblocks drawn with the middle detail level."));
	TAutoConsoleVariable<int32> CVarFarScenerySimplestRing(TEXT("personal.FarScenery.SimplestRing"), 5,
		TEXT("First ring of landblocks drawn with the simplest detail level."));
	TAutoConsoleVariable<FString> CVarFarSceneryTestSetup(TEXT("personal.FarScenery.TestSetup"), TEXT(""),
		TEXT("For measuring only: a Setup id in hex. Every far object is drawn with that Setup's mesh, at its own place. Empty for the normal meshes."));

	/** Squared distance in landblocks from Center to the nearest landblock of a unit (X, Y: low corner; Z: size). */
	int32 UnitDistanceSquared(const FIntVector& Unit, FIntPoint Center)
	{
		const int32 DX = FMath::Max3(Unit.X - Center.X, 0, Center.X - (Unit.X + Unit.Z - 1));
		const int32 DY = FMath::Max3(Unit.Y - Center.Y, 0, Center.Y - (Unit.Y + Unit.Z - 1));
		return DX * DX + DY * DY;
	}

	FVector LandblockOrigin(FIntPoint Block, float WorldScale)
	{
		return FACEPosition::AceVectorToUnreal(FVector(Block.X * 192., Block.Y * 192., 0.), WorldScale);
	}
}

bool UPersonalFarScenerySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UPersonalFarScenerySubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	InWorld.GetTimerManager().SetTimer(UpdateTimer, this, &UPersonalFarScenerySubsystem::Update, UpdateSeconds, true);
}

void UPersonalFarScenerySubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimer);
	}
	ClearAll();
	Super::Deinitialize();
}

void UPersonalFarScenerySubsystem::ClearAll()
{
	for (auto& Pair : Units)
	{
		if (AActor* Actor = Pair.Value.Actor.Get()) Actor->Destroy();
	}
	for (const FRetiring& Entry : Retiring)
	{
		if (Entry.Actor.IsValid()) Entry.Actor->Destroy();
	}
	for (const TArray<TWeakObjectPtr<AActor>>* Actors : {&Releasing, &Pool})
	{
		for (const TWeakObjectPtr<AActor>& Actor : *Actors)
		{
			if (Actor.IsValid()) Actor->Destroy();
		}
	}
	Blocks.Reset();
	Units.Reset();
	Retiring.Reset();
	Releasing.Reset();
	Pool.Reset();
	bPlanDirty = true;
	bComplete = false;
}

bool UPersonalFarScenerySubsystem::EnsureSource(const FString& DatDirectory)
{
	if (Source)
	{
		return true;
	}
	if (!bSourceRequested)
	{
		bSourceRequested = true;
		PendingSource = Async(EAsyncExecution::ThreadPool, [DatDirectory]() { return PersonalFarSceneryPlacement::Open(DatDirectory); });
	}
	else if (PendingSource.IsValid() && PendingSource.IsReady())
	{
		// A failed load leaves this null for good: no far scenery this session.
		Source = PendingSource.Get();
		PendingSource = TFuture<PersonalFarSceneryPlacement::FSourcePtr>();
	}
	return Source.IsValid();
}

void UPersonalFarScenerySubsystem::SetUnitHidden(FUnit& Unit, bool bHide)
{
	if (Unit.bHidden != bHide)
	{
		Unit.bHidden = bHide;
		if (AActor* Actor = Unit.Actor.Get())
		{
			Actor->SetActorHiddenInGame(bHide);
		}
	}
}

bool UPersonalFarScenerySubsystem::ApplyVisibility(FIntPoint Center)
{
	TArray<FIntVector> Changing;
	for (const auto& Pair : Units)
	{
		if (Pair.Value.bHidden != Pair.Value.bWantHidden)
		{
			Changing.Add(Pair.Key);
		}
	}
	if (Changing.Num() == 0)
	{
		return false;
	}
	Changing.Sort([Center](const FIntVector& A, const FIntVector& B) { return UnitDistanceSquared(A, Center) < UnitDistanceSquared(B, Center); });
	// About 25 microseconds per component, so the default budget of 1 ms allows 40.
	const int32 MaxComponents = FMath::Max(1, FMath::RoundToInt32(FMath::Clamp(CVarFarSceneryBudgetMs.GetValueOnGameThread(), 0.1f, 100.f) * 40.f));
	int32 Components = 0;
	for (const FIntVector& Key : Changing)
	{
		FUnit& Unit = Units.FindChecked(Key);
		// At least one unit per update, however many components it has.
		if (Components > 0 && Components + Unit.Components > MaxComponents)
		{
			return true;
		}
		Components += Unit.Components;
		SetUnitHidden(Unit, Unit.bWantHidden);
	}
	return false;
}

void UPersonalFarScenerySubsystem::HideAll()
{
	bAllHidden = true;
	for (auto& Pair : Units)
	{
		Pair.Value.bWantHidden = true;
	}
	ApplyVisibility(LastCenter);
	// What is on its way out stays hidden from here on.
	for (const FRetiring& Entry : Retiring)
	{
		if (Entry.Actor.IsValid() && !Entry.Actor->IsHidden()) Entry.Actor->SetActorHiddenInGame(true);
	}
	for (const TWeakObjectPtr<AActor>& Actor : Releasing)
	{
		if (Actor.IsValid() && !Actor->IsHidden()) Actor->SetActorHiddenInGame(true);
	}
}

void UPersonalFarScenerySubsystem::StartPlacement(const PersonalFarTerrainBake::FBakePtr& Bake, FIntPoint Center, float WorldScale, int32 OuterRing)
{
	if (bPlacing)
	{
		return;
	}
	TArray<FIntPoint> Missing;
	for (int32 DX = -OuterRing; DX <= OuterRing; ++DX)
	{
		for (int32 DY = -OuterRing; DY <= OuterRing; ++DY)
		{
			const FIntPoint Block = Center + FIntPoint(DX, DY);
			if (PersonalFarScenery::IsWanted(Block, Center, OuterRing) && !Blocks.Contains(Block))
			{
				Missing.Add(Block);
			}
		}
	}
	if (Missing.Num() == 0)
	{
		return;
	}
	Missing.Sort([Center](const FIntPoint& A, const FIntPoint& B) { return DistanceSquared(A, Center) < DistanceSquared(B, Center); });
	Missing.SetNum(FMath::Min(Missing.Num(), PlaceBatch));
	for (const FIntPoint Block : Missing)
	{
		Blocks.Add(Block);
	}
	bPlacing = true;
	Async(EAsyncExecution::ThreadPool,
		[Weak = TWeakObjectPtr<UPersonalFarScenerySubsystem>(this), Data = Bake, Scenes = Source, WorldScale, Missing = MoveTemp(Missing)]()
	{
		TArray<FPlaced> Placed;
		Placed.Reserve(Missing.Num());
		TArray<PersonalFarSceneryPlacement::FItem> Items;
		for (const FIntPoint Block : Missing)
		{
			FPlaced& Out = Placed.AddDefaulted_GetRef();
			Out.Block = Block;
			PersonalFarSceneryPlacement::Place(*Data, *Scenes, (uint32(Block.X) << 24) | (uint32(Block.Y) << 16), Items);
			for (const PersonalFarSceneryPlacement::FItem& Item : Items)
			{
				// The same steps, through the same float types, as the client's own scenery
				// (AACELandblockActor::TryQueueRegionDesc and TrySpawnOneScenery), so both land on the same spot.
				const FVector3f Origin(Item.OriginAc);
				const FQuat4f Orientation(Item.Orientation);
				const FQuat Rotation = FACEPosition::AceQuatToUnreal(Orientation.W, FVector(Orientation.X, Orientation.Y, Orientation.Z));
				const FVector Location = FACEPosition::AceVectorToUnreal(FVector(Origin.X, Origin.Y, Origin.Z), WorldScale);
				const float Scale = Item.Scale > KINDA_SMALL_NUMBER ? Item.Scale : 1.f;
				FTypeInstances& Type = Out.Instances.FindOrAdd(PersonalFarSceneryPlacement::TypeKey(Item.SetupId, Item.Kind));
				Type.Transforms.Emplace(Rotation, Location, FVector(Scale));
				Type.MaxScale = FMath::Max(Type.MaxScale, Scale);
			}
		}
		AsyncTask(ENamedThreads::GameThread, [Weak, Placed = MoveTemp(Placed)]() mutable
		{
			if (UPersonalFarScenerySubsystem* Self = Weak.Get())
			{
				Self->ApplyPlaced(MoveTemp(Placed));
			}
		});
	});
}

void UPersonalFarScenerySubsystem::ApplyPlaced(TArray<FPlaced>&& Placed)
{
	bPlacing = false;
	for (FPlaced& Result : Placed)
	{
		// A landblock that left the set while the worker ran is not in the map any more.
		FBlock* Block = Blocks.Find(Result.Block);
		if (Block && !Block->bPlaced)
		{
			Block->bPlaced = true;
			Block->Types = MoveTemp(Result.Instances);
			bPlanDirty = true;
		}
	}
}

AActor* UPersonalFarScenerySubsystem::AcquireActor(FIntPoint Key, float WorldScale)
{
	const FVector Origin = LandblockOrigin(Key, WorldScale);
	while (Pool.Num() > 0)
	{
		if (AActor* Actor = Pool.Pop(EAllowShrinking::No).Get())
		{
			Actor->SetActorLocation(Origin);
			return Actor;
		}
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (!Actor)
	{
		return nullptr;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Actor);
	Root->SetMobility(EComponentMobility::Movable);
	Actor->SetRootComponent(Root);
	Root->RegisterComponent();
	Actor->SetActorLocation(Origin);
	return Actor;
}

const UPersonalFarScenerySubsystem::FTypeInfo& UPersonalFarScenerySubsystem::GetTypeInfo(UACEDatSubsystem& Dat, uint32 SetupId, float WorldScale)
{
	if (const FTypeInfo* Known = Types.Find(SetupId))
	{
		return *Known;
	}
	FTypeInfo Info;
	// Height from the built Setup, before any static mesh exists: a type too small to see needs no mesh.
	const uint32 DatId = PersonalFarSceneryPlacement::TypeId(SetupId);
	if (const FACEBuiltSetupMesh* Built = Dat.FindSetupMesh(DatId, WorldScale, PersonalFarSceneryPlacement::TypePlacement(SetupId)))
	{
		double Low = MAX_dbl, High = -MAX_dbl;
		for (const FACEBuiltSetupPart& Part : Built->Parts)
		{
			for (const FACEBuiltMeshSection& Section : Part.Sections)
			{
				if (Section.bCollisionOnly || Section.bFullyTransparent)
				{
					continue;
				}
				for (const FVector& Vertex : Section.Vertices)
				{
					const double Z = Part.BindTransform.TransformPosition(Vertex).Z;
					Low = FMath::Min(Low, Z);
					High = FMath::Max(High, Z);
				}
			}
		}
		Info.Height = High > Low ? float(High - Low) : 0.f;
	}
	float StepUp = 0.f, Height = 0.f, Radius = 0.f;
	uint32 DefaultAnimation = 0;
	Dat.TryGetSetupPhysics(DatId, StepUp, Height, Radius, DefaultAnimation);
	// A building is never a live actor in the client (AACELandblockActor::TrySpawnOneScenery).
	Info.bAnimated = DefaultAnimation != 0 && PersonalFarSceneryPlacement::TypeKind(SetupId) != PersonalFarSceneryPlacement::EKind::Building;
	return Types.Add(SetupId, Info);
}

void UPersonalFarScenerySubsystem::AddComponent(UACEDatSubsystem& Dat, FUnit& Unit, uint32 SetupId, int32 Level, const TArray<FTransform>& Transforms, float WorldScale, float Distance)
{
	// The same mesh the client's own scenery uses (AACELandblockActor::GetOrCreateSceneryHism):
	// same cache key, so nothing is built twice. MakeMesh has created it already.
	constexpr bool bCollisionKey = true;
	uint32 DatId = PersonalFarSceneryPlacement::TypeId(SetupId);
	int32 PlacementId = PersonalFarSceneryPlacement::TypePlacement(SetupId);
	bool bBuilding = PersonalFarSceneryPlacement::TypeKind(SetupId) == PersonalFarSceneryPlacement::EKind::Building;
	// Measuring: one small mesh for every type shows what the triangles cost and what the instances cost.
	const uint32 TestSetup = FParse::HexNumber(*CVarFarSceneryTestSetup.GetValueOnGameThread());
	const FPersonalFarSceneryLevelMesh* LevelMesh = Level > 0 ? LevelMeshes.Find(uint64(SetupId) * 4u + uint64(Level)) : nullptr;
	if (!LevelMesh && TestSetup != 0 && Dat.RequestSetupMesh(TestSetup, WorldScale, 0) == UACEDatSubsystem::EACESetupMeshStatus::Ready)
	{
		DatId = TestSetup;
		PlacementId = 0;
		bBuilding = false;
	}
	UStaticMesh* Mesh = LevelMesh ? LevelMesh->Mesh.Get() : Dat.GetOrCreateSetupStaticMesh(DatId, WorldScale, bCollisionKey, PlacementId,
		/*bParticleGfx*/ false, /*bStencilHoleClip*/ bBuilding, /*bOutdoorLit*/ !bBuilding);
	AActor* Actor = Unit.Actor.Get();
	if (!Mesh || !Actor || Transforms.Num() == 0)
	{
		return;
	}
	// Size rule: each object is drawn out to the distance where its type is still worth seeing.
	// Types that reach past the last ring are never culled.
	const float Reach = (PersonalFarScenery::LastRing + 2) * BlockLength * WorldScale;
	if (bBuilding)
	{
		// A building is a plain component with the client's shell material, as in the client
		// (AACELandblockActor::TrySpawnOneScenery): that material is not made for instances.
		// Its openings stay open, as in the client; far windows (Far Terrain Rooms) fills them.
		for (const FTransform& Transform : Transforms)
		{
			UStaticMeshComponent* Shell = NewObject<UStaticMeshComponent>(Actor);
			Shell->SetupAttachment(Actor->GetRootComponent());
			Shell->SetStaticMesh(Mesh);
			Dat.BindSetupStaticMeshMaterials(Shell, DatId, WorldScale, bCollisionKey, PlacementId, /*bParticleGfx*/ false, /*bStencilHoleClip*/ true);
			Shell->SetMobility(EComponentMobility::Movable);
			Shell->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Shell->SetGenerateOverlapEvents(false);
			Shell->SetCanEverAffectNavigation(false);
			Shell->SetCastShadow(false);
			Shell->bCastDynamicShadow = false;
			Shell->bUseAsOccluder = false;
			Shell->SetReceivesDecals(false);
			Shell->SetRelativeTransform(Transform);
			Shell->bNeverDistanceCull = Distance >= Reach;
			if (Distance < Reach)
			{
				Shell->SetCullDistance(Distance);
			}
			Shell->RegisterComponent();
			++Unit.Components;
			++Unit.Instances;
			Unit.Triangles += Shell->GetStaticMesh()->GetNumTriangles(0);
		}
		return;
	}
	UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(Actor);
	Instances->SetupAttachment(Actor->GetRootComponent());
	Instances->SetStaticMesh(Mesh);
	Instances->SetMobility(EComponentMobility::Movable);
	if (LevelMesh)
	{
		for (int32 Slot = 0; Slot < LevelMesh->Materials.Num(); ++Slot)
		{
			Instances->SetMaterial(Slot, LevelMesh->Materials[Slot]);
		}
	}
	else
	{
		Dat.BindSetupStaticMeshMaterials(Instances, DatId, WorldScale, bCollisionKey, PlacementId,
			/*bParticleGfx*/ false, /*bStencilHoleClip*/ false, /*bOutdoorLit*/ true);
	}
	// The mesh carries the collision of the near scenery. Off here, so no bodies are made.
	Instances->bDisableCollision = true;
	Instances->bUseDefaultCollision = false;
	Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Instances->SetGenerateOverlapEvents(false);
	Instances->SetCanEverAffectNavigation(false);
	Instances->SetCastShadow(false);
	Instances->bCastDynamicShadow = false;
	Instances->bUseAsOccluder = false;
	Instances->SetReceivesDecals(false);
	Instances->bNeverDistanceCull = Distance >= Reach;
	if (Distance < Reach)
	{
		Instances->SetCullDistances(FMath::RoundToInt32(Distance * 0.9f), FMath::RoundToInt32(Distance));
	}
	Instances->AddInstances(Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
	Instances->RegisterComponent();
	++Unit.Components;
	Unit.Instances += Transforms.Num();
	Unit.Triangles += int64(Mesh->GetNumTriangles(0)) * Transforms.Num();
}

void UPersonalFarScenerySubsystem::PlanUnits(UACEDatSubsystem& Dat, FIntPoint Center, float WorldScale)
{
	const int32 Group = FMath::Clamp(CVarFarSceneryGroup.GetValueOnGameThread(), 1, 8);
	const float Factor = PersonalFarScenery::SizeFactor();
	const float BlockSize = BlockLength * WorldScale;
	const bool bDetail = CVarFarSceneryDetail.GetValueOnGameThread() != 0;
	const int32 MiddleRing = CVarFarSceneryMiddleRing.GetValueOnGameThread();
	const int32 SimplestRing = FMath::Max(CVarFarScenerySimplestRing.GetValueOnGameThread(), MiddleRing);
	++PlanSerial;
	bool bPending = false;
	// One mesh request per object type and plan, not one per landblock.
	TMap<uint32, UACEDatSubsystem::EACESetupMeshStatus> Status;
	for (const auto& Pair : Blocks)
	{
		if (!Pair.Value.bPlaced)
		{
			continue;
		}
		const FIntPoint Block = Pair.Key;
		const int32 Ring = PersonalFarScenery::Ring(Block, Center);
		// Landblocks next to the client's own scenery stay on their own, so each can be hidden and shown for the handoff.
		const int32 FarGroup = Ring > PersonalFarScenery::InteriorRings ? FMath::Max(4, Group) : Group;
		const int32 Size = Ring <= PersonalFarScenery::NearRing ? 1 : FarGroup;
		const FIntVector Key(FMath::FloorToInt32(float(Block.X) / Size) * Size, FMath::FloorToInt32(float(Block.Y) / Size) * Size, Size);
		FUnit& Unit = Units.FindOrAdd(Key);
		if (Unit.PlanSerial != PlanSerial)
		{
			Unit.PlanSerial = PlanSerial;
			Unit.Members.Reset();
			Unit.Included.Reset();
			Unit.bPending = false;
			Unit.TooSmall = 0;
			Unit.Level = PersonalFarSceneryMeshes::LevelCount - 1;
		}
		Unit.Members.Add(Block);
		// Past the fog end everything is the fog color: a silhouette, so the simplest model.
		Unit.Level = FMath::Min(Unit.Level, !bDetail ? 0 : ((Ring >= SimplestRing || Ring > FogRingNow) ? 2 : (Ring >= MiddleRing ? 1 : 0)));
		for (const auto& Type : Pair.Value.Types)
		{
			UACEDatSubsystem::EACESetupMeshStatus* Known = Status.Find(Type.Key);
			if (!Known)
			{
				Known = &Status.Add(Type.Key, Dat.RequestSetupMesh(PersonalFarSceneryPlacement::TypeId(Type.Key), WorldScale,
					PersonalFarSceneryPlacement::TypePlacement(Type.Key)));
			}
			if (*Known == UACEDatSubsystem::EACESetupMeshStatus::Pending || *Known == UACEDatSubsystem::EACESetupMeshStatus::NotReady)
			{
				Unit.bPending = true;
				bPending = true;
				continue;
			}
			if (*Known != UACEDatSubsystem::EACESetupMeshStatus::Ready)
			{
				// A type whose mesh failed to build is left out.
				continue;
			}
			const FTypeInfo& Info = GetTypeInfo(Dat, Type.Key, WorldScale);
			if (Info.bAnimated)
			{
				continue;
			}
			if (PersonalFarScenery::InReach(PersonalFarScenery::DrawDistance(Info.Height, Type.Value.MaxScale, Factor), Ring, BlockSize))
			{
				Unit.Included.Add({Block, Type.Key});
			}
			else
			{
				// Too small to see from here. It is drawn when the landblock comes closer.
				Unit.TooSmall += Type.Value.Transforms.Num();
			}
		}
	}
	for (auto It = Units.CreateIterator(); It; ++It)
	{
		FUnit& Unit = It.Value();
		if (Unit.PlanSerial != PlanSerial)
		{
			// No landblock belongs to this unit any more. Its actor goes once the units that took its landblocks are built.
			if (AActor* Actor = Unit.Actor.Get())
			{
				if (Unit.bHidden && !bAllHidden)
				{
					Actor->SetActorHiddenInGame(false);
				}
				Retiring.Add({Actor, Unit.BuiltMembers});
			}
			It.RemoveCurrent();
			continue;
		}
		if (Unit.Level > 0)
		{
			// The simpler models come from a worker; the unit waits until every type has them.
			for (FIncluded& Entry : Unit.Included)
			{
				if (PersonalFarSceneryPlacement::TypeKind(Entry.SetupId) == PersonalFarSceneryPlacement::EKind::Building)
				{
					// Buildings keep the client's mesh and shell material at every distance.
					continue;
				}
				if (const PersonalFarSceneryMeshes::FLevelsPtr* Known = Levels.Find(Entry.SetupId))
				{
					Entry.Level = uint8((*Known)->Effective(Unit.Level));
				}
				else
				{
					LevelsWanted.Add(Entry.SetupId);
					Unit.bPending = true;
					bPending = true;
				}
			}
		}
		Unit.Included.Sort([](const FIncluded& A, const FIncluded& B)
		{
			return A.SetupId != B.SetupId ? A.SetupId < B.SetupId : (A.Block.X != B.Block.X ? A.Block.X < B.Block.X : A.Block.Y < B.Block.Y);
		});
		uint32 Hash = GetTypeHash(Factor);
		for (const FIncluded& Entry : Unit.Included)
		{
			Hash = HashCombine(Hash, HashCombine(GetTypeHash(Entry.Block), GetTypeHash(Entry.SetupId * 4u + Entry.Level)));
		}
		Unit.WantSignature = Hash;
	}
	// While a mesh is still on a worker the plan is made again at the next update.
	bPlanDirty = bPending;
	RequestLevels(Dat.GetDatDirectory(), WorldScale);
}

void UPersonalFarScenerySubsystem::RequestLevels(const FString& DatDirectory, float WorldScale)
{
	if (bLevelsBusy || LevelsWanted.Num() == 0)
	{
		return;
	}
	bLevelsBusy = true;
	TArray<uint32> Wanted = LevelsWanted.Array();
	LevelsWanted.Reset();
	Async(EAsyncExecution::ThreadPool,
		[Weak = TWeakObjectPtr<UPersonalFarScenerySubsystem>(this), Wanted = MoveTemp(Wanted), Meshes = MeshSource, DatDirectory, WorldScale]() mutable
	{
		if (!Meshes)
		{
			Meshes = PersonalFarSceneryMeshes::Open(DatDirectory);
		}
		TMap<uint32, PersonalFarSceneryMeshes::FLevelsPtr> Built;
		if (Meshes)
		{
			PersonalFarSceneryMeshes::Build(*Meshes, Wanted, WorldScale, Built);
		}
		else
		{
			// No DAT handle: these types keep the full model.
			for (const uint32 SetupId : Wanted)
			{
				Built.Add(SetupId, MakeShared<PersonalFarSceneryMeshes::FLevels, ESPMode::ThreadSafe>());
			}
		}
		AsyncTask(ENamedThreads::GameThread, [Weak, Built = MoveTemp(Built), Meshes]() mutable
		{
			if (UPersonalFarScenerySubsystem* Self = Weak.Get())
			{
				Self->ApplyLevels(MoveTemp(Built), Meshes);
			}
		});
	});
}

void UPersonalFarScenerySubsystem::ApplyLevels(TMap<uint32, PersonalFarSceneryMeshes::FLevelsPtr>&& Built, PersonalFarSceneryMeshes::FSourcePtr UsedSource)
{
	bLevelsBusy = false;
	MeshSource = UsedSource;
	Levels.Append(MoveTemp(Built));
	bPlanDirty = true;
}

bool UPersonalFarScenerySubsystem::MakeMesh(UACEDatSubsystem& Dat, uint32 SetupId, int32 Level, float WorldScale)
{
	const uint64 Key = uint64(SetupId) * 4u + uint64(Level);
	if (const bool* Made = MeshMade.Find(Key))
	{
		return *Made;
	}
	const double Start = FPlatformTime::Seconds();
	UStaticMesh* Mesh = nullptr;
	if (Level > 0)
	{
		// A simpler model: own mesh, no collision, the materials of the client.
		const PersonalFarSceneryMeshes::FLevelsPtr* Known = Levels.Find(SetupId);
		if (Known && (*Known)->Sections[Level - 1].Num() > 0)
		{
			TArray<UMaterialInterface*> Materials;
			Mesh = PersonalFarSceneryMeshes::MakeStaticMesh(Dat, this, (*Known)->Sections[Level - 1], Materials);
			if (Mesh)
			{
				FPersonalFarSceneryLevelMesh& Entry = LevelMeshes.Add(Key);
				Entry.Mesh = Mesh;
				Entry.Materials.Append(Materials);
			}
		}
	}
	else
	{
		// The client's own mesh, by the client's cache key for that kind (AACELandblockActor::GetOrCreateSceneryHism, TrySpawnOneScenery).
		const bool bBuilding = PersonalFarSceneryPlacement::TypeKind(SetupId) == PersonalFarSceneryPlacement::EKind::Building;
		Mesh = Dat.GetOrCreateSetupStaticMesh(PersonalFarSceneryPlacement::TypeId(SetupId), WorldScale, /*bEnableCollision*/ true,
			PersonalFarSceneryPlacement::TypePlacement(SetupId), /*bParticleGfx*/ false, /*bStencilHoleClip*/ bBuilding, /*bOutdoorLit*/ !bBuilding);
	}
	const double Seconds = FPlatformTime::Seconds() - Start;
	if (Seconds > 0.005)
	{
		UE_LOG(LogTemp, Log, TEXT("PersonalFarScenery: slow mesh build, 0x%08X kind %d level %d: %.1f ms, %d triangles, collision %s"),
			PersonalFarSceneryPlacement::TypeId(SetupId), int32(PersonalFarSceneryPlacement::TypeKind(SetupId)), Level, Seconds * 1000., Mesh ? Mesh->GetNumTriangles(0) : 0, Level == 0 && Mesh && Mesh->GetBodySetup() ? TEXT("yes") : TEXT("no"));
	}
	return MeshMade.Add(Key, Mesh != nullptr);
}

bool UPersonalFarScenerySubsystem::BuildUnits(UACEDatSubsystem& Dat, FIntPoint Center, float WorldScale, double Start, double Budget)
{
	TArray<FIntVector> Order;
	bool bLeft = false;
	for (const auto& Pair : Units)
	{
		const FUnit& Unit = Pair.Value;
		if (Unit.bPending)
		{
			bLeft = true;
		}
		else if (!Unit.bBuilt || Unit.BuiltSignature != Unit.WantSignature)
		{
			Order.Add(Pair.Key);
		}
	}
	Order.Sort([Center](const FIntVector& A, const FIntVector& B) { return UnitDistanceSquared(A, Center) < UnitDistanceSquared(B, Center); });
	const float Factor = PersonalFarScenery::SizeFactor();
	int32 Built = 0;
	for (const FIntVector& Key : Order)
	{
		FUnit& Unit = Units.FindChecked(Key);
		// The static meshes first, one at a time: a new one can take tens of ms.
		for (const FIncluded& Entry : Unit.Included)
		{
			if (!MeshMade.Contains(uint64(Entry.SetupId) * 4u + Entry.Level))
			{
				if (FPlatformTime::Seconds() - Start >= Budget)
				{
					return true;
				}
				MakeMesh(Dat, Entry.SetupId, Entry.Level, WorldScale);
			}
		}
		if (Built > 0 && FPlatformTime::Seconds() - Start >= Budget)
		{
			return true;
		}
		// Old components out and new ones in within one update, so nothing is missing in between.
		AActor* Actor = Unit.Actor.Get();
		if (Actor)
		{
			TInlineComponentArray<UStaticMeshComponent*> Old(Actor);
			for (UStaticMeshComponent* Component : Old)
			{
				Component->DestroyComponent();
			}
		}
		Unit.Components = 0;
		Unit.Instances = 0;
		Unit.Triangles = 0;
		if (Unit.Included.Num() > 0 && !Actor)
		{
			Actor = AcquireActor(FIntPoint(Key.X, Key.Y), WorldScale);
			Unit.Actor = Actor;
			if (Actor)
			{
				Actor->SetActorHiddenInGame(Unit.bHidden);
			}
		}
		const FVector UnitOrigin = LandblockOrigin(FIntPoint(Key.X, Key.Y), WorldScale);
		// Included is sorted by object type: one component per run of the same type.
		TArray<FTransform> Transforms;
		for (int32 First = 0; First < Unit.Included.Num() && Actor;)
		{
			const uint32 SetupId = Unit.Included[First].SetupId;
			int32 Level = Unit.Included[First].Level;
			Transforms.Reset();
			float MaxScale = 0.f;
			int32 Next = First;
			for (; Next < Unit.Included.Num() && Unit.Included[Next].SetupId == SetupId; ++Next)
			{
				const FIntPoint Block = Unit.Included[Next].Block;
				const FBlock* From = Blocks.Find(Block);
				const FTypeInstances* Type = From ? From->Types.Find(SetupId) : nullptr;
				if (!Type)
				{
					continue;
				}
				MaxScale = FMath::Max(MaxScale, Type->MaxScale);
				const FVector Offset = LandblockOrigin(Block, WorldScale) - UnitOrigin;
				const int32 At = Transforms.Num();
				Transforms.Append(Type->Transforms);
				if (!Offset.IsZero())
				{
					for (int32 I = At; I < Transforms.Num(); ++I)
					{
						Transforms[I].AddToTranslation(Offset);
					}
				}
			}
			First = Next;
			if (Level > 0 && !MakeMesh(Dat, SetupId, Level, WorldScale))
			{
				// The simpler model could not be made: the full one.
				Level = 0;
			}
			if (MakeMesh(Dat, SetupId, Level, WorldScale))
			{
				AddComponent(Dat, Unit, SetupId, Level, Transforms, WorldScale,
					PersonalFarScenery::DrawDistance(GetTypeInfo(Dat, SetupId, WorldScale).Height, MaxScale, Factor));
			}
		}
		Unit.bBuilt = true;
		Unit.BuiltSignature = Unit.WantSignature;
		Unit.BuiltMembers = Unit.Members;
		++Built;
	}
	return bLeft;
}

void UPersonalFarScenerySubsystem::UpdateRetiring()
{
	for (int32 Index = Retiring.Num() - 1; Index >= 0; --Index)
	{
		bool bReady = true;
		for (const FIntPoint Block : Retiring[Index].Blocks)
		{
			if (!Blocks.Contains(Block))
			{
				continue;
			}
			// The landblock is still wanted: wait until the unit that has it now is built.
			bool bTaken = false;
			for (const auto& Pair : Units)
			{
				const FUnit& Unit = Pair.Value;
				if (Unit.bBuilt && !Unit.bPending && Unit.BuiltSignature == Unit.WantSignature && Unit.BuiltMembers.Contains(Block))
				{
					bTaken = true;
					break;
				}
			}
			if (!bTaken)
			{
				bReady = false;
				break;
			}
		}
		if (bReady)
		{
			if (Retiring[Index].Actor.IsValid())
			{
				Releasing.Add(Retiring[Index].Actor);
			}
			Retiring.RemoveAtSwap(Index);
		}
	}
}

AActor* UPersonalFarScenerySubsystem::ActorOf(FIntPoint Block) const
{
	for (const auto& Pair : Units)
	{
		if (Pair.Value.BuiltMembers.Contains(Block))
		{
			return Pair.Value.Actor.Get();
		}
	}
	return nullptr;
}

void UPersonalFarScenerySubsystem::ReleaseActors(double Start, double Budget)
{
	while (Releasing.Num() > 0)
	{
		AActor* Actor = Releasing.Last().Get();
		if (Actor)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
			for (UStaticMeshComponent* Component : Components)
			{
				if (FPlatformTime::Seconds() - Start >= Budget)
				{
					return;
				}
				Component->DestroyComponent();
			}
			if (Pool.Num() < MaxPooledActors)
			{
				Actor->SetActorHiddenInGame(true);
				Pool.Add(Actor);
			}
			else
			{
				Actor->Destroy();
			}
		}
		Releasing.Pop(EAllowShrinking::No);
	}
}

UPersonalFarScenerySubsystem::FStats UPersonalFarScenerySubsystem::GetStats() const
{
	FStats Stats;
	Stats.InSet = Blocks.Num();
	Stats.Units = Units.Num();
	for (const auto& Pair : Units)
	{
		const FUnit& Unit = Pair.Value;
		Stats.Landblocks += Unit.Components > 0 ? Unit.BuiltMembers.Num() : 0;
		Stats.Hidden += Unit.bHidden ? Unit.Members.Num() : 0;
		Stats.Components += Unit.Components;
		Stats.Instances += Unit.Instances;
		Stats.Triangles += Unit.Triangles;
		Stats.TooSmall += Unit.TooSmall;
	}
	return Stats;
}

void UPersonalFarScenerySubsystem::LogStats(double FillSeconds) const
{
	const FStats Stats = GetStats();
	UE_LOG(LogTemp, Log,
		TEXT("PersonalFarScenery: %d landblocks in the set, %d with objects, %d hidden, %d components, %d instances, %lld triangles, %d instances too small to draw, %d units. Filled in %.1f s; game thread %.1f ms in all, longest update %.2f ms"),
		Stats.InSet, Stats.Landblocks, Stats.Hidden, Stats.Components, Stats.Instances, Stats.Triangles, Stats.TooSmall, Stats.Units,
		FillSeconds, WorkSeconds * 1000., LongestUpdate * 1000.);
}

void UPersonalFarScenerySubsystem::LogFrameRate(const TCHAR* What) const
{
	const FFrameSample& Oldest = FrameSamples[FrameSampleIndex];
	const FFrameSample& Newest = FrameSamples[(FrameSampleIndex + FrameSampleCount - 1) % FrameSampleCount];
	const double Seconds = Newest.Seconds - Oldest.Seconds;
	const uint64 Frames = Newest.Frame - Oldest.Frame;
	if (Oldest.Seconds <= 0. || Frames == 0 || Seconds <= 0.)
	{
		UE_LOG(LogTemp, Log, TEXT("PersonalFarScenery: %s: not enough samples yet"), What);
		return;
	}
	double Game = 0., Render = 0., Gpu = 0.;
	for (const FFrameSample& Sample : FrameSamples)
	{
		Game += Sample.GameMs;
		Render += Sample.RenderMs;
		Gpu += Sample.GpuMs;
	}
	const FStats Stats = GetStats();
	const IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
	UE_LOG(LogTemp, Log,
		TEXT("PersonalFarScenery: %s, last %.1f s: %.1f FPS, %.2f ms; game %.2f ms, render %.2f ms, GPU %.2f ms; %d components, %d instances, %lld triangles, %d landblocks; screen %.0f%%"),
		What, Seconds, Frames / Seconds, Seconds * 1000. / Frames, Game / FrameSampleCount, Render / FrameSampleCount, Gpu / FrameSampleCount,
		Stats.Components, Stats.Instances, Stats.Triangles, Stats.InSet, ScreenPercentage ? ScreenPercentage->GetFloat() : 0.f);
}

void UPersonalFarScenerySubsystem::RunCommandFile()
{
	static const FString CommandFile = FPaths::ProjectSavedDir() / TEXT("Personal/FarSceneryCommands.txt");
	const FDateTime Time = IFileManager::Get().GetTimeStamp(*CommandFile);
	// A file left from an earlier session is not run: only one that changes after the first look.
	const bool bFirstLook = !bCommandFileSeen;
	bCommandFileSeen = true;
	if (Time == FDateTime::MinValue() || Time == CommandFileTime)
	{
		return;
	}
	CommandFileTime = Time;
	if (bFirstLook)
	{
		return;
	}
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *CommandFile))
	{
		return;
	}
	for (FString& Line : Lines)
	{
		Line.TrimStartAndEndInline();
		// Own console variables and commands only.
		if (Line.StartsWith(TEXT("personal."), ESearchCase::IgnoreCase) && GEngine)
		{
			UE_LOG(LogTemp, Log, TEXT("PersonalFarScenery: command '%s'"), *Line);
			GEngine->Exec(GetWorld(), *Line);
		}
	}
}

void UPersonalFarScenerySubsystem::Update()
{

	// Frame rate over the last 10 seconds, from the frame counter: nothing runs per frame.
	const double Now = FPlatformTime::Seconds();
	const FFrameSample Oldest = FrameSamples[FrameSampleIndex];
	FrameSamples[FrameSampleIndex] = {Now, GFrameCounter, FPlatformTime::ToMilliseconds(GGameThreadTime),
		FPlatformTime::ToMilliseconds(GRenderThreadTime), FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles())};
	FrameSampleIndex = (FrameSampleIndex + 1) % FrameSampleCount;
	RunCommandFile();

	// Off: the Far Terrain Objects check box (or its console variable), the far terrain off,
	// or a file the measuring script makes and deletes.
	static const FString OffFile = FPaths::ProjectSavedDir() / TEXT("Personal/FarSceneryOff");
	const bool bOff = !PersonalFarScenery::IsOn() || IFileManager::Get().FileExists(*OffFile);
	if (bOff != bSwitchedOff)
	{
		bSwitchedOff = bOff;
		const double Seconds = Now - Oldest.Seconds;
		const uint64 Frames = GFrameCounter - Oldest.Frame;
		if (Oldest.Seconds > 0. && Frames > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("PersonalFarScenery: switched %s. The %.1f s before: %.1f FPS, %.2f ms, far scenery %s"),
				bOff ? TEXT("off") : TEXT("on"), Seconds, Frames / Seconds, Seconds * 1000. / Frames, bOff ? TEXT("on") : TEXT("off"));
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("PersonalFarScenery: switched %s"), bOff ? TEXT("off") : TEXT("on"));
		}
		if (bOff)
		{
			ClearAll();
			bBusy = false;
		}
	}
	if (bOff)
	{
		return;
	}

	UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = Game ? Game->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
	const UPersonalFarTerrainSubsystem* FarTerrain = World ? World->GetSubsystem<UPersonalFarTerrainSubsystem>() : nullptr;
	if (!Dat || !Client || !FarTerrain || !PersonalFarTerrain::IsOn())
	{
		HideAll();
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
	// Like the far terrain: not at the login screen, in portal space or in a dungeon. Inside a building it stays,
	// for the view out of doors and windows (the client's own rule for keeping the land: NeedsExteriorTerrain).
	const uint32 Cell = uint32(Client->GetPlayerPosition().CellId);
	const uint32 CellIndex = Cell & 0xFFFFu;
	const UACETerrainPresenterComponent* Terrain = Presenter.Get();
	if (!Terrain || Terrain->LoadRadius <= 0 || CellIndex == 0 || !Terrain->NeedsExteriorTerrain(Cell)
		|| Dat->IsInPortalSpace() || !Terrain->IsLandblockSpawned(Cell))
	{
		HideAll();
		return;
	}
	const PersonalFarTerrainBake::FBakePtr Bake = FarTerrain->GetBake();
	if (!Bake)
	{
		return;
	}
	// Handoff: the landblocks next to the far ones where the client's own scenery is complete and shown.
	const FIntPoint Center(int32(Cell >> 24), int32((Cell >> 16) & 0xFFu));
	TSet<FIntPoint> NearShown;
	for (TActorIterator<AACELandblockActor> It(World); It; ++It)
	{
		const uint32 Id = uint32(It->LandblockId);
		const FIntPoint Block(int32(Id >> 24), int32((Id >> 16) & 0xFFu));
		if (PersonalFarScenery::Ring(Block, Center) <= PersonalFarScenery::NearRing && It->IsNearSceneryShown())
		{
			NearShown.Add(Block);
		}
	}
	Stream(*Dat, Bake, Center, Terrain->WorldScale, Dat->GetWorldFogEndCm(), [&NearShown](FIntPoint Block) { return NearShown.Contains(Block); });
}

void UPersonalFarScenerySubsystem::Stream(UACEDatSubsystem& Dat, const PersonalFarTerrainBake::FBakePtr& Bake, FIntPoint Center, float WorldScale,
	float FogEnd, TFunctionRef<bool(FIntPoint)> NearShown)
{
	if (!EnsureSource(Dat.GetDatDirectory()))
	{
		return;
	}
	// Fog rule: nothing is created past the fog end. What exists is kept one ring further,
	// so objects do not come and go while the fog end moves.
	const int32 FogRing = PersonalFarScenery::FogRing(FogEnd, BlockLength * WorldScale);
	const int32 VisibleRadius = FMath::Min(PersonalFarScenery::Radius(), PersonalFarTerrain::Radius());
	const int32 BuildRing = PersonalFarScenery::HideWithFog() ? FMath::Min(FogRing, VisibleRadius) : VisibleRadius;
	const int32 KeepRing = FMath::Min(BuildRing + 1, VisibleRadius);
	const bool bScaleChanged = WorldScale != BuiltWorldScale;
	BuiltWorldScale = WorldScale;
	if (bScaleChanged)
	{
		ClearAll();
		Types.Reset();
		MeshMade.Reset();
		Levels.Reset();
		LevelMeshes.Reset();
	}
	for (auto It = Blocks.CreateIterator(); It; ++It)
	{
		if (!PersonalFarScenery::IsWanted(It.Key(), Center, KeepRing))
		{
			It.RemoveCurrent();
			bPlanDirty = true;
		}
	}
	StartPlacement(Bake, Center, WorldScale, BuildRing);

	const double Start = FPlatformTime::Seconds();
	FogRingNow = FogRing;
	uint32 Settings = HashCombine(GetTypeHash(CVarFarSceneryGroup.GetValueOnGameThread()), GetTypeHash(PersonalFarScenery::SizeFactor()));
	Settings = HashCombine(Settings, GetTypeHash(CVarFarSceneryDetail.GetValueOnGameThread()));
	Settings = HashCombine(Settings, GetTypeHash(CVarFarSceneryMiddleRing.GetValueOnGameThread()));
	Settings = HashCombine(Settings, GetTypeHash(CVarFarScenerySimplestRing.GetValueOnGameThread()));
	Settings = HashCombine(Settings, GetTypeHash(CVarFarSceneryTestSetup.GetValueOnGameThread()));
	Settings = HashCombine(Settings, GetTypeHash(FogRing));
	if (Center != LastCenter || Settings != PlannedSettings)
	{
		LastCenter = Center;
		PlannedSettings = Settings;
		bPlanDirty = true;
	}
	if (bPlanDirty)
	{
		PlanUnits(Dat, Center, WorldScale);
	}

	// Handoff: a landblock next to the client's own scenery is hidden while that scenery is complete and shown.
	bAllHidden = false;
	for (auto& Pair : Units)
	{
		FUnit& Unit = Pair.Value;
		const FIntPoint Block(Pair.Key.X, Pair.Key.Y);
		const int32 Ring = PersonalFarScenery::Ring(Block, Center);
		const bool bNear = Pair.Key.Z == 1 && Ring <= PersonalFarScenery::NearRing;
		Unit.bWantHidden = bNear && !PersonalFarScenery::ShowFar(Ring, NearShown(Block));
		if (bNear)
		{
			// In the same update, so there is no gap.
			SetUnitHidden(Unit, Unit.bWantHidden);
		}
	}
	const bool bVisibilityLeft = ApplyVisibility(Center);

	const double Budget = FMath::Clamp(CVarFarSceneryBudgetMs.GetValueOnGameThread(), 0.1f, 100.f) * 0.001;
	bool bWork = BuildUnits(Dat, Center, WorldScale, Start, Budget);
	UpdateRetiring();
	ReleaseActors(Start, Budget);
	const double Used = FPlatformTime::Seconds() - Start;

	bWork = bWork || bPlanDirty || bLevelsBusy || bVisibilityLeft || bPlacing || Retiring.Num() > 0 || Releasing.Num() > 0;
	for (auto It = Blocks.CreateConstIterator(); It && !bWork; ++It)
	{
		bWork = !It.Value().bPlaced;
	}
	if (bWork && !bBusy)
	{
		BusySince = Start;
		WorkSeconds = 0.;
		LongestUpdate = 0.;
	}
	if (bWork || bBusy)
	{
		WorkSeconds += Used;
		LongestUpdate = FMath::Max(LongestUpdate, Used);
	}
	if (!bWork && bBusy)
	{
		LogStats(FPlatformTime::Seconds() - BusySince);
	}
	bBusy = bWork;
	bComplete = !bWork;
}
