#include "ACECharacterAppearanceComponent.h"
#include "ACEAttackMotion.h"
#include "ACERuntimeOptions.h"
#include "ACEProfiling.h"
#include "ACELoadingScreenActor.h"
#include "ACERegionSceneryActor.h"
#include "ACEDatSubsystem.h"
#include "ACEScriptComponent.h"
#include "ACEWorldEntityActor.h"
#include "ProceduralMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

static TAutoConsoleVariable<int32> CVarACEReusePoseBuffers(TEXT("ace.Animation.ReusePoseBuffers"), 1,
	TEXT("Reuse per-actor pose storage across animation ticks without changing sampling or hooks."));

static TAutoConsoleVariable<int32> CVarACECachedActorDraws(
	TEXT("ace.Render.CachedActorDraws"), PLATFORM_ANDROID ? 0 : 1,
	TEXT("Reuse rigid actor draw commands. PC default on; Android opt-in pending native frame timings. Live changes refresh existing actors."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Variable)
	{
		// A reversible native A/B capture should not need a logout, different
		// camera placement, or newly streamed actors for the switch to take effect.
		if (!GIsRunning || !IsInGameThread()) return;
		const bool bCached = Variable->GetInt() != 0;
		for (TObjectIterator<UACECharacterAppearanceComponent> It; It; ++It)
		{
			if (It->IsTemplate() || !It->IsRegistered()) continue;
			for (int32 Index=0; Index<It->GetPartCount(); ++Index)
				if (auto* Part=Cast<UProceduralMeshComponent>(It->GetPartMesh(Index)); Part && Part->bPreferCachedDraws!=bCached)
				{
					Part->bPreferCachedDraws=bCached;
					Part->MarkRenderStateDirty();
				}
		}
	}));

static bool ACEIsMagicPowerUp(uint32 Command)
{
	return (Command >= 0x1000006Fu && Command <= 0x10000078u)
		|| (Command >= 0x1000012Bu && Command <= 0x10000134u);
}

static bool ACEIsMagicCastCommand(uint32 Command)
{
	return (Command >= 0x4000002Bu && Command <= 0x40000039u)
		|| Command == 0x400000D3u
		|| Command == 0x400000E0u
		|| Command == 0x400000E1u;
}

static bool ACEIsMissileSubstate(uint32 Command)
{
	return Command == 0x40000016u // Reload
		|| (Command >= 0x4000001Eu && Command <= 0x4000002Au); // Aim
}

static bool ACEIsLauncherStyle(uint32 Style)
{
	return Style == 0x8000003Fu || Style == 0x80000041u || Style == 0x8000013Bu
		|| Style == 0x800000E8u || Style == 0x800000E9u;
}

static bool ACEIsEmoteSubstate(uint32 Command)
{
	// Action gestures (0x1300...) already contain their return motion. Held
	// substates have separate entry/exit links in CMotionTable::GetObjectSequence.
	return Command != ACEMotion::Dead && (Command & 0x40000000u) != 0
		&& ACEMotion::IsHeldRestCommand(Command);
}

UACECharacterAppearanceComponent::UACECharacterAppearanceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UACECharacterAppearanceComponent::EnsureMeshRoot()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}
	if (!MeshRoot)
	{
		MeshRoot = NewObject<USceneComponent>(Owner, TEXT("ACEMeshRoot"));
		MeshRoot->SetupAttachment(Owner->GetRootComponent());
		MeshRoot->RegisterComponent();
	}

	FVector FeetOffset = FVector::ZeroVector;
	if (bAlignMeshToCapsuleBottom)
	{
		if (UCapsuleComponent* Cap = Owner->FindComponentByClass<UCapsuleComponent>())
		{
			const FVector FeetWorld = Cap->GetComponentLocation()
				- Cap->GetUpVector() * Cap->GetScaledCapsuleHalfHeight();
			FeetOffset = Owner->GetRootComponent()->GetComponentTransform().InverseTransformPosition(FeetWorld);
		}
	}
	if (bVRPoseControlled) return; // The tracked rig owns this root during appearance refreshes.
	MeshRoot->SetRelativeLocation(FeetOffset);
	MeshRoot->SetRelativeRotation(FRotator(0.f, MeshFacingYawDegrees, 0.f));
}

void UACECharacterAppearanceComponent::SetPreviewCapture(bool bEnabled)
{
	bPreviewCapture = bEnabled;
	for (UProceduralMeshComponent* Part : PartMeshes)
		if (Part) Part->SetForceHighestDetail(bPreviewCapture || bLocalPlayerAppearance);
}

float UACECharacterAppearanceComponent::GetVisualHeightCm() const
{
	FBox VisualBounds(ForceInit);
	for (const UProceduralMeshComponent* Part : PartMeshes)
	{
		if (Part && Part->IsRegistered() && Part->GetNumSections() > 0)
		{
			VisualBounds += Part->GetHighestDetailBounds().GetBox();
		}
	}
	return VisualBounds.IsValid ? VisualBounds.GetSize().Z : 0.f;
}

bool UACECharacterAppearanceComponent::GetVisualWorldBounds(FBox& OutBox) const
{
	OutBox = FBox(ForceInit);
	for (const UProceduralMeshComponent* Part : PartMeshes)
	{
		if (Part && Part->IsRegistered() && Part->GetNumSections() > 0)
		{
			OutBox += Part->GetHighestDetailBounds().GetBox();
		}
	}
	return OutBox.IsValid != 0;
}

bool UACECharacterAppearanceComponent::GetPartBindTransform(int32 PartIndex, FTransform& OutTransform) const
{
	if (!BindTransforms.IsValidIndex(PartIndex))
	{
		return false;
	}
	OutTransform = BindTransforms[PartIndex];
	return true;
}

bool UACECharacterAppearanceComponent::GetSelectionWorldBounds(FBox& OutBox) const
{
	GetVisualWorldBounds(OutBox);
	for (int32 I = 0; I < PartDrawingSpheres.Num(); ++I)
	{
		if (!PartMeshes.IsValidIndex(I) || !PartMeshes[I] || PartDrawingSpheres[I].W <= 0) continue;
		const FSphere Sphere = PartDrawingSpheres[I].TransformBy(PartMeshes[I]->GetComponentTransform());
		OutBox += FBox(Sphere.Center - FVector(Sphere.W), Sphere.Center + FVector(Sphere.W));
	}
	return OutBox.IsValid != 0;
}

bool UACECharacterAppearanceComponent::TraceDrawingSpheres(const FVector& Start, const FVector& End, double& ClosestDistance) const
{
	bool Hit = false;
	const double Length = FVector::Distance(Start, End);
	for (int32 I = 0; I < PartDrawingSpheres.Num(); ++I)
	{
		const auto* Part = PartMeshes.IsValidIndex(I) ? PartMeshes[I].Get() : nullptr;
		const FSphere& Sphere = PartDrawingSpheres[I];
		if (!Part || !Part->IsRegistered() || !Part->IsVisible() || Part->bHiddenInGame || Sphere.W <= 0) continue;
		bool VisibleGeometry = false;
		for (int32 S = 0; S < Part->GetNumSections(); ++S)
		{
			const auto* Section = const_cast<UProceduralMeshComponent*>(Part)->GetProcMeshSection(S);
			if (Section && Section->bSectionVisible && Section->ProcIndexBuffer.Num() >= 3) { VisibleGeometry = true; break; }
		}
		if (!VisibleGeometry) continue;
		// Trace in part space, so animation, orientation and nonuniform server
		// scale apply exactly once. CSphere::sphere_intersects_ray rejects an
		// origin inside the sphere; it must not capture the viewer's own body.
		const FTransform& Transform = Part->GetComponentTransform();
		const FVector A = Transform.InverseTransformPosition(Start) - Sphere.Center;
		const FVector D = Transform.InverseTransformPosition(End) - Transform.InverseTransformPosition(Start);
		const double DD = D.SizeSquared(), AD = FVector::DotProduct(A, D);
		const double C = A.SizeSquared() - Sphere.W * Sphere.W;
		const double Discriminant = AD * AD - DD * C;
		if (DD <= UE_SMALL_NUMBER || C <= 0 || Discriminant < 0) continue;
		const double T = (-AD - FMath::Sqrt(Discriminant)) / DD;
		if (T >= 0 && T <= 1 && T * Length < ClosestDistance)
		{
			ClosestDistance = T * Length;
			Hit = true;
		}
	}
	return Hit;
}

void UACECharacterAppearanceComponent::StabilizeVRPelvis()
{
	FTransform Bind, Animated;
	if (!GetPartBindTransform(0, Bind) || !GetPartCurrentTransform(0, Animated)) return;
	// Stance animations move the abdomen, but VR pins the chest under the
	// headset. Remove that common lower-body shift while keeping leg motion.
	for (int32 I=1; I<9; ++I)
		if (auto* Part=GetPartMesh(I)) Part->SetRelativeTransform(Part->GetRelativeTransform().GetRelativeTransform(Animated) * Bind);
	if (auto* Pelvis=GetPartMesh(0)) Pelvis->SetRelativeTransform(Bind);
}

bool UACECharacterAppearanceComponent::GetPartCurrentTransform(int32 PartIndex, FTransform& OutTransform) const
{
	if (PartMeshes.IsValidIndex(PartIndex) && PartMeshes[PartIndex])
	{
		OutTransform = PartMeshes[PartIndex]->GetRelativeTransform();
		return true;
	}
	return GetPartBindTransform(PartIndex, OutTransform);
}

USceneComponent* UACECharacterAppearanceComponent::GetPartMesh(int32 PartIndex) const
{
	if (PartMeshes.IsValidIndex(PartIndex) && PartMeshes[PartIndex])
	{
		return PartMeshes[PartIndex];
	}
	return nullptr;
}

void UACECharacterAppearanceComponent::SetSelectionHighlight(float Strength)
{
	for (const auto& Part : PartMeshes)
		if (Part)
		{
			const auto& Data=Part->GetCustomPrimitiveData().Data;
			if ((Data.IsValidIndex(1) ? Data[1] : 0.f) != Strength) Part->SetCustomPrimitiveDataFloat(1,Strength);
		}
}

bool UACECharacterAppearanceComponent::ReplacePartGfxObj(int32 PartIndex, uint32 GfxObjId)
{
	if (!PartMeshes.IsValidIndex(PartIndex) || !PartMeshes[PartIndex] || GfxObjId == 0)
	{
		return false;
	}
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat)
	{
		return false;
	}
	// ApplySetupToProceduralMesh accepts bare GfxObj DIDs (0x01) via BuildGfxObjOnly.
	const bool bOk = Dat->ApplySetupToProceduralMesh(
		PartMeshes[PartIndex], static_cast<int32>(GfxObjId), WorldScale, /*bEnableCollision*/ false);
	if (bOk)
	{
		if (const auto* Built = Dat->GetOrBuildSetupMesh(GfxObjId, WorldScale); Built && Built->Parts.Num() == 1)
		{
			PartDrawingSpheres.SetNum(PartMeshes.Num());
			PartDrawingSpheres[PartIndex] = Built->Parts[0].DrawingSphere;
		}
		// Keep the part's current placement/anim transform; only geometry/materials change.
		UE_LOG(LogTemp, Verbose, TEXT("ACEAppearance: ReplaceObject part=%d gfx=0x%08X"),
			PartIndex, GfxObjId);
	}
	return bOk;
}

void UACECharacterAppearanceComponent::SetAppearanceVisible(bool bVisible)
{
	// Animation/door restoration cannot override an authoritative visibility flag.
	// Keep this at the part boundary so attachment and appearance rebuilds agree.
	if (const auto* Entity = Cast<AACEWorldEntityActor>(GetOwner()))
		bVisible = bVisible && !Entity->IsMeshSuppressed();
	for (UProceduralMeshComponent* Part : PartMeshes)
	{
		if (Part)
		{
			// Held actors attach under these parts. Their collision proxies and
			// script components own their visibility; never recursively reveal them.
			Part->SetVisibility(bVisible);
			Part->SetHiddenInGame(!bVisible);
		}
	}
}

void UACECharacterAppearanceComponent::SetPartsCastShadow(bool bCast, bool bInset)
{
	const bool bWantInset = bInset && bCast;
	for (UProceduralMeshComponent* Part : PartMeshes)
	{
		if (!Part)
		{
			continue;
		}
		if (bCast && Part->Mobility != EComponentMobility::Movable)
		{
			Part->SetMobility(EComponentMobility::Movable);
		}
		if (static_cast<bool>(Part->CastShadow) != bCast || Part->bCastDynamicShadow != bCast)
		{
			Part->SetCastShadow(bCast);
			Part->bCastDynamicShadow = bCast;
			Part->bCastShadowAsTwoSided = bCast;
			Part->MarkRenderStateDirty();
		}
		if (Part->bCastInsetShadow != bWantInset)
		{
			Part->bCastInsetShadow = bWantInset;
		}
	}
}

bool UACECharacterAppearanceComponent::GetPartsCastShadow() const
{
	for (const UProceduralMeshComponent* Part : PartMeshes)
	{
		if (Part)
		{
			return static_cast<bool>(Part->CastShadow);
		}
	}
	return false;
}

bool UACECharacterAppearanceComponent::ApplyPositionPlacement(int32 PlacementId)
{
	// Retail SmartBox::HandleReceivedPosition only sets placement when HasAnims
	// is false. A position correction must not replace a creature/prop animation.
	if (!bHasMesh || AppliedPlacementId == PlacementId || bPlayIdleMotion
		|| ActionCommand != 0 || bPreviewAnimation || bVRPoseControlled) return false;
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat) return false;
	const auto Built = Dat->GetOrBuildSetupAppearanceShared(SetupId, Appearance, WorldScale, PlacementId);
	if (!Built || Built->Parts.Num() != BindTransforms.Num()) return false;
	for (int32 Index = 0; Index < BindTransforms.Num(); ++Index)
	{
		BindTransforms[Index] = Built->Parts[Index].BindTransform;
		// Static parts do not tick when they become visible again, so update
		// hidden parts too. Keep their mesh sections, materials and script state.
		if (PartMeshes.IsValidIndex(Index) && PartMeshes[Index])
			PartMeshes[Index]->SetRelativeTransform(BindTransforms[Index]);
	}
	bHeldKeepPlacementPose = false;
	AppliedPlacementId = PlacementId;
	++AppearanceRevision; // Ground-support bounds depend on these part transforms.
	return true;
}

void UACECharacterAppearanceComponent::ApplyPartTransform(int32 PartIndex, const FTransform& AnimOrBind)
{
	if (bVRPoseControlled) return;
	if (!PartMeshes.IsValidIndex(PartIndex) || !PartMeshes[PartIndex])
	{
		return;
	}
	// Portal visibility can hide whole creatures/props while their motion clocks
	// and animation hooks still need to advance. Do not dirty render transforms
	// for those hidden, non-colliding parts. Doors/query geometry keep updating.
	if (GetOwner()->IsHidden() && PartMeshes[PartIndex]->GetCollisionEnabled() == ECollisionEnabled::NoCollision
		&& !bPreviewCapture && !bPreviewAnimation) return;
	// Facing is on MeshRoot only — parts keep raw Setup/Anim transforms so head/body stay coherent.
	const FTransform& Previous = PartMeshes[PartIndex]->GetRelativeTransform();
	// Centimeter translation tolerance is not a quaternion tolerance: .04 skipped
	// several degrees of rotation, turning slow tunnel/creature motion into steps.
	if (Previous.GetTranslation().Equals(AnimOrBind.GetTranslation(), .04f)
		&& Previous.GetRotation().Equals(AnimOrBind.GetRotation(), .00001f)
		&& Previous.GetScale3D().Equals(AnimOrBind.GetScale3D(), .00001f))
	{
		return;
	}
	PartMeshes[PartIndex]->SetRelativeTransform(AnimOrBind);
}

void UACECharacterAppearanceComponent::TickObjectAnimFrame(float DeltaTime)
{
	FVector OmegaUe = FVector::ZeroVector;
	if (AACEWorldEntityActor* Entity = Cast<AACEWorldEntityActor>(GetOwner()))
	{
		OmegaUe = -FACEPosition::AceVectorToUnreal(Entity->GetAcePhysicsOmega(), 1.f);
	}
	else if (AActor* Owner = GetOwner())
	{
		// Portal tunnel: retail animate_static_object rotates Position.Frame by Omega after
		// PartArray.Update — SetOmega hooks land on Scripts, not the world entity.
		if (UACEScriptComponent* Scripts = Owner->FindComponentByClass<UACEScriptComponent>())
		{
			OmegaUe = Scripts->GetRootOmegaRadiansPerSecond();
		}
	}
	if (OmegaUe.IsNearlyZero())
	{
		return;
	}
	const FQuat DeltaQ(OmegaUe.GetSafeNormal(), OmegaUe.Size() * DeltaTime);
	ObjectAnimFrame.SetRotation((DeltaQ * ObjectAnimFrame.GetRotation()).GetNormalized());
}

void UACECharacterAppearanceComponent::ApplyDefaultAnimPartTransforms(
	const TArray<FTransform>& Animated, int32 AnimatedCount)
{
	// Retail CPartArray::UpdateParts: Frame::combine(part, object_frame, anim_frame[i], scale).
	// Sequence part frames replace Setup placement while the clip plays. portalspace_background
	// placement is (51,0,0) but anim 0x030005AC frame 0 is (0,0,51) — composing bind rotation
	// onto those frames leaves gaps in the tunnel wall.
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (!bHeldKeepPlacementPose && i < AnimatedCount && Animated.IsValidIndex(i))
		{
			FTransform AnimPart = Animated[i];
			if (BindTransforms.IsValidIndex(i))
				AnimPart.SetScale3D(BindTransforms[i].GetScale3D());
			FTransform PartXform = FACEPosition::CombineAceFrames(ObjectAnimFrame, AnimPart);
			ApplyPartTransform(i, PartXform);
		}
		else if (BindTransforms.IsValidIndex(i))
		{
			ApplyPartTransform(i, BindTransforms[i]);
		}
	}
}

void UACECharacterAppearanceComponent::BillboardSpriteParts()
{
	if (!bBillboardSpriteParts)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager : nullptr;
	if (!Camera)
	{
		return;
	}
	const FVector CamFwd = Camera->GetActorForwardVector();
	const FVector CamUp = Camera->GetActorUpVector();
	if (CamFwd.IsNearlyZero() || CamUp.IsNearlyZero())
	{
		return;
	}
	// Match particle XZ sprites: authored facing local -Y toward the camera.
	const FQuat Face = FRotationMatrix::MakeFromYZ(CamFwd, CamUp).ToQuat();
	for (UProceduralMeshComponent* Part : PartMeshes)
	{
		if (Part && Part->GetNumSections() > 0)
		{
			Part->SetWorldRotation(Face);
		}
	}
}

void UACECharacterAppearanceComponent::RecookPartPhysics()
{
	for (UProceduralMeshComponent* Proc : PartMeshes)
	{
		if (!Proc || Proc->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
		{
			continue;
		}
		Proc->bUseComplexAsSimpleCollision = true;
		Proc->RecreatePhysicsState();
	}
}

void UACECharacterAppearanceComponent::SetPreviewAnimation(uint32 AnimationId, bool bAnimate, int32 StartFrame)
{
	// gmCG3DView::StartAnimation/StopAnimation use an absolute part-frame sequence,
	// not the setup's Resting placement and not the world locomotion state machine.
	bPreviewAnimation = true;
	PreviewStartFrame = FMath::Max(0,StartFrame);
	DefaultAnimationId = AnimationId;
	AnimMode = EACEAnimMode::DefaultAnimLoop;
	bPlayIdleMotion = true;
	LocomotionPlayRate = bAnimate ? 1.f : 0.f;
	ObjectAnimFrame = FTransform::Identity;
	AnimTime = DeferredPoseDeltaTime = 0.f;
	ResetHookTracking();
	TickComponent(0.f, LEVELTICK_All, nullptr); // Pose before the first capture, including face view.
	SetComponentTickEnabled(bAnimate);
}

void UACECharacterAppearanceComponent::ResetDefaultAnimClock()
{
	AnimTime = 0.f;
	DeferredPoseDeltaTime = 0.f;
	ObjectAnimFrame = FTransform::Identity;
	ResetHookTracking();
}

void UACECharacterAppearanceComponent::InitializePropAnimationPose()
{
	if (!bNeedsInitialPropPose) return;
	// Retail initializes the motion table's default state before drawing. Its
	// first-frame hooks can hide parts (e.g. the Bind Stone's four spikes).
	// Use the normal evaluation path so frame-zero hooks run exactly once and
	// later use animations can reveal those parts again.
	TickComponent(0.f, LEVELTICK_All, nullptr);
	if (bHookTrackValid) bNeedsInitialPropPose = false;
}

void UACECharacterAppearanceComponent::DispatchCrossedHooks(const TArray<FACEDatAnimationHook>& Hooks)
{
	if (Hooks.Num() == 0)
	{
		return;
	}
	UACEScriptComponent* Scripts = GetOwner()
		? GetOwner()->FindComponentByClass<UACEScriptComponent>()
		: nullptr;
	if (!IsValid(Scripts))
	{
		return;
	}
	AACELoadingScreenActor* PortalTunnel = Cast<AACELoadingScreenActor>(GetOwner());
	for (const FACEDatAnimationHook& Hook : Hooks)
	{
		if (PortalTunnel && Hook.Type == EACEAnimationHookType::SoundTweaked)
		{
			if (PortalTunnel->TryStartPortalAmbient(Hook.Id, Hook.Volume))
			{
				continue;
			}
		}
		if (GetOwner() && GetOwner()->IsA<AACERegionSceneryActor>()
			&& (Hook.Type == EACEAnimationHookType::Sound
				|| Hook.Type == EACEAnimationHookType::SoundTable
				|| Hook.Type == EACEAnimationHookType::SoundTweaked))
		{
			continue;
		}
		Scripts->DispatchAnimationHook(Hook);
	}
}

const float* UACECharacterAppearanceComponent::GetPreviousHookTime(uint64 TrackKey, float CurrentEvalTime)
{
	if (!bHookTrackValid || HookTrackKey != TrackKey || !FMath::IsFinite(CurrentEvalTime))
	{
		return nullptr;
	}
	return &PreviousHookTime;
}

void UACECharacterAppearanceComponent::CommitHookTime(uint64 TrackKey, float CurrentEvalTime)
{
	HookTrackKey = TrackKey;
	PreviousHookTime = CurrentEvalTime;
	bHookTrackValid = FMath::IsFinite(CurrentEvalTime);
}

void UACECharacterAppearanceComponent::ResetHookTracking()
{
	HookTrackKey = 0;
	PreviousHookTime = 0.f;
	bHookTrackValid = false;
}

void UACECharacterAppearanceComponent::EnsurePartMeshes(int32 Count)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}
	EnsureMeshRoot();
	USceneComponent* AttachParent = MeshRoot ? MeshRoot.Get() : Owner->GetRootComponent();
	while (PartMeshes.Num() < Count)
	{
		const FName Name = *FString::Printf(TEXT("ACEPart_%d"), PartMeshes.Num());
		UProceduralMeshComponent* Proc = NewObject<UProceduralMeshComponent>(Owner, Name);
		// These are rigid part meshes: animation updates their primitive transform,
		// not their vertices. Cached draws retain that live uniform buffer, just as
		// movable StaticMeshComponents do. Geometry/material edits invalidate it.
		Proc->bPreferCachedDraws = CVarACECachedActorDraws.GetValueOnGameThread() != 0;
		if (AttachParent)
		{
			Proc->SetupAttachment(AttachParent);
		}
		Proc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// AC uses explicit movement/contact/selection traces, never overlap events
		// on the individual animated body parts. Avoid overlap maintenance for
		// every pose update (including parts later enabled for visibility queries).
		Proc->SetGenerateOverlapEvents(false);
		Proc->SetCastShadow(false);
		Proc->bCastDynamicShadow = false;
		Proc->bCastContactShadow = false;
		Proc->bCastInsetShadow = false;
		Proc->bCastFarShadow = false;
		Proc->RegisterComponent();
		PartMeshes.Add(Proc);
	}
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (PartMeshes[i])
		{
			PartMeshes[i]->SetVisibility(i < Count);
		}
	}
}

void UACECharacterAppearanceComponent::HideOwnerPrimitiveMeshes()
{
	AActor* Owner = GetOwner();
	if (!Owner || !bHideOwnerMeshes)
	{
		return;
	}
	TArray<UPrimitiveComponent*> Primatives;
	Owner->GetComponents<UPrimitiveComponent>(Primatives);
	for (UPrimitiveComponent* Prim : Primatives)
	{
		if (!Prim || PartMeshes.Contains(Prim))
		{
			continue;
		}
		if (Prim->IsA(UProceduralMeshComponent::StaticClass()))
		{
			continue;
		}
		Prim->SetVisibility(false);
	}
}

void UACECharacterAppearanceComponent::ClearAppearance()
{
	WalkingMotion.Reset();
	bRemoteWalkingFramePending = false;
	WorldLightingInstances.Reset(); bWorldLightingInterior=false; bWorldLightingEnclosed=false;
	AppearanceSourceMaterials.Reset();
	if (AActor* Owner = GetOwner())
		if (auto* Scripts = Owner->FindComponentByClass<UACEScriptComponent>()) Scripts->StopAllEffects();
	for (UProceduralMeshComponent* P : PartMeshes)
	{
		if (P)
		{
			P->ClearAllMeshSections();
			P->EmptyOverrideMaterials();
			P->SetVisibility(false);
		}
	}
	bHasMesh = false;
	bNeedsInitialPropPose = false;
	SetupId = 0;
	MotionTableId = 0;
	DefaultAnimationId = 0;
	bBillboardSpriteParts = false;
	bPreviewAnimation = false;
	Appearance = FACEObjDesc();
	AppliedPlacementId = 0;
	AppliedAppearanceHash = 0;
	AppliedTranslucencyKey = -1.f;
	AppliedWorldScale = 0.f;
	AppliedPartCollision = false;
	AppliedDistanceLevels = false;
	LocomotionForward = 0.f;
	LocomotionStrafe = 0.f;
	LocomotionPlayRate = 1.f;
	RunBlend = 0.f;
	AnimMode = EACEAnimMode::Locomotion;
	ActionCommand = 0;
	ActionFromCommand = ACEMotion::Ready;
	bActionUsesStateTransition = false;
	bDoorHoldFinal = false;
	bHoldActionFinal = false;
	bHoldActionFinalAfterFinish = false;
	bSuppressNextHookDispatch = false;
	AnimTime = 0.f;
	DeferredPoseDeltaTime = 0.f;
	ResetHookTracking();
}

bool UACECharacterAppearanceComponent::ApplyWorldObject(const FACEWorldObject& Object, float InWorldScale, bool bEnablePartCollision)
{
	ACE_PROFILE_SCOPE(Appearance);
	WorldScale = InWorldScale;
	bLocalPlayerAppearance = Object.bIsSelf;
	for (UProceduralMeshComponent* Part : PartMeshes)
		if (Part) Part->SetForceHighestDetail(bLocalPlayerAppearance || bPreviewCapture);
	const int32 NewSetupId = Object.SetupId;
	const bool bCreatureLike = (Object.ItemType & ACEItemType::Creature) != 0 || Object.bIsPlayer || Object.bIsSelf;
	int32 PlacementId = Object.PlacementId;
	const bool bUsesOnOff = Object.UsesOnOffMotion();
	const bool bHeld = Object.ParentGuid != 0 && Object.ParentLocation != 0;
	const bool bStaticProp = !Object.bIsPlayer && (Object.ItemType & ACEItemType::Creature) == 0
		&& (Object.PhysicsState & ACEPhysicsState::Missile) == 0 && Object.ParentGuid == 0
		&& Object.WielderId == 0 && Object.ContainerId == 0 && !bUsesOnOff;
	if (bHeld)
	{
		// Always use the ParentLocation placement (RightHandCombat / LeftHand / Shield).
		// Server Resting/Default/leftover ids leave the gfx centered on the grip so the
		// staff reads twice as long and particle hooks sit in the hand.
		const int32 HeldPlacement = ACEPlacementFromParentLocation(Object.ParentLocation);
		if (HeldPlacement != 0)
		{
			PlacementId = HeldPlacement;
		}
	}
	// Retail CPhysicsObj::InitPhysicsDesc applies the supplied animframe_id,
	// including zero. Resting (101) is only valid when explicitly supplied by
	// the server: substituting it for custom world props changes their offset
	// and orientation, hiding floating crystals inside their pedestals.

	// Retail CPhysicsObj::SetScale changes transforms independently of ObjDesc.
	// Apply scale before the mesh-cache fast path, including preview actors.
	if (!bHeld)
	{
		const float S = Object.GetValidObjectScale();
		if (AActor* Owner = GetOwner())
		{
			UCapsuleComponent* Capsule = bAlignMeshToCapsuleBottom ? Owner->FindComponentByClass<UCapsuleComponent>() : nullptr;
			const FVector Feet = Capsule ? Capsule->GetComponentLocation() - Capsule->GetUpVector() * Capsule->GetScaledCapsuleHalfHeight() : FVector::ZeroVector;
			Owner->SetActorScale3D(FVector(S));
			if (Capsule)
			{
				const FVector NewFeet = Capsule->GetComponentLocation() - Capsule->GetUpVector() * Capsule->GetScaledCapsuleHalfHeight();
				Owner->AddActorWorldOffset(Feet - NewFeet, false);
			}
		}
	}

	// ObjDescEvent / WieldItem re-broadcast OnObjectCreated frequently. Skip the expensive
	// ClearAllMeshSections + DAT rebuild when Setup + clothing fingerprint are unchanged.
	const uint64 AppearanceHash = Object.Appearance.GetContentHash();
	const float TranslucencyKey = FMath::RoundToFloat(Object.Translucency * 255.f);
	if (bHasMesh
		&& SetupId == NewSetupId
		&& AppliedPlacementId == PlacementId
		&& (!Object.bHasServerPhysicsDescription || MotionTableId == Object.MotionTableId)
		&& AppliedAppearanceHash == AppearanceHash
		&& FMath::IsNearlyEqual(AppliedTranslucencyKey, TranslucencyKey)
		&& FMath::IsNearlyEqual(AppliedWorldScale, InWorldScale)
		&& AppliedPartCollision == bEnablePartCollision
		&& AppliedDistanceLevels == bCreatureLike)
	{
		MotionTableId = Object.MotionTableId != 0 ? Object.MotionTableId : MotionTableId;
		DefaultAnimationId = Object.DefaultAnimationId != 0 ? Object.DefaultAnimationId : DefaultAnimationId;
		bHeldKeepPlacementPose = bHeld;
		if (Object.IsCorpse() && MotionTableId != 0)
		{
			SetHeldActionMotion(ACEMotion::Dead, ACEMotion::StanceNonCombat);
		}
		else if (!bHeld && MotionTableId != 0 && ACEMotion::IsHeldRestCommand(Object.InitialMotionCommand))
		{
			SetHeldActionMotion(Object.InitialMotionCommand,Object.InitialMotionStyle ? Object.InitialMotionStyle : ACEMotion::StanceNonCombat);
		}
		const bool bCreatureLikeEarly = Object.bIsPlayer || Object.bIsSelf
			|| (Object.ItemType & ACEItemType::Creature) != 0;
		if (bCreatureLikeEarly)
		{
			SetPartsCastShadow(true, /*bInset*/ false);
		}
		return true;
	}

	const bool PreserveAnimation = bHasMesh && SetupId == NewSetupId && (Object.bIsPlayer || Object.bIsSelf || (Object.ItemType & ACEItemType::Creature));
	TArray<FTransform> PreviousParts;
	if (PreserveAnimation) for (const auto& Part : PartMeshes)
		PreviousParts.Add(Part ? Part->GetRelativeTransform() : FTransform::Identity);
	SetupId = NewSetupId;
	MotionTableId = Object.MotionTableId;
	DefaultAnimationId = Object.DefaultAnimationId;
	Appearance = Object.Appearance;
	if (!PreserveAnimation)
	{
		ActionCommand = 0;
		ActionFromCommand = ACEMotion::Ready;
		bActionUsesStateTransition = false;
		AnimTime = 0.f;
		DeferredPoseDeltaTime = 0.f;
		bHasMesh = false;
		bDoorHoldFinal = false;
		bHoldActionFinal = false;
		bHoldActionFinalAfterFinish = false;
		bResumeDefaultAnimAfterAction = false;
		bSuppressNextHookDispatch = false;
		AnimMode = EACEAnimMode::Locomotion;
		ResetHookTracking();
	}
	bHasMesh = false;
	const bool bIsCorpse = Object.IsCorpse();
	bPlayIdleMotion = bIsCorpse || bCreatureLike || bUsesOnOff;
	SetComponentTickEnabled(bPlayIdleMotion);

	if (SetupId == 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("ACEAppearance: '%s' has no SetupId — ObjectCreate physics desc had no CSetup flag, falling back to placeholder"), *Object.Name);
		return false;
	}

	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (!GI)
	{
		return false;
	}
	UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>();
	if (!Dat)
	{
		return false;
	}

	const double BuildStart = FPlatformTime::Seconds();
	const auto SharedBuilt = Dat->GetOrBuildSetupAppearanceShared(static_cast<uint32>(SetupId), Appearance, WorldScale, PlacementId,
		/*bDistanceLevels*/ bCreatureLike);
	if (!SharedBuilt || SharedBuilt->Parts.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEAppearance: failed Setup 0x%08X for '%s'"), SetupId, *Object.Name);
		return false;
	}
	const FACEBuiltSetupMesh& Built = *SharedBuilt;

	// A dissolve's saved materials belong to the previous ObjDesc. Retire them
	// before replacing the parts, otherwise UnHide restores the old character's
	// palette and texture replacements onto this character's geometry.
	if (AActor* Owner = GetOwner())
		if (auto* Scripts = Owner->FindComponentByClass<UACEScriptComponent>()) Scripts->NotifyAppearanceChanging();
	EnsurePartMeshes(Built.Parts.Num());
	PartDrawingSpheres.Reset(Built.Parts.Num());
	for (const auto& Part : Built.Parts) PartDrawingSpheres.Add(Part.DrawingSphere);
	const double MeshStart = FPlatformTime::Seconds();
	TArray<UProceduralMeshComponent*> Meshes;
	for (int32 i = 0; i < Built.Parts.Num(); ++i)
	{
		Meshes.Add(PartMeshes[i]);
		PartMeshes[i]->SetForceHighestDetail(Object.bIsSelf || bPreviewCapture);
	}

	uint32 DefaultMT = 0;
	if (!Dat->ApplySetupParts(Meshes, SetupId, Appearance, WorldScale, BindTransforms, DefaultMT,
		PlacementId, bEnablePartCollision, Object.Translucency, &Built))
	{
		return false;
	}
	// Retail set_description replaces the setup's default table even when the
	// server sends no table (zero). Resting decorative bodies must keep their
	// placement instead of acquiring the human setup's standing idle animation.
	if (MotionTableId == 0 && !Object.bHasServerPhysicsDescription)
	{
		MotionTableId = static_cast<int32>(DefaultMT);
	}
	AppearanceSourceMaterials.Reset();
	if (bUseWorldLighting)
	{
		WorldLightingInstances.Reset();
		// Share identical lighting within this body. Part-specific hooks already
		// clone to mesh-owned MIDs before mutation; other actors remain independent.
		TMap<UMaterialInterface*, UMaterialInstanceDynamic*> ActorMaterials;
		for (auto* Part : Meshes)
			if (Part) for (int32 I = 0; I < Part->GetNumMaterials(); ++I)
			{
				auto* Source=Part->GetMaterial(I);
				if (Source) AppearanceSourceMaterials.AddUnique(Source);
				auto* WorldMaterial=Dat->GetWorldObjectMaterial(Source);
				if (WorldMaterial && WorldMaterial!=Source)
				{
					auto*& Instance=ActorMaterials.FindOrAdd(WorldMaterial);
					if (!Instance)
					{
						Instance=UMaterialInstanceDynamic::Create(WorldMaterial->GetMaterial(),this);
						Instance->CopyMaterialUniformParameters(WorldMaterial);
						Dat->UpdateWorldObjectLighting(Instance,false);
						WorldLightingInstances.Add(Instance);
					}
					Part->SetMaterial(I,Instance);
				}
			}
		// Rebuilt parts can retain the former cell's channels. Reapply even
		// when the new appearance is built outdoors.
		bWorldLightingEnclosed=false;
		for (auto* Part : Meshes) if (Part) Part->SetLightingChannels(true,false,false);
		bWorldLightingInterior=false;
		UpdateCellLighting(Object.Position.CellId);
	}
	const double ApplyEnd = FPlatformTime::Seconds();
	if (ApplyEnd-BuildStart > .008)
		UE_LOG(LogTemp, Log, TEXT("ACEAppearance cost: '%s' setup=%08X decode/components=%.2fms mesh/materials=%.2fms parts=%d"),
			*Object.Name,SetupId,(MeshStart-BuildStart)*1000,(ApplyEnd-MeshStart)*1000,Built.Parts.Num());

	{
		float StepUp = 0.5f, Height = 2.f, Radius = 0.5f;
		uint32 SetupDefaultAnim = 0;
		if (Dat->TryGetSetupPhysics(static_cast<uint32>(SetupId), StepUp, Height, Radius, SetupDefaultAnim))
		{
			if (DefaultAnimationId == 0)
			{
				DefaultAnimationId = static_cast<int32>(SetupDefaultAnim);
			}
		}
	}

	// Retail HasDefaultAnim is Static-only. Held weapons keep Placement (RightHandCombat)
	// and do not run DefaultAnimLoop — those hooks StopParticle the Setup DefaultScript
	// drips (acid/frost/fire). Elemental FX comes from StartDefaultScripts instead.
	bHeldKeepPlacementPose = bHeld;
	const bool bUseDefaultAnimLoop = DefaultAnimationId != 0
		&& !bHeld
		&& !bUsesOnOff
		&& !bIsCorpse
		&& !bCreatureLike;
	if (bUseDefaultAnimLoop)
	{
		AnimMode = EACEAnimMode::DefaultAnimLoop;
		ObjectAnimFrame = FTransform::Identity;
		bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	}
	else if (bUsesOnOff && MotionTableId != 0)
	{
		AnimMode = EACEAnimMode::DoorTransition;
		bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
		// Prefer server On/Off. Do NOT infer open from Ethereal alone — closed doors can
		// still carry Ethereal in weenie PhysicsState. Default closed when unsure.
		// Pose is applied after bHasMesh below (SetDoorHeldCommand needs the mesh).
	}
	else if (bIsCorpse && MotionTableId != 0)
	{
		// Pose applied after mesh is ready (below) so bind pose never flashes.
		bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	}
	else if (bCreatureLike && MotionTableId != 0)
	{
		if (!PreserveAnimation) AnimMode = EACEAnimMode::Locomotion;
		bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	}
	else if (!bHeld && MotionTableId != 0)
	{
		// CPhysicsObj::SetMotionTableID initializes the table's default state for
		// every object, not only creatures and lifestones. Custom crystals and
		// switches can use the idle's part frames to sit above their setup origin.
		AnimMode = EACEAnimMode::Locomotion;
		bPlayIdleMotion = true;
		SetComponentTickEnabled(true);
	}

	EnsureMeshRoot();
	for (int32 i = 0; i < BindTransforms.Num(); ++i)
	{
		if (PreviousParts.IsValidIndex(i) && PartMeshes.IsValidIndex(i) && PartMeshes[i])
			PartMeshes[i]->SetRelativeTransform(PreviousParts[i]);
		else ApplyPartTransform(i, BindTransforms[i]);
	}

	HideOwnerPrimitiveMeshes();
	bHasMesh = true;
	// Object-looking NPCs use Creature too. Retail initializes their motion
	// table before drawing/picking, just as it does for ordinary animated props.
	bNeedsInitialPropPose = !bHeld && !bIsCorpse && bPlayIdleMotion;
	AppliedPlacementId = PlacementId;
	AppliedAppearanceHash = AppearanceHash;
	++AppearanceRevision;
	AppliedTranslucencyKey = TranslucencyKey;
	AppliedWorldScale = InWorldScale;
	AppliedPartCollision = bEnablePartCollision;
	AppliedDistanceLevels = bCreatureLike;

	// Doors/chests: apply held On/Off AFTER the mesh exists. Calling SetDoorHeldCommand earlier
	// returned without posing, leaving the Setup bind pose (chests often look permanently open).
	if (bUsesOnOff && MotionTableId != 0)
	{
		const int32 Cmd = (Object.InitialMotionCommand == ACEMotion::OnCommandU16
			|| Object.InitialMotionCommand == ACEMotion::OffCommandU16)
			? Object.InitialMotionCommand
			: static_cast<int32>(ACEMotion::OffCommandU16);
		SetDoorHeldCommand(Cmd);
	}

	if (bIsCorpse && MotionTableId != 0)
	{
		const int32 Style = Object.InitialMotionStyle != 0
			? Object.InitialMotionStyle
			: static_cast<int32>(ACEMotion::StanceNonCombat);
		const int32 Cmd = (Object.InitialMotionCommand == ACEMotion::DeadCommandU16
			|| Object.InitialMotionCommand == static_cast<int32>(ACEMotion::Dead))
			? Object.InitialMotionCommand
			: static_cast<int32>(ACEMotion::Dead);
		SetHeldActionMotion(Cmd, Style);
	}
	else if (!bUsesOnOff && MotionTableId != 0 && ACEMotion::IsHeldRestCommand(Object.InitialMotionCommand))
	{
		const int32 Style = Object.InitialMotionStyle != 0
			? Object.InitialMotionStyle
			: static_cast<int32>(ACEMotion::StanceNonCombat);
		SetHeldActionMotion(Object.InitialMotionCommand, Style);
	}

	// Do not camera-lock thin world meshes. Doors, pedestals, cacti, and trees are
	// authored in Setup space; yawing them toward the camera was never correct.
	bBillboardSpriteParts = false;
	// Bounds are diagnostic only; avoid transforming every vertex on each spawn.
	if (UE_LOG_ACTIVE(LogTemp, Verbose))
	{
		int32 TotalVerts = 0;
		FBox LocalBounds(ForceInit);
		for (const FACEBuiltSetupPart& Part : Built.Parts)
		{
			for (const FACEBuiltMeshSection& Sec : Part.Sections)
			{
				for (const FVector& V : Sec.Vertices)
				{
					LocalBounds += Part.BindTransform.TransformPosition(V);
					++TotalVerts;
				}
			}
		}
		const FVector Extent = TotalVerts > 0 ? LocalBounds.GetExtent() : FVector::ZeroVector;
		UE_LOG(LogTemp, Verbose, TEXT("ACEAppearance: applied '%s' setup=0x%08X parts=%d verts=%d boxExtent=(%.1f,%.1f,%.1f) meshYaw=%.0f overrides=%s"),
			*Object.Name, SetupId, Built.Parts.Num(), TotalVerts, Extent.X, Extent.Y, Extent.Z, MeshFacingYawDegrees,
			Appearance.HasVisualOverrides() ? TEXT("yes") : TEXT("no"));
	}

	if (Object.bIsPlayer || Object.bIsSelf)
	{
		int32 HeadAnim = 0;
		uint32 HeadGfx = 0;
		int32 HeadTex = 0;
		for (const FACEObjDescAnimPartChange& Ap : Appearance.AnimPartChanges)
		{
			if (Ap.PartIndex == 0x10)
			{
				++HeadAnim;
				HeadGfx = static_cast<uint32>(Ap.PartId);
			}
		}
		for (const FACEObjDescTextureChange& Tc : Appearance.TextureChanges)
		{
			if (Tc.PartIndex == 0x10)
			{
				++HeadTex;
			}
		}
		UE_LOG(LogTemp, Verbose,
			TEXT("ACEAppearance: head part0x10 animParts=%d gfx=0x%08X texChanges=%d subPals=%d paletteBase=0x%08X"),
			HeadAnim, HeadGfx, HeadTex, Appearance.SubPalettes.Num(),
			static_cast<uint32>(Appearance.PaletteBaseId));
	}
	if (bCreatureLike)
	{
		SetPartsCastShadow(true, /*bInset*/ false);
	}
	return true;
}

void UACECharacterAppearanceComponent::UpdateCellLighting(uint32 CellId)
{
	const uint32 LocalCell=CellId & 0xFFFFu;
	const bool bInterior=LocalCell>=0x100u && LocalCell<0xFFFEu;
	if (!bUseWorldLighting || (!bInterior && !bWorldLightingInterior)) return;
	auto* GI=GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	auto* Dat=GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const auto* Room=bInterior && Dat ? Dat->FindEnvCellMesh(CellId,WorldScale) : nullptr;
	const bool bEnclosed=Room && !Room->CanSeeOutside();
	if (bWorldLightingInterior==bInterior && bWorldLightingEnclosed==bEnclosed) return;
	bWorldLightingInterior=bInterior;
	bWorldLightingEnclosed=bEnclosed;
	for (UProceduralMeshComponent* Part : PartMeshes) if (Part) Part->SetLightingChannels(!bEnclosed,bEnclosed,false);
	for (UMaterialInstanceDynamic* Material:WorldLightingInstances)
	{
		if (Dat) Dat->UpdateWorldObjectLighting(Material,bInterior,bEnclosed);
	}
}

bool UACECharacterAppearanceComponent::HasAuthoredPhysicsGeometry() const
{
	for (UProceduralMeshComponent* Proc : PartMeshes)
		if (Proc) for (int32 I = 0; I < Proc->GetNumSections(); ++I)
		{
			const auto* Section = Proc->GetProcMeshSection(I);
			if (Section && Section->ProcIndexBuffer.Num() >= 3 &&
				Proc->ComponentTags.Contains(FName(*FString::Printf(TEXT("ACEPhysicsSection_%d"), I)))) return true;
		}
	return false;
}

void UACECharacterAppearanceComponent::ConfigurePartCollision(bool bBlocking, bool bQueryVisibilityOnly, bool bSelectOnMesh)
{
	for (UProceduralMeshComponent* Proc : PartMeshes)
	{
		if (!Proc || !Proc->GetNumSections())
		{
			continue;
		}
		const bool bComplexChanged = !Proc->bUseComplexAsSimpleCollision;
		if (bBlocking || bQueryVisibilityOnly || bSelectOnMesh)
			Proc->bUseComplexAsSimpleCollision = true;
		for (int32 Index = 0; Index < Proc->GetNumSections(); ++Index)
		{
			const FProcMeshSection* Section = Proc->GetProcMeshSection(Index);
			if (!Section) continue;
			const bool bPhysics = Proc->ComponentTags.Contains(
				FName(*FString::Printf(TEXT("ACEPhysicsSection_%d"), Index)));
			const bool bCollide = bBlocking ? bPhysics : (!bPhysics && Proc->IsHighestDetailSection(Index) && (bQueryVisibilityOnly || bSelectOnMesh));
			if (Section->bEnableCollision != bCollide)
			{
				FProcMeshSection Updated = *Section;
				Updated.bEnableCollision = bCollide;
				Proc->SetProcMeshSection(Index, Updated);
			}
		}
		if (bBlocking)
		{
			Proc->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Proc->SetCollisionObjectType(ECC_WorldStatic);
			Proc->SetCollisionResponseToAllChannels(ECR_Block);
			Proc->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			// Doors: Visibility on the DAT mesh so loot behind isn't occluded by a fat capsule.
			// Other props: capsule CollisionProxy handles hover/select.
			Proc->SetCollisionResponseToChannel(ECC_Visibility,
				bSelectOnMesh ? ECR_Block : ECR_Ignore);
			Proc->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
			Proc->bUseComplexAsSimpleCollision = true;
			// Forge / lifestone / static props: block the pawn but do not allow step-up —
			// TryStepUp onto complex meshes caused shake/trap/pass-through. Doors keep Yes.
			Proc->CanCharacterStepUpOn = bSelectOnMesh ? ECB_Yes : ECB_No;
			if (bComplexChanged) Proc->RecreatePhysicsState();
		}
		else if (bQueryVisibilityOnly || bSelectOnMesh)
		{
			// Open / ethereal door: walk through, still pick the mesh silhouette.
			Proc->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Proc->SetCollisionObjectType(ECC_WorldDynamic);
			Proc->SetCollisionResponseToAllChannels(ECR_Ignore);
			Proc->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
			Proc->bUseComplexAsSimpleCollision = true;
			if (bComplexChanged) Proc->RecreatePhysicsState();
		}
		else
		{
			Proc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
}

void UACECharacterAppearanceComponent::InterruptCastWithMovement()
{
	if (AnimMode != EACEAnimMode::ActionOneShot) return;
	if (UsesClassicCasting())
	{
		// A forward command or server Ready replaces the cyclic tail. Retain
		// the entry and queued windups, then play the gesture's exit to Ready.
		if (!PendingActionCommands.IsEmpty())
		{
			if (ACEIsMagicCastCommand(PendingActionCommands.Last()))
				QueueActionMotion(ACEMotion::Ready, 1.f, PendingActionStyles.Last(), false);
		}
		else if (ACEIsMagicCastCommand(ActionCommand))
			PlayActionMotion(ACEMotion::Ready, 1.f, ActionStyle, false);
		return;
	}
	// CMotionTable::GetObjectSequence / CSequence::remove_cyclic_anims retain
	// unfinished links. Only the held cyclic tail can disappear immediately.
	// Sidestep/turn are modifiers; neither they nor a fast-buff tap truncate
	// the authored cast entry or queued PowerUp actions.
	if (ACEIsMagicCastCommand(ActionCommand))
	{
		bHoldActionFinalAfterFinish = false;
		if (bHoldActionFinal)
		{
			ClearActionMotion();
		}
	}
}

void UACECharacterAppearanceComponent::SetWalkingTransitionsEnabled(bool bEnabled)
{
	bWalkingTransitionsEnabled = bEnabled;
	if (!bEnabled) WalkingMotion.Reset();
}

FTransform UACECharacterAppearanceComponent::AdvanceWalkingMotion(float Dt, float& CycleSeconds)
{
	bWalkingExternallyAdvanced = true;
	return WalkingMotion.Advance(Dt, CycleSeconds);
}

void UACECharacterAppearanceComponent::SetRemoteWalkingFrame(const ACEWalkingMotion::FLink& Link, float CycleSeconds, bool bHadTransition, float CycleTime)
{
	WalkingMotion.Display = Link;
	bRemoteWalkingFramePending = true;
	if (AnimMode == EACEAnimMode::ActionOneShot && ActionCommand == ACEMotion::Dead && !bHoldActionFinal)
    {
        // The unfinished walk exit runs before Dead in CSequence. Advance the
        // fall only by the remaining time, on the same clock as its physics root.
        AnimTime += CycleSeconds*ActionPlayRate;
        if (bHadTransition) { StanceBlendFrom.Reset(); StanceBlendAlpha = 1.f; }
        return;
    }
	if (AnimMode != EACEAnimMode::Locomotion) return;
	// The physics clock continues even when distance throttles mesh evaluation.
	// Only the unconsumed tail of the step belongs to the following cycle.
	if (bHadTransition)
	{
		AnimTime = 0.f;
		StanceBlendFrom.Reset();
		StanceBlendAlpha = 1.f;
		bWasLocomotionMoving = !FMath::IsNearlyZero(LocomotionForward) || !FMath::IsNearlyZero(LocomotionStrafe);
	}
	AnimTime = CycleTime >= 0.f ? CycleTime : AnimTime + CycleSeconds * LocomotionPlayRate;
}

void UACECharacterAppearanceComponent::SetTurnInput(float Turn)
{
	if (FMath::Sign(Turn)!=FMath::Sign(LocomotionTurn) && AnimMode==EACEAnimMode::Locomotion
		&& FMath::IsNearlyZero(LocomotionForward) && FMath::IsNearlyZero(LocomotionStrafe))
	{ AnimTime=0.f; LocomotionTurnTime=0.f; ResetHookTracking(); }
	LocomotionTurn=Turn;
}

bool UACECharacterAppearanceComponent::IsMovementLockedByAction() const
{
	return AnimMode==EACEAnimMode::ActionOneShot && !bHoldActionFinal
		&& (ACEAttackMotion::IsAction(ActionCommand) || (ActionCommand&0xFF000000u)==0x80000000u);
}

bool UACECharacterAppearanceComponent::HasPendingCombatForwardMotion() const
{
	if (AnimMode != EACEAnimMode::ActionOneShot || bHoldActionFinal) return false;
	return ACEIsMissileSubstate(ActionCommand)
		|| (bActionUsesStateTransition && ACEIsMissileSubstate(ActionFromCommand))
		|| IsMovementLockedByAction();
}

bool UACECharacterAppearanceComponent::UsesClassicCasting() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	return ACERuntimeOptions::ClassicMovement() && Pawn && Pawn->IsLocallyControlled();
}

bool UACECharacterAppearanceComponent::HasPendingCastForwardMotion() const
{
	return bHasMesh && AnimMode == EACEAnimMode::ActionOneShot && !bHoldActionFinal
		&& (ACEIsMagicPowerUp(ActionCommand) || ACEIsMagicCastCommand(ActionCommand)
			|| (bActionUsesStateTransition && ACEIsMagicCastCommand(ActionFromCommand)));
}

void UACECharacterAppearanceComponent::SetLocomotionInput(float Forward, float Strafe, bool bRunning, float PlayRate, bool bInterpretedRate)
{
	if (AnimMode == EACEAnimMode::DoorTransition)
	{
		return;
	}
	if (AnimMode == EACEAnimMode::DefaultAnimLoop)
	{
		// Static props ignore loco. Creatures mis-classified as DefaultAnimLoop promote.
		if (MotionTableId == 0 || (FMath::IsNearlyZero(Forward) && FMath::IsNearlyZero(Strafe)))
		{
			return;
		}
		AnimMode = EACEAnimMode::Locomotion;
		bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	}
	if (!bWalkingTransitionsEnabled || bVRPoseControlled || bInterpretedRate
		|| AnimMode != EACEAnimMode::Locomotion)
		WalkingMotion.Reset();
	else if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
		if (auto* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			WalkingMotion.SetDirection(FMath::IsNearlyZero(Forward) ? 0 : Forward < 0.f ? -1 : bRunning ? 2 : 1,
				[&](int8 From, int8 To, ACEWalkingMotion::FLink& Link)
				{
					if (!Dat->BuildWalkingLink(MotionTableId, From, To, PreferredStyle, Link)) return false;
					const float Scale = To ? FMath::Max(.05f,PlayRate) : 1.f;
					for (auto& Clip:Link.Clips) Clip.Framerate*=Scale;
					for (auto& Key:Link.Root.Keys) Key.Time/=Scale;
					return true;
				});
	// CMotionTable replaces the previous cyclic sequence at its starting frame.
	// This applies to forward movement as well as sidesteps: repeated short nav
	// segments otherwise spend most of their travel blending out of idle.
	auto Direction = [bInterpretedRate](float F, float R) -> int8
	{
		if (FMath::IsNearlyZero(F) && FMath::IsNearlyZero(R)) return 0;
		if (bInterpretedRate && !FMath::IsNearlyZero(F)) return F > 0.f ? 1 : -1;
		return FMath::Abs(R) > FMath::Abs(F) ? (R > 0.f ? 2 : -2) : (F > 0.f ? 1 : -1);
	};
	const int8 NewDirection = Direction(Forward, Strafe);
	if (AnimMode == EACEAnimMode::Locomotion && NewDirection != 0
		&& NewDirection != Direction(LocomotionForward, LocomotionStrafe))
	{
		AnimTime = DeferredPoseDeltaTime = 0.f;
		StanceBlendFrom.Reset();
		StanceBlendAlpha = 1.f;
		bWasLocomotionMoving = true;
		LastLocomotionFwdSign = FMath::Abs(NewDirection) == 1 ? NewDirection : 0;
		ResetHookTracking();
	}
	// ActionOneShot still records loco so we resume the right cycle when the attack ends.
	LocomotionForward = Forward;
	LocomotionStrafe = Strafe;
	bLocomotionRunning = bRunning;
	LocomotionPlayRate = FMath::Max(0.05f, PlayRate);
	bLocomotionInterpretedRate = bInterpretedRate;
	// Snap run blend so remotes don't linger in a walk/run interpolate while charging.
	if (bRunning && !FMath::IsNearlyZero(Forward))
	{
		RunBlend = 1.f;
	}
	else if (!bRunning)
	{
		RunBlend = 0.f;
	}
	// Forward movement replaces the forward substate. Retail sidestep and turn
	// are independent channels: a player can slide while holding Play Possum.
	// Jump/Falling must NOT cancel: airborne + W still feeds locomotion axes for land resume,
	// and clearing Falling mid-air made the run cycle play instead of the jump.
	// Ignore analog/camera noise: IsNearlyZero cancelled *point* / *playpossum* on the next tick.
	const bool bJumpHold = ActionCommand == 0x1000004bu // Jumpup
		|| ActionCommand == 0x40000015u // Falling
		|| ActionCommand == 0x4000001du; // JumpCharging
	const bool bMoving = FMath::Abs(Forward) > 0.15f;
	const bool bIdleTwitch = (ActionCommand >= 0x10000051u && ActionCommand <= 0x10000054u)
		|| (ActionCommand >= 0x400000E4u && ActionCommand <= 0x400000E6u);
	if ((bHoldActionFinal || bHoldActionFinalAfterFinish || bIdleTwitch) && AnimMode == EACEAnimMode::ActionOneShot
		&& !bJumpHold
		&& ActionCommand != ACEMotion::Dead
		&& !(ACEIsMagicCastCommand(ActionCommand) && UsesClassicCasting())
		&& bMoving)
	{
		if (bIdleTwitch) ClearActionMotion();
		else CancelHeldActionMotion();
	}
}

void UACECharacterAppearanceComponent::SetPreferredStyle(int32 Style)
{
	// MotionStance values are 0x8000… — negative when stored as signed int32.
	const uint32 AsU32 = static_cast<uint32>(Style);
	if (AsU32 == 0)
	{
		return;
	}
	uint32 Expanded = AsU32;
	if (Expanded <= 0xFFFFu)
	{
		Expanded = ACEMotion::ExpandPackedCommand(static_cast<uint16>(Expanded));
	}
	// Only accept MotionStance family (0x8000…).
	if ((Expanded & 0xFF000000u) != 0x80000000u)
	{
		return;
	}
	if (Expanded == PreferredStyle)
	{
		return;
	}
	WalkingMotion.Reset();

	// Capture the current visual pose so the next ticks can cross-fade into the new stance.
	StanceBlendFrom.SetNum(PartMeshes.Num());
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (PartMeshes[i])
		{
			StanceBlendFrom[i] = PartMeshes[i]->GetRelativeTransform();
		}
		else if (BindTransforms.IsValidIndex(i))
		{
			StanceBlendFrom[i] = BindTransforms[i];
		}
		else
		{
			StanceBlendFrom[i] = FTransform::Identity;
		}
	}
	StanceBlendAlpha = 0.f;
	PoseBlendDuration = StanceBlendDuration;
	const uint32 PreviousStyle = PreferredStyle;
	PreferredStyle = Expanded;
	if (PreviousStyle != 0
		&& AnimMode != EACEAnimMode::DoorTransition && ActionCommand != ACEMotion::Dead)
	{
		// Every stance change uses its authored linkage, including sheathing
		// melee weapons. The linkage owns the pause before the next Ready cycle.
		PlayActionMotion(Expanded, 1.f, PreviousStyle, false);
	}
}

void UACECharacterAppearanceComponent::BeginPoseBlendFromCurrent(float Duration)
{
	if (Duration <= 0.f)
	{
		StanceBlendFrom.Reset();
		StanceBlendAlpha = 1.f;
		return;
	}
	PoseBlendDuration = FMath::Max(.001f, Duration);
	StanceBlendFrom.SetNum(PartMeshes.Num());
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (PartMeshes[i])
		{
			StanceBlendFrom[i] = PartMeshes[i]->GetRelativeTransform();
		}
		else if (BindTransforms.IsValidIndex(i))
		{
			StanceBlendFrom[i] = BindTransforms[i];
		}
		else
		{
			StanceBlendFrom[i] = FTransform::Identity;
		}
	}
	StanceBlendAlpha = 0.f;
}

void UACECharacterAppearanceComponent::ApplyAnimatedPartsWithBlend(
	const TArray<FTransform>& Animated, int32 AnimatedCount, float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_ApplyAnimatedParts);
	const bool bBlend = StanceBlendAlpha < 1.f - KINDA_SMALL_NUMBER && StanceBlendFrom.Num() == PartMeshes.Num();
	if (bBlend)
	{
		StanceBlendAlpha = FMath::Clamp(StanceBlendAlpha + DeltaTime / PoseBlendDuration, 0.f, 1.f);
	}
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (i < AnimatedCount && Animated.IsValidIndex(i))
		{
			// Blend directly into the part being applied. The source pose remains
			// immutable; no full-pose allocation/copy is needed during transitions.
			FTransform PartXform = Animated[i];
			if (bBlend) PartXform.Blend(StanceBlendFrom[i], Animated[i], StanceBlendAlpha);
			if (BindTransforms.IsValidIndex(i))
			{
				PartXform.SetScale3D(BindTransforms[i].GetScale3D());
			}
			ApplyPartTransform(i, PartXform);
		}
		else if (BindTransforms.IsValidIndex(i))
		{
			ApplyPartTransform(i, BindTransforms[i]);
		}
	}
	if (bBlend && StanceBlendAlpha >= 1.f - KINDA_SMALL_NUMBER)
	{
		StanceBlendFrom.Reset();
		StanceBlendAlpha = 1.f;
	}
}

void UACECharacterAppearanceComponent::PlayActionMotion(int32 InActionCommand, float PlayRate, int32 Style, bool bHoldFinalPose, bool bNewAction)
{
	// Remotes stuck in DefaultAnimLoop (missing Creature flag, etc.) must still play cast/attack.
	if (AnimMode == EACEAnimMode::DefaultAnimLoop)
	{
		bResumeDefaultAnimAfterAction = DefaultAnimationId != 0;
		AnimMode = EACEAnimMode::Locomotion;
	}
	if (AnimMode == EACEAnimMode::DoorTransition)
	{
		return;
	}
	uint32 Cmd = static_cast<uint32>(InActionCommand);
	if (Cmd == 0)
	{
		return;
	}
	// Props may have been quiescent since spawn. A lever's Twitch1 is an
	// ordinary network motion; receiving it must wake pose evaluation too.
	bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	if (Cmd <= 0xFFFFu)
	{
		Cmd = ACEMotion::ExpandPackedCommand(static_cast<uint16>(Cmd));
	}
	// CMotionTable appends action links before the existing cyclic tail.
	// A Wave/ClapHands action returns to that cycle; only substates hold.
	if (Cmd & 0x10000000u) bHoldFinalPose = false;
	const bool bMagicPowerUp = ACEIsMagicPowerUp(Cmd);
	const bool bMagicCastGesture = ACEIsMagicCastCommand(Cmd);
	const bool bCurPowerUp = ACEIsMagicPowerUp(ActionCommand);
	// Scarab windups own HighFrame particle tiers (frame 9/19/29…). Never cut them short
	// when the cast gesture arrives — queue it (and chained PowerUps) instead.
	if (AnimMode == EACEAnimMode::ActionOneShot && !bHoldActionFinal
		&& (bCurPowerUp || ACEIsMagicCastCommand(ActionCommand)
			|| (bActionUsesStateTransition && ACEIsMagicCastCommand(ActionFromCommand) && UsesClassicCasting()))
		&& (bMagicCastGesture || bMagicPowerUp) && ActionCommand != Cmd)
	{
		QueueActionMotion(static_cast<int32>(Cmd), PlayRate, Style, bHoldFinalPose);
		return;
	}
	// State echoes must not restart held poses. A newly stamped action (e.g.
	// another crafting ClapHands) is a distinct performance, even without movement.
	if (AnimMode == EACEAnimMode::ActionOneShot && ActionCommand == Cmd)
	{
		if (!bNewAction)
		{
			// A repeated interpreted substate can change speed without restarting
			// its entry or cyclic pose (CMotionInterp::apply_interpreted_movement).
			if (!(Cmd & 0x10000000u)) ActionPlayRate = FMath::Max(0.f, PlayRate);
			return;
		}
		if (!bHoldActionFinal)
		{
			QueueActionMotion(Cmd, PlayRate, Style, bHoldFinalPose);
			return;
		}
	}
	const bool bFromEmote = AnimMode == EACEAnimMode::ActionOneShot && ACEIsEmoteSubstate(ActionCommand)
		&& !(bNewAction && ActionCommand == Cmd);
	if (bNewAction && AnimMode == EACEAnimMode::ActionOneShot && !bHoldActionFinal
		&& (ActionCommand & 0x10000000u) && (Cmd & 0x10000000u))
	{
		// CMotionTable::GetObjectSequence appends actions before the cyclic
		// tail. A new gesture or swing must not truncate the preceding action.
		QueueActionMotion(Cmd, PlayRate, Style, bHoldFinalPose);
		return;
	}
	const bool bFromMissile = AnimMode == EACEAnimMode::ActionOneShot && ACEIsMissileSubstate(ActionCommand)
		&& (ACEIsMissileSubstate(Cmd) || Cmd == ACEMotion::Ready);
	const bool bFromCast = AnimMode == EACEAnimMode::ActionOneShot && ACEIsMagicCastCommand(ActionCommand)
		&& (Cmd == ACEMotion::Ready || bMagicCastGesture || bMagicPowerUp) && UsesClassicCasting();
	if (AnimMode == EACEAnimMode::ActionOneShot && (ActionCommand & 0xFF000000u) == 0x80000000u
		&& ACEIsMissileSubstate(Cmd))
	{
		if (PendingActionCommands.IsEmpty() || PendingActionCommands.Last() != Cmd)
			QueueActionMotion(Cmd, PlayRate, Style, bHoldFinalPose);
		return;
	}
	const bool bToEmoteOrReady = (ACEMotion::IsHeldRestCommand(Cmd) && Cmd != ACEMotion::Dead) || Cmd == ACEMotion::Ready;
	if (AnimMode == EACEAnimMode::ActionOneShot && !bHoldActionFinal
		&& (bFromCast || bFromMissile || ((bToEmoteOrReady || (Cmd & 0x80000000u)) && (bFromEmote || bActionUsesStateTransition))))
	{
		// Retail removes only the cyclic tail, not an unfinished entry/exit.
		// Repeated movement/Ready echoes must not append another stand-up sequence.
		if (PendingActionCommands.IsEmpty() || PendingActionCommands.Last() != Cmd)
			QueueActionMotion(Cmd, PlayRate, Style, bHoldFinalPose || ACEMotion::IsHeldRestCommand(Cmd));
		bHoldActionFinalAfterFinish = false;
		return;
	}
	ActionFromCommand = bFromEmote || bFromMissile || bFromCast ? ActionCommand : ACEMotion::Ready;
	WalkingMotion.Reset();
	bActionUsesStateTransition = bFromEmote || bFromMissile || bFromCast || (Cmd & 0x80000000u);
	// Falling already contains retail's takeoff link (~0.2 s). A full stance
	// cross-fade hid it behind the previous jump's frozen endpoint.
	// Attack links already contain their authored approach and recovery poses.
	// A wall-clock stance blend flattens the height/arc, especially at fast speeds.
	BeginPoseBlendFromCurrent(ACEAttackMotion::IsAttack(Cmd) ? 0.f
		: bFromEmote || bFromMissile || ACEIsMissileSubstate(Cmd) || Cmd == 0x40000015u ? .06f : StanceBlendDuration);
	AnimMode = EACEAnimMode::ActionOneShot;
	ActionCommand = Cmd;
	PendingActionCommands.Reset();
	PendingActionStyles.Reset();
	PendingActionPlayRates.Reset();
	PendingActionHolds.Reset();
	bHoldActionFinal = false;
	bHoldActionFinalAfterFinish = bHoldFinalPose;
	// Held soul-emote substates keep their endpoint until the next forward
	// command. One-shot social actions have already returned through their link.
	const uint32 HoldFam = Cmd & 0xFF000000u;
	if (HoldFam == 0x42000000u || HoldFam == 0x43000000u
		|| ACEMotion::IsHeldRestCommand(Cmd) || Cmd == 0x40000015u || Cmd == 0x4000001Du)
	{
		bHoldActionFinalAfterFinish = true;
	}
	// MotionStance is 0x8000… (negative as int32) — do not use Style > 0.
	ActionStyle = static_cast<uint32>(Style);
	if (ActionStyle != 0 && ActionStyle <= 0xFFFFu)
	{
		ActionStyle = ACEMotion::ExpandPackedCommand(static_cast<uint16>(ActionStyle));
	}
	if ((ActionStyle & 0xFF000000u) != 0x80000000u)
	{
		ActionStyle = PreferredStyle != 0 ? PreferredStyle : ACEMotion::StanceNonCombat;
	}
	// Magic windups / casts: prefer the SERVER stance (HandCombat for most monsters).
	// Only fall back to Magic when style is unset / NonCombat (player scarab casts).
	// Forcing Magic for every cast made monster CastSpell resolve as melee via MT fallbacks.
	if ((bMagicPowerUp || bMagicCastGesture)
		&& (ActionStyle == 0 || ActionStyle == ACEMotion::StanceNonCombat))
	{
		ActionStyle = ACEMotion::StanceMagic;
	}
	ActionPlayRate = FMath::Max(0.f, PlayRate);
	// Local chat requests supply 1.0; received motions supply their own rate.
	// Do not overwrite an NPC emote, pickup, or combat action's server speed.
	if (bMagicPowerUp || bMagicCastGesture)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("ACEAnim: PlayAction 0x%08X style=0x%08X rate=%.2f (PowerUp=%d Cast=%d)"),
			Cmd, ActionStyle, ActionPlayRate, bMagicPowerUp ? 1 : 0, bMagicCastGesture ? 1 : 0);
	}
	bActionEverEvaluated = false;
	AnimTime = 0.f;
	DeferredPoseDeltaTime = 0.f;
	bSuppressNextHookDispatch = false;
	ResetHookTracking();
}

void UACECharacterAppearanceComponent::QueueActionMotion(int32 InActionCommand, float PlayRate, int32 Style, bool bHoldFinalPose)
{
	uint32 Cmd = static_cast<uint32>(InActionCommand);
	if (Cmd == 0)
	{
		return;
	}
	if (Cmd <= 0xFFFFu)
	{
		Cmd = ACEMotion::ExpandPackedCommand(static_cast<uint16>(Cmd));
	}
	bHoldActionFinalAfterFinish = false; // A queued action must run before the final hold.
	PendingActionCommands.Add(Cmd);
	PendingActionPlayRates.Add(FMath::Max(0.f, PlayRate));
	uint32 St = static_cast<uint32>(Style);
	if (St != 0 && St <= 0xFFFFu)
	{
		St = ACEMotion::ExpandPackedCommand(static_cast<uint16>(St));
	}
	PendingActionStyles.Add(St);
	PendingActionHolds.Add(bHoldFinalPose && !(Cmd & 0x10000000u));
}

void UACECharacterAppearanceComponent::FinishMissileMotion()
{
	if (AnimMode != EACEAnimMode::ActionOneShot) return;
	if (ACEIsMissileSubstate(ActionCommand))
		PlayActionMotion(ACEMotion::Ready, 1.f, ActionStyle, false);
	else if (!PendingActionCommands.IsEmpty() && ACEIsMissileSubstate(PendingActionCommands.Last()))
		QueueActionMotion(ACEMotion::Ready, 1.f, PendingActionStyles.Last(), false);
}

void UACECharacterAppearanceComponent::CancelHeldActionMotion()
{
	// Explicit Ready or a new forward command replaces a held chat pose.
	// Monster missile tables also contain zero-rate Aim/Reload endpoint cycles.
	// A drudge has no Ready->Reload link, so that fallback never sets bFinished
	// or bHoldActionFinal. The server's Ready must release it just like other holds.
	const bool bMissileEndpoint = ActionCommand == 0x40000016u
		|| (ActionCommand >= 0x4000001Eu && ActionCommand <= 0x4000002Au);
	if (AnimMode == EACEAnimMode::ActionOneShot && (bMissileEndpoint
		|| (!PendingActionCommands.IsEmpty() && ACEIsMissileSubstate(PendingActionCommands.Last()))))
	{
		FinishMissileMotion();
		return;
	}
	if (AnimMode == EACEAnimMode::ActionOneShot && ACEIsEmoteSubstate(ActionCommand))
	{
		PlayActionMotion(ACEMotion::Ready, 1.f, ActionStyle, false);
		return;
	}
	if (AnimMode == EACEAnimMode::ActionOneShot && (bHoldActionFinal || bHoldActionFinalAfterFinish || bMissileEndpoint))
	{
		bHoldActionFinal = false;
		bHoldActionFinalAfterFinish = false;
		BeginPoseBlendFromCurrent();
		AnimMode = EACEAnimMode::Locomotion;
		ActionCommand = 0;
		ActionFromCommand = ACEMotion::Ready;
		bActionUsesStateTransition = false;
		AnimTime = 0.f;
		DeferredPoseDeltaTime = 0.f;
		ResetHookTracking();
	}
}

void UACECharacterAppearanceComponent::FinishEmoteMotion()
{
	if (AnimMode == EACEAnimMode::ActionOneShot && ActionCommand != ACEMotion::Dead
		&& ACEMotion::IsHeldRestCommand(ActionCommand)) CancelHeldActionMotion();
}

bool UACECharacterAppearanceComponent::IsJumpBlockedByMotion(bool bCharging) const
{
	if (AnimMode != EACEAnimMode::ActionOneShot) return false;
	if (bCharging) return ACEMotion::BlocksJumpCharge(ActionCommand);
	// Retail pending_motions retains the source's jump error while standing up.
	return ACEMotion::BlocksJumpRelease(ActionCommand)
		|| (!bHoldActionFinal && bActionUsesStateTransition && ACEMotion::BlocksJumpRelease(ActionFromCommand));
}

void UACECharacterAppearanceComponent::QueueHeldActionAfterCurrent(int32 InActionCommand, int32 Style)
{
	uint32 Cmd = static_cast<uint32>(InActionCommand);
	if (Cmd == 0)
	{
		QueuedHoldAction = 0;
		QueuedHoldStyle = 0;
		return;
	}
	if (Cmd <= 0xFFFFu)
	{
		Cmd = ACEMotion::ExpandPackedCommand(static_cast<uint16>(Cmd));
	}
	QueuedHoldAction = Cmd;
	uint32 St = static_cast<uint32>(Style);
	if (St != 0 && St <= 0xFFFFu)
	{
		St = ACEMotion::ExpandPackedCommand(static_cast<uint16>(St));
	}
	if ((St & 0xFF000000u) != 0x80000000u)
	{
		St = PreferredStyle != 0 ? PreferredStyle : ACEMotion::StanceNonCombat;
	}
	QueuedHoldStyle = St;
}

void UACECharacterAppearanceComponent::SetSuppressLocoIdleBlend(bool bSuppress)
{
	bSuppressLocoIdleBlend = bSuppress;
	if (bSuppress)
	{
		// Keep "moving" so we don't capture a Ready blend mid-jump.
		bWasLocomotionMoving = true;
	}
	else
	{
		// Align with actual axes — releasing WASD during jump must blend to Ready on land.
		bWasLocomotionMoving = !FMath::IsNearlyZero(LocomotionForward)
			|| !FMath::IsNearlyZero(LocomotionStrafe);
	}
}

bool UACECharacterAppearanceComponent::BuildLandingRootTrack(ACELandingMotion::FRootTrack& Out) const
{
	Out.Reset();
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	return Dat && bActionUsesStateTransition && ActionFromCommand == 0x40000015u
		&& Dat->BuildTransitionRootTrack(MotionTableId, ActionFromCommand, ActionCommand, PreferredStyle, Out);
}

void UACECharacterAppearanceComponent::ClearJumpMotionIfAny(bool bLanded, int32 ForwardPose)
{
	constexpr uint32 Jumpup = 0x1000004bu;
	constexpr uint32 Falling = 0x40000015u;
	constexpr uint32 JumpCharging = 0x4000001du;
	const bool bJumpCmd = ActionCommand == Jumpup || ActionCommand == Falling || ActionCommand == JumpCharging;
	const bool bQueuedJump = QueuedHoldAction == Jumpup || QueuedHoldAction == Falling || QueuedHoldAction == JumpCharging;
	if (!bJumpCmd && !bQueuedJump)
	{
		return;
	}
	QueuedHoldAction = 0;
	QueuedHoldStyle = 0;
	bSuppressLocoIdleBlend = false;
	bWasLocomotionMoving = !FMath::IsNearlyZero(LocomotionForward)
		|| !FMath::IsNearlyZero(LocomotionStrafe);
	if (AnimMode == EACEAnimMode::ActionOneShot && bJumpCmd)
	{
		if (bLanded && ActionCommand != JumpCharging)
		{
			// CMotionInterp::HitGround removes the airborne links, then applies
			// current movement through the motion table. Falling->Ready supplies
			// the kneel/recovery; Falling->Run/Walk supplies the moving landing.
			const uint32 Destination = LocomotionForward > KINDA_SMALL_NUMBER
				? (bLocomotionRunning ? 0x44000007u : 0x45000005u)
				: LocomotionForward < -KINDA_SMALL_NUMBER ? 0x45000006u
				: ACEMotion::IsHeldPoseSubstate(ForwardPose) ? ForwardPose : ACEMotion::Ready;
			PlayActionMotion(Destination, 1.f, PreferredStyle, ACEMotion::IsHeldPoseSubstate(Destination));
			ActionFromCommand = Falling;
			bActionUsesStateTransition = true;
			BeginPoseBlendFromCurrent(.06f);
			return;
		}
		// Cancellation/teleport cleanup does not imply a ground contact.
		BeginPoseBlendFromCurrent(.08f);
		AnimMode = EACEAnimMode::Locomotion;
		ActionCommand = 0;
		ActionFromCommand = ACEMotion::Ready;
		bActionUsesStateTransition = false;
		bHoldActionFinal = false;
		bHoldActionFinalAfterFinish = false;
		bActionEverEvaluated = false;
		AnimTime = 0.f;
		DeferredPoseDeltaTime = 0.f;
		ResetHookTracking();
	}
}

void UACECharacterAppearanceComponent::ClearDeathMotion()
{
	if (AnimMode != EACEAnimMode::ActionOneShot || ActionCommand != ACEMotion::Dead) return;
	ClearActionMotion();
	bHoldActionFinalAfterFinish = false;
	PendingActionCommands.Reset();
	PendingActionStyles.Reset();
	PendingActionPlayRates.Reset();
	PendingActionHolds.Reset();
}

void UACECharacterAppearanceComponent::ClearActionMotion()
{
	if (AnimMode != EACEAnimMode::ActionOneShot)
	{
		return;
	}
	QueuedHoldAction = 0;
	QueuedHoldStyle = 0;
	bSuppressLocoIdleBlend = false;
	BeginPoseBlendFromCurrent();
	AnimMode = EACEAnimMode::Locomotion;
	ActionCommand = 0;
	ActionFromCommand = ACEMotion::Ready;
	bActionUsesStateTransition = false;
	bHoldActionFinal = false;
	bHoldActionFinalAfterFinish = false;
	PendingActionCommands.Reset();
	PendingActionStyles.Reset();
	PendingActionPlayRates.Reset();
	PendingActionHolds.Reset();
	AnimTime = 0.f;
	DeferredPoseDeltaTime = 0.f;
	ResetHookTracking();
}

void UACECharacterAppearanceComponent::ResetMotionForTeleport()
{
	// Retail removes pending actions/link animations on world exit. A reused
	// pawn must also discard the old locomotion and blend source: zero input
	// alone cannot interrupt a Falling->Run landing or queued one-shot.
	ClearActionMotion();
	WalkingMotion.Reset();
	bWalkingExternallyAdvanced = false;
	bRemoteWalkingFramePending = false;
	LocomotionForward = LocomotionStrafe = 0.f;
	LocomotionPlayRate = 1.f;
	bLocomotionRunning = bLocomotionInterpretedRate = false;
	RunBlend = 0.f;
	LastLocomotionFwdSign = 0;
	bWasLocomotionMoving = bSuppressLocoIdleBlend = false;
	AnimMode = EACEAnimMode::Locomotion;
	ActionCommand = QueuedHoldAction = QueuedHoldStyle = 0;
	ActionFromCommand = ACEMotion::Ready;
	bActionUsesStateTransition = bHoldActionFinal = bHoldActionFinalAfterFinish = false;
	bActionEverEvaluated = bResumeDefaultAnimAfterAction = false;
	PendingActionCommands.Reset();
	PendingActionStyles.Reset();
	PendingActionPlayRates.Reset();
	PendingActionHolds.Reset();
	AnimTime = DeferredPoseDeltaTime = 0.f;
	StanceBlendFrom.Reset();
	StanceBlendAlpha = 1.f;
	ResetHookTracking();
	ResetVRLowerBody();
}

void UACECharacterAppearanceComponent::SetHeldActionMotion(int32 InActionCommand, int32 Style)
{
	uint32 Cmd = static_cast<uint32>(InActionCommand);
	if (Cmd == 0)
	{
		return;
	}
	if (Cmd <= 0xFFFFu)
	{
		Cmd = ACEMotion::ExpandPackedCommand(static_cast<uint16>(Cmd));
	}
	if (AnimMode == EACEAnimMode::ActionOneShot && bHoldActionFinal && ActionCommand == Cmd)
	{
		return;
	}
	// A received substate starts directly in its cycle; zero-rate cycles are
	// static poses. Do not blend back from the Setup bind pose on the next tick.
	StanceBlendFrom.Reset();
	StanceBlendAlpha = 1.f;
	AnimMode = EACEAnimMode::ActionOneShot;
	bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	ActionCommand = Cmd;
	ActionFromCommand = ACEMotion::Ready;
	bActionUsesStateTransition = false;
	bHoldActionFinal = true;
	bHoldActionFinalAfterFinish = false;
	ActionStyle = static_cast<uint32>(Style);
	if (ActionStyle != 0 && ActionStyle <= 0xFFFFu)
	{
		ActionStyle = ACEMotion::ExpandPackedCommand(static_cast<uint16>(ActionStyle));
	}
	if ((ActionStyle & 0xFF000000u) != 0x80000000u)
	{
		ActionStyle = PreferredStyle != 0 ? PreferredStyle : ACEMotion::StanceNonCombat;
	}
	// A newly observed resting creature already has a style in CreateObject.
	// Retain it so the first combat update can play its full activation link.
	if (PreferredStyle == 0) PreferredStyle = ActionStyle;
	ActionPlayRate = 1.f;
	bSuppressNextHookDispatch = true;
	const bool bLoopSubstate = Cmd != ACEMotion::Dead && ACEMotion::IsHeldRestCommand(Cmd);
	AnimTime = bLoopSubstate ? 0.f : 1000.f;
	ResetHookTracking();

	if (!bHasMesh || MotionTableId == 0 || PartMeshes.Num() == 0)
	{
		return;
	}
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat)
	{
		return;
	}

	TArray<FTransform> Animated;
	int32 AnimatedCount = 0;
	bool bFinished = false;
	bool bOk = false;
	// Prefer Dead cycle (resting corpse pose); fall back to Ready→Dead link final frame.
	bOk = Dat->EvaluateMotionCommand(
		static_cast<uint32>(MotionTableId), ActionCommand, AnimTime,
		PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
		nullptr, nullptr, ActionStyle, bLoopSubstate, &bFinished);
	if (!bOk)
	{
		bOk = Dat->EvaluateMotionLink(
			static_cast<uint32>(MotionTableId), ACEMotion::Ready, ActionCommand, AnimTime,
			PartMeshes.Num(), Animated, WorldScale, AnimatedCount, bFinished,
			nullptr, nullptr, ActionStyle);
	}
	if (!bOk)
	{
		return;
	}
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (i < AnimatedCount && Animated.IsValidIndex(i))
		{
			FTransform PartXform = Animated[i];
			if (BindTransforms.IsValidIndex(i))
			{
				PartXform.SetScale3D(BindTransforms[i].GetScale3D());
			}
			ApplyPartTransform(i, PartXform);
		}
		else if (BindTransforms.IsValidIndex(i))
		{
			ApplyPartTransform(i, BindTransforms[i]);
		}
	}
	RecookPartPhysics();
	const uint64 HeldTrackKey = (5ull << 60)
		| static_cast<uint64>(HashCombine(GetTypeHash(ActionCommand), GetTypeHash(ActionStyle)));
	CommitHookTime(HeldTrackKey, AnimTime);
	bSuppressNextHookDispatch = false;
}

void UACECharacterAppearanceComponent::ApplyBindPartTransforms()
{
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (BindTransforms.IsValidIndex(i))
		{
			ApplyPartTransform(i, BindTransforms[i]);
		}
	}
	RecookPartPhysics();
}

void UACECharacterAppearanceComponent::PlayDoorMotion(int32 ToCommandU16)
{
	const uint32 To = (static_cast<uint32>(ToCommandU16) <= 0xFFFFu)
		? (0x40000000u | (static_cast<uint32>(ToCommandU16) & 0xFFFFu))
		: static_cast<uint32>(ToCommandU16);
	if (To != ACEMotion::On && To != ACEMotion::Off)
	{
		return;
	}
	// Ignore duplicate On/Off while already playing or holding that pose (UpdateMotion can
	// arrive more than once for the same transition).
	if (AnimMode == EACEAnimMode::DoorTransition && DoorToCommand == To)
	{
		return;
	}
	const uint32 From = (To == ACEMotion::On) ? ACEMotion::Off : ACEMotion::On;
	AnimMode = EACEAnimMode::DoorTransition;
	bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	DoorFromCommand = From;
	DoorToCommand = To;
	bDoorHoldFinal = false;
	bDoorStartsInCycle = false;
	bSuppressNextHookDispatch = false;
	AnimTime = 0.f;
	DeferredPoseDeltaTime = 0.f;
	ResetHookTracking();
}

void UACECharacterAppearanceComponent::SetDoorHeldCommand(int32 CommandU16)
{
	const uint32 Raw = static_cast<uint32>(CommandU16);
	const uint32 Cmd = (Raw <= 0xFFFFu)
		? (0x40000000u | (Raw & 0xFFFFu))
		: Raw;
	if (Cmd != ACEMotion::On && Cmd != ACEMotion::Off)
	{
		return;
	}
	AnimMode = EACEAnimMode::DoorTransition;
	bPlayIdleMotion = true;
	SetComponentTickEnabled(true);
	DoorFromCommand = (Cmd == ACEMotion::On) ? ACEMotion::Off : ACEMotion::On;
	DoorToCommand = Cmd;
	bDoorHoldFinal = true;
	bDoorStartsInCycle = true;
	bSuppressNextHookDispatch = true;
	AnimTime = 1000.f;
	ResetHookTracking();

	if (!bHasMesh || PartMeshes.Num() == 0)
	{
		return;
	}

	auto RestoreClosedVisuals = [this]()
	{
		SetAppearanceVisible(true);
		if (UACEScriptComponent* Scripts = GetOwner()
			? GetOwner()->FindComponentByClass<UACEScriptComponent>() : nullptr)
		{
			Scripts->RestoreMeshVisuals();
		}
	};

	if (MotionTableId == 0)
	{
		if (Cmd == ACEMotion::Off)
		{
			ApplyBindPartTransforms();
			RestoreClosedVisuals();
		}
		return;
	}
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat)
	{
		return;
	}
	TArray<FTransform> Animated;
	int32 AnimatedCount = 0;
	bool bFinished = false;
	TArray<FACEDatAnimationHook> HeldHooks;
	bool bOk = Dat->EvaluateMotionLink(
		static_cast<uint32>(MotionTableId), DoorFromCommand, DoorToCommand, AnimTime,
		PartMeshes.Num(), Animated, WorldScale, AnimatedCount, bFinished,
		nullptr, &HeldHooks, ACEMotion::StanceNonCombat);
	if (!bOk)
	{
		// A zero-rate destination cycle holds its authored initial frame.
		bOk = Dat->EvaluateMotionCommand(
			static_cast<uint32>(MotionTableId), DoorToCommand, 0.f,
			PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
			nullptr, &HeldHooks, ACEMotion::StanceNonCombat, /*bLoop*/ false);
	}
	if (!bOk)
	{
		// CMotionTable rejects unsupported commands without replacing the
		// default state. Corpse-looking chests can use a human MTable with no
		// On/Off cycle at all; restoring the setup bind pose makes them stand.
		AnimMode = EACEAnimMode::Locomotion;
		AnimTime = 0.f;
		ResetHookTracking();
		if (Dat->EvaluateIdleMotion(MotionTableId, 0.f, PartMeshes.Num(), Animated, WorldScale, AnimatedCount))
			for (int32 I=0; I<AnimatedCount; ++I) ApplyPartTransform(I, Animated[I]);
		return;
	}
	// An already settled object starts in the destination cycle. Keep the link's
	// completed visual hooks below, but not its pose or historical sounds/effects.
	AnimTime = 0.f;
	DeferredPoseDeltaTime = 0.f;
	Dat->EvaluateStateMotion(static_cast<uint32>(MotionTableId), DoorFromCommand, DoorToCommand,
		AnimTime, true, PartMeshes.Num(), Animated, WorldScale, AnimatedCount, bFinished);
	for (int32 i = 0; i < PartMeshes.Num(); ++i)
	{
		if (i < AnimatedCount && Animated.IsValidIndex(i))
		{
			FTransform PartXform = Animated[i];
			if (BindTransforms.IsValidIndex(i))
			{
				PartXform.SetScale3D(BindTransforms[i].GetScale3D());
			}
			ApplyPartTransform(i, PartXform);
		}
		else if (BindTransforms.IsValidIndex(i))
		{
			ApplyPartTransform(i, BindTransforms[i]);
		}
	}
	RecookPartPhysics();
	// The next tick starts fresh cycle hooks. EvaluateStateMotion skips historical
	// link hooks for this object, so its live cycle must not be suppressed as well.
	bSuppressNextHookDispatch = false;
	// A late ObjectCreate arrives in a settled state. Apply the completed visual
	// hooks from its transition, without replaying sounds or particle bursts.
	if (UACEScriptComponent* Scripts = GetOwner()->FindComponentByClass<UACEScriptComponent>())
	{
		for (FACEDatAnimationHook Hook : HeldHooks)
		{
			if (Hook.Type == EACEAnimationHookType::TransparentPart
				|| Hook.Type == EACEAnimationHookType::Transparent
				|| Hook.Type == EACEAnimationHookType::Scale
				|| Hook.Type == EACEAnimationHookType::NoDraw)
			{
				Hook.Start=Hook.End; Hook.Time=0.f;
				Scripts->DispatchAnimationHook(Hook);
			}
		}
	}
}

void UACECharacterAppearanceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	ACE_PROFILE_SCOPE(Animation);
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bHasMesh || PartMeshes.Num() == 0)
	{
		// Particle-only scenery has no pose to evaluate. A later successful
		// ApplyWorldObject explicitly enables ticking when it installs a mesh.
		SetComponentTickEnabled(false);
		return;
	}
	if (!bPlayIdleMotion)
	{
		SetComponentTickEnabled(false);
		return;
	}
	DeferredPoseDeltaTime += DeltaTime;
	if (AActor* Owner = GetOwner())
	{
		if (!bPreviewCapture && !bPreviewAnimation && !Owner->IsA<AACELoadingScreenActor>() && !Owner->IsA<AACERegionSceneryActor>())
		{
			if (UWorld* World = GetWorld())
			{
				if (const APlayerController* PC = World->GetFirstPlayerController())
				{
					FVector Eye = Owner->GetActorLocation();
					if (PC->PlayerCameraManager)
					{
						Eye = PC->PlayerCameraManager->GetCameraLocation();
					}
					const float DistSq = static_cast<float>(
						FVector::DistSquared(Owner->GetActorLocation(), Eye));
					// Distance reduces pose work, never stops a visible actor's motion.
					// A frame-number mask dropped far animations to 12 FPS on Quest and
					// the 80 m cutoff froze running players into a sliding pose entirely.
					const auto* Entity = Cast<AACEWorldEntityActor>(Owner);
					const float Interval = DistSq <= FMath::Square(4000.f) ? 0.f
						: Entity && Entity->bIsPlayer ? 1.f / 30.f
						: DistSq <= FMath::Square(8000.f) ? 1.f / 30.f : 1.f / 15.f;
					// Never defer the first pose/visibility hooks behind distance throttling.
					if (bHookTrackValid && DeferredPoseDeltaTime + KINDA_SMALL_NUMBER < Interval)
					{
						return;
					}
				}
			}
		}
	}
	// Pose evaluation may be throttled at distance; the motion clock must not
	// run at one quarter speed or replay an old idle when the creature approaches.
	DeltaTime = DeferredPoseDeltaTime;
	DeferredPoseDeltaTime = 0.f;
	const bool RemoteWalkingFrame = bRemoteWalkingFramePending;
	ON_SCOPE_EXIT
	{
		if (RemoteWalkingFrame) WalkingMotion.Display = {};
		bRemoteWalkingFramePending = false;
	};
	const bool bNeedsMotionTable = AnimMode != EACEAnimMode::DefaultAnimLoop;
	if (bNeedsMotionTable && MotionTableId == 0)
	{
		return;
	}
	if (AnimMode == EACEAnimMode::DefaultAnimLoop && DefaultAnimationId == 0)
	{
		return;
	}
	TArray<FTransform> Animated;
	const bool bReusePose = CVarACEReusePoseBuffers.GetValueOnGameThread() != 0;
	if (bReusePose) Swap(Animated, TickPoseScratch);
	ON_SCOPE_EXIT { if (bReusePose) Swap(Animated, TickPoseScratch); };

	const bool bLoopHeldSubstate = AnimMode == EACEAnimMode::ActionOneShot && bHoldActionFinal
		&& ActionCommand != ACEMotion::Dead && ACEMotion::IsHeldRestCommand(ActionCommand);
	if ((!(AnimMode == EACEAnimMode::ActionOneShot && bHoldActionFinal) || bLoopHeldSubstate)
		&& !(RemoteWalkingFrame && (AnimMode == EACEAnimMode::Locomotion || ActionCommand == ACEMotion::Dead)))
	{
		const float Rate = AnimMode == EACEAnimMode::DoorTransition ? 1.f
			: (AnimMode == EACEAnimMode::ActionOneShot) ? ActionPlayRate : LocomotionPlayRate;
		AnimTime += DeltaTime * Rate;
	}
	// Integrate speed. Multiplying total elapsed time by the latest mouse rate
	// jumps backwards/forwards through the clip whenever that rate changes.
	if (AnimMode == EACEAnimMode::Locomotion)
		LocomotionTurnTime += DeltaTime * LocomotionPlayRate * LocomotionTurn;
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (!GI)
	{
		return;
	}
	UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>();
	if (!Dat)
	{
		return;
	}

	if (AnimMode == EACEAnimMode::DefaultAnimLoop)
	{
		TickObjectAnimFrame(DeltaTime);
		TArray<FACEDatAnimationHook> Hooks;
		int32 AnimatedCount = 0;
		const uint64 TrackKey = (1ull << 60) | static_cast<uint32>(DefaultAnimationId);
		const float* PreviousTime = GetPreviousHookTime(TrackKey, AnimTime);
		const float AnimFramerate = (SetupId == 0x02000306) ? 40.f : 30.f;
		const int32 AnimLowFrame = bPreviewAnimation ? PreviewStartFrame : (SetupId == 0x02000306) ? 1 : 0;
		if (Dat->EvaluateAnimationLoop(static_cast<uint32>(DefaultAnimationId), AnimTime, PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
			PreviousTime, &Hooks, AnimFramerate, AnimLowFrame))
		{
			CommitHookTime(TrackKey, AnimTime);
			ApplyDefaultAnimPartTransforms(Animated, AnimatedCount);
			DispatchCrossedHooks(Hooks);
		}
		return;
	}

    if (RemoteWalkingFrame && !WalkingMotion.Display.Clips.IsEmpty()
        && AnimMode == EACEAnimMode::ActionOneShot && ActionCommand == ACEMotion::Dead)
    {
        int32 Count = 0; TArray<FACEDatAnimationHook> Hooks;
        if (Dat->EvaluateWalkingLink(WalkingMotion.Display, PartMeshes.Num(), Animated, WorldScale, Count, Hooks))
        {
            ApplyAnimatedPartsWithBlend(Animated, Count, DeltaTime);
            DispatchCrossedHooks(Hooks);
            return;
        }
    }

	if (AnimMode == EACEAnimMode::ActionOneShot)
	{
		TArray<FACEDatAnimationHook> Hooks;
		int32 AnimatedCount = 0;
		bool bFinished = false;
		const uint64 TrackKey = (5ull << 60)
			| static_cast<uint64>(HashCombine(GetTypeHash(ActionFromCommand), HashCombine(GetTypeHash(ActionCommand), GetTypeHash(ActionStyle))));
		const float* PreviousTime = bSuppressNextHookDispatch ? nullptr : GetPreviousHookTime(TrackKey, AnimTime);
		// Dead must play Ready→Dead fall (Link) then hold; Dead cycle alone is the resting pose.
		// Pickup (drop / loot) also uses Ready→Pickup Links — Cycles finish too quickly vs ACE
		// GetAnimationLength(stance, Pickup, Ready) which times the server delay.
		// Other 0x4000/0x4100 actions prefer Cycles (Sit, Sleep, …). Magic cast gestures'
		// Cycles are framerate-0 end holds — prefer Ready→gesture Links so CastSpell /
		// MagicThrowMissile / etc. play the real cast (level windups already Link-first).
		const uint32 Family = ActionCommand & 0xF0000000u;
		const uint32 ActionFam8 = ActionCommand & 0xFF000000u;
		const bool bIsDead = ActionCommand == ACEMotion::Dead;
		const uint16 ActionLow = static_cast<uint16>(ActionCommand);
		const bool bIsPickup = ActionLow == 0x0018
			|| ActionLow == 0x0136 || ActionLow == 0x0137
			|| ActionLow == 0x0138 || ActionLow == 0x0139;
		const bool bIsMagicCast = ACEIsMagicCastCommand(ActionCommand);
		// Aim/Reload cycles are endpoints, just like cast gestures. Their
		// Ready -> action links contain the actual timed missile animation.
		const bool bIsMissileGesture = ActionCommand == 0x40000016u
			|| (ActionCommand >= 0x4000001Eu && ActionCommand <= 0x4000002Au);
		// Consumables likewise have a held endpoint cycle; the link is the bite/sip.
		const bool bIsConsumable = ActionLow == 0x001A || ActionLow == 0x001B;
		const bool bIsChatPose = ActionFam8 == 0x13000000u || ActionFam8 == 0x42000000u
			|| ActionFam8 == 0x43000000u;
		const bool bIsRestHold = ACEMotion::IsHeldRestCommand(ActionCommand);
		const bool bIsJump = ActionCommand == 0x1000004Bu || ActionCommand == 0x40000015u || ActionCommand == 0x4000001Du;
		// Soul-emote substates play their entry, then their authored cycle. Some
		// cycles hold at zero rate; others (dances, gestures) keep animating.
		// Spawn uses SetHeldActionMotion to begin directly in that cycle.
		const bool bPreferCycle = bHoldActionFinal || (!bActionUsesStateTransition && !bIsDead && !bIsPickup && !bIsMagicCast && !bIsMissileGesture && !bIsConsumable && !bIsChatPose && !bIsRestHold && !bIsJump
			&& (Family == 0x40000000u || Family == 0x41000000u));
		bool bOk = false;
		if (bPreferCycle)
		{
			bOk = Dat->EvaluateMotionCommand(
				static_cast<uint32>(MotionTableId), ActionCommand, AnimTime,
				PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
				PreviousTime, &Hooks, ActionStyle, bLoopHeldSubstate, &bFinished);
		}
		if (!bOk)
		{
			if (bActionUsesStateTransition)
				bOk = Dat->EvaluateMotionTransition(
					static_cast<uint32>(MotionTableId), ActionFromCommand, ActionCommand, AnimTime,
					PartMeshes.Num(), Animated, WorldScale, AnimatedCount, bFinished,
					PreviousTime, &Hooks, ActionStyle);
			else
				bOk = Dat->EvaluateMotionLink(
					static_cast<uint32>(MotionTableId), ACEMotion::Ready, ActionCommand, AnimTime,
					PartMeshes.Num(), Animated, WorldScale, AnimatedCount, bFinished,
					PreviousTime, &Hooks, ActionStyle);
		}
		if (!bOk && bIsDead && ActionStyle != ACEMotion::StanceNonCombat)
		{
			bOk = Dat->EvaluateMotionLink(
				static_cast<uint32>(MotionTableId), ACEMotion::Ready, ActionCommand, AnimTime,
				PartMeshes.Num(), Animated, WorldScale, AnimatedCount, bFinished,
				PreviousTime, &Hooks, ACEMotion::StanceNonCombat);
		}
		// A table without an exit link may resume locomotion immediately. Do not
		// strand it in Ready's zero-rate cycle while movement input is active.
		if (!bOk && !bPreferCycle && (ActionCommand & 0xFF000000u) != 0x80000000u
			&& !(bActionUsesStateTransition && ActionCommand == ACEMotion::Ready))
		{
			bOk = Dat->EvaluateMotionCommand(
				static_cast<uint32>(MotionTableId), ActionCommand, AnimTime,
				PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
				PreviousTime, &Hooks, ActionStyle, /*bLoop*/ false, &bFinished);
			// With no entry link the substate has reached its cyclic endpoint.
			// A zero-rate pose must not strand a queued Ready.
			if (bOk && (bIsMissileGesture || (bIsMagicCast && UsesClassicCasting()))) bFinished = true;
		}
		if (!bOk && bIsDead && ActionStyle != ACEMotion::StanceNonCombat)
		{
			bOk = Dat->EvaluateMotionCommand(
				static_cast<uint32>(MotionTableId), ActionCommand, AnimTime,
				PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
				PreviousTime, &Hooks, ACEMotion::StanceNonCombat, /*bLoop*/ false, &bFinished);
		}
		// A one-shot gesture can end at Ready before a queued Ready request. Its
		// idle cycle is not another non-cyclic action that should block locomotion.
		if (ActionCommand == ACEMotion::Ready && !bActionUsesStateTransition) bFinished = true;
		if (bOk)
		{
			bActionEverEvaluated = true;
			CommitHookTime(TrackKey, AnimTime);
			bSuppressNextHookDispatch = false;
			ApplyAnimatedPartsWithBlend(Animated, AnimatedCount, DeltaTime);
			DispatchCrossedHooks(Hooks);
		}
		// Never abandon a magic windup/cast on a single failed eval (MotionTable swap mid-tick
		// used to skip PowerUp instantly → every spell looked like a bare L1 cast gesture).
		const bool bMagicAction = ACEIsMagicPowerUp(ActionCommand) || bIsMagicCast;
		const uint32 ActionFam = ActionCommand & 0xFF000000u;
		const bool bChatPoseAction = ActionFam == 0x13000000u || ActionFam == 0x42000000u
			|| ActionFam == 0x43000000u;
		// Chat poses / magic: give Link time to resolve before giving up.
		const bool bGiveUpFailed = !bOk
			&& !(bMagicAction && AnimTime < 2.f)
			&& !(bChatPoseAction && AnimTime < 3.f);
		if ((bFinished || bGiveUpFailed) && !bHoldActionFinal)
		{
			// Retail CSequence executes the authored particle hooks. Completing an
			// animation does not discard unrelated emitters or their remaining tails.
			if (bIsDead && bOk && bFinished)
			{
				// Keep the final death frame until ObjectDelete / corpse replace.
				bHoldActionFinal = true;
			}
			else if ((bIsMissileGesture || (bIsMagicCast && UsesClassicCasting())) && bOk && PendingActionCommands.IsEmpty())
			{
				// These substates rest at their endpoint until the next forward
				// command. Their exit to Ready has its own animation.
				bHoldActionFinal = true;
			}
			else if (bHoldActionFinalAfterFinish && (bFinished || bActionEverEvaluated) && bOk)
			{
				bHoldActionFinal = true;
				bHoldActionFinalAfterFinish = false;
				if (ActionCommand != ACEMotion::Dead && ACEMotion::IsHeldRestCommand(ActionCommand))
				{
					// The link and cyclic tail have separate clocks/hook traversal.
					// Keeping the link's end time froze every persistent dance.
					AnimTime = 0.f;
					ResetHookTracking();
				}
			}
			else if (bHoldActionFinalAfterFinish && !bOk && bActionEverEvaluated)
			{
				// Evaluation dropped out past the last frame — freeze the pose we already
				// applied rather than snapping back to locomotion Ready (chat-pose hold).
				bHoldActionFinal = true;
				bHoldActionFinalAfterFinish = false;
			}
			else if (PendingActionCommands.Num() > 0)
			{
				const uint32 Next = PendingActionCommands[0];
				const uint32 NextStyle = PendingActionStyles.IsValidIndex(0) ? PendingActionStyles[0] : 0u;
				const float NextRate = PendingActionPlayRates.IsValidIndex(0) ? PendingActionPlayRates[0] : 1.f;
				const bool bNextHold = PendingActionHolds.IsValidIndex(0) && PendingActionHolds[0];
				PendingActionCommands.RemoveAt(0);
				if (PendingActionStyles.Num() > 0) { PendingActionStyles.RemoveAt(0); }
				if (PendingActionPlayRates.Num() > 0) { PendingActionPlayRates.RemoveAt(0); }
				if (PendingActionHolds.Num() > 0) { PendingActionHolds.RemoveAt(0); }
				// Start next without wiping the rest of the scarab/cast queue.
				const bool bCastExit = ACEIsMagicCastCommand(ActionCommand) && UsesClassicCasting()
					&& (Next == ACEMotion::Ready || ACEIsMagicCastCommand(Next) || ACEIsMagicPowerUp(Next));
				ActionFromCommand = ACEIsEmoteSubstate(ActionCommand) || ACEIsMissileSubstate(ActionCommand) || bCastExit ? ActionCommand : ACEMotion::Ready;
				bActionUsesStateTransition = ActionFromCommand != ACEMotion::Ready || (Next & 0x80000000u);
				BeginPoseBlendFromCurrent(ACEAttackMotion::IsAttack(Next) ? 0.f : bActionUsesStateTransition ? .06f : StanceBlendDuration);
				AnimMode = EACEAnimMode::ActionOneShot;
				ActionCommand = Next;
				bHoldActionFinal = false;
				bHoldActionFinalAfterFinish = bNextHold && PendingActionCommands.IsEmpty();
				ActionStyle = NextStyle;
				if (ActionStyle != 0 && ActionStyle <= 0xFFFFu)
				{
					ActionStyle = ACEMotion::ExpandPackedCommand(static_cast<uint16>(ActionStyle));
				}
				if ((ActionStyle & 0xFF000000u) != 0x80000000u)
				{
					ActionStyle = PreferredStyle != 0 ? PreferredStyle : ACEMotion::StanceNonCombat;
				}
				if ((ACEIsMagicPowerUp(Next) || ACEIsMagicCastCommand(Next))
					&& (ActionStyle == 0 || ActionStyle == ACEMotion::StanceNonCombat))
				{
					ActionStyle = ACEMotion::StanceMagic;
				}
				ActionPlayRate = FMath::Max(0.f, NextRate);
				bActionEverEvaluated = false;
				AnimTime = 0.f;
				DeferredPoseDeltaTime = 0.f;
				bSuppressNextHookDispatch = false;
				ResetHookTracking();
			}
			else if (QueuedHoldAction != 0)
			{
				const uint32 Next = QueuedHoldAction;
				const uint32 NextStyle = QueuedHoldStyle;
				QueuedHoldAction = 0;
				QueuedHoldStyle = 0;
				SetHeldActionMotion(static_cast<int32>(Next), static_cast<int32>(NextStyle));
			}
			else
			{
				// Falling->Run/Walk already enters the requested cyclic motion.
				// Keep that direction when handing back to locomotion; treating it
				// as Ready queues another entry link, briefly braking held movement.
				if (bActionUsesStateTransition && ActionFromCommand == 0x40000015u)
				{
					WalkingMotion.Reset();
					WalkingMotion.Direction = ActionCommand == 0x44000007u ? 2
						: ActionCommand == 0x45000005u ? 1 : ActionCommand == 0x45000006u ? -1 : 0;
				}
				BeginPoseBlendFromCurrent(ACEAttackMotion::IsAttack(ActionCommand) ? 0.f : bActionUsesStateTransition ? .06f : StanceBlendDuration);
				ActionCommand = 0;
				ActionFromCommand = ACEMotion::Ready;
				bActionUsesStateTransition = false;
				AnimTime = 0.f;
				DeferredPoseDeltaTime = 0.f;
				ResetHookTracking();
				if (bResumeDefaultAnimAfterAction && DefaultAnimationId != 0)
				{
					bResumeDefaultAnimAfterAction = false;
					AnimMode = EACEAnimMode::DefaultAnimLoop;
					ObjectAnimFrame = FTransform::Identity;
				}
				else
				{
					AnimMode = EACEAnimMode::Locomotion;
				}
			}
		}
		return;
	}

	if (AnimMode == EACEAnimMode::DoorTransition)
	{
		TArray<FACEDatAnimationHook> Hooks;
		int32 AnimatedCount = 0;
		bool bFinished = false;
		const uint64 LinkTrackKey = (2ull << 60)
			| static_cast<uint64>(HashCombine(GetTypeHash(DoorFromCommand), GetTypeHash(DoorToCommand)));
		uint64 ActiveTrackKey = LinkTrackKey;
		float ActiveEvalTime = AnimTime;
		float SuppressedPreviousTime = AnimTime;
		const float* PreviousTime = bSuppressNextHookDispatch
			? &SuppressedPreviousTime
			: GetPreviousHookTime(LinkTrackKey, AnimTime);
		const bool bOk = Dat->EvaluateStateMotion(
			static_cast<uint32>(MotionTableId), DoorFromCommand, DoorToCommand, AnimTime, bDoorStartsInCycle,
			PartMeshes.Num(), Animated, WorldScale, AnimatedCount, bFinished, PreviousTime, &Hooks);
		if (bOk)
		{
			CommitHookTime(ActiveTrackKey, ActiveEvalTime);
			bSuppressNextHookDispatch = false;
			const bool bJustFinished = bFinished && !bDoorHoldFinal;
			if (bFinished)
			{
				bDoorHoldFinal = true;
			}
			for (int32 i = 0; i < PartMeshes.Num(); ++i)
			{
				if (i < AnimatedCount && Animated.IsValidIndex(i))
				{
					FTransform PartXform = Animated[i];
					if (BindTransforms.IsValidIndex(i))
					{
						PartXform.SetScale3D(BindTransforms[i].GetScale3D());
					}
					ApplyPartTransform(i, PartXform);
				}
				else if (BindTransforms.IsValidIndex(i))
				{
					ApplyPartTransform(i, BindTransforms[i]);
				}
			}
			if (bJustFinished || (bDoorHoldFinal && AnimTime < 0.05f))
			{
				RecookPartPhysics();
			}
			// The evaluator crosses link hooks only once, then dispatches cycle hooks.
			DispatchCrossedHooks(Hooks);
		}
		else
		{
			// A live unsupported On/Off must not trap the object in a failed
			// door evaluator forever. Resume its authored idle on the next tick.
			AnimMode = EACEAnimMode::Locomotion;
			AnimTime = 0.f;
			ResetHookTracking();
		}
		return;
	}

	float WalkCycleSeconds = DeltaTime;
	if (!bWalkingExternallyAdvanced && !RemoteWalkingFrame) WalkingMotion.Advance(DeltaTime, WalkCycleSeconds);
	if (!WalkingMotion.Display.Clips.IsEmpty())
	{
		int32 Count = 0;
		TArray<FACEDatAnimationHook> Hooks;
		if (Dat->EvaluateWalkingLink(WalkingMotion.Display, PartMeshes.Num(), Animated, WorldScale, Count, Hooks))
		{
			ApplyAnimatedPartsWithBlend(Animated, Count, DeltaTime);
			DispatchCrossedHooks(Hooks);
			// The cyclic tail starts at frame zero after the last linkage.
			AnimTime = 0.f;
			bWasLocomotionMoving = !FMath::IsNearlyZero(LocomotionForward) || !FMath::IsNearlyZero(LocomotionStrafe);
			return;
		}
	}
	const bool bMoving = !FMath::IsNearlyZero(LocomotionForward) || !FMath::IsNearlyZero(LocomotionStrafe);
	{
		const int8 FwdSign = (LocomotionForward > 0.15f) ? 1
			: (LocomotionForward < -0.15f) ? -1 : 0;
		if (FwdSign != 0 && LastLocomotionFwdSign != 0 && FwdSign != LastLocomotionFwdSign)
		{
			AnimTime = 0.f;
			DeferredPoseDeltaTime = 0.f;
			BeginPoseBlendFromCurrent();
		}
		if (FwdSign != 0)
		{
			LastLocomotionFwdSign = FwdSign;
		}
		else if (!bMoving)
		{
			LastLocomotionFwdSign = 0;
		}
	}
	if (!bSuppressLocoIdleBlend && bMoving != bWasLocomotionMoving)
	{
		BeginPoseBlendFromCurrent();
		bWasLocomotionMoving = bMoving;
	}
	else if (bMoving)
	{
		bWasLocomotionMoving = true;
	}
	// A received forward substate owns the cyclic animation. Retail's sidestep
	// modifier adds velocity; its magnitude does not replace that forward pose.
	const bool bForwardDominant = bLocomotionInterpretedRate
		? !FMath::IsNearlyZero(LocomotionForward)
		: FMath::Abs(LocomotionForward) + KINDA_SMALL_NUMBER >= FMath::Abs(LocomotionStrafe);
	const float TargetRunBlend = bMoving && bForwardDominant && LocomotionForward > 0.f && bLocomotionRunning ? 1.f : 0.f;
	RunBlend = FMath::FInterpTo(RunBlend, TargetRunBlend, DeltaTime, 5.f);

	constexpr uint32 WalkForward = 0x45000005u;
	constexpr uint32 WalkBackwards = 0x45000006u;
	constexpr uint32 RunForward = 0x44000007u;
	constexpr uint32 SideStepRight = 0x6500000fu;
	constexpr uint32 SideStepLeft = 0x65000010u;
	constexpr uint32 Ready = 0x41000003u;

	TArray<FACEDatAnimationHook> Hooks;
	int32 AnimatedCount = 0;
	bool bOk = false;
	auto MakeTrackKey = [](uint32 Command, bool bReverse)
	{
		return (4ull << 60) | static_cast<uint64>(Command) | (bReverse ? (1ull << 40) : 0ull);
	};
	auto Evaluate = [&](uint32 Command, float Time, TArray<FTransform>& Out, int32& OutCount,
		bool bCollectHooks, TArray<FACEDatAnimationHook>* CollectedHooks = nullptr)
	{
		const uint64 TrackKey = MakeTrackKey(Command, Time < 0.f);
		const float* PreviousTime = bCollectHooks ? GetPreviousHookTime(TrackKey, Time) : nullptr;
		const bool bEvaluated = Dat->EvaluateMotionCommand(
			static_cast<uint32>(MotionTableId), Command, Time,
			PartMeshes.Num(), Out, WorldScale, OutCount,
			PreviousTime, bCollectHooks ? CollectedHooks : nullptr, PreferredStyle);
		if (bEvaluated && bCollectHooks)
		{
			CommitHookTime(TrackKey, Time);
		}
		return bEvaluated;
	};

	if (bMoving && !bForwardDominant)
	{
		// Retail represents left strafe as SideStepRight with negative speed. Reverse the
		// authored right-strafe cycle so MTs without a separate left cycle still animate.
		const bool bLeft = LocomotionStrafe < 0.f;
		bOk = Evaluate(SideStepRight, bLeft ? -AnimTime : AnimTime, Animated, AnimatedCount, true, &Hooks);
		if (!bOk && bLeft)
		{
			bOk = Evaluate(SideStepLeft, AnimTime, Animated, AnimatedCount, true, &Hooks);
		}
	}
	else if (bMoving && LocomotionForward < -KINDA_SMALL_NUMBER)
	{
		// ACE: WalkBackwards canonicalizes to WalkForward at -0.65 playback rate.
		const float BackwardsFactor = bLocomotionInterpretedRate ? 1.f : 0.65f;
		bOk = Evaluate(WalkForward, -AnimTime * BackwardsFactor, Animated, AnimatedCount, true, &Hooks);
		if (!bOk)
		{
			bOk = Evaluate(WalkBackwards, AnimTime * BackwardsFactor, Animated, AnimatedCount, true, &Hooks);
		}
	}
	else if (bMoving)
	{
		// Never blend walk↔run: mismatched cycle phases mirrored limbs ("flip on repeat").
		// Gait and playback rate are independent: a fast walk remains a walk.
		const bool bWantRun = LocomotionForward > 0.f
			&& bLocomotionRunning;
		if (bWantRun)
		{
			bOk = Evaluate(RunForward, AnimTime, Animated, AnimatedCount, true, &Hooks);
			if (!bOk)
			{
				bOk = Evaluate(WalkForward, AnimTime, Animated, AnimatedCount, true, &Hooks);
			}
		}
		else
		{
			bOk = Evaluate(WalkForward, AnimTime, Animated, AnimatedCount, true, &Hooks);
		}
	}
	if (!bMoving && !FMath::IsNearlyZero(LocomotionTurn))
		bOk = Evaluate(ACEMotion::TurnRight, LocomotionTurnTime, Animated, AnimatedCount, true, &Hooks);
	if (!bOk)
	{
		const uint64 IdleTrackKey = (5ull << 60) | static_cast<uint32>(MotionTableId)
			| (static_cast<uint64>(PreferredStyle) << 20);
		const float* PreviousTime = GetPreviousHookTime(IdleTrackKey, AnimTime);
		// Prefer stance Ready cycle (HandCombat / Magic / …) so combat idle persists.
		if (PreferredStyle != 0)
		{
			bOk = Dat->EvaluateMotionCommand(
				static_cast<uint32>(MotionTableId), Ready, AnimTime,
				PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
				PreviousTime, &Hooks, PreferredStyle);
		}
		if (!bOk)
		{
			bOk = Dat->EvaluateIdleMotion(static_cast<uint32>(MotionTableId), AnimTime, PartMeshes.Num(), Animated, WorldScale, AnimatedCount,
				PreviousTime, &Hooks);
		}
		if (bOk)
		{
			CommitHookTime(IdleTrackKey, AnimTime);
		}
	}
	if (!bOk)
	{
		return;
	}

	// Stance / loco / action cross-fade toward the newly evaluated pose.
	ApplyAnimatedPartsWithBlend(Animated, AnimatedCount, DeltaTime);
	DispatchCrossedHooks(Hooks);
}

FTransform UACECharacterAppearanceComponent::UpdateVRUpperBody(const FTransform& BodyFrame, const FTransform& Head,
	const FTransform& LeftGrip, const FTransform& RightGrip, bool bLeftTracked, bool bRightTracked, float Dt)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_TrackedBodyPose);
	if (!MeshRoot) return FTransform::Identity;
	FTransform Frame = BodyFrame;
	if (!bVRPoseControlled || !BindTransforms.IsValidIndex(16) || Dt <= 0.f)
	{
		MeshRoot->SetWorldTransform(Frame);
		// The former caller restored untracked upper parts even on a paused
		// frame. Keep that fallback when no torso bend can be evaluated.
		for (int32 I=9; I<PartMeshes.Num(); ++I)
		{
			if (!PartMeshes[I] || !BindTransforms.IsValidIndex(I) || I==16 || I==21 || I==22) continue;
			const bool TrackedArm = (bLeftTracked && ((I>=10 && I<=12) || I==27))
				|| (bRightTracked && ((I>=13 && I<=15) || I==28));
			if (!TrackedArm) PartMeshes[I]->SetRelativeTransform(BindTransforms[I]);
		}
		return Frame;
	}
	const FRotator HeadAngles = Head.Rotator();
	const FQuat Yaw = FRotator(0, HeadAngles.Yaw, 0).Quaternion();
	const float Units = FMath::Max(1.f, WorldScale * float(Frame.GetScale3D().Z));
	// Infer only a small amount of chest motion. Untracked hands contribute
	// nothing, and arm reach is measured relative to the head, not world motion.
	const FVector Left = bLeftTracked ? Yaw.UnrotateVector(LeftGrip.GetLocation()-Head.GetLocation()) / Units : FVector::ZeroVector;
	const FVector Right = bRightTracked ? Yaw.UnrotateVector(RightGrip.GetLocation()-Head.GetLocation()) / Units : FVector::ZeroVector;
	const float Hands = float(bLeftTracked) + float(bRightTracked);
	const FVector Average = Hands > 0 ? (Left+Right)/Hands : FVector::ZeroVector;
	const FVector Target(
		FMath::Clamp(float(HeadAngles.Pitch)*.16f - FMath::Max(0.f,float(Average.X)-.25f)*4.f,-8.f,6.f),
		bLeftTracked && bRightTracked ? FMath::Clamp(float(Right.X-Left.X)*5.f,-4.f,4.f) : 0.f,
		FMath::Clamp(float(HeadAngles.Roll)*.12f + float(Average.Y)*4.f,-4.f,4.f));
	VRTorsoAngles = FMath::Lerp(VRTorsoAngles, Target, 1.f-FMath::Exp(-10.f*FMath::Min(Dt,.25f)));
	const FQuat Axes = Frame.GetRotation().Inverse()*Yaw;
	const FQuat Bend = Axes * FRotator(VRTorsoAngles.X,VRTorsoAngles.Y,VRTorsoAngles.Z).Quaternion() * Axes.Inverse();
	// Human chest and girth overlap around the midpoint of their authored
	// origins. Pivot there so bending cannot pull the waist seam apart.
	const FVector Waist = (BindTransforms[0].GetLocation()+BindTransforms[9].GetLocation())*.5f;
	const FTransform Delta(Bend, Waist-Bend.RotateVector(Waist));
	// Keep the neck attached to the tracked head. Shift the visual pelvis/legs
	// a few centimetres, never the pawn capsule or camera.
	const FVector Neck = BindTransforms[16].GetLocation();
	Frame.AddToTranslation(Frame.TransformVector(Neck-Delta.TransformPosition(Neck)));
	MeshRoot->SetWorldTransform(Frame);
	for (int32 I=9; I<PartMeshes.Num(); ++I)
	{
		if (!PartMeshes[I] || !BindTransforms.IsValidIndex(I) || I==16 || I==21 || I==22) continue;
		const bool TrackedArm = (bLeftTracked && ((I>=10 && I<=12) || I==27))
			|| (bRightTracked && ((I>=13 && I<=15) || I==28));
		if (!TrackedArm)
			PartMeshes[I]->SetRelativeTransform(BindTransforms[I].GetLocation().Z >= Waist.Z
				? BindTransforms[I]*Delta : BindTransforms[I]);
	}
	return Delta*Frame;
}

void UACECharacterAppearanceComponent::UpdateVRLowerBody(float Dt)
{
	if (!bVRPoseControlled || !MeshRoot || BindTransforms.Num() < 9 || Dt <= 0.f) return;
	const FVector Position = GetOwner()->GetActorLocation();
	const FVector Delta = bVRLowerBodyReady ? Position - VRPreviousBodyLocation : FVector::ZeroVector;
	VRPreviousBodyLocation = Position; bVRLowerBodyReady = true;
	const float Distance = Delta.Size2D();
	const bool Continuous = Dt < .25f && Delta.SizeSquared() < FMath::Square(200.f);
	const float Speed = Continuous ? Distance / Dt : 0.f;
	if (!Continuous) VRGaitTime = VRGaitBlend = 0.f;
	const bool Moving = Speed > 10.f;
	VRGaitBlend = FMath::FInterpConstantTo(VRGaitBlend, Moving ? 1.f : 0.f, Dt, 7.f);
	const FVector Local = MeshRoot->GetComponentQuat().UnrotateVector(Delta);
	const bool Sideways = FMath::Abs(Local.X) > FMath::Abs(Local.Y);
	const bool Run = Speed > WorldScale * 3.5f && !Sideways;
	// Human Setup faces +Y. Distances, not elapsed wall time, advance the gait,
	// so collisions and server corrections cannot leave feet running in place.
	const bool Reverse = Sideways ? Local.X > 0.f : Local.Y < 0.f;
	if (Moving) VRGaitTime += (Reverse ? -1.f : 1.f) * Distance / (WorldScale * (Run ? 4.f : 3.12f));
	TArray<FTransform> Animated; int32 Count = 0;
	const bool bReusePose = CVarACEReusePoseBuffers.GetValueOnGameThread() != 0;
	if (bReusePose) Swap(Animated, VRGaitPoseScratch);
	ON_SCOPE_EXIT { if (bReusePose) Swap(Animated, VRGaitPoseScratch); };
	auto* Dat = GetWorld()->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
	const uint32 Command = Sideways ? 0x6500000fu : Run ? 0x44000007u : 0x45000005u;
	const bool Evaluated = VRGaitBlend > 0.f && Dat && Dat->EvaluateMotionCommand(
		MotionTableId, Command, VRGaitTime, PartMeshes.Num(), Animated, WorldScale, Count,
		nullptr, nullptr, ACEMotion::StanceNonCombat) && Count >= 9 && Animated.Num() >= 9;
	for (int32 I = 0; I < 9; ++I)
	{
		if (!PartMeshes.IsValidIndex(I) || !PartMeshes[I]) continue;
		FTransform Pose = BindTransforms[I];
		if (I > 0 && Evaluated)
		{
			FTransform Walking = Animated[I].GetRelativeTransform(Animated[0]) * BindTransforms[0];
			Walking.SetScale3D(BindTransforms[I].GetScale3D());
			Pose.Blend(BindTransforms[I], Walking, VRGaitBlend);
		}
		PartMeshes[I]->SetRelativeTransform(Pose);
	}
}
