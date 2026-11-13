#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Async/Future.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "Personal/PersonalFarTerrainDetail.h"
#include "PersonalFarTerrainSubsystem.generated.h"

class UACETerrainPresenterComponent;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UTexture2D;
struct FPersonalFarTerrainMesh;

/**
 * Personal far terrain: one mesh that continues the terrain past the loaded
 * landblocks, out to the Far Terrain Radius setting. It has no collision and no shadows.
 */
UCLASS()
class UPersonalFarTerrainSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	/** Seconds between checks of the player's landblock, the loaded terrain and the setting. */
	static constexpr float UpdateSeconds = 0.1f;
	/** Pushes the shell's depth back, so loaded terrain wins while both cover the same ground. */
	static constexpr float DepthBiasCm = 100.f;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	void Update();
	/** True when the bake is loaded. Starts the load on first use. */
	bool EnsureBake(const FString& DatDirectory);
	bool EnsureMesh(class UACEDatSubsystem& Dat);
	void SetShown(bool bShown);
	/** Game thread: take a mesh a worker built. DetailCenter is set when it uses the detail texture. */
	void ApplyMesh(const TSharedRef<FPersonalFarTerrainMesh, ESPMode::ThreadSafe>& Mesh, int32 Radius, TSet<FIntPoint>&& Holes, float WorldScale,
		TOptional<FIntPoint> InDetailCenter);
	/** Start bringing the detail texture to Center when it is somewhere else. */
	void UpdateDetail(FIntPoint Center, const FString& DatDirectory);
	/** Game thread: copy what a worker made into the detail texture. */
	void ApplyDetail(FIntPoint Center, const TSharedRef<TArray<FPersonalFarTerrainDetail::FUpload>, ESPMode::ThreadSafe>& Uploads);
	bool EnsureDetailTexture();

	UPROPERTY()
	TObjectPtr<AActor> ShellActor;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> ShellMesh;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> ShellMaterial;
	UPROPERTY()
	TObjectPtr<UTexture2D> ColorTexture;

	TWeakObjectPtr<UACETerrainPresenterComponent> Presenter;
	FTimerHandle UpdateTimer;

	PersonalFarTerrainBake::FBakePtr Bake;
	TFuture<PersonalFarTerrainBake::FBakePtr> PendingBake;
	bool bBakeRequested = false;

	/** What the current mesh was built from. */
	FIntPoint BuiltCenter = FIntPoint(MIN_int32, MIN_int32);
	int32 BuiltRadius = 0;
	TSet<FIntPoint> BuiltHoles;
	/** A worker is building a mesh. */
	bool bBuilding = false;
	float AppliedEmissive = -1.f;
	bool bWireframe = false;

	/** Detail texture: the terrain textures of the landblocks around DetailCenter. */
	UPROPERTY()
	TObjectPtr<UTexture2D> DetailTexture;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> DetailMaterial;
	TSharedPtr<FPersonalFarTerrainDetail, ESPMode::ThreadSafe> Detail;
	FPersonalFarTerrainDetail::FBakeTile DetailTiles;
	/** A worker is updating the detail texture. */
	bool bDetailBusy = false;
	/** Landblock the detail texture was last made for. Unset until the first one is there. */
	TOptional<FIntPoint> DetailCenter;
	/** Detail texture center the current mesh was built for. Unset when it has no detail part. */
	TOptional<FIntPoint> BuiltDetailCenter;
};
