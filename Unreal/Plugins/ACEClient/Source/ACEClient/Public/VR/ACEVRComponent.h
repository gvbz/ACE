#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VR/ACEVRMath.h"
#include "VR/ACEVRTriggerState.h"
#include "VR/ACEVRHandCollision.h"
#include "ACETypes.h"
#include "ACEVRComponent.generated.h"

class UCameraComponent;
class UMotionControllerComponent;
class UWidgetComponent;
class UWidget;
class UWidgetInteractionComponent;
class UStaticMeshComponent;
class UACEVRWidget;
class UACEVRSettings;
class UACEVRRetailSurface;
class UACEClientSubsystem;
class AACEPlayerController;
class AACEWorldEntityActor;
class UACEVRMenu;
class UACEVRChat;
class UACECharacterAppearanceComponent;

/** OpenXR rig layered over ACE's existing collision, prediction and authenticated session. */
UCLASS(ClassGroup = ACE, meta = (BlueprintSpawnableComponent))
class ACECLIENT_API UACEVRComponent : public UActorComponent
{
	GENERATED_BODY()
	friend class FACEVRRigTest;
	friend class FACEVRMenuRecenterTest;
	friend class FACEInteractionEffectsTest;
	friend class FACEVRInteriorTest;
	friend class FACEEntranceTransitionTest;
	friend class FACERunSpeedParityTest;
	friend class FACEVRWallContactTest;
	friend class FACEFortTethStairsTest;
	friend class FACEBuildingPassageTest;
	friend class FACEAcademyCornerTest;
	friend class FACEVRStairCeilingTest;
	friend class FACELedgeSafetyTest;
	friend class FACEMansionEdgesTest;
	friend class FACEAvatarMotionTest;
	friend class AACEPlayerController;
	friend class UACEVRMenu;
	friend class UACEVRChat;
	friend class FACEVRChatTest;
	friend class FACERetailWorldEntryTest;
public:
	UACEVRComponent();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void TickComponent(float Dt, ELevelTick TickType, FActorComponentTickFunction* ThisTick) override;
	bool IsActive() const { return bActive; }
	bool HasTracking() const { return bTracking; }
	bool IsJumpHeld() const { return bJumpHeld; }
	bool IsMenuOpen() const { return bInventoryOpen || bSettingsOpen; }
	bool IsLoginPanelReady() const;
	bool IsInputBlocked() const;
	bool IsMovementBlocked() const;
	void GetMovement(float& Forward, float& Right, bool& Running) const;
	void PrepareMovement(float Dt);
	FVector GetBodyForward() const;
	FVector GetRoomScaleDelta() const;
	void CompensateRoomScale(const FVector& ActualDelta);
	void ResetTrackingOrigin();
	void ToggleInventory();
	void ToggleChat();
	void FocusPanelSurface(UWidgetComponent* Panel);
	void InventoryPressed();
	void InventoryReleased();
	void UpdateInventoryHold(float Dt);
	bool TryDropInventoryItem(int32 Item, int32 SplitAmount = 0);
	void ToggleSettings();
	void ToggleSpellWheel();
	void ToggleCombat();
	void SetCombat(int32 Mode);
	void OpenRetailPanel(FName Name);
	void OpenPluginManager();
	void RevealSalvagePanel();
	bool RevealLearnedSpellInMenu(int32 SpellId);
	void CycleSpell(int32 Direction);
	void SelectWristSlot(int32 Slot);
	bool SelectSpell(int32 Spell);
	/** Restore a retail tab cursor without casting, stance changes, feedback, or recursion. */
	void RestoreSpellBarSelection(int32 Spell);
	bool IsWristSlotSelected(int32 Slot) const;
	FString GetCastFeedback() const;
	void CycleSpellBar(int32 Direction);
	FString GetWristSlotText(int32 Slot) const;
	FString GetStatusText() const;
	FString GetSelectionText() const;
	int32 GetCombatMode() const;
	bool HasMeleeRecovery() const;
	float GetMeleeRecoveryRemaining() const;
	float GetMeleeRecoveryProgress() const;
	bool HasCombatTimer() const;
	FString GetCombatTimerText() const;
	float GetCombatTimerProgress() const;
	float GetDrawFraction() const { return BowFraction; }
	void ChangeSetting(FName Setting);
	void SetSliderSetting(FName Setting, float Value);
	float GetSliderSetting(FName Setting) const;
	void ToggleKeyboard();
	void FocusTextEntry(UWidget* Entry);
	void DismissTextEntry();
	FString GetTextInputPreview() const;
	void ToggleVitalsLock();
	bool IsPanelLocked(FName Panel) const;
	bool ShouldShowPanelControls(FName Panel) const;
	void TogglePanelLock(FName Panel);
	bool IsRetailLayoutLocked() const;
	void ToggleRetailLayoutLock();
	void BeginPanelEdit(FName Panel, bool bLeft, bool bResize);
	void ConfigureButton(FName Input,FName Action);
	void ResetButtonBindings();
	void RouteControllerButton(FName Input,bool bPressed);
	void DispatchControllerAction(FName Action,bool bPressed);
	TMap<FName,FName> HeldControllerActions;
	void EndPanelEdit(bool bSave = true, int32 Pointer = INDEX_NONE);
	bool ShouldShowVitalsControls() const;
	void BeginVitalsDrag(bool bLeft);
	void EndVitalsDrag(bool bSave = true, int32 Pointer = INDEX_NONE);
	void TypeText(const FString& Text);
	void TypeKey(FKey Key);
	UACEVRSettings* GetSettings() const { return Settings; }
	USceneComponent* GetHeldAnchor(int32 ParentLocation, bool TwoHanded = false) const;
	bool UpdateMissileAttachment(AACEWorldEntityActor* Actor);
	UPROPERTY(Transient) TObjectPtr<USceneComponent> TrackingOrigin;
	UPROPERTY(Transient) TObjectPtr<UCameraComponent> Head;
	UPROPERTY(Transient) TObjectPtr<UMotionControllerComponent> LeftGrip;
	UPROPERTY(Transient) TObjectPtr<UMotionControllerComponent> RightGrip;
	UPROPERTY(Transient) TObjectPtr<UMotionControllerComponent> LeftAim;
	UPROPERTY(Transient) TObjectPtr<UMotionControllerComponent> RightAim;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> RetailPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> SettingsPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> WristPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> KeyboardPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> GameplayMenuPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> MenuInspectionPanel;
	UPROPERTY(Transient) TObjectPtr<UACEVRMenu> GameplayMenu;
	bool bUseDesktopMenu = false;

private:
	void UpdateChatPanel(bool Available, float Dt);
	void UpdatePersonalPanelLayers();
	UPROPERTY(Transient) TObjectPtr<UACEVRChat> ChatWidget;
	bool bChatOpen = false, bChatDocked = false;
	int32 ChatVendorGuid = 0;
	FName FrontPanel = "Menu";
	FTransform ChatAnchorFrame;
	FTransform MenuAnchorFrame;
	FVector MenuOwnerLocation = FVector::ZeroVector;
	bool bMenuAnchorReady = false, bMenuAnchorTurning = false;
	FTransform GetMenuAnchorTransform() const;
	void UpdateMenuAnchor(float Dt);
	FVector ChatOwnerLocation = FVector::ZeroVector;
	bool bChatAnchorReady = false, bChatAnchorTurning = false;
	FTransform GetChatAnchorTransform() const;
	void UpdateChatAnchor(float Dt);
	void UpdateGameplayMenu(bool Visible, float Dt);
	FName PendingGameplayPage;
	UPROPERTY(Transient) TObjectPtr<AActor> MenuMirrorActor;
	UPROPERTY(Transient) TObjectPtr<UACECharacterAppearanceComponent> MenuMirrorAppearance;
	uint64 MenuMirrorRevision = 0;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> MenuDragPanel;
	int32 MenuDragGuid=0;
	FVector2D MenuPointerPositions[2] = {FVector2D(-1,-1),FVector2D(-1,-1)};
	uint8 MenuPointerState = 0;
	FVector2D InspectionPointerPositions[2] = {FVector2D(-1,-1),FVector2D(-1,-1)};
	uint8 InspectionPointerState = 0;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> SpellWheelPanel;
	bool bSpellWheelOpen = false, bWheelStickReady = false, bWheelTurnNeutral = false;
	bool bWheelTriggerConsumed[2] = {false, false};
	int32 WheelHover = INDEX_NONE, WheelPage = 0, WheelRedraws = 0;
	TArray<int32> WheelSpells;
	void CloseSpellWheel();
	void RefreshSpellWheel();
	void UpdateSpellWheel();
	void ChangeWheelTab(int32 Direction);
	void ChangeWheelPage(int32 Direction);
	void ConfirmWheelSpell();
	struct FContactHand
	{
		FACEVRContactState State;
		TArray<FACEVRContactShape> Shapes;
		uint64 ShapeKey = 0;
		double LastPulse = -1.;
		bool bAvailable = false;
	};
	FContactHand ContactHands[2];
	void UpdateHandContacts(float Dt);
	void ResetHandContacts();
	FTransform GetPhysicalGrip(bool Left) const;
	FTransform GetPhysicalAim(bool Left) const;
	FVector PreviousContactGrip = FVector::ZeroVector, PreviousContactTip = FVector::ZeroVector;
	FVector SwingContactGrip = FVector::ZeroVector, SwingContactTip = FVector::ZeroVector;
	bool bPreviousContactBlade = false;
	FTransform VitalsAnchorFrame;
	FVector VitalsOwnerLocation = FVector::ZeroVector;
	void PlaceFocusPopup(const FVector& Point, bool ToLeft);
	void PaintFocusPopup();
	bool bVitalsAnchorReady = false;
	bool bVitalsAnchorTurning = false;
	FTransform GetVitalsAnchorTransform() const;
	void UpdateVitalsAnchor(float Dt);
	FTransform CompassAnchorFrame;
	FVector CompassOwnerLocation = FVector::ZeroVector;
	bool bCompassAnchorReady = false;
	bool bCompassAnchorTurning = false;
	FTransform GetCompassAnchorTransform() const;
	void UpdateCompassAnchor(float Dt);
	UWidgetComponent* GetEditablePanel(FName Panel) const;
	float& GetPanelScale(FName Panel);
	void UpdatePanelEdit(float Dt = 0.f);
	void UpdatePanelControls(bool MainVisible, bool OptionsVisible);
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> MenuControlsPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> OptionsControlsPanel;
	FName EditingPanel;
	int32 PanelEditHand = INDEX_NONE;
	bool bPanelResize = false;
	FTransform PanelEditFrame;
	FTransform PanelGrabInHand;
	FVector2D PanelEditStart = FVector2D::ZeroVector;
	FVector PanelEditLocation = FVector::ZeroVector;
	float PanelEditScale = 1.f;
	struct FWorldNotice
	{
		TWeakObjectPtr<UWidgetComponent> Panel;
		TWeakObjectPtr<AACEWorldEntityActor> Actor;
		FVector Position = FVector::ZeroVector;
		FString Text;
		FLinearColor Color;
		TSharedPtr<class SScrollBox> Scroll;
		double Started = 0., Expires = 0.;
		double ScrollStarts = 0., LastRedraw = 0.;
		float ScrollFrom = 0.f, ScrollEnd = 0.f, NumberLane = 0.f;
		int32 Kind = 0;
	};
	TArray<FWorldNotice> WorldNotices;
	FRotator CombatNoticeRotation = FRotator::ZeroRotator;
	double LastCombatNoticeUpdate = 0.;
	struct FEnemyHealthBar
	{
		TWeakObjectPtr<UWidgetComponent> Panel;
		TWeakObjectPtr<AACEWorldEntityActor> Actor;
		TSharedPtr<class SACEEnemyHealthBar> Meter;
		double Expires = 0;
		int32 RedrawsRemaining = 0;
	};
	TArray<FEnemyHealthBar> EnemyHealthBars;
	void EnemyHealth(int32 Guid, float Fraction);
	void UpdateEnemyHealthBars();
	int32 NoticePlayerGuid = 0, NoticeHealth = -1;
	uint32 NoticeNumberSequence = 0;
	void ShowWorldNotice(const FString& Text, int32 Guid, int32 Kind, FLinearColor Color);
	void UpdateWorldNotices();
	void NPCSpeech(int32 Guid, const FString& Text);
	void ClearConversationSelection(int32 Guid);
	void CombatFeedback(const FString& Name, int32 Amount, bool Incoming, bool Critical);
	void VitalsFeedback(const FACEPlayerVitals& Vitals);
	void HealthFeedback(int32 Guid, int32 Change, uint32 Flags);
	int32 TrackingCalibrationFrames = 0;
	bool IsPointerNearPanel(bool Left, FVector* Impact = nullptr) const;
	void ActivateRig();
	void UpdateTrackingState(bool Tracked);
	void BindInput();
	void UpdatePanels(float Dt = 1.f / 90.f);
	void UpdateNativeHUD(bool Available, float Dt = 1.f / 90.f);
	TSharedPtr<class SACEVRVitals> NativeVitals;
	TSharedPtr<class SACEVRJumpCharge> NativeJump;
	TSharedPtr<class SACEVRCompass> NativeCompass;
	TSharedPtr<class SACEVRFellowship> NativeFellowship;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> FellowshipPanel;
	FTransform FellowshipAnchorFrame;
	FVector FellowshipOwnerLocation = FVector::ZeroVector;
	bool bFellowshipAnchorReady = false;
	bool bFellowshipAnchorTurning = false;
	FTransform GetFellowshipAnchorTransform() const;
	void UpdateFellowshipAnchor(float Dt);
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> CompassPanel;
	double NextNativeHUDUpdate = 0.;
	void UpdateTextEntryFocus();
	UWidget* TextEntryUnderPointer(UWidgetInteractionComponent* Pointer) const;
	void UpdateVitalsDrag();
	void UpdatePointerVisuals(bool Available, bool MenuVisible);
	void UpdateInteractionFeedback(bool Available);
	UFUNCTION() void RevealRetailDialog(int32 Guid);
	UFUNCTION() void TradeChanged(int32 EventType);
	UFUNCTION() void CombatMessage(const FString& Text, const FString& Sender, int32 ChatType);
	void SetCastFeedback(const FString& Text);
	void UpdateArms(float Dt = 1.f / 90.f);
	FTransform GetAvatarGrip(bool Left) const;
	void UpdateCombat(float Dt);
	void UpdateComfort(float Dt);
	void InitializeComfortCurtain();
	void UpdatePortalView();
	void UpdatePortalEquipmentVisibility();
	bool HideEquipmentForPortal(AACEWorldEntityActor* Item);
	static bool UsesPlatformKeyboard();
	void RotateTracking(float Degrees);
	void CancelGestures();
	void PositionPanel(UWidgetComponent* Panel);
	void Trigger(bool bLeft, bool bPressed);
	void TriggerAxis(bool bLeft, float Value);
	void LeftTriggerAxis(float Value) { TriggerAxis(true, Value); }
	void RightTriggerAxis(float Value) { TriggerAxis(false, Value); }
	FACEVRTriggerState TriggerStates[2];
	void Grip(bool bLeft, bool bPressed);
	void SelectPressed();
	void PreviousSpell();
	void LeftTriggerDown() { Trigger(true, true); }
	void LeftTriggerUp() { Trigger(true, false); }
	void RightTriggerDown() { Trigger(false, true); }
	void RightTriggerUp() { Trigger(false, false); }
	void LeftGripDown() { Grip(true, true); }
	void LeftGripUp() { Grip(true, false); }
	void RightGripDown() { Grip(false, true); }
	void RightGripUp() { Grip(false, false); }
	void MoveX(float V) { MoveStick.X = V; }
	void MoveY(float V) { MoveStick.Y = V; }
	void TurnX(float V) { TurnStick.X = V; }
	void ScrollY(float V) { TurnStick.Y = V; }
	void JumpDown();
	void JumpUp();
	void FireSpell();
	bool IsPointedBuffRecipient(const AACEWorldEntityActor* Target) const;
	void GetSpellAim(FVector& Origin, FVector& Direction);
	void FireCrossbow();
	float GetRequestedCombatPower() const;
	void FireThrownMissile();
	bool HasEquippedCaster() const;
	FVector BowDrawDirection() const;
	FVector GetCrossbowMuzzle() const;
	void GetThrownAim(FVector& Origin, FVector& Direction) const;
	void ReleaseArrow();
	void UpdateMissileTrajectory(const FVector& Origin, const FVector& Direction, float Power, float SpellSpeed = 0.f, bool Gravity = true);
	TWeakObjectPtr<AACEWorldEntityActor> PredictedCombatTarget;
	UPROPERTY(Transient) TObjectPtr<class UProceduralMeshComponent> MissileTrajectory;
	double NextMissileProfileRequest = 0.;
	double NextSpellProfileRequest = 0.;
	void Pulse(bool bLeft, float Strength = .3f);
	AACEWorldEntityActor* AimTarget(FVector& Impact);
	FVector ToAceOffset(const FVector& World) const;
	UMotionControllerComponent* WeaponGrip() const;
	UMotionControllerComponent* WeaponAim() const;
	UMotionControllerComponent* BowGrip() const;
	UMotionControllerComponent* BowAim() const;
	int32 MissileStyle() const;
	bool IsAmmoLauncher() const;
	FACEWorldObject EquippedAmmo() const;
	void UpdateAmmoVisual(const FACEWorldObject& Ammo, const FVector& Nock, const FVector& Direction, bool bAtTip = false);
	TArray<int32> SpellSlots() const;
	FACEWorldObject EquippedWeapon() const;
	FACEWorldObject EquippedMissileWeapon() const;
	FVector GetWeaponTip();
	UPROPERTY(Transient) TObjectPtr<AACEPlayerController> PC;
	UPROPERTY(Transient) TObjectPtr<UACEClientSubsystem> Client;
	UPROPERTY(Transient) TObjectPtr<UACEVRSettings> Settings;
	UPROPERTY(Transient) TObjectPtr<UCameraComponent> DesktopCamera;
	UPROPERTY(Transient) TObjectPtr<AActor> PresentationActor;
	UPROPERTY(Transient) TObjectPtr<AActor> AmmoActor;
	int32 AmmoVisualGuid = 0;
	FVector AmmoNockLocal=FVector::ZeroVector, AmmoTipLocal=FVector::ZeroVector;
	TWeakObjectPtr<AACEWorldEntityActor> MissileVisualActor;
	uint64 MissileVisualRevision = 0;
	FVector CrossbowGripLocal = FVector::ZeroVector, CrossbowMuzzleLocal = FVector::ZeroVector;
	UPROPERTY(Transient) TObjectPtr<UWidgetInteractionComponent> LeftPointer;
	UPROPERTY(Transient) TObjectPtr<UWidgetInteractionComponent> RightPointer;
	UPROPERTY(Transient) TObjectPtr<UACEVRWidget> MenuWidget;
	UPROPERTY(Transient) TObjectPtr<UACEVRWidget> WristWidget;
	UPROPERTY(Transient) TObjectPtr<UACEVRRetailSurface> WristRetail;
	UPROPERTY(Transient) TObjectPtr<UACEVRRetailSurface> VitalsRetail;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> JumpPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> VitalsPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> FrameRatePanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> ChatPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> FocusPanel;
	UPROPERTY(Transient) TObjectPtr<UWidgetComponent> FocusOutline;
	UPROPERTY(Transient) TObjectPtr<class UProceduralMeshComponent> ComfortCurtain;
	UPROPERTY(Transient) TObjectPtr<class UMaterialInstanceDynamic> ComfortMaterial;
	FString FocusTitle, FocusHint;
	FString PaintedFocusTitle, PaintedFocusHint;
	int32 FeedbackHand = 1, LastFocusGuid = 0, ConversationHoverGuid = 0;
	TWeakObjectPtr<AACEWorldEntityActor> SelectedWorldTarget;
	TWeakObjectPtr<AACEWorldEntityActor> HighlightedSelection, HighlightedHover;
	void UpdateWorldSelectionHighlights(AACEWorldEntityActor* Selection, AACEWorldEntityActor* Hover);
	bool bWristPoseReady = false;
	FTransform WristLocalPose = FTransform::Identity;
	FTransform SettingsLayoutFrame = FTransform::Identity;
	bool bWristGazed = false;
	float WristLookAwayTime = 0.f;
	int32 VitalsDragHand = INDEX_NONE;
	FVector2D VitalsGrabOffset = FVector2D::ZeroVector;
	UPROPERTY(Transient) TObjectPtr<UACEVRWidget> KeyboardWidget;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> FallbackArms;
	UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> Arrow;
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> BowStrings;
	UPROPERTY(Transient) TArray<TObjectPtr<class UProceduralMeshComponent>> PointerBeams;
	int32 PointerBeamModes[2] = {-1,-1};
	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> PointerTips;
	int32 LastTrackingStatus = INDEX_NONE;
	FVector2D MoveStick = FVector2D::ZeroVector, TurnStick = FVector2D::ZeroVector;
	FVector2D SmoothedMoveStick = FVector2D::ZeroVector;
	ACEVRMath::FSwing Swing;
	struct FPunch
	{
		ACEVRMath::FSwing Gesture;
		FVector PreviousContact = FVector::ZeroVector, StartContact = FVector::ZeroVector;
	};
	FPunch Punches[2];
	TArray<FACEVRContactTriangle> ContactTriangles;
	void UpdateTwoHandUse(float Dt);
	int32 TouchUseGuid = 0;
	float TouchUseHold = 0.f;
	bool bTouchUseArmed = true, bTouchUsePrevious = false;
	FVector TouchUsePreviousHands[2];
	FVector TouchUsePreviousBody = FVector::ZeroVector;
	void UpdateUnarmed(float Dt);
	bool TryTrackedStrike(int32 Weapon, bool Left, ACEVRMath::FSwing& Gesture,
		const FVector& StartGrip, const FVector& StartTip, const FVector& Grip, const FVector& Tip);
	FVector PreviousTipWorld = FVector::ZeroVector;
	FVector SwingStartGrip = FVector::ZeroVector, PreviousSwingGrip = FVector::ZeroVector;
	FVector BladeTipInGrip = FVector(65, 0, 0);
	bool bBladeCalibrated = false;
	FVector LastPawnLocation = FVector::ZeroVector;
	TWeakObjectPtr<AACEWorldEntityActor> TrackedWeaponActor;
	TWeakObjectPtr<UMotionControllerComponent> UIAimOverride;
	FVector WeaponTipLocal = FVector::ZeroVector;
	FVector WeaponAxisLocal = FVector::UpVector;
	uint64 WeaponMeshRevision = 0;
	bool bActive = false, bTracking = false, bInputBound = false, bTurnReady = true;
	bool bInventoryOpen = false, bSettingsOpen = false, bDrawing = false;
	bool bInventoryButtonHeld = false;
	float InventoryHoldSeconds = 0.f;
	bool bLeftGripHeld = false, bRightGripHeld = false;
	bool bLeftPointerPressed = false, bRightPointerPressed = false;
	bool bSelectPointerPressed = false;
	bool bJumpHeld = false;
	float JumpHoldSeconds = 0.f;
	bool bTriggerDrawing = false;
	bool bKeyboardOpen = false;
	bool bTextKeyboardOpen = false;
	TWeakObjectPtr<UWidget> FocusedTextEntry;
	TSharedPtr<class FACEVRPlatformTextEntry> PlatformTextEntry;
	FTransform TextKeyboardTransform;
	bool bWasLoading = false;
	bool bPortalViewActive = false;
	TMap<TWeakObjectPtr<AACEWorldEntityActor>, bool> PortalEquipmentVisibility;
	FTransform PortalTrackingTransform;
	double TimingSeconds = 0., TimingGame = 0., TimingRender = 0., TimingRHI = 0., LastTimingFrameSeconds = 0.;
	int32 TimingFrames = 0;
	double TimingMoveSeconds = 0., TimingMoveDistance = 0., TimingWorldSeconds = 0.;
	FVector LastTimingPosition = FVector::ZeroVector;
	int32 SelectedSpell = 0, SpellPage = 0, LastPlayer = 0, LastTeleport = -1, LastWeapon = 0, LastMode = 0;
	float BowFraction = 0.f, BowHoldTime = 0.f, Fade = 0.f;
	double LastHello = -100, LastCast = -100, LastScroll = -100;
	int32 WorldRefreshRemaining = 0;
	double NextWorldRefresh = 0;
	FString CastFeedback;
	double CastFeedbackUntil = 0;
	float RecenterHeight = 0.f;
	float AvatarEyeHeight = 165.f;
	double NextPoseSendTime = 0.;
	float VerticalComfortOffset = 0.f;
};
