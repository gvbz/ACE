#include "VR/ACEVRComponent.h"
#include "VR/ACEVRMenu.h"
#include "VR/ACEVRSettings.h"
#include "VR/ACEVRInputLayout.h"
#include "VR/ACEVRRetailSurface.h"
#include "VR/ACEVRWidgetInteraction.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUICanvasWidget.h"
#include "ACEPlayerController.h"
#include "ACEClientSubsystem.h"
#include "ACEWorldEntityActor.h"
#include "ACESession.h"
#include "Components/InputComponent.h"
#include "Components/WidgetInteractionComponent.h"
#include "Components/WidgetComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"

void UACEVRComponent::BindInput()
{
	if (bInputBound || !PC || !PC->InputComponent) return;
	auto* I = PC->InputComponent.Get();
	I->BindAxis(TEXT("VRMoveX"), this, &UACEVRComponent::MoveX);
	I->BindAxis(TEXT("VRMoveY"), this, &UACEVRComponent::MoveY);
	I->BindAxis(TEXT("VRTurnX"), this, &UACEVRComponent::TurnX);
	I->BindAxis(TEXT("VRScrollY"), this, &UACEVRComponent::ScrollY);
	I->BindAxis(TEXT("VRLeftTriggerValue"), this, &UACEVRComponent::LeftTriggerAxis);
	I->BindAxis(TEXT("VRRightTriggerValue"), this, &UACEVRComponent::RightTriggerAxis);
	// Keep OpenXR's physical action set stable. Remap dispatch in the client so
	// changes work immediately on PC and Quest without restarting the XR session.
	for(const auto& B:ACEVRInputLayout::Buttons())for(bool Pressed:{true,false})
	{
		FInputActionBinding Binding(B.Input,Pressed?IE_Pressed:IE_Released);
		Binding.ActionDelegate.GetDelegateForManualSet().BindWeakLambda(this,[this,Name=B.Input,Pressed]()
		{
			if (Name == "VRLeftTrigger" || Name == "VRRightTrigger")
			{
				auto& State = TriggerStates[Name == "VRLeftTrigger" ? 0 : 1];
				if (!State.SetDigital(Pressed)) return;
				RouteControllerButton(Name, State.IsPressed());
			}
			else RouteControllerButton(Name,Pressed);
		});
		I->AddActionBinding(Binding);
	}
	bInputBound = true;
}

void UACEVRComponent::TriggerAxis(bool bLeft, float Value)
{
	auto& State = TriggerStates[bLeft ? 0 : 1];
	if (State.SetAnalog(Value))
		RouteControllerButton(bLeft ? FName("VRLeftTrigger") : FName("VRRightTrigger"), State.IsPressed());
}

void UACEVRComponent::InventoryPressed()
{
	if (!bActive || !bTracking || !PC || PC->bEnterWorldLoading || PC->bWorldRevealActive) return;
	// Preserve immediate tap-to-inventory. Steam Link can reserve Menu for its
	// dashboard; holding X supplies an app-owned path to options on every runtime.
	ToggleInventory();
	bInventoryButtonHeld = true;
	InventoryHoldSeconds = 0.f;
}

void UACEVRComponent::RouteControllerButton(FName Input,bool Pressed)
{
	if(!Settings)return;
	if(Pressed)
	{
		if(HeldControllerActions.Contains(Input))return;
		FName Action=Settings->GetButtonAction(Input);
		if(GameplayMenuPanel && GameplayMenuPanel->IsVisible() && !bSettingsOpen && !bTextKeyboardOpen && !bSpellWheelOpen)
		{
			const FName MenuAction=Settings->GetMenuButtonAction(Input);
			if(!MenuAction.IsNone())Action=MenuAction;
		}
		// Grip manipulates an unlocked panel under the ray before its default chat action.
		if(Action=="VRChat" && (Input=="VRLeftGrip" || Input=="VRRightGrip"))
		{
			const bool Left=Input=="VRLeftGrip";
			auto* Pointer=Left?LeftPointer.Get():RightPointer.Get();
			if(auto* P=Cast<UACEVRWidgetInteraction>(Pointer))P->RefreshHit();
			for(FName Name:{FName("Chat"),FName("Menu"),FName("Options"),FName("Vitals"),FName("Compass"),FName("Fellowship")})
			{
				auto* Surface=Name=="Menu" && GameplayMenuPanel && GameplayMenuPanel->IsVisible()?GameplayMenuPanel.Get():GetEditablePanel(Name);
				if(Pointer && Surface && Pointer->GetHoveredWidgetComponent()==Surface && !IsPanelLocked(Name))
				{
					BeginPanelEdit(Name,Left,false);
					if(PanelEditHand!=(Left?0:1))continue;
					FocusPanelSurface(Surface);Action="VRPanelGrab";break;
				}
			}
			if(Action=="VRChat" && GetCombatMode()==ACECombatMode::Missile && Left==Settings->bLeftHanded)Action=Input;
		}
		HeldControllerActions.Add(Input,Action);DispatchControllerAction(Action,true);
	}
	else
	{
		FName Action;
		if(HeldControllerActions.RemoveAndCopyValue(Input,Action))
		{
			if(Action=="VRPanelGrab")EndPanelEdit(true,Input=="VRLeftGrip"?0:1);
			else DispatchControllerAction(Action,false);
		}
	}
}
void UACEVRComponent::DispatchControllerAction(FName Action,bool Pressed)
{
	if(Action=="VRMenuUse" || Action=="VRMenuInspect")
	{
		if(!Pressed || !bActive || !bTracking || !PC || PC->bEnterWorldLoading || PC->bWorldRevealActive
			|| !GameplayMenu || !GameplayMenuPanel || !GameplayMenuPanel->IsVisible() || bLeftPointerPressed || bRightPointerPressed)return;
		for(bool Left:{FeedbackHand==0,FeedbackHand!=0})
		{
			auto* Pointer=Left?LeftPointer.Get():RightPointer.Get();
			auto* Aim=Left?LeftAim.Get():RightAim.Get();
			if(!Pointer || !Aim || !Aim->IsTracked())continue;
			if(auto* VRPointer=Cast<UACEVRWidgetInteraction>(Pointer))VRPointer->RefreshHit();
			if(Pointer->GetHoveredWidgetComponent()!=GameplayMenuPanel)continue;
			GameplayMenu->QuickAction(Action=="VRMenuInspect",true,Pointer->Get2DHitLocation());Pulse(Left,.25f);return;
		}
		GameplayMenu->QuickAction(Action=="VRMenuInspect");return;
	}
	if(Action=="VRInventory"){if(Pressed)InventoryPressed();else InventoryReleased();}
	else if(Action=="VRLeftTrigger")Trigger(true,Pressed);
	else if(Action=="VRRightTrigger")Trigger(false,Pressed);
	else if(Action=="VRLeftGrip")Grip(true,Pressed);
	else if(Action=="VRRightGrip")Grip(false,Pressed);
	else if(Action=="VRJump"){if(Pressed)JumpDown();else JumpUp();}
	else if(Action=="VRChat"){if(Pressed)ToggleChat();}
	else if(Pressed)
	{
		if(Action=="VRCombat")ToggleCombat();
		else if(Action=="VRSettings")ToggleSettings();
		else if(Action=="VRSpellWheel")ToggleSpellWheel();
		else if(Action=="VRSelect")SelectPressed();
		else if(Action=="VRPrevious")PreviousSpell();
	}
}
void UACEVRComponent::ConfigureButton(FName Input,FName Action)
{
	// Held inputs retain their original release route, so rebinding cannot leave
	// a pointer pressed or turn an in-progress jump into a different action.
	Settings->SetButtonAction(Input,Action);
}
void UACEVRComponent::ResetButtonBindings()
{
	Settings->ButtonBindings.Reset();Settings->MenuButtonBindings.Reset();Settings->Persist();
}

void UACEVRComponent::InventoryReleased()
{
	bInventoryButtonHeld = false;
	InventoryHoldSeconds = 0.f;
}

void UACEVRComponent::UpdateInventoryHold(float Dt)
{
	if (!bActive || !bTracking || !PC || PC->bEnterWorldLoading || PC->bWorldRevealActive)
	{ InventoryReleased(); return; }
	if (!bInventoryButtonHeld) return;
	InventoryHoldSeconds += FMath::Max(0.f, Dt);
	if (InventoryHoldSeconds < .65f) return;
	InventoryReleased(); // Fire once; release cannot also toggle the menu.
	if (!bSettingsOpen) ToggleSettings();
}

void UACEVRComponent::Trigger(bool bLeft, bool bPressed)
{
	if (!bActive) return;
	const int32 HandIndex=bLeft ? 0 : 1;
	if (!bPressed && bWheelTriggerConsumed[HandIndex]) { bWheelTriggerConsumed[HandIndex]=false; return; }
	if (bSpellWheelOpen)
	{
		if (bPressed) { bWheelTriggerConsumed[HandIndex]=true; if (bLeft==Settings->bLeftHanded) ConfirmWheelSpell(); }
		return;
	}
	auto* Pointer = bLeft ? LeftPointer.Get() : RightPointer.Get();
	if (auto* VRPointer = Cast<UACEVRWidgetInteraction>(Pointer)) VRPointer->RefreshHit();
	bool& Held = bLeft ? bLeftPointerPressed : bRightPointerPressed;
	if (!bPressed)
	{
		if (bLeft == Settings->bLeftHanded && bTriggerDrawing)
		{
			bTriggerDrawing = false; Grip(bLeft, false);
		}
		if (VitalsDragHand == (bLeft ? 0 : 1)) EndVitalsDrag();
		if (PanelEditHand == (bLeft ? 0 : 1)) EndPanelEdit();
		UIAimOverride = bLeft ? LeftAim : RightAim;
		if(Held && GameplayMenu)
			GameplayMenu->FinishItemPointer(Pointer->GetHoveredWidgetComponent()==GameplayMenuPanel,Pointer->Get2DHitLocation(),
				!Pointer->GetHoveredWidgetComponent() && bTracking && (bLeft?LeftAim:RightAim)->IsTracked());
		if (Held && PC->DatGameplayBinder)
		{
			auto* Binder = PC->DatGameplayBinder.Get();
			auto* Hovered = Pointer->GetHoveredWidgetComponent();
			FVector2D Drop = Pointer->Get2DHitLocation();
			bool RetailDrop = Hovered == RetailPanel;
			if (Hovered == WristPanel || Hovered == VitalsPanel) Binder->CancelPointerGestures();
			if (RetailDrop)
			{
				// A captured Slate pointer keeps the source window's geometry. Use
				// the actual destination when crossing from the canvas to a pinned window.
				Binder->UpdateSpellDrag(Drop); Binder->UpdateInventoryDrag(Drop);
				if (Binder->IsSpellDragActive()) Binder->TryFinishSpellDrag(Drop);
				if (Binder->IsInventoryDragActive()) Binder->TryFinishInventoryDrag(Drop);
			}
			else if (!Hovered && bTracking && (bLeft ? LeftAim : RightAim)->IsTracked())
			{
				// A fast drag can leave the source quad before it receives a move.
				Binder->UpdateInventoryDrag(FVector2D(-1000, -1000));
				if (Binder->IsInventoryDragActive()) Binder->TryFinishInventoryDrag(FVector2D(-1000, -1000));
				Binder->CancelPointerGestures();
			}
			else Binder->CancelPointerGestures();
		}
		if (Held) Pointer->ReleasePointerKey(EKeys::LeftMouseButton);
		if (Held && PC->DatCanvasWidget) PC->DatCanvasWidget->ResetPointerOwnership();
		UIAimOverride.Reset();
		Held = false; UpdateInteractionFeedback(bTracking && !bKeyboardOpen && !bSettingsOpen); return;
	}
	const auto* Aim = bLeft ? LeftAim.Get() : RightAim.Get();
	if (Client)
		UE_LOG(LogTemp, Log, TEXT("ACE VR trigger: hand=%s head=%d aim=%d hit=%s clickable=%d"),
			bLeft ? TEXT("left") : TEXT("right"), bTracking, Aim->IsTracked(), *GetNameSafe(Pointer->GetHoveredWidgetComponent()), Pointer->IsOverHitTestVisibleWidget());
	if (!bTracking || !Aim->IsTracked() || PC->bEnterWorldLoading || PC->bWorldRevealActive) return;
	auto* TextEntry = TextEntryUnderPointer(Pointer);
	if (UsesPlatformKeyboard() && TextEntry)
	{
		FocusPanelSurface(Pointer->GetHoveredWidgetComponent());
		// Native editing provides caret/selection. Do not also send a Slate mouse
		// press: its permanent keyboard entry competes with the native session.
		FocusTextEntry(TextEntry); Pulse(bLeft, .2f); return;
	}
	if (bTextKeyboardOpen && Pointer->GetHoveredWidgetComponent() != KeyboardPanel)
	{
		DismissTextEntry(); UpdatePanels();
		// Lobby controls stay visible beside the keyboard. One trigger click on
		// Finish or navigation should both dismiss editing and operate the control.
		if (!UsesPlatformKeyboard() && Client && Client->GetSessionState() == EACESessionState::InWorld) return;
	}
	if (Pointer->IsOverHitTestVisibleWidget())
	{
		if (bLeftPointerPressed || bRightPointerPressed) return;
		FeedbackHand = bLeft ? 0 : 1;
		auto* Entry = TextEntryUnderPointer(Pointer);
		FocusPanelSurface(Pointer->GetHoveredWidgetComponent());
		Pointer->PressPointerKey(EKeys::LeftMouseButton); Held = true;
		Pulse(bLeft, .2f); UpdateInteractionFeedback(true);
		if (Entry) FocusTextEntry(Entry);
		return;
	}
	if (IsPointerNearPanel(bLeft))
	{
		if (bLeft == Settings->bLeftHanded && GetCombatMode() == ACECombatMode::Magic)
			SetCastFeedback(TEXT("Aim away from the panel to cast."));
		return;
	}
	if (bLeft != Settings->bLeftHanded) return;
	if (GetCombatMode() == ACECombatMode::Magic) { FireSpell(); return; }
	if (IsInputBlocked()) return;
	if (GetCombatMode() == ACECombatMode::Missile)
	{
		if (MissileStyle() == 0x20) FireCrossbow();
		else if (MissileStyle() == 0x10) { Grip(bLeft, true); bTriggerDrawing = bDrawing; }
		else FireThrownMissile();
	}
	else if (GetCombatMode() == ACECombatMode::NonCombat)
	{
		FVector Hit;
		if (auto* Target = AimTarget(Hit))
		{
			Client->SelectObject(Target->GetACEGuid());
			Pulse(bLeft, .25f);
			// Only interact within reach; do not trigger automatic walking in VR.
			FACEWorldObject Object;
			if (Client->GetWorldObject(Target->GetACEGuid(), Object))
			{
				const FVector TargetFeet = Object.Position.ToUnrealLocation(PC->WorldScale);
				const FVector Feet = WeaponAim()->GetComponentLocation() - FACEPosition::AceVectorToUnreal(ToAceOffset(WeaponAim()->GetComponentLocation()), PC->WorldScale);
				if (PC->GetUseCylinderDistanceCm(Object, Feet, TargetFeet) <= Object.UseRadius * PC->WorldScale + 5.f)
					PC->InteractWithObject(Target->GetACEGuid());
			}
		}
		else { Client->SelectObject(0); Pulse(bLeft, .12f); }
	}
}

void UACEVRComponent::SelectPressed()
{
	if (bSpellWheelOpen) { ChangeWheelTab(1); return; }
	if (bLeftPointerPressed || bRightPointerPressed) return;
	if (!bTracking || PC->bEnterWorldLoading || PC->bWorldRevealActive) return;
	for (const bool Left : {FeedbackHand == 0, FeedbackHand != 0})
	{
		auto* Aim = Left ? LeftAim.Get() : RightAim.Get();
		auto* Pointer = Left ? LeftPointer.Get() : RightPointer.Get();
		if (!Aim || !Aim->IsTracked() || !Pointer) continue;
		if (auto* VRPointer = Cast<UACEVRWidgetInteraction>(Pointer)) VRPointer->RefreshHit();
		if (!Pointer->IsOverHitTestVisibleWidget()) continue;
		if (Pointer->GetHoveredWidgetComponent() == RetailPanel && PC->DatGameplayBinder
			&& PC->DatGameplayBinder->ActivateVRPointerItem(Pointer->Get2DHitLocation()))
		{ FeedbackHand = Left ? 0 : 1; Pulse(Left, .5f); UpdateInteractionFeedback(true); return; }
		Trigger(Left, true); Trigger(Left, false); return;
	}
	if (IsInputBlocked()) return;
	if (GetCombatMode() == ACECombatMode::Magic) { CycleSpell(1); return; }
	FVector Impact;
	if (auto* Target = AimTarget(Impact)) { Client->SelectObject(Target->GetACEGuid()); Pulse(Settings->bLeftHanded); }
	else Client->SelectObject(0);
}

void UACEVRComponent::PreviousSpell()
{
	if (bSpellWheelOpen) { ChangeWheelTab(-1); return; }
	// B is contextual in VR. Inspect the pointed item without a mouse press,
	// drag, use action, or a change to the selected spell.
	if (!bActive || !bTracking || !PC || PC->bEnterWorldLoading || PC->bWorldRevealActive
		|| bLeftPointerPressed || bRightPointerPressed || bTextKeyboardOpen || bSettingsOpen) return;
	for (const bool Left : {FeedbackHand == 0, FeedbackHand != 0})
	{
		auto* Aim = Left ? LeftAim.Get() : RightAim.Get();
		auto* Pointer = Left ? LeftPointer.Get() : RightPointer.Get();
		if (!Aim || !Aim->IsTracked() || !Pointer) continue;
		if (auto* VRPointer = Cast<UACEVRWidgetInteraction>(Pointer)) VRPointer->RefreshHit();
		if (Pointer->GetHoveredWidgetComponent() != RetailPanel) continue;
		if (PC->DatGameplayBinder && PC->DatGameplayBinder->InspectVRPointerItem(Pointer->Get2DHitLocation()))
		{
			FeedbackHand = Left ? 0 : 1; Pulse(Left, .35f); UpdateInteractionFeedback(true);
		}
		return; // Empty slots and window chrome do not change the combat hotbar.
	}
	if (!IsMenuOpen() && GetCombatMode()==ACECombatMode::NonCombat)
	{
		FVector Hit;
		if (auto* Target=AimTarget(Hit))
		{
			Client->SelectObject(Target->GetACEGuid());
			Client->SendIdentifyObject(Target->GetACEGuid()); Pulse(Settings->bLeftHanded,.3f);
		}
	}
	else if (!IsMenuOpen()) CycleSpell(-1);
}

void UACEVRComponent::Grip(bool bLeft, bool bPressed)
{
	if (bSpellWheelOpen) { if (bPressed) ChangeWheelPage(bLeft ? -1 : 1); return; }
	(bLeft ? bLeftGripHeld : bRightGripHeld) = bPressed;
	if (!bActive || bLeft != Settings->bLeftHanded) return;
	if (!bPressed)
	{
		if (bDrawing)
		{
			ReleaseArrow();
		}
		return;
	}
	if (IsInputBlocked() || GetCombatMode() != ACECombatMode::Missile || !WeaponGrip()->IsTracked()
		|| (IsAmmoLauncher() && !BowGrip()->IsTracked())) return;
	if (MissileStyle() != 0x10) return; // Only bows require a draw gesture.
	if (bDrawing) return;
	const bool BowLike = IsAmmoLauncher();
	if (BowLike && EquippedAmmo().Guid == 0) { SetCastFeedback(TEXT("Equip compatible arrows first.")); return; }
	const FVector Nock = GetPhysicalGrip(!Settings->bLeftHanded).GetLocation() - GetPhysicalAim(!Settings->bLeftHanded).GetUnitAxis(EAxis::X) * 12.f;
	if (BowLike && FVector::Distance(GetPhysicalGrip(Settings->bLeftHanded).GetLocation(), Nock) <= 30.f)
	{
		bDrawing = true; BowFraction = BowHoldTime = 0.f; Pulse(bLeft);
		SetCastFeedback(TEXT("Arrow nocked. Pull back, then release the grip or trigger."));
	}
	else if (!BowLike) { bDrawing = true; BowFraction = 1.f; BowHoldTime = 0.f; }
	else SetCastFeedback(TEXT("Bring your weapon hand to the arrow at the bow, then hold grip or trigger."));
}

void UACEVRComponent::JumpDown()
{
	if (!bActive || IsInputBlocked() || bSpellWheelOpen || bJumpHeld) return;
	bJumpHeld = true; JumpHoldSeconds = 0.f; PC->BeginJumpCharge();
}

void UACEVRComponent::JumpUp()
{
	if (!bActive || !bJumpHeld) return;
	bJumpHeld = false;
	if (!IsInputBlocked())
	{
		float Forward, Right; GetMovement(Forward, Right, PC->bRunning);
		UE_LOG(LogTemp, Log, TEXT("ACE VR jump released: hold=%.3f charge=%.3f"), JumpHoldSeconds, PC->JumpChargeExtent);
		PC->ReleaseJump(Forward, Right);
	}
	else { PC->bJumpCharging = false; PC->JumpChargeExtent = 0.f; }
	JumpHoldSeconds = 0.f;
}

void UACEVRComponent::CancelGestures()
{
	if(GameplayMenu)GameplayMenu->CancelItemPointer();
	CloseSpellWheel();
	EndVitalsDrag(); EndPanelEdit();
	if (PC && PC->DatGameplayBinder) PC->DatGameplayBinder->CancelPointerGestures();
	if (PC && PC->DatCanvasWidget)
	{
		// Cancel before synthesizing releases: tracking loss must not click the
		// hovered creator button or leave a slider held by the missing hand.
		PC->DatCanvasWidget->CancelPointerGestures();
	}
	if (LeftPointer && bLeftPointerPressed) LeftPointer->ReleasePointerKey(EKeys::LeftMouseButton);
	if (RightPointer && bRightPointerPressed) RightPointer->ReleasePointerKey(EKeys::LeftMouseButton);
	bLeftPointerPressed = bRightPointerPressed = false;
	bDrawing = bTriggerDrawing = false; BowFraction = BowHoldTime = 0.f;
	Swing.Reset();
	for (auto& Punch : Punches) Punch.Gesture.Reset();
	bPreviousContactBlade = false;
	bLeftGripHeld = bRightGripHeld = false;
	if (PC && bJumpHeld) { PC->bJumpCharging = false; PC->JumpChargeExtent = 0.f; }
	bJumpHeld = false; JumpHoldSeconds = 0.f;
}

void UACEVRComponent::Pulse(bool bLeft, float Strength)
{
	if (!Settings->bHaptics || !PC) return;
	PC->SetHapticsByValue(.6f, Strength, bLeft ? EControllerHand::Left : EControllerHand::Right);
	FTimerHandle Handle;
	TWeakObjectPtr<AACEPlayerController> WeakPC = PC;
	GetWorld()->GetTimerManager().SetTimer(Handle, [WeakPC, bLeft]()
	{ if (WeakPC.IsValid()) WeakPC->SetHapticsByValue(0.f, 0.f, bLeft ? EControllerHand::Left : EControllerHand::Right); }, .05f, false);
}
