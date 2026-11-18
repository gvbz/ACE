#include "ACEScriptComponent.h"
#include "VR/ACEVRComponent.h"
#include "ACEProfiling.h"
#include "ACERuntimeOptions.h"

#include "ACEDatSubsystem.h"
#include "ACELoadingScreenActor.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEWorldEntityActor.h"
#include "ACERegionSceneryActor.h"
#include "ACEEnvCellActor.h"
#include "ACEEffectLightSubsystem.h"
#include "ACEPlayerController.h"
#include "ACELandblockActor.h"
#include "ACETypes.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "CollisionQueryParams.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"
#include "Math/RotationMatrix.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundAttenuation.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformTime.h"
#include "ProceduralMeshComponent.h"
#include "ACEParticleBatchComponent.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarParticleBatches(TEXT("ace.Particles.Batched"), 1,
	TEXT("Batch spell particles by emitter; 0 enables the reference per-particle renderer for comparisons."));
static TAutoConsoleVariable<int32> CVarParticleDistanceCulling(TEXT("ace.Particles.DistanceCulling"), 1,
	TEXT("Use retail GfxObj degrade distances to suspend distant particle simulation and lights. 0 is the unbounded reference path."));
static TAutoConsoleVariable<float> CVarParticleIdleInterval(TEXT("ace.Particles.IdleTickInterval"), .1f,
	TEXT("Seconds between range checks for components containing only suspended infinite ambient emitters. 0 keeps the per-frame reference path."));
static TAutoConsoleVariable<int32> CVarParticleReuseFrames(TEXT("ace.Particles.ReuseEmitterFrames"), 1,
	TEXT("Resolve the camera and each emitter's live parent frame once per simulation update. 0 is the per-particle reference path."));

namespace
{
	constexpr int32 MaxScriptDepth = 8;
	constexpr int32 MaxScripts = 128;
	constexpr int32 MaxParticlesPerEmitter = 512;
	constexpr int32 MaxEmitters = 128;
	constexpr int32 MaxPooledParticleMeshes = 96;

	void ClassifyParticleExtent(const FVector& Ext, bool& bGroundDisc, bool& bVerticalSprite, bool& bHwCube)
	{
		const float Horiz = FMath::Max(Ext.X, Ext.Y);
		const float ThinZ = Ext.Z;
		bGroundDisc = Horiz > 5.f && ThinZ < Horiz * 0.35f;
		const float Face = FMath::Max(Ext.X, Ext.Z);
		const float ThinY = Ext.Y;
		bVerticalSprite = Face > 1.f && Face < 80.f && ThinY < Face * 0.25f;
		const float MaxE = Ext.GetMax();
		const float MinE = Ext.GetMin();
		bHwCube = MaxE >= 4.f && MaxE <= 80.f && MinE > MaxE * 0.45f;
	}

	// Retail PScriptType: all twelve directional blood splatters, regardless of color.
	bool IsBloodSplatter(uint32 ScriptType)
	{
		return ScriptType >= 0x5Bu && ScriptType <= 0x66u;
	}

	uint32 AetheriaSlotScope(uint32 Source)
	{
		// Keep the five sustained surge visuals independent of spell-animation
		// slots 5..9. This is renderer isolation, not a change to the DAT's motion,
		// emission interval or lifetime. Enum and direct-DID playback share a scope
		// so a new proc replaces the old one instead of accumulating copies.
		if (Source >= 0xA2u && Source <= 0xA6u) return Source;
		if (Source >= 0x3300125Bu && Source <= 0x3300125Fu) return 0xA2u + Source - 0x3300125Bu;
		return 0;
	}

	bool IsPhysicsScriptDid(uint32 Id)
	{
		return (Id & 0xFF000000u) == 0x33000000u;
	}

	/** Town Network / dungeon / destroyed portal Setups. Their swirl cards are vertical
	 *  -Y quads (often 0x010016C8) but must keep birth/parent facing — never camera-lock. */
	bool IsRetailPortalFxSetup(uint32 SetupId)
	{
		switch (SetupId)
		{
		case 0x020001B3u: // Purple
		case 0x020005D2u: // Blue
		case 0x020005D3u: // Green
		case 0x020005D4u: // Orange
		case 0x020005D5u: // Red
		case 0x020005D6u: // Yellow
		case 0x020006F4u: // White
		case 0x020008FDu: // Shadow
		case 0x02000F2Eu: // Broken
		case 0x020019E4u: // Destroyed
			return true;
		default:
			return false;
		}
	}

	float SafeFloat(float Value, float Fallback = 0.f)
	{
		return FMath::IsFinite(Value) ? Value : Fallback;
	}
}

UACEScriptComponent::UACEScriptComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	Random.Initialize(0xACE);
}

void UACEScriptComponent::InitializeFromObject(const FACEWorldObject& Object, float InWorldScale)
{
	WorldScale = FMath::Max(0.01f, InWorldScale);
	ObjectScale = Object.GetValidObjectScale();
	const uint32 NewSetupId = static_cast<uint32>(Object.SetupId);
	const bool bSameObject = bHasRandomObject && RandomObjectGuid == Object.Guid;
	const bool bSameSetup = bSameObject && SetupId == NewSetupId && bDefaultStarted;
	SetupId = NewSetupId;
	ObjectPeTableId = static_cast<uint32>(Object.PhysicsEffectTableId);
	ObjectSoundTableId = static_cast<uint32>(Object.SoundTableId);
	ObjectDefaultScript = static_cast<uint32>(Object.DefaultScriptId);
	ObjectDefaultIntensity = (Object.DefaultScriptIntensity > KINDA_SMALL_NUMBER)
		? Object.DefaultScriptIntensity
		: 1.f;
	Omega = Object.Omega;
	if (!bSameObject)
	{
		// Object/property refreshes must not restart the emission sequence. Doing
		// so makes successive stars reuse the same offset whenever updates arrive
		// faster than the emitter's birth interval.
		RandomObjectGuid = Object.Guid;
		bHasRandomObject = true;
		Random.Initialize(static_cast<int32>(Object.Guid) ^ 0x33AC67A);
	}
	if (UACECharacterAppearanceComponent* Appearance = GetAppearance())
	{
		AddTickPrerequisiteComponent(Appearance);
	}
	if (!bSameSetup)
	{
		// Hook morph (empty → Font of Jojii) and scenery Setup swaps must clear prior FX.
		StopAllEffects();
		bAppearanceReady = false;
		bDefaultStarted = false;
	}
	OriginalObjectTranslucency = FMath::Clamp(SafeFloat(Object.Translucency, 0.f), 0.f, 1.f);
	if (!bSameSetup) ObjectTranslucency = OriginalObjectTranslucency;
	// Creatures get root pose from F748/F74C prediction — DefaultAnim SetOmega must not
	// fight that with AddActorWorldRotation (chickens / NPCs).
	bSuppressRootOmega = (Object.ItemType & ACEItemType::Creature) != 0
		|| Object.bIsPlayer || Object.bIsSelf;
	bEnvironmentWeather = false;
	// Region snow, insects, smoke and other scenery run ordinary Setup scripts,
	// not SkyDesc weather. Retail CPartArray::InitLights uses Setup::lights;
	// a particle surface's Luminosity only describes its own brightness. Treating
	// that as a moving light creates hundreds of scene light interactions in snow.
	// Keep scenery's world-space parenting, distance culling and batching intact.
	const AACERegionSceneryActor* Scenery = Cast<AACERegionSceneryActor>(GetOwner());
	bAllowInferredParticleLights = !Scenery || !Scenery->IsRegionTerrainScenery();

	SetupDefaultScript = 0;
	SetupScriptTableId = 0;
	SetupSoundTableId = 0;
	if (UACEDatSubsystem* Dat = GetDat())
	{
		bool bHasPointLight = false;
		Dat->TryGetSetupRuntimeMetadata(SetupId, SetupDefaultScript, SetupScriptTableId, SetupSoundTableId, &bHasPointLight);
		const auto* Room = Scenery ? Cast<AACEEnvCellActor>(Scenery->GetOwner()) : nullptr;
		// The placed fixture supplies this flame's scene light. Keep its particles visible.
		if (Room && !Room->bSeenOutside && bHasPointLight) bAllowInferredParticleLights = false;
	}
	// ObjectCreate/UpdateObject often omit PeTable (0). Players/creatures still need
	// 0x34000004 so TargetEffects (HealthUpYellow from pools, buffs) resolve.
	if (ObjectPeTableId == 0
		&& (Object.bIsPlayer || Object.bIsSelf || (Object.ItemType & ACEItemType::Creature) != 0
			|| bSuppressRootOmega))
	{
		ObjectPeTableId = 0x34000004u;
	}

	if (!Omega.IsNearlyZero())
	{
		// Omega is AC-space radians/sec.
		// Angular velocity is axial: the X reflection reverses handedness.
		Omega = -FACEPosition::AceVectorToUnreal(Omega, 1.f);
	}

	SetPhysicsHidden((Object.PhysicsState & ACEPhysicsState::Hidden) != 0);

	// Default scripts start in NotifyAppearanceReady — Presenter applies DAT meshes async,
	// and emitters must parent to part BindTransforms (Town Network Z=1.415) that only exist then.
}

void UACEScriptComponent::InitializeForEnvironment(float InWorldScale)
{
	EnvironmentSkyTime.Reset(); PreviousEnvironmentSkyTime.Reset();
	EnvironmentScriptDrawOrder.Reset();
	WorldScale = FMath::Max(0.01f, InWorldScale);
	ObjectScale = 1.f;
	SetupId = 0;
	ObjectPeTableId = 0;
	ObjectSoundTableId = 0;
	ObjectDefaultScript = 0;
	ObjectDefaultIntensity = 1.f;
	SetupDefaultScript = 0;
	SetupScriptTableId = 0;
	SetupSoundTableId = 0;
	Omega = FVector::ZeroVector;
	OriginalObjectTranslucency = 0.f;
	bSuppressRootOmega = true;
	bEnvironmentWeather = true;
	bAllowInferredParticleLights = false;
	bDefaultStarted = true; // do not auto-start Setup defaults
	bAppearanceReady = true;
	Random.Initialize(0xACE5E7);
	bHasRandomObject = false;
	StopAllEffects();
	WakeEffectTick();
}

void UACEScriptComponent::SetEnvironmentLighting(const FLinearColor& Ambient, const FLinearColor& SunColor, const FLinearColor& Direction)
{
    // A star field can own thousands of particle materials. Sub-byte changes
    // in a two-hour day cycle must not rebuild every uniform buffer per frame.
    if (EnvironmentAmbient.Equals(Ambient,.004f) && EnvironmentSunColor.Equals(SunColor,.004f)
        && EnvironmentSunDirection.Equals(Direction,.004f)) return;
    EnvironmentAmbient=Ambient; EnvironmentSunColor=SunColor; EnvironmentSunDirection=Direction;
    for (int32 I=EnvironmentSkyMaterials.Num()-1; I>=0; --I)
    {
        auto* Mid=EnvironmentSkyMaterials[I].Get();
        if (!Mid) { EnvironmentSkyMaterials.RemoveAtSwap(I); continue; }
        Mid->SetVectorParameterValue(TEXT("SkyAmbient"),Ambient);
        Mid->SetVectorParameterValue(TEXT("SkySunColor"),SunColor);
        Mid->SetVectorParameterValue(TEXT("SkySunDirection"),Direction);
    }
}

void UACEScriptComponent::SetEnvironmentScriptDrawOrder(uint32 ScriptId, int32 SortPriority)
{
	EnvironmentScriptDrawOrder.Add(ScriptId, SortPriority);
}

void UACEScriptComponent::StartDefaultScripts(bool bIsDoor)
{
	if (bDefaultStarted)
	{
		return;
	}
	// Re-fetch Setup defaults — InitializeFromObject often runs before portal DAT is ready,
	// leaving SetupDefaultScript=0. Latched empty then skips Claude-style ambient FX forever.
	if (SetupId != 0)
	{
		UACEDatSubsystem* Dat = GetDat();
		if (!Dat || !Dat->TryGetSetupRuntimeMetadata(SetupId, SetupDefaultScript, SetupScriptTableId, SetupSoundTableId))
		{
			// DAT still cold — retry on a later NotifyAppearanceReady.
			return;
		}
	}
	// A loaded setup with no default script is ready too. Treating zero as a
	// failed load made duplicate descriptors clear animation-driven visibility.
	bDefaultStarted = true;
	(void)bIsDoor;
	// ObjectCreate DefaultScript is a PlayScript enum (e.g. ProjectileCollision=0x5A) for
	// ScriptedCollision — NOT auto-play-on-spawn. Only Setup DefaultScript PhysicsScript
	// DIDs (0x33…) start here — retail InitDefaults → play_script_internal(setup.DefaultScript).
	// Doors still play Setup DefaultScript (pedestal glow). Ethereal/NoDraw from that
	// script are ignored while the door is held closed.
	if (SetupDefaultScript != 0 && IsPhysicsScriptDid(SetupDefaultScript))
	{
		UE_LOG(LogTemp, Verbose, TEXT("ACEScript: StartDefaultScripts setup=0x%08X script=0x%08X"),
			SetupId, SetupDefaultScript);
		PlayScriptInternal(SetupDefaultScript, 1.f, 0, 0.f);
		WakeEffectTick();
	}
}

void UACEScriptComponent::SetPhysicsHidden(bool bHidden)
{
    bPhysicsHidden = bHidden;
    if (!bAppearanceReady || bHiddenEffectActive == bHidden) return;
    bHiddenEffectActive = bHidden;
    PlayEffect(bHidden ? 0x76 : 0x75); // PS_Hidden / PS_UnHide, not portal-object effects.
}

void UACEScriptComponent::NotifyAppearanceChanging()
{
	RestoreDissolveMaterials();
}

void UACEScriptComponent::NotifyAppearanceReady(bool bIsDoor)
{
	const bool bWasReady = bAppearanceReady;
	bAppearanceReady = true;
	for (FActiveEmitter& Emitter : ActiveEmitters)
	{
		ResolveEmitterParent(Emitter);
		if (!bWasReady) Emitter.LastOrigin = GetEmitterTransform(Emitter).GetLocation();
		// Rebind future births/current parent-local motion, not live particle
		// initial conditions. Retail ParticleEmitter::UpdateParticles uses the
		// saved start_frame for non-parent-local particles throughout their life.
		// Resetting those frames here snapped trails onto a running weapon and
		// replacing Offset with the hook erased the authored random radial spread.
		// Keep LastOrigin too: property refresh is not a reset of distance emission.
	}
	StartDefaultScripts(bIsDoor);
	if (auto* Appearance = GetAppearance()) Appearance->InitializePropAnimationPose();
	SetPhysicsHidden(bPhysicsHidden);
	if (ActiveEmitters.Num() == 0 && ActiveScripts.Num() == 0 && ScheduledScripts.Num() == 0 && ActiveSounds.Num() == 0
		&& Tweens.Num() == 0 && UvScrolls.Num() == 0 && Omega.IsNearlyZero())
	{
		SetComponentTickEnabled(false);
	}
	else
	{
		WakeEffectTick();
	}
}

USceneComponent* UACEScriptComponent::GetVRFeedbackAnchor(uint32 ScriptType) const
{
	// Short self-cast feedback is authored around the desktop avatar's chest.
	// Put it at the casting hand for the local VR wearer; observers keep retail FX.
	const bool Feedback = ScriptType == 0x51u || (ScriptType >= 0x1Fu && ScriptType <= 0x24u) || (ScriptType >= 0x49u && ScriptType <= 0x4Eu);
	if (Feedback && GetOwner())
		if (auto* VR = GetOwner()->FindComponentByClass<UACEVRComponent>(); VR && VR->IsActive()) return VR->GetHeldAnchor(1);
	return nullptr;
}

void UACEScriptComponent::ResolveEmitterParent(FActiveEmitter& Emitter) const
{
    if (!ImpactFrames.IsEmpty())
    {
        Emitter.Parent = GetOwner()->GetRootComponent();
        const FTransform Frame = ImpactFrames.Contains(Emitter.PartIndex) ? ImpactFrames[Emitter.PartIndex] : ImpactFrames[-1];
        Emitter.RelativeFrame = Emitter.HookFrame * Frame.GetRelativeTransform(GetOwner()->GetActorTransform());
        return;
    }
	if (auto* Hand = GetVRFeedbackAnchor(Emitter.SourcePlayScript))
	{
		Emitter.bVRHandFeedback = true;
		Emitter.Parent = Hand; Emitter.HookFrame = Emitter.RelativeFrame = FTransform::Identity;
		Emitter.HookLocalUe = FVector::ZeroVector;
		return;
	}
	Emitter.Parent = nullptr;
	Emitter.RelativeFrame = Emitter.HookFrame;

	const AACEWorldEntityActor* Ent = Cast<AACEWorldEntityActor>(GetOwner());
	const bool bCreatureLike = Ent == nullptr
		|| Ent->bIsPlayer
		|| (Ent->ItemType & ACEItemType::Creature) != 0;

	// Retail Particle::Init: partIdx -1 = PhysicsObj origin; else part world * hook frame.
	// Wielded weapons keep PartIndex 0 on the gfx (tip / head). Parenting those to the
	// actor root put wand/staff FX in the hand.
	const bool bObjectRoot = Emitter.PartIndex < 0;
	if (UACECharacterAppearanceComponent* Appearance = GetAppearance())
	{
		USceneComponent* MeshRoot = Appearance->GetMeshRoot();
		if (!bObjectRoot)
		{
			// Prefer the live part PMC (hand/weapon joint). UE world = Relative * Parent.
			if (USceneComponent* Part = Appearance->GetPartMesh(Emitter.PartIndex))
			{
				Emitter.Parent = Part;
				Emitter.RelativeFrame = Emitter.HookFrame;
				// Identity-relative HUMAN parts (anim not applied yet) pin HealthUp to the
				// MeshRoot at the feet. Prefer the Setup bind (human part 9 ≈ chest).
				if (bCreatureLike && Part->GetRelativeLocation().SizeSquared() < 4.f)
				{
					FTransform Bind;
					if (Appearance->GetPartBindTransform(Emitter.PartIndex, Bind)
						&& Bind.GetLocation().SizeSquared() > 16.f)
					{
						Bind.SetScale3D(FVector::OneVector);
						Emitter.Parent = MeshRoot;
						Emitter.RelativeFrame = Emitter.HookFrame * Bind;
					}
				}
			}
			else
			{
				FTransform PartXform;
				const bool bHavePart =
					Appearance->GetPartCurrentTransform(Emitter.PartIndex, PartXform)
					|| Appearance->GetPartBindTransform(Emitter.PartIndex, PartXform);
				if (bHavePart && MeshRoot)
				{
					PartXform.SetScale3D(FVector::OneVector);
					Emitter.Parent = MeshRoot;
					// UE composes the local hook first, then its part frame.
					Emitter.RelativeFrame = Emitter.HookFrame * PartXform;
				}
				else if (MeshRoot)
				{
					Emitter.Parent = MeshRoot;
				}
			}
		}
		else if (!bCreatureLike && GetOwner() && GetOwner()->GetRootComponent())
		{
			// MeshRoot is shifted by capsule FeetOffsetZ for standing creatures. World
			// props and wielded items use the actor/PhysicsObj origin (retail partIdx -1).
			Emitter.Parent = GetOwner()->GetRootComponent();
		}
		else if (MeshRoot)
		{
			Emitter.Parent = MeshRoot;
		}
	}
	if (!Emitter.Parent.IsValid() && GetOwner())
	{
		Emitter.Parent = GetOwner()->GetRootComponent();
	}
}

void UACEScriptComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (FActiveEmitter& Emitter : ActiveEmitters)
	{
		for (FActiveParticle& Particle : Emitter.Particles)
		{
			if (UProceduralMeshComponent* Mesh = Particle.Mesh.Get())
			{
				Mesh->DestroyComponent();
			}
		}
		if (Emitter.Batch)
		{
			Emitter.Batch->DestroyComponent();
			Emitter.Batch = nullptr;
		}
	}
	for (TWeakObjectPtr<UProceduralMeshComponent>& Pooled : ParticlePool)
	{
		if (UProceduralMeshComponent* Mesh = Pooled.Get())
		{
			Mesh->DestroyComponent();
		}
	}
	ActiveEmitters.Reset();
	ParticlePool.Reset();
	const auto* SoundOwner = Cast<AACEWorldEntityActor>(GetOwner());
	const bool FinishDeathSounds = EndPlayReason == EEndPlayReason::Destroyed && SoundOwner
		&& SoundOwner->bReceivedDeathMotion && GetWorld() && GetDat() && !GetDat()->IsInPortalSpace();
	for (FActiveAceSound& Sound : ActiveSounds)
	{
		if (Sound.Component)
		{
			if (FinishDeathSounds && !Sound.bLooping && Sound.EndSeconds>FPlatformTime::Seconds())
			{
				Sound.Component->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
				Sound.Component->bStopWhenOwnerDestroyed = false;
				Sound.Component->bAutoDestroy = true;
				const TWeakObjectPtr<UAudioComponent> Tail = Sound.Component;
				FTimerHandle Cleanup;
				GetWorld()->GetTimerManager().SetTimer(Cleanup, [Tail]()
				{ if (Tail.IsValid()) { Tail->Stop(); Tail->DestroyComponent(); } },
					FMath::Clamp(float(Sound.EndSeconds-FPlatformTime::Seconds()), .05f, 30.f), false);
				continue;
			}
			Sound.Component->Stop();
			Sound.Component->DestroyComponent();
		}
	}
	ActiveSounds.Reset();
	if (auto* Lights = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
		Lights->RemoveSource(this);
	Super::EndPlay(EndPlayReason);
}

UACEDatSubsystem* UACEScriptComponent::GetDat() const
{
	const UWorld* World = GetWorld();
	const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
}

UACECharacterAppearanceComponent* UACEScriptComponent::GetAppearance() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UACECharacterAppearanceComponent>() : nullptr;
}

void UACEScriptComponent::PlayScriptId(int32 PhysicsScriptId, float Intensity)
{
	const uint32 Id = static_cast<uint32>(PhysicsScriptId);
	if (!IsPhysicsScriptDid(Id))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE Script: rejected non-PhysicsScript DID 0x%08X"), Id);
		return;
	}
	// Tag emitters with the PhysicsScript DID so sky weather windows can stop one PES cleanly.
	PlayScriptInternal(Id, Intensity, 0, 0.f, Id);
}

void UACEScriptComponent::StopScriptId(uint32 PhysicsScriptId)
{
	if (PhysicsScriptId == 0)
	{
		return;
	}
	for (int32 i = ActiveScripts.Num() - 1; i >= 0; --i)
	{
		if (ActiveScripts[i].Id == PhysicsScriptId || ActiveScripts[i].SourcePlayScript == PhysicsScriptId)
		{
			ActiveScripts.RemoveAt(i, 1, EAllowShrinking::No);
		}
	}
	StopPlayScriptEffects(PhysicsScriptId);
}

uint32 UACEScriptComponent::SelectCandidate(
	const FACEDatPhysicsScriptTable* Table, uint32 ScriptType, float Intensity) const
{
	if (!Table)
	{
		return 0;
	}
	const TArray<FACEDatPhysicsScriptCandidate>* Candidates = Table->Entries.Find(ScriptType);
	if (!Candidates || Candidates->Num() == 0)
	{
		return 0;
	}

	TArray<FACEDatPhysicsScriptCandidate, TInlineAllocator<8>> Ordered;
	for (const FACEDatPhysicsScriptCandidate& Candidate : *Candidates)
	{
		if (FMath::IsFinite(Candidate.Mod) && Candidate.ScriptId != 0)
		{
			Ordered.Add(Candidate);
		}
	}
	if (Ordered.Num() == 0)
	{
		return 0;
	}
	// Highest tier the intensity qualifies for (Intensity >= Mod). Lead scarab Scale=0.05
	// must pick mod=0, not the first entry where Intensity <= Mod (that skipped to 0.5).
	Ordered.Sort([](const FACEDatPhysicsScriptCandidate& A, const FACEDatPhysicsScriptCandidate& B)
	{
		return A.Mod < B.Mod;
	});
	uint32 BestId = 0;
	for (const FACEDatPhysicsScriptCandidate& Candidate : Ordered)
	{
		if (Intensity + KINDA_SMALL_NUMBER >= Candidate.Mod)
		{
			BestId = Candidate.ScriptId;
		}
		else
		{
			break;
		}
	}
	return BestId != 0 ? BestId : Ordered[0].ScriptId;
}

uint32 UACEScriptComponent::ResolveEffect(uint32 ScriptType, float Intensity) const
{
	UACEDatSubsystem* Dat = GetDat();
	if (!Dat)
	{
		return 0;
	}
	// Retail CPhysicsObj::play_script_type resolves PlayScript enums ONLY through the object's
	// own PhysicsScriptTable (PeTable), then the Setup's DefaultScriptTable. There is no global
	// fallback. Bolts/walls/arrows all carry their own PeTable in the world DB
	// (e.g. flamebolt wcid 1499 → 0x34000005), so they resolve here without help. Ring
	// projectiles (wcid 7269-7274) deliberately have NO PeTable — a hardcoded fallback to
	// 0x34000005 made all 9 ring projectiles play the flamebolt Launch/Explode burst, which is
	// the "ring casts every variant at once" artifact. Rings show only their flying GfxObj.
	uint32 Id = 0;
	if (ObjectPeTableId != 0)
	{
		Id = SelectCandidate(Dat->GetPhysicsScriptTable(ObjectPeTableId), ScriptType, Intensity);
	}
	if (Id == 0 && SetupScriptTableId != 0)
	{
		Id = SelectCandidate(Dat->GetPhysicsScriptTable(SetupScriptTableId), ScriptType, Intensity);
	}
	// Human/creature ObjectCreate sometimes omits PeTable. Do NOT use this fallback on
	// held weapons — EnchantUp* would wrap the staff in body-buff FX.
	if (Id == 0 && bSuppressRootOmega)
	{
		Id = SelectCandidate(Dat->GetPhysicsScriptTable(0x34000004u), ScriptType, Intensity);
	}
	return Id;
}

UACEScriptComponent* UACEScriptComponent::PlayImpactEffect(int32 ScriptType, float Intensity)
{
    if (!IsBloodSplatter(uint32(ScriptType))) { PlayEffect(ScriptType, Intensity); return this; }
    if (!GetWorld() || !GetOwner()) return nullptr;
    // The victim's FIFO may contain a long buff or death dissolve. Independent
    // impact playback starts now and cannot be erased by corpse replacement.
    FActorSpawnParameters Params; Params.ObjectFlags |= RF_Transient;
    auto* Host = GetWorld()->SpawnActor<AActor>(Params);
    if (!Host) return nullptr;
    auto* Root = NewObject<USceneComponent>(Host);
    Host->AddInstanceComponent(Root); Host->SetRootComponent(Root); Root->RegisterComponent();
    Host->SetActorTransform(GetOwner()->GetActorTransform());
    Host->SetActorEnableCollision(false); Host->SetLifeSpan(5.f);
    auto* Impact = NewObject<UACEScriptComponent>(Host);
    Host->AddInstanceComponent(Impact);
    Impact->WorldScale = WorldScale; Impact->ObjectScale = ObjectScale;
    Impact->SetupId = SetupId; Impact->ObjectPeTableId = ObjectPeTableId;
    Impact->SetupScriptTableId = SetupScriptTableId;
    Impact->bAppearanceReady = Impact->bDefaultStarted = true;
    Impact->bSuppressRootOmega = true;
    Impact->Random.Initialize(Random.GetUnsignedInt());
    const auto* App = GetAppearance();
    for (int32 Part = -1; Part < (App ? App->GetPartCount() : 0); ++Part)
    {
        FActiveEmitter Sample; Sample.PartIndex = Part;
        Impact->ImpactFrames.Add(Part, GetParticleStartFrame(Sample, true));
    }
    Impact->RegisterComponent();
    Impact->PlayEffect(ScriptType, Intensity);
    return Impact;
}

void UACEScriptComponent::PlayEffect(int32 ScriptType, float Intensity)
{
	const float SafeIntensity = FMath::IsFinite(Intensity) ? Intensity : 0.f;
	const uint32 PlayScript = static_cast<uint32>(ScriptType);
	// Server replays ambient aura (0x58) on ObjMaint — one instance per object is enough.
	if (PlayScript == 0x58u)
	{
		for (const FActiveScript& S : ActiveScripts)
		{
			if (S.SourcePlayScript == PlayScript)
			{
				return;
			}
		}
		if (AActor* Owner = GetOwner())
		{
			if (UWorld* World = Owner->GetWorld())
			{
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					FVector ListenerLoc;
					FRotator IgnoredRot;
					PC->GetPlayerViewPoint(ListenerLoc, IgnoredRot);
					const float MaxDist = 42.f * FMath::Max(1.f, WorldScale);
					if (FVector::DistSquared(ListenerLoc, Owner->GetActorLocation()) > MaxDist * MaxDist)
					{
						return;
					}
				}
			}
		}
	}
	const uint32 ScriptId = ResolveEffect(PlayScript, SafeIntensity);
	if (ScriptId == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ACE Script: PlayEffect type=0x%X intensity=%.3f unresolved (peTable=0x%08X setupTable=0x%08X)"),
			PlayScript, SafeIntensity, ObjectPeTableId, SetupScriptTableId);
		return;
	}
	UE_LOG(LogTemp, Verbose,
		TEXT("ACE FX: PlayEffect type=0x%X intensity=%.3f → script=0x%08X peTable=0x%08X emitters=%d"),
		PlayScript, SafeIntensity, ScriptId, ObjectPeTableId, ActiveEmitters.Num());
	// Retail ScriptManager appends effects to this object's FIFO. Replacing or
	// starting every received effect at once loses authored hook order.
	PlayScriptInternal(ScriptId, SafeIntensity, 0, 0.f, PlayScript);
}

void UACEScriptComponent::StopPlayScriptEffects(uint32 SourcePlayScript)
{
	if (SourcePlayScript == 0)
	{
		return;
	}
	ScheduledScripts.RemoveAll([SourcePlayScript](const FActiveScript& Script)
	{ return Script.SourcePlayScript == SourcePlayScript; });
	for (int32 i = ActiveScripts.Num() - 1; i >= 0; --i)
	{
		if (ActiveScripts[i].SourcePlayScript == SourcePlayScript)
		{
			ActiveScripts.RemoveAt(i, 1, EAllowShrinking::No);
		}
	}
	for (int32 i = ActiveEmitters.Num() - 1; i >= 0; --i)
	{
		if (ActiveEmitters[i].SourcePlayScript != SourcePlayScript)
		{
			continue;
		}
		for (FActiveParticle& Particle : ActiveEmitters[i].Particles)
		{
			ReleaseParticleVisual(ActiveEmitters[i], Particle);
		}
		if (ActiveEmitters[i].Batch)
		{
			ActiveEmitters[i].Batch->DestroyComponent();
			ActiveEmitters[i].Batch = nullptr;
		}
		ActiveEmitters.RemoveAtSwap(i, 1, EAllowShrinking::No);
	}
}

void UACEScriptComponent::PlayScriptInternal(
	uint32 ScriptId, float Intensity, int32 Depth, float Delay, uint32 SourcePlayScript)
{
	if (Depth > MaxScriptDepth || ActiveScripts.Num() + ScheduledScripts.Num() >= MaxScripts)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE Script: recursion/active limit reached at 0x%08X"), ScriptId);
		return;
	}
	UACEDatSubsystem* Dat = GetDat();
	const FACEDatPhysicsScript* Script = Dat ? Dat->GetPhysicsScript(ScriptId) : nullptr;
	if (!Script)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE Script: PhysicsScript 0x%08X unavailable"), ScriptId);
		return;
	}
	WakeEffectTick();

	FActiveScript Active;
	Active.Id = ScriptId;
	Active.SourcePlayScript = SourcePlayScript != 0 ? SourcePlayScript
		: (AetheriaSlotScope(ScriptId) != 0 ? ScriptId : 0);
	Active.Entries = Script->Entries;
	Active.Entries.StableSort([](const FACEDatPhysicsScriptEntry& A, const FACEDatPhysicsScriptEntry& B)
	{
		return A.StartTime < B.StartTime;
	});
	// Keep intensity 0 (level-1 bolts / low rings) — do not clamp to 0.01 or PeTable
	// selection / nested CallPES pick the wrong mod tier.
	Active.Intensity = FMath::Clamp(SafeFloat(Intensity, 0.f), 0.f, 100.f);
	Active.Depth = Depth;
	const double Pause = FMath::Max(0.f, SafeFloat(Delay));
	if (Pause > 0.0)
	{
		// CPhysicsObj::CallPES schedules a future call; it joins ScriptManager
		// only when the pause expires, so an ambient timer cannot block combat FX.
		Active.StartTime = ScriptClock + Pause;
		ScheduledScripts.Add(MoveTemp(Active));
		ScheduledScripts.StableSort([](const FActiveScript& A, const FActiveScript& B)
		{ return A.StartTime < B.StartTime; });
	}
	else
	{
		Active.StartTime = ActiveScripts.IsEmpty() ? ScriptClock : ActiveScripts.Last().StartTime
			+ (ActiveScripts.Last().Entries.IsEmpty() ? 0.0 : ActiveScripts.Last().Entries.Last().StartTime);
		ActiveScripts.Add(MoveTemp(Active));
	}
	if (!bFlushingScripts) TickScripts(0.f);
}

void UACEScriptComponent::PlaySound(int32 SoundType, float Volume)
{
	if (const AACEWorldEntityActor* WorldEntity = Cast<AACEWorldEntityActor>(GetOwner()))
	{
		PlaySoundAtWorldLocation(SoundType, Volume, WorldEntity->GetSoundEmitLocation());
		return;
	}
	ExecuteSound(static_cast<uint32>(SoundType), Volume, true);
}

void UACEScriptComponent::PlaySoundAtWorldLocation(int32 SoundType, float Volume, const FVector& WorldLocation)
{
	bHasSoundWorldLocation = true;
	SoundWorldLocation = WorldLocation;
	bForceCenteredSound = false;
	ExecuteSound(static_cast<uint32>(SoundType), Volume, true);
	bHasSoundWorldLocation = false;
}

void UACEScriptComponent::PlaySoundFromTable(int32 SoundTableId, int32 SoundType, float Volume)
{
	if (SoundTableId == 0)
	{
		return;
	}
	const uint32 PrevObject = ObjectSoundTableId;
	const uint32 PrevSetup = SetupSoundTableId;
	ObjectSoundTableId = static_cast<uint32>(SoundTableId);
	SetupSoundTableId = 0;
	ExecuteSound(static_cast<uint32>(SoundType), Volume, true);
	ObjectSoundTableId = PrevObject;
	SetupSoundTableId = PrevSetup;
}

void UACEScriptComponent::PlayCenteredSoundFromTable(int32 SoundTableId, int32 SoundType, float Volume)
{
	bForceCenteredSound = true;
	PlaySoundFromTable(SoundTableId, SoundType, Volume);
	bForceCenteredSound = false;
}

void UACEScriptComponent::PlayAmbientSoundFromTable(
	int32 SoundTableId, int32 SoundType, float Volume, const FVector& WorldLocation, bool bLoop)
{
	if (UACEDatSubsystem* Dat = GetDat())
	{
		if (Dat->IsInPortalSpace())
		{
			return;
		}
	}
	bHasSoundWorldLocation = true;
	SoundWorldLocation = WorldLocation;
	bForceCenteredSound = bLoop;
	bForceAmbientSound = true;
	bForceLoopSound = bLoop;
	PlaySoundFromTable(SoundTableId, SoundType, Volume);
	bForceLoopSound = false;
	bHasSoundWorldLocation = false;
	bForceCenteredSound = false;
	bForceAmbientSound = false;
}

void UACEScriptComponent::PlayCenteredWave(int32 WaveId, float Volume, bool bLoop)
{
	if (WaveId == 0)
	{
		return;
	}
	bForceCenteredSound = true;
	bForceLoopSound = bLoop;
	ExecuteSound(static_cast<uint32>(WaveId), Volume, /*bUseTable*/ false);
	bForceLoopSound = false;
	bForceCenteredSound = false;
}

void UACEScriptComponent::StopAllSounds(float FadeSeconds)
{
	for (FActiveAceSound& Sound : ActiveSounds)
	{
		if (UAudioComponent* Audio = Sound.Component)
		{
			if (FadeSeconds>0.f)
			{
				Audio->FadeOut(FadeSeconds, 0.f);
				Sound.bLooping=false;
				Sound.EndSeconds=FPlatformTime::Seconds()+FadeSeconds+.1f;
				continue;
			}
			Audio->Stop();
			Audio->DestroyComponent();
		}
	}
	if (FadeSeconds<=0.f) ActiveSounds.Reset();
}

void UACEScriptComponent::DispatchAnimationHook(const FACEDatAnimationHook& Hook)
{
	if (!IsValid(this) || !GetOwner() || GetOwner()->IsActorBeingDestroyed())
	{
		return;
	}
	// Motion evaluation owns direction filtering because it knows the effective frame
	// direction after authored rate and caller time direction are combined.
	ExecuteHook(Hook, 1.f, 0);
}

void UACEScriptComponent::ExecuteSound(
	uint32 IdOrType, float Volume, bool bUseTable, float Priority, float Probability)
{
	UACEDatSubsystem* Dat = GetDat();
	AActor* Owner = GetOwner();
	if (!Dat || !Owner)
	{
		return;
	}
	// Portal tunnel: only the centered portal whoosh (ForceCentered) is allowed.
	if (Dat->IsInPortalSpace() && !bForceCenteredSound)
	{
		return;
	}

	FVector SpatialEmitLocation = Owner->GetActorLocation();
	if (!bForceCenteredSound && !bEnvironmentWeather)
	{
		if (const AACEWorldEntityActor* WorldEntity = Cast<AACEWorldEntityActor>(Owner))
		{
			SpatialEmitLocation = WorldEntity->GetSoundEmitLocation();
		}
	}
	if (bHasSoundWorldLocation)
	{
		SpatialEmitLocation = SoundWorldLocation;
	}

	// Cull world SFX beyond retail hearing (~landcell). Server already limits to known
	// players, but ObjMaint range is larger than audible distance — without this cull
	// (and when attenuation is misapplied) every landblock sound feels "on the player".
	constexpr float MaxHearAcUnits = 48.f;
	if (!bForceCenteredSound && !bEnvironmentWeather)
	{
		if (UWorld* World = Owner->GetWorld())
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				FVector ListenerLoc = FVector::ZeroVector;
				FRotator ListenerRot = FRotator::ZeroRotator;
				PC->GetPlayerViewPoint(ListenerLoc, ListenerRot);
				const float MaxDist = MaxHearAcUnits * FMath::Max(1.f, WorldScale);
				if (FVector::DistSquared(ListenerLoc, SpatialEmitLocation) > MaxDist * MaxDist)
				{
					return;
				}
			}
		}
	}

	uint32 WaveId = IdOrType;
	float EffectiveVolume = FMath::Clamp(SafeFloat(Volume, 1.f), 0.f, 4.f);
	if (bUseTable)
	{
		const uint32 TableId = ObjectSoundTableId != 0 ? ObjectSoundTableId : SetupSoundTableId;
		const FACEDatSoundTable* Table = TableId ? Dat->GetSoundTable(TableId) : nullptr;
		const FACEDatSoundData* Data = Table ? Table->Data.Find(IdOrType) : nullptr;
		if (!Data || Data->Entries.Num() == 0)
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("ACE Sound: no table entry type=%u table=0x%08X"), IdOrType, TableId);
			return;
		}
		float TotalWeight = 0.f;
		for (const FACEDatSoundTableEntry& Entry : Data->Entries)
		{
			TotalWeight += FMath::Max(0.f, Entry.Probability);
		}
		float Pick = TotalWeight > 0.f ? Random.FRandRange(0.f, TotalWeight) : 0.f;
		const FACEDatSoundTableEntry* Selected = &Data->Entries[0];
		for (const FACEDatSoundTableEntry& Entry : Data->Entries)
		{
			Pick -= FMath::Max(0.f, Entry.Probability);
			if (Pick <= 0.f)
			{
				Selected = &Entry;
				break;
			}
		}
		WaveId = Selected->SoundId;
		EffectiveVolume *= FMath::Max(0.f, Selected->Volume);
		Priority = Selected->Priority;
		Probability = Selected->Probability;
	}

	// Retail data commonly uses 0 for "always"; nonzero values are a normalized chance.
	if (Probability > 0.f && Probability < 1.f && Random.FRand() > Probability)
	{
		return;
	}

	const FACEDatWave* Wave = Dat->GetWave(WaveId);
	if (!Wave || Wave->Header.Num() < 16 || Wave->Data.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE Sound: missing/empty wave=0x%08X"), WaveId);
		return;
	}

	auto ReadU16 = [&Wave](int32 Offset) -> uint16
	{
		return Wave->Header.IsValidIndex(Offset + 1)
			? static_cast<uint16>(Wave->Header[Offset] | (Wave->Header[Offset + 1] << 8))
			: 0;
	};
	auto ReadU32 = [&Wave](int32 Offset) -> uint32
	{
		return Wave->Header.IsValidIndex(Offset + 3)
			? static_cast<uint32>(Wave->Header[Offset])
				| (static_cast<uint32>(Wave->Header[Offset + 1]) << 8)
				| (static_cast<uint32>(Wave->Header[Offset + 2]) << 16)
				| (static_cast<uint32>(Wave->Header[Offset + 3]) << 24)
			: 0;
	};

	const uint16 Format = ReadU16(0);
	const uint16 Channels = ReadU16(2);
	const uint32 SampleRate = ReadU32(4);
	const uint16 Bits = ReadU16(14);
	if (Format != 1 || Channels == 0 || Channels > 2 || SampleRate < 1000 || SampleRate > 192000
		|| (Bits != 8 && Bits != 16))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ACE Sound: unsupported wave=0x%08X format=0x%X channels=%u rate=%u bits=%u"),
			WaveId, Format, Channels, SampleRate, Bits);
		return;
	}

	TArray<uint8> Pcm16;
	if (Bits == 16)
	{
		Pcm16 = Wave->Data;
	}
	else
	{
		// WAV PCM8 is unsigned; Unreal procedural PCM is signed 16-bit.
		Pcm16.SetNumUninitialized(Wave->Data.Num() * 2);
		for (int32 Index = 0; Index < Wave->Data.Num(); ++Index)
		{
			const int16 Sample = static_cast<int16>((static_cast<int32>(Wave->Data[Index]) - 128) << 8);
			Pcm16[Index * 2] = static_cast<uint8>(Sample & 0xFF);
			Pcm16[Index * 2 + 1] = static_cast<uint8>((Sample >> 8) & 0xFF);
		}
	}

	// Pad to an even sample frame so QueueAudio's SampleByteSize checks never reject.
	if ((Pcm16.Num() % 2) != 0)
	{
		Pcm16.Add(0);
	}

	// Weather sheets stay 2D (sky dome far from camera). UI portal whoosh is centered.
	// Everything else — world object SFX and outdoor ambient — is distance + stereo pan.
	const bool bAmbient2D = (bEnvironmentWeather && !bHasSoundWorldLocation) || bForceCenteredSound;

	// Preserve mono cues for Unreal's point-source spatializer. Duplicating them
	// into stereo both doubled PCM memory and changed their spatial behavior.
	const uint16 EffectiveChannels = Channels;

	CleanupFinishedAudio();
	constexpr int32 MaxConcurrentSounds = 48;
	while (ActiveSounds.Num() >= MaxConcurrentSounds)
	{
		if (UAudioComponent* Oldest = ActiveSounds[0].Component)
		{
			Oldest->Stop();
			Oldest->DestroyComponent();
		}
		ActiveSounds.RemoveAt(0);
	}

	USoundWaveProcedural* Procedural = NewObject<USoundWaveProcedural>(GetWorld());
	if (!Procedural)
	{
		return;
	}
	const float DurationSec = static_cast<float>(Pcm16.Num())
		/ static_cast<float>(SampleRate * EffectiveChannels * sizeof(int16));
	Procedural->SetSampleRate(static_cast<int32>(SampleRate));
	Procedural->NumChannels = EffectiveChannels;
	Procedural->Duration = FMath::Max(0.01f, DurationSec);
	Procedural->SoundGroup = SOUNDGROUP_Effects;
	Procedural->Priority = FMath::Clamp(SafeFloat(Priority, 1.f), 0.f, 100.f);
	Procedural->bLooping = bForceLoopSound;
	Procedural->VirtualizationMode = EVirtualizationMode::Disabled;
	Procedural->QueueAudio(Pcm16.GetData(), Pcm16.Num());
	if (bForceLoopSound)
	{
		// Keep a second copy queued so procedural loop has sample data ready.
		Procedural->QueueAudio(Pcm16.GetData(), Pcm16.Num());
	}

	FSoundAttenuationSettings Attenuation;
	USoundAttenuation* AttenuationAsset = nullptr;
	if (bAmbient2D)
	{
		Attenuation.bAttenuate = false;
		Attenuation.bSpatialize = false;
	}
	else
	{
		// Retail hearing is roughly tens of AC units. WorldScale maps AC→UU (default 100).
		const float Scale = FMath::Max(1.f, WorldScale);
		Attenuation.bAttenuate = true;
		Attenuation.bSpatialize = true;
		Attenuation.bAttenuateWithLPF = true;
		Attenuation.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
		Attenuation.AttenuationShape = EAttenuationShape::Sphere;
		Attenuation.AttenuationShapeExtents = FVector(4.f * Scale, 0.f, 0.f);
		Attenuation.FalloffDistance = 32.f * Scale;
		Attenuation.LPFRadiusMin = 12.f * Scale;
		Attenuation.LPFRadiusMax = 56.f * Scale;
		// SpawnSound* applies attenuation at start — overrides set after Play are ignored.
		AttenuationAsset = NewObject<USoundAttenuation>(GetWorld());
		if (AttenuationAsset)
		{
			AttenuationAsset->Attenuation = Attenuation;
		}
	}

	const float BaseVolume = EffectiveVolume;
	EffectiveVolume *= ACERuntimeOptions::SoundGain(bAmbient2D || bForceAmbientSound) * EnvironmentSoundGain;
	UAudioComponent* Audio = nullptr;
	if (bHasSoundWorldLocation || !bAmbient2D)
	{
		const FVector Loc = SpatialEmitLocation;
		Audio = UGameplayStatics::SpawnSoundAtLocation(
			GetWorld()->GetWorldSettings(),
			Procedural,
			Loc,
			FRotator::ZeroRotator,
			EffectiveVolume,
			1.f,
			0.f,
			AttenuationAsset,
			nullptr,
			/*bAutoDestroy*/ false);
	}
	else
	{
		USceneComponent* Attach = Owner->GetRootComponent();
		Audio = UGameplayStatics::SpawnSoundAttached(
			Procedural,
			Attach ? Attach : Owner->GetRootComponent(),
			NAME_None,
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			EAttachLocation::KeepRelativeOffset,
			/*bStopWhenAttachedToDestroyed*/ true,
			EffectiveVolume,
			1.f,
			0.f,
			AttenuationAsset,
			nullptr,
			/*bAutoDestroy*/ false);
	}
	if (!Audio)
	{
		// WorldSettings survives a creature's removal but participates in world
		// teardown. A component owned directly by UWorld has neither lifecycle.
		AActor* AudioOwner = GetWorld()->GetWorldSettings();
		UAudioComponent* Manual = NewObject<UAudioComponent>(AudioOwner);
		if (Manual)
		{
			AudioOwner->AddInstanceComponent(Manual);
			Manual->SetSound(Procedural);
			Manual->bAutoDestroy = false;
			Manual->bAllowSpatialization = !bAmbient2D;
			Manual->RegisterComponentWithWorld(GetWorld());
			if (!bAmbient2D)
			{
				Manual->SetWorldLocation(SpatialEmitLocation);
				Manual->bOverrideAttenuation = true;
				Manual->SetAttenuationOverrides(Attenuation);
			}
			Manual->SetVolumeMultiplier(EffectiveVolume);
			Manual->Play();
			Audio = Manual;
		}
	}
	if (!Audio)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE Sound: spawn failed wave=0x%08X"), WaveId);
		return;
	}

	Audio->bAllowSpatialization = !bAmbient2D;
	if (!bAmbient2D)
	{
		Audio->bOverrideAttenuation = true;
		Audio->SetAttenuationOverrides(Attenuation);
		Audio->SetWorldLocation(SpatialEmitLocation);
	}
	Audio->SetVolumeMultiplier(EffectiveVolume);
	if (bForceAmbientSound && bForceLoopSound) Audio->FadeIn(.75f, EffectiveVolume);

	FActiveAceSound Active;
	if (auto* Anchor = GetVRFeedbackAnchor(ExecutingPlayScript))
	{
		Audio->AttachToComponent(Anchor, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		Audio->SetRelativeLocation(FVector::ZeroVector);
	}
	Active.Component = Audio;
	Active.BaseVolume = BaseVolume;
	Active.bAmbient = bAmbient2D || bForceAmbientSound;
	Active.Wave = Procedural;
	Active.StartSeconds = FPlatformTime::Seconds();
	Active.bLooping = bForceLoopSound;
	if (Active.bLooping)
	{
		Active.LoopPcm = Pcm16;
	}
	// Grace past Duration: procedural IsPlaying is unreliable (false early / true forever).
	Active.EndSeconds = Active.bLooping
		? TNumericLimits<double>::Max()
		: Active.StartSeconds + static_cast<double>(DurationSec) + 0.25;
	ActiveSounds.Add(Active);

	UE_LOG(LogTemp, Verbose,
		TEXT("ACE Sound: play wave=0x%08X %uch %uHz %ubit dur=%.2fs vol=%.2f ambient2d=%d world=%d"),
		WaveId, EffectiveChannels, SampleRate, Bits, DurationSec, EffectiveVolume,
		bAmbient2D ? 1 : 0, (bHasSoundWorldLocation || !bAmbient2D) ? 1 : 0);
}

void UACEScriptComponent::CleanupFinishedAudio()
{
	const double Now = FPlatformTime::Seconds();
	for (int32 Index = ActiveSounds.Num() - 1; Index >= 0; --Index)
	{
		FActiveAceSound& Sound = ActiveSounds[Index];
		if (Sound.Component)
        {
            const float Gain=Sound.BaseVolume * ACERuntimeOptions::SoundGain(Sound.bAmbient) * EnvironmentSoundGain;
            if (!FMath::IsNearlyEqual(Sound.Component->VolumeMultiplier,Gain)) Sound.Component->SetVolumeMultiplier(Gain);
        }
		if (Sound.bLooping)
		{
			// Procedural waves stop when the queued buffer drains — keep re-filling for portal.
			if (USoundWaveProcedural* Wave = Sound.Wave)
			{
				const int32 Queued = Wave->GetAvailableAudioByteCount();
				if (Sound.LoopPcm.Num() > 0 && Queued < Sound.LoopPcm.Num())
				{
					Wave->QueueAudio(Sound.LoopPcm.GetData(), Sound.LoopPcm.Num());
				}
			}
			if (UAudioComponent* Audio = Sound.Component)
			{
				if (!Audio->IsPlaying())
				{
					Audio->Play();
				}
			}
			continue;
		}
		UAudioComponent* Audio = Sound.Component;
		const bool bPastEnd = Now >= Sound.EndSeconds;
		// Procedural audio can report !IsPlaying while its initial buffer is
		// being submitted. The decoded PCM duration is the reliable deadline.
		if (!bPastEnd && Audio)
		{
			continue;
		}
		if (Audio)
		{
			Audio->Stop();
			Audio->DestroyComponent();
		}
		ActiveSounds.RemoveAtSwap(Index, 1, EAllowShrinking::No);
	}
}

void UACEScriptComponent::ApplyRootOmegaRotation(float DeltaTime)
{
	if (AActor* Owner = GetOwner(); Owner && !Omega.IsNearlyZero() && !bSuppressRootOmega)
	{
		// Frame::grotate is an axis-angle quaternion, not three Euler rotations.
		const FQuat Delta(Omega.GetSafeNormal(), Omega.Size() * DeltaTime);
		Owner->AddActorWorldRotation(Delta);
	}
}

void UACEScriptComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	ACE_PROFILE_SCOPE(Effects);
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!ImpactFrames.IsEmpty())
	{
		// A pending logout still shows the world (including incoming hits) until
		// its retail delay and world fade finish. Retire only in the portal view.
		const auto* Dat = GetDat();
		if (Dat && Dat->IsInPortalSpace())
		{
			GetOwner()->Destroy();
			return;
		}
	}
	// Unreal's first tick after interval cancellation can include the old idle
	// period. Newly started combat effects must age only by the current frame.
	const float Dt = FMath::Max(0.f, SafeFloat(bWokeFromIdleTick && GetWorld()
		? FMath::Min(DeltaTime, GetWorld()->GetDeltaSeconds()) : DeltaTime));
	bWokeFromIdleTick = false;
	CleanupFinishedAudio();
	// Empty distant components must reach the idle-disable path below. Returning
	// early here kept finished ambient components ticking forever outside 45 m.
	if (AActor* Owner = GetOwner(); Owner && !Omega.IsNearlyZero() && !bSuppressRootOmega
		&& !Owner->IsA<AACELoadingScreenActor>())
	{
		ApplyRootOmegaRotation(Dt);
	}
	TickScripts(Dt);
	TickTweens(Dt);
	TickUvScrolls(Dt);
	TickParticleSimulation(Dt);
	UpdateEffectTickInterval();
	if (ActiveEmitters.Num() == 0 && ActiveScripts.Num() == 0 && ScheduledScripts.Num() == 0 && ActiveSounds.Num() == 0
		&& Tweens.Num() == 0 && UvScrolls.Num() == 0 && Omega.IsNearlyZero())
	{
        if (!ImpactFrames.IsEmpty()) GetOwner()->Destroy();
        else SetComponentTickEnabled(false);
	}
}

void UACEScriptComponent::WakeEffectTick()
{
	// A new script/hook must not inherit a distant ambient emitter's cooldown.
	if (GetComponentTickInterval() > 0.f)
	{
		bWokeFromIdleTick = true;
		SetComponentTickIntervalAndCooldown(0.f);
	}
	SetComponentTickEnabled(true);
}

void UACEScriptComponent::UpdateEffectTickInterval()
{
	bool Idle = !bEnvironmentWeather && ImpactFrames.IsEmpty() && Omega.IsNearlyZero()
		&& ActiveScripts.IsEmpty() && ScheduledScripts.IsEmpty() && ActiveSounds.IsEmpty()
		&& Tweens.IsEmpty() && UvScrolls.IsEmpty() && !ActiveEmitters.IsEmpty();
	for (const auto& Emitter : ActiveEmitters)
		Idle &= Emitter.bDegraded && !Emitter.bStopped && !Emitter.bVRHandFeedback
			&& Emitter.Info.TotalSeconds <= 0.0 && Emitter.Info.TotalParticles <= 0;
	const float Interval = Idle ? FMath::Clamp(CVarParticleIdleInterval.GetValueOnGameThread(), 0.f, .1f) : 0.f;
	if (GetComponentTickInterval() != Interval) SetComponentTickIntervalAndCooldown(Interval);
}

void UACEScriptComponent::TickScripts(float DeltaTime)
{
	ACE_PROFILE_SCOPE(EffectScripts);
	TGuardValue<bool> Flushing(bFlushingScripts, true);
	const double TargetTime = ScriptClock + FMath::Max(0.f, DeltaTime);
	for (int32 Dispatch = 0; Dispatch < 1024; ++Dispatch)
	{
		if (!ActiveScripts.IsEmpty() && ActiveScripts[0].NextEntry >= ActiveScripts[0].Entries.Num())
		{
			ActiveScripts.RemoveAt(0, 1, EAllowShrinking::No);
			continue;
		}
		const double NextHook = ActiveScripts.IsEmpty() ? TNumericLimits<double>::Max()
			: ActiveScripts[0].StartTime + ActiveScripts[0].Entries[ActiveScripts[0].NextEntry].StartTime;
		const double NextCall = ScheduledScripts.IsEmpty() ? TNumericLimits<double>::Max() : ScheduledScripts[0].StartTime;
		if (FMath::Min(NextHook, NextCall) > TargetTime) break;
		ScriptClock = FMath::Max(ScriptClock, FMath::Min(NextHook, NextCall));
		if (NextCall < NextHook)
		{
			FActiveScript Due = MoveTemp(ScheduledScripts[0]);
			ScheduledScripts.RemoveAt(0, 1, EAllowShrinking::No);
			Due.StartTime = ActiveScripts.IsEmpty() ? ScriptClock : ActiveScripts.Last().StartTime
				+ (ActiveScripts.Last().Entries.IsEmpty() ? 0.0 : ActiveScripts.Last().Entries.Last().StartTime);
			ActiveScripts.Add(MoveTemp(Due));
			continue;
		}
		// Keep the current script in the FIFO during Execute: a CallPES hook must
		// append after the tail's last hook, even when it is the current last hook.
		FActiveScript& Current = ActiveScripts[0];
		const auto Entry = Current.Entries[Current.NextEntry++];
		const float Intensity = Current.Intensity;
		const int32 Depth = Entry.StartTime > 0.0 ? 0 : Current.Depth;
		const uint32 Source = Current.SourcePlayScript, Id = Current.Id;
		if (Entry.Hook.Direction == EACEAnimationHookDirection::Forward
			|| Entry.Hook.Direction == EACEAnimationHookDirection::Both)
			ExecuteHook(Entry.Hook, Intensity, Depth, Source, Id);
	}
	ScriptClock = TargetTime;
}

void UACEScriptComponent::ExecuteHook(
	const FACEDatAnimationHook& Hook, float Intensity, int32 Depth, uint32 SourcePlayScript,
	uint32 CurrentScriptId)
{
	TGuardValue<uint32> SourceGuard(ExecutingPlayScript, SourcePlayScript);
	switch (Hook.Type)
	{
	case EACEAnimationHookType::NoOp:
	case EACEAnimationHookType::Attack:
	case EACEAnimationHookType::AnimationDone:
		break;
	case EACEAnimationHookType::Sound:
		ExecuteSound(Hook.Id, 1.f, false);
		break;
	case EACEAnimationHookType::SoundTable:
		ExecuteSound(Hook.Id, 1.f, true);
		break;
	case EACEAnimationHookType::SoundTweaked:
		ExecuteSound(Hook.Id, Hook.Volume, false, Hook.Priority, Hook.Probability);
		break;
	case EACEAnimationHookType::CreateParticle:
	case EACEAnimationHookType::CreateBlockingParticle:
		CreateEmitter(Hook, Hook.Type == EACEAnimationHookType::CreateBlockingParticle, Intensity, SourcePlayScript);
		break;
	case EACEAnimationHookType::DestroyParticle:
		StopEmitter(ResolveEmitterSlot(Hook.Id, SourcePlayScript), true);
		break;
	case EACEAnimationHookType::StopParticle:
		StopEmitter(ResolveEmitterSlot(Hook.Id, SourcePlayScript), false);
		break;
	case EACEAnimationHookType::CallPES:
	{
		// Retail CallPES Pause is a random delay in [0, Pause].
		const float Pause = FMath::Max(0.f, SafeFloat(Hook.Time));
		float Delay = Pause > KINDA_SMALL_NUMBER ? Random.FRandRange(0.f, Pause) : 0.f;
		// Ambient loops CallPES themselves (fountain 0x33000453 pause=30, Font Swarm
		// 0x33000455 pause=35). Those are restarts — reset depth. Incrementing depth on
		// every self-CallPES hit MaxScriptDepth and killed scenery particles forever.
		const bool bSelf = CurrentScriptId != 0 && Hook.Id == CurrentScriptId;
		if (bSelf && Delay <= KINDA_SMALL_NUMBER)
		{
			// Pause=0 self-loops (0x330006DA) still need a tick gap or ActiveScripts grows
			// without bound when StartTime is also 0.
			Delay = 1.f / 30.f;
		}
		const int32 NextDepth = (Delay > KINDA_SMALL_NUMBER || bSelf) ? 0 : (Depth + 1);
		if (IsPhysicsScriptDid(Hook.Id))
		{
			PlayScriptInternal(Hook.Id, Intensity, NextDepth, Delay, SourcePlayScript);
		}
		else if (const uint32 Nested = ResolveEffect(Hook.Id, Intensity))
		{
			PlayScriptInternal(Nested, Intensity, NextDepth, Delay,
				SourcePlayScript != 0 ? SourcePlayScript : Hook.Id);
		}
		break;
	}
	case EACEAnimationHookType::DefaultScript:
		// CPhysicsObj::play_default_script uses the replicated default effect/intensity.
		if (ObjectDefaultScript)
		{
			if (const uint32 Id = ResolveEffect(ObjectDefaultScript, ObjectDefaultIntensity))
				PlayScriptInternal(Id, ObjectDefaultIntensity, Depth + 1, 0.f, ObjectDefaultScript);
		}
		break;
	case EACEAnimationHookType::DefaultScriptPart:
	{
		// The part overload addresses the held child, not the wielder's setup script.
		auto* App = GetOwner()->FindComponentByClass<UACECharacterAppearanceComponent>();
		USceneComponent* Part = App ? App->GetPartMesh(Hook.PartIndex) : nullptr;
		TArray<AActor*> Children; GetOwner()->GetAttachedActors(Children);
		for (AActor* Child : Children)
			if (Part && Child && Child->GetRootComponent()->GetAttachParent() == Part)
			{
				if (auto* Scripts = Child->FindComponentByClass<UACEScriptComponent>())
				{
					FACEDatAnimationHook DefaultHook; DefaultHook.Type = EACEAnimationHookType::DefaultScript;
					Scripts->DispatchAnimationHook(DefaultHook);
				}
				break;
			}
		break;
	}
	case EACEAnimationHookType::SetOmega:
		// Retail SetOmegaHook::Execute → set_omega(axis) with no intensity scale.
		// animate_static_object then Frame::grotate(omega) once per ~MinQuantum (1/30s),
		// treating |omega| as radians per tick — convert to rad/s for Dt integration.
		Omega = -FACEPosition::AceVectorToUnreal(FVector(Hook.Vector), 1.f) * 30.f;
		WakeEffectTick();
		break;
	case EACEAnimationHookType::Ethereal:
		// Never SetActorEnableCollision(false) — that kills ECC_Visibility Use picking on
		// open doors. ApplyPhysicsState keeps ethereal QueryOnly + Visibility Block.
		if (AACEWorldEntityActor* World = Cast<AACEWorldEntityActor>(GetOwner()))
		{
			int32 State = World->PhysicsState;
			if (Hook.State != 0)
			{
				State |= ACEPhysicsState::Ethereal;
			}
			else
			{
				State &= ~ACEPhysicsState::Ethereal;
			}
			World->ApplyPhysicsState(State);
		}
		break;
	case EACEAnimationHookType::NoDraw:
		if (AACEWorldEntityActor* World = Cast<AACEWorldEntityActor>(GetOwner()))
		{
			// Creatures/players: Setup DefaultAnim NoDraw hooks thrash vs ApplyPhysicsState
			// on ObjDesc updates and blinked remotes. Ignore anim NoDraw for living actors —
			// server PhysicsState.NoDraw remains authoritative.
			const bool bCreatureOrPlayer = (World->ItemType & ACEItemType::Creature) != 0
				|| World->bIsPlayer || World->bIsSelf;
			if (bCreatureOrPlayer)
			{
				break;
			}
			// Retail NoDraw only hides Setup/part meshes — particles must keep drawing.
			// SetActorHiddenInGame would kill Launch bolt trails entirely.
			const bool bHideMesh = Hook.State != 0;
			// NoDrawHook::Execute -> set_nodraw updates physics state as well as
			// part visibility, so later collision changes preserve the visual state.
			World->PhysicsState = bHideMesh ? World->PhysicsState | ACEPhysicsState::NoDraw
				: World->PhysicsState & ~ACEPhysicsState::NoDraw;
			if (UACECharacterAppearanceComponent* App = GetAppearance())
			{
				App->SetAppearanceVisible(!bHideMesh);
			}
			if (World->Mesh)
			{
				World->Mesh->SetVisibility(!World->IsMeshSuppressed() && !World->bUsingDatMesh
					&& !World->bParticleOnlyAppearance);
			}
		}
		break;
	case EACEAnimationHookType::SetLight:
		if (IsBloodSplatter(SourcePlayScript)) break;
		if (auto* Lights = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
		{
			FACEEffectLightRequest Request;
			Request.Position = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
			Request.Color = FLinearColor(1.f, .82f, .55f);
			Request.Radius = IsWieldedItemFx() ? 320.f : 480.f;
			Request.Intensity = 6.4f;
			// Authored hook lights persist even when this component finishes its
			// script and stops ticking, but share the same world budget as particles.
			Lights->SetScriptLight(this, Hook.State != 0, Request);
		}
		break;
	case EACEAnimationHookType::Scale:
	case EACEAnimationHookType::Transparent:
	case EACEAnimationHookType::TransparentPart:
	case EACEAnimationHookType::Luminous:
	case EACEAnimationHookType::LuminousPart:
	case EACEAnimationHookType::Diffuse:
	case EACEAnimationHookType::DiffusePart:
	{
		// DefaultAnim / CallPES loops re-fire the same luminous/transparent hooks; stacking
		// tweens made torch/light flicker run far too fast.
		const int32 Part = static_cast<int32>(Hook.PartIndex);
		for (int32 i = Tweens.Num() - 1; i >= 0; --i)
		{
			if (Tweens[i].Type == Hook.Type && Tweens[i].PartIndex == Part)
			{
				Tweens.RemoveAtSwap(i, 1, EAllowShrinking::No);
			}
		}
		FScalarTween Tween;
		Tween.Type = Hook.Type;
		Tween.PartIndex = Part;
		Tween.Start = Hook.Type == EACEAnimationHookType::Scale
			? GetOwner()->GetActorScale3D().X / FMath::Max(ObjectScale, 0.01f)
			: Hook.Start;
		Tween.End = Hook.End;
		// Intensity selects PeTable tier — do not compress luminous/transparent durations
		// (that made light flicker race). Scarab scale still affects cast FX via script choice.
		Tween.Duration = FMath::Max(0.f, Hook.Time);
		if (Tween.Duration <= KINDA_SMALL_NUMBER)
			ApplyVisualValue(Tween.Type, Tween.PartIndex, Tween.End);
		else
		{
			// An animation hook can arrive after the idle script component stopped
			// ticking. Door fades (090000F9) have no emitter to wake it for us.
			ApplyVisualValue(Tween.Type, Tween.PartIndex, Tween.Start);
			Tweens.Add(Tween);
			WakeEffectTick();
		}
		break;
	}
	case EACEAnimationHookType::TextureVelocity:
		// Authored UV speeds are cycles/sec — do not multiply by PlayScript intensity.
		SetUvVelocity(INDEX_NONE, Hook.USpeed, Hook.VSpeed);
		break;
	case EACEAnimationHookType::TextureVelocityPart:
		SetUvVelocity(static_cast<int32>(Hook.PartIndex), Hook.USpeed, Hook.VSpeed);
		break;
	case EACEAnimationHookType::ReplaceObject:
		if (UACECharacterAppearanceComponent* Appearance = GetAppearance())
		{
			Appearance->ReplacePartGfxObj(static_cast<int32>(Hook.PartIndex), Hook.Id);
		}
		break;
	default:
		UE_LOG(LogTemp, Warning, TEXT("ACE Script: ignored unknown hook %d"), static_cast<int32>(Hook.Type));
		break;
	}
}

void UACEScriptComponent::TickTweens(float DeltaTime)
{
	for (int32 Index = Tweens.Num() - 1; Index >= 0; --Index)
	{
		FScalarTween& Tween = Tweens[Index];
		Tween.Age += DeltaTime;
		const float Alpha = Tween.Duration > 0.f ? FMath::Clamp(Tween.Age / Tween.Duration, 0.f, 1.f) : 1.f;
		ApplyVisualValue(Tween.Type, Tween.PartIndex, FMath::Lerp(Tween.Start, Tween.End, Alpha));
		if (Alpha >= 1.f)
			Tweens.RemoveAtSwap(Index, 1, EAllowShrinking::No);
	}
}

void UACEScriptComponent::ApplyVisualValue(EACEAnimationHookType Type, int32 PartIndex, float Value)
{
	switch (Type)
	{
	case EACEAnimationHookType::Scale:
		if (AActor* Owner = GetOwner())
			Owner->SetActorScale3D(FVector(ObjectScale * FMath::Clamp(SafeFloat(Value, 1.f), 0.001f, 100.f)));
		break;
	case EACEAnimationHookType::Transparent:
	case EACEAnimationHookType::TransparentPart:
		// AC translucency 1 = invisible; materials expose OpacityMul.
		// Portal FX hides the Setup mesh (ClipMap anchor) — fade must also scale particles.
		if (Type == EACEAnimationHookType::Transparent)
		{
			// CPhysicsObj::SetTranslucency2 clamps against the physics descriptor's
			// original translucency, including the final UnHide/death hook at zero.
			Value = FMath::Max(FMath::Clamp(Value, 0.f, 1.f), OriginalObjectTranslucency);
			ObjectTranslucency = Value;
		}
		if (Value > 0.02f)
		{
			EnsureDissolveTranslucentMaterials(Type == EACEAnimationHookType::TransparentPart ? PartIndex : INDEX_NONE);
		}
		ApplyMaterialScalar(Type == EACEAnimationHookType::TransparentPart ? PartIndex : INDEX_NONE,
			TEXT("OpacityMul"), 1.f - FMath::Clamp(Value, 0.f, 1.f));
		if (Value <= KINDA_SMALL_NUMBER)
		{
			// Return to the authored blend/lighting model after becoming opaque.
			// Leaving the dissolve material installed also loses solid shadows.
			RestoreDissolveMaterials(Type == EACEAnimationHookType::TransparentPart ? PartIndex : INDEX_NONE);
		}
		break;
	case EACEAnimationHookType::Luminous:
	case EACEAnimationHookType::LuminousPart:
		ApplyMaterialScalar(Type == EACEAnimationHookType::LuminousPart ? PartIndex : INDEX_NONE,
			TEXT("EmissiveStrength"), FMath::Max(0.f, Value));
		break;
	case EACEAnimationHookType::Diffuse:
	case EACEAnimationHookType::DiffusePart:
		ApplyMaterialScalar(Type == EACEAnimationHookType::DiffusePart ? PartIndex : INDEX_NONE,
			TEXT("DiffuseStrength"), FMath::Max(0.f, Value));
		break;
	default:
		break;
	}
}

void UACEScriptComponent::SetUvVelocity(int32 PartIndex, float USpeed, float VSpeed)
{
	WakeEffectTick();
	for (FUvScroll& Scroll : UvScrolls)
	{
		if (Scroll.PartIndex == PartIndex)
		{
			Scroll.Velocity = FVector2D(USpeed, VSpeed);
			return;
		}
	}
	FUvScroll Scroll;
	Scroll.PartIndex = PartIndex;
	Scroll.Velocity = FVector2D(USpeed, VSpeed);
	UvScrolls.Add(Scroll);
}

void UACEScriptComponent::TickUvScrolls(float DeltaTime)
{
	for (FUvScroll& Scroll : UvScrolls)
	{
		if (Scroll.Velocity.IsNearlyZero())
		{
			continue;
		}
		Scroll.Offset += Scroll.Velocity * DeltaTime;
		ApplyMaterialVector(Scroll.PartIndex, TEXT("UVOffset"),
			FVector(Scroll.Offset.X, Scroll.Offset.Y, 0.f));
	}
}

UMaterialInstanceDynamic* UACEScriptComponent::EnsurePrivateMaterialInstance(UMeshComponent* Mesh, int32 MaterialIndex)
{
	if (!Mesh || MaterialIndex < 0 || MaterialIndex >= Mesh->GetNumMaterials())
	{
		return nullptr;
	}
	UMaterialInterface* Source = Mesh->GetMaterial(MaterialIndex);
	if (!Source)
	{
		return nullptr;
	}
	if (UMaterialInstanceDynamic* Existing = Cast<UMaterialInstanceDynamic>(Source))
	{
		// GC'd / pending-kill shared MIDs assert inside IsValid — skip them.
		if (!Existing->IsValidLowLevelFast())
		{
			return nullptr;
		}
		if (Existing->GetOuter() == Mesh)
		{
			return Existing;
		}
		// Shared DAT cache MID — UE 5.8 forbids MID→MID parenting. Walk to Material/MIC.
		UMaterialInterface* Base = Existing->Parent;
		while (UMaterialInstanceDynamic* Nested = Cast<UMaterialInstanceDynamic>(Base))
		{
			if (!IsValid(Nested))
			{
				return nullptr;
			}
			Base = Nested->Parent;
		}
		if (!Base || (!Base->IsA<UMaterial>() && !Base->IsA<UMaterialInstanceConstant>()))
		{
			return nullptr;
		}
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, Mesh);
		if (!Mid)
		{
			return nullptr;
		}
		Mid->CopyParameterOverrides(Existing);
		Mesh->SetMaterial(MaterialIndex, Mid);
		return Mid;
	}
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Source, Mesh);
	if (Mid)
	{
		Mesh->SetMaterial(MaterialIndex, Mid);
	}
	return Mid;
}

void UACEScriptComponent::EnsureDissolveTranslucentMaterials(int32 PartIndex)
{
	UACEDatSubsystem* Dat = GetDat();
	if (!Dat)
	{
		return;
	}
	UMaterialInterface* TransBase = Dat->EnsureAceUnlitTranslucentMaterialBase();
	if (!TransBase)
	{
		return;
	}

	auto ConvertMesh = [&](UMeshComponent* Mesh)
	{
		if (!Mesh)
		{
			return;
		}
		const int32 Num = Mesh->GetNumMaterials();
		bool bAlreadyBacked = false;
		for (const FDissolveMeshBackup& Prev : DissolveMaterialBackup)
		{
			if (Prev.Mesh.Get() == Mesh)
			{
				bAlreadyBacked = true;
				break;
			}
		}
		FDissolveMeshBackup* Backup = nullptr;
		if (!bAlreadyBacked)
		{
			FDissolveMeshBackup& NewBackup = DissolveMaterialBackup.AddDefaulted_GetRef();
			NewBackup.Mesh = Mesh;
			NewBackup.Materials.SetNum(Num);
			Backup = &NewBackup;
		}
		for (int32 i = 0; i < Num; ++i)
		{
			UMaterialInterface* Source = Mesh->GetMaterial(i);
			if (Backup)
			{
				Backup->Materials[i] = Source;
				if (Source) DissolveMaterialsRetained.AddUnique(Source);
			}
			if (!Source)
			{
				continue;
			}
			const EBlendMode Blend = Source->GetBlendMode();
			if (Blend == BLEND_Translucent || Blend == BLEND_Additive || Blend == BLEND_Modulate)
			{
				continue;
			}
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(TransBase, Mesh);
			if (!Mid)
			{
				continue;
			}
			if (UMaterialInstance* Inst = Cast<UMaterialInstance>(Source))
			{
				Mid->CopyParameterOverrides(Inst);
			}
			else
			{
				UTexture* Tex = nullptr;
				if (Source->GetTextureParameterValue(TEXT("ACETexture"), Tex) && Tex)
				{
					Mid->SetTextureParameterValue(TEXT("ACETexture"), Tex);
				}
			}
			Mesh->SetMaterial(i, Mid);
		}
	};

	if (UACECharacterAppearanceComponent* Appearance = GetAppearance())
	{
		if (PartIndex != INDEX_NONE)
		{
			ConvertMesh(Cast<UMeshComponent>(Appearance->GetPartMesh(PartIndex)));
			return;
		}
		for (int32 i = 0; i < 64; ++i)
		{
			if (UMeshComponent* Part = Cast<UMeshComponent>(Appearance->GetPartMesh(i)))
			{
				ConvertMesh(Part);
			}
			else if (i > 0)
			{
				break;
			}
		}
	}
	if (AActor* Owner = GetOwner())
	{
		if (UStaticMeshComponent* StaticMesh = Owner->FindComponentByClass<UStaticMeshComponent>())
		{
			ConvertMesh(StaticMesh);
		}
		if (UProceduralMeshComponent* Proc = Owner->FindComponentByClass<UProceduralMeshComponent>())
		{
			ConvertMesh(Proc);
		}
	}
}

void UACEScriptComponent::RestoreDissolveMaterials(int32 PartIndex)
{
	USceneComponent* SelectedPart = nullptr;
	if (PartIndex != INDEX_NONE)
	{
		if (auto* Appearance = GetAppearance()) SelectedPart = Appearance->GetPartMesh(PartIndex);
		if (!SelectedPart) return;
	}
	for (int32 Index = DissolveMaterialBackup.Num() - 1; Index >= 0; --Index)
	{
		FDissolveMeshBackup& Backup = DissolveMaterialBackup[Index];
		if (SelectedPart && Backup.Mesh.Get() != SelectedPart) continue;
		if (UMeshComponent* Mesh = Backup.Mesh.Get())
		{
			const int32 N = FMath::Min(Mesh->GetNumMaterials(), Backup.Materials.Num());
			for (int32 i = 0; i < N; ++i)
			{
				if (Backup.Materials[i])
				{
					Mesh->SetMaterial(i, Backup.Materials[i]);
				}
			}
		}
		DissolveMaterialBackup.RemoveAtSwap(Index, 1, EAllowShrinking::No);
	}
	DissolveMaterialsRetained.Reset();
	for (const FDissolveMeshBackup& Backup : DissolveMaterialBackup)
		for (UMaterialInterface* Material : Backup.Materials)
			if (Material) DissolveMaterialsRetained.AddUnique(Material);
}

void UACEScriptComponent::ApplyMaterialScalar(int32 PartIndex, FName Parameter, float Value)
{
	auto ApplyToMesh = [&](UMeshComponent* Mesh)
	{
		if (!Mesh)
		{
			return;
		}
		const int32 Num = Mesh->GetNumMaterials();
		for (int32 i = 0; i < Num; ++i)
		{
			if (UMaterialInstanceDynamic* Mid = EnsurePrivateMaterialInstance(Mesh, i))
			{
				Mid->SetScalarParameterValue(Parameter, Value);
			}
		}
	};

	if (PartIndex != INDEX_NONE)
	{
		if (UACECharacterAppearanceComponent* Appearance = GetAppearance())
		{
			if (UMeshComponent* Part = Cast<UMeshComponent>(Appearance->GetPartMesh(PartIndex)))
			{
				ApplyToMesh(Part);
				return;
			}
		}
	}
	if (AActor* Owner = GetOwner())
	{
		TArray<UPrimitiveComponent*> Primitives;
		Owner->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (UMeshComponent* Mesh = Cast<UMeshComponent>(Primitive))
			{
				// Skip pooled particle meshes — they manage OpacityMul themselves.
				if (Mesh->ComponentTags.Num() > 0)
				{
					continue;
				}
				ApplyToMesh(Mesh);
			}
		}
	}
}

void UACEScriptComponent::ApplyMaterialVector(int32 PartIndex, FName Parameter, const FVector& Value)
{
	auto ApplyToMesh = [&](UMeshComponent* Mesh)
	{
		if (!Mesh)
		{
			return;
		}
		const int32 Num = Mesh->GetNumMaterials();
		for (int32 i = 0; i < Num; ++i)
		{
			if (UMaterialInstanceDynamic* Mid = EnsurePrivateMaterialInstance(Mesh, i))
			{
				Mid->SetVectorParameterValue(Parameter, FLinearColor(Value));
			}
		}
	};

	if (PartIndex != INDEX_NONE)
	{
		if (UACECharacterAppearanceComponent* Appearance = GetAppearance())
		{
			if (UMeshComponent* Part = Cast<UMeshComponent>(Appearance->GetPartMesh(PartIndex)))
			{
				ApplyToMesh(Part);
				return;
			}
		}
	}
	if (AActor* Owner = GetOwner())
	{
		TArray<UPrimitiveComponent*> Primitives;
		Owner->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (UMeshComponent* Mesh = Cast<UMeshComponent>(Primitive))
			{
				if (Mesh->ComponentTags.Num() > 0)
				{
					continue;
				}
				ApplyToMesh(Mesh);
			}
		}
	}
}

FTransform UACEScriptComponent::GetEmitterTransform(const FActiveEmitter& Emitter) const
{
	// Parent world, then local hook frame — matches Particle::Init parenting.
	// UE: child world = Parent * Relative. Strip scale: hook is already AC→UU cm.
	FTransform ParentWorld = Emitter.Parent.IsValid()
		? Emitter.Parent->GetComponentTransform()
		: (GetOwner() ? GetOwner()->GetActorTransform() : FTransform::Identity);
	ParentWorld.SetScale3D(FVector::OneVector);
	return Emitter.RelativeFrame * ParentWorld;
}

FQuat UACEScriptComponent::GetParticleDrawRotation(const FVector& Position, const FQuat& SimulationRotation, uint32 Mode,
	const FVector* ViewPosition) const
{
	if (Mode == 1) return SimulationRotation;
	FVector CameraLocation;
	if (!ViewPosition)
	{
		const UWorld* World = GetWorld();
		const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager : nullptr;
		if (!Camera) return SimulationRotation;
		CameraLocation = Camera->GetCameraLocation();
		ViewPosition = &CameraLocation;
	}
	// CPhysicsPart::viewer_heading is the ray from the viewer to the particle.
	// Using parallel camera forward makes off-center sprites tilt incorrectly.
	const FVector Heading = (Position - *ViewPosition).GetSafeNormal();
	if (Heading.IsNearlyZero()) return SimulationRotation;
	switch (Mode)
	{
	case 2: return FRotationMatrix::MakeFromYZ(Heading, FVector::UpVector).ToQuat();
	case 3: return FRotationMatrix::MakeFromXZ(SimulationRotation.GetAxisX(), Heading).ToQuat();
	case 4: return FRotationMatrix::MakeFromYX(SimulationRotation.GetAxisY(), -Heading).ToQuat();
	case 5: return FRotationMatrix::MakeFromZY(SimulationRotation.GetAxisZ(), Heading).ToQuat();
	default: return SimulationRotation;
	}
}

bool UACEScriptComponent::IsGroundPlaneDiscMesh(const UProceduralMeshComponent* Mesh)
{
	if (!Mesh || Mesh->GetNumSections() <= 0)
	{
		return false;
	}
	bool bGround = false, bVert = false, bCube = false;
	ClassifyParticleExtent(Mesh->GetLocalBounds().BoxExtent, bGround, bVert, bCube);
	return bGround;
}

bool UACEScriptComponent::IsVerticalSpriteMesh(const UProceduralMeshComponent* Mesh)
{
	if (!Mesh || Mesh->GetNumSections() <= 0)
	{
		return false;
	}
	bool bGround = false, bVert = false, bCube = false;
	ClassifyParticleExtent(Mesh->GetLocalBounds().BoxExtent, bGround, bVert, bCube);
	return bVert;
}

bool UACEScriptComponent::IsSmallHwSpriteCube(const UProceduralMeshComponent* Mesh)
{
	if (!Mesh || Mesh->GetNumSections() <= 0)
	{
		return false;
	}
	bool bGround = false, bVert = false, bCube = false;
	ClassifyParticleExtent(Mesh->GetLocalBounds().BoxExtent, bGround, bVert, bCube);
	return bCube;
}

/** Retail Particle::Init StartFrame: part world frame, or object/MeshRoot for partIdx==-1. */
FTransform UACEScriptComponent::GetParticleStartFrame(const FActiveEmitter& Emitter, bool bKeepScale) const
{
    if (!ImpactFrames.IsEmpty())
    {
        FTransform Frame = ImpactFrames.Contains(Emitter.PartIndex) ? ImpactFrames[Emitter.PartIndex] : ImpactFrames[-1];
        if (!bKeepScale) Frame.SetScale3D(FVector::OneVector);
        return Frame;
    }
	if (Emitter.bVRHandFeedback && Emitter.Parent.IsValid())
	{
		FTransform Frame = Emitter.Parent->GetComponentTransform();
		if (!bKeepScale) Frame.SetScale3D(FVector::OneVector);
		return Frame;
	}
	if (UACECharacterAppearanceComponent* Appearance = GetAppearance())
	{
		USceneComponent* MeshRoot = Appearance->GetMeshRoot();
		const FTransform RootWorld = MeshRoot
			? MeshRoot->GetComponentTransform()
			: (GetOwner() ? GetOwner()->GetActorTransform() : FTransform::Identity);

		const AACEWorldEntityActor* Ent = Cast<AACEWorldEntityActor>(GetOwner());
		const bool bCreatureLike = Ent == nullptr
			|| Ent->bIsPlayer
			|| (Ent->ItemType & ACEItemType::Creature) != 0;

		if (Emitter.PartIndex >= 0)
		{
			if (USceneComponent* Part = Appearance->GetPartMesh(Emitter.PartIndex))
			{
				FTransform World = Part->GetComponentTransform();
				FTransform Bind;
				// Identity-relative HUMAN parts (anim not applied yet) pin HealthUp to the
				// MeshRoot at the feet. Prefer the Setup bind (human part 9 ≈ chest Z=1.19).
				if (bCreatureLike
					&& Appearance->GetPartBindTransform(Emitter.PartIndex, Bind)
					&& Part->GetRelativeLocation().SizeSquared() < 4.f
					&& Bind.GetLocation().SizeSquared() > 16.f)
				{
					if (!bKeepScale)
					{
						Bind.SetScale3D(FVector::OneVector);
					}
					World = Bind * RootWorld;
				}
				if (!bKeepScale)
				{
					World.SetScale3D(FVector::OneVector);
				}
				return World;
			}
			FTransform PartRel;
			if (Appearance->GetPartCurrentTransform(Emitter.PartIndex, PartRel)
				|| Appearance->GetPartBindTransform(Emitter.PartIndex, PartRel))
			{
				if (!bKeepScale)
				{
					PartRel.SetScale3D(FVector::OneVector);
				}
				FTransform World = PartRel * RootWorld;
				if (!bKeepScale)
				{
					World.SetScale3D(FVector::OneVector);
				}
				return World;
			}
			// Missing body part: torso estimate. Weapons use the object root (retail).
			FTransform World = RootWorld;
			if (bCreatureLike)
			{
				World.AddToTranslation(FVector(0.f, 0.f, WorldScale * 1.2f));
			}
			if (!bKeepScale)
			{
				World.SetScale3D(FVector::OneVector);
			}
			return World;
		}
		FTransform World = RootWorld;
		if (!bKeepScale)
		{
			World.SetScale3D(FVector::OneVector);
		}
		if (!bCreatureLike && Ent)
		{
			// Object root = actor origin. MeshRoot FeetOffsetZ would drop FX below the mesh.
			if (GetOwner() && GetOwner()->GetRootComponent())
			{
				World = GetOwner()->GetRootComponent()->GetComponentTransform();
				if (!bKeepScale)
				{
					World.SetScale3D(FVector::OneVector);
				}
			}
		}
		return World;
	}
	if (Emitter.Parent.IsValid())
	{
		FTransform World = Emitter.Parent->GetComponentTransform();
		if (!bKeepScale)
		{
			World.SetScale3D(FVector::OneVector);
		}
		return World;
	}
	return GetOwner() ? GetOwner()->GetActorTransform() : FTransform::Identity;
}

bool UACEScriptComponent::ShouldFollowOwner() const
{
	if (bEnvironmentWeather)
	{
		return true;
	}
	if (const AACEWorldEntityActor* Ent = Cast<AACEWorldEntityActor>(GetOwner()))
	{
		if (Ent->bAttachedToParent || Ent->GetParentGuid() != 0 || Ent->IsAttachedToParent())
		{
			return true;
		}
		return Ent->bIsPlayer || (Ent->ItemType & ACEItemType::Creature) != 0;
	}
	return GetAppearance() != nullptr;
}

bool UACEScriptComponent::IsWieldedItemFx() const
{
	const AACEWorldEntityActor* Ent = Cast<AACEWorldEntityActor>(GetOwner());
	if (!Ent || Ent->bIsPlayer || (Ent->ItemType & ACEItemType::Creature) != 0)
	{
		return false;
	}
	return Ent->bAttachedToParent || Ent->GetParentGuid() != 0 || Ent->IsAttachedToParent();
}

FVector UACEScriptComponent::ResolveHookOffsetLocal(const FActiveEmitter& Emitter) const
{
	// Weenie PhysicsScript only selects the 0x33 script. Spawn origin is Setup
	// CreateParticle Frame.Origin in the part/PhysicsObj frame (ACE.Server Particle::Init).
	// Acid Staff Setup 0x020003CE DefaultScript 0x3300001C: part 0, origin (0,1,0) ACE —
	// gfx 0x0100029F AABB Y=-0.01..1.11, so that hook is the orb, not an AABB guess.
	FVector HookUe = Emitter.HookLocalUe;
	if (HookUe.IsNearlyZero(0.05f))
	{
		HookUe = Emitter.HookFrame.GetLocation();
	}
	return HookUe;
}

void UACEScriptComponent::CreateEmitter(
	const FACEDatAnimationHook& Hook, bool bBlocking, float Intensity, uint32 SourcePlayScript)
{
	// ParticleManager::CreateParticleEmitter removes the occupied slot before
	// SetInfo/SetParenting can fail. Blocking creates leave that slot untouched.
	const uint32 SlotId = Hook.SecondaryId;
	if (SlotId != 0)
	{
		const uint32 ExistingId = ResolveEmitterSlot(SlotId, SourcePlayScript);
		if (bBlocking && ActiveEmitters.ContainsByPredicate([ExistingId](const FActiveEmitter& E)
			{ return E.InstanceId == ExistingId; })) return;
		StopEmitter(ExistingId, true);
	}
	UACEDatSubsystem* Dat = GetDat();
	const FACEDatParticleEmitterInfo* Source = Dat ? Dat->GetParticleEmitterInfo(Hook.Id) : nullptr;
	if (!Source)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEScript: CreateEmitter missing PE 0x%08X setup=0x%08X"),
			Hook.Id, SetupId);
		return;
	}

	// Retail ParticleEmitter.SetInfo requires HWGfxObjID != 0 and never draws GfxObjId.
	// Falling back to software gfx (Town Network 0x010016C9 / surface 0x080004B9) painted
	// the horizontal white square through the portal rings.
	const uint32 DrawId = Source->HwGfxObjId;
	if (DrawId == 0 || DrawId == 0x010001ECu || DrawId == 0x0100168Bu)
	{
		return;
	}

	auto SanitizeInfo = [](FACEDatParticleEmitterInfo& Info)
	{
		Info.MaxParticles = FMath::Clamp(Info.MaxParticles, 0, MaxParticlesPerEmitter);
		Info.InitialParticles = FMath::Clamp(Info.InitialParticles, 0, Info.MaxParticles);
		// The simultaneous render budget is independent of the emitter's total
		// births or duration. Retail DAT includes 800-birth bursts and day-long
		// ambience; clamping either to a buffer limit changes their animation.
		Info.TotalParticles = FMath::Max(0, Info.TotalParticles);
		Info.Birthrate = FMath::IsFinite(Info.Birthrate) ? FMath::Max(0.0, Info.Birthrate) : 0.0;
		Info.Lifespan = FMath::IsFinite(Info.Lifespan) ? FMath::Max(0.0, Info.Lifespan) : 0.0;
		Info.LifespanRand = FMath::IsFinite(Info.LifespanRand) ? FMath::Max(0.0, Info.LifespanRand) : 0.0;
	};

	// Retail ParticleManager keys emitters by the hook's per-object slot, not
	// the emitter-info DID. A normal create destroys the old slot immediately;
	// a blocking create leaves an occupied slot alone. Slot zero always allocates.
	const uint32 EmitterInfoId = Hook.Id;
	if (ActiveEmitters.Num() >= MaxEmitters) return;
	const FQuat4f HookR = Hook.Frame.GetRotation();
	const FVector3f HookT = Hook.Frame.GetTranslation();
	// ParticleEmitter::SetParenting copies the hook frame unchanged. The part's
	// world origin already includes model scale; its local emitter offset does not.
	// Scaling both put Empyrean eye glows above the authored eye sockets.
	const FVector NewHookLocal = FACEPosition::AceVectorToUnreal(FVector(HookT), WorldScale);
	const FTransform NewHookFrame(
		FACEPosition::AceQuatToUnreal(HookR.W, FVector(HookR.X, HookR.Y, HookR.Z)),
		NewHookLocal, FVector::OneVector);
	const int32 NewPartIndex = (Hook.PartIndex == 0xFFFFFFFFu) ? -1 : static_cast<int32>(Hook.PartIndex);
	const uint32 NewInstanceId = SlotId != 0 && AetheriaSlotScope(SourcePlayScript) == 0 ? SlotId : NextEmitterId++;

	FActiveEmitter Emitter;
	Emitter.InstanceId = NewInstanceId;
	Emitter.AuthoredEmitterId = EmitterInfoId;
	Emitter.SlotId = SlotId;
	Emitter.SourcePlayScript = SourcePlayScript;
	Emitter.RenderSortPriority = bEnvironmentWeather ? 2500 : 400;
	if (bEnvironmentWeather)
		if (const int32* Order = EnvironmentScriptDrawOrder.Find(SourcePlayScript)) Emitter.RenderSortPriority = *Order;
	Emitter.Info = *Source;
	Emitter.Info.GfxObjId = DrawId;
	if (const auto* Shape = Dat->GetOrBuildSetupMesh(DrawId, WorldScale))
		if (!Shape->Parts.IsEmpty())
		{
			Emitter.DrawMode = Shape->Parts[0].DrawMode;
			Emitter.MaxDegradeDistance = Shape->Parts[0].MaxDegradeDistance;
		}
	SanitizeInfo(Emitter.Info);
	Emitter.bBlocking = bBlocking;
	// PropertyFloat.PhysicsScriptIntensity — PeTable tier + Scale/Transparent/UV tween speed.
	// Do not rewrite Birthrate/particle A,B,C: those are authored for the intensity-selected script.
	Emitter.Intensity = FMath::Clamp(SafeFloat(Intensity, 1.f), 0.01f, 100.f);
	Emitter.EmissiveBoost = 1.f;
	if (Dat && bAllowInferredParticleLights && !bEnvironmentWeather && !IsBloodSplatter(SourcePlayScript))
	{
		if (!Dat->TryEstimateGfxLight(DrawId, Emitter.LightColor, Emitter.LightLum))
			Emitter.LightLum = 0.f;
	}

	Emitter.HookFrame = NewHookFrame;
	Emitter.HookLocalUe = NewHookLocal;
	// Retail partIdx==-1 (wire 0xFFFFFFFF) uses the object root frame; part 0 is Setup part 0.
	Emitter.PartIndex = NewPartIndex;
	ResolveEmitterParent(Emitter);
	Emitter.LastOrigin = GetEmitterTransform(Emitter).GetLocation();
	Emitter.LastEmissionOrigin = Emitter.LastOrigin;
	ActiveEmitters.Add(MoveTemp(Emitter));
	WakeEffectTick();
	FActiveEmitter& Added = ActiveEmitters.Last();
	FVector ViewLocation;
	if (GetParticleViewLocation(ViewLocation) && ShouldDegradeEmitter(Added, ViewLocation))
	{
		// InitEnd still creates the initial particles, with their original birth
		// frame and lifetime. Defer only GPU resources until they become visible.
		Added.bDegraded = true;
	}
	int32 Born = 0;
	for (int32 i = 0; i < Added.Info.InitialParticles; ++i)
	{
		const int32 Before = Added.Particles.Num();
		SpawnParticle(Added);
		if (Added.Particles.Num() > Before)
		{
			++Born;
		}
	}
	if (Born <= 0 && Added.Info.InitialParticles > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ACEScript: CreateEmitter PE 0x%08X gfx=0x%08X spawned 0/%d particles (ApplyParticleGfx failed?)"),
			EmitterInfoId, DrawId, Added.Info.InitialParticles);
	}
}

UProceduralMeshComponent* UACEScriptComponent::AcquireParticleMesh(uint32 GfxObjId, bool bBlocking, int32 SortPriority)
{
	UProceduralMeshComponent* Mesh = nullptr;
	const bool bBackgroundSky = bEnvironmentWeather && SortPriority < 0;
	const FName BuiltTag(*FString::Printf(TEXT("ACEParticleGfxN12_%08X_%d"), GfxObjId, bBackgroundSky));
	// Interleaved emitters (weapon glow and drips, for example) retire different
	// meshes into the same pool. Reuse matching geometry before rebuilding one.
	for (int32 Index = ParticlePool.Num() - 1; Index >= 0; --Index)
	{
		auto* Candidate = ParticlePool[Index].Get();
		if (IsValid(Candidate) && Candidate->IsRegistered()
			&& !Candidate->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Candidate->ComponentTags.Contains(BuiltTag) && Candidate->GetNumSections() > 0)
		{
			Mesh = Candidate;
			ParticlePool.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			break;
		}
	}
	while (ParticlePool.Num() > 0 && !Mesh)
	{
		UProceduralMeshComponent* Candidate = ParticlePool.Pop(EAllowShrinking::No).Get();
		if (IsValid(Candidate) && Candidate->IsRegistered()
			&& !Candidate->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			Mesh = Candidate;
		}
	}
	AActor* Owner = GetOwner();
	if (!Mesh && IsValid(Owner))
	{
		Mesh = NewObject<UProceduralMeshComponent>(Owner);
		Owner->AddInstanceComponent(Mesh);
		Mesh->RegisterComponent();
		Mesh->bUseAsyncCooking = false;
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCastShadow(false);
		Mesh->SetReceivesDecals(false);
	}
	if (!IsValid(Mesh))
	{
		return nullptr;
	}
	Mesh->SetTranslucentSortPriority(SortPriority);

	// Free-floating particles must not inherit actor scale/attachment — retail PhysicsParts
	// apply gfxobj_scale in world space after the object frame has already placed them.
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	Mesh->SetUsingAbsoluteLocation(true);
	Mesh->SetUsingAbsoluteRotation(true);
	Mesh->SetUsingAbsoluteScale(true);
	Mesh->SetVisibility(!bCosmeticParticlesHidden);
	Mesh->SetHiddenInGame(false);
	// Owner hide (portal-space loading) would otherwise suppress all child primitives.
	Mesh->SetOwnerNoSee(false);
	Mesh->bVisibleInSceneCaptureOnly = false;
	// The legacy blocking hook belongs to AC physics. Cosmetic particles must not
	// intercept world selection rays, including particles owned by a selectable caster.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->bUseAsyncCooking = false;

	const bool bAlreadyBuilt = Mesh->ComponentTags.Contains(BuiltTag) && Mesh->GetNumSections() > 0;
	if (!bAlreadyBuilt)
	{
		UACEDatSubsystem* Dat = GetDat();
		// Particle GfxObjs (0x01) must get real DAT textures — never VertexColor slabs
		// (those flash white when OpacityMul / additive paths fight).
		if (!Dat || !Dat->ApplyParticleGfxToProceduralMesh(Mesh, static_cast<int32>(GfxObjId), WorldScale, false))
		{
			ReleaseParticleMesh(Mesh);
			return nullptr;
		}
		if (Mesh->GetNumSections() <= 0)
		{
			ReleaseParticleMesh(Mesh);
			return nullptr;
		}
		Mesh->ComponentTags.Reset();
		Mesh->ComponentTags.Add(BuiltTag);
		// Private MIDs so per-particle OpacityMul (1 - AC translucency) does not mutate caches.
		for (int32 MaterialIndex = 0; MaterialIndex < Mesh->GetNumMaterials(); ++MaterialIndex)
		{
			if (bBackgroundSky)
			{
				UMaterialInterface* SourceMaterial = Mesh->GetMaterial(MaterialIndex);
				auto* SkyMid=Dat->CreateSkyParticleMaterial(SourceMaterial,Mesh);
				if (!SkyMid) { ReleaseParticleMesh(Mesh); return nullptr; }
                float AuthoredLuminosity=0.f;
                if (SourceMaterial) SourceMaterial->GetScalarParameterValue(TEXT("AuthoredLuminosity"),AuthoredLuminosity);
                SkyMid->SetScalarParameterValue(TEXT("EmissiveStrength"),AuthoredLuminosity);
                SkyMid->SetVectorParameterValue(TEXT("SkyAmbient"),EnvironmentAmbient);
                SkyMid->SetVectorParameterValue(TEXT("SkySunColor"),EnvironmentSunColor);
                SkyMid->SetVectorParameterValue(TEXT("SkySunDirection"),EnvironmentSunDirection);
                EnvironmentSkyMaterials.Add(SkyMid);
				Mesh->SetMaterial(MaterialIndex, SkyMid);
			}
			if (UMaterialInterface* Base = Mesh->GetMaterial(MaterialIndex))
			{
				// Engine fallback mats have no OpacityMul — hide rather than flash solid colors.
				const FString Path = Base->GetPathName();
				if (Path.Contains(TEXT("VertexColorMaterial"))
					|| Path.Contains(TEXT("WorldGridMaterial"))
					|| Path.Contains(TEXT("DefaultMaterial")))
				{
					ReleaseParticleMesh(Mesh);
					return nullptr;
				}
			}
			if (UMaterialInstanceDynamic* Mid = EnsurePrivateMaterialInstance(Mesh, MaterialIndex))
			{
				Mid->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
				// Keep particle-mat EmissiveStrength (luminosity) — do not crush to 1.
			}
		}
	}
	return Mesh;
}

void UACEScriptComponent::ReleaseParticleMesh(UProceduralMeshComponent* Mesh)
{
	if (!IsValid(Mesh))
	{
		return;
	}
	// A short-lived glow commonly reuses this exact mesh later in this update.
	// Delay hiding until reuse is resolved to avoid rebuilding its render proxy.
	if (!bUpdatingParticleVisuals)
	{
		Mesh->SetVisibility(false);
		Mesh->SetHiddenInGame(true);
	}
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	if (ParticlePool.Num() >= MaxPooledParticleMeshes)
	{
		Mesh->DestroyComponent();
		return;
	}
	ParticlePool.Add(Mesh);
}

bool UACEScriptComponent::EnsureParticleBatch(FActiveEmitter& Emitter)
{
	if (IsValid(Emitter.Batch)) return true;
	// Sky/weather preserves authored before/after-world submission order.
	// Spell particles use a dedicated shader with an actual per-instance fade.
	if (bEnvironmentWeather || Emitter.Info.MaxParticles <= 1 || CVarParticleBatches.GetValueOnGameThread() == 0) return false;
	auto* Dat = GetDat(); auto* Owner = GetOwner();
	if (!Dat || !Owner) return false;
	auto* Batch = NewObject<UACEParticleBatchComponent>(Owner);
	Owner->AddInstanceComponent(Batch);
	// Object-wide transparency/diffuse hooks belong to the model. Particles
	// carry their own authored opacity, just like the pooled mesh path.
	Batch->ComponentTags.Add(TEXT("ACEParticle"));
	Batch->SetMobility(EComponentMobility::Movable);
	Batch->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Batch->SetGenerateOverlapEvents(false); Batch->SetCastShadow(false);
	Batch->SetReceivesDecals(false); Batch->SetCanEverAffectNavigation(false);
	// Store transforms near the emitter, avoiding centimeter precision loss at
	// Dereth's large world coordinates. Free particles stay in world space.
	Batch->SetWorldLocation(GetEmitterTransform(Emitter).GetLocation());
	if (!Dat->ApplyParticleGfxToProceduralMesh(Batch, Emitter.Info.GfxObjId, WorldScale, false))
	{ Batch->DestroyComponent(); return false; }
	for (int32 I = 0; I < Batch->GetNumMaterials(); ++I)
	{
		auto* Source = Batch->GetMaterial(I);
		if (!Source) { Batch->DestroyComponent(); return false; }
		auto* Base = Dat->EnsureAceBatchedParticleMaterialBase(Source->GetBlendMode() == BLEND_Additive);
		if (!Base) { Batch->DestroyComponent(); return false; }
		auto* Mid = UMaterialInstanceDynamic::Create(Base, Batch);
		Mid->CopyMaterialUniformParameters(Source);
		Mid->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
		Mid->SetScalarParameterValue(TEXT("ParticleBrightness"), 1.f);
		Batch->SetMaterial(I, Mid);
	}
	Batch->SetTranslucentSortPriority(Emitter.RenderSortPriority);
	Batch->InitializeParticles(Emitter.Info.MaxParticles);
	Batch->SetVisibility(!bCosmeticParticlesHidden && !Emitter.bDegraded);
	Batch->RegisterComponent();
	Emitter.Batch = Batch; Emitter.bInstanced = true;
	// DrawMode came from GfxObjInfo. Do not infer billboarding from mesh bounds.
	return true;
}

void UACEScriptComponent::ReleaseParticleVisual(FActiveEmitter& Emitter, FActiveParticle& Particle)
{
	if (Particle.InstanceIndex >= 0 && Emitter.Batch)
	{
		const int32 Idx = Particle.InstanceIndex;
		const int32 Last = Emitter.Batch->GetParticleCount() - 1;
		Emitter.Batch->RemoveParticle(Idx);
		if (Idx != Last)
		{
			for (FActiveParticle& Other : Emitter.Particles)
			{
				if (&Other != &Particle && Other.InstanceIndex == Last)
				{
					Other.InstanceIndex = Idx;
					break;
				}
			}
		}
		Particle.InstanceIndex = INDEX_NONE;
	}
	ReleaseParticleMesh(Particle.Mesh.Get());
	Particle.Mesh = nullptr;
}

void UACEScriptComponent::ApplyParticleVisual(FActiveEmitter& Emitter, FActiveParticle& Particle,
	const FQuat& Rotation, const FVector& Scale, float Opacity)
{
	if (Emitter.bDegraded) return;
	if (Particle.InstanceIndex < 0 && !Particle.Mesh.IsValid())
	{
		if (EnsureParticleBatch(Emitter))
		{
			Particle.InstanceIndex = Emitter.Batch->AddParticle(FTransform(Rotation,Particle.Position,Scale),Opacity);
			return;
		}
		Particle.Mesh = AcquireParticleMesh(Emitter.Info.GfxObjId,Emitter.bBlocking,Emitter.RenderSortPriority);
		if (auto* Mesh=Particle.Mesh.Get())
			for (int32 I=0; I<Mesh->GetNumMaterials(); ++I)
				if (auto* Mid=EnsurePrivateMaterialInstance(Mesh,I)) Mid->SetScalarParameterValue(TEXT("ParticleBrightness"),1.f);
	}
	if (Particle.InstanceIndex >= 0 && Emitter.Batch)
	{
		const FTransform Xform(Rotation, Particle.Position, Scale);
		Emitter.Batch->SetParticleVisual(Particle.InstanceIndex, Xform, Opacity);
		return;
	}
	UProceduralMeshComponent* Mesh = Particle.Mesh.Get();
	if (!Mesh)
	{
		return;
	}
	// One transform update per particle instead of three scene/attachment updates.
	Mesh->SetWorldTransform(FTransform(Rotation, Particle.Position, Scale));
		for (int32 Mi = 0; Mi < Mesh->GetNumMaterials(); ++Mi)
		{
			if (UMaterialInstanceDynamic* Mid = EnsurePrivateMaterialInstance(Mesh, Mi))
			{
				Mid->SetScalarParameterValue(TEXT("OpacityMul"), Opacity);
			}
		}
	Mesh->SetVisibility(!bCosmeticParticlesHidden && !Emitter.bDegraded);
	Mesh->SetHiddenInGame(false);
}

void UACEScriptComponent::SpawnParticle(FActiveEmitter& Emitter)
{
	if (Emitter.Particles.Num() >= Emitter.Info.MaxParticles
		|| (Emitter.Info.TotalParticles > 0 && Emitter.TotalBorn >= Emitter.Info.TotalParticles))
	{
		return;
	}
	auto Range = [this](float Min, float Max)
	{
		Min = SafeFloat(Min);
		Max = SafeFloat(Max);
		return Random.FRandRange(FMath::Min(Min, Max), FMath::Max(Min, Max));
	};
	// Spatial vectors are AC units → Unreal cm. Angular rates stay in AC radians/sec (axis flip only).
	auto Spatial = [&](const FVector3f& V, float Min, float Max)
	{
		return FACEPosition::AceVectorToUnreal(FVector(V) * Range(Min, Max), WorldScale);
	};
	auto Angular = [&](const FVector3f& V, float Min, float Max)
	{
		// Angular velocity is an axial vector under the X reflection.
		return -FACEPosition::AceVectorToUnreal(FVector(V) * Range(Min, Max), 1.f);
	};

	FActiveParticle Particle;
	// Retail Particle::Init StartFrame is the part/object WORLD frame (NOT the hook frame).
	// Offset = StartFrame.LocalToGlobalVec(hook.Origin + randomOffset) — world-space vector.
	const FTransform StartFrameWorld = GetParticleStartFrame(Emitter);
	const FQuat StartFrameRot = StartFrameWorld.GetRotation();
	Particle.Type = FMath::Clamp(Emitter.Info.ParticleType, 1, 12);
	Particle.bParentLocal = Emitter.Info.IsParentLocal != 0 || bEnvironmentWeather || Emitter.bVRHandFeedback;
	Particle.StartOrigin = StartFrameWorld.GetLocation();
	Particle.StartRotation = StartFrameRot;
	Particle.bWieldedGfx = IsWieldedItemFx();
	// OffsetDir / random disc are in StartFrame (part) ACE space — match ACE GetRandomOffset.
	// Project out of OffsetDir so fountain (0,0,1) births on a horizontal disc, not a sphere.
	// GetRandomOffset uses the authored direction directly, without normalizing it.
	const FVector OffsetDirAce(Emitter.Info.OffsetDir);
	FVector RandomAngle(
		Random.FRandRange(-1.f, 1.f),
		Random.FRandRange(-1.f, 1.f),
		Random.FRandRange(-1.f, 1.f));
	RandomAngle -= OffsetDirAce * FVector::DotProduct(OffsetDirAce, RandomAngle);
	FVector RandomLocal = FVector::ZeroVector;
	if (RandomAngle.Normalize())
	{
		// Retail: |offset| = MinOffset + (MaxOffset - MinOffset) * rng  (ACE.Server dropped Min).
		const float MinOff = SafeFloat(Emitter.Info.MinOffset);
		const float MaxOff = SafeFloat(Emitter.Info.MaxOffset);
		const float Mag = MinOff + (MaxOff - MinOff) * Random.FRand();
		RandomLocal = FACEPosition::AceVectorToUnreal(RandomAngle * Mag, WorldScale);
	}
	// Normalize fail → Zero (not VRand). Zero OffsetDir still yields a sphere when the
	// unprojected rng normalizes successfully (retail).
	// Hook translation only (already AC→UE in CreateEmitter). Do NOT bake part origin into Offset.
	const FVector HookLocal = ResolveHookOffsetLocal(Emitter);
	const FVector LocalOff = HookLocal + RandomLocal;
	Particle.Offset = StartFrameRot.RotateVector(LocalOff);

	FVector A = Spatial(Emitter.Info.A, Emitter.Info.MinA, Emitter.Info.MaxA);
	FVector B = Spatial(Emitter.Info.B, Emitter.Info.MinB, Emitter.Info.MaxB);
	FVector C = Spatial(Emitter.Info.C, Emitter.Info.MinC, Emitter.Info.MaxC);

	// Type names (LV/GV/LA/GA/LR/GR) are the retail Init contract. ACE.Server Particle::Init
	// only assigned the "new" field per case (decompile gap) which zeroed local velocity on
	// parabolic types — fountains/bolts sat still or rose on the wrong axis.
	const FQuat& BirthRotation = StartFrameRot;
	switch (Particle.Type)
	{
	case 1: // Still
		A = FVector::ZeroVector;
		B = FVector::ZeroVector;
		C = FVector::ZeroVector;
		break;
	case 2: // LocalVelocity
		A = BirthRotation.RotateVector(A);
		B = FVector::ZeroVector;
		C = FVector::ZeroVector;
		break;
	case 3: // ParabolicLVGA — local velocity + global acceleration
		A = BirthRotation.RotateVector(A);
		C = FVector::ZeroVector;
		break;
	case 4: // ParabolicLVGAGR — local vel + global accel + global rotation
		A = BirthRotation.RotateVector(A);
		C = Angular(Emitter.Info.C, Emitter.Info.MinC, Emitter.Info.MaxC);
		break;
	case 5: // Swarm — A = L2G(a); B = raw rates (rad/s); C = raw AC orbit radii
		A = BirthRotation.RotateVector(A);
		B = FVector(Emitter.Info.B) * Range(Emitter.Info.MinB, Emitter.Info.MaxB);
		C = FVector(Emitter.Info.C) * Range(Emitter.Info.MinC, Emitter.Info.MaxC);
		break;
	case 6: // Explode — sphere dir in AC, then RH→LH. A.X is a scalar (do not X-flip it).
	{
		const float MagA = Range(Emitter.Info.MinA, Emitter.Info.MaxA);
		const float MagC = Range(Emitter.Info.MinC, Emitter.Info.MaxC);
		A = FVector(Emitter.Info.A.X * MagA, 0.f, Emitter.Info.A.Z * MagA * WorldScale);
		B = Spatial(Emitter.Info.B, Emitter.Info.MinB, Emitter.Info.MaxB);
		const float Ra = Random.FRandRange(-PI, PI);
		const float Po = Random.FRandRange(-PI, PI);
		const float Rb = FMath::Cos(Po);
		const FVector Cac(
			FMath::Cos(Ra) * Emitter.Info.C.X * MagC * Rb,
			FMath::Sin(Ra) * Emitter.Info.C.Y * MagC * Rb,
			FMath::Sin(Po) * Emitter.Info.C.Z * MagC);
		C = FACEPosition::AceVectorToUnreal(Cac.GetSafeNormal(), WorldScale);
		if (C.SizeSquared() < KINDA_SMALL_NUMBER)
		{
			C = FVector::ZeroVector;
		}
		break;
	}
	case 7: // Implode — scale world offset by AC C (not cm), then Cos(A.X*t)*C
	{
		const FVector Cac = FVector(Emitter.Info.C) * Range(Emitter.Info.MinC, Emitter.Info.MaxC);
		A = FVector(Emitter.Info.A.X * Range(Emitter.Info.MinA, Emitter.Info.MaxA), 0.f, 0.f);
		B = Spatial(Emitter.Info.B, Emitter.Info.MinB, Emitter.Info.MaxB);
		Particle.Offset = FVector(
			Particle.Offset.X * Cac.X, Particle.Offset.Y * Cac.Y, Particle.Offset.Z * Cac.Z);
		C = Particle.Offset;
		break;
	}
	case 8: // ParabolicLVLA — local velocity + local acceleration
		A = BirthRotation.RotateVector(A);
		B = BirthRotation.RotateVector(B);
		C = FVector::ZeroVector;
		break;
	case 9: // ParabolicLVLALR — local vel + local accel + local spin
		A = BirthRotation.RotateVector(A);
		B = BirthRotation.RotateVector(B);
		C = BirthRotation.RotateVector(Angular(Emitter.Info.C, Emitter.Info.MinC, Emitter.Info.MaxC));
		break;
	case 10: // ParabolicGVGA — global velocity + global acceleration
		C = FVector::ZeroVector;
		break;
	case 11: // ParabolicGVGAGR — global vel + global accel + global rotation
		C = Angular(Emitter.Info.C, Emitter.Info.MinC, Emitter.Info.MaxC);
		break;
	case 12: // GlobalVelocity
		B = FVector::ZeroVector;
		C = FVector::ZeroVector;
		break;
	default:
		break;
	}

	Particle.A = A;
	Particle.B = B;
	Particle.C = C;
	Particle.Position = Particle.StartOrigin + Particle.Offset;
	// Retail Init evaluates Update at t=0, including swarm/implode displacement.
	if (Particle.Type==5) Particle.Position+=FACEPosition::AceVectorToUnreal(FVector(C.X,0,C.Z),WorldScale);
	if (Particle.Type==7) Particle.Position+=C;
	Particle.Life = FMath::Max(0.f, static_cast<float>(Emitter.Info.Lifespan)
		+ Range(-static_cast<float>(Emitter.Info.LifespanRand), static_cast<float>(Emitter.Info.LifespanRand)));
	// ParticleEmitter::EmitParticle calls ParticleEmitterInfo::GetRandom* before
	// Particle::Init: independent endpoint rolls with retail's 0.1..10 scale clamp.
	const float ScaleRand = FMath::Abs(SafeFloat(Emitter.Info.ScaleRand));
	const float TransRand = FMath::Abs(SafeFloat(Emitter.Info.TransRand));
	Particle.StartScale = FMath::Clamp(SafeFloat(Emitter.Info.StartScale, 1.f) + Range(-ScaleRand, ScaleRand), .1f, 10.f);
	Particle.FinalScale = FMath::Clamp(SafeFloat(Emitter.Info.FinalScale, 1.f) + Range(-ScaleRand, ScaleRand), .1f, 10.f);
	Particle.StartTrans = FMath::Clamp(SafeFloat(Emitter.Info.StartTrans) + Range(-TransRand, TransRand), 0.f, 1.f);
	Particle.FinalTrans = FMath::Clamp(SafeFloat(Emitter.Info.FinalTrans) + Range(-TransRand, TransRand), 0.f, 1.f);
	Particle.InstanceIndex = INDEX_NONE;

	Particle.StartRotation = BirthRotation;
	Particle.DrawMode = Emitter.DrawMode;
	Particle.bHoldStill = false;
	const FQuat MeshFacing = GetParticleDrawRotation(Particle.Position, BirthRotation, Particle.DrawMode);

	// Retail evaluates Update at birth, including the final endpoints when life=0.
	const float BirthScale = Particle.Life > 0.f ? Particle.StartScale : Particle.FinalScale;
	const float BirthTrans = Particle.Life > 0.f ? Particle.StartTrans : Particle.FinalTrans;
	const float Opacity = FMath::Clamp(
		(1.f - BirthTrans) * (IsRetailPortalFxSetup(SetupId) ? 1.f - ObjectTranslucency : 1.f), 0.f, 1.f);
	ApplyParticleVisual(Emitter, Particle, MeshFacing, FVector(BirthScale), Opacity);
	Emitter.Particles.Add(MoveTemp(Particle));
	++Emitter.TotalBorn;
	Emitter.LastEmissionOrigin = GetEmitterTransform(Emitter).GetLocation();
	Emitter.TimeSinceEmit = 0.f;
}

void UACEScriptComponent::TickParticleSimulation(float DeltaTime)
{
	ACE_PROFILE_SCOPE(EffectSimulation);
	if (ActiveEmitters.IsEmpty())
	{
		ParticleTimeSinceUpdate = 0.0;
		// DestroyParticle can remove the last emitter during TickScripts. Retire
		// its light before the idle component disables ticking.
		if (auto* Lights = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
			Lights->RemoveParticles(this);
		return;
	}
	// CPhysicsObj::animate_static_object/update_position advance ParticleManager
	// only at MIN_QUANTUM. Short-lived glows (3200075A: .03 s) are replaced on
	// that tick; rendering their intermediate shrink at 60+ Hz creates a strobe.
	// Retail records "now" after updating, without a catch-up loop or remainder.
	ParticleTimeSinceUpdate += FMath::Max(0.f, DeltaTime);
	if (ParticleTimeSinceUpdate >= 1.0 / 30.0)
	{
		TickEmitters(static_cast<float>(ParticleTimeSinceUpdate));
		TickParticleLights();
		ParticleTimeSinceUpdate = 0.0;
	}
	else
	{
		// Retail refreshes calc_draw_frame during rendering. UE's body/camera
		// advance every frame too, so neither facing nor attachment may wait for
		// the next emission/lifetime update (especially visible on slopes).
		TickParticlePresentation(static_cast<float>(ParticleTimeSinceUpdate));
		TickParticleLights(true);
	}
}

bool UACEScriptComponent::GetParticleViewLocation(FVector& OutLocation) const
{
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager) return false;
	FRotator Rotation;
	PC->GetPlayerViewPoint(OutLocation, Rotation);
	return true;
}

bool UACEScriptComponent::ShouldDegradeEmitter(const FActiveEmitter& Emitter, const FVector& ViewLocation) const
{
	// Sky particles have camera-relative astronomical frames. Hand feedback must
	// remain visible even with custom/nonstandard DAT draw distances.
	if (bEnvironmentWeather || Emitter.bVRHandFeedback || CVarParticleDistanceCulling.GetValueOnGameThread() == 0)
		return false;
	// CameraSet's map view disables GfxObj degradation. Measuring the 450-unit
	// elevated camera as an ordinary viewer otherwise hides every nearby portal
	// or other particle object even though it remains in the overhead view.
	const auto* PC = GetWorld() ? Cast<AACEPlayerController>(GetWorld()->GetFirstPlayerController()) : nullptr;
	if (PC && PC->IsCameraMapView()) return false;
	const float Distance = Emitter.MaxDegradeDistance * FMath::Max(1.f, WorldScale);
	return FVector::DistSquared(ViewLocation, GetEmitterTransform(Emitter).GetLocation()) > FMath::Square(Distance);
}

void UACEScriptComponent::SetEmitterDegraded(FActiveEmitter& Emitter, bool bDegraded)
{
	if (Emitter.bDegraded == bDegraded) return;
	Emitter.bDegraded = bDegraded;
	if (Emitter.Batch) Emitter.Batch->SetVisibility(!bDegraded && !bCosmeticParticlesHidden);
	for (auto& Particle : Emitter.Particles)
	{
		if (auto* Mesh = Particle.Mesh.Get()) Mesh->SetVisibility(!bDegraded && !bCosmeticParticlesHidden);
		// Persistent particles retain accumulated lifetime while their update
		// timestamp advances offscreen. Returning must not restart their orbit/fade.
	}
}

void UACEScriptComponent::AccumulateParticleAudit(int32& Emitters, int32& Degraded, int32& Particles, int32& Lights) const
{
	Emitters += ActiveEmitters.Num();
	for (const auto& Emitter : ActiveEmitters)
	{
		Degraded += Emitter.bDegraded ? 1 : 0;
		if (!Emitter.bDegraded) Particles += Emitter.Particles.Num();
	}
	if (auto* Budget = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
		Lights += Budget->GetVisibleLightCount(this);
}

void UACEScriptComponent::ApplyParticlePose(FActiveEmitter& Emitter, FActiveParticle& Particle,
	float MotionAge, const FTransform* LiveParentFrame, const FVector* ViewPosition, const FVector& ParentDelta)
{
	const float T = MotionAge;
	// Retail: is_parent_local → current part/object Origin; else birth StartFrame.
	FVector ParentOrigin = Particle.StartOrigin;
	FQuat ParentRot = Particle.StartRotation;
	FVector Off = Particle.Offset;
	if (Particle.bParentLocal)
	{
		const FTransform Live = LiveParentFrame ? *LiveParentFrame : GetParticleStartFrame(Emitter);
		ParentOrigin = Live.GetLocation();
		ParentRot = Live.GetRotation();
		// Birth offset remains a world-space vector, even for parent-local emitters.
	}

	switch (Particle.Type)
	{
	case 1:
		Particle.Position = ParentOrigin + Off;
		break;
	case 2:
	case 12:
		Particle.Position = ParentOrigin + Off + T * Particle.A;
		break;
	case 3: case 8: case 10:
		Particle.Position = ParentOrigin + Off
			+ T * Particle.A + 0.5f * T * T * Particle.B;
		break;
	case 4: case 9: case 11:
		Particle.Position = ParentOrigin + Off
			+ T * Particle.A + 0.5f * T * T * Particle.B;
		break;
	case 5:
		// Swarm: DAT C is ellipse radii (Font 0x320002B5 C=(1,1,0); buff rings C=(1,1,0.2)).
		// ACE.Server adds C as a translation and uses Cos/Sin ±1 — that offsets the
		// whole Font 1 AC off-center. Oscillation is in AC axes, then AceVectorToUnreal
		// (negate X). Do not extra-negate Sin; that double-flips UE Y.
		{
			const FVector Swarm = ParentOrigin + Off + T * Particle.A;
			const FVector AcOsc(
				Particle.C.X * FMath::Cos(Particle.B.X * T),
				Particle.C.Y * FMath::Sin(Particle.B.Y * T),
				Particle.C.Z * FMath::Cos(Particle.B.Z * T));
			Particle.Position = Swarm + FACEPosition::AceVectorToUnreal(AcOsc, WorldScale);
		}
		break;
	case 6:
		// ACE Particle.Update Explode: (T*B + C*A.X)*T + Offset + parent
		Particle.Position = ParentOrigin + Off + FVector(
			(T * Particle.B.X + Particle.C.X * Particle.A.X) * T,
			(T * Particle.B.Y + Particle.C.Y * Particle.A.X) * T,
			(T * Particle.B.Z + Particle.C.Z * Particle.A.X + Particle.A.Z) * T);
		break;
	case 7:
		Particle.Position = ParentOrigin + Off
			+ FMath::Cos(Particle.A.X * T) * Particle.C + (T * T) * Particle.B;
		break;
	default:
		Particle.Position += ParentDelta;
		break;
	}

	const float Alpha = Particle.bHoldStill
		? 0.f
		: (Particle.Age < Particle.Life ? Particle.Age / Particle.Life : 1.f);
	const float Scale = FMath::Lerp(Particle.StartScale, Particle.FinalScale, Alpha);
	const float Trans = FMath::Lerp(Particle.StartTrans, Particle.FinalTrans, Alpha);
	FQuat Facing = Particle.bParentLocal ? ParentRot : Particle.StartRotation;
	if (Particle.Type == 4 || Particle.Type == 9 || Particle.Type == 11)
	{
		const double Angle = Particle.C.Size() * T;
		if (Angle > SMALL_NUMBER)
			Facing = (ParentRot * FQuat(Particle.C.GetSafeNormal(), Angle)).GetNormalized();
	}
	Facing = GetParticleDrawRotation(Particle.Position, Facing, Particle.DrawMode,
		ViewPosition);
	const float Opacity = FMath::Clamp(
		(1.f - Trans) * (IsRetailPortalFxSetup(SetupId) ? 1.f - ObjectTranslucency : 1.f), 0.f, 1.f);
	ApplyParticleVisual(Emitter, Particle, Facing, FVector(Scale), Opacity);
}

void UACEScriptComponent::TickParticlePresentation(float RenderAhead)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_ParticlePresentation);
	TGuardValue<bool> UpdatingVisuals(bUpdatingParticleVisuals, true);
	const bool bReuseFrames = CVarParticleReuseFrames.GetValueOnGameThread() != 0;
	TOptional<FVector> ViewPosition;
	if (bReuseFrames)
	{
		const auto* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager) ViewPosition = PC->PlayerCameraManager->GetCameraLocation();
	}
	for (FActiveEmitter& Emitter : ActiveEmitters)
	{
		if (Emitter.bDegraded) continue;
		TOptional<FTransform> LiveParentFrame;
		for (FActiveParticle& Particle : Emitter.Particles)
		{
			if (Particle.InstanceIndex < 0 && !Particle.Mesh.IsValid()) continue;
			if (bReuseFrames && Particle.bParentLocal && !LiveParentFrame.IsSet())
				LiveParentFrame = GetParticleStartFrame(Emitter);
			// Evaluate the authored trajectory between physics updates, without
			// consuming births, random values, or lifetime. Keep scale/translucency
			// at the lifecycle sample: sub-quantum repeating glows must not strobe.
			ApplyParticlePose(Emitter, Particle, FMath::Min(Particle.Age + RenderAhead, Particle.Life),
				LiveParentFrame.IsSet() ? &LiveParentFrame.GetValue() : nullptr,
				ViewPosition.IsSet() ? &ViewPosition.GetValue() : nullptr, FVector::ZeroVector);
		}
		if (Emitter.Batch) Emitter.Batch->FlushParticles();
	}
}

void UACEScriptComponent::TickEmitters(float DeltaTime)
{
	double SkyDelta = DeltaTime;
	if (bEnvironmentWeather && EnvironmentSkyTime.IsSet())
	{
		if (PreviousEnvironmentSkyTime.IsSet())
			SkyDelta = FMath::Max(0.,EnvironmentSkyTime.GetValue()-PreviousEnvironmentSkyTime.GetValue());
		PreviousEnvironmentSkyTime = EnvironmentSkyTime;
	}
	TGuardValue<bool> UpdatingVisuals(bUpdatingParticleVisuals, true);
	FVector ViewLocation;
	const bool bHasView = GetParticleViewLocation(ViewLocation);
	const bool bReuseFrames = CVarParticleReuseFrames.GetValueOnGameThread() != 0;
	TOptional<FVector> DrawViewPosition;
	if (bReuseFrames)
	{
		const auto* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager) DrawViewPosition = PC->PlayerCameraManager->GetCameraLocation();
	}
	for (int32 EmitterIndex = ActiveEmitters.Num() - 1; EmitterIndex >= 0; --EmitterIndex)
	{
		FActiveEmitter& Emitter = ActiveEmitters[EmitterIndex];
		const double Dt = bEnvironmentWeather && Emitter.RenderSortPriority < 0 ? SkyDelta : DeltaTime;
		Emitter.Age += Dt;
		const FTransform Anchor = GetEmitterTransform(Emitter);
		const FVector EmitterOrigin = Anchor.GetLocation();
		const FVector ParentDelta = EmitterOrigin - Emitter.LastOrigin;
		const double DisplacementSquared = FVector::DistSquared(EmitterOrigin, Emitter.LastEmissionOrigin)
			/ FMath::Square(FMath::Max(1.f, WorldScale));
		Emitter.LastOrigin = EmitterOrigin;
		Emitter.TimeSinceEmit += Dt;
		const bool bTimed = (Emitter.Info.EmitterType & 1) != 0;
		const bool bPerMeter = !bTimed && (Emitter.Info.EmitterType & 2) != 0;
		const bool bEmissionDue = bTimed ? Emitter.TimeSinceEmit > Emitter.Info.Birthrate
			: bPerMeter && DisplacementSquared > FMath::Square(Emitter.Info.Birthrate);
		const bool bDegrade = bHasView && ShouldDegradeEmitter(Emitter, ViewLocation);
		const bool bFinite = Emitter.Info.TotalSeconds > 0.0 || Emitter.Info.TotalParticles > 0;
		SetEmitterDegraded(Emitter, bDegrade);
		if (bDegrade)
		{
			if (bFinite || Emitter.bStopped)
			{
				// Retail ages existing finite particles while hidden. A short range
				// excursion must not erase their remaining tail or restart it later.
				for (int32 I=Emitter.Particles.Num()-1; I>=0; --I)
				{
					auto& Particle=Emitter.Particles[I];
					Particle.Age += Dt;
					if (Particle.Age >= Particle.Life)
					{
						ReleaseParticleVisual(Emitter,Particle);
						Emitter.Particles.RemoveAtSwap(I,1,EAllowShrinking::No);
					}
				}
				if (!Emitter.bStopped && bEmissionDue && Emitter.Particles.Num() < Emitter.Info.MaxParticles
					&& (Emitter.Info.TotalParticles <= 0 || Emitter.TotalBorn < Emitter.Info.TotalParticles))
				{
					++Emitter.TotalBorn;
					Emitter.TimeSinceEmit = 0.f;
					Emitter.LastEmissionOrigin = EmitterOrigin;
				}
				Emitter.bStopped |= (Emitter.Info.TotalSeconds > 0.0 && Emitter.Age > Emitter.Info.TotalSeconds)
					|| (Emitter.Info.TotalParticles > 0 && Emitter.TotalBorn >= Emitter.Info.TotalParticles);
				if (Emitter.bStopped && Emitter.Particles.IsEmpty())
				{
					if (Emitter.Batch) Emitter.Batch->DestroyComponent();
					ActiveEmitters.RemoveAtSwap(EmitterIndex, 1, EAllowShrinking::No);
				}
			}
			// No transforms, buffer uploads, or light updates for distant effects.
			continue;
		}
		// Neither the parent nor camera changes inside this simulation loop. Keep
		// the cache local: later emitters and same-frame hand/camera motion must
		// resolve fresh frames, and world-space particles retain their birth frame.
		TOptional<FTransform> LiveParentFrame;
		for (int32 ParticleIndex = Emitter.Particles.Num() - 1; ParticleIndex >= 0; --ParticleIndex)
		{
			FActiveParticle& Particle = Emitter.Particles[ParticleIndex];
			Particle.Age += Dt;
			if (Particle.Age >= Particle.Life)
			{
				if (Particle.bHoldStill && !Emitter.bStopped)
				{
					Particle.Age = 0.f;
				}
				else
				{
					ReleaseParticleVisual(Emitter, Particle);
					Emitter.Particles.RemoveAtSwap(ParticleIndex, 1, EAllowShrinking::No);
					continue;
				}
			}
			if (bReuseFrames && Particle.bParentLocal && !LiveParentFrame.IsSet())
				LiveParentFrame = GetParticleStartFrame(Emitter);
			ApplyParticlePose(Emitter, Particle, Particle.Age,
				LiveParentFrame.IsSet() ? &LiveParentFrame.GetValue() : nullptr,
				DrawViewPosition.IsSet() ? &DrawViewPosition.GetValue() : nullptr, ParentDelta);
		}
		// ParticleEmitter::UpdateParticles updates existing particles before emitting.
		// EmitParticle records the current time, so a hitch produces one new birth,
		// not a backlog burst or a newborn already aged by this frame's delta.
		if (!Emitter.bStopped)
		{
			if (bEmissionDue
				&& Emitter.Particles.Num() < Emitter.Info.MaxParticles
				&& (Emitter.Info.TotalParticles <= 0 || Emitter.TotalBorn < Emitter.Info.TotalParticles))
			{
				const int32 Before = Emitter.TotalBorn;
				SpawnParticle(Emitter);
				if (Emitter.TotalBorn > Before) Emitter.TimeSinceEmit = 0.f;
			}
		}
		// StopEmitter is evaluated after this tick's emission in retail.
		if (Emitter.Info.TotalSeconds > 0.0 && Emitter.Age > Emitter.Info.TotalSeconds)
		{
			Emitter.bStopped = true;
		}
		if (Emitter.Info.TotalParticles > 0 && Emitter.TotalBorn >= Emitter.Info.TotalParticles)
			Emitter.bStopped = true;
		// Upload one vertex buffer per emitter surface after simulation, preserving
		// independent fades without allocating a component and MID per spark.
		if (Emitter.Batch) Emitter.Batch->FlushParticles();
		if (Emitter.bStopped && Emitter.Particles.Num() == 0)
		{
			if (Emitter.Batch)
			{
				Emitter.Batch->DestroyComponent();
				Emitter.Batch = nullptr;
			}
			ActiveEmitters.RemoveAtSwap(EmitterIndex, 1, EAllowShrinking::No);
		}
	}
	for (const auto& Pooled : ParticlePool)
	{
		if (auto* Mesh = Pooled.Get())
		{
			Mesh->SetVisibility(false);
			Mesh->SetHiddenInGame(true);
		}
	}
}

void UACEScriptComponent::TickParticleLights(bool bPresentationOnly)
{
	auto* Lights = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr;
	if (!Lights) return;
	if (!bAllowInferredParticleLights || bEnvironmentWeather || bCosmeticParticlesHidden)
	{
		Lights->RemoveParticles(this);
		return;
	}

	TArray<FACEEffectLightRequest, TInlineAllocator<8>> Requests;
	const bool bPortal = IsRetailPortalFxSetup(SetupId);
	const bool bWielded = IsWieldedItemFx();
	const auto* Entity = Cast<AACEWorldEntityActor>(GetOwner());
	const bool bMissile = Entity && (Entity->PhysicsState & ACEPhysicsState::Missile) != 0;
	for (const FActiveEmitter& Emitter : ActiveEmitters)
	{
		if (Emitter.bDegraded || IsBloodSplatter(Emitter.SourcePlayScript)
			|| Emitter.LightLum < .05f || Emitter.Particles.IsEmpty()) continue;
		const bool bSelected = Lights->IsParticleSelected(this, Emitter.InstanceId);
		// Rejected effects retain their authored particle rendering. Only the
		// bounded set of lights needs per-frame centroid walks and transforms.
		if (bPresentationOnly && !bSelected) continue;
		auto ParticleWeight = [](const FActiveParticle& Particle)
		{
			const float Alpha = FMath::Clamp(Particle.Age / FMath::Max(Particle.Life, .01f), 0.f, 1.f);
			const float Opacity = FMath::Clamp(1.f - FMath::Lerp(Particle.StartTrans, Particle.FinalTrans, Alpha), 0.f, 1.f);
			return Opacity * FMath::Max(.15f, FMath::Lerp(Particle.StartScale, Particle.FinalScale, Alpha));
		};
		// Admission uses a representative particle in O(1), instead of walking
		// every spark of every rejected weapon just to decide whether to light it.
		FVector Position = Emitter.Particles[0].Position;
		float Weight = ParticleWeight(Emitter.Particles[0]) * Emitter.Particles.Num();
		if (bSelected)
		{
			FVector Acc = FVector::ZeroVector;
			Weight = 0.f;
			for (const FActiveParticle& Particle : Emitter.Particles)
			{
				const float W = ParticleWeight(Particle);
				Acc += Particle.Position * W;
				Weight += W;
			}
			if (Weight > KINDA_SMALL_NUMBER) Position = Acc / Weight;
		}
		if (Weight < .01f) continue;
		// A held weapon's illumination belongs to its current emitter hook.
		// World-space sparks intentionally remain behind as they age; using their
		// centroid makes the scene light trail the weapon and jump at each birth.
		// Keep that authored particle motion, but follow the live hand/weapon frame
		// on both simulation and presentation ticks (also during turns).
		if (bWielded) Position = GetEmitterTransform(Emitter).GetLocation();
		else if (bMissile) Position = Entity->GetActorLocation();
		if (bPresentationOnly)
		{
			Lights->UpdateParticlePosition(this, Emitter.InstanceId, Position);
			continue;
		}
		FACEEffectLightRequest& Request = Requests.AddDefaulted_GetRef();
		Request.EmitterId = Emitter.InstanceId;
		Request.Position = Position;
		Request.Color = Emitter.LightColor;
		if (bWielded || bPortal)
		{
			// The texture estimate supplies the hue. Dark/transparent padding in
			// small spark textures must not attenuate carried illumination again:
			// its strength already comes from authored luminosity and live opacity.
			const float Peak = FMath::Max3(Request.Color.R, Request.Color.G, Request.Color.B);
			if (Peak <= KINDA_SMALL_NUMBER) { Requests.Pop(EAllowShrinking::No); continue; }
			Request.Color /= Peak;
			Request.Color.A = 1.f;
		}
		// Portals illuminate a room-sized area; weapon sparks remain local.
		Request.Radius = bPortal ? 900.f : bWielded ? 320.f : 480.f;
		Request.bPortal = bPortal;
		const float Strength = Emitter.LightLum * Weight;
		// Keep isolated effects readable, but allow fading effects to reach zero.
		// The world budget additionally limits summed overlap in crowded rooms.
		Request.Intensity = FMath::Min(12.f, 10.f + FMath::Sqrt(Strength) * 1.4f)
			* FMath::Clamp(Strength / .2f, 0.f, 1.f);
	}
	if (!bPresentationOnly) Lights->SubmitParticles(this, Requests);
}

uint32 UACEScriptComponent::ResolveEmitterSlot(uint32 SlotId, uint32 SourcePlayScript) const
{
	const uint32 Scope = AetheriaSlotScope(SourcePlayScript);
	if (Scope == 0 || SlotId == 0) return SlotId;
	const FActiveEmitter* Emitter = ActiveEmitters.FindByPredicate([SlotId, Scope](const FActiveEmitter& E)
	{ return E.SlotId == SlotId && AetheriaSlotScope(E.SourcePlayScript) == Scope; });
	return Emitter ? Emitter->InstanceId : 0;
}

void UACEScriptComponent::StopEmitter(uint32 InstanceId, bool bDestroy)
{
	if (InstanceId == 0) return;
	if (bDestroy)
		if (auto* Lights = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
			Lights->RemoveEmitter(this, InstanceId);
	for (int32 Index = ActiveEmitters.Num() - 1; Index >= 0; --Index)
	{
		FActiveEmitter& Emitter = ActiveEmitters[Index];
		// Hook slots are resolved before entering here; the unique instance ID
		// also keeps isolated surges' particle lights separate from cast lights.
		if (Emitter.InstanceId != InstanceId)
		{
			continue;
		}
		Emitter.bStopped = true;
		if (bDestroy)
		{
			for (FActiveParticle& Particle : Emitter.Particles)
			{
				ReleaseParticleVisual(Emitter, Particle);
			}
			if (Emitter.Batch)
			{
				Emitter.Batch->DestroyComponent();
				Emitter.Batch = nullptr;
			}
			ActiveEmitters.RemoveAtSwap(Index, 1, EAllowShrinking::No);
		}
	}
	if (ActiveEmitters.IsEmpty())
		if (auto* Lights = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
			Lights->RemoveParticles(this);
}

void UACEScriptComponent::SetCosmeticParticlesHidden(UObject* Owner, bool bHidden)
{
	if (!Owner) return;
	if (bHidden) CosmeticParticleHideOwners.Add(Owner);
	else CosmeticParticleHideOwners.Remove(Owner);
	for (auto It = CosmeticParticleHideOwners.CreateIterator(); It; ++It)
		if (!It->IsValid()) It.RemoveCurrent();
	const bool bHide = !CosmeticParticleHideOwners.IsEmpty();
	if (bHide == bCosmeticParticlesHidden) return;
	bCosmeticParticlesHidden = bHide;
	// These components are not children of the weapon mesh. Preserve clocks and
	// distance degradation; stopping scripts would lose persistent elemental FX.
	for (auto& Emitter : ActiveEmitters)
	{
		const bool bVisible = !bHide && !Emitter.bDegraded;
		if (Emitter.Batch) Emitter.Batch->SetVisibility(bVisible);
		for (auto& Particle : Emitter.Particles)
			if (auto* Mesh = Particle.Mesh.Get()) Mesh->SetVisibility(bVisible);
	}
	TickParticleLights();
}

void UACEScriptComponent::StopAllEffects()
{
	bHiddenEffectActive = false;
	ActiveScripts.Reset();
	ScheduledScripts.Reset();
	ScriptClock = 0.0;
	ParticleTimeSinceUpdate = 0.0;
	for (int32 Index = ActiveEmitters.Num() - 1; Index >= 0; --Index)
	{
		FActiveEmitter& Emitter = ActiveEmitters[Index];
		for (FActiveParticle& Particle : Emitter.Particles)
		{
			ReleaseParticleVisual(Emitter, Particle);
		}
		if (Emitter.Batch)
		{
			Emitter.Batch->DestroyComponent();
			Emitter.Batch = nullptr;
		}
	}
	ActiveEmitters.Reset();
	Tweens.Reset();
	ObjectTranslucency = OriginalObjectTranslucency;
	if (auto* Lights = GetWorld() ? GetWorld()->GetSubsystem<UACEEffectLightSubsystem>() : nullptr)
		Lights->RemoveSource(this);
	// Environment scripts own their particles, not the sky host's slot materials.
	// Cloning those MIDs disconnects GameSky UV animation after an indoor stop.
	if (!bEnvironmentWeather) RestoreMeshVisuals();
}

void UACEScriptComponent::RestoreMeshVisuals()
{
	ObjectTranslucency = OriginalObjectTranslucency;
	RestoreDissolveMaterials();
	ApplyMaterialScalar(INDEX_NONE, TEXT("OpacityMul"), 1.f - OriginalObjectTranslucency);
	// Local pawn (not a WorldEntityActor): portal/spell Transparent + NoDraw must not
	// leave HiddenInGame parts after recall. World objects keep ApplyPhysicsState.
	if (!Cast<AACEWorldEntityActor>(GetOwner()))
	{
		if (UACECharacterAppearanceComponent* App = GetAppearance())
		{
			App->SetAppearanceVisible(true);
		}
	}
}
