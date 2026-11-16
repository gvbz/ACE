#include "Personal/PersonalFarWindowsSubsystem.h"
#include "Personal/PersonalFarWindows.h"
#include "Personal/PersonalFarWindowsOverrides.h"
#include "Personal/PersonalFarRooms.h"
#include "Personal/PersonalFarRoomsMeshes.h"
#include "Personal/PersonalFarRoomComponent.h"
#include "Personal/PersonalFarSceneryMeshes.h"
#include "Async/Async.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEEnvCellActor.h"
#include "ACETerrainPresenterComponent.h"
#include "ACETypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACESetupMeshBuilder.h"
#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"

namespace
{
	TAutoConsoleVariable<float> CVarFarWindowsBudgetMs(TEXT("personal.FarWindows.BudgetMs"), 1.f,
		TEXT("Game thread time per update (10 per second) the far windows may use to make panels, in ms."));

	uint32 LandblockKey(FIntPoint Block)
	{
		return (uint32(Block.X) << 24) | (uint32(Block.Y) << 16);
	}

	FAutoConsoleCommandWithWorldAndArgs FarWindowsMark(TEXT("personal.FarWindows.Mark"),
		TEXT("door, open or clear: an override for the opening nearest the center of the screen (its building type and order in the model), written to Saved/Personal/FarWindowsOverrides.txt."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPersonalFarWindowsSubsystem* Windows = World ? World->GetSubsystem<UPersonalFarWindowsSubsystem>() : nullptr)
			{
				Windows->Mark(Args.Num() > 0 ? Args[0] : FString());
			}
		}));

	/** How far far scenery draws a building of this model (PersonalFarScenery::DrawDistance, from its drawn height), in Unreal units. */
	float BuildingDrawDistance(const FACEBuiltSetupMesh& Setup)
	{
		double Low = MAX_dbl, High = -MAX_dbl;
		for (const FACEBuiltSetupPart& Part : Setup.Parts)
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
		return PersonalFarScenery::DrawDistance(High > Low ? float(High - Low) : 0.f, 1.f, PersonalFarScenery::SizeFactor());
	}

	/** The same cull distance as the far building (UPersonalFarScenerySubsystem::AddComponent), so nothing stands where the building is gone. */
	void SetBuildingCull(UPrimitiveComponent& Component, float Distance, float WorldScale)
	{
		const float Reach = (PersonalFarScenery::LastRing + 2) * 192.f * WorldScale;
		Component.bNeverDistanceCull = Distance >= Reach;
		if (Distance < Reach)
		{
			Component.SetCullDistance(Distance);
		}
	}
}

void FPersonalFarWindowsTick::ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	if (Target && TickType != LEVELTICK_ViewportsOnly)
	{
		Target->ApplyShown();
		Target->ApplyRoomsShown();
	}
}

bool UPersonalFarWindowsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && !IsRunningDedicatedServer() && Super::ShouldCreateSubsystem(Outer);
}

void UPersonalFarWindowsSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	InWorld.GetTimerManager().SetTimer(UpdateTimer, this, &UPersonalFarWindowsSubsystem::Update, UpdateSeconds, true);
	// The terrain presenter shows and hides the rooms in this tick group; the tick is put behind it once it is known.
	Tick.Target = this;
	Tick.bCanEverTick = true;
	Tick.bStartWithTickEnabled = true;
	Tick.TickGroup = TG_PostUpdateWork;
	Tick.RegisterTickFunction(InWorld.PersistentLevel);
}

void UPersonalFarWindowsSubsystem::Deinitialize()
{
	Tick.UnRegisterTickFunction();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimer);
	}
	ClearAll();
	ClearRooms();
	Super::Deinitialize();
}

void UPersonalFarWindowsSubsystem::ClearAll()
{
	for (auto& Pair : Blocks)
	{
		if (AActor* Actor = Pair.Value.Actor.Get()) Actor->Destroy();
	}
	Blocks.Reset();
	bPanelsLogged = false;
}

UPersonalFarWindowsSubsystem::FStats UPersonalFarWindowsSubsystem::GetStats() const
{
	FStats Stats;
	for (const auto& Pair : Blocks)
	{
		Stats.Landblocks += Pair.Value.Panels.Num() > 0 ? 1 : 0;
		Stats.Panels += Pair.Value.Panels.Num();
		for (const FPanel& Panel : Pair.Value.Panels)
		{
			Stats.Shown += Panel.bShown ? 1 : 0;
			Stats.Doors += Panel.bDoor ? 1 : 0;
		}
	}
	for (const auto& Pair : Meshes) Stats.Meshes += Pair.Value ? 1 : 0;
	return Stats;
}

void UPersonalFarWindowsSubsystem::Update()
{
	const bool bPanels = PersonalFarWindows::IsOn();
	const bool bRooms = PersonalFarRooms::IsOn();
	ApplyLook();
	// Test mode on or off: the panels of the other openings are made only while it is on.
	const bool bKinds = bPanels && PersonalFarWindows::ShowKinds();
	if (bKinds != bKindsWereOn)
	{
		bKindsWereOn = bKinds;
		ClearAll();
		UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: test colors %s"), bKinds ? TEXT("on: a panel in every opening, doors red, far-room openings green") : TEXT("off"));
	}
	if (bRooms != bRoomsWereOn)
	{
		bRoomsWereOn = bRooms;
		if (!bRooms)
		{
			ClearRooms();
		}
		UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: far rooms %s"), bRooms ? TEXT("on") : TEXT("off"));
	}
	if (!bPanels && (Blocks.Num() > 0 || Meshes.Num() > 0))
	{
		ClearAll();
		Meshes.Reset();
		UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: off, door panels removed"));
	}
	if (!bPanels && !bRooms)
	{
		bAllHidden = true;
		return;
	}
	UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = Game ? Game->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
	if (!Dat || !Client)
	{
		bAllHidden = true;
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
	UACETerrainPresenterComponent* Terrain = Presenter.Get();
	if (Terrain && TickPresenter != Terrain)
	{
		// Show and hide after UACETerrainPresenterComponent::TickComponent (UpdateCameraVisibility) of the same frame.
		Tick.AddPrerequisite(Terrain, Terrain->PrimaryComponentTick);
		TickPresenter = Terrain;
		UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: per-frame check runs after the terrain presenter (tick group %d, presenter's %d)"),
			int32(Tick.TickGroup), int32(Terrain->PrimaryComponentTick.TickGroup));
	}
	// As the far scenery: not at the login screen, in portal space or in a dungeon.
	const uint32 Cell = uint32(Client->GetPlayerPosition().CellId);
	if (!Terrain || Terrain->LoadRadius <= 0 || (Cell & 0xFFFFu) == 0 || !Terrain->NeedsExteriorTerrain(Cell)
		|| Dat->IsInPortalSpace() || !Terrain->IsLandblockSpawned(Cell))
	{
		bAllHidden = true;
		return;
	}
	bAllHidden = false;
	const FIntPoint Here(int32(Cell >> 24), int32((Cell >> 16) & 0xFFu));
	const int32 OuterRing = PersonalFarWindows::OuterRing(Dat->GetWorldFogEndCm(), Terrain->WorldScale);
	if (bPanels)
	{
		Stream(*Dat, Here, Terrain->WorldScale, OuterRing);
	}
	if (bRooms)
	{
		StreamRooms(*Dat, Here, Terrain->WorldScale, OuterRing);
		if (const APlayerController* Controller = World->GetFirstPlayerController())
		{
			if (Controller->PlayerCameraManager)
			{
				UpdateRoomFog(Controller->PlayerCameraManager->GetCameraLocation(), PersonalFarWindows::NightAmount(Client->GetGameDayFraction(0.5f)), Terrain->WorldScale);
			}
		}
		UpdateRoomLift(Terrain->WorldScale);
	}
}

void UPersonalFarWindowsSubsystem::Stream(UACEDatSubsystem& Dat, FIntPoint InCenter, float WorldScale, int32 OuterRing)
{
	if (WorldScale != BuiltWorldScale)
	{
		ClearAll();
		Meshes.Reset();
		BuiltWorldScale = WorldScale;
	}
	Center = InCenter;
	for (auto It = Blocks.CreateIterator(); It; ++It)
	{
		// Kept one ring further, so they do not come and go at the edge.
		if (!PersonalFarWindows::IsPrepared(It.Key(), Center, OuterRing + 1))
		{
			if (AActor* Actor = It.Value().Actor.Get()) Actor->Destroy();
			It.RemoveCurrent();
			bPanelsLogged = false;
		}
	}
	TArray<FIntPoint> Todo;
	for (int32 DX = -OuterRing; DX <= OuterRing; ++DX)
	{
		for (int32 DY = -OuterRing; DY <= OuterRing; ++DY)
		{
			const FIntPoint Block = Center + FIntPoint(DX, DY);
			if (PersonalFarWindows::IsPrepared(Block, Center, OuterRing) && !Blocks.FindOrAdd(Block).bComplete)
			{
				Todo.Add(Block);
			}
		}
	}
	if (Todo.Num() == 0)
	{
		if (!bPanelsLogged)
		{
			bPanelsLogged = true;
			const FStats Stats = GetStats();
			UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: panels ready around %02X%02X, rings 0 to %d: %d panels (%d doors) in %d landblocks, %d meshes"),
				Center.X, Center.Y, OuterRing, Stats.Panels, Stats.Doors, Stats.Landblocks, Stats.Meshes);
		}
		return;
	}
	bPanelsLogged = false;
	Todo.Sort([this](const FIntPoint& A, const FIntPoint& B) { return (A - Center).SizeSquared() < (B - Center).SizeSquared(); });
	const double Start = FPlatformTime::Seconds();
	const double Budget = FMath::Clamp(CVarFarWindowsBudgetMs.GetValueOnGameThread(), 0.1f, 100.f) * 0.001;
	int32 MeshesBuilt = 0;
	for (const FIntPoint Block : Todo)
	{
		if (!BuildBlock(Dat, Block, Blocks.FindChecked(Block), WorldScale, Start, Budget, MeshesBuilt) && FPlatformTime::Seconds() - Start >= Budget)
		{
			break;
		}
	}
}

bool UPersonalFarWindowsSubsystem::BuildBlock(UACEDatSubsystem& Dat, FIntPoint Block, FBlock& Entry, float WorldScale, double Start, double Budget, int32& InOutMeshesBuilt)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const uint32 Key = LandblockKey(Block);
	const auto Info = Dat.GetLandblockInfo(Key);
	const PersonalFarSceneryMeshes::FPanelLook Look{PanelMaterials, PanelTextures};
	while (Info && Entry.BuildingsDone < Info->Buildings.Num())
	{
		const FACEDatLandblockBuilding& Building = Info->Buildings[Entry.BuildingsDone];
		const FACEBuiltSetupMesh* Setup = Building.ModelId != 0 ? Dat.FindSetupMesh(Building.ModelId, WorldScale, 0) : nullptr;
		if (!Setup)
		{
			// The far scenery and the client ask for the same meshes; a failed one has no panels.
			if (Building.ModelId == 0 || Dat.RequestSetupMesh(Building.ModelId, WorldScale, 0) == UACEDatSubsystem::EACESetupMeshStatus::Failed)
			{
				++Entry.BuildingsDone;
				continue;
			}
			return false;
		}
		// The openings that get a panel: the doors; in test mode every opening. By the room behind them
		// (UACEDatSubsystem::GetBuildingDoorwayApertures); an opening with no room has none, the survey found no such opening.
		TArray<TPair<int32, int32>> Openings;
		TSet<int32> DoorIndices;
		TArray<PersonalFarWindows::EKind> Kinds;
		PersonalFarSceneryMeshes::OpeningKinds(*Setup, WorldScale, Kinds);
		PersonalFarWindowsOverrides::Apply(PersonalFarWindowsOverrides::Get(), Building.ModelId, Key, Entry.BuildingsDone, Kinds);
		int32 KindAt = 0;
		for (const FACEBuiltSetupPart& Part : Setup->Parts)
		{
			for (const FACEBuiltSetupPortal& Opening : Part.Portals)
			{
				if (Opening.Vertices.Num() < 3)
				{
					continue;
				}
				const PersonalFarWindows::EKind Kind = Kinds[KindAt++];
				if (!Building.Portals.IsValidIndex(Opening.PortalIndex) || !PersonalFarRooms::IsRoom(Building.Portals[Opening.PortalIndex].OtherCellId))
				{
					continue;
				}
				if (Kind != PersonalFarWindows::EKind::Door && !bKindsWereOn)
				{
					continue;
				}
				Openings.AddUnique(TPair<int32, int32>(Opening.PortalIndex, int32(Key | Building.Portals[Opening.PortalIndex].OtherCellId)));
				if (Kind == PersonalFarWindows::EKind::Door) DoorIndices.Add(Opening.PortalIndex);
			}
		}
		for (const TPair<int32, int32>& Opening : Openings)
		{
			// By type, opening and whether it is a door (an override can make it one).
			const uint64 MeshKey = (uint64(DoorIndices.Contains(Opening.Key)) << 48) | (uint64(Building.ModelId) << 16) | uint64(Opening.Key);
			if (!Meshes.Contains(MeshKey))
			{
				// At least one new mesh per update.
				if (InOutMeshesBuilt > 0 && FPlatformTime::Seconds() - Start >= Budget)
				{
					return false;
				}
				Meshes.Add(MeshKey, PersonalFarSceneryMeshes::MakePanelMesh(Dat, this, Building.ModelId, WorldScale, Look, nullptr, Opening.Key, &Kinds));
				++InOutMeshesBuilt;
			}
		}
		AActor* Actor = Entry.Actor.Get();
		if (!Actor && Openings.Num() > 0)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
			if (!Actor)
			{
				return false;
			}
			USceneComponent* Root = NewObject<USceneComponent>(Actor);
			Root->SetMobility(EComponentMobility::Movable);
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			Actor->SetActorLocation(FACEPosition::AceVectorToUnreal(FVector(Block.X * 192., Block.Y * 192., 0.), WorldScale));
			Entry.Actor = Actor;
		}
		// Where the client puts the building's shell, relative to the landblock.
		const FTransform Frame(FACEPosition::AceQuatToUnreal(Building.Orientation.W, FVector(Building.Orientation.X, Building.Orientation.Y, Building.Orientation.Z)),
			FACEPosition::AceVectorToUnreal(FVector(Building.Origin.X, Building.Origin.Y, Building.Origin.Z), WorldScale));
		const float Cull = Openings.Num() > 0 ? BuildingDrawDistance(*Setup) : 0.f;
		for (const TPair<int32, int32>& Opening : Openings)
		{
			UStaticMesh* Mesh = Meshes.FindRef((uint64(DoorIndices.Contains(Opening.Key)) << 48) | (uint64(Building.ModelId) << 16) | uint64(Opening.Key));
			if (!Mesh)
			{
				continue;
			}
			UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
			Component->SetupAttachment(Actor->GetRootComponent());
			Component->SetStaticMesh(Mesh);
			Component->ComponentTags.Add(TEXT("PersonalFarWindowsPanel"));
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetGenerateOverlapEvents(false);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			Component->bCastDynamicShadow = false;
			Component->bUseAsOccluder = false;
			Component->SetReceivesDecals(false);
			Component->SetRelativeTransform(Frame);
			// Drawn as far as far scenery draws the building.
			SetBuildingCull(*Component, Cull, WorldScale);
			// Hidden until the per-frame check shows it.
			Component->SetVisibility(false);
			Component->RegisterComponent();
			FPanel& Panel = Entry.Panels.AddDefaulted_GetRef();
			Panel.Component = Component;
			Panel.Room = Opening.Value;
			Panel.Building = Entry.BuildingsDone;
			Panel.bDoor = DoorIndices.Contains(Opening.Key);
		}
		++Entry.BuildingsDone;
	}
	Entry.bComplete = true;
	return true;
}

void UPersonalFarWindowsSubsystem::ApplyLook()
{
	// The materials are made with the first panel; nothing to do before that.
	if (PanelTextures.Num() != PersonalFarWindows::KindCount || PanelMaterials.Num() != PersonalFarWindows::KindCount)
	{
		LookMode = 0;
		return;
	}
	const int32 Mode = bKindsWereOn ? 2 : 3;
	float Night = 0.f;
	if (Mode == 3)
	{
		const UWorld* World = GetWorld();
		const UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
		const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
		Night = PersonalFarWindows::NightAmount(Client ? Client->GetGameDayFraction(0.5f) : 0.5f);
	}
	if (Mode != LookMode)
	{
		// Own color, no sun or shadow; the distance fog as the walls.
		for (UMaterialInterface* Material : PanelMaterials)
		{
			if (UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Material))
			{
				Dynamic->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
				Dynamic->SetScalarParameterValue(TEXT("FogAmount"), 1.f);
			}
		}
		LookMode = Mode;
	}
	if (AppliedColor.Num() != PersonalFarWindows::KindCount)
	{
		AppliedColor.Init(FColor(0, 0, 0, 0), PersonalFarWindows::KindCount);
	}
	// A pixel is written only when it changed, so the color moves in steps one can see and no more often.
	const FColor Door = PersonalFarWindows::Scaled(PersonalFarWindows::DoorColor(), PersonalFarWindows::DimLevel(Night));
	for (int32 Index = 0; Index < PersonalFarWindows::KindCount; ++Index)
	{
		const PersonalFarWindows::EKind Kind = PersonalFarWindows::EKind(Index);
		FColor Color = Mode == 2 ? PersonalFarWindows::TestColor(Kind) : Door;
		Color.A = 255;
		if (AppliedColor[Index] != Color)
		{
			AppliedColor[Index] = Color;
			PersonalFarSceneryMeshes::SetPanelColor(PanelTextures[Index], Color);
		}
	}
}

void UPersonalFarWindowsSubsystem::PruneRoomMeshes()
{
	TSet<uint32> Used;
	for (const auto& Pair : RoomBlocks)
	{
		for (const FRoomBuilding& Building : Pair.Value.Buildings) Used.Add(Building.MeshKey);
	}
	for (auto It = RoomMeshes.CreateIterator(); It; ++It)
	{
		if (!Used.Contains(It.Key()))
		{
			RoomMeshSlots.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
	// The client's instances that only those meshes used.
	TSet<const UMaterialInterface*> Drawn;
	for (const auto& Pair : RoomMeshes)
	{
		if (const UStaticMesh* Mesh = Pair.Value.Get())
		{
			for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials()) Drawn.Add(Slot.MaterialInterface);
		}
	}
	RoomMaterials.RemoveAll([&Drawn](const TObjectPtr<UMaterialInterface>& Material) { return !Drawn.Contains(Material.Get()); });
}

void UPersonalFarWindowsSubsystem::ClearRooms()
{
	for (auto& Pair : RoomBlocks)
	{
		if (AActor* Actor = Pair.Value.Actor.Get()) Actor->Destroy();
	}
	RoomBlocks.Reset();
	RoomMeshes.Reset();
	RoomMeshSlots.Reset();
	RoomMaterials.Reset();
	bRoomsLogged = false;
}

UPersonalFarWindowsSubsystem::FRoomStats UPersonalFarWindowsSubsystem::GetRoomStats() const
{
	FRoomStats Stats;
	for (const auto& Pair : RoomBlocks)
	{
		for (const FRoomBuilding& Building : Pair.Value.Buildings)
		{
			++Stats.Buildings;
			Stats.Rooms += Building.Rooms;
			Stats.Triangles += Building.Triangles;
			Stats.Components += Building.Component.IsValid() ? 1 : 0;
			Stats.Shown += Building.bShown ? 1 : 0;
			if (const UPersonalFarRoomComponent* Own = Cast<UPersonalFarRoomComponent>(Building.Component.Get()))
			{
				++Stats.OwnComponents;
				Stats.OwnProxies += (Building.bShown && Own->HasOwnProxy()) ? 1 : 0;
			}
			for (const TWeakObjectPtr<UMaterialInstanceDynamic>& Own : Building.FogMaterials) Stats.FogInstances += Own.IsValid() ? 1 : 0;
		}
	}
	Stats.Meshes = RoomMeshes.Num();
	return Stats;
}

void UPersonalFarWindowsSubsystem::StreamRooms(UACEDatSubsystem& Dat, FIntPoint InCenter, float WorldScale, int32 OuterRing)
{
	if (WorldScale != RoomsWorldScale)
	{
		ClearRooms();
		RoomsWorldScale = WorldScale;
	}
	// Own DAT handles and room builder, opened on a worker on first use.
	if (!RoomSource)
	{
		if (!bRoomSourceRequested)
		{
			bRoomSourceRequested = true;
			PendingRoomSource = Async(EAsyncExecution::ThreadPool, [Directory = Dat.GetDatDirectory()]() { return PersonalFarRoomsMeshes::Open(Directory); });
		}
		if (!PendingRoomSource.IsReady())
		{
			return;
		}
		RoomSource = PendingRoomSource.Get();
		if (!RoomSource)
		{
			return;
		}
	}
	bool bRemoved = false;
	for (auto It = RoomBlocks.CreateIterator(); It; ++It)
	{
		// Kept one ring further, so they do not come and go at the edge.
		if (!PersonalFarRooms::IsWanted(It.Key(), InCenter, OuterRing + 1) && !(bRoomsBusy && It.Key() == RoomsBusyBlock))
		{
			if (AActor* Actor = It.Value().Actor.Get()) Actor->Destroy();
			It.RemoveCurrent();
			bRoomsLogged = false;
			bRemoved = true;
		}
	}
	if (bRemoved)
	{
		PruneRoomMeshes();
	}
	// What the worker built.
	if (bRoomsBusy && PendingRooms.IsReady())
	{
		bRoomsBusy = false;
		TArray<PersonalFarRoomsMeshes::FBuiltPtr> Built = PendingRooms.Get();
		if (FRoomBlock* Block = RoomBlocks.Find(RoomsBusyBlock))
		{
			for (int32 I = 0; I < Block->Jobs.Num() && I < Built.Num(); ++I)
			{
				Block->Jobs[I].Built = Built[I];
			}
			Block->State = 2;
		}
	}
	TArray<FIntPoint> Todo;
	for (int32 DX = -OuterRing; DX <= OuterRing; ++DX)
	{
		for (int32 DY = -OuterRing; DY <= OuterRing; ++DY)
		{
			const FIntPoint Block = InCenter + FIntPoint(DX, DY);
			if (PersonalFarRooms::IsWanted(Block, InCenter, OuterRing) && RoomBlocks.FindOrAdd(Block).State != 3)
			{
				Todo.Add(Block);
			}
		}
	}
	if (Todo.Num() == 0)
	{
		if (!bRoomsLogged)
		{
			bRoomsLogged = true;
			const FRoomStats Stats = GetRoomStats();
			UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: far rooms ready around %02X%02X, rings 0 to %d: %d buildings, %d rooms, %d triangles, %d components, %d meshes, %d materials kept, %d fog instances; %d own components (cannot be occluded), %d of %d shown with the own scene proxy"),
				InCenter.X, InCenter.Y, OuterRing, Stats.Buildings, Stats.Rooms, Stats.Triangles, Stats.Components, Stats.Meshes, RoomMaterials.Num(), Stats.FogInstances, Stats.OwnComponents, Stats.OwnProxies, Stats.Shown);
		}
		return;
	}
	bRoomsLogged = false;
	Todo.Sort([InCenter](const FIntPoint& A, const FIntPoint& B) { return (A - InCenter).SizeSquared() < (B - InCenter).SizeSquared(); });
	const double Start = FPlatformTime::Seconds();
	const double Budget = PersonalFarRooms::BudgetMs() * 0.001;
	int32 MeshesMade = 0;
	UWorld* World = GetWorld();
	for (const FIntPoint Block : Todo)
	{
		FRoomBlock& Entry = RoomBlocks.FindChecked(Block);
		const uint32 Key = LandblockKey(Block);
		if (Entry.State == 0)
		{
			// Which buildings have openings that show rooms, and which rooms.
			const auto Info = Dat.GetLandblockInfo(Key);
			bool bWait = false;
			Entry.Jobs.Reset();
			for (int32 Index = 0; Info && Index < Info->Buildings.Num() && !bWait; ++Index)
			{
				const FACEDatLandblockBuilding& Building = Info->Buildings[Index];
				const FACEBuiltSetupMesh* Setup = Building.ModelId != 0 ? Dat.FindSetupMesh(Building.ModelId, WorldScale, 0) : nullptr;
				if (!Setup)
				{
					bWait = Building.ModelId != 0 && Dat.RequestSetupMesh(Building.ModelId, WorldScale, 0) != UACEDatSubsystem::EACESetupMeshStatus::Failed;
					continue;
				}
				TArray<PersonalFarWindows::EKind> Kinds;
				PersonalFarSceneryMeshes::OpeningKinds(*Setup, WorldScale, Kinds);
				PersonalFarWindowsOverrides::Apply(PersonalFarWindowsOverrides::Get(), Building.ModelId, Key, Index, Kinds);
				FRoomJob Job;
				Job.Building = Index;
				Job.CullDistance = BuildingDrawDistance(*Setup);
				int32 At = 0;
				for (const FACEBuiltSetupPart& Part : Setup->Parts)
				{
					for (const FACEBuiltSetupPortal& Opening : Part.Portals)
					{
						if (Opening.Vertices.Num() < 3)
						{
							continue;
						}
						const PersonalFarWindows::EKind Kind = Kinds[At++];
						if (!PersonalFarRooms::GetsRooms(Kind) || !Building.Portals.IsValidIndex(Opening.PortalIndex))
						{
							continue;
						}
						const FACEDatBuildingPortal& Portal = Building.Portals[Opening.PortalIndex];
						PersonalFarRooms::AddRooms(Key, Portal.OtherCellId, Portal.StabCells, Job.Rooms);
						if (PersonalFarRooms::IsRoom(Portal.OtherCellId)) Job.Entries.AddUnique(int32(Key | Portal.OtherCellId));
					}
				}
				if (Job.Rooms.Num() == 0)
				{
					continue;
				}
				// Where the client puts the building's shell, relative to the landblock.
				Job.Frame = FTransform(FACEPosition::AceQuatToUnreal(Building.Orientation.W, FVector(Building.Orientation.X, Building.Orientation.Y, Building.Orientation.Z)),
					FACEPosition::AceVectorToUnreal(FVector(Building.Origin.X, Building.Origin.Y, Building.Origin.Z), WorldScale));
				Entry.Jobs.Add(MoveTemp(Job));
			}
			if (bWait)
			{
				// A building's model is still loading; again at the next update.
				Entry.Jobs.Reset();
				continue;
			}
			Entry.State = Entry.Jobs.Num() > 0 ? 1 : 3;
		}
		if (Entry.State == 1 && !bRoomsBusy)
		{
			// One landblock at a time on a worker: the room builder is not shared between threads.
			bRoomsBusy = true;
			RoomsBusyBlock = Block;
			TArray<TPair<TArray<uint32>, FTransform>> Work;
			for (const FRoomJob& Job : Entry.Jobs) Work.Emplace(Job.Rooms, Job.Frame);
			PendingRooms = Async(EAsyncExecution::ThreadPool, [Source = RoomSource, Work = MoveTemp(Work), WorldScale]()
			{
				TArray<PersonalFarRoomsMeshes::FBuiltPtr> Built;
				for (const TPair<TArray<uint32>, FTransform>& One : Work)
				{
					Built.Add(PersonalFarRoomsMeshes::Build(*Source, One.Key, One.Value, WorldScale));
				}
				return Built;
			});
		}
		if (Entry.State == 2)
		{
			const FTransform BlockToWorld(FACEPosition::AceVectorToUnreal(FVector(Block.X * 192., Block.Y * 192., 0.), WorldScale));
			while (Entry.Jobs.Num() > 0)
			{
				// At least one building per update.
				if (MeshesMade > 0 && FPlatformTime::Seconds() - Start >= Budget)
				{
					break;
				}
				FRoomJob Job = Entry.Jobs.Pop(EAllowShrinking::No);
				if (!Job.Built || Job.Built->Sections.Num() == 0)
				{
					continue;
				}
				UStaticMesh* Mesh = RoomMeshes.FindRef(Job.Built->Key);
				if (!Mesh)
				{
					TArray<UMaterialInterface*> Materials;
					TArray<int32> Slots;
					Mesh = PersonalFarRoomsMeshes::MakeStaticMesh(Dat, this, *Job.Built, Materials, &Slots);
					++MeshesMade;
					if (!Mesh)
					{
						continue;
					}
					RoomMeshes.Add(Job.Built->Key, Mesh);
					RoomMeshSlots.Add(Job.Built->Key, MoveTemp(Slots));
					// Hard references: the client's own instances stay in use when its cache lets go of them.
					for (UMaterialInterface* Material : Materials) RoomMaterials.AddUnique(Material);
				}
				AActor* Actor = Entry.Actor.Get();
				if (!Actor)
				{
					FActorSpawnParameters Params;
					Params.ObjectFlags |= RF_Transient;
					Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
					if (!Actor)
					{
						continue;
					}
					USceneComponent* Root = NewObject<USceneComponent>(Actor);
					Root->SetMobility(EComponentMobility::Movable);
					Actor->SetRootComponent(Root);
					Root->RegisterComponent();
					Actor->SetActorLocation(BlockToWorld.GetLocation());
					Entry.Actor = Actor;
				}
				UStaticMeshComponent* Component = NewObject<UPersonalFarRoomComponent>(Actor);
				Component->SetupAttachment(Actor->GetRootComponent());
				Component->SetStaticMesh(Mesh);
				Component->ComponentTags.Add(TEXT("PersonalFarRooms"));
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Component->SetGenerateOverlapEvents(false);
				Component->SetCanEverAffectNavigation(false);
				Component->SetCastShadow(false);
				Component->bCastDynamicShadow = false;
				Component->bUseAsOccluder = false;
				Component->SetReceivesDecals(false);
				// Drawn as far as far scenery draws the building.
				SetBuildingCull(*Component, Job.CullDistance, WorldScale);
				// As the client's rooms (AACEEnvCellActor): the sun is on channel 0 and does not light rooms.
				Component->SetLightingChannels(false, true, false);
				// A little higher than the client's room (personal.FarWindows.RoomLift): its floor must not fight the ground in the same plane.
				Component->SetRelativeTransform(Job.Frame * FTransform(PersonalFarRooms::LiftOffset(WorldScale)));
				// Hidden until the per-frame check shows it.
				Component->SetVisibility(false);
				Component->RegisterComponent();
				TArray<UMaterialInstanceDynamic*> Shaded;
				PersonalFarRoomsMeshes::ApplyDoorShading(*Component, *Job.Built, RoomMeshSlots.FindRef(Job.Built->Key), Job.Frame * BlockToWorld, Shaded);
				FRoomBuilding& Building = Entry.Buildings.AddDefaulted_GetRef();
				Building.Component = Component;
				Building.Building = Job.Building;
				Building.Entries = MoveTemp(Job.Entries);
				Building.Rooms = Job.Built->Rooms;
				Building.Triangles = Job.Built->Triangles;
				Building.MeshKey = Job.Built->Key;
				Building.Frame = Job.Frame;
				for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
				{
					Building.BaseMaterials.Add(Component->GetMaterial(Slot));
				}
				Building.FogMaterials.SetNum(Building.BaseMaterials.Num());
			}
			if (Entry.Jobs.Num() == 0)
			{
				Entry.State = 3;
			}
		}
		if (FPlatformTime::Seconds() - Start >= Budget && MeshesMade > 0)
		{
			break;
		}
	}
}

void UPersonalFarWindowsSubsystem::UpdateRoomFog(const FVector& Camera, float Night, float WorldScale)
{

	const float Near = PersonalFarRooms::RoomFogNear();
	const float Far = PersonalFarRooms::RoomFogFar();
	for (auto& Pair : RoomBlocks)
	{
		for (FRoomBuilding& Building : Pair.Value.Buildings)
		{
			UStaticMeshComponent* Component = Building.Component.Get();
			if (!Component)
			{
				continue;
			}
			const float Meters = float(FVector::Dist(Component->Bounds.Origin, Camera) / FMath::Max(WorldScale, 1.f));
			const int32 Step = PersonalFarRooms::FogStep(PersonalFarRooms::RoomFog(Night, Meters, Near, Far));
			if (Step == Building.FogStep)
			{
				continue;
			}
			Building.FogStep = Step;
			const float Amount = Step * PersonalFarRooms::FogStepSize;
			for (int32 Slot = 0; Slot < Building.BaseMaterials.Num(); ++Slot)
			{
				UMaterialInstanceDynamic* Own = Building.FogMaterials[Slot].Get();
				if (!Own && Step > 0)
				{
					// An own copy of the client's instance (or of its doorway-shaded one), made the first time it takes fog.
					UMaterialInterface* Base = Building.BaseMaterials[Slot].Get();
					if (!Base || !Base->GetMaterial())
					{
						continue;
					}
					Own = UMaterialInstanceDynamic::Create(Base->GetMaterial(), Component);
					if (!Own)
					{
						continue;
					}
					Own->CopyMaterialUniformParameters(Base);
					Component->SetMaterial(Slot, Own);
					Building.FogMaterials[Slot] = Own;
				}
				if (Own)
				{
					Own->SetScalarParameterValue(TEXT("FogAmount"), Amount);
				}
			}
		}
	}
}

void UPersonalFarWindowsSubsystem::UpdateRoomLift(float WorldScale)
{
	const FVector Offset = PersonalFarRooms::LiftOffset(WorldScale);
	if (Offset == AppliedLift)
	{
		return;
	}
	AppliedLift = Offset;
	for (auto& Pair : RoomBlocks)
	{
		for (FRoomBuilding& Building : Pair.Value.Buildings)
		{
			if (UStaticMeshComponent* Component = Building.Component.Get())
			{
				Component->SetRelativeTransform(Building.Frame * FTransform(Offset));
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: far rooms lifted by %.1f cm"), Offset.Z * 100.f / FMath::Max(WorldScale, 1.f));
}

void UPersonalFarWindowsSubsystem::ApplyRoomsShown()
{
	if (RoomBlocks.Num() == 0)
	{
		return;
	}
	const UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = Game ? Game->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
	const UACETerrainPresenterComponent* Terrain = Presenter.Get();
	const uint32 Cell = Client ? uint32(Client->GetPlayerPosition().CellId) : 0u;
	if (Cell != RoomsOccupiedCell)
	{
		RoomsOccupiedCell = Cell;
		RoomsOccupiedBuilding = ((Cell & 0xFFFFu) >= 0x100u && Dat && Terrain)
			? Dat->FindBuildingInfoIndexForIndoorCell(Cell & 0xFFFF0000u, Cell, Terrain->WorldScale) : INDEX_NONE;
	}
	// The player's landblock as it is this frame.
	const FIntPoint Live(int32(Cell >> 24), int32((Cell >> 16) & 0xFFu));
	const bool bOn = !bAllHidden && Terrain && (Cell & 0xFFFFu) != 0;
	const bool bAlways = PersonalFarRooms::RoomsAlways();
	// personal.FarWindows.LogFlicker: every change within 600 m of the camera, with the frame number.
	const bool bLog = PersonalFarRooms::LogFlicker();
	FVector Camera = FVector::ZeroVector;
	if (bLog)
	{
		if (const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr)
		{
			if (Controller->PlayerCameraManager) Camera = Controller->PlayerCameraManager->GetCameraLocation();
		}
	}
	const float LogReach = 600.f * (Terrain ? Terrain->WorldScale : 100.f);
	for (auto& Pair : RoomBlocks)
	{
		const int32 Ring = PersonalFarScenery::Ring(Pair.Key, Live);
		const bool bInsideBlock = RoomsOccupiedBuilding != INDEX_NONE && Pair.Key == Live;
		for (int32 Index = 0; Index < Pair.Value.Buildings.Num(); ++Index)
		{
			FRoomBuilding& Building = Pair.Value.Buildings[Index];
			bool bShow = false;
			uint32 Drawn = 0;
			if (bOn)
			{
				// Which of the rooms behind its openings the client draws this frame (only asked within its own rings).
				bool bNotDrawn = Ring > PersonalFarRooms::ClientRoomRing;
				if (!bNotDrawn || bLog)
				{
					for (int32 I = 0; I < Building.Entries.Num(); ++I)
					{
						const bool bDrawn = Terrain->IsWorldCellVisible(Building.Entries[I]);
						Drawn |= (bDrawn && I < 32) ? (1u << I) : 0u;
						bNotDrawn = bNotDrawn || !bDrawn;
					}
				}
				// personal.FarWindows.RoomsAlways: shown whatever the client draws.
				bShow = PersonalFarRooms::ShowRooms(Ring, bNotDrawn || bAlways, bInsideBlock && Building.Building == RoomsOccupiedBuilding);
			}
			UStaticMeshComponent* Component = Building.Component.Get();
			if (bLog && Component && (bShow != Building.bShown || Drawn != Building.EntriesDrawn) && FVector::Dist(Component->Bounds.Origin, Camera) <= LogReach)
			{
				const UPersonalFarRoomComponent* Own = Cast<UPersonalFarRoomComponent>(Component);
				UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: flicker frame %llu: %02X%02X building %d (ring %d, %.0f m): far rooms %s -> %s; client draws %d of %d rooms behind (0x%X -> 0x%X)%s%s"),
					GFrameCounter, Pair.Key.X, Pair.Key.Y, Building.Building, Ring, FVector::Dist(Component->Bounds.Origin, Camera) / (Terrain ? Terrain->WorldScale : 100.f),
					Building.bShown ? TEXT("shown") : TEXT("hidden"), bShow ? TEXT("shown") : TEXT("hidden"), FMath::CountBits(Drawn), Building.Entries.Num(),
					Building.EntriesDrawn, Drawn, bAlways ? TEXT(", always on") : TEXT(""),
					!Own ? TEXT(", not the own component") : (Building.bShown && !Own->HasOwnProxy() ? TEXT(", shown without the own proxy") : TEXT("")));
			}
			Building.EntriesDrawn = Drawn;
			if (bShow != Building.bShown)
			{
				Building.bShown = bShow;
				if (Component)
				{
					Component->SetVisibility(bShow);
				}
			}
		}
	}
}

void UPersonalFarWindowsSubsystem::Mark(const FString& What)
{
	using PersonalFarWindowsOverrides::EOverride;
	const EOverride Override = What.Equals(TEXT("door"), ESearchCase::IgnoreCase) ? EOverride::Door
		: (What.Equals(TEXT("open"), ESearchCase::IgnoreCase) ? EOverride::Open : EOverride::None);
	if (Override == EOverride::None && !What.Equals(TEXT("clear"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogTemp, Warning, TEXT("PersonalFarWindows: mark takes door, open or clear"));
		return;
	}
	UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = Game ? Game->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
	const APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
	const UACETerrainPresenterComponent* Terrain = Presenter.Get();
	if (!Dat || !Client || !Controller || !Controller->PlayerCameraManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("PersonalFarWindows: mark: no camera or no world"));
		return;
	}
	const float WorldScale = Terrain ? Terrain->WorldScale : 100.f;
	const FVector Camera = Controller->PlayerCameraManager->GetCameraLocation();
	const FVector Forward = Controller->PlayerCameraManager->GetCameraRotation().Vector();
	const uint32 Cell = uint32(Client->GetPlayerPosition().CellId);
	const FIntPoint Here(int32(Cell >> 24), int32((Cell >> 16) & 0xFFu));
	const int32 Rings = PersonalFarWindows::OuterRing(Dat->GetWorldFogEndCm(), WorldScale);

	// Every opening of every building around, in world space.
	struct FPolygon
	{
		TArray<FVector> Corners;
		FVector Normal = FVector::ZeroVector;
		FVector Middle = FVector::ZeroVector;
		int32 Order = 0;
		int32 Portal = 0;
	};
	struct FCandidate
	{
		uint32 ModelId = 0;
		uint32 Block = 0;
		int32 Building = 0;
		TArray<FPolygon> Polygons;
	};
	TArray<FCandidate> Candidates;
	for (int32 DX = -Rings; DX <= Rings; ++DX)
	{
		for (int32 DY = -Rings; DY <= Rings; ++DY)
		{
			const FIntPoint Block = Here + FIntPoint(DX, DY);
			if (!PersonalFarWindows::IsPrepared(Block, Here, Rings))
			{
				continue;
			}
			const uint32 Key = LandblockKey(Block);
			const auto Info = Dat->GetLandblockInfo(Key);
			const FVector BlockOrigin = FACEPosition::AceVectorToUnreal(FVector(Block.X * 192., Block.Y * 192., 0.), WorldScale);
			for (int32 Index = 0; Info && Index < Info->Buildings.Num(); ++Index)
			{
				const FACEDatLandblockBuilding& Building = Info->Buildings[Index];
				const FACEBuiltSetupMesh* Setup = Building.ModelId != 0 ? Dat->FindSetupMesh(Building.ModelId, WorldScale, 0) : nullptr;
				if (!Setup)
				{
					continue;
				}
				const FTransform Frame(FACEPosition::AceQuatToUnreal(Building.Orientation.W, FVector(Building.Orientation.X, Building.Orientation.Y, Building.Orientation.Z)),
					FACEPosition::AceVectorToUnreal(FVector(Building.Origin.X, Building.Origin.Y, Building.Origin.Z), WorldScale));
				FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
				Candidate.ModelId = Building.ModelId;
				Candidate.Block = Key;
				Candidate.Building = Index;
				int32 Order = 0;
				for (const FACEBuiltSetupPart& Part : Setup->Parts)
				{
					for (const FACEBuiltSetupPortal& Opening : Part.Portals)
					{
						if (Opening.Vertices.Num() < 3)
						{
							continue;
						}
						FPolygon& Polygon = Candidate.Polygons.AddDefaulted_GetRef();
						Polygon.Order = Order++;
						Polygon.Portal = Opening.PortalIndex;
						for (const FVector& Vertex : Opening.Vertices)
						{
							Polygon.Corners.Add(BlockOrigin + Frame.TransformPosition(Part.BindTransform.TransformPosition(Vertex)));
							Polygon.Middle += Polygon.Corners.Last();
						}
						Polygon.Middle /= Polygon.Corners.Num();
						for (int32 I = 1; I + 1 < Polygon.Corners.Num(); ++I)
						{
							Polygon.Normal += FVector::CrossProduct(Polygon.Corners[I] - Polygon.Corners[0], Polygon.Corners[I + 1] - Polygon.Corners[0]);
						}
						Polygon.Normal = Polygon.Normal.GetSafeNormal();
					}
				}
			}
		}
	}

	// The opening the center of the screen looks through: the nearest one the camera's line passes through.
	int32 BestCandidate = INDEX_NONE, BestPolygon = INDEX_NONE;
	double BestDistance = MAX_dbl;
	FString How = TEXT("at the center of the screen");
	for (int32 C = 0; C < Candidates.Num(); ++C)
	{
		for (int32 P = 0; P < Candidates[C].Polygons.Num(); ++P)
		{
			const FPolygon& Polygon = Candidates[C].Polygons[P];
			const double Facing = FVector::DotProduct(Polygon.Normal, Forward);
			if (FMath::Abs(Facing) < 1.e-4)
			{
				continue;
			}
			const double Along = FVector::DotProduct(Polygon.Normal, Polygon.Corners[0] - Camera) / Facing;
			if (Along <= 0. || Along >= BestDistance)
			{
				continue;
			}
			const FVector Hit = Camera + Forward * Along;
			bool bInside = false;
			for (int32 I = 1; I + 1 < Polygon.Corners.Num() && !bInside; ++I)
			{
				const FVector Weights = FMath::ComputeBaryCentric2D(Hit, Polygon.Corners[0], Polygon.Corners[I], Polygon.Corners[I + 1]);
				bInside = Weights.X >= -1.e-3 && Weights.Y >= -1.e-3 && Weights.Z >= -1.e-3;
			}
			if (bInside)
			{
				BestDistance = Along;
				BestCandidate = C;
				BestPolygon = P;
			}
		}
	}
	// Else the opening whose middle is nearest the center of the screen, within MaxDegrees.
	constexpr float MaxDegrees = 3.f;
	if (BestPolygon == INDEX_NONE)
	{
		double BestFacing = -2.;
		int32 NearCandidate = INDEX_NONE, NearPolygon = INDEX_NONE;
		for (int32 C = 0; C < Candidates.Num(); ++C)
		{
			for (int32 P = 0; P < Candidates[C].Polygons.Num(); ++P)
			{
				const double Facing = FVector::DotProduct((Candidates[C].Polygons[P].Middle - Camera).GetSafeNormal(), Forward);
				if (Facing > BestFacing)
				{
					BestFacing = Facing;
					NearCandidate = C;
					NearPolygon = P;
				}
			}
		}
		const float Degrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(float(BestFacing), -1.f, 1.f)));
		if (NearPolygon == INDEX_NONE || Degrees > MaxDegrees)
		{
			if (NearPolygon != INDEX_NONE)
			{
				const FCandidate& Near = Candidates[NearCandidate];
				UE_LOG(LogTemp, Warning, TEXT("PersonalFarWindows: mark %s: the center of the screen is on no opening; nearest 0x%08X [%d] (landblock %04X building %d, %.0f m away), %.1f degrees off center"),
					*What, Near.ModelId, Near.Polygons[NearPolygon].Order, Near.Block >> 16, Near.Building,
					FVector::Dist(Near.Polygons[NearPolygon].Middle, Camera) / WorldScale, Degrees);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("PersonalFarWindows: mark %s: no opening around"), *What);
			}
			return;
		}
		BestCandidate = NearCandidate;
		BestPolygon = NearPolygon;
		How = FString::Printf(TEXT("%.1f degrees off center"), Degrees);
	}

	// With it, the openings of the same building in the same plane that touch it: a doorway split in two
	// (two triangles, or a lower and an upper part) is one opening to the eye.
	const FCandidate& Chosen = Candidates[BestCandidate];
	const FPolygon& Hit = Chosen.Polygons[BestPolygon];
	const double Close = 0.05 * WorldScale;
	auto Touch = [Close](const FPolygon& A, const FPolygon& B)
	{
		for (const FVector& Point : A.Corners)
		{
			for (int32 I = 0; I < B.Corners.Num(); ++I)
			{
				if (FMath::PointDistToSegment(Point, B.Corners[I], B.Corners[(I + 1) % B.Corners.Num()]) <= Close) return true;
			}
		}
		return false;
	};
	TArray<int32> Group = {BestPolygon};
	for (int32 Next = 0; Next < Group.Num(); ++Next)
	{
		const FPolygon& From = Chosen.Polygons[Group[Next]];
		for (int32 P = 0; P < Chosen.Polygons.Num(); ++P)
		{
			const FPolygon& Other = Chosen.Polygons[P];
			if (Group.Contains(P) || FMath::Abs(FVector::DotProduct(Other.Normal, Hit.Normal)) < 0.995
				|| FMath::Abs(FVector::DotProduct(Other.Middle - Hit.Middle, Hit.Normal)) > Close || !Touch(From, Other) && !Touch(Other, From))
			{
				continue;
			}
			Group.Add(P);
		}
	}
	TArray<int32> Orders;
	FString Names;
	for (const int32 P : Group)
	{
		Orders.Add(Chosen.Polygons[P].Order);
		Names += FString::Printf(TEXT("%s[%d] (portal %d)"), Names.IsEmpty() ? TEXT("") : TEXT(", "), Chosen.Polygons[P].Order, Chosen.Polygons[P].Portal);
	}
	const bool bSaved = PersonalFarWindowsOverrides::SetForType(Chosen.ModelId, Orders, Override);
	UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: marked 0x%08X %s %s (landblock %04X building %d, %.0f m away, %s); %s %s"),
		Chosen.ModelId, *Names, What == TEXT("clear") ? TEXT("clear") : PersonalFarWindowsOverrides::Name(Override), Chosen.Block >> 16, Chosen.Building,
		FVector::Dist(Hit.Middle, Camera) / WorldScale, *How, bSaved ? TEXT("written to") : TEXT("could not write"), *PersonalFarWindowsOverrides::FilePath());
	// Made again with the new kinds: the panels at once, the far rooms over the next seconds.
	ClearAll();
	ClearRooms();
}

void UPersonalFarWindowsSubsystem::WatchRoom(int32 Room)
{
	for (TActorIterator<AACEEnvCellActor> It(GetWorld()); It; ++It)
	{
		if (It->EnvCellId == Room)
		{
			FWatch& Watch = Watching.AddDefaulted_GetRef();
			Watch.Room = Room;
			Watch.Actor = *It;
			Watch.Frame = GFrameCounter;
			Watch.WorldSeconds = GetWorld()->GetTimeSeconds();
			return;
		}
	}
}

void UPersonalFarWindowsSubsystem::UpdateWatching()
{
	const UWorld* World = GetWorld();
	if (Watching.Num() == 0 || !World)
	{
		FrameTimes.Reset();
		return;
	}
	// The renderer stamps a mesh with the world time of the frame it was drawn in; this maps that time back to a frame.
	FrameTimes.Add(TPair<uint64, float>(GFrameCounter, float(World->GetTimeSeconds())));
	if (FrameTimes.Num() > 1024)
	{
		FrameTimes.RemoveAt(0, 512);
	}
	for (int32 Index = Watching.Num() - 1; Index >= 0; --Index)
	{
		const FWatch& Watch = Watching[Index];
		const AACEEnvCellActor* Actor = Watch.Actor.Get();
		const UPrimitiveComponent* Mesh = Actor ? Actor->CellMesh.Get() : nullptr;
		const double Waited = World->GetTimeSeconds() - Watch.WorldSeconds;
		if (Mesh && Mesh->GetLastRenderTimeOnScreen() >= float(Watch.WorldSeconds) - 1.e-4f)
		{
			uint64 DrawnFrame = GFrameCounter;
			for (const TPair<uint64, float>& Frame : FrameTimes)
			{
				if (Frame.Value >= Mesh->GetLastRenderTimeOnScreen() - 1.e-4f)
				{
					DrawnFrame = Frame.Key;
					break;
				}
			}
			UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: room 0x%08X actor first shown in frame %llu, mesh on screen from frame %llu (%lld frames, %.0f ms later; seen in frame %llu)"),
				uint32(Watch.Room), Watch.Frame, DrawnFrame, int64(DrawnFrame) - int64(Watch.Frame),
				(Mesh->GetLastRenderTimeOnScreen() - float(Watch.WorldSeconds)) * 1000.f, GFrameCounter);
			Watching.RemoveAtSwap(Index);
		}
		else if (!Actor || Waited > 3.)
		{
			UE_LOG(LogTemp, Log, TEXT("PersonalFarWindows: room 0x%08X actor first shown in frame %llu, mesh not on screen within %.1f s (%s)"),
				uint32(Watch.Room), Watch.Frame, Waited, !Actor ? TEXT("actor gone") : (Mesh ? TEXT("off screen, covered, or nothing to draw") : TEXT("no mesh")));
			Watching.RemoveAtSwap(Index);
		}
	}
}


void UPersonalFarWindowsSubsystem::ApplyShown()
{
	UpdateWatching();
	if (Blocks.Num() == 0)
	{
		return;
	}
	const UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = Game ? Game->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
	const UACETerrainPresenterComponent* Terrain = Presenter.Get();
	// The building the player is in: the client hides its shell, and its panels must not show from inside.
	const uint32 Cell = Client ? uint32(Client->GetPlayerPosition().CellId) : 0u;
	if (Cell != OccupiedCell)
	{
		OccupiedCell = Cell;
		OccupiedBuilding = ((Cell & 0xFFFFu) >= 0x100u && Dat && Terrain)
			? Dat->FindBuildingInfoIndexForIndoorCell(Cell & 0xFFFF0000u, Cell, Terrain->WorldScale) : INDEX_NONE;
	}
	const FIntPoint OccupiedBlock(int32(Cell >> 24), int32((Cell >> 16) & 0xFFu));
	const float DoorReach = PersonalFarWindows::DoorDistance() * (Terrain ? Terrain->WorldScale : 100.f);
	const FVector Player = (Client && Terrain) ? Client->GetPlayerPosition().ToUnrealLocation(Terrain->WorldScale) : FVector::ZeroVector;
	const bool bOn = !bAllHidden && Terrain;
	const bool bTest = bKindsWereOn;
	const bool bLog = PersonalFarWindows::LogRooms();
	for (auto& Pair : Blocks)
	{
		const bool bInsideBlock = OccupiedBuilding != INDEX_NONE && Pair.Key == OccupiedBlock;
		for (FPanel& Panel : Pair.Value.Panels)
		{
			UStaticMeshComponent* Component = Panel.Component.Get();
			bool bShow = false;
			if (bOn && Component)
			{
				// A door is taken over by the server's door object once the player is near and the client draws the room behind it.
				bool bTakenOver = false;
				if (Panel.bDoor && !bTest)
				{
					Panel.bInReach = PersonalFarWindows::DoorInReach(float(FVector::Dist(Player, Component->Bounds.Origin)), DoorReach, Panel.bInReach);
					const bool bRoom = Panel.bInReach && Terrain->IsWorldCellVisible(Panel.Room);
					if (bRoom && !Panel.bRoomSeen)
					{
						Panel.bRoomSeen = true;
						if (bLog)
						{
							WatchRoom(Panel.Room);
						}
					}
					bTakenOver = bRoom;
				}
				bShow = PersonalFarWindows::ShowPanel(bTest, bTakenOver, bInsideBlock && Panel.Building == OccupiedBuilding);
			}
			if (bShow != Panel.bShown)
			{
				Panel.bShown = bShow;
				if (Component) Component->SetVisibility(bShow);
			}
		}
	}
}
