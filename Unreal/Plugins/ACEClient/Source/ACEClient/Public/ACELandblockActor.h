#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ACELandblockActor.generated.h"

class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class UStaticMeshComponent;
class UPrimitiveComponent;
class UACEDatSubsystem;
class AACERegionSceneryActor;

struct FACESceneryShadowCand
{
	UProceduralMeshComponent* Proc = nullptr;
	AACERegionSceneryActor* Animated = nullptr;
	float DistCm = 0.f;
	float SphereRadius = 0.f;
	/** 0 = building shell, 1 = flora/stab PMC, 2 = live animated scenery. */
	uint8 Kind = 1;
};

/** One outdoor doorway slab used to clip Setup shell pixels (retail portal depth punch). */
struct FACEBuildingDoorwayClip
{
	uint32 DestEnvCellId = 0;
	FVector Center = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	float HalfWidth = 0.f;
	float HalfHeight = 0.f;
	float Thickness = 80.f;
};

/** One outdoor landblock heightfield + DAT scenery (stabs/buildings/RegionDesc) from client_cell_1.dat. */
UCLASS()
class ACECLIENT_API AACELandblockActor : public AActor
{
	GENERATED_BODY()
	friend class FACERetailInteriorStreamingTest;
	friend class FACEEntranceTransitionTest;
	friend class FACECameraEdgeTest;
	friend class FACESpawnSupportTest;
	friend class FACERetailRuntimeRegressionTest;

public:
	AACELandblockActor();
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintCallable, Category = "ACE|Terrain")
	bool LoadLandblock(int32 LandblockId, float InWorldScale = 100.f, bool bFullResDetail = true, int32 PolySize = 1, int32 TransDir = 0);

	/**
	 * Rebuild outdoor heightfield (re-punches basement lids after EnvCell floors arrive).
	 * No-op when this actor is scenery-only under a terrain chunk.
	 */
	bool ReapplyOutdoorTerrain();

	/** Scenery + placement only — terrain mesh owned by AACETerrainChunkActor. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Terrain")
	bool LoadLandblockSceneryOnly(int32 InLandblockId, float InWorldScale = 100.f, bool bFullResDetail = true);

	/**
	 * Retail CLandBlock::init_buildings / init_static_objs: scenery only when
	 * side_cell_count==8 (LScape::get_block_orient Chebyshev ≤ 1). Terrain mesh stays.
	 */
	void SetFullResDetail(bool bFull);

	/** Retail CLandBlockStruct::generate poly_size / trans_dir. No-op when already applied. */
	void SetTerrainLod(int32 PolySize, int32 TransDir);

	/** True once outdoor heightfield mesh exists (buildings/flora may still stream). */
	bool IsOutdoorTerrainMeshReady() const;

	/** True once terrain collision exists and pending building meshes are drained. */
	UFUNCTION(BlueprintPure, Category = "ACE|Terrain")
	bool IsLandblockReady() const;
	/** Includes authored static props (tables/pedestals), but not decorative RegionDesc flora. */
	bool IsPlacementCollisionReady() const;
	void CollectSceneryShadowCandidates(const FVector& CameraWorld, float MaxDistCm, TArray<FACESceneryShadowCand>& Out) const;
	void ApplySceneryShadowWinners(
		const TArray<UProceduralMeshComponent*>& WinningProcs,
		const TArray<AACERegionSceneryActor*>& WinningAnimated,
		const FVector& CameraWorld,
		float MaxDistCm,
		float StickyDistCm);
	/** Runtime CSM disabled — ensure scenery never casts (spawn + LOD paths). */
	void ForceDisableShadowCasting();

	UFUNCTION(BlueprintPure, Category = "ACE|Terrain")
	bool IsSceneryComplete() const { return PendingScenery.Num() == 0 && !bRegionDescCollectPending; }
	bool IsNearSceneryShown() const { return bFullResDetail && bSceneryQueued && IsSceneryComplete(); } // personal: far-scenery

	/**
	 * Toggle collision on the outdoor heightfield only (scenery/buildings stay collidable).
	 * Indoor EnvCells / basements / dungeons sit below grade — full land collision would
	 * block entry. Draw is toggled separately via SetOutdoorTerrainHiddenInGame (retail
	 * skips LScape while EnvCell-resident).
	 */
	UFUNCTION(BlueprintCallable, Category = "ACE|Terrain")
	void SetOutdoorTerrainCollisionEnabled(bool bEnabled);
	/** Hide heightfield draw while keeping scenery (doorway shells) when deep indoors. */
	void SetOutdoorTerrainHiddenInGame(bool bHideTerrain);
	/** Re-apply stored draw/collision after async cook or mesh rebuild. */
	void ApplyDesiredOutdoorTerrainState();
	/** Swap land section MIDs between opaque outdoor and CustomStencil look-out. */
	void SetLandPortalLookOut(bool bLookOut);
	/** Push coplanar LScape behind EnvCell floors / Setup stairs via LandDepthBias PDO. */
	void SetLandEnvCellFloorPriority(bool bEnable, UACEDatSubsystem* Dat = nullptr, bool bIndoorLookOut = false);

	/**
	 * Indoor presentation: hide RegionDesc flora/fauna and outdoor stabs when bSuppress.
	 * HideBuildingInfoIndices used to hide the occupied Setup; retail still draws it
	 * (PORT omitted). Always pass empty so look-out keeps the facade around the door.
	 */
	void SetIndoorScenerySuppressed(bool bSuppress, const TArray<int32>& HideBuildingInfoIndices = TArray<int32>());

	/**
	 * Outdoor: when true, building shells ignore ECC_Pawn (EnvCell doorway + door weenies
	 * own entry). Walls still block Camera. When false, restore pawn-blocking shells.
	 * IgnoreBuildingIndices: those Setup shells ignore pawn while others still block
	 * (street door of one shop must not open every wall on the landblock).
	 */
	void SetBuildingShellsBlockPawn(bool bBlockPawn);
	void SetBuildingShellsBlockPawn(bool bBlockPawn, const TArray<int32>& IgnoreBuildingIndices);

	/** Retail Render::CalcDegLevel analogue — HISM end cull in Unreal cm (flora/stabs). */
	void ApplyDegradeCull(float EndCullCm);

	/**
	 * Clip Setup shell pixels in admitted doorway slabs so EnvCell interiors show through
	 * without hiding the whole building. Empty list restores a solid shell.
	 */
	void ApplyOutdoorDoorwayClips(const TArray<FACEBuildingDoorwayClip>& Clips);

	int32 GetDoorwayClipMeshCount() const { return BuildingShells.Num() + BuildingMeshIndices.Num(); }
	/** Refine a building PhysicsBSP hit against its drawn shell (windows are not walls). */
	bool RefineBuildingSelectionHit(const UPrimitiveComponent* HitComponent, const FVector& Start,
		const FVector& End, double& ClosestDistance) const;

	/** Push current DAT linear fog onto per-building MIDs (door-clip children). */
	void ApplyWorldDistanceFog(UACEDatSubsystem* Dat);

	/** WC baked map: skip duplicate RegionDesc/stab flora (baked tile already draws trees). */
	void SetSkipNonBuildingScenery(bool bSkip) { bSkipNonBuildingScenery = bSkip; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ACE")
	TObjectPtr<UProceduralMeshComponent> TerrainMesh;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 LandblockId = 0;

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	float WorldScale = 100.f;

	/** Soft cap on scenery meshes / HISM instances per tick (also gated by SceneryTimeBudgetMs). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "64"))
	int32 SceneryPerTick = 4;

	/**
	 * Per-landblock scenery cook budget. A global frame budget also caps total scenery
	 * across all landblocks (see Tick) so 11×11 rings can't spend 14ms × N per frame.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "0.5", ClampMax = "32"))
	float SceneryTimeBudgetMs = 3.f;

protected:
	friend class FACEVRSceneryCollisionTest;
	friend class FACESceneryParticleLightingTest;
	struct FPendingScenery
	{
		uint32 ModelId = 0;
		FVector3f Origin = FVector3f::ZeroVector;
		FQuat4f Orientation = FQuat4f::Identity;
		float Scale = 1.f;
		bool bBuilding = false;
		bool bRegionDesc = false;
		/** FACEDatLandblockInfo::Buildings index when bBuilding; else INDEX_NONE. */
		int32 LandblockBuildingIndex = INDEX_NONE;
	};

	void ClearScenery();
	void QueueScenery(UACEDatSubsystem* Dat);
	/** Append RegionDesc placements; returns false if land heights not ready yet. */
	bool TryQueueRegionDesc(UACEDatSubsystem* Dat);
	/** Returns false if Setup mesh still pending (item stays at front of queue). */
	bool TrySpawnOneScenery(UACEDatSubsystem* Dat, const FPendingScenery& Item);
	UInstancedStaticMeshComponent* GetOrCreateSceneryHism(UACEDatSubsystem* Dat, uint32 SetupId, bool bEnableCollision, int32 PlacementId = 0);

	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> SceneryMeshes;

	/** Shared-mesh building shells (one UStaticMesh per Setup DID, per-instance component). */
	struct FBuildingShell
	{
		int32 InfoIndex = INDEX_NONE;
		uint32 ModelId = 0;
		int32 PlacementId = 0;
		TObjectPtr<UStaticMeshComponent> Mesh;
		/** PhysicsBSP only — visual UStaticMesh omits it (section-slot mismatch). */
		TObjectPtr<UStaticMeshComponent> CollisionMesh;
	};
	TArray<FBuildingShell> BuildingShells;

	/** Indices into SceneryMeshes that are building shells (PMC fallback). */
	TArray<int32> BuildingMeshIndices;
	/** Parallel to BuildingMeshIndices: FACEDatLandblockInfo::Buildings index. */
	TArray<int32> BuildingInfoIndices;

	bool bIndoorScenerySuppressed = false;
	bool bSkipNonBuildingScenery = false;
	bool bFullResDetail = true;
	bool bSceneryQueued = false;
	TMap<TWeakObjectPtr<UPrimitiveComponent>, ECollisionEnabled::Type> SuspendedSceneryCollision;
	int32 AppliedPolySize = 1;
	int32 AppliedTransDir = 0;
	bool bIndoorSceneryStateApplied = false;
	int32 AppliedIndoorSceneryMeshCount = 0;
	TArray<int32> IndoorHideBuildingInfoIndices;
	bool bWantTerrainCollision = true;
	bool bWantTerrainHidden = false;
	bool bBuildingShellPawnApplied = false;
	bool bLastBuildingShellBlockPawn = true;
	int32 AppliedBuildingShellCount = -1;
	uint32 AppliedBuildingShellIgnoreHash = 0;
	int32 AppliedIndoorBuildingShellCount = -1;
	float AppliedDegradeEndCullCm = 0.f;
	float AppliedLandDepthBias = -1.f;
	int32 AppliedLandDepthBiasSections = -1;

	static uint64 SceneryMeshKey(uint32 SetupId, int32 PlacementId, bool bCollision)
	{
		return (uint64(SetupId) << 32) | (uint32(PlacementId) & 0x7FFFFFFFu) | (bCollision ? 0x80000000u : 0u);
	}
	UPROPERTY()
	TMap<uint64, TObjectPtr<UInstancedStaticMeshComponent>> SceneryHisms;

	UPROPERTY()
	TArray<TObjectPtr<AACERegionSceneryActor>> AnimatedScenery;

	/** Live SetupIds that already have an AACERegionSceneryActor on this landblock. */
	TSet<uint32> ClaimedLiveSceneryModels;
	/** RegionDesc DefaultAnimation live actors (birds). Extra copies are dropped, not instanced. */
	int32 LiveRegionAnimCount = 0;
	uint32 LastDoorwayClipFingerprint = 0;

	TArray<FPendingScenery> PendingScenery;

	/** CollectRegionScenery failed (heights still baking) — retry on Tick. */
	bool bRegionDescCollectPending = false;
};
