#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "GameFramework/Actor.h"
#include "Dat/ACEDatFileTypes.h"
#include "ACESkyDomeActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInstanceDynamic;
class UACEDatSubsystem;
class UACEClientSubsystem;
class UACEScriptComponent;
class AExponentialHeightFog;
class ADirectionalLight;
class ASkyLight;
struct FACELegionsSkyRenderer;
namespace ACELegionsSky { struct FBackdrop; }

/**
 * Retail Asheron's Call sky — RegionDesc 0x13000000 SkyDesc → GameSky / GetSky.
 *
 * Properties (retail SkyObject):
 *   0x1  After-pass (drawn after world when weather is on)
 *   0x2  Skip under fog override (reserved)
 *   0x4  Weather — outdoor-only; camera-follow; Z forced to -120 AC unless 0x8
 *   0x8  With 0x4: do not force weather Z
 *
 * Per frame: CalcPresentDayGroup → GetSky-equivalent CelestialPosition state →
 * CalcFrame(heading, rotation) → before/after draw order via TranslucentSortPriority.
 */
UCLASS()
class ACECLIENT_API AACESkyDomeActor : public AActor
{
	GENERATED_BODY()
	friend class FACERetailWeatherTest;
	friend class FACERetailRuntimeRegressionTest;
	friend class FACERetailNetworkWeatherTest;
	friend class FACEMobileShadowTest;
	friend class FACELowEndVideoSettingsTest;
	friend class FACELegionsSkyTest;

public:
	AACESkyDomeActor();

	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	float WorldScale = 100.f;

	/**
	 * Extra scale for before-pass shells (dome/stars/clouds) so faces sit beyond typical
	 * scenery depth. Weather stays at WorldScale. Retail used DEPTHTEST_ALWAYS + draw order;
	 * Unreal needs far depth + depth test so the world occludes the sky cube.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "0.5", ClampMax = "8"))
	float SkyDistanceScale = 2.5f;

	/** Drive retail linear world fog; Unreal ExponentialHeightFog stays suppressed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	bool bDriveWorldFog = true;

	/**
	 * Cap fog end at FogClamp * 192 AC (streaming-edge debug). 0 = DAT MaxWorldFog only (retail).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "0", ClampMax = "16"))
	float FogClampLandblocks = 0.f;

	/** Drive directional + sky/ambient lights from SkyTimeOfDay (retail GetLighting). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	bool bDriveWorldLighting = true;

	/**
	 * PlayerModule::PersistentAtDay — pin terrain/object lighting to noon (0.5) while
	 * sky objects keep moving.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE")
	bool bPersistentAtDay = false;

	/**
	 * DAT DirBright is ~0.25–0.80 (retail light-vector magnitude). Unreal outdoor lights
	 * need a much larger Intensity — without this scale the world stays night-dark all day.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "1", ClampMax = "40"))
	float DirLightIntensityScale = 4.f;

	/** DAT AmbBright is ~0.35–0.45; scale for USkyLightComponent Intensity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ACE", meta = (ClampMin = "0.5", ClampMax = "20"))
	float AmbLightIntensityScale = 2.5f;

	/** Outdoor weather gate (TerrainPresenter / portal). After-pass + Properties&4 respect this. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SetWeatherEnabled(bool bEnabled);

	UFUNCTION(BlueprintPure, Category = "ACE")
	bool IsWeatherEnabled() const { return bWeatherEnabled; }

	/** Drop PES/meshes and rebuild from current PortalYearTicks (portal / landblock change). */
	void InvalidateAndRebuild();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** Retail CelestialPosition + Unreal draw resources for one DayGroup.SkyObjects entry. */
	struct FSkySlot
	{
		FACEDatSkyObject Def;
		int32 ObjectIndex = 0;

		/** Resolved gfx this frame (DefaultGFX or SkyObjReplace.GFXObjId). */
		uint32 ActiveGfxId = 0;
		uint32 PesScriptId = 0;
		bool bPesPlaying = false;

		UProceduralMeshComponent* Mesh = nullptr;
		TArray<UMaterialInstanceDynamic*> Mids;
		/** Per-section authored luminosity, diffuse, opacity. */
		TArray<FVector3f> SurfaceLighting;
		FVector2D UvOffset = FVector2D::ZeroVector;

		bool bAfterPass = false;
		bool bWeather = false;
		bool bForceWeatherZ = false;
		/** Local bearing of the AC1 sun or moon card. */
		FVector LocalBodyDirection = FVector::ZeroVector;
		FVector LocalCloudTravel = FVector::ZeroVector;
	};

	/** GetSky-resolved per-object state for one frame. */
	struct FCelestialState
	{
		uint32 GfxId = 0;
		float HeadingDeg = 0.f;
		float RotationDeg = 0.f;
		float Transparent = -1.f;
		float Luminosity = 0.f;
		float MaxBright = 0.f;
		bool bVisible = false;
	};

	bool TryBuild();
	void ClearSky(bool KeepImported = false);
	void RebuildSlotMesh(FSkySlot& Slot, uint32 GfxId);
	void DestroySlotMesh(FSkySlot& Slot);

	void UpdateSky(float DayFraction, float DeltaSeconds, bool bUseImported = false);
	void ResolveCelestialState(float DayFraction, int32 IdxA, int32 IdxB, float Alpha,
		int32 ObjIndex, const FACEDatSkyObject& Def, FCelestialState& Out) const;
	void ApplySlotFrame(FSkySlot& Slot, const FCelestialState& State);
	void UpdateFog(const FACEDatSkyTimeOfDay& A, const FACEDatSkyTimeOfDay& B, float Alpha);
	void UpdateWorldLighting(const FACEDatSkyTimeOfDay& A, const FACEDatSkyTimeOfDay& B, float Alpha, float DayFraction);
	void SyncSlotPes(FSkySlot& Slot, bool bVisible);
	static int32 SlotDrawOrder(const FSkySlot& Slot);

	int32 PickDayGroup(const FACEDatRegionSky& Sky, double Ticks) const;
	static FLinearColor ArgbToLinear(uint32 Argb);
	static bool BracketTimeOfDay(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction,
		int32& OutIdxA, int32& OutIdxB, float& OutAlpha);

	/** Bump when sky mesh authoring / retail policy changes so PIE rebuilds. */
	static constexpr int32 SkyMeshFormatVersion = 56;

	UPROPERTY(Transient)
	TObjectPtr<UACEDatSubsystem> DatSubsystem;

	UPROPERTY(Transient)
	TObjectPtr<UACEClientSubsystem> ClientSubsystem;

	UPROPERTY(Transient)
	TObjectPtr<UACEScriptComponent> WeatherScripts;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> SlotMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SlotMids;

	UPROPERTY(Transient)
	TObjectPtr<AExponentialHeightFog> FogActor;

	UPROPERTY(Transient)
	TObjectPtr<ADirectionalLight> SunLightActor;

	UPROPERTY(Transient)
	TObjectPtr<ASkyLight> AmbientSkyLightActor;

	/** Inward translucent color cube (includes floor) so missing DAT-cube faces are not black. */
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> ColorFillMesh;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ColorFillMid;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> HorizonMesh;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> HorizonMid;

	void SnapToView();
	void EnsureColorFillMesh();
	void DestroyColorFillMesh();
	void UpdateColorFill(const FLinearColor& Color);
	FLinearColor LastFillColor = FLinearColor::Transparent;
	void ResetImportedSky();
	bool UpdateImportedSky(float DayFraction, bool bEnabled);
	float ImportedPaletteTime(float DayFraction) const;
	// Night end, daylight start, daylight end, night start from the AC1 star field.
	FVector4f Twilight = FVector4f(.16f,.27f,.84f,.96f);
	float LastImportedTime = -1.f;
	double LastImportedUpdate = -1.;
	bool bImportedClouds = false;
	TSharedPtr<FACELegionsSkyRenderer> ImportedSky;
	TFuture<TSharedPtr<ACELegionsSky::FBackdrop>> PendingSky;
	int32 ImportedRevision = INDEX_NONE;
	bool bImportedVisible = false;

	TArray<FSkySlot> Slots;
	FACEDatSkyDayGroup ActiveGroup;
	int32 BuiltDayNumber = -1;
	int32 BuiltSkyMeshFormat = 0;
	bool bBuilt = false;
	bool bWeatherEnabled = true;
	float RetryAccumulator = 0.f;
	float FogUpdateAccumulator = 0.f;
	float LastFogDayFraction = -1.f;
	int32 AppliedSkyMaterialGeneration = -1;
	/** Shadow cascade/distance applied once — rewriting them every lighting tick rebuilds maps. */
	bool bDirShadowSettingsApplied = false;
	int32 AppliedShadowSettingsRev = -1;
	int32 AppliedShadowQuality = -1;
	/** -1 unset; 0 outdoor (dir shadows on); 1 indoor (dir shadows off). */
	int8 LastDirShadowIndoor = -1;
	bool bHaveAppliedSunRotation = false;
	FRotator AppliedSunRotation = FRotator::ZeroRotator;
	float AppliedDirIntensity = -1.f;
	FLinearColor AppliedDirColor = FLinearColor::Transparent;
	float AppliedSkyIntensity = -1.f;
	FLinearColor AppliedSkyColor = FLinearColor::Transparent;
};
