#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "Async/Future.h"
#include "Personal/PersonalFarRoomsMeshes.h"
#include "PersonalFarWindowsSubsystem.generated.h"

class AActor;
class AACEEnvCellActor;
class UACEDatSubsystem;
class UACETerrainPresenterComponent;
class UPersonalFarWindowsSubsystem;
class UPrimitiveComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;
struct FACEBuiltSetupMesh;

/** Runs UPersonalFarWindowsSubsystem::ApplyShown and ApplyRoomsShown each frame, after the terrain presenter has shown and hidden the rooms. */
struct FPersonalFarWindowsTick : public FTickFunction
{
	UPersonalFarWindowsSubsystem* Target = nullptr;
	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override { return TEXT("PersonalFarWindows"); }
	virtual FName DiagnosticContext(bool bDetailed) override { return FName(TEXT("PersonalFarWindows")); }
};

/**
 * Personal far windows (check box Far Terrain Rooms), out to the ring where far scenery draws buildings:
 * - Far rooms: the rooms behind every opening of a building that is not a door, one static mesh component per building.
 * - Door panels: a brown panel in every door opening, near and far, until the player is near and the client draws the room behind it.
 * - Test colors (personal.FarWindows.Kinds 1): a panel in every opening, doors red, the others green, always shown.
 * Made and removed on a timer; shown and hidden every frame. The rules are in PersonalFarWindows.h and PersonalFarRooms.h.
 */
UCLASS()
class UPersonalFarWindowsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	friend class FPersonalFarWindowsDrawTest;
	friend class FPersonalFarWindowsRoomsDrawTest;
public:
	static constexpr float UpdateSeconds = 0.1f;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	struct FStats
	{
		int32 Landblocks = 0;
		int32 Panels = 0;
		int32 Doors = 0;
		int32 Shown = 0;
		int32 Meshes = 0;
	};
	FStats GetStats() const;
	/** Per frame: show or hide every panel by the show rule. */
	void ApplyShown();
	/** Per frame: show or hide the far rooms of every building by the show rule (PersonalFarRooms::ShowRooms). */
	void ApplyRoomsShown();
	struct FRoomStats
	{
		int32 Buildings = 0;
		int32 Rooms = 0;
		int32 Triangles = 0;
		int32 Components = 0;
		int32 Shown = 0;
		int32 Meshes = 0;
		/** Far-room components of the own class, and of those shown, how many have the own scene proxy (cannot be occluded). */
		int32 OwnComponents = 0;
		int32 OwnProxies = 0;
		/** Own material instances of the day fog. */
		int32 FogInstances = 0;
	};
	FRoomStats GetRoomStats() const;
	/** personal.FarWindows.Mark door|open|clear: an override for the opening nearest the center of the screen, written to the file. */
	void Mark(const FString& What);

private:
	struct FPanel
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		/** The room behind the opening. */
		int32 Room = 0;
		/** Index of the building in the landblock's info file. */
		int32 Building = 0;
		bool bShown = false;
		/** A door panel; the others exist only in test mode. */
		bool bDoor = false;
		/** The player is near enough to this door for its panel to go (PersonalFarWindows::DoorInReach). */
		bool bInReach = false;
		/** The room behind was seen drawn once (for personal.FarWindows.LogRooms). */
		bool bRoomSeen = false;
	};
	struct FBlock
	{
		TWeakObjectPtr<AActor> Actor;
		TArray<FPanel> Panels;
		/** Buildings of the info file that have their panels. */
		int32 BuildingsDone = 0;
		bool bComplete = false;
	};

	/** Timer: which landblocks have panels and far rooms, and making them. */
	void Update();
	/** One update of the panels for a player in landblock Center, out to OuterRing. */
	void Stream(UACEDatSubsystem& Dat, FIntPoint Center, float WorldScale, int32 OuterRing);
	/** Make the panels of the next buildings of a landblock. False when time ran out or a building's mesh is not loaded yet. */
	bool BuildBlock(UACEDatSubsystem& Dat, FIntPoint Block, FBlock& Entry, float WorldScale, double Start, double Budget, int32& InOutMeshesBuilt);
	void ClearAll();
	/** The door color by the time of day, or the test colors (personal.FarWindows.Kinds), on the panel materials. */
	void ApplyLook();
	/** personal.FarWindows.LogRooms: follow a room from the frame its actor is shown to the frame its mesh is on screen. */
	void WatchRoom(int32 Room);
	void UpdateWatching();

	struct FWatch
	{
		int32 Room = 0;
		TWeakObjectPtr<AACEEnvCellActor> Actor;
		uint64 Frame = 0;
		double WorldSeconds = 0.;
	};
	TArray<FWatch> Watching;
	TArray<TPair<uint64, float>> FrameTimes;
	/** 0: the materials do not exist yet, 2: test colors, 3: door color. */
	int32 LookMode = 0;
	/** What each panel material was last set to, by kind. */
	TArray<FColor> AppliedColor;
	/** The materials all panels share, one per kind (PersonalFarSceneryMeshes::FPanelLook), each with its own 1 x 1 texture. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> PanelMaterials;
	UPROPERTY()
	TArray<TObjectPtr<UTexture2D>> PanelTextures;

	TMap<FIntPoint, FBlock> Blocks;
	/** One mesh per building type and opening: model id x 65536 + portal index. Null: no panel for it. */
	UPROPERTY()
	TMap<uint64, TObjectPtr<UStaticMesh>> Meshes;

	TWeakObjectPtr<UACETerrainPresenterComponent> Presenter;
	/** The presenter the tick was put behind. */
	TWeakObjectPtr<UACETerrainPresenterComponent> TickPresenter;
	FTimerHandle UpdateTimer;
	FPersonalFarWindowsTick Tick;
	FIntPoint Center = FIntPoint(MIN_int32, MIN_int32);
	float BuiltWorldScale = 0.f;
	/** Nothing is shown: off, login screen, portal space, dungeon. */
	bool bAllHidden = true;
	/** The building the player is in, from the player's cell. */
	uint32 OccupiedCell = 0;
	int32 OccupiedBuilding = INDEX_NONE;
	/** Test mode as the panels were made: a panel in every opening. */
	bool bKindsWereOn = false;
	bool bPanelsLogged = false;

	// Far rooms: the rooms behind every opening of a building that is not a door, one component per building.
	struct FRoomBuilding
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		/** Index of the building in the landblock's info file. */
		int32 Building = 0;
		/** The rooms directly behind its far-room openings: while the client draws them all, the far rooms are hidden. */
		TArray<int32> Entries;
		bool bShown = false;
		int32 Rooms = 0;
		int32 Triangles = 0;
		/** Which of Entries the client drew last frame (bit per room), for personal.FarWindows.LogFlicker. */
		uint32 EntriesDrawn = 0;
		/** The mesh it draws (PersonalFarRoomsMeshes::FBuilt::Key). */
		uint32 MeshKey = 0;
		/** Where the building stands in its landblock; the component stands there plus the lift. */
		FTransform Frame;
		/** Day fog: the step it was last set to (PersonalFarRooms::FogStep); 0 is the client's own materials. */
		int32 FogStep = 0;
		/** Per slot: the material it draws with no fog (the client's shared instance, or its own with the doorway shading). */
		TArray<TWeakObjectPtr<UMaterialInterface>> BaseMaterials;
		/** Per slot: its own instance, whose fog amount is set; null while it draws its base material. */
		TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> FogMaterials;
	};
	struct FRoomJob
	{
		int32 Building = 0;
		/** Where the building stands in its landblock. */
		FTransform Frame;
		/** As far as far scenery draws the building. */
		float CullDistance = 0.f;
		TArray<uint32> Rooms;
		TArray<int32> Entries;
		PersonalFarRoomsMeshes::FBuiltPtr Built;
	};
	struct FRoomBlock
	{
		TWeakObjectPtr<AActor> Actor;
		TArray<FRoomBuilding> Buildings;
		TArray<FRoomJob> Jobs;
		/** 0: rooms to find, 1: waits for the worker, 2: meshes and components to make, 3: done. */
		int32 State = 0;
	};
	/** One update of the far rooms for a player in landblock Center, out to OuterRing. */
	void StreamRooms(UACEDatSubsystem& Dat, FIntPoint Center, float WorldScale, int32 OuterRing);
	void ClearRooms();
	/** Drop the far-room meshes no building draws any more, and the materials only they used. */
	void PruneRoomMeshes();
	/** Timer: the day fog of every far-room building, by the time of day and its distance from the camera. */
	void UpdateRoomFog(const FVector& Camera, float Night, float WorldScale);
	/** Timer: move every far-room component to its building's place plus the lift (personal.FarWindows.RoomLift) when the lift changed. */
	void UpdateRoomLift(float WorldScale);
	FVector AppliedLift = FVector::ZeroVector;
	TMap<FIntPoint, FRoomBlock> RoomBlocks;
	/** One mesh per different set of far rooms (PersonalFarRoomsMeshes::FBuilt::Key). */
	UPROPERTY()
	TMap<uint32, TObjectPtr<UStaticMesh>> RoomMeshes;
	/** For each mesh, the section each of its material slots draws. */
	TMap<uint32, TArray<int32>> RoomMeshSlots;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> RoomMaterials;
	PersonalFarRoomsMeshes::FSourcePtr RoomSource;
	TFuture<PersonalFarRoomsMeshes::FSourcePtr> PendingRoomSource;
	bool bRoomSourceRequested = false;
	/** The worker builds the rooms of one landblock at a time. */
	TFuture<TArray<PersonalFarRoomsMeshes::FBuiltPtr>> PendingRooms;
	bool bRoomsBusy = false;
	FIntPoint RoomsBusyBlock = FIntPoint(MIN_int32, MIN_int32);
	float RoomsWorldScale = 0.f;
	bool bRoomsWereOn = false;
	bool bRoomsLogged = false;
	uint32 RoomsOccupiedCell = 0;
	int32 RoomsOccupiedBuilding = INDEX_NONE;
};
