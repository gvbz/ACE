#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/WidgetInteractionComponent.h"
#include "ACEVRNativeHUD.h"
#include "ACEPlayerController.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUIElementManager.h"

namespace
{
	bool ProjectToViewPlane(const FTransform& View, const UMotionControllerComponent* Aim, float Distance, FVector2D& Point)
	{
		if (!Aim || !Aim->IsTracked()) return false;
		const FVector Origin = View.InverseTransformPosition(Aim->GetComponentLocation());
		const FVector Direction = View.InverseTransformVectorNoScale(Aim->GetForwardVector());
		if (Direction.X <= .05f) return false;
		const float T = (Distance - Origin.X) / Direction.X;
		if (T < 0.f || T > 500.f) return false;
		const FVector Hit = Origin + Direction * T;
		Point = FVector2D(Hit.Y, Hit.Z);
		return true;
	}
}

FTransform UACEVRComponent::GetVitalsAnchorTransform() const
{
	return Settings->VitalsAnchorMode == 0 ? Head->GetComponentTransform() : VitalsAnchorFrame;
}

namespace
{
	void UpdateHUDAnchor(FTransform& Frame, FVector& PreviousOwner, bool& Ready, bool& Turning, const FTransform& Head,
		const FVector& Owner, int32 Mode, bool Held, float Dt, float FollowAngle = 28.f)
	{
		const FTransform Target(FRotator(0, Head.Rotator().Yaw, 0), Head.GetLocation());
		const FVector Travel = Ready ? Owner - PreviousOwner : FVector::ZeroVector;
		PreviousOwner = Owner;
		if (!Ready) { Frame = Target; Ready = true; Turning = false; }
		if (Mode != 1 || Held) { Turning = false; return; }
		Dt = FMath::Clamp(Dt, 0.f, .1f);
		// Transport locomotion exactly; smooth only room-scale/head motion.
		const FVector Transported = Frame.GetLocation() + Travel;
		const FVector Error = Target.GetLocation() - Transported;
		// A resting head has sub-centimetre tracking noise. A soft positional
		// dead band keeps that out of body-follow panels without accumulating
		// drift; deliberate room-scale motion still follows exponentially.
		const FVector Correction = Error.GetSafeNormal() * FMath::Max(0., Error.Size() - .75);
		const FVector Smoothed = Transported + Correction * (1.f-FMath::Exp(-Dt*6.f));
		Frame.SetLocation(Target.GetLocation() + (Smoothed - Target.GetLocation()).GetClampedToMaxSize(12.f));
		const float Difference = FMath::FindDeltaAngleDegrees(Frame.Rotator().Yaw, Target.Rotator().Yaw);
		// Separate start/stop thresholds prevent tracking noise at the edge of
		// the viewing cone from repeatedly starting and stopping the follow.
		if (!Turning && FMath::Abs(Difference) > FollowAngle) Turning = true;
		if (Turning && FMath::Abs(Difference) < FollowAngle - 10.f) Turning = false;
		if (Turning)
			Frame.SetRotation(FRotator(0, Frame.Rotator().Yaw + (Difference - FMath::Sign(Difference)*(FollowAngle - 13.f))
				* (1.f-FMath::Exp(-Dt*3.f)), 0).Quaternion());
	}
}

void UACEVRComponent::UpdateVitalsAnchor(float Dt)
{
	UpdateHUDAnchor(VitalsAnchorFrame, VitalsOwnerLocation, bVitalsAnchorReady, bVitalsAnchorTurning, Head->GetComponentTransform(),
		GetOwner()->GetActorLocation(), Settings->VitalsAnchorMode, VitalsDragHand != INDEX_NONE || EditingPanel == "Vitals", Dt);
}

FTransform UACEVRComponent::GetCompassAnchorTransform() const
{
	return Settings->CompassAnchorMode == 0 ? Head->GetComponentTransform() : CompassAnchorFrame;
}

void UACEVRComponent::UpdateCompassAnchor(float Dt)
{
	UpdateHUDAnchor(CompassAnchorFrame, CompassOwnerLocation, bCompassAnchorReady, bCompassAnchorTurning, Head->GetComponentTransform(),
		GetOwner()->GetActorLocation(), Settings->CompassAnchorMode, EditingPanel == "Compass", Dt);
}

void UACEVRComponent::ToggleVitalsLock()
{
	EndVitalsDrag(false); EndPanelEdit(false);
	Settings->bVitalsLocked = !Settings->bVitalsLocked;
	Settings->Persist();
}

FTransform UACEVRComponent::GetChatAnchorTransform() const
{
	return Settings->ChatAnchorMode==0 ? Head->GetComponentTransform() : ChatAnchorFrame;
}

FTransform UACEVRComponent::GetMenuAnchorTransform() const
{
	return Settings->MenuAnchorMode == 0 ? Head->GetComponentTransform() : MenuAnchorFrame;
}

void UACEVRComponent::UpdateMenuAnchor(float Dt)
{
	// Inventory is much wider than the small HUD meters. Allow an edge-to-edge
	// head scan, including resized/offset panels, before body follow begins.
	float FollowAngle = Settings->MenuRecenterDegrees;
	const FQuat PanelRotation = Settings->MenuViewRotation.Quaternion() * FRotator(0,180,0).Quaternion();
	auto IncludePanel = [&](FVector2D Size, FVector Offset)
	{
		for (float Side : {-1.f, 1.f}) for (float Up : {-1.f, 1.f})
		{
			const FVector Corner = Settings->MenuViewOffset + PanelRotation.RotateVector(
				(Offset + FVector(0, Side*Size.X*.5, Up*Size.Y*.5)) * Settings->PanelScale);
			FollowAngle = FMath::Max(FollowAngle, float(FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(Corner.Y, Corner.X)))) + 12.f);
		}
	};
	IncludePanel(!bUseDesktopMenu && GameplayMenuPanel ? GameplayMenuPanel->GetDrawSize() : RetailPanel->GetDrawSize(), FVector::ZeroVector);
	if (!bUseDesktopMenu && MenuInspectionPanel && MenuInspectionPanel->IsVisible())
		IncludePanel(MenuInspectionPanel->GetDrawSize(), MenuInspectionPanel->GetRelativeLocation());
	UpdateHUDAnchor(MenuAnchorFrame, MenuOwnerLocation, bMenuAnchorReady, bMenuAnchorTurning, Head->GetComponentTransform(),
		GetOwner()->GetActorLocation(), Settings->MenuAnchorMode, EditingPanel == "Menu", Dt, FMath::Min(FollowAngle, 179.f));
}
void UACEVRComponent::UpdateChatAnchor(float Dt)
{
	UpdateHUDAnchor(ChatAnchorFrame,ChatOwnerLocation,bChatAnchorReady,bChatAnchorTurning,Head->GetComponentTransform(),
		GetOwner()->GetActorLocation(),Settings->ChatAnchorMode,EditingPanel=="Chat",Dt);
}

void UACEVRComponent::BeginVitalsDrag(bool bLeft)
{
	BeginPanelEdit("Vitals", bLeft, false);
}

FTransform UACEVRComponent::GetFellowshipAnchorTransform() const
{
	return Settings->FellowshipAnchorMode==0 ? Head->GetComponentTransform() : FellowshipAnchorFrame;
}
void UACEVRComponent::UpdateFellowshipAnchor(float Dt)
{
	UpdateHUDAnchor(FellowshipAnchorFrame,FellowshipOwnerLocation,bFellowshipAnchorReady,bFellowshipAnchorTurning,Head->GetComponentTransform(),
		GetOwner()->GetActorLocation(),Settings->FellowshipAnchorMode,EditingPanel=="Fellowship",Dt);
}

void UACEVRComponent::UpdateVitalsDrag()
{
	if (EditingPanel == "Vitals") UpdatePanelEdit();
}

void UACEVRComponent::EndVitalsDrag(bool bSave, int32 Pointer)
{
	if (EditingPanel == "Vitals") EndPanelEdit(bSave, Pointer);
	if (VitalsDragHand == INDEX_NONE || (Pointer != INDEX_NONE && Pointer != VitalsDragHand)) return;
	VitalsDragHand = INDEX_NONE;
	if (bSave) Settings->Persist();
}

UWidgetComponent* UACEVRComponent::GetEditablePanel(FName Panel) const
{
	if (Panel == "Chat") return ChatPanel;
	if (Panel == "Vitals") return VitalsPanel;
	if (Panel == "Compass") return CompassPanel;
	if (Panel == "Fellowship") return FellowshipPanel;
	if (Panel == "Menu") return RetailPanel;
	if (Panel == "Options") return SettingsPanel;
	return nullptr;
}

float& UACEVRComponent::GetPanelScale(FName Panel)
{
	if (Panel == "Chat") return Settings->ChatScale;
	if (Panel == "Vitals") return Settings->VitalsScale;
	if (Panel == "Compass") return Settings->CompassScale;
	if (Panel == "Fellowship") return Settings->FellowshipScale;
	if (Panel == "Options") return Settings->OptionsScale;
	return Settings->PanelScale;
}

bool UACEVRComponent::IsPanelLocked(FName Panel) const
{
	if (Panel == "Chat") return Settings->bChatLocked;
	if (Panel == "Vitals") return Settings->bVitalsLocked;
	if (Panel == "Compass") return Settings->bCompassLocked;
	if (Panel == "Fellowship") return Settings->bFellowshipLocked;
	if (Panel == "Menu") return Settings->bMenuLocked;
	if (Panel == "Options") return Settings->bOptionsLocked;
	return true;
}

bool UACEVRComponent::ShouldShowPanelControls(FName Panel) const
{
	const auto* Surface = GetEditablePanel(Panel);
	return Surface && (!IsPanelLocked(Panel) || IsMenuOpen() || EditingPanel == Panel ||
		(LeftPointer && LeftPointer->GetHoveredWidgetComponent() == Surface) ||
		(RightPointer && RightPointer->GetHoveredWidgetComponent() == Surface));
}

void UACEVRComponent::TogglePanelLock(FName Panel)
{
	if (!GetEditablePanel(Panel)) return;
	EndPanelEdit(false); EndVitalsDrag(false);
	if (Panel == "Chat") Settings->bChatLocked = !Settings->bChatLocked;
	else if (Panel == "Vitals") Settings->bVitalsLocked = !Settings->bVitalsLocked;
	else if (Panel == "Compass") Settings->bCompassLocked = !Settings->bCompassLocked;
	else if (Panel == "Fellowship") Settings->bFellowshipLocked = !Settings->bFellowshipLocked;
	else if (Panel == "Menu") Settings->bMenuLocked = !Settings->bMenuLocked;
	else if (Panel == "Options") Settings->bOptionsLocked = !Settings->bOptionsLocked;
	Settings->Persist();
}

void UACEVRComponent::BeginPanelEdit(FName Panel, bool bLeft, bool bResize)
{
	auto* Surface = GetEditablePanel(Panel);
	const auto* HitSurface = Panel == "Menu" && GameplayMenuPanel && GameplayMenuPanel->IsVisible() ? GameplayMenuPanel.Get() : Surface;
	if (!bTracking || !HitSurface || !HitSurface->IsVisible() || HitSurface->GetCollisionEnabled() == ECollisionEnabled::NoCollision
		|| IsPanelLocked(Panel) || PanelEditHand != INDEX_NONE || VitalsDragHand != INDEX_NONE) return;
	const FTransform Plane(Surface->GetComponentQuat() * FRotator(0,180,0).Quaternion(), Surface->GetComponentLocation());
	FVector2D Hit;
	if (!ProjectToViewPlane(Plane, bLeft ? LeftAim.Get() : RightAim.Get(), 0, Hit) || (bResize && Hit.SizeSquared() < 4.)) return;
	EditingPanel = Panel; PanelEditHand = bLeft ? 0 : 1; bPanelResize = bResize;
	if (Panel == "Chat" && !bResize) bChatDocked=false;
	PanelEditFrame = Plane; PanelEditStart = Hit; PanelEditLocation = Surface->GetComponentLocation();
	PanelEditScale = GetPanelScale(Panel);
	FTransform Pose=Surface->GetComponentTransform();Pose.SetScale3D(FVector::OneVector);
	FTransform Hand=(bLeft ? LeftAim.Get() : RightAim.Get())->GetComponentTransform();Hand.SetScale3D(FVector::OneVector);
	PanelGrabInHand=Pose.GetRelativeTransform(Hand);
}

void UACEVRComponent::UpdatePanelEdit(float Dt)
{
	if (PanelEditHand == INDEX_NONE) return;
	auto* Surface = GetEditablePanel(EditingPanel);
	const auto* Aim = PanelEditHand == 0 ? LeftAim.Get() : RightAim.Get();
	const auto* HitSurface = EditingPanel == "Menu" && GameplayMenuPanel && GameplayMenuPanel->IsVisible() ? GameplayMenuPanel.Get() : Surface;
	if (!bTracking || !Aim || !Aim->IsTracked() || !HitSurface || !HitSurface->IsVisible()
		|| HitSurface->GetCollisionEnabled() == ECollisionEnabled::NoCollision || IsPanelLocked(EditingPanel)) { EndPanelEdit(); return; }
	if (bPanelResize)
	{
		FVector2D Hit;
		if (!ProjectToViewPlane(PanelEditFrame, Aim, 0, Hit)) return;
		// Use the original ray plane and grab radius, never the rescaled widget geometry.
		// This preserves the center and prevents recursive scale drift while holding still.
		GetPanelScale(EditingPanel) = PanelEditScale * float(FVector2D::DotProduct(Hit, PanelEditStart) / PanelEditStart.SizeSquared());
	}
	else
	{
		// Right stick forward/back controls distance while either hand holds Move.
		// Keep the grab rotation and lateral offset, without scrolling the panel
		// or turning the player. Apply once per frame, independent of frame rate.
		const float Depth = FMath::Abs(TurnStick.Y) > Settings->StickDeadZone
			? FMath::Sign(TurnStick.Y) * (FMath::Abs(TurnStick.Y)-Settings->StickDeadZone)/(1.f-Settings->StickDeadZone) : 0.f;
		FVector Grab = PanelGrabInHand.GetLocation();
		if (Depth != 0.f) Grab.X = FMath::Clamp(Grab.X + Depth * 90.f * FMath::Clamp(Dt,0.f,.1f),20.,350.);
		PanelGrabInHand.SetLocation(Grab);
		// Keep the initial grip-to-panel transform. Translation moves the surface
		// in all three axes; turning the wrist sets pitch, yaw and roll without a snap.
		FTransform Hand=Aim->GetComponentTransform();Hand.SetScale3D(FVector::OneVector);
		const FTransform Pose=PanelGrabInHand*Hand;
		auto SaveAnchored=[&](const FTransform& Anchor,FVector& Offset,FRotator& Rotation)
		{
			Offset=Anchor.InverseTransformPosition(Pose.GetLocation());
			Rotation=(Anchor.GetRotation().Inverse()*Pose.GetRotation()*FRotator(0,180,0).Quaternion().Inverse()).Rotator();
		};
		if (EditingPanel == "Compass") SaveAnchored(GetCompassAnchorTransform(),Settings->CompassViewOffset,Settings->CompassViewRotation);
		else if (EditingPanel == "Chat") SaveAnchored(GetChatAnchorTransform(),Settings->ChatViewOffset,Settings->ChatViewRotation);
		else if (EditingPanel == "Fellowship") SaveAnchored(GetFellowshipAnchorTransform(),Settings->FellowshipViewOffset,Settings->FellowshipViewRotation);
		else if (EditingPanel == "Vitals") SaveAnchored(GetVitalsAnchorTransform(),Settings->VitalsViewOffset,Settings->VitalsViewRotation);
		else if (EditingPanel == "Menu" && !bSettingsOpen)
		{
			SaveAnchored(GetMenuAnchorTransform(),Settings->MenuViewOffset,Settings->MenuViewRotation);
		}
		else Surface->SetWorldLocationAndRotation(Pose.GetLocation(),Pose.GetRotation());
	}
	Settings->Sanitize();
	// Publish the edited anchored pose before native children copy the surface.
	if (EditingPanel == "Menu" && !bSettingsOpen) PositionPanel(Surface);
}

void UACEVRComponent::EndPanelEdit(bool bSave, int32 Pointer)
{
	if (PanelEditHand == INDEX_NONE || (Pointer != INDEX_NONE && Pointer != PanelEditHand)) return;
	if (bSave && !bPanelResize && (EditingPanel=="Options" || (EditingPanel=="Menu" && bSettingsOpen)))
	{
		const FTransform Frame=bSettingsOpen ? SettingsLayoutFrame : FTransform(FRotator(0,Head->GetComponentRotation().Yaw,0),Head->GetComponentLocation());
		FTransform Pose=GetEditablePanel(EditingPanel)->GetComponentTransform();Pose.SetScale3D(FVector::OneVector);
		Settings->PanelLayouts.Add(EditingPanel=="Menu" && bSettingsOpen ? FName("MenuPreview") : EditingPanel,Pose.GetRelativeTransform(Frame));
	}
	PanelEditHand = INDEX_NONE; EditingPanel = NAME_None;
	bWheelTurnNeutral = true;
	if (bSave) Settings->Persist();
}

bool UACEVRComponent::IsRetailLayoutLocked() const
{
	const auto* Manager = PC && PC->DatGameplayBinder ? PC->DatGameplayBinder->Manager.Get() : nullptr;
	return !Manager || Manager->IsUiLocked();
}

void UACEVRComponent::ToggleRetailLayoutLock()
{
	if (bUseDesktopMenu && PC && PC->DatGameplayBinder && PC->DatGameplayBinder->Manager)
		PC->DatGameplayBinder->Manager->ToggleUiLocked();
}

void UACEVRComponent::UpdatePanelControls(bool MainVisible, bool OptionsVisible)
{
	for (FName Name : {FName("Menu"), FName("Options")})
	{
		auto* Controls = Name == "Menu" ? MenuControlsPanel.Get() : OptionsControlsPanel.Get();
		auto* Surface = GetEditablePanel(Name);
		const bool Visible = Name == "Menu" ? MainVisible : OptionsVisible;
		const bool WasVisible = Controls->IsVisible();
		Controls->SetVisibility(Visible); Controls->SetComponentTickEnabled(Visible);
		Controls->SetCollisionEnabled(Visible ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
		const bool Retail = Name == "Menu" && bUseDesktopMenu;
		Controls->SetDrawSize(FVector2D(Retail ? 720 : 480, 48));
		// Extend alongside the existing controls; a second row overlaps the wrist hotbar.
		Controls->SetRelativeLocationAndRotation(FVector(.1, Retail ? -120 : 0, -Surface->GetDrawSize().Y*.5 - 30), FRotator::ZeroRotator);
		if (Visible)
		{
			const auto Widget = StaticCastSharedPtr<SACEVRPanelControls>(Controls->GetSlateWidget());
			if (Widget->Refresh() || !WasVisible) Controls->RequestRedraw();
		}
	}
}
