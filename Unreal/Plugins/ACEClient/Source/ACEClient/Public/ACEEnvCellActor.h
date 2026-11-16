#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Dat/ACEDatFileTypes.h"
#include "ACEEnvCellActor.generated.h"

class UProceduralMeshComponent;
class UInstancedStaticMeshComponent;
class UACEDatSubsystem;
class AACERegionSceneryActor;

/**
 * One indoor EnvCell (dungeon / building interior) — CellStruct wall/floor geometry plus any
 * Stab scenery (StaticObjects) placed inside it. Mirrors AACELandblockActor's shape: a root
 * mesh (the cell's own drawing polygons) plus child ISM / procedural meshes for scenery.
 */
UCLASS()
class ACECLIENT_API AACEEnvCellActor : public AActor
{
	GENERATED_BODY()
	friend class FACERetailInteriorStreamingTest;
	friend class FACEBridgeEntryCollisionTest;
	friend class FACERetailRuntimeRegressionTest;
	friend class FACEStaticCellVisibilityTest;
	friend class FACEDungeonFixtureLightTest;
	friend class FACEDungeonLightPerformanceTest;

public:
	AACEEnvCellActor();
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

	virtual void Tick(float DeltaSeconds) override;

	/**
	 * LandblockOrigin is the Unreal-space origin of the owning landblock (same LBx*192, LBy*192
	 * convention as AACELandblockActor::LoadLandblock). EnvCell Position and Stab StaticObject
	 * frames are both landblock-relative (retail PhysicsObj.add_obj_to_cell assigns Frame as-is).
	 * Requires FindEnvCellMesh Ready (async RequestEnvCellMesh).
	 */
	UFUNCTION(BlueprintCallable, Category = "ACE|Terrain")
	bool LoadEnvCell(int32 InEnvCellId, const FVector& LandblockOrigin, float InWorldScale = 100.f);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ACE")
	TObjectPtr<UProceduralMeshComponent> CellMesh;

	/** Invisible PhysicsPolygons mesh — retail indoor walk/block collision. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ACE")
	TObjectPtr<UProceduralMeshComponent> CellCollisionMesh;

	/** Outside-portal polys — CustomDepth stencil only (no main-pass color). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ACE")
	TObjectPtr<UProceduralMeshComponent> PortalStencilMesh;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 EnvCellId = 0;

	/** Full CellIds (landblock high word | short id) this cell lists as visible — for the
	 *  presenter to know which neighboring EnvCells to also keep loaded. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	TArray<int32> VisibleCells;

	/** EnvCellFlags.SeenOutside — interiors that can see / be seen with outdoor landblocks. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bSeenOutside = false;

	/** CellPortal OtherCellId 0xFFFF — doorway look-out (not the SeenOutside flag). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bHasOutsidePortal = false;

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	float WorldScale = 100.f;

	/** How many indoor static Setup meshes to build per tick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "16"))
	int32 StaticObjectsPerTick = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "0.5", ClampMax = "32"))
	float StaticObjectTimeBudgetMs = 2.f;

	/** Includes stabs deferred before queueing as well as in-progress builds. */
	bool HasPendingStaticObjects() const { return !DeferredStaticObjects.IsEmpty() || !PendingStaticObjects.IsEmpty(); }

	/** Retail CEnvCell::init_static_objects — create Stab objects once the cell is loaded. */
	void EnsureStaticObjectsQueued();

	/**
	 * Explicitly enable/disable EnvCell PhysicsBSP (occupied indoor floors/walls).
	 * When active, Stab/furniture objects also collide (retail CEnvCell FindObjCollisions).
	 */
	void SetEnvCellCollisionActive(bool bActive, bool bWalkableDrawMesh = false, bool bAllowAsync = false);

	/** Start PhysicsBSP cook on a worker without enabling pawn collision (doorway approach). */
	void PrefetchCollisionCookAsync();

	/**
	 * Outdoor doorway presentation: hide downward-facing draw faces (ceilings) so they
	 * do not z-fight Setup roof undersides. Floors/walls stay for vestibule peeks —
	 * sections are face-split so a shared SurfaceId cannot hide floors with ceilings.
	 */
	void SetCeilingDrawSuppressed(bool bSuppress);

	/**
	 * Indoor look-out: write CustomDepth stencil from outside-portal polys (retail PView).
	 * Only enable for shown cells while the player is indoor + CanSeeOutside.
	 */
	void SetPortalStencilActive(bool bActive);

	bool HasOutsidePortalStencil() const { return bHasOutsidePortalStencil; }

	/** Hide cell mesh + live stab actors (fountains/lamps) together. */
	void SetEnvCellHiddenInGame(bool bHide);
	/** Stabs can occupy a visible neighboring room even when their origin cell is hidden. */
	void RefreshStaticVisibility(TFunctionRef<bool(const FBox&)> IsVisible);

	/** Combine the requested sun shadows with local light occlusion for this room. */
	void SetSunCastShadow(bool bCast);
	void RefreshLightingOptions();
	/** Outdoor look-in: EnvCell PDO stays 0 (UE clamps negative). Land PDO recedes LScape. */
	void SetLookInDepthBias(float DepthBiasCm);

	/**
	 * Deprecated draw-lift hook. Always pass false — EnvCell draw stays at authored Z.
	 * Coplanar LScape loses via land PixelDepthOffset, not mesh translation.
	 */
	void SetLookInDrawLift(bool bLift);

	/** PhysicsBSP collision mesh exists — draw lift will not move pawn collision. */
	bool HasPhysicsCollisionMesh() const;

	/** True after PhysicsBSP / walkable draw has been cooked for pawn collision. */
	bool IsCollisionCooked() const { return bCollisionCooked; }
	/** Distinguishes an unloaded room from a loaded room with no floor below a prop. */
	static bool HasActiveCollision(UWorld* World, int32 CellId);

protected:
	/** Last applied collision state — RecreatePhysicsState is far too costly to repeat per tick. */
	bool bCollisionStateApplied = false;
	bool bAppliedCollisionActive = false;
	bool bAppliedWalkableDrawMesh = false;
	/** PhysicsBSP (or walkable draw) cooked — deferred until the player actually enters. */
	bool bCollisionCooked = false;

	bool EnsureCollisionCooked(bool bWalkableDrawMesh, bool bAllowAsync = false);
	void ApplyStabPawnCollision(bool bEnable);

	void ClearStaticObjects();
	void ClearDoorwayLights();
	void ConfigureSceneryLighting(class UMeshComponent* Part, UACEDatSubsystem* Dat) const;
	void QueueStaticObjects(const TArray<FACEDatStab>& StaticObjects, UACEDatSubsystem* Dat);
	bool TrySpawnOneStaticObject(UACEDatSubsystem* Dat, const FACEDatStab& Stab);
	UInstancedStaticMeshComponent* GetOrCreateStaticHism(UACEDatSubsystem* Dat, uint32 SetupId);

	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> StaticObjectMeshes;

	UPROPERTY()
	TMap<uint32, TObjectPtr<UInstancedStaticMeshComponent>> StaticObjectHisms;

	/** Indoor stabs with DefaultScript / DefaultAnimation (fountains, lamps). */
	UPROPERTY()
	TArray<TObjectPtr<AACERegionSceneryActor>> AnimatedStabs;
	UPROPERTY(Transient) TObjectPtr<AActor> StaticSceneryActor;
	USceneComponent* GetStaticSceneryRoot();
	UPROPERTY() TArray<TObjectPtr<class UPointLightComponent>> DoorwayLights;
	bool bRequestedSunCastShadow = false;
	/** Cell-owned lifetime, independent of the room's view-dependent draw visibility. */
	UPROPERTY(Transient) TObjectPtr<AActor> DoorwayLightActor;

	TArray<FACEDatStab> PendingStaticObjects;
	TArray<FACEDatStab> DeferredStaticObjects;
	bool bStaticsQueued = false;

	/** Parallel to CellMesh draw sections — true for face-split ceiling buckets only. */
	TArray<bool> SectionIsCeiling;
	bool bCeilingDrawSuppressed = false;
	bool bHasOutsidePortalStencil = false;
	bool bPortalStencilActive = false;
	FVector CellMeshAuthoredLoc = FVector::ZeroVector;
	bool bLookInDrawLift = false;
	float AppliedLookInDepthBias = -1.f;
};
