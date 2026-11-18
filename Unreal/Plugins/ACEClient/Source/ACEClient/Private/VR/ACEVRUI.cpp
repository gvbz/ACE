#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "VR/ACEVRRetailSurface.h"
#include "VR/ACEVRMenu.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "Protocol/ACECombatChat.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIGameplayBinder.h"
#include "ACELoginWidget.h"
#include "UI/ACEGameHUDWidget.h"
#include "Camera/CameraComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/WidgetInteractionComponent.h"
#include "Components/StaticMeshComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "ACEVisibleObjectPick.h"
#include "ACERuntimeOptions.h"
#include "UI/ACEFrameRateWidget.h"
#include "ProceduralMeshComponent.h"
#include "VR/ACEVRPointerVisuals.h"
#include "UI/ACEUIElementManager.h"

void UACEVRComponent::PositionPanel(UWidgetComponent* Panel)
{
	if (!Panel || !Head) return;
	if (Panel == RetailPanel && Client && Client->GetSessionState() == EACESessionState::InWorld && !bSettingsOpen)
	{
		if (!bInventoryOpen && !bMenuAnchorReady) return;
		if (!bMenuAnchorReady) UpdateMenuAnchor(0.f);
		const FTransform Anchor = GetMenuAnchorTransform();
		Panel->SetWorldLocationAndRotation(Anchor.TransformPosition(Settings->MenuViewOffset),
			Anchor.GetRotation() * Settings->MenuViewRotation.Quaternion() * FRotator(0,180,0).Quaternion());
		return;
	}
	if (Client && Client->GetSessionState()==EACESessionState::InWorld)
	{
		const FName Key=Panel==SettingsPanel ? FName("Options") : bSettingsOpen ? FName("MenuPreview") : FName("Menu");
		if (const auto* Saved=Settings->PanelLayouts.Find(Key))
		{
			const FTransform Frame=bSettingsOpen ? SettingsLayoutFrame : FTransform(FRotator(0,Head->GetComponentRotation().Yaw,0),Head->GetComponentLocation());
			const FTransform Pose=*Saved*Frame;
			Panel->SetWorldLocationAndRotation(Pose.GetLocation(),Pose.GetRotation());return;
		}
	}
	if (bSettingsOpen && Client && Client->GetSessionState() == EACESessionState::InWorld
		&& (Panel == SettingsPanel || Panel == RetailPanel))
	{
		// Put options straight ahead at a comfortable depth and retain the live
		// AC canvas as a separate preview to the left. Neither plane follows the
		// head while editing: a pinned canvas must not sweep through the options.
		const float Angle = Panel == RetailPanel ? -70.f : 0.f;
		const float PreviewDistance=FMath::Max(110.f,Settings->PanelScale*640.f/FMath::Tan(FMath::DegreesToRadians(35.f)));
		const float Distance = FMath::Max(Panel == RetailPanel ? PreviewDistance : 130.f, Settings->PanelDistance);
		const FVector Local = FRotator(0,Angle,0).RotateVector(FVector(Distance,0,0)) + FVector(0,0,-10);
		const FVector Position = SettingsLayoutFrame.TransformPosition(Local);
		Panel->SetWorldLocationAndRotation(Position, (SettingsLayoutFrame.GetLocation()-Position).Rotation());
		return;
	}
	const FRotator Facing(0.f, Head->GetComponentRotation().Yaw, 0.f);
	const bool Login = Panel == RetailPanel && PC && PC->LoginWidget && Panel->GetWidget() == PC->LoginWidget;
	const bool Lobby = Panel == RetailPanel && (!Client || Client->GetSessionState() != EACESessionState::InWorld);
	const float Distance = Lobby ? FMath::Max(150.f, Settings->PanelDistance) : Settings->PanelDistance;
	Panel->SetWorldLocation(Head->GetComponentLocation() + Facing.Vector() * Distance + FVector(0, 0, Login ? 4.f : -10.f));
	Panel->SetWorldRotation(FRotator(0.f, Facing.Yaw + 180.f, 0.f));
}

bool UACEVRComponent::IsLoginPanelReady() const
{
	// An unworn headset hides and stops painting panels. Their geometry can be
	// zero until tracking resumes; this must not keep rebuilding the login flow.
	return bActive && PC && PC->LoginWidget && RetailPanel && RetailPanel->GetWidget() == PC->LoginWidget;
}

void UACEVRComponent::UpdatePanels(float Dt)
{
	const bool InWorld = Client && Client->GetSessionState() == EACESessionState::InWorld;
	const bool ShowFPS=InWorld && bTracking && ACERuntimeOptions::Get(TEXT("ShowFrameRate"))>.5f && PC->FrameRateWidget;
	if(ShowFPS && !IsValid(FrameRatePanel))
	{
		FrameRatePanel=NewObject<UWidgetComponent>(PresentationActor,TEXT("VRFrameRate"));
		PresentationActor->AddInstanceComponent(FrameRatePanel);FrameRatePanel->SetupAttachment(Head);
		FrameRatePanel->SetWidgetSpace(EWidgetSpace::World);FrameRatePanel->SetDrawSize(FVector2D(400,64));
		FrameRatePanel->SetWidget(PC->FrameRateWidget);FrameRatePanel->SetBlendMode(EWidgetBlendMode::Transparent);
		FrameRatePanel->SetBackgroundColor(FLinearColor::Transparent);FrameRatePanel->SetTwoSided(true);
		FrameRatePanel->SetWindowFocusable(false);FrameRatePanel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		FrameRatePanel->SetCastShadow(false);FrameRatePanel->SetRedrawTime(.25f);
		FrameRatePanel->SetRelativeLocationAndRotation(FVector(100,-32,24),FRotator(0,180,0));
		FrameRatePanel->SetRelativeScale3D(FVector(.065f));FrameRatePanel->RegisterComponent();
	}
	if(IsValid(FrameRatePanel))
	{
		FrameRatePanel->SetVisibility(ShowFPS);FrameRatePanel->SetComponentTickEnabled(ShowFPS);
	}
	// Only enabled pinned surfaces refresh below. A full name index costs more
	// than these few lookups on Quest; the gameplay binder retains its own index
	// for its much larger batch of UI queries.
	UUserWidget* Widget = nullptr;
	if (PC->LoginWidget && PC->LoginWidget->GetVisibility() != ESlateVisibility::Collapsed
		&& !PC->DatCharSelectBinder && !PC->DatCharGenBinder && !InWorld) Widget = PC->LoginWidget;
	else if (PC->DatCanvasWidget) Widget = PC->DatCanvasWidget;
	else if (PC->GameHUDWidget) Widget = PC->GameHUDWidget;
	const bool Login = Widget && Widget == PC->LoginWidget;
	// Crop the login surface to the card rather than a large, mostly empty desktop.
	// Character selection and gameplay retain their normal canvas dimensions.
	RetailPanel->SetDrawSize(Login ? FVector2D(1100, 900) : FVector2D(1280, 960));
	if (Widget)
	{
		if (Widget->IsInViewport()) Widget->RemoveFromParent();
		if (RetailPanel->GetWidget() != Widget) { DismissTextEntry(); RetailPanel->SetWidget(Widget); PositionPanel(RetailPanel); PC->ApplyInWorldInputMode(); }
	}
	// Do not leave the retail intro canvas composited over both stereo eyes behind the world-space login.
	if (PC->DatCanvasWidget && PC->DatCanvasWidget != Widget && PC->DatCanvasWidget->IsInViewport()) PC->DatCanvasWidget->RemoveFromParent();
	const bool Available = bTracking && !PC->bEnterWorldLoading && !PC->bWorldRevealActive;
	auto PinToHead = [&](UWidgetComponent* Panel, bool Pinned)
	{
		if (Pinned)
		{
			if (Panel->GetAttachParent() != Head.Get()) Panel->AttachToComponent(Head, FAttachmentTransformRules::KeepWorldTransform);
		}
		else if (Panel->GetAttachParent())
		{
			// Body/world anchors are positioned explicitly. Parenting them to the
			// tracking origin first moved them with floor correction/locomotion,
			// then moved them back during this tick. Keep the world pose independent
			// throughout the frame, including camera and controller late updates.
			Panel->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		}
	};
	// Camera descendants participate in HMD late update, so viewport-pinned
	// text remains fixed during head motion between simulation and rendering.
	PinToHead(RetailPanel, InWorld && Settings->MenuAnchorMode == 0 && !bSettingsOpen);
	PinToHead(WristPanel, InWorld && Settings->bPinHotbarToView);
	PinToHead(CompassPanel, Settings->CompassAnchorMode == 0);
	PinToHead(FellowshipPanel,Settings->FellowshipAnchorMode==0);
	PinToHead(VitalsPanel, Settings->VitalsAnchorMode == 0); PinToHead(ChatPanel, Settings->ChatAnchorMode==0 && !bChatDocked);
	auto Show = [](UWidgetComponent* P, bool Visible)
	{
		P->SetVisibility(Visible); P->SetCollisionEnabled(Visible ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
		// Hidden gameplay panels still tick the binder, which handles incoming vendor/trade updates.
	};
	UpdateTextEntryFocus();
	const bool ShowNative = InWorld && Available && bInventoryOpen && !bSettingsOpen && !bUseDesktopMenu;
	const bool ShowMain = Available && Widget && (!InWorld || (bInventoryOpen && bUseDesktopMenu) || bSettingsOpen) && (!bTextKeyboardOpen || UsesPlatformKeyboard() || !InWorld);
	// Gameplay state must continue to process vendor/trade/attack events while
	// menus are closed, but a native-only HUD consumes no desktop render target.
	const bool NeedsRetailDraw = ShowMain || (InWorld && Settings->bShowWristSpellBar && GetCombatMode() == ACECombatMode::Magic);
	if (InWorld && PC->DatCanvasWidget) PC->DatCanvasWidget->TickGameplayState(NeedsRetailDraw);
	RetailPanel->SetVisibility(Available && Widget && NeedsRetailDraw);
	RetailPanel->SetComponentTickEnabled(Available && Widget && NeedsRetailDraw);
	RetailPanel->SetRenderInMainPass(ShowMain);
	RetailPanel->SetCollisionEnabled(ShowMain ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
	if (InWorld && !bSettingsOpen && (bInventoryOpen || bMenuAnchorReady))
	{
		UpdateMenuAnchor(Dt);
		PositionPanel(RetailPanel);
	}
	Show(SettingsPanel, Available && bSettingsOpen);
	Show(KeyboardPanel, !UsesPlatformKeyboard() && Available && bKeyboardOpen);
	const float PanelScale = Login ? FMath::Clamp(Settings->PanelScale, .09f, .10f) : Settings->PanelScale;
	const float KeyboardScale = Login && !bSettingsOpen ? PanelScale * .68f : Settings->PanelScale * .75f;
	KeyboardPanel->SetWorldScale3D(FVector(KeyboardScale));
	const auto* BasePanel = bSettingsOpen ? SettingsPanel.Get() : RetailPanel.Get();
	// Size the gap from both surfaces so the keyboard cannot cover the Login button
	// or be intercepted by the transparent lower portion of the login quad.
	const bool LobbyLayout = !InWorld && !bSettingsOpen;
	const float KeyboardPitch = LobbyLayout ? 15.f : 25.f;
	const float Below = LobbyLayout ? RetailPanel->GetDrawSize().Y * .5f * PanelScale + 6.f + 200.f * KeyboardScale * FMath::Cos(FMath::DegreesToRadians(KeyboardPitch)) : 38.f;
	KeyboardPanel->SetWorldLocation(BasePanel->GetComponentLocation() + BasePanel->GetForwardVector() * (LobbyLayout ? 0.f : 20.f) - FVector(0, 0, Below));
	FRotator KeyboardFacing = BasePanel->GetComponentRotation(); KeyboardFacing.Pitch = KeyboardPitch;
	KeyboardPanel->SetWorldRotation(KeyboardFacing);
	if (bTextKeyboardOpen && !LobbyLayout) KeyboardPanel->SetWorldTransform(TextKeyboardTransform);
	auto RefreshRetail = [&](UACEVRRetailSurface* Surface, UWidgetComponent* Panel, FName Window, bool Enabled)
	{
		Surface->SetSource(RetailPanel, PC->DatCanvasWidget, Window);
		const bool Ready = InWorld && Available && Enabled && (!bTextKeyboardOpen || UsesPlatformKeyboard())
			&& Surface->RefreshSurface();
		if (Ready) Panel->SetDrawSize(Surface->GetSurfaceSize());
		Show(Panel, Available && Enabled && Ready && (!bTextKeyboardOpen || UsesPlatformKeyboard()));
	};
	// Magic stance owns visibility. Looking away must not destroy the wrist surface
	// or invalidate an in-progress tab selection; retain its current placement.
	RefreshRetail(WristRetail, WristPanel, TEXT("RootGameplay_FloatyCombatPanel_Field"),
		Settings->bShowWristSpellBar && GetCombatMode() == ACECombatMode::Magic);
	UpdateNativeHUD(InWorld && Available, Dt);
	// Open menus take visual and pointer priority over the ambient HUD.
	// Keep the meters visible behind the menu without stealing its buttons.
	const FVector WristLocation = Settings->bPinHotbarToView
		? Head->GetComponentTransform().TransformPosition(FVector(80, 0, -24))
		: GetPhysicalGrip(true).GetLocation() + FVector(0, 0, 12.f);
	const FQuat WristRotation = Settings->bPinHotbarToView ? Head->GetComponentQuat() * FRotator(0, 180, 0).Quaternion()
		: (Head->GetComponentLocation() - WristLocation).Rotation().Quaternion();
	const FTransform WristLocal = FTransform(WristRotation, WristLocation).GetRelativeTransform(TrackingOrigin->GetComponentTransform());
	const bool ResetWrist = !bWristPoseReady || FVector::DistSquared(WristLocalPose.GetLocation(), WristLocal.GetLocation()) > FMath::Square(100.f);
	const float Follow = ResetWrist || Settings->bPinHotbarToView || Settings->WristSmoothing <= 0.f ? 1.f
		: 1.f - FMath::Exp(-FMath::Max(0.f, Dt) / Settings->WristSmoothing);
	// Smooth only the user's local hand motion. Pawn travel and stick turns must
	// move the panel and controller ray together, with no world-space trailing.
	WristLocalPose = FTransform(FQuat::Slerp(WristLocalPose.GetRotation(), WristLocal.GetRotation(), Follow).GetNormalized(),
		FMath::Lerp(WristLocalPose.GetLocation(), WristLocal.GetLocation(), Follow));
	const FTransform WristWorld = WristLocalPose * TrackingOrigin->GetComponentTransform();
	WristPanel->SetWorldLocationAndRotation(WristWorld.GetLocation(), WristWorld.GetRotation());
	bWristPoseReady = WristPanel->IsVisible();
	UpdateVitalsAnchor(Dt); UpdateCompassAnchor(Dt); UpdateFellowshipAnchor(Dt); UpdatePanelEdit(Dt);
	const auto FellowFrame=GetFellowshipAnchorTransform();
	FellowshipPanel->SetWorldLocationAndRotation(FellowFrame.TransformPosition(Settings->FellowshipViewOffset),
		FellowFrame.GetRotation()*Settings->FellowshipViewRotation.Quaternion()*FRotator(0,180,0).Quaternion());
	FellowshipPanel->SetWorldScale3D(FVector(Settings->FellowshipScale));
	const FTransform VitalsFrame = GetVitalsAnchorTransform();
	VitalsPanel->SetWorldLocationAndRotation(VitalsFrame.TransformPosition(Settings->VitalsViewOffset),
		VitalsFrame.GetRotation() * Settings->VitalsViewRotation.Quaternion() * FRotator(0, 180, 0).Quaternion());
	const FTransform CompassFrame = GetCompassAnchorTransform();
	CompassPanel->SetWorldLocationAndRotation(CompassFrame.TransformPosition(Settings->CompassViewOffset),
		CompassFrame.GetRotation() * Settings->CompassViewRotation.Quaternion() * FRotator(0, 180, 0).Quaternion());
	CompassPanel->SetWorldScale3D(FVector(Settings->CompassScale));
	VitalsPanel->SetWorldScale3D(FVector(Settings->VitalsScale));
	ChatPanel->SetWorldScale3D(FVector(Settings->ChatScale));
	RetailPanel->SetWorldScale3D(FVector(Login ? PanelScale : Settings->PanelScale));
	UpdateGameplayMenu(ShowNative,Dt);
	UpdateChatPanel(InWorld && Available,Dt);
	UpdatePersonalPanelLayers();
	SettingsPanel->SetWorldScale3D(FVector(Settings->OptionsScale));
	UpdatePanelControls(InWorld && (ShowMain || ShowNative), Available && bSettingsOpen);
	WristPanel->SetRelativeScale3D(FVector(Settings->WristScale));
	UpdatePointerVisuals(Available, !InWorld || bInventoryOpen || bSettingsOpen || bKeyboardOpen || bChatOpen);
	UpdateInteractionFeedback(Available && InWorld && !bKeyboardOpen && !bSettingsOpen);
}

bool UACEVRComponent::IsPointerNearPanel(bool Left, FVector* Impact) const
{
	const auto* Aim = Left ? LeftAim.Get() : RightAim.Get();
	if (!Aim || !Aim->IsTracked()) return false;
	const FVector Origin = Aim->GetComponentLocation(), Direction = Aim->GetForwardVector();
	for (auto* Panel : {ChatPanel.Get(), WristPanel.Get(), RetailPanel.Get(), GameplayMenuPanel.Get(), MenuInspectionPanel.Get(), SettingsPanel.Get(), MenuControlsPanel.Get(), OptionsControlsPanel.Get()})
	{
		if (!ACEVRPointerVisuals::IsInteractivePanel(Panel)) continue;
		const FVector Normal = Panel->GetForwardVector();
		const float Denominator = FVector::DotProduct(Direction, Normal);
		if (FMath::Abs(Denominator) < .15f) continue;
		const float Distance = FVector::DotProduct(Panel->GetComponentLocation() - Origin, Normal) / Denominator;
		if (Distance <= 0.f || Distance > 250.f) continue;
		const FVector Point = Origin + Direction * Distance;
		if (const auto* Canvas = Cast<UACEUICanvasWidget>(Panel->GetWidget()); Canvas && Canvas->IsGameplayCanvas() && Canvas->GetManager())
		{
			FVector2D Hit; Panel->GetLocalHitLocation(Point, Hit);
			const auto Layout = Canvas->ViewportToLayout(Hit);
			if (!Canvas->GetManager()->FindWindowAtCanvas(FMath::FloorToInt(Layout.X), FMath::FloorToInt(Layout.Y))) continue;
		}
		const FVector Local = Panel->GetComponentTransform().InverseTransformPosition(Point);
		const FVector2D Size = Panel->GetDrawSize();
		// A small border protects imprecise clicks without creating a large,
		// invisible no-cast zone around the wrist. Controller aim offsets differ
		// across devices; the former 8cm padding more than tripled its height.
		const FVector Scale = Panel->GetComponentScale().GetAbs();
		if (FMath::Abs(Local.Y) <= Size.X * .5f + 1.f / FMath::Max(.01f, float(Scale.Y))
			&& FMath::Abs(Local.Z) <= Size.Y * .5f + 1.f / FMath::Max(.01f, float(Scale.Z)))
		{
			if (Impact) *Impact = Point;
			return true;
		}
	}
	return false;
}

void UACEVRComponent::UpdatePointerVisuals(bool Available, bool MenuVisible)
{
	for (int32 I = 0; I < 2; ++I)
	{
		auto* Aim = I == 0 ? LeftAim.Get() : RightAim.Get();
		auto* Pointer = I == 0 ? LeftPointer.Get() : RightPointer.Get();
		// Both hands share one Slate user so either can type in the focused field.
		// Deactivating one pointer unregisters that shared user and breaks the other hand.
		Pointer->bEnableHitTesting = Available && Aim->IsTracked();
		auto* Beam = PointerBeams[I].Get();
		const bool OverUI = Available && Aim->IsTracked() && Pointer->IsOverHitTestVisibleWidget();
		FVector PanelImpact;
		const bool NearUI = IsPointerNearPanel(I == 0, &PanelImpact);
		const bool WorldPointing = Client && Client->GetSessionState() == EACESessionState::InWorld
			&& GetCombatMode() == ACECombatMode::NonCombat && I == (Settings->bLeftHanded ? 0 : 1);
		const bool SpellPointing = Client && Client->GetSessionState() == EACESessionState::InWorld
			&& GetCombatMode() == ACECombatMode::Magic && I == (Settings->bLeftHanded ? 0 : 1);
		const bool CrossbowPointing = Client && Client->GetSessionState() == EACESessionState::InWorld
			&& GetCombatMode() == ACECombatMode::Missile && MissileStyle() == 0x20
			&& I == (Settings->bLeftHanded ? 1 : 0) && !IsInputBlocked();
		const bool ShowPointer = Available && Aim->IsTracked() && (MenuVisible || OverUI || NearUI || WorldPointing || SpellPointing || CrossbowPointing);
		Beam->SetVisibility(ShowPointer);
		auto* Tip = PointerTips[I].Get(); Tip->SetVisibility(ShowPointer);
		if (ShowPointer)
		{
			const int32 Mode=SpellPointing?1:CrossbowPointing?2:0;
			const bool Overlay = Pointer->GetHoveredWidgetComponent() || NearUI;
			auto* Material = ACEVRPointerVisuals::GetMaterial(Overlay);
			if (Beam->GetMaterial(0) != Material) Beam->SetMaterial(0, Material);
			if (Tip->GetMaterial(0) != Material) Tip->SetMaterial(0, Material);
			const int32 Layer = Overlay ? ACEVRPointerVisuals::OverlayLayer : 0;
			if (Beam->TranslucencySortPriority != Layer) Beam->SetTranslucentSortPriority(Layer);
			if (Tip->TranslucencySortPriority != Layer + 1) Tip->SetTranslucentSortPriority(Layer + 1);
			if(PointerBeamModes[I]!=Mode)
			{
				// A bright core and feathered outer strips give a small halo without
				// bloom, dynamic lights, particles, or per-frame geometry rebuilds.
				TArray<FVector> Vertices,Normals;TArray<int32> Indices;TArray<FVector2D> UV;
				TArray<FLinearColor> Colors;TArray<FProcMeshTangent> Tangents;
				const FLinearColor Color=ACEVRPointerVisuals::ModeColor(Mode);
				for(int32 Column=0;Column<4;++Column)
				{
					const float X = Column == 0 ? -150.f : Column == 1 ? -50.f : Column == 2 ? 50.f : 150.f;
					FLinearColor EdgeColor = Color; EdgeColor.A = (Column == 0 || Column == 3) ? 0.f : 1.f;
					for(float Z:{-50.f,50.f}){Vertices.Add(FVector(X,0,Z));Normals.Add(FVector::RightVector);UV.Add(FVector2D::ZeroVector);Colors.Add(EdgeColor);}
					if(Column<3){const int32 A=Column*2,B=A+2;Indices.Append({A,B,A+1,B,B+1,A+1});}
				}
				Beam->CreateMeshSection_LinearColor(0,Vertices,Indices,Normals,UV,Colors,Tangents,false);
				PointerBeamModes[I]=Mode;
			}
			FVector A = Aim->GetComponentLocation();
			FVector B = Pointer->GetHoveredWidgetComponent() ? FVector(Pointer->GetLastHitResult().ImpactPoint) : A + Aim->GetForwardVector() * 200.f;
			if (NearUI && !OverUI) B = PanelImpact;
			if (WorldPointing && !OverUI && !NearUI && !IsInputBlocked())
			{
				FVector Direction=Aim->GetForwardVector();
				if (EquippedWeapon().ItemType & ACEItemType::Caster) GetSpellAim(A,Direction);
				B=A+Direction*10000.f;
				ACEVisibleObjectPick::Trace(*GetWorld(),A,B,Cast<APawn>(GetOwner()),&B,false);
			}
			if ((SpellPointing || CrossbowPointing) && !OverUI && !NearUI && !IsInputBlocked())
			{
				FVector Direction, CastOrigin;
				if (SpellPointing) GetSpellAim(CastOrigin, Direction);
				else { CastOrigin = GetCrossbowMuzzle(); Direction = BowAim()->GetForwardVector(); }
				A = CastOrigin;
				B = CastOrigin + Direction * 10000.f;
				ACEVisibleObjectPick::Trace(*GetWorld(), CastOrigin, B, Cast<APawn>(GetOwner()), &B, false);
			}
			const FVector Along = (B - A).GetSafeNormal();
			const FVector Side = FVector::CrossProduct(Along, Head->GetComponentLocation() - A).GetSafeNormal(SMALL_NUMBER, Aim->GetRightVector());
			Beam->SetWorldLocation((A + B) * .5f); Beam->SetWorldRotation(FRotationMatrix::MakeFromZX(Along, Side).Rotator());
			const float Width=Mode==0?.0025f:.005f;
			Beam->SetWorldScale3D(FVector(Width,Width,FVector::Dist(A,B)/100.f));
			const bool Pressed = I == 0 ? bLeftPointerPressed : bRightPointerPressed;
			const float AimTipScale = FMath::Clamp(float(FVector::Dist(A, B)) * .00003f, .012f, .12f);
			Tip->SetWorldLocation(B); Tip->SetWorldScale3D(FVector(Pressed ? .027f : OverUI ? .02f : (SpellPointing || CrossbowPointing) ? AimTipScale : .012f));
		}
	}
}

void UACEVRComponent::ToggleInventory()
{
	if (bSpellWheelOpen) { CloseSpellWheel(); return; }
	if (!bActive || !bTracking || !Client || Client->GetSessionState() != EACESessionState::InWorld) return;
	if (bTextKeyboardOpen) { CancelGestures(); DismissTextEntry(); UpdatePanels(); return; }
	CancelGestures(); bInventoryOpen = !bInventoryOpen; bSettingsOpen = false;
	if(!bInventoryOpen && GameplayMenu)GameplayMenu->UseSource=0;
	if (bInventoryOpen)
	{
		// Like chat, capture the current view on each open, then honor the anchor mode.
		bMenuAnchorReady=false;
		FrontPanel="Menu";
		bUseDesktopMenu=false;
		if(GameplayMenu)GameplayMenu->OpenPage("Inventory");
		PC->EndUseApproach(); if (auto Session = Client->GetSession()) Session->SendCancelAttack();
		if (PC->DatGameplayBinder) PC->DatGameplayBinder->OpenGameplayPanel(TEXT("InventoryPanel_Field"));
		PositionPanel(RetailPanel);
	}
	UpdatePanels();
}

void UACEVRComponent::RevealSalvagePanel()
{
	if(!bActive)return;
	bUseDesktopMenu=false;PendingGameplayPage="Salvage";
	if(GameplayMenu){GameplayMenu->OpenPage(PendingGameplayPage);PendingGameplayPage=NAME_None;}
	if(bInventoryOpen && !bSettingsOpen)return;
	DismissTextEntry();CancelGestures();bSettingsOpen=false;bInventoryOpen=true;bMenuAnchorReady=false;PositionPanel(RetailPanel);
}

void UACEVRComponent::RevealRetailDialog(int32 Guid)
{
	if (!bActive || Guid == 0) return;
	FrontPanel="Menu";
	ClearConversationSelection(Guid);
	bUseDesktopMenu=false;
	PendingGameplayPage=Client->GetOpenVendorGuid()?FName("Vendor"):Client->GetTradePartnerGuid()?FName("Trade"):FName("Loot");
	if(GameplayMenu){GameplayMenu->OpenPage(PendingGameplayPage);PendingGameplayPage=NAME_None;}
	// Inventory/vendor packets also refresh an already open dialog after a
	// purchase. Only opening the surface may capture a new viewing position.
	if (bInventoryOpen && !bSettingsOpen) return;
	DismissTextEntry();
	CancelGestures(); bSettingsOpen = false; bInventoryOpen = true;
	bMenuAnchorReady=false;
	PositionPanel(RetailPanel);
}

void UACEVRComponent::TradeChanged(int32 EventType)
{
	if (Client && (EventType==ACEGameEvent::OpenTrade || EventType==ACEGameEvent::RegisterTrade))
		RevealRetailDialog(Client->GetTradePartnerGuid());
}

void UACEVRComponent::ToggleSettings()
{
	if (!bActive || !bTracking) return;
	DismissTextEntry();
	CancelGestures(); bSettingsOpen = !bSettingsOpen; bInventoryOpen = false;
	PC->EndUseApproach(); if (Client) if (auto Session = Client->GetSession()) Session->SendCancelAttack();
	if (bSettingsOpen)
	{
		FrontPanel="Options";
		SettingsLayoutFrame = FTransform(FRotator(0,Head->GetComponentRotation().Yaw,0), Head->GetComponentLocation());
	}
	PositionPanel(RetailPanel); PositionPanel(SettingsPanel);
	UpdatePanels();
}

bool UACEVRComponent::RevealLearnedSpellInMenu(int32 SpellId)
{
	if (!bActive || !bInventoryOpen || bSettingsOpen || bUseDesktopMenu || !GameplayMenu
		|| GameplayMenu->GetPage() != "Spellbook") return false;
	// SpellAdded refreshes the book already being used. Switching surfaces here
	// hides the controller's live controls underneath an unrelated desktop panel.
	GameplayMenu->PendingLearnedSpellId = SpellId;
	GameplayMenu->bDirty = true;
	return true;
}

void UACEVRComponent::OpenRetailPanel(FName Name)
{
	if (!bInventoryOpen || bSettingsOpen) bMenuAnchorReady=false;
	DismissTextEntry();
	bUseDesktopMenu=true;
	CancelGestures(); bSettingsOpen = false; bInventoryOpen = true;
	// A server dialog already exists on the retail canvas. Reveal it without
	// switching the underlying inventory/spellbook page.
	if (!Name.IsNone() && PC->DatGameplayBinder) PC->DatGameplayBinder->OpenGameplayPanel(Name.ToString());
	PositionPanel(RetailPanel); UpdatePanels();
}

TArray<int32> UACEVRComponent::SpellSlots() const
{
	if (!Client) return {};
	TArray<int32> Result;
	for (int32 Spell : Client->GetSpellBar(Client->GetActiveSpellBar())) if (Spell != 0) Result.AddUnique(Spell);
	if (Result.Num() == 0) Result = Client->GetKnownSpells();
	const auto Weapon = EquippedWeapon(); if (Weapon.SpellDID != 0) Result.AddUnique(Weapon.SpellDID);
	return Result;
}

void UACEVRComponent::CycleSpell(int32 Direction)
{
	if (!bActive || !bTracking || GetCombatMode() != ACECombatMode::Magic) return;
	const auto Spells = SpellSlots(); if (Spells.Num() == 0) return;
	int32 Index = Spells.IndexOfByKey(SelectedSpell);
	if (Index == INDEX_NONE) Index = Direction > 0 ? -1 : 0;
	Index = (Index + Direction + Spells.Num()) % Spells.Num(); SelectSpell(Spells[Index]); SpellPage = Index / 8;
}

void UACEVRComponent::SelectWristSlot(int32 Slot)
{
	const auto Spells = SpellSlots(); const int32 Index = SpellPage * 8 + Slot;
	if (Spells.IsValidIndex(Index)) SelectSpell(Spells[Index]);
}

bool UACEVRComponent::SelectSpell(int32 Spell)
{
	if (!bActive || !Client || Client->GetSessionState() != EACESessionState::InWorld || Spell == 0) return false;
	const auto Spells = SpellSlots();
	if (!Spells.Contains(Spell) && !Client->GetKnownSpells().Contains(Spell)) return false;
	SelectedSpell = Spell;
	if (PC->DatGameplayBinder) PC->DatGameplayBinder->SelectVRSpell(Spell);
	const int32 Index = Spells.IndexOfByKey(Spell);
	if (Index != INDEX_NONE) SpellPage = Index / 8;
	int32 AutomaticTarget = 0;
	const bool bAutomatic = Client->ResolveSpellCastTarget(Spell, 0, AutomaticTarget);
	SetCastFeedback(bAutomatic && AutomaticTarget == Client->GetPlayerGuid()
		? TEXT("Self spell selected. X closes menus. Pull trigger to cast on yourself.")
		: bAutomatic ? TEXT("Selected. X closes menus. Pull trigger to cast.")
		: TEXT("Selected. X closes menus. Aim and pull trigger to cast."));
	Pulse(false);
	return true;
}

void UACEVRComponent::RestoreSpellBarSelection(int32 Spell)
{
	if (!bActive || !Client) return;
	SelectedSpell = Spell;
	const int32 Index = SpellSlots().Find(Spell);
	SpellPage = Index >= 0 ? Index / 8 : 0;
}

bool UACEVRComponent::IsWristSlotSelected(int32 Slot) const
{
	const auto Spells = SpellSlots(); const int32 Index = SpellPage * 8 + Slot;
	return Spells.IsValidIndex(Index) && Spells[Index] == SelectedSpell;
}

void UACEVRComponent::SetCastFeedback(const FString& Text)
{
	CastFeedback = Text; CastFeedbackUntil = GetWorld()->GetTimeSeconds() + 8.f;
	UE_LOG(LogTemp, Log, TEXT("ACE VR combat: %s"), *Text);
}

void UACEVRComponent::CombatMessage(const FString& Text, const FString& Sender, int32 ChatType)
{
	if (Sender.IsEmpty() && (ChatType == ACEChatMessageType::Magic ||
		(ChatType == ACEChatMessageType::CombatSelf && Text.Contains(TEXT(" points of periodic ")))))
	{
		FString Target; int32 Damage = 0; bool Critical = false;
		if (ACECombatChat::ParseOutgoingSpellDamage(Text, Target, Damage, Critical))
			CombatFeedback(Target, Damage, false, Critical);
	}
	if (Sender.IsEmpty() && ACEChatMessageType::IsTransientNotice(ChatType))
		ShowWorldNotice(Text, 0, 3, FLinearColor(FColor(255,225,75)));
	if (Sender.IsEmpty() && ChatType == ACEChatMessageType::Broadcast && Text.Contains(TEXT(" gives you ")))
		ShowWorldNotice(Text, 0, 1, FLinearColor(1.f,.8f,.35f));
	// Cast errors arrive through the existing system-chat channel. Keep them in
	// the headset even while the large retail chat window is closed.
	if (Sender.IsEmpty() && GetWorld()->GetTimeSeconds() - LastCast < 8.f)
		SetCastFeedback(Text.Left(240));
}

FString UACEVRComponent::GetCastFeedback() const
{
	if (GetWorld()->GetTimeSeconds() < CastFeedbackUntil) return CastFeedback;
	if (IsMenuOpen()) return TEXT("Select a spell, then press X to close menus and cast.");
	return TEXT("A selects / Aim into the world / Trigger casts");
}

void UACEVRComponent::CycleSpellBar(int32 Direction)
{
	if (!Client) return;
	const int32 Tab = (Client->GetActiveSpellBar() + Direction + 8) % 8;
	if (PC && PC->DatGameplayBinder) PC->DatGameplayBinder->SetCombatSpellBar(Tab);
	else Client->SetActiveSpellBar(Tab);
}

FString UACEVRComponent::GetWristSlotText(int32 Slot) const
{
	const auto Spells = SpellSlots(); const int32 Index = SpellPage * 8 + Slot;
	if (!Spells.IsValidIndex(Index)) return TEXT("--");
	FString Name; uint32 Icon;
	if (auto* Dat = GetWorld()->GetGameInstance()->GetSubsystem<UACEDatSubsystem>()) Dat->TryGetSpellInfo(Spells[Index], Name, Icon);
	if (Name.IsEmpty()) Name = FString::Printf(TEXT("Spell %d"), Spells[Index]);
	return FString::Printf(TEXT("%s %s"), SelectedSpell == Spells[Index] ? TEXT(">") : TEXT(" "), *Name);
}

FString UACEVRComponent::GetSelectionText() const
{
	FString Name; uint32 Icon;
	if (auto* Dat = GetWorld()->GetGameInstance()->GetSubsystem<UACEDatSubsystem>()) Dat->TryGetSpellInfo(SelectedSpell, Name, Icon);
	return Name.IsEmpty() ? TEXT("Select a spell with A") : Name;
}

FString UACEVRComponent::GetStatusText() const
{
	if (!Client) return TEXT("Connecting...");
	const auto V = Client->GetPlayerVitals();
	const bool Supported = Client->GetSession() && Client->GetSession()->SupportsVRCombat();
	const TCHAR* Mode = GetCombatMode() == 8 ? TEXT("Magic") : GetCombatMode() == 4 ? TEXT("Missile") : GetCombatMode() == 2 ? TEXT("Melee") : TEXT("Peace");
	return FString::Printf(TEXT("%s  |  Health %d/%d  Mana %d/%d\n%s"), Mode, V.Health, V.MaxHealth, V.Mana, V.MaxMana,
		Supported ? TEXT("VR combat ready") : TEXT("Standard server: targeted spell casting ready"));
}

void UACEVRComponent::ChangeSetting(FName Setting)
{
	if (Setting=="FellowshipAnchor")
	{
		EndPanelEdit();Settings->FellowshipAnchorMode=(Settings->FellowshipAnchorMode+1)%3;
		bFellowshipAnchorReady=false;Settings->Persist();UpdatePanels();return;
	}
	if (Setting == TEXT("CompassAnchor"))
	{
		EndPanelEdit(); Settings->CompassAnchorMode = (Settings->CompassAnchorMode + 1) % 3;
		bCompassAnchorReady = false; Settings->Persist(); UpdatePanels(); return;
	}
	if (Setting == TEXT("VitalsAnchor"))
	{
		EndPanelEdit(); EndVitalsDrag(); Settings->VitalsAnchorMode = (Settings->VitalsAnchorMode + 1) % 3;
		bVitalsAnchorReady = false; Settings->Persist(); UpdatePanels(); return;
	}
	CancelGestures();
	if (Setting == TEXT("Turn")) Settings->bSnapTurn = !Settings->bSnapTurn;
	else if (Setting == TEXT("Run")) Settings->bRun = !Settings->bRun;
	else if (Setting == TEXT("Angle")) Settings->SnapDegrees = Settings->SnapDegrees >= 60.f ? 15.f : Settings->SnapDegrees + 15.f;
	else if (Setting == TEXT("Hand")) Settings->bLeftHanded = !Settings->bLeftHanded;
	else if (Setting == TEXT("Movement")) Settings->MovementDirection = (Settings->MovementDirection + 1) % 3;
	else if (Setting == TEXT("Seated")) { Settings->bSeated = !Settings->bSeated; ResetTrackingOrigin(); }
	else if (Setting == TEXT("CharacterHeight")) { Settings->bMatchCharacterHeight = !Settings->bMatchCharacterHeight; ResetTrackingOrigin(); }
	else if (Setting == TEXT("Body")) Settings->bShowBody = !Settings->bShowBody;
	else if (Setting == TEXT("PinMenu")) { Settings->MenuAnchorMode = (Settings->MenuAnchorMode + 1) % 3; bMenuAnchorReady = false; PositionPanel(RetailPanel); }
	else if (Setting == TEXT("PinHotbar")) { Settings->bPinHotbarToView = !Settings->bPinHotbarToView; bWristPoseReady = false; }
	else if (Setting == TEXT("ShowWrist")) Settings->bShowWristSpellBar = !Settings->bShowWristSpellBar;
	else if (Setting == TEXT("Compass")) Settings->bShowCompass = !Settings->bShowCompass;
	else if (Setting == "Fellowship") Settings->bShowFellowship=!Settings->bShowFellowship;
	else if (Setting == "FellowshipLock") Settings->bFellowshipLocked=!Settings->bFellowshipLocked;
	else if (Setting == TEXT("PinVitals")) Settings->bPinVitalsToView = !Settings->bPinVitalsToView;
	else if (Setting == TEXT("CompassLock")) Settings->bCompassLocked = !Settings->bCompassLocked;
	else if (Setting == TEXT("MenuLock")) Settings->bMenuLocked = !Settings->bMenuLocked;
	else if (Setting == TEXT("OptionsLock")) Settings->bOptionsLocked = !Settings->bOptionsLocked;
	else if (Setting == TEXT("VitalsLock")) Settings->bVitalsLocked = !Settings->bVitalsLocked;
	else if (Setting == TEXT("PinChat")) {Settings->ChatAnchorMode=(Settings->ChatAnchorMode+1)%3;bChatAnchorReady=false;bChatDocked=false;}
	else if (Setting == TEXT("Haptics")) Settings->bHaptics = !Settings->bHaptics;
	else if (Setting == TEXT("Panel")) Settings->PanelScale = Settings->PanelScale >= .14f ? .08f : Settings->PanelScale + .01f;
	else if (Setting == TEXT("Distance")) { Settings->PanelDistance = Settings->PanelDistance >= 130.f ? 70.f : Settings->PanelDistance + 10.f; Settings->MenuViewOffset.X=Settings->PanelDistance; Settings->PanelLayouts.Reset(); PositionPanel(RetailPanel); PositionPanel(SettingsPanel); }
	else if (Setting == TEXT("Wrist")) Settings->WristScale = Settings->WristScale >= .085f ? .03f : Settings->WristScale + .005f;
	else if (Setting == TEXT("Draw")) Settings->BowFullDraw = Settings->BowFullDraw >= 80.f ? 30.f : Settings->BowFullDraw + 10.f;
	else if (Setting == TEXT("Render"))
	{
		Settings->RenderScale = Settings->RenderScale >= 120.f ? 70.f : Settings->RenderScale + 10.f;
		Settings->ApplyRenderScale();
	}
	else if (Setting == TEXT("EdgeSmoothing"))
	{
		Settings->MSAASamples = Settings->MSAASamples == 4 ? 2 : 4;
		Settings->ApplyEdgeSmoothing();
	}
	Settings->Persist();
}

void UACEVRComponent::ToggleKeyboard()
{
	if (UsesPlatformKeyboard()) return;
	if (bTextKeyboardOpen) DismissTextEntry();
	else bKeyboardOpen = !bKeyboardOpen;
}

float UACEVRComponent::GetSliderSetting(FName Setting) const
{
	if (Setting == "TurnSpeed") return (Settings->SmoothTurnDegreesPerSecond - 15.f) / 345.f;
	if (Setting == "HandPitch") return (Settings->HandPitch + 30.f) / 60.f;
	if (Setting == "Draw") return (Settings->BowFullDraw - 30.f) / 60.f;
	if (Setting == "BowAnchor") return Settings->BowAnchorOffset / 20.f;
	if (Setting == "Height") return (Settings->EyeHeightOffset + 30.f) / 60.f;
	if (Setting == "Panel") return (Settings->PanelScale - .05f) / .1f;
	if (Setting == "Distance") return (Settings->PanelDistance - 70.f) / 110.f;
	if (Setting == "MenuRecenter") return (Settings->MenuRecenterDegrees - 30.f) / 90.f;
	if (Setting == "Wrist") return (Settings->WristScale - .03f) / .055f;
	if (Setting == "CompassSize") return (Settings->CompassScale - .04f) / .11f;
	if (Setting == "OptionsSize") return (Settings->OptionsScale - .06f) / .09f;
	if (Setting == "Vitals") return (Settings->VitalsScale - .05f) / .13f;
	if (Setting == "Chat") return (Settings->ChatScale - .04f) / .11f;
	if (Setting == "ForwardAssist") return Settings->ForwardAssistDegrees / 20.f;
	if (Setting == "Speed") return (Settings->MovementScale - .25f) / .75f;
	return Settings->WristSmoothing / .2f;
}

void UACEVRComponent::SetSliderSetting(FName Setting, float Value)
{
	Value = FMath::Clamp(Value, 0.f, 1.f);
	if (Setting == "TurnSpeed") Settings->SmoothTurnDegreesPerSecond = 15.f + Value * 345.f;
	else if (Setting == "HandPitch") Settings->HandPitch = Value * 60.f - 30.f;
	else if (Setting == "Draw") Settings->BowFullDraw = 30.f + Value * 60.f;
	else if (Setting == "BowAnchor") Settings->BowAnchorOffset = Value * 20.f;
	else if (Setting == "Panel") { Settings->PanelScale = .05f + Value * .1f; if (bSettingsOpen) PositionPanel(RetailPanel); }
	else if (Setting == "Distance") { Settings->PanelDistance = 70.f + Value * 110.f; Settings->MenuViewOffset.X=Settings->PanelDistance; Settings->PanelLayouts.Reset(); PositionPanel(RetailPanel); PositionPanel(SettingsPanel); }
	else if (Setting == "MenuRecenter")
	{
		Settings->MenuRecenterDegrees = 30.f + Value * 90.f;
		// Re-evaluate the start threshold without moving the saved anchor/layout.
		bMenuAnchorTurning = false;
	}
	else if (Setting == "Wrist") Settings->WristScale = .03f + Value * .055f;
	else if (Setting == "CompassSize") Settings->CompassScale = .04f + Value * .11f;
	else if (Setting == "OptionsSize") Settings->OptionsScale = .06f + Value * .09f;
	else if (Setting == "Vitals") Settings->VitalsScale = .05f + Value * .13f;
	else if (Setting == "Chat") Settings->ChatScale = .04f + Value * .11f;
	else if (Setting == "ForwardAssist") Settings->ForwardAssistDegrees = Value * 20.f;
	else if (Setting == "Speed") Settings->MovementScale = .25f + Value * .75f;
	else if (Setting == "Stability") Settings->WristSmoothing = Value * .2f;
	else if (Setting == "Height")
	{
		const float Offset = Value * 60.f - 30.f;
		const float Delta = Offset - Settings->EyeHeightOffset;
		Settings->EyeHeightOffset = Offset; RecenterHeight += Delta; AvatarEyeHeight += Delta;
		// Keep a held slider captured; a full recenter would cancel its drag.
		if (TrackingOrigin) TrackingOrigin->AddWorldOffset(FVector(0, 0, Delta));
	}
	Settings->Sanitize();
}
void UACEVRComponent::TypeText(const FString& Text) { if (bTracking) RightPointer->SendKeyChar(Text); }
void UACEVRComponent::TypeKey(FKey Key)
{
	if (!bTracking) return;
	RightPointer->PressAndReleaseKey(Key);
	// Retail text entries clear focus on commit. Close the keyboard as well,
	// rather than leaving visible keys with nowhere to send their characters.
	if (Key == EKeys::Enter && bTextKeyboardOpen) { DismissTextEntry(); UpdatePanels(); }
}
