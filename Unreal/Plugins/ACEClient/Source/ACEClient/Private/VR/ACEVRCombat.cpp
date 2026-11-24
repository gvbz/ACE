#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "VR/ACEVRPointerVisuals.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEWorldEntityActor.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACECombatStance.h"
#include "ACESession.h"
#include "UI/ACEUIGameplayBinder.h"
#include "ACEDatSubsystem.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "ProceduralMeshComponent.h"
#include "ACEVisibleObjectPick.h"
#include "Misc/ScopeExit.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

static TAutoConsoleVariable<int32> CVarProjectileBodyBounds(TEXT("ace.VR.ProjectileBodyBounds"),1,
	TEXT("Prepare creature contact bounds once per trajectory before detailed part checks."));

float UACEVRComponent::GetRequestedCombatPower() const
{
	return PC && PC->DatGameplayBinder ? FMath::Clamp(PC->DatGameplayBinder->RequestedAttackPower,0.f,1.f) : .5f;
}

bool UACEVRComponent::TryDropInventoryItem(int32 Item, int32 SplitAmount)
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	if (!bActive || !PC || !Session || !Session->SupportsVRDrops()) return false;
	FACEWorldObject Object;
	if (!Client->GetWorldObject(Item,Object) || !Client->IsOwnedInventoryItem(Object) || !Object.CanDropToWorld()) return false;
	// VRDrop uses zero for a whole object. A positive quantity is explicitly a
	// split and the server rejects splitting a stack into its entire count.
	if (SplitAmount >= FMath::Max(1,Object.StackSize)) SplitAmount=0;
	// The controller releasing the inventory drag is the release point, including
	// offhand drags. Contact-constrained grips keep it on our side of a wall.
	FVector Origin = ToAceOffset(GetAvatarGrip(FeedbackHand == 0).GetLocation());
	if (Origin.ContainsNaN()) return true;
	const FVector Horizontal = FVector(Origin.X, Origin.Y, 0).GetClampedToMaxSize(1.7f);
	Origin = FVector(Horizontal.X, Horizontal.Y, FMath::Clamp(Origin.Z, .1, 2.5));
	Session->SendVRDrop(PC->GetEffectiveCellId(), Item, SplitAmount, Origin);
	return true; // Never fall through and send a second, desktop drop.
}

namespace
{
	void IgnoreHeld(AActor* Owner, FCollisionQueryParams& Q)
	{
		Q.AddIgnoredActor(Owner); TArray<AActor*> Children; Owner->GetAttachedActors(Children, true, true);
		for (auto* Child : Children) Q.AddIgnoredActor(Child);
	}
	void Beam(UStaticMeshComponent* Mesh, FVector A, FVector B, float Width)
	{
		Mesh->SetWorldLocation((A + B) * .5f);
		Mesh->SetWorldRotation(FRotationMatrix::MakeFromZ(B - A).Rotator());
		Mesh->SetWorldScale3D(FVector(Width / 100.f, Width / 100.f, FVector::Distance(A, B) / 100.f));
	}
}

UMotionControllerComponent* UACEVRComponent::WeaponGrip() const { return Settings->bLeftHanded ? LeftGrip.Get() : RightGrip.Get(); }
UMotionControllerComponent* UACEVRComponent::WeaponAim() const { return Settings->bLeftHanded ? LeftAim.Get() : RightAim.Get(); }
UMotionControllerComponent* UACEVRComponent::BowGrip() const { return Settings->bLeftHanded ? RightGrip.Get() : LeftGrip.Get(); }
UMotionControllerComponent* UACEVRComponent::BowAim() const { return Settings->bLeftHanded ? RightAim.Get() : LeftAim.Get(); }
int32 UACEVRComponent::MissileStyle() const { return ACECombatStance::InferCombatStyle(EquippedMissileWeapon()); }
bool UACEVRComponent::IsAmmoLauncher() const { const int32 Style = MissileStyle(); return Style == 0x10 || Style == 0x20; }

FACEWorldObject UACEVRComponent::EquippedAmmo() const
{
	if (!Client) return {};
	const int32 Style = MissileStyle();
	const int32 Expected = Style == 0x10 ? (1|8|64) : Style == 0x20 ? (2|16|128) : Style == 0x400 ? (4|32|256) : 0;
	if (!Expected) return {}; // Thrown weapons are their own ammunition.
	if (const auto Session = Client->GetSession())
		if (const auto* Item = Session->FindEquippedItem(ACEEquipMask::MissileAmmo, 0, Expected)) return *Item;
	return {};
}

int32 UACEVRComponent::GetCombatMode() const { return Client ? Client->GetPlayerVitals().CombatMode : ACECombatMode::NonCombat; }

bool UACEVRComponent::HasEquippedCaster() const
{
	if (const auto Session = Client ? Client->GetSession() : nullptr)
		return Session->FindEquippedItem(ACEEquipMask::Held, ACEItemType::Caster) != nullptr;
	return false;
}

FACEWorldObject UACEVRComponent::EquippedWeapon() const
{
	if (!Client) return {};
	const int32 Mode = GetCombatMode();
	const int64 Mask = Mode == ACECombatMode::Magic ? ACEEquipMask::Held : Mode == ACECombatMode::Missile
		? ACEEquipMask::MissileWeapon : ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded;
	if (const auto Session = Client->GetSession())
		if (const auto* Item = Session->FindEquippedItem(Mask)) return *Item;
	return {};
}

FACEWorldObject UACEVRComponent::EquippedMissileWeapon() const
{
	if (const auto Session = Client ? Client->GetSession() : nullptr)
		if (const auto* Item = Session->FindEquippedItem(ACEEquipMask::MissileWeapon)) return *Item;
	return {};
}

FVector UACEVRComponent::ToAceOffset(const FVector& World) const
{
	const auto* Capsule = GetOwner()->FindComponentByClass<UCapsuleComponent>();
	const FVector Feet = GetOwner()->GetActorLocation() - FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight());
	return FACEPosition::AceVectorToUnreal(World - Feet, 1.f / PC->WorldScale);
}

AACEWorldEntityActor* UACEVRComponent::AimTarget(FVector& Impact)
{
	const auto* Aim = UIAimOverride.IsValid() ? UIAimOverride.Get() : WeaponAim();
	if (!Aim || !Aim->IsTracked()) return nullptr;
	FVector Start = Aim->GetComponentLocation(), Direction=Aim->GetForwardVector();
	if (Aim==WeaponAim() && (EquippedWeapon().ItemType & ACEItemType::Caster)) GetSpellAim(Start,Direction);
	const FVector End = Start + Direction * 10000.f;
	return Cast<AACEWorldEntityActor>(ACEVisibleObjectPick::Trace(*GetWorld(), Start, End,
		Cast<APawn>(GetOwner()), &Impact, false));
}

void UACEVRComponent::SetCombat(int32 Mode)
{
	if (!Client || !bTracking || PC->bEnterWorldLoading || PC->bWorldRevealActive) return;
	if (Mode != 1 && Mode != 2 && Mode != 4 && Mode != 8) return;
	CancelGestures(); PC->EndUseApproach(); if (auto Session = Client->GetSession()) Session->SendCancelAttack(); Client->SendChangeCombatMode(Mode); Pulse(false);
}

void UACEVRComponent::ToggleCombat()
{
	if (!bActive || !bTracking || !Client || Client->GetSessionState() != EACESessionState::InWorld
		|| PC->bEnterWorldLoading || PC->bWorldRevealActive) return;
	// Y changes stance without dismissing the interface (X owns that action).
	if (GetCombatMode() != ACECombatMode::NonCombat) { SetCombat(ACECombatMode::NonCombat); return; }
	SetCombat(ACECombatStance::ResolveEquippedMode(Client->GetEquippedItems()));
}

bool UACEVRComponent::IsPointedBuffRecipient(const AACEWorldEntityActor* Target) const
{
	if (!Target || !Target->bIsPlayer || Target->IsCorpse() || Target->bReceivedDeathMotion) return false;
	auto* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	auto* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	uint32 Flags = 0, TargetType = 0;
	// Use the retail spell metadata, not the localized "Other" name. Self,
	// fellowship, and projectile spells retain their existing targeting rules.
	return Dat && Dat->TryGetSpellTargeting(SelectedSpell, Flags, TargetType)
		&& (Flags & 0x4u) && !(Flags & (0x8u | 0x100u | 0x2000u))
		&& (TargetType & ACEItemType::Creature);
}

void UACEVRComponent::FireSpell()
{
	if (IsInputBlocked()) { SetCastFeedback(TEXT("Close the menu with X before casting.")); return; }
	if (!WeaponAim()->IsTracked() || !WeaponGrip()->IsTracked()) { SetCastFeedback(TEXT("Weapon controller tracking is unavailable.")); return; }
	if (GetWorld()->GetTimeSeconds() - LastCast < .25f) return;
	const auto Slots = SpellSlots();
	if (SelectedSpell == 0) { if (Slots.Num() == 0) { SetCastFeedback(TEXT("No spells available. Add a known spell or equip a casting item.")); return; } SelectedSpell = Slots[0]; }
	const auto Weapon = EquippedWeapon();
	const auto Session = Client->GetSession();
	if (!Session || Session->GetState() != EACESessionState::InWorld) return;
	const bool EnhancedCombat = Session->SupportsVRCombat();
	if (EnhancedCombat && Session->GetVRCastPhase()==1) return;
	if (Weapon.Guid == 0) { SetCastFeedback(TEXT("Equip a wand, staff, or orb before casting.")); return; }
	FVector Origin, Direction;
	GetSpellAim(Origin, Direction);
	int32 TargetGuid = 0;
	// ClientMagicSystem::CastSpell resolves SelfTargeted and untargeted spells
	// before consulting selection. Do the same before even tracing the pointer:
	// a self buff needs neither a visible body nor a selected world object.
	if (!Client->ResolveSpellCastTarget(SelectedSpell, 0, TargetGuid))
	{
		auto* Target = Cast<AACEWorldEntityActor>(ACEVisibleObjectPick::Trace(*GetWorld(), Origin,
			Origin + Direction * 10000.f, Cast<APawn>(GetOwner()), nullptr, false));
		// A deliberate trigger click on a visible player selects that recipient and
		// casts an Other buff in one action, even after another object was selected.
		const bool bPointedBuff = IsPointedBuffRecipient(Target);
		if (bPointedBuff && Client->GetSelectedObject().Guid != Target->GetACEGuid())
			Client->SelectObject(Target->GetACEGuid());
		const auto Selected = Client->GetSelectedObject();
		// Otherwise retain explicit selection: crossing a sign, corpse or held mesh
		// must not silently replace a targeted debuff or an inventory-item buff.
		// Free-aim projectiles still use Origin/Direction, independently of selection.
		const int32 RequestedTarget = bPointedBuff ? Target->GetACEGuid() : Selected.bValid ? Selected.Guid : Target ? Target->GetACEGuid() : 0;
		if (!Client->ResolveSpellCastTarget(SelectedSpell, RequestedTarget, TargetGuid, EnhancedCombat))
		{
			SetCastFeedback(TEXT("Select an appropriate target for this spell."));
			return;
		}
	}
	bool Sent = false;
	if (EnhancedCombat)
		Sent = Session->SendVRCombat(1, PC->GetEffectiveCellId(), Weapon.Guid, SelectedSpell, TargetGuid,
			ToAceOffset(Origin), FACEPosition::AceVectorToUnreal(Direction), 1.f, 0.f);
	else
	{
		// Standard ACE uses retail targeting and spell requests. Send directly:
		// Client->SendCastSpell deliberately only selects a spell while in VR.
		if (Weapon.SpellDID == SelectedSpell)
		{
			if (Client->IsUseBusy()) { SetCastFeedback(TEXT("You're too busy!")); return; }
			Session->SendUseWithTarget(Weapon.Guid, TargetGuid ? TargetGuid : Client->GetPlayerGuid());
		}
		else Session->SendCastSpell(SelectedSpell, TargetGuid);
		Sent = true;
	}
	if (Sent)
	{
		LastCast = GetWorld()->GetTimeSeconds(); Pulse(Settings->bLeftHanded);
		SetCastFeedback(TEXT("Casting ") + GetSelectionText());
		UE_LOG(LogTemp, Log, TEXT("ACE VR cast sent: protocol=%s spell=%d weapon=0x%08X target=0x%08X cell=0x%08X"), EnhancedCombat ? TEXT("VR") : TEXT("retail"), SelectedSpell, Weapon.Guid, TargetGuid, PC->GetEffectiveCellId());
	}
	else SetCastFeedback(TEXT("The cast could not be sent. Check the server connection."));
}

void UACEVRComponent::GetSpellAim(FVector& Origin, FVector& Direction)
{
	Origin = GetWeaponTip();
	Direction = GetPhysicalAim(Settings->bLeftHanded).GetUnitAxis(EAxis::X);
	// Traditional shafts retain their rendered tip/axis. An orb, phylactery,
	// or reverse-oriented ornament has no useful barrel: its controller ray
	// defines forward. Never infer a backward shot from an arbitrary mesh axis.
	if (const auto* Item = TrackedWeaponActor.Get(); Item && Item->Appearance)
	{
		const auto* Part = Cast<UPrimitiveComponent>(Item->Appearance->GetPartMesh(0));
		if (Part)
		{
			const FVector Extent = Part->CalcLocalBounds().BoxExtent;
			const FVector Axis = Item->GetActorTransform().TransformVectorNoScale(WeaponAxisLocal).GetSafeNormal();
			if (Extent.Z > FMath::Max(Extent.X, Extent.Y) * 2.f && FVector::DotProduct(Axis, Direction) > .75f)
				Direction = Axis;
			else Origin = Part->Bounds.Origin + Direction * FMath::Min(Part->Bounds.SphereRadius, 18.f);
		}
	}
}

FVector UACEVRComponent::GetCrossbowMuzzle() const
{
	if (const auto* Item = MissileVisualActor.Get(); Item && Item->Appearance)
		return Item->GetActorTransform().TransformPosition(CrossbowMuzzleLocal);
	return GetPhysicalGrip(!Settings->bLeftHanded).GetLocation() + GetPhysicalAim(!Settings->bLeftHanded).GetUnitAxis(EAxis::X) * 55.f
		+ GetPhysicalGrip(!Settings->bLeftHanded).GetUnitAxis(EAxis::Z) * 5.f;
}

void UACEVRComponent::FireCrossbow()
{
	if (IsInputBlocked() || MissileStyle() != 0x20 || GetCombatMode() != ACECombatMode::Missile) return;
	if (GetMeleeRecoveryRemaining() > 0) return;
	if (!BowGrip()->IsTracked() || !BowAim()->IsTracked()) { SetCastFeedback(TEXT("Crossbow controller tracking is unavailable.")); return; }
	const auto Weapon = EquippedMissileWeapon();
	const auto Ammo = EquippedAmmo();
	if (!Ammo.Guid) { SetCastFeedback(TEXT("Equip compatible bolts first.")); return; }
	if (auto* Item = MissileVisualActor.Get()) UpdateMissileAttachment(Item);
	if (auto Session = Client->GetSession(); Session && Session->SendVRCombat(3, PC->GetEffectiveCellId(), Weapon.Guid,
		0, 0, ToAceOffset(GetCrossbowMuzzle()), FACEPosition::AceVectorToUnreal(GetPhysicalAim(!Settings->bLeftHanded).GetUnitAxis(EAxis::X)), 1.f, 0.f, GetRequestedCombatPower()))
	{
		Pulse(Settings->bLeftHanded, .6f);
		UE_LOG(LogTemp, Log, TEXT("ACE VR crossbow sent: weapon=0x%08X ammo=0x%08X"), Weapon.Guid, Ammo.Guid);
	}
	else SetCastFeedback(Client->GetSession() && Client->GetSession()->IsMissileMovementBlocked()
		? TEXT("Stand still and land before shooting.") : TEXT("Missile could not be sent. Check the VR server connection."));
}

void UACEVRComponent::GetThrownAim(FVector& Origin, FVector& Direction) const
{
	const FTransform Grip = GetPhysicalGrip(Settings->bLeftHanded);
	Origin = Grip.GetLocation();
	// Atlatls and individually thrown items share the over-shoulder grip.
	Direction = -Grip.GetUnitAxis(EAxis::X);
	Origin += Direction * 8.f;
}

void UACEVRComponent::FireThrownMissile()
{
	if (IsInputBlocked() || IsAmmoLauncher() || GetCombatMode() != ACECombatMode::Missile
		|| !WeaponGrip()->IsTracked() || !WeaponAim()->IsTracked() || GetMeleeRecoveryRemaining() > 0) return;
	const auto Weapon = EquippedMissileWeapon();
	if (!Weapon.Guid) return;
	// Atlatls consume their equipped dart; ordinary thrown weapons are their own stack.
	if (MissileStyle() == 0x400 && EquippedAmmo().Guid == 0)
	{ SetCastFeedback(TEXT("Equip compatible darts first.")); return; }
	FVector Origin, Direction; GetThrownAim(Origin, Direction);
	if (auto Session = Client->GetSession(); Session && Session->SendVRCombat(3, PC->GetEffectiveCellId(), Weapon.Guid,
		0, 0, ToAceOffset(Origin), FACEPosition::AceVectorToUnreal(Direction), 1.f, 0.f, GetRequestedCombatPower()))
	{
		Pulse(Settings->bLeftHanded, .6f);
		UE_LOG(LogTemp, Log, TEXT("ACE VR missile sent: weapon=0x%08X ammo=0x%08X style=0x%X instant=1"),
			Weapon.Guid,EquippedAmmo().Guid,MissileStyle());
	}
	else SetCastFeedback(Client->GetSession() && Client->GetSession()->IsMissileMovementBlocked()
		? TEXT("Stand still and land before shooting.") : TEXT("Missile could not be sent. Check the VR server connection."));
}

FVector UACEVRComponent::BowDrawDirection() const
{
	const FTransform Aim=GetPhysicalAim(!Settings->bLeftHanded);
	const FVector Side=Aim.GetUnitAxis(EAxis::Y)*(Settings->bLeftHanded ? -1.f : 1.f);
	return (GetPhysicalGrip(!Settings->bLeftHanded).GetLocation()-GetPhysicalGrip(Settings->bLeftHanded).GetLocation()
		+ Side*Settings->BowAnchorOffset*BowFraction).GetSafeNormal(SMALL_NUMBER,Aim.GetUnitAxis(EAxis::X));
}

void UACEVRComponent::ReleaseArrow()
{
	const bool WasDrawn = bDrawing;
	if (MissileStyle()!=0x10) { bDrawing=false; return; }
	if (bDrawing)
	{
		float Fraction;
		BowFraction = ACEVRMath::BowDraw(GetPhysicalGrip(!Settings->bLeftHanded).GetLocation(), GetPhysicalGrip(Settings->bLeftHanded).GetLocation(),
			BowAim()->GetForwardVector(), Settings->BowFullDraw, Fraction) ? Fraction : 0.f;
	}
	bDrawing = false;
	if (GetCombatMode()!=ACECombatMode::Missile || GetMeleeRecoveryRemaining()>0) return;
	const float Fraction = BowFraction;
	if (!WasDrawn || IsInputBlocked() || !WeaponGrip()->IsTracked()
		|| !BowGrip()->IsTracked() || !BowAim()->IsTracked() || Fraction < .2f || BowHoldTime < .15f)
	{
		if (WasDrawn) SetCastFeedback(TEXT("Pull the arrow back before releasing."));
		return;
	}
	const auto Weapon = EquippedMissileWeapon();
	if (!Weapon.Guid) { SetCastFeedback(TEXT("No equipped missile weapon. Re-equip your bow.")); return; }
	if (EquippedAmmo().Guid == 0) { SetCastFeedback(TEXT("Equip compatible arrows first.")); return; }
	const FVector Origin = GetPhysicalGrip(!Settings->bLeftHanded).GetLocation();
	const FVector Direction = BowDrawDirection();
	if (auto Session = Client->GetSession(); Session && Session->SendVRCombat(3, PC->GetEffectiveCellId(), Weapon.Guid,
		0, 0, ToAceOffset(Origin), FACEPosition::AceVectorToUnreal(Direction), Fraction, FMath::Min(BowHoldTime, 2.f), GetRequestedCombatPower()))
	{
		Pulse(Settings->bLeftHanded, .6f); SetCastFeedback(TEXT("Missile released."));
		UE_LOG(LogTemp, Log, TEXT("ACE VR missile sent: weapon=0x%08X ammo=0x%08X draw=%.2f hold=%.2f"), Weapon.Guid, EquippedAmmo().Guid, Fraction, BowHoldTime);
	}
	else SetCastFeedback(Client->GetSession() && Client->GetSession()->IsMissileMovementBlocked()
		? TEXT("Stand still and land before shooting.") : TEXT("Missile could not be sent. Check the VR server connection."));
}

void UACEVRComponent::UpdateCombat(float Dt)
{
	if (MissileTrajectory) MissileTrajectory->SetVisibility(false);
	const auto Weapon = EquippedWeapon(); const int32 Mode = GetCombatMode();
	PredictedCombatTarget.Reset();
	// Apply once, after prediction. Clearing and reapplying the same tint every
	// frame creates needless material updates, and leaves two targets lit.
	ON_SCOPE_EXIT
	{
		if (Mode != ACECombatMode::NonCombat)
			UpdateWorldSelectionHighlights(Mode == ACECombatMode::Melee ? SelectedWorldTarget.Get() : PredictedCombatTarget.Get(), nullptr);
	};
	if (Weapon.Guid != LastWeapon || Mode != LastMode)
	{
		CancelGestures(); LastWeapon = Weapon.Guid; LastMode = Mode; TrackedWeaponActor.Reset(); WeaponMeshRevision = 0; bBladeCalibrated = false;
		if (Client && Mode == ACECombatMode::Missile) if (auto Session = Client->GetSession()) Session->RequestVRCapabilities();
	}
	Arrow->SetVisibility(false); for (UStaticMeshComponent* String : BowStrings) String->SetVisibility(false);
	if (AmmoActor) AmmoActor->SetActorHiddenInGame(true);
	if (IsInputBlocked() || (bSpellWheelOpen && Mode != ACECombatMode::Magic))
	{ Swing.Reset(); for (auto& Punch : Punches) Punch.Gesture.Reset(); bPreviousContactBlade = false; bDrawing = false; return; }
	if (Mode == ACECombatMode::Melee && Weapon.Guid == 0) { Swing.Reset(); UpdateUnarmed(Dt); return; }
	if (!WeaponGrip()->IsTracked() || !WeaponAim()->IsTracked()) { Swing.Reset(); bPreviousContactBlade = false; bDrawing = false; return; }
	if (Mode==ACECombatMode::Magic)
		if (auto Session=Client->GetSession())
		{
			FVector Origin,Direction; GetSpellAim(Origin,Direction);
			Session->SendVRAim(PC->GetEffectiveCellId(),Weapon.Guid,ToAceOffset(Origin),FACEPosition::AceVectorToUnreal(Direction));
		}
	// A wheel opened during an existing windup must not freeze the release aim.
	if (bSpellWheelOpen)
	{ Swing.Reset(); for (auto& Punch : Punches) Punch.Gesture.Reset(); bPreviousContactBlade=false; bDrawing=false; return; }
	if (Mode == ACECombatMode::Missile)
	{
		if (IsAmmoLauncher() && (!BowGrip()->IsTracked() || !BowAim()->IsTracked())) { CancelGestures(); return; }
		if (IsPointerNearPanel(IsAmmoLauncher() ? !Settings->bLeftHanded : Settings->bLeftHanded)) return;
		if (auto Session = Client->GetSession(); Session && Session->GetVRMissileSpeed(Weapon.Guid) <= 0
			&& GetWorld()->GetTimeSeconds() >= NextMissileProfileRequest)
		{ Session->RequestVRCapabilities(); NextMissileProfileRequest = GetWorld()->GetTimeSeconds() + 1.1; }
		if (bDrawing) BowHoldTime += Dt;
		const bool BowLike = IsAmmoLauncher();
		if (BowLike)
		{
			const auto Ammo = EquippedAmmo();
			if (!Ammo.Guid) { bDrawing = false; return; }
			if (MissileStyle() == 0x20)
			{
				if (auto* Item = MissileVisualActor.Get()) UpdateMissileAttachment(Item);
				const FVector Forward = GetPhysicalAim(!Settings->bLeftHanded).GetUnitAxis(EAxis::X);
				UpdateAmmoVisual(Ammo, GetCrossbowMuzzle(), Forward, true);
				UpdateMissileTrajectory(GetCrossbowMuzzle(), Forward, 1.f);
				return; // Crossbow has its own authored limbs/string; no vertical bow string.
			}
			const FVector Bow = GetPhysicalGrip(!Settings->bLeftHanded).GetLocation(), Hand = GetPhysicalGrip(Settings->bLeftHanded).GetLocation();
			float Fraction;
			const FVector Forward = GetPhysicalAim(!Settings->bLeftHanded).GetUnitAxis(EAxis::X);
			const bool Valid = ACEVRMath::BowDraw(Bow, Hand, Forward, Settings->BowFullDraw, Fraction);
			if (bDrawing) BowFraction = Valid ? Fraction : 0.f;
			if (bDrawing && FVector::Distance(Bow, Hand) > 180.f) { CancelGestures(); return; }
			const FVector Nock = bDrawing ? Hand : Bow - Forward * 12.f;
			const FVector Direction = bDrawing ? BowDrawDirection() : Forward;
			if (bDrawing) UpdateMissileTrajectory(Bow, Direction, BowFraction);
			UpdateAmmoVisual(Ammo, Nock, Direction);
			for (int32 I = 0; I < 2; ++I)
			{ Beam(BowStrings[I], Bow + GetPhysicalGrip(!Settings->bLeftHanded).GetUnitAxis(EAxis::Z) * (I ? 48.f : -48.f), Nock, .25f); BowStrings[I]->SetVisibility(true); }
		}
		else
		{
			FVector Origin, Direction; GetThrownAim(Origin, Direction);
			if (MissileStyle()==0x400 && EquippedAmmo().Guid)
				UpdateAmmoVisual(EquippedAmmo(),Origin,Direction,true);
			UpdateMissileTrajectory(Origin, Direction, 1.f);
		}
	}
	if (Mode == ACECombatMode::Magic && SelectedSpell && !IsInputBlocked() && !IsPointerNearPanel(Settings->bLeftHanded))
	{
		if (auto Session = Client->GetSession())
		{
			float Speed = 0; bool Gravity = false;
			if (Session->GetVRSpellProfile(SelectedSpell,Speed,Gravity))
			{
				FVector Origin, Direction; GetSpellAim(Origin,Direction);
				if (Speed > 0) UpdateMissileTrajectory(Origin,Direction,1.f,Speed,Gravity);
				else
				{
					// Preview the same recipient FireSpell will use, without changing
					// selection until the trigger is pressed.
					uint32 Flags=0, TargetType=0;
					auto* Dat=GetWorld()->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
					const bool Self=Dat && Dat->TryGetSpellTargeting(SelectedSpell,Flags,TargetType) && (Flags & 0x8u);
					if (!Self)
					{
						auto* Pointed=Cast<AACEWorldEntityActor>(ACEVisibleObjectPick::Trace(*GetWorld(),Origin,
							Origin+Direction*10000.f,Cast<APawn>(GetOwner()),nullptr,false));
						auto* Target=IsPointedBuffRecipient(Pointed) ? Pointed
							: Client->GetSelectedObject().bValid ? SelectedWorldTarget.Get() : Pointed;
						if (Target && Target->IsStandingCreatureOrPlayer() && !Target->bReceivedDeathMotion)
							PredictedCombatTarget=Target;
					}
				}
			}
			else if (GetWorld()->GetTimeSeconds() >= NextSpellProfileRequest)
			{ Session->RequestVRSpellProfile(SelectedSpell); NextSpellProfileRequest = GetWorld()->GetTimeSeconds()+.3; }
		}
	}
	if (Mode != ACECombatMode::Melee || Weapon.Guid == 0) { Swing.Reset(); return; }
	// Test several points along the blade, in the tracking frame. Locomotion cannot generate swing speed.
	const FVector Grip = WeaponGrip()->GetComponentLocation();
	if (!bBladeCalibrated)
	{
		BladeTipInGrip = GetPhysicalGrip(Settings->bLeftHanded).InverseTransformPosition(GetWeaponTip()).GetClampedToMaxSize(140.f);
		// Calibration is fixed for the gesture, so animation/IK can never add speed.
		bBladeCalibrated = true; Swing.Reset();
	}
	const FVector Tip = WeaponGrip()->GetComponentTransform().TransformPosition(BladeTipInGrip);
	const FTransform Tracking = TrackingOrigin->GetComponentTransform();
	const FTransform Physical = GetPhysicalGrip(Settings->bLeftHanded);
	const FVector ContactGrip = Tracking.InverseTransformPosition(Physical.GetLocation());
	const FVector ContactTip = Tracking.InverseTransformPosition(Physical.TransformPosition(BladeTipInGrip));
	const FVector LocalGrip = Tracking.InverseTransformPosition(Grip);
	const bool WasValid = Swing.bValid;
	float Speed;
	const bool Ready = Swing.Sample(Tracking.InverseTransformPosition(Tip), Dt, Settings->MeleeMinSpeed, true, Speed);
	if (Swing.Elapsed <= Dt + SMALL_NUMBER)
	{
		SwingStartGrip = WasValid ? PreviousSwingGrip : LocalGrip;
		SwingContactGrip = WasValid && bPreviousContactBlade ? PreviousContactGrip : ContactGrip;
		SwingContactTip = WasValid && bPreviousContactBlade ? PreviousContactTip : ContactTip;
	}
	PreviousSwingGrip = LocalGrip;
	PreviousContactGrip = ContactGrip; PreviousContactTip = ContactTip; bPreviousContactBlade = true;
	if (!Ready || !Swing.HasDeliberateHandMotion(SwingStartGrip, LocalGrip)) return;
	const FVector StartTip = Tracking.TransformPosition(SwingContactTip), StartGrip = Tracking.TransformPosition(SwingContactGrip);
	TryTrackedStrike(Weapon.Guid, Settings->bLeftHanded, Swing, StartGrip, StartTip,
		Physical.GetLocation(), Physical.TransformPosition(BladeTipInGrip));
}

void UACEVRComponent::UpdateUnarmed(float Dt)
{
	const auto Session = Client->GetSession();
	const auto Items = Client->GetEquippedItems();
	const FTransform Tracking = TrackingOrigin->GetComponentTransform();
	for (int32 Side=0; Side<2; ++Side)
	{
		const bool Left = Side == 0, Dominant = Left == Settings->bLeftHanded;
		const auto* Controller = Left ? LeftGrip.Get() : RightGrip.Get();
		auto& Punch = Punches[Side];
		bool Occupied = false;
		for (const auto& Item : Items)
			if (Item.CurrentWieldedLocation & (Dominant ? ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded
				| ACEEquipMask::Held | ACEEquipMask::MissileWeapon : ACEEquipMask::Shield)) Occupied = true;
		if (Occupied || !Controller->IsTracked() || !Session || !Session->SupportsVRUnarmed())
		{ Punch.Gesture.Reset(); continue; }
		// Raw grip displacement in tracking space measures the player's punch.
		// Walking, IK correction and contact constraints cannot create velocity.
		const FVector Raw = Tracking.InverseTransformPosition(Controller->GetComponentLocation());
		const FVector Contact = Tracking.InverseTransformPosition(GetPhysicalGrip(Left).GetLocation());
		const bool WasValid = Punch.Gesture.bValid;
		float Speed;
		const bool Ready = Punch.Gesture.Sample(Raw, Dt, Settings->MeleeMinSpeed, true, Speed);
		if (Punch.Gesture.Elapsed <= Dt + SMALL_NUMBER)
			Punch.StartContact = WasValid ? Punch.PreviousContact : Contact;
		Punch.PreviousContact = Contact;
		if (!Ready) continue;
		const FVector A = Tracking.TransformPosition(Punch.StartContact), B = Tracking.TransformPosition(Contact);
		if (FVector(ToAceOffset(A).X,ToAceOffset(A).Y,0).Size()>1.35f
			|| FVector(ToAceOffset(B).X,ToAceOffset(B).Y,0).Size()>1.35f) continue;
		TryTrackedStrike(0, Left, Punch.Gesture, A, A, B, B);
	}
}

bool UACEVRComponent::HasMeleeRecovery() const
{
	return (GetCombatMode() == ACECombatMode::Melee || GetCombatMode() == ACECombatMode::Missile)
		&& Client && Client->GetSession() && Client->GetSession()->SupportsVRRecovery();
}

float UACEVRComponent::GetMeleeRecoveryRemaining() const
{
	return HasMeleeRecovery() ? Client->GetSession()->GetVRRecoveryRemaining() : 0.f;
}

float UACEVRComponent::GetMeleeRecoveryProgress() const
{
	return HasMeleeRecovery() ? Client->GetSession()->GetVRRecoveryProgress() : 1.f;
}

bool UACEVRComponent::TryTrackedStrike(int32 Weapon, bool Left, ACEVRMath::FSwing& Gesture,
	const FVector& StartGrip, const FVector& StartTip, const FVector& Grip, const FVector& Tip)
{
	// Consume early strokes, rather than queueing damage when the timer expires.
	// A new deliberate swing is still required after recovery.
	if (GetMeleeRecoveryRemaining() > 0) { Gesture.Commit(); return false; }
	FCollisionQueryParams Q(SCENE_QUERY_STAT(VRBladeSweep), false); IgnoreHeld(GetOwner(), Q);
	AACEWorldEntityActor* Best = nullptr; FVector BestA, BestB; float BestDistance = MAX_flt;
	// Discover all nearby bodies independently of selection proxies. A corpse,
	// sign or another creature's generous pick sphere cannot steal the sweep.
	TArray<FOverlapResult> Overlaps;
	const FVector Center=(StartGrip+StartTip+Grip+Tip)*.25f;
	const float Reach=FMath::Max(FMath::Max(FVector::Distance(Center,StartGrip),FVector::Distance(Center,StartTip)),
		FMath::Max(FVector::Distance(Center,Grip),FVector::Distance(Center,Tip)))+150.f;
	GetWorld()->OverlapMultiByObjectType(Overlaps,Center,FQuat::Identity,FCollisionObjectQueryParams::AllObjects,
		FCollisionShape::MakeSphere(Reach),Q);
	TSet<AACEWorldEntityActor*> Bodies;
	for (const auto& Overlap:Overlaps)
		if (auto* Body=Cast<AACEWorldEntityActor>(Overlap.GetActor()); Body && (Body->ItemType & ACEItemType::Creature))
		{
			Q.AddIgnoredActor(Body);
			if (!Body->bReceivedDeathMotion && !Body->IsCorpse()) Bodies.Add(Body);
		}
	const int32 Samples=Weapon==0 ? 1 : FMath::Clamp(FMath::CeilToInt(FVector::Distance(Grip,Tip)/18.f),2,9);
	for (int32 Sample=0; Sample<Samples; ++Sample)
	{
		const float Part=Samples==1 ? 1.f : float(Sample)/(Samples-1);
		const FVector A=FMath::Lerp(StartGrip,StartTip,Part), B=FMath::Lerp(Grip,Tip,Part);
		const float Distance=FVector::Distance(A,B);
		if (Distance<12.f || Distance/Gesture.Elapsed<140.f) continue;
		for (auto* Target:Bodies)
		{
			float Along;
			if (!Target->FindMeleeContact(A,B,Along) || Along>=BestDistance) continue;
			FHitResult Obstacle;
			// Pawn blockers represent physical scenery; Visibility also contains
			// intentionally generous UI pick volumes. Do not use those as walls.
			if (GetWorld()->LineTraceSingleByChannel(Obstacle,A,FMath::Lerp(A,B,Along),ECC_Pawn,Q)) continue;
			Best=Target; BestA=A; BestB=B; BestDistance=Along;
		}
	}
	if (Best)
	{
		if (auto Session=Client->GetSession()) Session->SetVRMeleeBody(ToAceOffset(Best->GetActorLocation()));
		if (auto Session = Client->GetSession(); Session && Session->SendVRCombat(2, PC->GetEffectiveCellId(), Weapon, Weapon == 0 && Left != Settings->bLeftHanded ? 1 : 0,
			Best->GetACEGuid(), ToAceOffset(BestA), ToAceOffset(BestB), FMath::Clamp(float(FVector::Distance(BestA,BestB)) / Gesture.Elapsed / 600.f, 0.f, 1.f), Gesture.Elapsed, GetRequestedCombatPower()))
		{
			Gesture.Commit(); Client->SelectObject(Best->GetACEGuid()); Pulse(Left, .55f);
			UE_LOG(LogTemp, Log, TEXT("ACE VR melee sent: weapon=0x%08X target=0x%08X speed=%.1f duration=%.3f"), Weapon, Best->GetACEGuid(), FVector::Distance(BestA,BestB)/Gesture.Elapsed, Gesture.Elapsed);
			FACEWorldObject TargetObject, SelfObject;
			Client->GetWorldObject(Best->GetACEGuid(),TargetObject); Client->GetWorldObject(Client->GetPlayerGuid(),SelfObject);
			UE_LOG(LogTemp, Log, TEXT("ACE melee geometry: body=%s radius=%.3f height=%.3f received=%s playerPose=%s pawnOffset=%s selfSetup=%08X selfCell=%08X"),
				*ToAceOffset(Best->GetActorLocation()).ToString(), Best->GetMeleeBodyRadius()/PC->WorldScale, Best->GetMeleeBodyHeight()/PC->WorldScale,
				*ToAceOffset(TargetObject.Position.ToUnrealLocation(PC->WorldScale)).ToString(), *Client->GetPlayerPosition().Location.ToString(),
				*ToAceOffset(Client->GetPlayerPosition().ToUnrealLocation(PC->WorldScale)).ToString(),SelfObject.SetupId,SelfObject.Position.CellId);
			return true;
		}
	}
	return false;
}

bool UACEVRComponent::HasCombatTimer() const
{
	return HasMeleeRecovery() || (GetCombatMode()==ACECombatMode::Magic && Client && Client->GetSession() && Client->GetSession()->SupportsVRCasting());
}
FString UACEVRComponent::GetCombatTimerText() const
{
	if (GetCombatMode()==ACECombatMode::Magic && Client && Client->GetSession())
	{
		const auto S=Client->GetSession(); const uint32 Phase=S->GetVRCastPhase();
		return Phase==1 ? FString::Printf(TEXT("Casting %.1fs"), FMath::CeilToFloat(S->GetVRCastRemaining()*10.f)/10.f)
			: Phase==2 ? TEXT("Recovering") : TEXT("Cast ready");
	}
	const float Remaining=GetMeleeRecoveryRemaining();
	if (GetCombatMode() == ACECombatMode::Missile)
		return Remaining>0 ? FString::Printf(TEXT("Next shot %.1fs"),FMath::CeilToFloat(Remaining*10.f)/10.f) : TEXT("Shot ready");
	return Remaining>0 ? FString::Printf(TEXT("Next swing %.1fs"),FMath::CeilToFloat(Remaining*10.f)/10.f) : TEXT("Swing ready");
}
float UACEVRComponent::GetCombatTimerProgress() const
{
	return GetCombatMode()==ACECombatMode::Magic && Client && Client->GetSession() ? Client->GetSession()->GetVRCastProgress() : GetMeleeRecoveryProgress();
}

USceneComponent* UACEVRComponent::GetHeldAnchor(int32 ParentLocation, bool TwoHanded) const
{
	if (!bActive) return nullptr;
	bool Left;
	if (TwoHanded || ParentLocation == 1) Left = Settings->bLeftHanded;
	else if (ParentLocation == 2 || ParentLocation == 8 || ParentLocation == 9 || ParentLocation == 3) Left = !Settings->bLeftHanded;
	else return nullptr;
	if (auto* App = GetOwner()->FindComponentByClass<UACECharacterAppearanceComponent>())
		if (auto* Hand = App->GetPartMesh(Left ? 12 : 15)) return Hand;
	return Left ? LeftGrip.Get() : RightGrip.Get();
}

FVector UACEVRComponent::GetWeaponTip()
{
	const int32 Guid = EquippedWeapon().Guid;
	if (TrackedWeaponActor.IsValid() && TrackedWeaponActor->GetACEGuid() != Guid) { TrackedWeaponActor.Reset(); WeaponMeshRevision = 0; }
	if (!TrackedWeaponActor.IsValid())
	{
		TArray<AActor*> Attached; GetOwner()->GetAttachedActors(Attached, true, true);
		for (auto* A : Attached)
			if (auto* Entity = Cast<AACEWorldEntityActor>(A); Entity && Entity->GetACEGuid() == Guid) { TrackedWeaponActor = Entity; WeaponMeshRevision = 0; break; }
	}
	auto* Weapon = TrackedWeaponActor.Get();
	if (!Weapon || !Weapon->Appearance || !Weapon->Appearance->HasAppearance())
		return GetPhysicalGrip(Settings->bLeftHanded).GetLocation() + GetPhysicalAim(Settings->bLeftHanded).GetUnitAxis(EAxis::X) * 65.f;
	const uint64 Revision = Weapon->Appearance->GetAppearanceRevision();
	if (Revision != WeaponMeshRevision)
	{
		float Furthest = -FLT_MAX;
		UProceduralMeshComponent* TipMesh = nullptr;
		TArray<UProceduralMeshComponent*> Meshes;
		for (int32 I = 0; I < Weapon->Appearance->GetPartCount(); ++I)
			if (auto* Part = Cast<UProceduralMeshComponent>(Weapon->Appearance->GetPartMesh(I))) Meshes.Add(Part);
		for (auto* Mesh : Meshes)
			for (int32 S = 0; S < Mesh->GetNumSections(); ++S)
				if (const auto* Section = Mesh->GetProcMeshSection(S); Section && Section->bSectionVisible)
					for (const auto& Vertex : Section->ProcVertexBuffer)
					{
						const FVector World = Mesh->GetComponentTransform().TransformPosition(Vertex.Position);
						const float Distance = FVector::DotProduct(World - GetPhysicalGrip(Settings->bLeftHanded).GetLocation(), GetPhysicalGrip(Settings->bLeftHanded).GetUnitAxis(EAxis::X));
						if (Distance > Furthest) { Furthest = Distance; TipMesh = Mesh; WeaponTipLocal = Weapon->GetActorTransform().InverseTransformPosition(World); }
					}
		if (TipMesh)
		{
			// Wands/staves have their shaft along authored +Z. The held placement
			// rotates that shaft, so use the rendered part's axis rather than the ray.
			FVector Axis = TipMesh->GetUpVector();
			if (FVector::DotProduct(Axis, GetPhysicalGrip(Settings->bLeftHanded).GetUnitAxis(EAxis::X)) < 0.f) Axis = -Axis;
			WeaponAxisLocal = Weapon->GetActorTransform().InverseTransformVectorNoScale(Axis);
			if (EquippedWeapon().ItemType & ACEItemType::Caster)
			{
				float Max = -FLT_MAX; FVector Center = FVector::ZeroVector; int32 Count = 0;
				for (int32 S = 0; S < TipMesh->GetNumSections(); ++S)
					if (const auto* Section = TipMesh->GetProcMeshSection(S); Section && Section->bSectionVisible)
						for (const auto& Vertex : Section->ProcVertexBuffer)
							Max = FMath::Max(Max, float(FVector::DotProduct(TipMesh->GetComponentTransform().TransformPosition(Vertex.Position), Axis)));
				for (int32 S = 0; S < TipMesh->GetNumSections(); ++S)
					if (const auto* Section = TipMesh->GetProcMeshSection(S); Section && Section->bSectionVisible)
						for (const auto& Vertex : Section->ProcVertexBuffer)
						{
							const FVector World = TipMesh->GetComponentTransform().TransformPosition(Vertex.Position);
							if (FVector::DotProduct(World, Axis) >= Max - .75f) { Center += World; ++Count; }
						}
				if (Count) WeaponTipLocal = Weapon->GetActorTransform().InverseTransformPosition(Center / Count);
			}
		}
		WeaponMeshRevision = Revision;
	}
	return Weapon->GetActorTransform().TransformPosition(WeaponTipLocal);
}

bool UACEVRComponent::UpdateMissileAttachment(AACEWorldEntityActor* Actor)
{
	// Attached entities tick after the rig. A late equip/reload must not reveal
	// a weapon or ammo after the rig has hidden equipment for portal space.
	if (HideEquipmentForPortal(Actor)) return true;
	FACEWorldObject Object;
	if (!Client || !Actor || !Client->GetWorldObject(Actor->GetACEGuid(), Object)) return false;
	const bool Missile = IsAmmoLauncher();
	if ((Object.CurrentWieldedLocation & ACEEquipMask::MissileAmmo) != 0)
	{
		// Only the currently compatible nocked copy is visible. A leftover
		// arrow slot must not produce an arrow beside a thrown weapon.
		Actor->SetActorHiddenInGame(true); return true;
	}
	if (!Missile || Actor->GetACEGuid() != EquippedMissileWeapon().Guid)
	{
		if (MissileVisualActor == Actor)
		{
			if (Actor->Appearance) Actor->Appearance->ApplyWorldObject(Object, PC->WorldScale, false);
			MissileVisualActor.Reset(); MissileVisualRevision = 0;
			Actor->SetActorEnableCollision(true);
		}
		return false;
	}
	if (Actor->Appearance && (MissileVisualActor != Actor || MissileVisualRevision != Actor->Appearance->GetAppearanceRevision()))
	{
		Object.ParentGuid = Object.ParentLocation = Object.PlacementId = 0;
		// Authored held placement normalizes crossbows whose raw meshes use
		// different axes. Setups without placement 3 fall back to placement 0.
		if (MissileStyle()==0x20) Object.PlacementId=3;
		Object.MotionTableId = Object.DefaultAnimationId = 0;
		Actor->Appearance->ApplyWorldObject(Object, PC->WorldScale, false);
		MissileVisualActor = Actor; MissileVisualRevision = Actor->Appearance->GetAppearanceRevision();
		if (MissileStyle()==0x20)
		{
			FBox Stock(ForceInit), Grip(ForceInit);
			for (int32 I=0;I<Actor->Appearance->GetPartCount();++I)
				if (auto* Part=Cast<UProceduralMeshComponent>(Actor->Appearance->GetPartMesh(I)))
				{
					FTransform Bind; if (!Actor->Appearance->GetPartBindTransform(I,Bind)) continue;
					for (int32 S=0;S<Part->GetNumSections();++S)
						if (const auto* Section=Part->GetProcMeshSection(S); Section && Section->bSectionVisible)
							for (const auto& V:Section->ProcVertexBuffer)
							{
								const FVector P=Bind.TransformPosition(V.Position);
								if (FMath::Abs(P.Z)<6.f) { Stock+=P; if (FMath::Abs(P.Y)<10.f) Grip+=P; }
							}
				}
			// Sparse low-poly stocks need not have vertices at the hand plane.
			// Fall back to the stock underside, never to an unrelated model origin.
			CrossbowGripLocal=Stock.IsValid ? FVector(FMath::Max((Grip.IsValid ? Grip.Max.X : Stock.Max.X)-2.f,Stock.Min.X+2.f),0,Stock.GetCenter().Z) : FVector::ZeroVector;
			CrossbowMuzzleLocal=Stock.IsValid ? FVector(Stock.Min.X,Stock.Min.Y,Stock.GetCenter().Z) : FVector(0,-55,0);
		}
	}
	// Script emitters use absolute transforms and cannot be descendants of a
	// motion controller's late-update hierarchy. Keep the owner attachment;
	// update after the rig and put the model at the offhand's tracked pose.
	if (Actor->GetAttachParentActor() != GetOwner())
		Actor->AttachToActor(GetOwner(), FAttachmentTransformRules::KeepWorldTransform);
	// The offhand owns the bow's orientation through nock, draw and release.
	// Switching to the hand-to-hand vector while drawing flipped the model
	// near the nock and snapped it back on every shot.
	const FTransform SupportGrip = GetPhysicalGrip(!Settings->bLeftHanded);
	const FTransform SupportAim = GetPhysicalAim(!Settings->bLeftHanded);
	const FVector Forward = SupportAim.GetUnitAxis(EAxis::X);
	const FVector Up = SupportGrip.GetUnitAxis(EAxis::Z);
	// In the authored held frame, -Y is the barrel, -X is up, Z spans the limbs.
	const FQuat Rotation = MissileStyle() == 0x20 ? FRotationMatrix::MakeFromYX(-Forward, -SupportAim.GetUnitAxis(EAxis::Z)).ToQuat()
		: FRotationMatrix::MakeFromZY(-Forward, Up).ToQuat();
	Actor->SetActorLocationAndRotation(SupportGrip.GetLocation()
		- Rotation.RotateVector((MissileStyle()==0x20 ? CrossbowGripLocal : FVector::ZeroVector) * Actor->GetActorScale3D()), Rotation);
	Actor->SetActorEnableCollision(false);
	return true;
}

void UACEVRComponent::UpdateMissileTrajectory(const FVector& Origin, const FVector& Direction, float Power, float SpellSpeed, bool Gravity)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACETrajectory);
	PredictedCombatTarget.Reset();
	const auto Session = Client ? Client->GetSession() : nullptr;
	const bool Spell = SpellSpeed > 0.f;
	const float BaseSpeed = Spell ? SpellSpeed : Session ? Session->GetVRMissileSpeed(EquippedMissileWeapon().Guid) : 0.f;
	if (BaseSpeed <= 0.f || Direction.IsNearlyZero() || IsMenuOpen()) return;
	if (!MissileTrajectory)
	{
		MissileTrajectory = NewObject<UProceduralMeshComponent>(PresentationActor);
		PresentationActor->AddInstanceComponent(MissileTrajectory);
		MissileTrajectory->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MissileTrajectory->SetCastShadow(false);
		MissileTrajectory->RegisterComponent();
		MissileTrajectory->SetMaterial(0, ACEVRPointerVisuals::GetMaterial(false));
	}
	float Radius=.05f;
	if (Spell) { float ProfileSpeed; bool ProfileGravity; Session->GetVRSpellProfile(SelectedSpell,ProfileSpeed,ProfileGravity,&Radius); }
	else if (auto* Dat=GetWorld()->GetGameInstance()->GetSubsystem<UACEDatSubsystem>())
	{
		const auto Ammo=MissileStyle()==0x80 || MissileStyle()==0x800 ? EquippedMissileWeapon() : EquippedAmmo(); float Step,Height; uint32 Animation;
		if (Dat->TryGetSetupPhysics(Ammo.SetupId,Step,Height,Radius,Animation)) Radius*=Ammo.Scale;
	}
	Radius=FMath::Max(.001f,Radius)*PC->WorldScale;
	// Six seconds of free flight, sampled finely enough to follow small bodies.
	// One broad-phase query covers the curve; there is no full-world entity scan.
	const int32 Segments = Gravity ? 48 : 1;
	TArray<FVector,TInlineAllocator<49>> Path; Path.Add(Origin);
	FBox Bounds(Origin,Origin);
	for (int32 I=1;I<=Segments;++I)
	{
		Path.Add(Origin+(Gravity ? ACEVRMath::BallisticOffset(Direction,BaseSpeed,Power,I/8.f,PC->WorldScale)
			: Direction.GetSafeNormal()*FMath::Min(10000.f,BaseSpeed*6.f*PC->WorldScale)));
		Bounds+=Path.Last();
	}
	Bounds=Bounds.ExpandBy(Radius+PC->WorldScale*.10f);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(VRProjectilePreview),false); IgnoreHeld(GetOwner(),Query);
	TArray<FOverlapResult> Nearby;
	GetWorld()->OverlapMultiByObjectType(Nearby,Bounds.GetCenter(),FQuat::Identity,
		FCollisionObjectQueryParams::AllObjects,FCollisionShape::MakeBox(Bounds.GetExtent()),Query);
	TSet<AACEWorldEntityActor*> Bodies;
	for (const auto& Candidate:Nearby)
		if (auto* Body=Cast<AACEWorldEntityActor>(Candidate.GetActor()); Body && Body->IsStandingCreatureOrPlayer())
		{
			Query.AddIgnoredActor(Body);
			TArray<AActor*> Held; Body->GetAttachedActors(Held,true,true); Query.AddIgnoredActors(Held);
			if (!Body->IsHidden() && Body->IsCellVisible() && !Body->bReceivedDeathMotion
				&& !(Body->PhysicsState & (ACEPhysicsState::Ethereal|ACEPhysicsState::IgnoreCollisions))) Bodies.Add(Body);
		}
	struct FBodyCandidate { AACEWorldEntityActor* Actor;FBox Bounds; };
	TArray<FBodyCandidate,TInlineAllocator<32>> Candidates;
	const bool UseBounds=Segments>1 && CVarProjectileBodyBounds.GetValueOnGameThread()!=0;
	for(auto* Body:Bodies) Candidates.Add({Body,UseBounds ? Body->GetProjectileContactBounds(Radius) : FBox(ForceInit)});
	TArray<FVector> Vertices, Normals; TArray<FVector2D> UVs; TArray<int32> Triangles;
	Vertices.Reserve(Segments*8);Normals.Reserve(Segments*8);UVs.Reserve(Segments*8);Triangles.Reserve(Segments*18);
	TArray<FLinearColor> Colors; TArray<FProcMeshTangent> Tangents;
	FVector Previous = Origin, End = Origin; bool HitWorld = false;
	AACEWorldEntityActor* HitBody = nullptr;
	for (int32 I=0; I<Segments; ++I)
	{
		FVector Next = HitWorld ? End : Path[I+1];
		if (!HitWorld)
		{
			FHitResult Hit;
			float Earliest=1.f;
			if (GetWorld()->SweepSingleByChannel(Hit,Previous,Next,FQuat::Identity,ECC_Pawn,FCollisionShape::MakeSphere(Radius),Query))
			{ Earliest=Hit.Time; HitWorld=true; }
			const FBox SegmentBounds(Previous.ComponentMin(Next),Previous.ComponentMax(Next));
			for (const auto& Candidate:Candidates)
			{
				if(UseBounds && !SegmentBounds.Intersect(Candidate.Bounds)) continue;
				auto* Body=Candidate.Actor;
				float Along;
				if (Body->FindProjectileContact(Previous,Next,Radius,Along) && Along<Earliest)
				{ Earliest=Along; HitWorld=true; HitBody=Body; }
			}
			Next=FMath::Lerp(Previous,Next,Earliest);
		}
		const float Width=FMath::Clamp(float(FVector::Distance(Head->GetComponentLocation(),Previous))*.0007f,.4f,4.f);
		const FVector Side = FVector::CrossProduct(Next-Previous, Head->GetComponentLocation()-Previous).GetSafeNormal() * Width;
		const int32 V = Vertices.Num();
		for (int32 Column = 0; Column < 4; ++Column)
		{
			const float Offset = Column == 0 ? -3.f : Column == 1 ? -1.f : Column == 2 ? 1.f : 3.f;
			FLinearColor Color = ACEVRPointerVisuals::ModeColor(Spell ? 1 : 2);
			Color.A = (Column == 0 || Column == 3) ? 0.f : 1.f;
			Vertices.Append({Previous + Side * Offset, Next + Side * Offset});
			for (int32 J=0;J<2;++J) { Normals.Add(FVector::UpVector); UVs.Add(FVector2D::ZeroVector); Colors.Add(Color); }
			if (Column < 3) { const int32 A=V+Column*2,B=A+2;Triangles.Append({A,B,A+1,B,B+1,A+1}); }
		}
		Previous=End=Next;
	}
	// One mesh section for the entire arc, rather than one draw call per segment.
	MissileTrajectory->SetWorldLocation(Origin);
	for (FVector& V : Vertices) V -= Origin;
	const auto* Section=MissileTrajectory->GetProcMeshSection(0);
	// Switching between a straight bolt and an arc changes the index topology.
	if (!Section || Section->ProcVertexBuffer.Num()!=Vertices.Num() || Section->GetRenderIndexCount()!=Triangles.Num())
		MissileTrajectory->CreateMeshSection_LinearColor(0,Vertices,Triangles,Normals,UVs,Colors,Tangents,false);
	else MissileTrajectory->UpdateMeshSection_LinearColor(0,Vertices,Normals,UVs,Colors,Tangents);
	MissileTrajectory->SetVisibility(true);
	PredictedCombatTarget=HitBody;
	const bool AimLeft = (Spell || !IsAmmoLauncher()) ? Settings->bLeftHanded : !Settings->bLeftHanded;
	const int32 Side = AimLeft ? 0 : 1;
	PointerBeams[Side]->SetVisibility(false);
	PointerTips[Side]->SetWorldLocation(End); PointerTips[Side]->SetVisibility(true);
	PointerTips[Side]->SetWorldScale3D(FVector(FMath::Clamp(float(FVector::Distance(Origin,End))*.00003f,.02f,.12f)));
	// The line, endpoint and model tint provide combat feedback without a panel
	// covering the target. Interaction instructions remain in peace mode.
}

void UACEVRComponent::UpdateAmmoVisual(const FACEWorldObject& Ammo, const FVector& Nock, const FVector& Direction, bool bAtTip)
{
	if (!AmmoActor)
	{
		AmmoActor = GetWorld()->SpawnActor<AActor>(); AmmoActor->SetOwner(GetOwner());
		auto* Root = NewObject<USceneComponent>(AmmoActor); AmmoActor->AddInstanceComponent(Root);
		AmmoActor->SetRootComponent(Root); Root->RegisterComponent();
		AmmoActor->AttachToActor(PresentationActor, FAttachmentTransformRules::KeepWorldTransform);
		auto* App = NewObject<UACECharacterAppearanceComponent>(AmmoActor); AmmoActor->AddInstanceComponent(App);
		App->bUseWorldLighting = true; App->RegisterComponent();
		AmmoActor->SetActorEnableCollision(false);
	}
	auto* App = AmmoActor->FindComponentByClass<UACECharacterAppearanceComponent>();
	if (AmmoVisualGuid != Ammo.Guid || App->GetSetupId() != Ammo.SetupId || App->GetAppliedAppearanceHash() != Ammo.Appearance.GetContentHash())
	{
		FACEWorldObject Copy = Ammo; Copy.ParentGuid = Copy.ParentLocation = 0; Copy.PlacementId=52; // MissileFlight, fallback 0.
		Copy.MotionTableId = Copy.DefaultAnimationId = 0;
		App->ApplyWorldObject(Copy, PC->WorldScale, false); AmmoVisualGuid = Ammo.Guid;
		FBox Bounds(ForceInit);
		for (int32 I=0;I<App->GetPartCount();++I)
			if (auto* Part=Cast<UProceduralMeshComponent>(App->GetPartMesh(I)))
			{
				FTransform Bind; if (!App->GetPartBindTransform(I,Bind)) continue;
				for (int32 S=0;S<Part->GetNumSections();++S)
					if (const auto* Section=Part->GetProcMeshSection(S); Section && Section->bSectionVisible)
						for (const auto& V:Section->ProcVertexBuffer) Bounds+=Bind.TransformPosition(V.Position);
			}
		AmmoNockLocal=Bounds.IsValid ? FVector(Bounds.GetCenter().X,Bounds.Min.Y,Bounds.GetCenter().Z) : FVector::ZeroVector;
		AmmoTipLocal=Bounds.IsValid ? FVector(Bounds.GetCenter().X,Bounds.Max.Y,Bounds.GetCenter().Z) : FVector::ZeroVector;
	}
	const FQuat Rotation = FRotationMatrix::MakeFromYZ(Direction, GetPhysicalGrip(!Settings->bLeftHanded).GetUnitAxis(EAxis::Z)).ToQuat();
	AmmoActor->SetActorLocationAndRotation(Nock-Rotation.RotateVector(bAtTip ? AmmoTipLocal : AmmoNockLocal), Rotation);
	AmmoActor->SetActorHiddenInGame(!App->HasAppearance());
	if (!App->HasAppearance()) { Beam(Arrow, Nock, Nock + Direction * 70.f, .7f); Arrow->SetVisibility(true); }
}
