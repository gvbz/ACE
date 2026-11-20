#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TimerManager.h"
#include "ACETypes.h"
#include "Dat/ACEOutdoorPortalPlan.h"
#include "ACETerrainPresenterComponent.generated.h"

class UACEClientSubsystem;
class UACEDatSubsystem;
class UACEScriptComponent;
class AACELandblockActor;
class AACEEnvCellActor;
class AACETerrainChunkActor;
class UProceduralMeshComponent;

/**
 * Spawns outdoor landblock meshes around the player, plus EnvCells for building portals /
 * indoor VisibleCells (retail-style: doorway interiors outdoors, full visible set indoors).
 * Add alongside ACEWorldPresenterComponent on your Game Mode or level actor.
 */
UCLASS(ClassGroup = (ACE), meta = (BlueprintSpawnableComponent))
class ACECLIENT_API UACETerrainPresenterComponent : public UActorComponent
{
	GENERATED_BODY()
	friend class FACESpawnSupportTest;
	friend class FACEStreamingRetryBudgetTest;
	friend class FACERetailInteriorStreamingTest;
	friend class FACEFortTethStairsTest;
	friend class FACEMansionEdgesTest;
	friend class FACEBuildingPassageTest;
	friend class FACEEntranceTransitionTest;
	friend class FACEVRInteriorNetworkTest;

	friend class FACERetailRuntimeRegressionTest;
	friend class FACERetailNetworkWeatherTest;
	friend class FACERetailWorldEntryTest;
	friend class FACEPortalPlacementTest;
	friend class FACEPortalDropTest;
	friend class FACEPortalReportedPlacementsTest;
	friend class FACELoadingTransitionTest;
	friend class FACEStreamingRetirementTest;
	friend class FACEPortalRetirementTest;
	friend class FACETerrainPortalRevealTest;
	friend class FACECameraEdgeTest;

public:
	UACETerrainPresenterComponent();
	// Visibility follows the final camera every frame; residency and physics do not.
	void UpdateCameraVisibility();
	/** Network occupants use the same PView room admission as their geometry. */
	bool IsWorldCellVisible(int32 CellId) const;
	/** Moving/large objects can overlap an admitted room beyond their spawn cell. */
	bool IsWorldObjectVisible(int32 CellId, const FBox& WorldBounds) const;
	bool bShowOutdoorEntities = true;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	float WorldScale = 100.f;

	/**
	 * Chebyshev radius of landblocks to keep loaded around the player.
	 * Retail LScape::mid_radius default is 5 (prefs 3/5/8/11/15/25) → 11×11 terrain.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "25"))
	int32 LoadRadius = 5;

	/**
	 * Unload only outside this radius (must be >= LoadRadius). Extra ring prevents pop-out
	 * and momentary voids when the player crosses a landblock boundary.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "26"))
	int32 UnloadRadius = 6;

	/**
	 * Buildings, stabs, and RegionDesc flora — retail LScape::get_block_orient size=1
	 * (side_cell_count==8) when Chebyshev distance from the viewer block is ≤ 1 (3×3).
	 * Outer LoadRadius rings are heightfield only (retail coarser poly_size 2/4/8).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "0", ClampMax = "4"))
	int32 FullDetailRadius = 1;

	/** How many landblocks to load per RetrySync tick (ignored when using chunks). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "36"))
	int32 LandblocksPerTick = 4;

	/**
	 * WorldBuilder-style multi-LB terrain chunks. 4 = 4×4 landblocks per ProcMesh
	 * (one GPU TexMerge draw when atlas is ready). Set 1 to keep per-LB actors.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "16"))
	int32 TerrainChunkSize = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	TSubclassOf<AACELandblockActor> LandblockClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	TSubclassOf<AACETerrainChunkActor> TerrainChunkClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	TSubclassOf<AACEEnvCellActor> EnvCellClass;

	/**
	 * Indoor EnvCell streaming (building / dungeon interiors + DAT static props).
	 * Indoors: PView portal BFS from viewer_cell. Look-out land only through 0xFFFF
	 * apertures in view (not a town-wide LScape unhide). Outdoors: aperture-admitted StabList.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	bool bEnableEnvCells = true;

	/** Soft cap on EnvCells per RetrySync tick (also gated by EnvCellTimeBudgetMs). Outdoor default 8. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "16"))
	int32 EnvCellsPerTick = 1;

	/**
	 * Cap EnvCell mesh build+spawn work per sync pass (WorldBuilder ProcessUploads pattern).
	 * Player's current cell always loads even if the budget is already spent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "0.5", ClampMax = "32"))
	float EnvCellTimeBudgetMs = 2.f;

	/** True once the landblocks around the player have finished their first successful sync. */
	UFUNCTION(BlueprintPure, Category = "ACE")
	bool IsInitialSyncComplete() const { return bLastSyncComplete; }

	/**
	 * True when every landblock in LoadRadius has its outdoor terrain mesh spawned.
	 * Buildings/flora may still be pending — use IsLoadRadiusBuildingsReadyForRadius.
	 */
	UFUNCTION(BlueprintPure, Category = "ACE")
	bool IsLoadRadiusTerrainReady() const { return IsLoadRadiusTerrainReadyForRadius(LoadRadius); }

	/** Same as IsLoadRadiusTerrainReady but for an explicit Chebyshev radius. */
	bool IsLoadRadiusTerrainReadyForRadius(int32 Radius) const;

	/**
	 * True when every landblock in Radius has walkable terrain + buildings.
	 * RegionDesc flora may still stream. Failed DAT holes are skipped so one missing
	 * neighbor cannot trap portal space.
	 */
	bool IsLoadRadiusBuildingsReadyForRadius(int32 Radius) const;

	/** True when every spawned landblock in the current load set has drained PendingScenery. */
	UFUNCTION(BlueprintPure, Category = "ACE")
	bool IsLoadRadiusSceneryComplete() const;

	/** Same as IsLoadRadiusSceneryComplete but for an explicit Chebyshev radius. */
	bool IsLoadRadiusSceneryCompleteForRadius(int32 Radius) const;

	/**
	 * Center landblock walkable (collision + buildings). RegionDesc flora may still drain.
	 * Used for portal reveal — outer LoadRadius scenery finishes after exit.
	 */
	UFUNCTION(BlueprintPure, Category = "ACE")
	bool IsCenterSceneryComplete() const;

	/**
	 * True when the player's current cell has walkable collision ready.
	 * Outdoor: center landblock IsLandblockReady. Indoor: current EnvCell mesh+collision.
	 */
	UFUNCTION(BlueprintPure, Category = "ACE")
	bool IsPlayerCellCollisionReady() const;
	/** Physics entry waits for this cell's terrain, room and authored supporting props. */
	bool IsObjectCellCollisionReady(uint32 CellId) const;
	/** Confirms the requested destination (not the previous cell) and its placement geometry. */
	bool IsPortalPlacementGeometryReady(uint32 DestinationCell, uint32 CandidateCell = 0) const;
	/** Current indoor cell draw + stab scenery (portal reveal / enter-world). */
	bool IsPlayerCellVisualReady() const;

	/**
	 * Indoor portal exit (legacy/narrow): current cell + 1-hop neighbors spawned.
	 * Prefer IsEnvCellSyncComplete for marketplace / full dungeon shells.
	 */
	bool IsPlayerIndoorNeighborhoodReady() const;
	/** Surface interiors retain outdoor terrain; dungeon coordinate frames do not. */
	bool NeedsExteriorTerrain(uint32 CellId) const;

	/**
	 * True when the indoor EnvCell set for the player landblock finished its first sync
	 * (all LandblockInfo.NumCells spawned or skipped). Prefer IsPlayerIndoorNeighborhoodReady
	 * for portal exit — full marketplace sync can take minutes.
	 */
	UFUNCTION(BlueprintPure, Category = "ACE")
	bool IsEnvCellSyncComplete() const { return !bEnableEnvCells || bLastEnvSyncComplete; }

	/** Clear spawned landblocks + EnvCells so portal destination reloads from player cell. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void ResetStreamingForTransition();

	/** Re-enable procedural landblock streaming after login hold (non-WC maps only). */
	void RestoreProceduralStreamingAfterLogin(int32 InLoadRadius = 5, int32 InUnloadRadius = 6);
	/** Change residency through the existing staged loader; keep mobile memory limits. */
	void ApplyLandscapeDrawRadius(int32 Radius);

	/** Retail get_block_orient full-res radius, clamped to the current LoadRadius. */
	int32 GetFullDetailRadius() const { return FMath::Clamp(FullDetailRadius, 0, FMath::Max(0, LoadRadius)); }

	/** WC Dereth: building-shell collision only — baked landscape is walkable ground. */
	void RestoreWcBakedCollisionStreaming(int32 InLoadRadius = 1, int32 InUnloadRadius = 1);

	/** Burst the inner spawn ring so portal destination fills quickly. */
	void KickLandblockLoginBurst();

	/** True when this landblock is in the sliding load/hysteresis ring around the player. */
	bool IsLandblockKept(uint32 LandblockId) const;

	/** True when a terrain actor for this landblock is actually spawned (not just queued). */
	bool IsLandblockSpawned(uint32 LandblockId) const;

	/** Current keep-ring landblock keys (CellId & 0xFFFF0000). Empty until the first sync. */
	const TSet<int32>& GetKeptLandblocks() const { return KeptLandblocks; }

protected:
	UFUNCTION()
	void HandleEnteredWorld(int32 PlayerGuid, const FACEPosition& SpawnPosition);

	UFUNCTION()
	void HandlePositionUpdate(int32 ObjectGuid, const FACEPosition& Position);

	UFUNCTION()
	void HandleLogout(EACESessionState NewState);

	void SyncAroundCell(uint32 CellId);
	void ApplyLandblockDetailLevels(int32 CenterX, int32 CenterY);
	static int32 LandblockChebyshev(int32 LandblockKey, int32 CenterX, int32 CenterY);
	void ClearLandblocks();
	void RetrySyncIfNeeded();
	void EnsureTerrainStreamingSubscriptions();
	bool ShouldHoldStagedLoadRadius() const;

	/** (CellId & 0xFFFF) >= 0x100 — dungeons / building interiors, per EnvCell.cs remarks. */
	static bool IsIndoorCell(uint32 CellId) { return (CellId & 0xFFFFu) >= 0x0100u; }

	/** Indoor EnvCell *draw* occupancy (SmartBox viewer_cell). Camera owns this. */
	uint32 ResolveDrawOccupancyCellId(UACEDatSubsystem* Dat) const;
	FACEPosition GetPresentationPosition() const;
	/** Indoor pawn collision / LScape occupancy (physics object CellId). */
	uint32 ResolveCollideOccupancyCellId(UACEDatSubsystem* Dat) const;
	/** Path-walk a 0.3 AC sphere from the pawn toward the camera. No occupancy debounce. */
	void RefreshViewerCellId(UACEDatSubsystem* Dat);
	/**
	 * Retail occupancy is the physics object's one CellId (find_cell_list / server).
	 * Follow the accepted movement cell immediately; camera visibility is independent.
	 */
	void CommitOccupancyCellId(uint32 Candidate);

	/** Streams portal / VisibleCells EnvCells and applies outdoor↔indoor actor visibility. */
	void SyncEnvCells();
	void ClearEnvCells();
	void CollectNeededEnvCells(TArray<int32>& OutOrdered, TSet<int32>& OutNeeded);
	void UpdateBuildingVisibility();
	/** Outdoor doorway collision only — does not hide/show interiors or restomp landblocks. */
	void UpdateOutdoorEnvCollision();
	/** Outdoor look-in draw: nearby doorway PVS + admitted peeks (not viewer_cell residency). */
	void UpdateOutdoorEnvCellDraw();
	/** Runtime CSM: nearby buildings/scenery/characters. Land never casts. */
	void DisableAllRuntimeShadowCasters();
	void RefreshOutdoorShadowCasters(const FVector& CameraWorld);
	void RefreshEntityShadowCasters(const FVector& CameraWorld);
	void UpdateSkyWeatherState();
	void TickDegradeController(float DeltaTime);
	void ApplyOutdoorDoorwayClipsFromApertures();
	void SyncOutdoorPortalDepthApertures(const TArray<ACEOutdoorPortalPlan::FAdmittedAperture>& Apertures);
	void ClearOutdoorPortalDepthApertures();
	void EnsureAmbientScripts();
	void TickAmbientSounds(float DeltaTime);
	void RebuildAmbientSchedule(uint32 CellId);

	UPROPERTY()
	TObjectPtr<UACEClientSubsystem> Client;

	UPROPERTY()
	TObjectPtr<UACEScriptComponent> AmbientScripts;

	struct FAmbientSlot
	{
		uint32 SoundTableId = 0;
		uint32 SoundType = 0;
		float Volume = 1.f;
		float BaseChance = 1.f;
		float MinRate = 1.f;
		float MaxRate = 10.f;
		bool bContinuous = false;
		/** Continuous beds start once as a loop; intermittent uses NextPlaySeconds. */
		bool bContinuousPlaying = false;
		double NextPlaySeconds = 0.0;
	};
	TArray<FAmbientSlot> AmbientSlots;
	uint32 AmbientStbKey = 0;
	uint32 AmbientListenerCell = 0;
	uint32 AmbientOutdoorCell = 0;
	FVector AmbientSourcePosition = FVector::ZeroVector;
	float AmbientReachCountdown = 0.f;
	float AmbientTargetGain = 0.f;
	float AmbientCurrentGain = 0.f;
	FRandomStream AmbientRandom;

	UPROPERTY()
	TMap<int32, TObjectPtr<AACELandblockActor>> Spawned;

	UPROPERTY()
	TMap<int32, TObjectPtr<AACETerrainChunkActor>> SpawnedChunks;

	/** Landblocks that failed to build (missing DAT / empty) — skip so one bad neighbor can't stall the 3x3. */
	TSet<int32> FailedLandblocks;

	UPROPERTY()
	TMap<int32, TObjectPtr<AACEEnvCellActor>> SpawnedEnvCells;

	/** Depth-only doorway apertures for outdoor look-in (portal depth before EnvCell/exterior). */
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> OutdoorPortalDepthMesh;

	/** Fingerprint of last SyncOutdoorPortalDepthApertures input — skip PMC rebuild when unchanged. */
	uint32 OutdoorPortalDepthFingerprint = 0;

	/** EnvCells that failed to build (empty/missing) — skip so we don't retry forever. */
	TSet<int32> FailedEnvCells;

	/** Last specific CellId / landblock set SyncEnvCells completed for. */
	uint32 LastEnvSyncKey = 0;
	uint32 LastEnvDirtyKey = 0;
	int32 LastEnvLbActorCount = -1;
	uint64 LastEnvSyncFrame = MAX_uint64;
	FVector LastOutdoorCollidePlayerUe = FVector::ZeroVector;
	bool bHaveOutdoorCollidePlayerUe = false;
	/** Outdoor doorway EnvCells kept after the camera looks away (no spawn/despawn pop). */
	TMap<int32, double> OutdoorPeekKeepUntil;
	double LastOutdoorEnvSyncSec = 0.0;
	/** Last outdoor PView apertures — depth-only doorway holes (retail DrawPortalPoly). */
	TArray<ACEOutdoorPortalPlan::FAdmittedAperture> LastOutdoorApertures;
	TArray<uint32> OutdoorAdmitKeys;
	uint32 OutdoorAdmitLandblock=0;
	int32 OutdoorAdmitRadius=-1;
	uint32 LastDoorwayClipFingerprint = 0;
	int32 LastDoorwayClipLandblock = 0;
	uint32 EnvStreamCellId = 0;
	double IndoorStreamHoldUntil = 0.0;
	/** Indoor EnvCells that have been shown this occupancy — do not hide stairs/halls on PVS hop. */
	TSet<int32> HeldIndoorVisible;
	/** Occupied + 1–2 portal hops kept colliding across a stair/landing CellId flip. */
	TSet<int32> HeldIndoorCollide;
	double IndoorCollideKeepUntil = 0.0;
	uint32 LastIndoorCollideCell = 0;
	bool bLastEnvSyncComplete = false;
	/** Last Tick IsInPortalSpace — leaving portal must unhide land even if occupancy is unchanged. */
	bool bWasInPortalSpace = false;

	float DegradeMul = 0.f;
	bool bMapDegradesDisabled = false;
	float DegradeFpsEma = 0.f;
	float DegradeApplyAccum = 0.f;
	TArray<float> DegradeHistory;

	int32 TrackedPlayerGuid = 0;
	int32 LastCenterLB = -1;
	/** Candidate landblock while we wait to commit an adjacent step (boundary flicker). */
	int32 PendingStreamCenterKey = -1;
	float StreamCenterHoldAccum = 0.f;

	/** Landblocks currently in the load + unload-hysteresis ring (sliding window). */
	TSet<int32> KeptLandblocks;

	/**
	 * Grows from 0 (current landblock only) to LoadRadius after login / long teleport.
	 * Adjacent walking snaps this to LoadRadius so the active set is always the
	 * Chebyshev ring around the landblock the player is standing on.
	 */
	int32 StagedLoadRadius = 0;
	/** Seconds since the last StagedLoadRadius increment — spreads Ayan scenery/CSM. */
	float StagedExpandAccum = 0.f;
	/** Last time Refresh*ShadowCasters ran. */
	float ShadowRefreshAccum = 0.f;
	/** RetrySync drains the full load ring quickly after login (0.1s timer ticks). */
	int32 LandblockLoginBurstTicks = 0;
	/** Ignore Chebyshev jumps that would restage from 0 during portal / login burst. */
	bool bHoldStagedLoadRadius = false;

	/** True only once every landblock actor in the last SyncAroundCell's radius loaded
	 *  successfully — lets SyncAroundCell's "nothing changed" shortcut avoid falsely skipping
	 *  retries when some (but not all) landblocks in the area failed to build (e.g. DAT was
	 *  still being opened / AV-scanned). */
	bool bLastSyncComplete = false;
    FIntPoint LastTerrainProgressCenter = FIntPoint(INDEX_NONE, INDEX_NONE);
    int32 LastTerrainProgressRemaining = INDEX_NONE;
    double LastTerrainProgressAt = 0.0;
    double LastTerrainPendingLogAt = 0.0;

	/** Baked WC map — hide procedural LScape, skip duplicate flora, keep building shells. */
	bool bWcBakedTerrainMode = false;

	/** Last cell we know the player was in — used by the periodic retry timer below so terrain
	 *  keeps attempting to load even if the DAT open was still failing when position/enter-world
	 *  events first arrived (no further server position updates may ever come in to retrigger
	 *  a sync otherwise). */
	uint32 LastKnownCellId = 0;
	bool bHasKnownCell = false;
	/** Last outdoor landcell — dungeon interiors live on other LBs; keep this ring loaded. */
	uint32 LastOutdoorCellId = 0;
	uint64 LastPortalTerrainSyncFrame = MAX_uint64;
	/** Full actor visibility must be restored once the portal tunnel releases. */
	bool bWorldHiddenForPortal = false;
	/** SmartBox viewer_cell — draw occupancy. 0 until RefreshViewerCellId runs. */
	uint32 ViewerCellId = 0;

	/**
	 * Indoor/outdoor residency (LScape hide, collision) follows the player's CellId — same
	 * as retail body residency. Outdoor EnvCell *draw* admission uses the camera frustum
	 * (building aperture → copied PView). Camera-based indoor/outdoor hysteresis was wrong:
	 * third-person kept LScape until the lens entered, so grass stayed under shop floors.
	 */

	FTimerHandle RetryTimerHandle;
	float ShadowLodAccum = 0.f;
};
