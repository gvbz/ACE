#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ACETypes.h"
#include "Dat/ACEDatFileTypes.h"
#include "ACEScriptComponent.generated.h"

class UACECharacterAppearanceComponent;
class UACEDatSubsystem;
class UACEParticleBatchComponent;
class UProceduralMeshComponent;
class UMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class USceneComponent;
class UAudioComponent;
class USoundWaveProcedural;

USTRUCT()
struct FActiveAceSound
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UAudioComponent> Component;

	UPROPERTY()
	TObjectPtr<USoundWaveProcedural> Wave;

	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
	bool bLooping = false;
	bool bAmbient = false;
	float BaseVolume = 1.f;
	/** PCM16 bytes to re-queue while looping (USoundWaveProcedural does not auto-refill). */
	TArray<uint8> LoopPcm;
};

/**
 * Runtime for client_portal PhysicsScripts and their legacy particle emitters.
 * The integrator deliberately bounds corrupt DAT values and uses stable approximations
 * for undocumented particle modes while preserving authored frames and timing.
 */
UCLASS(ClassGroup = (ACE), meta = (BlueprintSpawnableComponent))
class ACECLIENT_API UACEScriptComponent : public UActorComponent
{
	friend class FACEPortalLifetimeTest;
	friend class FACEInteractionEffectsTest;
	GENERATED_BODY()
	friend class FACERetailParticleLightingTest;
	friend class FACERetailParticleTimingTest;
	friend class FACERetailScreenTest;
	friend class FACEParticleDistanceTest;
	friend class FACEParticleFrameReuseTest;
	friend class FACEParticlePresentationTest;
	friend class FACESceneryParticleLightingTest;
	friend class FACESnowTuskerPerformanceTest;
	friend class FACECreatureSpawnCacheTest;
	friend class FACECrowdWeaponLightingTest;
	friend class FACEDualWieldLightingTest;
	friend class FACEEntranceParticleTest;
	friend class FACERetailWeatherTest;
	friend class FACEVRRenderReplicationTest;
	friend class FACEPluginCosmeticEffectsTest;

public:
	UACEScriptComponent();

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	void InitializeFromObject(const FACEWorldObject& Object, float InWorldScale);

	/** Sky / weather PES parents — camera-rooted, no weenie Setup metadata. */
	void InitializeForEnvironment(float InWorldScale);
	/** Sky particle children inherit their celestial object's before/after draw order. */
	void SetEnvironmentScriptDrawOrder(uint32 ScriptId, int32 SortPriority);
	void SetEnvironmentSkyTime(double Seconds) { EnvironmentSkyTime = Seconds; }
	void SetEnvironmentLighting(const FLinearColor& Ambient, const FLinearColor& SunColor, const FLinearColor& Direction);

	/**
	 * Call after Setup part meshes / BindTransforms exist. Default PhysicsScripts are deferred
	 * until this runs so portal emitters parent to part placement frames (not the object root).
	 */
	void NotifyAppearanceReady(bool bIsDoor = false);
	/** Retire material backups before replacing geometry/textures on reused parts. */
	void NotifyAppearanceChanging();

	/** CPhysicsObj::set_hidden starts Hidden/UnHide from the object's PE table. */
	void SetPhysicsHidden(bool bHidden);

	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void PlayScriptId(int32 PhysicsScriptId, float Intensity = 1.f);

	/** Stop one PhysicsScript DID and emitters it spawned (sky rain time windows). */
	void StopScriptId(uint32 PhysicsScriptId);

	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void PlayEffect(int32 ScriptType, float Intensity = 1.f);
    /** A blood burst owns its lifetime and snapshots the struck body's frames. */
    UACEScriptComponent* PlayImpactEffect(int32 ScriptType, float Intensity = 1.f);

	/** False until NotifyAppearanceReady — PlayEffect may be queued until then. */
	bool IsEffectReady() const { return bAppearanceReady; }
	/** True after StartDefaultScripts latched (or env weather skip). */
	bool HasStartedDefaultScripts() const { return bDefaultStarted; }
	uint32 GetSetupId() const { return SetupId; }

	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void PlaySound(int32 SoundType, float Volume = 1.f);
	/** Network / explicit world-origin SFX (mob combat sounds). */
	void PlaySoundAtWorldLocation(int32 SoundType, float Volume, const FVector& WorldLocation);

	/** Play SoundType through an explicit SoundTable DID (RegionDesc ambient STBId). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void PlaySoundFromTable(int32 SoundTableId, int32 SoundType, float Volume = 1.f);

	/**
	 * Retail PlaySoundFromCenter — UI / portal-space cues (SoundTable 0x2000004B).
	 * Always 2D so tunnel camera distance never mutes the whoosh.
	 */
	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void PlayCenteredSoundFromTable(int32 SoundTableId, int32 SoundType, float Volume = 1.f);

	/**
	 * Outdoor ambient STB cue at a world position (distance + stereo pan).
	 * Continuous cues should pass bLoop=true so they are not re-triggered every rate tick.
	 */
	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void PlayAmbientSoundFromTable(int32 SoundTableId, int32 SoundType, float Volume, const FVector& WorldLocation, bool bLoop = false);

	/** Additional gain for this environmental source, including already-playing loops. */
	void SetEnvironmentSoundGain(float Gain) { EnvironmentSoundGain=FMath::Clamp(Gain,0.f,1.f); }

	/**
	 * Play a Wave DID directly (e.g. portal-space ambient 0x0A000316). Optional loop for tunnel hold.
	 */
	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void PlayCenteredWave(int32 WaveId, float Volume = 1.f, bool bLoop = false);

	/** Stop every active sound started by this component (portal mute / cleanup). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void StopAllSounds(float FadeSeconds = 0.f);

	/** Stop every active PhysicsScript / particle emitter (portal-space cleanup). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Effects")
	void StopAllEffects();

	/** Local replacement of this item's appearance; keeps effect lifetimes running.
	 * Owners independently release suppression when their cosmetic is removed. */
	void SetCosmeticParticlesHidden(UObject* Owner, bool bHidden);

	/** Restore OpacityMul / appearance after StopAllEffects or portal reveal. */
	void RestoreMeshVisuals();

	/** Execute a frame-crossing AnimationHook. Direction was already filtered by motion playback. */
	void DispatchAnimationHook(const FACEDatAnimationHook& Hook);

	/** Retail Frame::grotate(omega) on owner root (portal tunnel applies in PrePhysics). */
	void ApplyRootOmegaRotation(float DeltaTime);

	/** UE-space radians/sec for retail object-frame grotate (portal default anim). */
	FVector GetRootOmegaRadiansPerSecond() const { return Omega; }
	/** Read-only inventory for ace.RenderAudit; not a per-frame actor scan. */
	void AccumulateParticleAudit(int32& Emitters, int32& Degraded, int32& Particles, int32& Lights) const;

private:
    TMap<int32, FTransform> ImpactFrames;
	struct FActiveScript
	{
		uint32 Id = 0;
		/** PlayScript enum or direct PhysicsScript DID; 0 for ordinary default scripts. */
		uint32 SourcePlayScript = 0;
		TArray<FACEDatPhysicsScriptEntry> Entries;
		double StartTime = 0.0;
		float Intensity = 1.f;
		int32 NextEntry = 0;
		int32 Depth = 0;
	};

	struct FActiveParticle
	{
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		int32 InstanceIndex = INDEX_NONE;
		FVector Position = FVector::ZeroVector;
		FVector StartOrigin = FVector::ZeroVector;
		/** World-space birth offset, including the transformed hook origin. */
		FVector Offset = FVector::ZeroVector;
		FVector A = FVector::ZeroVector;
		FVector B = FVector::ZeroVector;
		FVector C = FVector::ZeroVector;
		/** Birth parent frame (for LR types when !is_parent_local). */
		FQuat StartRotation = FQuat::Identity;
		uint32 DrawMode = 1; // Retail GfxObjInfo, independent of particle motion.
		/** Still/lamp glow: keep one mesh at constant opacity (retail HW sprites look solid). */
		bool bHoldStill = false;
		/** EmitterInfo.is_parent_local — Update uses current parent origin each tick. */
		bool bParentLocal = false;
		/** Held weapon: transform hook through the gfx PMC including placement scale. */
		bool bWieldedGfx = false;
		int32 Type = 1;
		float Age = 0.f;
		float Life = 1.f;
		float StartScale = 1.f;
		float FinalScale = 1.f;
		float StartTrans = 0.f;
		float FinalTrans = 0.f;
	};

	uint32 NextEmitterId = 0xFFFF0000u;

	struct FActiveEmitter
	{
		bool bVRHandFeedback = false;
		float MaxDegradeDistance = 100.f;
		bool bDegraded = false;
		uint32 InstanceId = 0;
		/** ParticleEmitterInfo DID (0x32…). */
		uint32 AuthoredEmitterId = 0;
		/** CreateParticle SecondaryId / local slot (0 = unslotted; many lamp flames share one PE). */
		uint32 SlotId = 0;
		/** PlayScript that created this emitter (for intensity-tier replacement). */
		uint32 SourcePlayScript = 0;
		FACEDatParticleEmitterInfo Info;
		int32 RenderSortPriority = 400;
		TWeakObjectPtr<USceneComponent> Parent;
		/** Hook frame only (AC→UE). Parent may already include Setup BindTransform. */
		FTransform HookFrame = FTransform::Identity;
		/** CreateParticle origin in part-local Unreal cm (not via FTransform::GetLocation). */
		FVector HookLocalUe = FVector::ZeroVector;
		FTransform RelativeFrame = FTransform::Identity;
		/** Setup part index, or -1 for object root (wire 0xFFFFFFFF). */
		int32 PartIndex = -1;
		TArray<FActiveParticle> Particles;
		TObjectPtr<UACEParticleBatchComponent> Batch;
		FVector ShapeExtent = FVector::ZeroVector;
		bool bInstanced = false;
		uint32 DrawMode = 1;
		bool bShapeGroundDisc = false;
		bool bShapeVerticalSprite = false;
		bool bShapeHwCube = false;
		FVector LastOrigin = FVector::ZeroVector;
		FVector LastEmissionOrigin = FVector::ZeroVector;
		float Age = 0.f;
		/** Seconds since last EmitParticle — retail emits at most once per update. */
		float TimeSinceEmit = 0.f;
		int32 TotalBorn = 0;
		bool bStopped = false;
		bool bBlocking = false;
		/** PhysicsScriptIntensity / PlayScript mod — scales emit rate and particle motion. */
		float Intensity = 1.f;
		/** Cached from particle MID luminosity — tick must not crush to 1. */
		float EmissiveBoost = 1.f;
		/** Point-light tint sampled from the particle gfx surface. */
		FLinearColor LightColor = FLinearColor::White;
		float LightLum = 0.f;
	};

	struct FScalarTween
	{
		EACEAnimationHookType Type = EACEAnimationHookType::NoOp;
		int32 PartIndex = INDEX_NONE;
		float Start = 0.f;
		float End = 0.f;
		float Age = 0.f;
		float Duration = 0.f;
	};

	struct FUvScroll
	{
		int32 PartIndex = INDEX_NONE;
		FVector2D Offset = FVector2D::ZeroVector;
		FVector2D Velocity = FVector2D::ZeroVector;
	};

	UACEDatSubsystem* GetDat() const;
	UACECharacterAppearanceComponent* GetAppearance() const;
	uint32 ResolveEffect(uint32 ScriptType, float Intensity) const;
	uint32 SelectCandidate(const FACEDatPhysicsScriptTable* Table, uint32 ScriptType, float Intensity) const;
	void PlayScriptInternal(uint32 ScriptId, float Intensity, int32 Depth, float Delay, uint32 SourcePlayScript = 0);
	/** Drop prior PeTable results for this PlayScript so intensity tiers cannot stack. */
	void StopPlayScriptEffects(uint32 SourcePlayScript);
	void ExecuteHook(const FACEDatAnimationHook& Hook, float Intensity, int32 Depth, uint32 SourcePlayScript = 0, uint32 CurrentScriptId = 0);
	void ExecuteSound(uint32 IdOrType, float Volume, bool bUseTable, float Priority = 1.f, float Probability = 0.f);
	USceneComponent* GetVRFeedbackAnchor(uint32 ScriptType) const;
	uint32 ExecutingPlayScript = 0;
	void CleanupFinishedAudio();
	void CreateEmitter(const FACEDatAnimationHook& Hook, bool bBlocking, float Intensity, uint32 SourcePlayScript = 0);
	void ResolveEmitterParent(FActiveEmitter& Emitter) const;
	void StartDefaultScripts(bool bIsDoor);
	/** Map a hook's numbered slot to its active visual instance. */
	uint32 ResolveEmitterSlot(uint32 SlotId, uint32 SourcePlayScript) const;
	void StopEmitter(uint32 InstanceId, bool bDestroy);
	void TickScripts(float DeltaTime);
	void TickParticleSimulation(float DeltaTime);
	void TickParticlePresentation(float RenderAhead);
	void ApplyParticlePose(FActiveEmitter& Emitter, FActiveParticle& Particle, float MotionAge,
		const FTransform* LiveParentFrame, const FVector* ViewPosition, const FVector& ParentDelta);
	void WakeEffectTick();
	void UpdateEffectTickInterval();
	void TickEmitters(float DeltaTime);
	bool GetParticleViewLocation(FVector& OutLocation) const;
	bool ShouldDegradeEmitter(const FActiveEmitter& Emitter, const FVector& ViewLocation) const;
	void SetEmitterDegraded(FActiveEmitter& Emitter, bool bDegraded);
	void TickParticleLights(bool bPresentationOnly = false);
	void TickTweens(float DeltaTime);
	void TickUvScrolls(float DeltaTime);
	void SetUvVelocity(int32 PartIndex, float USpeed, float VSpeed);
	void SpawnParticle(FActiveEmitter& Emitter);
	bool EnsureParticleBatch(FActiveEmitter& Emitter);
	void ReleaseParticleVisual(FActiveEmitter& Emitter, FActiveParticle& Particle);
	void ApplyParticleVisual(FActiveEmitter& Emitter, FActiveParticle& Particle, const FQuat& Rotation, const FVector& Scale, float Opacity);
	UProceduralMeshComponent* AcquireParticleMesh(uint32 GfxObjId, bool bBlocking, int32 SortPriority = 400);
	void ReleaseParticleMesh(UProceduralMeshComponent* Mesh);
	FTransform GetEmitterTransform(const FActiveEmitter& Emitter) const;
	FQuat GetParticleDrawRotation(const FVector& Position, const FQuat& SimulationRotation, uint32 Mode,
		const FVector* ViewPosition = nullptr) const;
	/** AC XY plane (thin Z) — lies on the ground; do not tip toward camera. */
	static bool IsGroundPlaneDiscMesh(const UProceduralMeshComponent* Mesh);
	/** AC XZ plane (thin Y, faces −Y) — vertical sprites that should camera-billboard. */
	static bool IsVerticalSpriteMesh(const UProceduralMeshComponent* Mesh);
	/** HW GfxObj cubes (~0.6 AC after shrink) — retail point sprites, not 3D debris. */
	static bool IsSmallHwSpriteCube(const UProceduralMeshComponent* Mesh);
	/** Retail Particle::Init StartFrame (part/object world). Scale stripped unless bKeepScale. */
	FTransform GetParticleStartFrame(const FActiveEmitter& Emitter, bool bKeepScale = false) const;
	/** Held weapons / creatures: follow the mesh even when DAT IsParentLocal is 0. */
	bool ShouldFollowOwner() const;
	/** True when this component is on a wielded item (not a creature / player). */
	bool IsWieldedItemFx() const;
	/** CreateParticle Frame.Origin in part-local Unreal cm (DAT only — no AABB/fudge). */
	FVector ResolveHookOffsetLocal(const FActiveEmitter& Emitter) const;
	void ApplyVisualValue(EACEAnimationHookType Type, int32 PartIndex, float Value);
	void ApplyMaterialScalar(int32 PartIndex, FName Parameter, float Value);
	void ApplyMaterialVector(int32 PartIndex, FName Parameter, const FVector& Value);
	/** Clone shared DAT MIDs onto the mesh Outer so Opacity/UV tweaks never mutate the cache. */
	static UMaterialInstanceDynamic* EnsurePrivateMaterialInstance(UMeshComponent* Mesh, int32 MaterialIndex);
	/** Opaque DAT materials ignore OpacityMul — swap to the translucent base for door dissolve. */
	void EnsureDissolveTranslucentMaterials(int32 PartIndex = INDEX_NONE);
	void RestoreDissolveMaterials(int32 PartIndex = INDEX_NONE);

	struct FDissolveMeshBackup
	{
		TWeakObjectPtr<UMeshComponent> Mesh;
		TArray<UMaterialInterface*> Materials;
	};
	TArray<FDissolveMeshBackup> DissolveMaterialBackup;
	/** Original materials remain owned while temporary fades replace mesh references. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> DissolveMaterialsRetained;

	float WorldScale = 100.f;
	float ObjectScale = 1.f;
	uint32 SetupId = 0;
	uint32 ObjectPeTableId = 0;
	uint32 ObjectSoundTableId = 0;
	uint32 ObjectDefaultScript = 0;
	float ObjectDefaultIntensity = 1.f;
	uint32 SetupDefaultScript = 0;
	uint32 SetupScriptTableId = 0;
	uint32 SetupSoundTableId = 0;
	FVector Omega = FVector::ZeroVector;
	bool bDefaultStarted = false;
	bool bAppearanceReady = false;
	bool bPhysicsHidden = false;
	bool bHiddenEffectActive = false;
	/** Guards PlayScriptInternal → TickScripts(0) so CallPES cannot recurse unbound. */
	bool bFlushingScripts = false;
	/** True for creatures — root Omega must not override F748/F74C prediction. */
	bool bSuppressRootOmega = false;
	/** Sky rain/lightning PES — particles sort in front of world geometry (2D SFX). */
	bool bEnvironmentWeather = false;
	TOptional<double> EnvironmentSkyTime, PreviousEnvironmentSkyTime;
	/** Terrain scenery surface luminosity is self-brightness, not a local light.
	 * Gameplay FX and placed props retain their inferred fill lighting. */
	bool bAllowInferredParticleLights = true;
	TMap<uint32, int32> EnvironmentScriptDrawOrder;
	FLinearColor EnvironmentAmbient = FLinearColor::Black;
	FLinearColor EnvironmentSunColor = FLinearColor::Black;
	FLinearColor EnvironmentSunDirection = FLinearColor(0,0,1);
	TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> EnvironmentSkyMaterials;
	/** Physics Transparent hook (AC 0=opaque, 1=invisible). Multiplies particle OpacityMul. */
	float ObjectTranslucency = 0.f;
	/** CPhysicsObj::translucencyOriginal: temporary fades cannot make ghosts opaque. */
	float OriginalObjectTranslucency = 0.f;
	/** Next ExecuteSound plays as 2D (UI / portal-space PlaySoundFromCenter). */
	bool bForceCenteredSound = false;
	bool bForceAmbientSound = false;
	/** Next ExecuteSound / PlayCenteredWave loops until StopAllSounds. */
	bool bForceLoopSound = false;
	/** Next ExecuteSound spawns at an absolute world position (outdoor ambient). */
	bool bHasSoundWorldLocation = false;
	FVector SoundWorldLocation = FVector::ZeroVector;
	FRandomStream Random;
	int32 RandomObjectGuid = 0;
	bool bHasRandomObject = false;

	TArray<FActiveScript> ActiveScripts;
	TArray<FActiveScript> ScheduledScripts;
	double ScriptClock = 0.0;
	double ParticleTimeSinceUpdate = 0.0;
	bool bWokeFromIdleTick = false;
	TArray<FActiveEmitter> ActiveEmitters;
	TSet<TWeakObjectPtr<UObject>> CosmeticParticleHideOwners;
	bool bCosmeticParticlesHidden = false;
	TArray<FScalarTween> Tweens;
	TArray<FUvScroll> UvScrolls;
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> ParticlePool;
	bool bUpdatingParticleVisuals = false;

	UPROPERTY(Transient)
	TArray<FActiveAceSound> ActiveSounds;
	float EnvironmentSoundGain = 1.f;
};
