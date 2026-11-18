#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Async/Future.h"
#include "Personal/PersonalFarScenery.h"
#include "Personal/PersonalFarSceneryMeshes.h"
#include "Personal/PersonalFarSceneryPlacement.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "PersonalFarScenerySubsystem.generated.h"

class AActor;
class UACEDatSubsystem;
class UACETerrainPresenterComponent;
class UMaterialInterface;
class UStaticMesh;

/** A lower-detail mesh of one object type, built by the far scenery, with its materials by slot. */
USTRUCT()
struct FPersonalFarSceneryLevelMesh
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UStaticMesh> Mesh;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> Materials;
};

/**
 * Personal far scenery: draws the RegionDesc scenery, stabs and buildings of the landblocks past the ones
 * where the client draws its own. Instances, and a plain component per building: no collision, no shadows, no actors per object.
 * Object types are keyed by PersonalFarSceneryPlacement::TypeKey (id and kind) wherever a Setup id is named here.
 * Needs the far terrain (its world data, and ground past the loaded landblocks).
 * The rules (rings, object size, fog, handoff) are in PersonalFarScenery.h.
 *
 * Landblocks are placed one by one. They are drawn in units: one actor with one instance component
 * per object type. A unit is one landblock next to the client's own scenery, and a square of
 * landblocks (personal.FarScenery.Group per side) further out.
 */
UCLASS()
class UPersonalFarScenerySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
	friend class FPersonalFarSceneryDrawTest;
	friend class FACELegionsSkyTest;
public:
	/** Seconds between updates. Nothing runs per frame. */
	static constexpr float UpdateSeconds = 0.1f;
	/** Landblocks handed to a worker at once. */
	static constexpr int32 PlaceBatch = 64;
	static constexpr int32 MaxPooledActors = 64;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	/** The instances of one object type in one landblock, relative to the landblock's low corner. */
	struct FTypeInstances
	{
		TArray<FTransform> Transforms;
		float MaxScale = 0.f;
	};
	/** What a worker placed in one landblock, by Setup id. */
	struct FPlaced
	{
		FIntPoint Block = FIntPoint::ZeroValue;
		TMap<uint32, FTypeInstances> Instances;
	};

	/** What is drawn now. */
	struct FStats
	{
		int32 InSet = 0;
		int32 Units = 0;
		int32 Landblocks = 0;
		int32 Hidden = 0;
		int32 Components = 0;
		int32 Instances = 0;
		int64 Triangles = 0;
		/** Instances of object types too small to be seen from where their landblock is: not created. */
		int32 TooSmall = 0;
	};
	FStats GetStats() const;
	/** Write the frame rate and thread times of the last 10 seconds to the log (console command personal.FarScenery.LogFrameRate). */
	void LogFrameRate(const TCHAR* What) const;

private:
	/** One landblock in the set. */
	struct FBlock
	{
		/** False while a worker computes the placement. */
		bool bPlaced = false;
		TMap<uint32, FTypeInstances> Types;
	};
	/** One object type of one landblock that a unit draws. */
	struct FIncluded
	{
		FIntPoint Block;
		uint32 SetupId;
		/** Detail level it is drawn with: 0 the full mesh of the client, 1 and 2 the simpler models of the DAT. */
		uint8 Level = 0;
	};
	/** What is drawn together. Key: X, Y of the low landblock, Z landblocks per side. */
	struct FUnit
	{
		TWeakObjectPtr<AActor> Actor;
		bool bHidden = false;
		/** What bHidden should become. Changes past the client's rings are spread over updates. */
		bool bWantHidden = false;
		/** From the last plan: its landblocks, and what it should draw of them. */
		uint32 PlanSerial = 0;
		/** Detail level asked for, from the ring of its nearest landblock and the fog. */
		int32 Level = 0;
		TArray<FIntPoint> Members;
		TArray<FIncluded> Included;
		uint32 WantSignature = 0;
		/** A mesh of one of its object types is still on a worker; it is built when all are known. */
		bool bPending = false;
		int32 TooSmall = 0;
		/** What its components were built from. */
		bool bBuilt = false;
		uint32 BuiltSignature = 0;
		TArray<FIntPoint> BuiltMembers;
		int32 Components = 0;
		int32 Instances = 0;
		int64 Triangles = 0;
	};
	/** A unit that no landblock belongs to any more, kept until the units that took its landblocks are built. */
	struct FRetiring
	{
		TWeakObjectPtr<AActor> Actor;
		TArray<FIntPoint> Blocks;
	};
	/** What the rules need to know about an object type. */
	struct FTypeInfo
	{
		/** Height of the mesh at scale 1, in Unreal units. */
		float Height = 0.f;
		/** The client draws at most 3 of these per landblock, as live actors. Left out of the far scenery. */
		bool bAnimated = false;
	};

	/** Timer: checks where the player is and whether far scenery is drawn at all, then calls Stream. */
	void Update();
	/**
	 * One update for a player in landblock Center: drop, place, plan, show or hide, build and remove.
	 * FogEnd in Unreal units. NearShown: the client's own scenery of that landblock is complete and shown.
	 */
	void Stream(UACEDatSubsystem& Dat, const PersonalFarTerrainBake::FBakePtr& Bake, FIntPoint Center, float WorldScale,
		float FogEnd, TFunctionRef<bool(FIntPoint)> NearShown);
	/** Destroy every actor at once. */
	void ClearAll();
	/** True when the placement data is loaded. Starts the load on first use. */
	bool EnsureSource(const FString& DatDirectory);
	/** Ask for every unit to be hidden, and hide as many as one update allows. */
	void HideAll();
	/**
	 * Hide or show units that are not as wanted, nearest to Center first, up to a number of components per update:
	 * each change makes the renderer rebuild the unit's components. True when some are left for the next update.
	 */
	bool ApplyVisibility(FIntPoint Center);
	void SetUnitHidden(FUnit& Unit, bool bHide);
	/** Send the nearest landblocks that have no placement yet to a worker. */
	void StartPlacement(const PersonalFarTerrainBake::FBakePtr& Bake, FIntPoint Center, float WorldScale, int32 OuterRing);
	/** Game thread: take what a worker placed. */
	void ApplyPlaced(TArray<FPlaced>&& Placed);
	/** Sort the placed landblocks into units and work out what each unit should draw (size rule). */
	void PlanUnits(UACEDatSubsystem& Dat, FIntPoint Center, float WorldScale);
	/**
	 * Build the units whose content changed, nearest first, until the time is used up.
	 * True when something is left to build.
	 */
	bool BuildUnits(UACEDatSubsystem& Dat, FIntPoint Center, float WorldScale, double Start, double Budget);
	/** Create the static mesh of an object type once. False when it cannot be made. */
	bool MakeMesh(UACEDatSubsystem& Dat, uint32 SetupId, int32 Level, float WorldScale);
	/** Send the object types whose lower-detail models are not known yet to a worker. */
	void RequestLevels(const FString& DatDirectory, float WorldScale);
	void ApplyLevels(TMap<uint32, PersonalFarSceneryMeshes::FLevelsPtr>&& Built, PersonalFarSceneryMeshes::FSourcePtr UsedSource);
	void UpdateRetiring();
	const FTypeInfo& GetTypeInfo(UACEDatSubsystem& Dat, uint32 SetupId, float WorldScale);
	void AddComponent(UACEDatSubsystem& Dat, FUnit& Unit, uint32 SetupId, int32 Level, const TArray<FTransform>& Transforms, float WorldScale, float Distance);
	AActor* AcquireActor(FIntPoint Key, float WorldScale);
	/** The actor that draws a landblock now, if any. */
	AActor* ActorOf(FIntPoint Block) const;
	/** Remove the components of actors that are no longer used, until the time is used up. */
	void ReleaseActors(double Start, double Budget);
	void LogStats(double FillSeconds) const;
	/** Console commands from a file, so a script can drive the packaged client, which has no console key. */
	void RunCommandFile();

	TMap<FIntPoint, FBlock> Blocks;
	TMap<FIntVector, FUnit> Units;
	TArray<FRetiring> Retiring;
	/** Actors with components still to remove. */
	TArray<TWeakObjectPtr<AActor>> Releasing;
	/** Empty hidden actors to use again. */
	TArray<TWeakObjectPtr<AActor>> Pool;
	TMap<uint32, FTypeInfo> Types;
	/** Object type and level (Setup id x 4 + level) whose static mesh was asked for, and whether it exists. */
	TMap<uint64, bool> MeshMade;
	/** The lower-detail models of object types, once a worker has built them. */
	TMap<uint32, PersonalFarSceneryMeshes::FLevelsPtr> Levels;
	TSet<uint32> LevelsWanted;
	bool bLevelsBusy = false;
	PersonalFarSceneryMeshes::FSourcePtr MeshSource;
	UPROPERTY()
	TMap<uint64, FPersonalFarSceneryLevelMesh> LevelMeshes;

	TWeakObjectPtr<UACETerrainPresenterComponent> Presenter;
	FTimerHandle UpdateTimer;

	PersonalFarSceneryPlacement::FSourcePtr Source;
	TFuture<PersonalFarSceneryPlacement::FSourcePtr> PendingSource;
	bool bSourceRequested = false;

	/** A worker is placing landblocks. */
	bool bPlacing = false;
	float BuiltWorldScale = 0.f;
	/** The units have to be planned again: the player moved, landblocks came or went, a setting changed, or a mesh was pending. */
	bool bPlanDirty = true;
	uint32 PlanSerial = 0;
	FIntPoint LastCenter = FIntPoint(MIN_int32, MIN_int32);
	/** Hash of the settings the last plan was made with. */
	uint32 PlannedSettings = 0;
	/** The outermost ring whose near edge is inside the fog end, whatever the fog rule setting. */
	int32 FogRingNow = PersonalFarScenery::LastRing;
	/** Everything is hidden or on its way there (indoors, portal space). */
	bool bAllHidden = false;

	/** For the log line written each time all work is done. */
	bool bBusy = false;
	/** The last Stream call left nothing to do. */
	bool bComplete = false;
	double BusySince = 0.;
	double WorkSeconds = 0.;
	double LongestUpdate = 0.;

	/** Time and frame counter at the last updates: gives the frame rate over the last 10 seconds. */
	struct FFrameSample
	{
		double Seconds = 0.;
		uint64 Frame = 0;
		/** The engine's own times of the frame at that update, in ms. */
		float GameMs = 0.f;
		float RenderMs = 0.f;
		float GpuMs = 0.f;
	};
	static constexpr int32 FrameSampleCount = 100;
	FFrameSample FrameSamples[FrameSampleCount];
	int32 FrameSampleIndex = 0;
	/** The off switch (console variable or file) was set at the last update. */
	bool bSwitchedOff = false;
	FDateTime CommandFileTime;
	/** The command file's time has been looked at once: what was there at the start is not run. */
	bool bCommandFileSeen = false;
};
