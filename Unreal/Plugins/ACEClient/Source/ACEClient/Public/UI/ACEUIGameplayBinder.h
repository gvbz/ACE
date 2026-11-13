#pragma once

class UACEUICharGenBinder;

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ACETypes.h"
#include "ACECharacterCreation.h"
#include "ACEUIGameplayBinder.generated.h"

class UACEClientSubsystem;
class FACESession;
class UACEUIElementManager;
class UACEUICanvasWidget;
class UMultiLineEditableText;
class UACERetailTextEntry;
class UACEUIResourceResolver;
class AACEPlayerController;
class UBorder;
class UProgressBar;
class UCanvasPanel;
class USizeBox;
class UTextBlock;
class UEditableTextBox;
class UComboBoxString;
class UScrollBox;
class USlider;
class UButton;
class UWidget;
class UImage;
class UUserWidget;
class UPrimitiveComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTextureRenderTarget2D;
class USceneCaptureComponent2D;
struct FACEUIElement;

struct FAceIconPaintCache
{
	int32 Did = INDEX_NONE;
	FLinearColor Tint = FLinearColor::Transparent;
	bool bCleared = true;
	int32 Style = 0;
};

/** Already formatted and filtered retail chat, shared with the native VR view. */
struct FACEChatDisplayLine
{
	FString Text, Sender;
	FLinearColor Color;
	uint64 Serial = 0;
};

/**
 * Binds retail classic_gameplay ElementNames to ACE networking + dynamic overlays.
 * Owns click routing (toolbar / panel / examine / combat / chat) and data refresh
 * (vitals meters, selection label, inventory icons, appraisal text, chat log).
 */
UCLASS()
class ACECLIENT_API UACEUIGameplayBinder : public UObject
{
	GENERATED_BODY()
	friend class FACERetailScreenTest;
	friend struct FACERetailVendorUIFixture;
	friend struct FACEContractUIFixture;
	friend class FACEPortalInterfaceTest;
	friend class FACERetailLogoutTest;
	friend struct FACERetailAugmentationUIFixture;
	friend class FACEDungeonInterfacesTest;
	friend class FACEDesktopHighlightTest;
	friend class FACEPvPCombatParityTest;
	friend class FACEAttackerIdentityTest;
	friend class FACECharacterInfoParityTest;
	friend class FACEVRRigTest;
	friend class FACEVRProtocolTest;
	friend class UACEVRComponent;
	friend class UACEVRMenu;
    friend class UACEMobileWidget;
    friend class FACEMobileTouchTest;
	friend class UACEVRChat;
	friend class FACEVRChatTest;
	friend class AACEPlayerController;
	friend class FACEChatParityTest;
	friend class FACEArenaRecallTest;
	friend class FACERetailCommandAuditTest;
	friend struct FACEChatUIFixture;
	friend class FACEPluginPublicAPITest;
	friend class FACEEmoteTest;
	friend class FACEUILayoutCommandsTest;
	friend class FACEUIInteractionParityTest;
	friend class FACEVendorTransactionsTest;
	friend class FACERetailSpellComponentsTest;
	friend class FACEPanelResizeSocialTest;
	friend class FACEInventoryUICostTest;
	friend class FACEGameplayRefreshTest;
	friend class FACEUIWorkloadTest;
	friend class FACESalvageTest;
	friend class FACEHousingTest;
	friend class FACEServerDialogsTest;
	friend class FACEBookWritingTest;
	friend class FACEChessTest;
	friend class FACEBarberTest;
	friend class FACEItemPresentationTest;
	friend class FACESelectionToolbarTest;
    friend class FACEActionDeckTest;
    friend class FACERetailCursorTest;

public:
    /** Shared attack mark used by desktop controls, VR and plugin commands. */
    void SetRequestedAttackPower(float Power) { RequestedAttackPower=FMath::Clamp(Power,0.f,1.f); }
    void ShowPluginAttack(int32 Target, uint32 Height, float Power);
	bool ScrollFellowship(float WheelDelta, FVector2D CanvasLocalPos);
	bool ScrollAllegiance(float WheelDelta, FVector2D CanvasLocalPos);
	bool ScrollSalvage(float WheelDelta, FVector2D CanvasLocalPos);
	void Initialize(UACEClientSubsystem* InClient, UACEUIElementManager* InManager,
		UACEUICanvasWidget* InCanvas, AACEPlayerController* InPC);
	void Shutdown();
	void RequestGameplayScreenshot();
	float GetCameraFieldOfViewDegrees() const;
	void SetCombatSpellBar(int32 Tab);

	/** Called each canvas tick after layout sync. */
	void TickRefresh(bool bRefreshDesktopHUD = true);
	/** Presentation only; native VR HUDs do not consume these desktop overlays. */
	void RefreshDesktopHUD();
	bool InspectSpellAt(FVector2D CanvasPoint);

	UFUNCTION()
	void HandleChatMessage(const FString& Text, const FString& Sender, int32 ChatType, int32 FilterType = -1);

	UFUNCTION()
	void HandleAppraisal(const FACEAppraisalInfo& Appraisal);

	UFUNCTION()
	void HandleSelectionChanged(const FACESelectedObject& Selection);

	UFUNCTION()
	void HandleVitalsUpdated(const FACEPlayerVitals& Vitals);

	void ToggleGameplayPanel(const FString& PageElementName, const FString& TabName = FString());
	/** Idempotent panel selection for world-space VR menus. */
	void OpenGameplayPanel(const FString& PageElementName) { ShowPanelPage(PageElementName); }
	void HandleEscape();
private:
	uint64 DismissedIdentifySerial = 0;
	TArray<FACEChatDisplayLine> ChatDisplayLines[5];
	uint64 ChatDisplayRevision = 0;
	uint64 ChatDisplaySerial = 0;
	bool bExaminationDismissed = false;
public:
	void ToggleKeyboardMappingUI();
	void PollKeyboardActions(APlayerController* PC);
	/** Panel and window shortcuts which remain available during portal transitions. */
	void PollInterfaceKeyboardActions(APlayerController* PC);
	void HandleKeymapImport(const FString& Path);
	bool ScrollKeyboard(float WheelDelta, FVector2D CanvasLocalPos);
	void ToggleCombatModeHotkey();
	void PlayEmoteHotkey(uint32 MotionCommand, bool bHoldPose);

	void FocusChatEntry();
	void ToggleChatEntryFocus();
	bool IsChatTextEntryFocused() const;
	void ClearChatEntryFocus();
	double InventoryDoubleClickSeconds() const;
	bool GetVRPointerFeedback(FVector2D Point, FBox2D& Bounds, FString& Label, FString& Hint, int32& Guid) const;
	float GetVRSecondClickFraction(int32 Guid) const;
	bool ActivateVRPointerItem(FVector2D Point);
	bool InspectVRPointerItem(FVector2D Point);
	float InventoryDragThreshold() const;
	void CancelPendingChatRefocus() { bPendingChatRefocus = false; }
	/** Retail SelectCommandFromHistory: Up = previous, Down = next (past end clears). */
	void NavigateChatHistory(bool bPrevious);
	FString ExpandChatReply(const FString& Text) const;
	/**
	 * Send chat entry text. Optional OverrideText bypasses GetText() (cleared on focus loss).
	 * SourceWindow: 0 = main chat, 1..4 = FloatyChat1-4 (retail m_eWindowID semantics
	 * drive @title / @clear targeting).
	 */
	bool TrySendChatFromEntry(const FString* OverrideText = nullptr, int32 SourceWindow = 0);
	/** Dispatch an explicit macro command without changing typed chat, its history or focus. */
	bool DispatchActionDeckCommand(const FString& Command);
	/** Jump charge meter (0 = hidden, 0..1 = fill). Uses retail Powerbar fill texture. */
	void SetJumpChargeFraction(float Fraction);
	/** Hit-test inventory/pack/spell overlays. bRightClick → identify (inventory/doll). */
	bool TryHandleOverlayClick(FVector2D CanvasLocalPos, bool bRightClick = false);
	/** LMB press on inventory icon begins a pending drag (activate after small move). */
	bool TryBeginInventoryDrag(FVector2D CanvasLocalPos);
	/** Desktop world items share the captured inventory drag/drop path. */
	bool TryBeginWorldInventoryDrag(FVector2D CanvasLocalPos);
	bool BeginWorldItemDrag(int32 Guid, FVector2D CanvasLocalPos);
	void AssignInventoryShortcut(int32 Guid, int32 SlotIndex, int32 SourceShortcut = INDEX_NONE);
	void UpdateInventoryDrag(FVector2D CanvasLocalPos);
	/** Completes drag (move/wield/drop) or click/double-click use when not dragged. */
	bool TryFinishInventoryDrag(FVector2D CanvasLocalPos);
	/** Spellbook row / hotbar icon → add/remove/rearrange on magic bar. */
	bool TryBeginSpellDrag(FVector2D CanvasLocalPos);
	void UpdateSpellDrag(FVector2D CanvasLocalPos);
	bool TryFinishSpellDrag(FVector2D CanvasLocalPos);
	bool TryBeginCombatPowerDrag(FVector2D CanvasLocalPos);
	bool TryBeginCombatButton(FVector2D CanvasLocalPos);
	bool TryReleaseCombatButton();
	void CancelCombatAttack();
	void SetCombatMovementBlocked(bool bBlocked);
	void UpdateCombatPowerDrag(FVector2D CanvasLocalPos);
	bool TryFinishCombatPowerDrag(FVector2D CanvasLocalPos);
	/** DAT scrollbar thumb drag (inventory / spellbook / loot / skills). */
	bool TryBeginScrollbarDrag(FVector2D CanvasLocalPos);
	void UpdateScrollbarDrag(FVector2D CanvasLocalPos);
	bool TryFinishScrollbarDrag();
	bool IsScrollbarDragActive() const;
	bool IsInventoryDragActive() const { return bInvDragActive; }
	bool IsInventoryInteractionActive() const { return bInvDragPending; }
	bool IsSpellDragActive() const { return bSpellDragActive; }
	void CancelPointerGestures();
	void SelectVRSpell(int32 Spell);
	/** Scroll main chat log (wheel / DAT scrollbar). Negative = up. */
	bool ScrollChatLog(float PixelDelta, int32 Window = 0);
	int32 ChatWindowAtPointer(FVector2D CanvasLocalPos) const;
	bool IsPointerOverChatLog(FVector2D CanvasLocalPos) const;
	bool IsPointerOverInventoryGrid(FVector2D CanvasLocalPos) const;
	bool IsPointerOverInventoryPanel(FVector2D CanvasLocalPos) const;
	bool IsPointerOverPackList(FVector2D CanvasLocalPos) const;
	bool ScrollInventoryGrid(float WheelDelta);
	bool ScrollPackList(float WheelDelta);
	/** Skills / Attributes list under SkillManagementPanel. */
	bool IsPointerOverStatList(FVector2D CanvasLocalPos) const;
	bool ScrollStatList(float WheelDelta);
	bool GetStatTooltipAt(FVector2D Absolute, FString& OutText, uint32* OutTemplateId = nullptr) const;
	bool GetMapTooltipAt(FVector2D Absolute, FString& OutText) const;
	/** Spellbook spell list + scrollbar. */
	bool IsPointerOverSpellbookList(FVector2D CanvasLocalPos) const;
	bool IsPointerOverEffectsList(FVector2D CanvasLocalPos) const;
	bool ScrollEffectsList(float WheelDelta);
	/** Options panel character/chat/config option list. */
	bool IsPointerOverOptionsList(FVector2D CanvasLocalPos) const;
	bool ScrollOptionsList(float WheelDelta);
	/** Spell panel Components tab (fill component book). */
	bool IsPointerOverComponentList(FVector2D CanvasLocalPos) const;
	bool ScrollComponentList(float WheelDelta, FVector2D CanvasLocalPos);
	/** Quest panel contract list. */
	bool IsPointerOverQuestList(FVector2D CanvasLocalPos) const;
	bool ScrollQuestList(float WheelDelta);
	/** World panel housing text. */
	bool IsPointerOverHouseList(FVector2D CanvasLocalPos) const;
	bool ScrollHouseList(float WheelDelta);
	bool ScrollSpellbook(float WheelDelta);
	bool IsPointerOverExternalContainer(FVector2D CanvasLocalPos) const;
	bool ScrollExternalContainer(float WheelDelta);
	/** Toolbar shortcut row page (0/1). */
	bool IsPointerOverShortcutBar(FVector2D CanvasLocalPos) const;
	bool ScrollShortcutBar(float WheelDelta);
	bool IsChatEntryFocused() const;

	/** Number-key hotbar: cast when magic combat is up, else use shortcut slot. */
	void ActivateHotbarSlot(int32 SlotIndex);
    /** Manual deck actions use the same equip/target/charge paths as native UI. */
    void UseActionDeckItem(int32 Guid) { CancelPendingUseWith(); UseInventoryItem(Guid); }
    void TargetActionDeckSelf();
    void RequestActionDeckAttack(uint32 Height) { BeginCombatPowerCharge(Height, false); }

	/**
	 * If a dual-use source is armed (key/tool targeting), complete UseWithTarget on TargetGuid.
	 * Returns true when the pending use was consumed (caller should not Use normally).
	 */
	bool TryCompletePendingUseWithTarget(int32 TargetGuid);
	int32 GetPendingUseWithSourceGuid() const;
	bool IsPendingUseTargetCompatible(int32 TargetGuid) const;
	int32 GetInventoryTargetAt(FVector2D Absolute) const;
	void CancelPendingUseWith();
	void SyncPendingUseCursor();

	/** Re-apply MotionStance from CombatMode (portal / recall arrival). */
	void ApplyPreferredStance(int32 Mode);

	/** Restore CombatMode + spell hotbar after portal without re-sending ChangeCombatMode. */
	void SyncCombatModeFromServer(int32 Mode);

	UFUNCTION()
	void HandleChatTextCommitted(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void HandleFloatyChat1Committed(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void HandleFloatyChat2Committed(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void HandleFloatyChat3Committed(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void HandleFloatyChat4Committed(const FText& Text, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleExternalContainerOpened(int32 Guid);
	UFUNCTION()
	void HandleExternalContainerClosed(int32 Guid);
	UFUNCTION()
	void HandleVendorOpened(int32 Guid);
	UFUNCTION()
	void HandleTradeStateChanged(int32 EventType);
	UFUNCTION()
	void HandleChessEvent(const FACEChessEvent& Event);
	UFUNCTION()
	void HandleCharacterTitlesChanged();
	UFUNCTION()
	void HandleFellowshipChanged();
	UFUNCTION()
	void HandleAllegianceChanged();
	UFUNCTION()
	void HandleFriendsChanged();
	UFUNCTION()
	void HandleContractsChanged();
	UFUNCTION()
	void HandleSquelchChanged();
	UFUNCTION()
	void HandleSquelchNameCommitted(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION()
	void HandleBookChanged();
	UFUNCTION()
	void HandleEnchantmentsChanged();
	UFUNCTION()
	void HandleSpellLearned(int32 SpellId);
	UFUNCTION()
	void HandleLinkStatusChanged(const FACELinkStatus& Status);
	UFUNCTION()
	void HandleFriendNameCommitted(const FText& Text, ETextCommit::Type CommitMethod);
	UFUNCTION() void HandleFellowshipNameChanged(const FText& Text);
	UFUNCTION() void HandleFellowshipNameCommitted(const FText& Text, ETextCommit::Type CommitMethod);

public:
	bool TryHandleModalPopupClick(FVector2D Absolute);

private:
	enum class EACEUIScrollTarget : uint8
	{
		None,
		Spellbook,
		InvGrid,
		PackList,
		ExtItems,
		ExtPacks,
		StatList,
		Attributes,
		TitleList,
		StackSize,
		Effects,
		Fellowship,
		Friends,
		Squelch,
		Allegiance,
		Keyboard,
		EffectsInfo,
		Chat,
		VendorItems,
		VendorBuy,
		VendorSell,
		TradeSelf,
		HousePayment,
		TradeOther,
		Salvage,
		OptionsList,
		Examination,
		Inscription,
		CharacterInfo,
		Book,
		JournalList,
		JournalNotes,
		Components,
	};
	UPROPERTY()
	TObjectPtr<UACEClientSubsystem> Client;

	UPROPERTY()
	TObjectPtr<UACEUIElementManager> Manager;

	UPROPERTY()
	TObjectPtr<UACEUICanvasWidget> Canvas;

	UPROPERTY()
	TObjectPtr<AACEPlayerController> PlayerController;

	FDelegateHandle ActivatedHandle;

	FString ActivePanelPage;
	int32 CombatMode = 1; // ACECombatMode::NonCombat
	bool bActionDeckCombatBarHidden = false;
	void RefreshNativeCombatBarVisibility();
	int32 SelectedPackGuid = 0;
	int32 InventoryScrollOffset = 0;
	TWeakPtr<FACEUIElement> ScrollDragBar;
	float ScrollDragGrabOffset = 0.f;
	float ScrollDragFraction = 0.f;
	float ScrollDragVisibleFraction = 1.f;
	int32 ScrollDragLastOffset = INDEX_NONE;
	int32 PackScrollOffset = 0;
	bool bInventoryOverlaysCollapsed = false;
	uint64 LastInventoryOverlayHash = ~uint64(0);
	bool bVendorOverlaysCollapsed = false;
	uint64 LastVendorOverlayHash = ~uint64(0);
	bool bLootOverlaysCollapsed = false;
	uint64 LastLootOverlayHash = ~uint64(0);
	TMap<TWeakObjectPtr<UBorder>, FAceIconPaintCache> IconPaintCache;
	/** Absolute favorite index, independent of the visible scroll offset; -1 selects innate. */
	int32 SelectedCombatSpellSlot = 0;
	struct FSpellTabSelection { int32 Slot = 0; int32 SpellId = 0; int32 Offset = 0; };
	FSpellTabSelection SpellTabSelections[8];
	int32 SelectionSpellTab = INDEX_NONE;
	bool bRevealSelectedSpell = true;
	void SyncSpellTabSelection();
	void SelectCombatSpellSlot(int32 Slot);
	void StepCombatSpellSelection(int32 Direction, bool bFirst, bool bLast);
	FDelegateHandle ScreenshotProcessedHandle;
	FString PendingScreenshotFilename;
	double ScreenshotRequestTime = 0;
	void FinishGameplayScreenshot();
	void CancelGameplayScreenshot();
	/** Equipped wand/orb SpellDID for BuiltInSpell; SelectedCombatSpellSlot < 0 selects it. */
	int32 BuiltInSpellId = 0;
	/** Guid of the wielded caster that owns BuiltInSpellId (UseWithTarget source). */
	int32 BuiltInCasterGuid = 0;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> BuiltInSpellIconBorders;
	int32 ChatFilterMode = 0; // 0=All, 1=Speech, 2=Combat, 3=System
	/**
	 * ChatTarget send destination:
	 * 0=Say, 1=Fellow, 2=Allegiance, 3=Vassals, 4=Patron, 5=Monarch, 6=CoVassals.
	 */
	int32 ChatSendChannel = 0;
	bool bChatTargetPopupOpen = false;
	void ToggleMainChatExpanded();
	FString LastOutgoingTellName;
	FString LastOutgoingTellLineKey;
	double LastOutgoingTellLineTime = 0.0;
	/** Retail ChatInterface input history: 100 entries, up/down recall, pos resets on send. */
	/** "Stay in chat mode after sending" — refocus entry on the tick after commit. */
	bool bPendingChatRefocus = false;
	void ApplyPendingChatFocus();
	/** Window (0 = main, 1..4 = floaty) awaiting the deferred refocus. */
	int32 PendingChatRefocusWindow = 0;

	/** FloatyChat1-4 per-window state (array index 0..3 = window 1..4). */
	static constexpr int32 NumFloatyChats = 4;
	UPROPERTY()
	TArray<TObjectPtr<UScrollBox>> FloatyChatLogs;
	UPROPERTY()
	TArray<TObjectPtr<UEditableTextBox>> FloatyChatEntries;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> FloatyChatTitleLabels;
	TArray<int32> FloatyChatLineCounts;
	/** Window titles (retail @title / SetChatWindowTitle, floaty windows only). */
	TArray<FString> FloatyChatTitles;
	/** Retail per-window 64-bit m_llTextTypeFilter (bit per LogTextType; set = shown). */
	TArray<uint64> FloatyChatFilters;
	uint64 MainChatTypeFilter = 0xFBFFFFFFull;
	/** Which entry the command being dispatched came from (0 = main, 1..4 = floaty). */
	int32 ChatSourceWindow = 0;
	/** Retail @filter/@unfilter global squelch mask (bit per LogTextType; set = shown). */
	uint64 GlobalChatTypeFilter = ~0ull;
	/** Retail @log: mirror displayed chat lines to a text file. */
	bool bChatMirrorToFile = false;
	FString ChatMirrorFilePath;
	/** Chat rows eligible for click-to-tell (retail <Tell:IIDString> tags). */
	struct FChatRowSender
	{
		TWeakObjectPtr<UTextBlock> Row;
		FString Sender;
		int32 Window = 0;
	};
	TArray<FChatRowSender> ChatRowSenders;
	FString ActiveSkillTab = TEXT("AttributePage");
	FString ActiveSpellPanelTab = TEXT("SpellbookPage");
	int32 SelectedAttributeRow = INDEX_NONE;
	int32 AttributeScrollOffset = 0;
	int32 SelectedSkillId = 0;
	int32 SkillListScrollOffset = 0;
	int32 SkillListContentCount = 0;
	int32 TitleListScrollOffset = 0;
	TArray<int32> SortedTitleIds;
	/** Pending title selection for CharacterTitle_SetAsDisplayButton (0 = none). */
	int32 SelectedTitleId = 0;
	UPROPERTY()
	TObjectPtr<UTextBlock> TitleCurrentLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> TitleCurrentValue;
	UPROPERTY()
	TObjectPtr<UTextBlock> TitleSetButtonLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> InvContentsLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderLuminanceLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderLuminanceValue;
	int32 SpellHotbarScrollOffset = 0;
	bool bShowPaperdollSlots = false;
	/** Retail vitals: icons (detail) by default; click meter → numeric "cur / max". */
	bool bShowVitalNumbers = false;
	/** Auto-scroll chat only while already pinned to the latest line. */
	bool bChatStickToBottom = true;
	/** Re-assert ScrollToEnd after wrap layout (same-frame ScrollToEnd is stale). */
	bool bPendingChatScrollToEnd = false;
	/** Last GetScrollOffsetOfEnd while sticking — re-snap when wrap grows content. */
	float ChatStickEndOffset = -1.f;
	bool bFloatyChatStickToBottom[4] = { true, true, true, true };
	bool bChatUnread[5] = {};
	int32 ScrollDragChatWindow = 0;
	void RefreshChatRowLayout(int32 Window);
	void RefreshChatEntryLayouts();
	uint32 ChatEntryFontId = 0;
	int32 ChatEntryLineHeight = 12;
	float ChatRowWidths[5] = {};
	uint32 ChatRowFontIds[5] = {};
	FVector2D ChatRowScales[5] = {};
	/** Client-side loot range close debounce. */
	double PendingLootRangeCloseAt = 0.0;

	/** Melee / missile combat panel state (retail FloatyCombatPanel). */
	float CombatPowerOrAccuracy = 0.f;
	uint32 CombatAttackHeight = 2; // ACEAttackHeight::Medium
	bool bCombatAutoRepeat = false;
	bool bCombatAutoTarget = true;
	bool bCombatViewTarget = true;
	bool bCombatPowerDrag = false;
	float CombatPowerDragGrabOffset = 0.f;
	bool bCombatAdvancedUI = false;
	float JumpChargeDisplayFraction = 0.f;
	void RefreshPowerbarOverlay();
	bool bCombatPowerCharging = false;
	bool bCombatAttackRequestPending = false;
	bool bCombatAttackHeld = false;
	bool bCombatMovementBlocked = false;
	bool bCombatPointerHeld = false;
	bool bCombatKeyboardHeld = false;
	bool bCombatRequestSent = false;
	float ReleasedAttackPower = 0.f;
	bool bCombatRepeatActive = false;
	bool bCombatRetargetPending = false;
	bool bCombatPluginAttack = false;
	uint32 LastCombatEventRevision = 0;
	int32 LastCombatAttackTarget = 0;
	float LastCombatAttackPower = 0.f;
	double CombatPowerBuildStartTime = 0.0;
	float RequestedAttackPower = 0.5f;
	int32 LastAppliedStanceKey = INDEX_NONE;
	int32 LastEquipModeSnapshot = INDEX_NONE;

	/** User-requested combat mode; ignore transient server NonCombat while pending. */
	int32 PendingCombatMode = 0;
	double PendingCombatModeUntil = 0.0;

	FACEPlayerVitals LastVitals;
	FACESelectedObject LastSelection;
	double SelectionFlashUntil = 0;
	/** Toolbar DAT state pause is 0.25s, independently of the 0.4s object flash. */
	double ToolbarSelectionFlashUntil = 0;
	void SetRetailTooltip(UWidget* Widget, const FText& Text);
	void TickSelectionFlash();
	void RefreshKeyboardOverlays();
	void ShowKeymapImport(const FString& Report = FString());
	void RefreshKeymapDialog();
	UFUNCTION() UWidget* GenerateKeymapFileLabel(FString Item);
	bool HandleKeyboardNamedClick(const FString& Name);
	FString ActiveKeyboardPage = TEXT("Movement");
	UPROPERTY(Transient) TArray<TObjectPtr<UWidget>> KeyboardRows;
	TArray<TSharedPtr<FACEUIElement>> KeyboardEntryElements;
	TSharedPtr<FACEUIElement> KeyboardGroupElement;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> KeyboardGroupLabel;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> KeyboardRowLabels;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> KeyboardKeyLabels;
	int32 KeyboardScrollOffset=0, KeyboardMaxOffset=0, KeyboardVisibleRows=1;
	TSharedPtr<FACEUIElement> KeymapDialog;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> KeymapReportScroll;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> KeymapReportText;
	bool bKeymapReport = false;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> KeymapDialogLabels;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> KeymapFileChoice;
	TArray<FString> KeymapFiles;
	FString KeymapDialogMessage, KeymapOverwritePath;
	bool bKeymapSave=false;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> KeymapImportPath;
	bool bKeymapImportOpen = false;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> KeyboardLabels;
	UPROPERTY(Transient) TObjectPtr<UUserWidget> VideoSettings;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> FlashMeshes;
	TArray<int32> FlashMaterialSlots;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInterface>> FlashOriginalMaterials;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> FlashMaterials;
	FACEAppraisalInfo LastAppraisal;
	/** Selected stack split amount for give/drop/sell (1..StackSize). */
	int32 SelectedStackAmount = 1;
	int32 SelectedStackMax = 1;

	UPROPERTY()
	TObjectPtr<UTextBlock> HealthLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> StaminaLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> ManaLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> SelectionText;
	/** Retail transient banner: yellow center-top info text ("You're too busy!") that fades. */
	UPROPERTY()
	TObjectPtr<UTextBlock> TransientInfoText;
	UPROPERTY() TObjectPtr<USizeBox> TransientInfoBox;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> TransientInfoLines;
	TArray<double> TransientInfoTimes;
	TSharedPtr<FACEUIElement> TransientInfoTemplate;
	FVector2D TransientInfoScale = FVector2D::ZeroVector;
	/** Retail VividTargetIndicator_Selected parent (resized to screen AABB). */
	UPROPERTY()
	TObjectPtr<UCanvasPanel> SelectionMarkerBox;
	UPROPERTY(Transient) TObjectPtr<UBorder> SelectionDirectionArrow;
	/** Retail VividTargetIndicator_Selected_* DAT corners (0x06004C40–43). */
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SelectionMarkerCorners;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExamTitle;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExamBody;
	UPROPERTY()
	TObjectPtr<UScrollBox> ExamScroll;
	UPROPERTY() TObjectPtr<UScrollBox> ExamInscriptionScroll;
	int32 ExamInscriptionScrolledGuid = 0;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ExamAttributeLabels;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ExamAttributeValues;
	UPROPERTY() TObjectPtr<UTextBlock> ExamLevel;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ExamCreatureHeadings;
	UPROPERTY() TObjectPtr<UScrollBox> ExamCreatureDetailsScroll;
	UPROPERTY() TObjectPtr<USizeBox> ExamCreatureDetailsSize;
	UPROPERTY() TObjectPtr<UCanvasPanel> ExamCreatureDetailsCanvas;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ExamMiscLabels;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ExamMiscValues;
	TArray<TSharedPtr<FACEUIElement>> ExamMiscRows;
	int32 ExamCreatureScrolledGuid = 0;
	TArray<TSharedPtr<FACEUIElement>> ExamAttributeRows;
	int32 ExamScrolledObjectGuid = 0;
	void RefreshCreatureExamination();
	UPROPERTY()
	TObjectPtr<UTextBlock> PanelBodyText;
	UPROPERTY() TObjectPtr<UScrollBox> CharacterInfoScroll;
	UPROPERTY() TObjectPtr<UTextBlock> CharacterInfoTitle;
	FString BuildCharacterInformation();
	UPROPERTY()
	TObjectPtr<UScrollBox> ChatLog;
	UPROPERTY()
	TObjectPtr<UEditableTextBox> ChatEntry;
	int32 LastFocusedChatWindow = 0;
	int32 ChatLineCount = 0;
	static constexpr int32 MaxChatLines = 120;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> InventorySlots;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> InventorySlotBgs;
	/** IconOverlay DID (ItemMaxLevel diamonds, etc.) drawn above the icon. */
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> InventorySlotOverlays;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> InventorySlotSelected;
	/** Retail UIItem quantity overlay: stack count numeral on the icon. */
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> InventorySlotCounts;
	UPROPERTY()
	TArray<int32> InventorySlotGuids;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> PackSlots;
	/** Item-type background and custom underlay beneath occupied pack icons. */
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> PackSlotBgs;
	/** Selection ring over the active pack tab (main + side slots). */
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> PackSlotSelected;
	UPROPERTY() TArray<TObjectPtr<UBorder>> PackItemSelected;
	UPROPERTY() TArray<TObjectPtr<UProgressBar>> PackCapacityMeters;
	UPROPERTY() TArray<TObjectPtr<UBorder>> PackShortcutIcons;
	UPROPERTY(Transient) TObjectPtr<UBorder> MainPackShortcutIcon;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> MainPackCapacityMeter;
	UPROPERTY()
	TArray<int32> PackSlotGuids;
	UPROPERTY()
	TMap<FString, TObjectPtr<UBorder>> PaperDollIcons;
	UPROPERTY() TMap<FString, TObjectPtr<UBorder>> PaperDollShortcutIcons;
	UPROPERTY() TMap<FString, TObjectPtr<UBorder>> PaperDollSelectedIcons;
	UPROPERTY() TObjectPtr<UBorder> PaperDollDragTargetIcon;
	UPROPERTY()
	TMap<FString, TObjectPtr<UBorder>> PaperDollSlotBgs;
	/**
	 * Retail's PaperDoll element is a Viewport (LayoutDesc type 0x0D) that gmPaperDollUI draws
	 * the character into via CreatureMode — not a flat icon sheet. Scene-capture rig for it.
	 */
	UPROPERTY()
	TObjectPtr<UImage> PaperDollModelImage;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> PaperDollRenderTarget;
	UPROPERTY(Transient)
	TObjectPtr<AActor> PaperDollPreviewActor;
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> PaperDollCapture;
	int32 PaperDollAppliedGuid = 0;
	uint32 PaperDollAppliedHash = 0;
	UPROPERTY()
	TObjectPtr<UImage> ExamPaperDollModelImage;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ExamPaperDollRenderTarget;
	UPROPERTY(Transient)
	TObjectPtr<AActor> ExamPaperDollPreviewActor;
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> ExamPaperDollCapture;
	int32 ExamPaperDollAppliedGuid = 0;
	uint32 ExamPaperDollAppliedHash = 0;
	UPROPERTY()
	TObjectPtr<UBorder> PaperdollSlotsCheckboxIcon;
	UPROPERTY()
	TObjectPtr<UTextBlock> PaperdollSlotsCheckboxLabel;
	/** Clip shortcut overlays with the retail toolbar as its second row is revealed. */
	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> ShortcutClipPanel;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ShortcutIcons;
	/** Retail ItemSlot_Shortcut_01..10 numbered empty backgrounds (0x060010FA–102 / 0x060074CF). */
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ShortcutSlotBgs;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> ShortcutNumberIcons;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> ShortcutSelectedOverlays;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> ShortcutCooldownOverlays;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> ShortcutSlotNumbers;
	/** Retail spell slot backgrounds: blue numbered empty slots 1–9, brown afterward. */
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SpellBarSlotBgs;
	/** Retail spell-bar digit overlays (0x060019ED–F5 / 0x060019EC for 0). */
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SpellBarSlotNumIcons;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SpellBarSelectedOverlays;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SpellBarSlotNumbers;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SkillTabLabels;
	UPROPERTY()
	TObjectPtr<UTextBlock> PanelTitleLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderName;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderTitle;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderPk;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderLevelLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderLevel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderTotalXpLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderXp;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderXpToLevelLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrHeaderXpToLevel;
	UPROPERTY()
	TObjectPtr<UTextBlock> ChatTargetLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> ChatSendLabel;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextBlock>> ChatWindowLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> ChatTargetPopupRows;
	UPROPERTY() TArray<TObjectPtr<UBorder>> ChatTargetPopupRowBackgrounds;
	TArray<TSharedPtr<FACEUIElement>> ChatTargetPopupElements;
	UPROPERTY()
	TObjectPtr<UBorder> ChatTargetPopupBg;
	UPROPERTY()
	TObjectPtr<UTextBlock> InvBurdenBeginLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> InvBurdenValueLabel;
	/** Retail InvTitleText: "Inventory of %s". */
	UPROPERTY()
	TObjectPtr<UTextBlock> InvTitleLabel;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> EffectsListRows;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> EffectsListDurations;
	TArray<TSharedPtr<FACEUIElement>> EffectsRowElements;
	int32 EffectsContentCount = 0;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> EffectsListIcons;
	UPROPERTY()
	TArray<int32> EffectsListSpellIds;
	UPROPERTY()
	TObjectPtr<UTextBlock> EffectsInfoLabel;
	UPROPERTY() TObjectPtr<UScrollBox> EffectsInfoScroll;
	int32 EffectsInfoSpellId = 0;
	UPROPERTY()
	TObjectPtr<UTextBlock> EffectsTitleLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> VitaePanelLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> LinkStatusPanelLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExamValueLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExamBurdenLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExamInscriptionLabel;
	UPROPERTY() TObjectPtr<UTextBlock> ExamInscriptionSignature;
	UPROPERTY() TObjectPtr<UACERetailTextEntry> ExamInscriptionEditor;
	int32 EditingInscriptionGuid = 0;
	FString EditingInscriptionOriginal;
	bool CanEditInscription() const;
	void BeginInscriptionEdit();
	UFUNCTION() void HandleInscriptionCommitted(const FText& Text, ETextCommit::Type Method);
	UPROPERTY()
	TObjectPtr<UBorder> ExamIconBorder;
	int32 SelectedEffectsSpellId = 0;
	int32 EffectsScrollOffset = 0;
	bool bEffectsListPositive = true;
	FACELinkStatus LastLinkStatus;
	FACECharacterCreation LinkStatusStrings;
	bool bLoadedLinkStatusStrings = false;
	double LastLinkPingRequestAt = 0.0;
	double LastStatusIndicatorRefreshAt = 0.0;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SpellcastTabLabels;
	UPROPERTY()
	TObjectPtr<UTextBlock> CastSpellLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> SpellcastSpellNameLabel;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SpellBarIcons;
	UPROPERTY()
	TArray<int32> SpellBarSpellIds;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SpellbookRows;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SpellbookIcons;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SpellbookRowBackgrounds;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UBorder>> SpellbookRowSelections;
	UPROPERTY()
	TArray<int32> SpellbookRowIds;
	int32 SelectedSpellbookId = 0;
	bool bRevealLearnedSpell = false;
	/** One drawn line of the Components tab: a section heading or a component entry. */
	struct FACEComponentRow
	{
		bool bHeader = false;
		int32 Wcid = 0;
		int32 IconDid = 0;
		int32 Carried = 0;
		int32 Desired = 0;
		FString Name;
	};
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> ComponentNameLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> ComponentCountLabels;
	UPROPERTY()
	TArray<TObjectPtr<UACERetailTextEntry>> ComponentDesiredEntries;
	UPROPERTY() TArray<TObjectPtr<UBorder>> ComponentBackgrounds;
	UPROPERTY() TArray<TObjectPtr<UBorder>> ComponentEntryBackgrounds;
	UFUNCTION() void HandleComponentCommitted(int32 Wcid, const FText& Text, ETextCommit::Type Method);
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ComponentIcons;
	UPROPERTY()
	TArray<int32> ComponentRowWcids;
	int32 ComponentScrollOffset = 0;
	int32 SelectedComponentWcid = 0;
	/** Retail lists every component; hiding untracked ones keeps the panel usable early on. */
	bool bComponentsShowCarriedOnly = true;
	void BuildComponentRowList(TArray<FACEComponentRow>& OutRows) const;
	void RefreshComponentOverlays();
	void HideComponentOverlays();
	bool TryHandleComponentListClick(FVector2D CanvasLocalPos, bool bRightClick);
	int32 SpellbookScrollOffset = 0;
	/** Bitmasks: school 1..5 → bits 0..4; level 1..8 → bits 0..7. All on = show everything. */
	uint32 SpellbookSchoolFilterMask = 0x1Fu;
	uint32 SpellbookLevelFilterMask = 0xFFu;
	int32 SpellbookFilteredCount = 0;
	const TArray<int32>& FilteredSpellbook(class UACEDatSubsystem* Dat);
	TArray<int32> SpellbookKnownCache, SpellbookOrderCache;
	TWeakObjectPtr<class UACEDatSubsystem> SpellbookOrderDat;
	uint32 SpellbookOrderSchools = MAX_uint32, SpellbookOrderLevels = MAX_uint32;
	bool bSpellbookOrderDatReady = false;
	/** Brief pressed flash (light green DID) after a filter toggle. */
	FString SpellbookFilterPressedName;
	double SpellbookFilterPressedUntil = 0.0;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SpellbookChromeLabels;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SpellbookFilterCheckboxes;
	/** Open corpse/chest (ExternalContainer) / vendor / trade. */
	int32 OpenLootContainerGuid = 0;
	int32 OpenLootSelectedPackGuid = 0;
	/** Debounce CloseGroundContainer flicker at UseRadius edge. */
	int32 ExtItemScrollOffset = 0;
	int32 ExtPackScrollOffset = 0;
	int32 OpenVendorGuid = 0;
	/** 0=Items, 1=Buy, 2=Sell — persists across SyncEnvPanelMode ticks. */
	int32 ActiveVendorPage = 0;
	/** Horizontal scroll offsets for vendor item grids (per page). */
	int32 VendorItemsScrollOffset = 0;
	int32 VendorBuyScrollOffset = 0;
	int32 VendorSellScrollOffset = 0;
	/** After Use-on-vendor, add this inventory guid to the sell cart when ApproachVendor arrives. */
	int32 PendingVendorSellGuid = 0;
	int32 PendingVendorSellTargetGuid = 0;
	int32 PendingVendorSellAmount = INDEX_NONE;
	/** Retail offers a new server-created stack, never a partial quantity of the source GUID. */
	struct FInventoryStackSplit
	{
		int32 SourceGuid = 0, SourceContainerGuid = 0, ContainerGuid = 0, Wcid = 0;
		int32 Amount = 0, OriginalSize = 0;
		int64 WieldLocation = 0;
		bool bToWorld = false;
		double Deadline = 0;
		TSet<int32> ExistingGuids;
	};
	struct FVendorSellSplit : FInventoryStackSplit
	{
		int32 VendorGuid = 0;
		bool bSellWhenReady = false;
	};
	FVendorSellSplit VendorSellSplit;
	struct FTradeStackSplit : FInventoryStackSplit { int32 PartnerGuid = 0; };
	FTradeStackSplit TradeStackSplit;
	FInventoryStackSplit InventorySelectionSplit;
	void TrackInventoryStackSplit(FInventoryStackSplit& Split, const FACEWorldObject& Source, int32 Amount);
	int32 FindCompletedInventoryStackSplit(const FInventoryStackSplit& Split, bool bRequireOwned = true) const;
	void UpdateInventorySplitSelection();
	void AddInventoryGuidToTrade(int32 Guid, int32 Amount);
	void UpdateTradeStackSplit();
	int32 PendingTradeItemGuid = 0, PendingTradeItemAmount = 0, PendingTradeItemPartner = 0;
	double PendingTradeItemDeadline = 0;
	bool TryOpenTradeForDraggedItem(const FACEWorldObject& Target, int32 Guid, int32 Amount);
	int32 GetSelectedItemAmount(int32 Guid) const;
	void PickupInventoryAmount(int32 Guid, int32 Amount = 0);
	void MoveInventoryAmount(int32 Guid, int32 Container, int32 Placement, int32 Amount);
	bool MergeInventoryAmount(int32 Guid, int32 TargetGuid, int32 Amount);
	bool TryDropInExternalContainer(int32 Guid, int32 Amount, FVector2D Absolute);
	int32 ResolveInventoryDropPlacement(int32 Guid, int32 Amount, int32 Container, int32 TargetGuid) const;
	/** Dual-use: source item waiting for a target click (GameAction UseWithTarget). */
	int32 PendingUseWithSourceGuid = 0;
	TSet<int32> KeyboardOpenedCorpses;
	void PollAdditionalKeyboardActions(APlayerController* PC);
	void CycleKeyboardSelection(const FString& Kind, int32 Direction);
    UPROPERTY() TObjectPtr<UACERetailTextEntry> StackAmountEntry;
	int32 StackAmountEntryGuid = 0;
	UFUNCTION() void HandleStackAmountCommitted(const FText& Text, ETextCommit::Type Method);
	void RefreshStackAmountEntry(bool bShow);
	int32 ManaStoneConfirmSource = 0;
	int32 ExaminedSpellId = 0;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ExamSpellLabels;
	void RefreshSpellExamination();
	uint32 ServerConfirmType = 0, ServerConfirmContext = 0;
	FString ServerConfirmPrompt;
	TSharedPtr<FACEUIElement> ServerConfirmRoot;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ServerConfirmLabels;
	void RefreshServerConfirmation();
	void FinishServerConfirmation(bool bAccept);
	uint64 ServerPopupId = 0;
	FString ServerPopupPrompt;
	TSharedPtr<FACEUIElement> ServerPopupRoot;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ServerPopupLabels;
	void RefreshServerPopup();
	void FinishServerPopup(uint64 Id);
	int32 ManaStoneConfirmTarget = 0;
	TSharedPtr<FACEUIElement> ManaStoneConfirmRoot;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> ManaStoneConfirmLabels;
	void ShowManaStoneConfirmation(int32 Source, const FACEWorldObject& Target);
	void RefreshManaStoneConfirmation();
	void FinishManaStoneConfirmation(bool bAccept);
	/** Merchandise filter for Items page (0=All, then ItemType bit categories). */
	int32 VendorItemTypeFilter = 0;
	TArray<int32> VendorVisibleFilterIndices;
	bool bVendorFilterDropdownOpen = false;
	int32 VendorSelectedGuid = 0;
	int32 VendorSellSelectedGuid = 0;
	TArray<TPair<int32, int32>> VendorBuyCart;  // amount, objectId
	/** Items the player dragged onto the Sell page (not the whole pack). */
	TArray<TPair<int32, int32>> VendorSellCart;
	UPROPERTY()
	TObjectPtr<UBorder> VendorFilterDropdownBg;
	UPROPERTY()
	TObjectPtr<UTextBlock> VendorFilterText;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> VendorFilterDropdownLabels;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> VendorFilterDropdownRows;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> VendorTabLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> VendorButtonLabels;
	UPROPERTY()
	TObjectPtr<UTextBlock> VendorItemNameLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> VendorItemCostLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> VendorBuyCostLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> VendorBuyPurseLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> VendorSellCostLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> VendorSellPurseLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> SelectionStackAmountLabel;
	int32 TradePartnerGuid = 0;
	int32 TradeSelfOffset = 0;
	int32 TradeOtherOffset = 0;
	UPROPERTY() TArray<TObjectPtr<UBorder>> TradeSelfBackgrounds;
	UPROPERTY() TArray<TObjectPtr<UBorder>> TradeSelfSelections;
	UPROPERTY() TArray<TObjectPtr<UBorder>> TradeOtherSelections;
	UPROPERTY() TArray<TObjectPtr<UBorder>> TradeOtherBackgrounds;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> TradeLabels;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> TradeSelfCounts;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> TradeOtherCounts;
	bool bTradeOpen = false;
	EACEUIScrollTarget ScrollDragTarget = EACEUIScrollTarget::None;
	int32 ScrollDragMaxOff = 0;
	int32 ScrollDragTravel = 0;
	int32 ScrollDragTrackOrigin = 0;
	bool bScrollDragHorizontal = false;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ExtItemSlots;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ExtItemSlotBgs;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ExtItemSlotSelected;
	UPROPERTY()
	TArray<int32> ExtItemGuids;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ExtPackSlots;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ExtPackSlotBgs;
	UPROPERTY()
	TArray<int32> ExtPackGuids;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> VendorItemSlots;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> VendorItemSlotBgs;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> VendorItemSelections;
	UPROPERTY()
	TArray<int32> VendorItemGuids;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> TradeSelfSlots;
	UPROPERTY()
	TArray<int32> TradeSelfGuids;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> TradeOtherSlots;
	UPROPERTY()
	TArray<int32> TradeOtherGuids;
	/** Salvage env panel (Ust / CreateTinkeringTool). */
	int32 OpenSalvageToolGuid = 0;
	int32 SalvageMaterialType = 0;
	TArray<int32> SalvageQueueGuids;
	int32 SalvageScrollOffset = 0, SalvageVisibleSlots = 1;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> SalvageLabels;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> SalvageItemSelections;
	UPROPERTY(Transient) TArray<TObjectPtr<UBorder>> SalvageItemBackgrounds;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SalvageItemSlots;
	UPROPERTY()
	TArray<int32> SalvageItemSlotGuids;
	UPROPERTY()
	TObjectPtr<UTextBlock> SalvageWarningLabel;
	/** Options panel: GameplayOptionsPage / CharacterSettingsPage / ChatPage / ConfigPage. */
	FString ActiveOptionsTab = TEXT("GameplayOptionsPage");
	int32 OptionsScrollOffset = 0;
	/** Option bitfields as of panel open — restored by the Reset button. */
	uint32 OptionsSnapshot1 = 0;
	uint32 OptionsSnapshot2 = 0;
	uint32 OptionsDraft1 = 0, OptionsDraft2 = 0;
	uint64 MainChatFilterSnapshot = 0xFBFFFFFFull;
	TArray<uint64> FloatyChatFilterSnapshot;
	float ChatInactiveOpacity = .5f;
	float ChatActiveOpacity = 1.f;
	FVector2D ChatOpacitySnapshot = FVector2D(.5f, 1.f);
	UPROPERTY()
	TArray<TObjectPtr<USlider>> ChatOpacitySliders;
	UFUNCTION()
	void ChangeInactiveChatOpacity(float Value);
	UFUNCTION()
	void ChangeActiveChatOpacity(float Value);
	void RefreshChatOpacity();
	int32 GetOptionsRowCount() const;
	/** Retail PlayerOption index drawn on each visible option row. */
	TArray<int32> OptionRowOptions;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> OptionRowLabels;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> OptionRowCheckboxes;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> OptionsTabLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> OptionsButtonLabels;

	/** Social panel: AllegiancePage / FellowshipPage / FriendsPage. */
	FString ActiveSocialTab = TEXT("AllegiancePage");
	int32 SelectedFellowGuid = 0;
	int32 SelectedFriendGuid = 0;
	bool CanActivateFellowshipControl(const FString& Name) const;
	bool CanActivateAllegianceControl(const FString& Name, int32 TargetGuid = 0) const;
	void ShowAllegianceConfirmation(const FString& Action);
	FString PendingAllegianceAction, PendingAllegiancePrompt;
	int32 PendingAllegianceGuid = 0;
	int32 VassalScrollOffset = 0, VassalVisibleRows = 1;
	bool bSocialEntriesBound = false;
	UPROPERTY()
	TObjectPtr<UACERetailTextEntry> FellowshipNameEntry;
	UPROPERTY()
	TObjectPtr<UACERetailTextEntry> FriendNameEntry;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> FellowRows;
	UPROPERTY()
	TArray<int32> FellowRowGuids;
	TArray<TSharedPtr<FACEUIElement>> FellowRowElements;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> FellowDetailLabels;
	int32 FellowScrollOffset = 0;
	int32 FellowVisibleRows = 1;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> FriendRows;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> FriendStatusRows;
	TArray<TSharedPtr<FACEUIElement>> FriendRowElements;
	int32 FriendScrollOffset = 0;
	int32 FriendVisibleRows = 1;
	UPROPERTY()
	TArray<int32> FriendRowGuids;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> VassalRows;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> VassalXPRows;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> VassalStatusRows;
	TArray<TSharedPtr<FACEUIElement>> VassalRowElements;
	FACECharacterCreation SocialStrings;
	bool bLoadedSocialStrings = false;
	UPROPERTY()
	TArray<int32> VassalRowGuids;
	UPROPERTY()
	TObjectPtr<UACERetailTextEntry> SquelchNameEntry;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SquelchRows;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> SquelchStatusRows;
	TArray<TSharedPtr<FACEUIElement>> SquelchRowElements;
	int32 SquelchScrollOffset = 0;
	int32 SquelchVisibleRows = 1;
	UPROPERTY()
	TArray<int32> SquelchRowGuids;
	TArray<FString> SquelchRowNames;
	int32 SelectedSquelchGuid = 0;
	FString SelectedSquelchName;
	int32 SelectedVassalGuid = 0;
	UPROPERTY()
	TObjectPtr<UTextBlock> AllegianceXPLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> AllegiancePatronXPLabel;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> AllegianceCaptionLabels;
	/** Labels drawn on the social page's DAT buttons + checkboxes. */
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SocialButtonLabels;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SocialCheckboxIcons;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> QuestRows;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> QuestStatusRows;
	TArray<TSharedPtr<FACEUIElement>> ContractRowElements;
	UPROPERTY()
	TArray<int32> QuestRowIds;
	/** Quest panel detail-pane captions, one per ContractTable field. */
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> QuestDetailLabels;
	FString ActiveQuestTab = TEXT("ContractsPage");
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> QuestTabLabels;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> JournalLabels;
	TArray<TSharedPtr<FACEUIElement>> JournalRowElements;
	UPROPERTY() TArray<TObjectPtr<UACERetailTextEntry>> JournalEntries;
	UPROPERTY() TObjectPtr<UACERetailTextEntry> JournalNotes;
	struct FJournalPage { FString Label, Title, Notes, Location; int32 Days=0, Hours=0, Minutes=0; double TimerEnd=0; };
	TArray<FJournalPage> JournalPages;
	FString JournalFile;
	FString JournalStorageRoot;
	FString JournalSearch;
	int32 JournalPageIndex=0;
	int32 JournalScrollOffset=0;
	int32 JournalFilteredCount=0;
	FString JournalSortColumn=TEXT("PageSortButton");
	bool bJournalSortDescending=false;
	int32 LastJournalClickPage=-1;
	double LastJournalClickTime=0;
	bool bJournalFieldsDirty=true;
	bool bUpdatingJournal=false;
	int32 SelectedContractId = 0;
	int32 QuestScrollOffset = 0;
	bool bQuestSortByStatus = false;
	bool bQuestReverseSort = false;
	/** World panel (map / housing): active page, tab captions, and map overlays. */
	FString ActiveWorldTab;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> WorldTabLabels;
	UPROPERTY()
	TObjectPtr<UTextBlock> MapCoordinateLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> MapDateTimeLabel;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> HouseTextRows;
	TArray<TSharedPtr<FACEUIElement>> HouseTextElements;
	int32 HouseScrollOffset = 0;
	int32 HouseMaxScrollOffset = 0;
	/** Abuse / Urgent Assistance wizard page (1..3). */
	int32 AbuseWizardPage = 1;
	int32 UrgentWizardPage = 1;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> AbuseTextLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> AbuseButtonLabels;
	UPROPERTY()
	TObjectPtr<UEditableTextBox> AbuseNameEntry;
	UPROPERTY()
	TObjectPtr<UEditableTextBox> AbuseComplaintEntry;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> UrgentTextLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> UrgentButtonLabels;
	UPROPERTY()
	TObjectPtr<UEditableTextBox> UrgentComplaintEntry;
	UPROPERTY()
	TObjectPtr<UTextBlock> BookPageTextLabel;
	UPROPERTY()
	TObjectPtr<UScrollBox> BookScroll;
	UPROPERTY() TObjectPtr<UACERetailTextEntry> BookPageEditor;
	UFUNCTION() void HandleBookTextChanged(const FText& Text);
	int32 EditedBookGuid = 0, EditedBookPage = INDEX_NONE;
	int32 ScrolledBookGuid = 0, ScrolledBookPage = INDEX_NONE;
	UPROPERTY()
	TObjectPtr<UTextBlock> BookTitleLabel;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> MiniGameLabels;
	bool bDialogEntriesBound = false;

	// —— Chess (retail gmMiniGameUI + GameBoardGrid, CM_Game protocol) ——
	bool bChessActive = false;
	bool bChessStarted = false;
	bool bChessStalemateOffered = false;
	bool bChessQuitRequested = false;
	bool bChessQuitConfirm = false;
	void ResetChessGame();
	bool SubmitChessMove(int32 FromX, int32 FromY, int32 ToX, int32 ToY);
	int32 ChessBoardGuid = 0;
	/** ChessColor: 0 = white, 1 = black, -1 = not playing. */
	int32 ChessMyColor = -1;
	int32 ChessTurnColor = -1;
	/** Selected own piece (board coords), -1 = none. */
	int32 ChessSelX = -1;
	int32 ChessSelY = -1;
	/** Own move awaiting MoveResponse. */
	int32 ChessPendingFromX = -1;
	int32 ChessPendingFromY = -1;
	int32 ChessPendingToX = -1;
	int32 ChessPendingToY = -1;
	/** Logical board mirror: 0 empty, +white/-black; abs 1 P, 2 N, 3 B, 4 R, 5 Q, 6 K. */
	int8 ChessBoard[64] = {};
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> ChessSquareIcons;
	UPROPERTY() TArray<TObjectPtr<UBorder>> ChessSelectionIcons;
	void ResetChessBoard();
	/** Mirrors a confirmed move (capture / castle rook / en passant / promotion). */
	void ApplyChessMove(int32 FromX, int32 FromY, int32 ToX, int32 ToY);
	bool TryHandleChessBoardClick(FVector2D CanvasLocalPos);
	UPROPERTY()
	TObjectPtr<UTextBlock> AllegianceNameLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> MonarchNameLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> PatronNameLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> FellowshipTitleLabel;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SocialTabLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> AttributeRows;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> AttributeRowValues;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> AttributeRowIcons;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> AttributeRowHighlights;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrFooterTitle;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrFooterLine1Label;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrFooterLine1Value;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrFooterLine2Label;
	UPROPERTY()
	TObjectPtr<UTextBlock> AttrFooterLine2Value;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SkillRows;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SkillRowValues;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SkillRowIcons;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> SkillRowHighlights;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> SkillSectionHeaders;
	UPROPERTY()
	TArray<int32> SkillRowIds;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> TitleRows;
	TArray<TSharedPtr<FACEUIElement>> TitleRowElements;

	UPROPERTY()
	TObjectPtr<UTextBlock> RadarCoordsLabel;
	UPROPERTY()
	TObjectPtr<UBorder> RadarPlayerDot;
	UPROPERTY()
	TArray<TObjectPtr<UBorder>> RadarBlips;
	UPROPERTY()
	TArray<int32> RadarBlipGuids;
	static constexpr int32 MaxRadarBlips = 48;
	bool bRadarCompassAuthored = false;
	float RadarNorthMag = 0.f;
	float RadarEastMag = 0.f;
	float RadarSouthMag = 0.f;
	float RadarWestMag = 0.f;

	UPROPERTY()
	TObjectPtr<UTextBlock> CombatSpeedLabel;
	UPROPERTY()
	TObjectPtr<UTextBlock> CombatPowerLabel;
	UPROPERTY()
	TObjectPtr<UBorder> CombatPowerFill;
	UPROPERTY()
	TObjectPtr<UBorder> JumpChargeFrame;
	UPROPERTY()
	TObjectPtr<UBorder> JumpChargeFill;
	UPROPERTY()
	TObjectPtr<UTextBlock> JumpChargeLabel;
	UPROPERTY()
	TObjectPtr<UBorder> CombatCheckboxAutoRepeat;
	UPROPERTY()
	TObjectPtr<UBorder> CombatCheckboxAutoTarget;
	UPROPERTY()
	TObjectPtr<UBorder> CombatCheckboxViewTarget;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> CombatAttackLabels;
	UPROPERTY()
	TArray<TObjectPtr<UTextBlock>> CombatCheckboxLabels;

	/** Inventory item drag (DAT overlays). */
	int32 InvDragGuid = 0;
	/** Freeze the selected quantity at pickup; hovering/selection changes cannot change it. */
	int32 InvDragAmount = 0;
	int32 InvDragShortcutSlot = INDEX_NONE;
	bool bInvDragFromVendorSell = false;
	bool bInvDragFromSalvage = false;
	bool bInvDragFromHouse = false;
	int32 HitTestShortcutSlot(FVector2D CanvasLocalPos) const;
	int32 InvDragIconDid = 0;
	int32 InvDragSourcePack = 0;
	int32 InvDragSourceSlot = INDEX_NONE;
	/** When dragging a side pack for rearrange, source pack-slot index (1..N); 0 = not a pack drag. */
	int32 InvDragPackSlotIndex = INDEX_NONE;
	FVector2D InvDragStartLocal = FVector2D::ZeroVector;
	bool bInvDragPending = false;
	bool bInvDragActive = false;
	/**
	 * Second MouseDown within the double-click window — Use only if the press is released
	 * without becoming an active drag (so click-drag after select still reorders).
	 */
	bool bInvDoubleClickPending = false;
	UPROPERTY()
	TObjectPtr<UBorder> InvDragIcon;

	/** Spellbook / hotbar spell drag. */
	int32 SpellDragId = 0;
	int32 SpellDragIconDid = 0;
	int32 SpellDragSourceBarSlot = INDEX_NONE;
	int32 SpellDragSourceBar = INDEX_NONE;
	FVector2D SpellDragStartLocal = FVector2D::ZeroVector;
	bool bSpellDragPending = false;
	bool bSpellDragActive = false;
	UPROPERTY()
	TObjectPtr<UBorder> SpellDragIcon;
	UPROPERTY(Transient)
	TObjectPtr<UBorder> SpellDropMarker;

	int32 LastInvClickGuid = 0;
	double LastInvClickTime = 0.0;
	/** Spell hotbar: first click selects, second within window casts (retail). */
	int32 LastSpellClickSlot = INDEX_NONE;
	int32 LastSpellClickId = 0;
	double LastSpellClickTime = 0.0;
	/** Spellbook double-click appends a favorite; it never casts a spell. */
	int32 LastSpellbookClickId = 0;
	int32 LastSpellbookClickBar = INDEX_NONE;
	double LastSpellbookClickTime = 0.0;
	FVector2D LastSpellbookClickPosition = FVector2D::ZeroVector;

	bool bBound = false;

	void OnElementActivated(TSharedPtr<FACEUIElement> Element);
	bool HandleNamedClick(const FString& Name);

	void SetFloatyVisible(const FString& ElementName, bool bVisible);
	void ToggleFloaty(const FString& ElementName);
	void ShowPanelPage(const FString& PageElementName);
	void HidePanel();
	void TogglePanelPage(const FString& PageElementName);
	/** Clear floaty-root slab fills that double-paint under page backgrounds. */
	void SanitizeFloatyPanelChrome();
	void ShowExamination(bool bVisible);
	/** Graft Mag-nus ItemExamine chrome missing from floaty LayoutDesc; clear ghost slabs. */
	void EnsureExamineItemChrome();
	/** Graft Mag-nus creature/player examine frame (floaty ships solid 0x06004CC2). */
	void EnsureExamineCreatureChrome();
	void SyncSkillPanelTab(const FString& PageName);
	void SyncStatFooterChrome();
	void SyncSpellPanelTab(const FString& PageName);
	/** Swap PanelTabTemplate LeftEnd/middle/RightEnd art for selected vs idle. */
	void ApplyPanelTabChrome(const FString& TabElementName, bool bSelected);
	/** Spellcast bar tabs use GenericSpellcastTab active/inactive DIDs. */
	void SyncSpellcastTabChrome();
	/**
	 * Retail Raise / Raise×10 triangles: lit green (0x06004CB6 / 0x0600712B) when the
	 * selected stat can be raised/trained; darker (0x06004CB7 / 0x0600712C) when not.
	 */
	void SyncRaiseButtonChrome(bool bCanRaise1, bool bCanRaise10);
	const TCHAR* GetStatSelectionFooter() const;
	void SyncChatScrollbar(int32 Window = 0);
	/** Show/hide + place ChatLogNewNonVisibleTextIndicator; jump-to-bottom on click. */
	void SyncChatJumpIndicator(int32 Window = 0);
	/** Parse @/ slash commands (@ls, @tell, @e, …). Returns true if handled (do not Talk). */
	bool TryDispatchMiscCommand(const FString& Cmd, const FString& Args);
	bool TryDispatchRetailUtilityCommand(const FString& Cmd, const FString& Args);
	uint32 PendingChatConfirmation = 0;
	int32 ChatConfirmationStage = 0;
	bool TryDispatchUILayoutCommand(const FString& Cmd, const FString& Args);
	void UpdateAutoUILayout();
	bool GetAutoUILayoutPath(FString& Path) const;
	TWeakPtr<FACESession> AutoLayoutSession;
	int32 AutoLayoutPlayer = 0;
	FIntPoint AutoLayoutSize = FIntPoint::ZeroValue;
	bool bAutoLayoutVR = false;
	void FillVendorComponents(const FString& Args);
	TSet<FString> ActiveChatFiles;
	bool TryDispatchChatCommand(const FString& Message, UEditableTextBox* Entry = nullptr, bool* bClearEntry = nullptr);
	void ToggleChatTargetPopup();
	void CloseChatTargetPopup();
	void RefreshChatTargetPopup();
	bool TryHandleChatTargetPopupClick(FVector2D Absolute);
	void SetChatSendChannel(int32 ChannelIndex);
	void CycleChatFilterMode();
	bool PassesChatFilter(int32 ChatType) const;
	/** Retail @clear — empties one chat window's log (0 = main, 1..4 = floaty). */
	void ClearChatLog(int32 Window = 0);
	/** Entry / log widget for a chat window (0 = main, 1..4 = floaty; null if unbuilt). */
	UEditableTextBox* GetChatEntryWidget(int32 Window) const;
	UScrollBox* GetChatLogWidget(int32 Window) const;
	void FocusChatEntryWindow(int32 Window);
	/** Append an already-formatted line to one window's log. */
	void AppendChatLineToLog(int32 Window, const FString& Line, const FLinearColor& Color,
		const FString& ClickSender);
	/** Place FloatyChat1-4 logs / entries / titles on their DAT elements. */
	void PlaceFloatyChatOverlays();
	/** Click-to-tell: chat row hit → prefill "@tell Name, " (retail Tell tag click). */
	bool TryHandleChatNameClick(FVector2D Absolute);
	void SaveFloatyChatSettings();
	void LoadFloatyChatSettings();
	void SendFloatyChatCommitted(int32 Window, const FText& Text, ETextCommit::Type CommitMethod);
	/** Retail LogTextType names for @filter / @unfilter / @messagetypes. */
	static bool ChatTypeFromName(const FString& Name, int32& OutType);
	static FString ChatTypeNames();
	static FString ChatSendChannelLabel(int32 ChannelIndex);
	static uint32 ChatSendChannelId(int32 ChannelIndex);
	static bool ChatSendIsTurbine(int32 ChannelIndex);
	static uint32 ChatSendTurbineChatType(int32 ChannelIndex);
	void AppendLocalChatLine(const FString& Line, int32 ChatType);
	void SendPoseChat(const FString& Command, FString MyEmote, FString OtherEmote);
	/** Position DAT scrollbar up/down/thumb; Frac 0=top .. 1=bottom. VisibleFrac sizes the thumb. */
	void SyncDatScrollbar(const TSharedPtr<FACEUIElement>& Bar, float Frac, float VisibleFrac = -1.f);
	void LoadVitalDisplayPreference();
	void SaveVitalDisplayPreference() const;
	void SyncInventoryScrollbars();
	void SyncStatListScrollbar();
	/** Grow ThreeDItemsField / item list / pack list when FloatyPanel is taller than authored. */
	void ReflowInventoryPanelGeometry();
	/** Stretch StatManagement list/footer to fill the floaty panel height. */
	/** Fit Social fellowship chrome (authored ~600px) into the floaty (~362px). */
	/** Stretch spell list / filter box to fill SpellManagementPanel. */
	void SyncSpellbookScrollbar();
	void SyncSpellbookFilterCheckboxes();
	bool ToggleSpellbookFilter(const FString& Name);
	void ShowExternalContainer(int32 Guid);
	void HideExternalContainer(bool bNotifyServer);
	void ShowVendorPanel(int32 Guid);
	void HideVendorPanel();
	void SetVendorPage(int32 PageIndex);
	void ToggleVendorFilterDropdown();
	void SetVendorItemTypeFilter(int32 FilterIndex);
	void CloseVendorFilterDropdown();
	void RefreshVendorFilterDropdown();
	bool TryHandleVendorFilterDropdownClick(FVector2D Absolute);
	void BuySelectedVendorItem();
	int32 GetVendorPurchaseLimit(int32 ItemGuid) const;
	int32 GetVendorSelectionLimit(int32 ItemGuid) const;
	void AddSelectedVendorItemToBuyCart();
	void BuyVendorCartItem();
	void BuyVendorCart();
	bool SubmitVendorPurchase(const TArray<TPair<int32,int32>>& Items);
	void SellVendorCart(bool bSelectedOnly = false);
	bool DropInventoryInWorld(int32 Guid, int32 Amount, const FVector2D& Absolute);
	void AddInventoryGuidToVendorSellCart(int32 Guid, int32 Amount = INDEX_NONE);
	bool CanStageVendorSellItem(int32 Guid) const;
	bool CanMoveInventoryAmountToContainer(int32 Guid, int32 Container, int32 Amount, int32 MergeTarget = 0) const;
	void UpdateInventoryContainerDropFeedback(FVector2D CanvasLocalPos);
	void UpdateVendorSellSplit();
	void SyncVendorPageVisibility();
	void RefreshVendorTabLabels();
	void RefreshVendorButtonLabels();
	void RefreshVendorInfoTexts();
	void HideVendorTextOverlays();
	void PlaceVendorFilterCaption();
	int32 CountPlayerPyreals() const;
	void SyncVendorScrollbars();
	FString VendorFilterLabel() const;
	bool VendorItemMatchesFilter(const FACEWorldObject& O, int32 FilterIndex) const;
	void RebuildVendorVisibleFilters();
	static constexpr int32 VendorFilterCount = 18;
	void ShowTradePanel(int32 PartnerGuid);
	void HideTradePanel(bool bNotifyServer);
	void SyncEnvPanelMode();
	/** Close vendor/loot when the player walks past UseRadius (server vendor only emotes). */
	void TickEnvPanelRangeChecks();
	bool IsBeyondContainerUseRadius(int32 ContainerGuid) const;
	void RefreshExternalContainerOverlays();
	void RefreshVendorOverlays();
	void RefreshTradeOverlays();
	void ShowSalvagePanel(int32 ToolGuid);
	void HideSalvagePanel();
	void RefreshSalvageOverlays();
	bool AddItemToSalvageQueue(int32 Guid);
	bool CanAddItemToSalvageQueue(int32 Guid) const;
	void SetSalvageScrollOffset(int32 Offset);
	void RemoveItemFromSalvageQueue(int32 Guid);
	void SubmitSalvageQueue();
	/** Options panel (0x2100002B pages inlined under OptionsPanel_Field). */
	void SyncOptionsPanelTab(const FString& PageName);
	void RefreshOptionsOverlays();
	void HideOptionsOverlays();
	bool HandleOptionsNamedClick(const FString& Name);
	bool TryHandleOptionsListClick(FVector2D CanvasLocalPos);
	void ToggleCharacterOption(int32 Option);
	void SyncOptionsScrollbar();
	/** Element name of the list box on the active options tab (empty on the gameplay tab). */
	FString ActiveOptionsListName() const;
	int32 ActiveOptionsPage() const;
	void SyncSocialPanelTab(const FString& PageName);
	void RefreshSocialOverlays();
	void RefreshAllegianceOverlays();
	void RefreshFellowshipOverlays();
	void RefreshFriendsOverlays();
	void RefreshSquelchOverlays();
	/** Labels + option checkboxes on the social page buttons (Swear/Kick/Tell/Add/…). */
	void RefreshSocialButtonLabels();
	void RefreshQuestOverlays();
	void RefreshJournalOverlays();
	bool HandleJournalNamedClick(const FString& Name);
	void SaveJournal();
	UFUNCTION() void HandleJournalEdited(const FText& Text);
	/** World panel: Map / Housing tabs (classic_world 0x21000027). */
	void SyncWorldPanelTab(const FString& PageName);
	void RefreshWorldOverlays();
	void RefreshWorldMapPage(const TSharedPtr<FACEUIElement>& Page);
	void RefreshWorldHousePage(const TSharedPtr<FACEUIElement>& Page);
	void RefreshHouseOffer();
	void RefreshBarber();
	UPROPERTY() TObjectPtr<UACEUICharGenBinder> Barber;
	uint64 SeenBarberRevision=0;
	void CloseHouseOffer();
	bool HandleHouseControl(const TSharedPtr<FACEUIElement>& Element);
	bool HitHouseItemList(FVector2D Local) const;
	bool TryHouseItemClick(FVector2D Local);
	void StageHouseItem(int32 Guid, int32 Amount);
	int32 OpenHouseLord = 0, HouseItemOffset = 0, SelectedHouseItem = 0;
	uint64 SeenHouseRevision = 0;
	bool bHouseRentTab = false, bHousePaymentConfirm = false;
	FString HouseConfirmPrompt;
	FInventoryStackSplit HousePaymentSplit;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> HouseOfferLabels;
	UPROPERTY() TArray<TObjectPtr<UBorder>> HouseOfferSlots;
	UPROPERTY() TArray<TObjectPtr<UBorder>> HouseOfferBackgrounds;
	UPROPERTY() TArray<TObjectPtr<UBorder>> HouseOfferSelections;
	void HideWorldOverlays();
	bool HandleWorldNamedClick(const FString& Name);
	/** Abuse / Urgent Assistance multi-page wizards + Book + MiniGame chrome. */
	void SyncAbusePanelPage(int32 PageIndex);
	void SyncUrgentPanelPage(int32 PageIndex);
	void RefreshAbuseOverlays();
	void RefreshUrgentOverlays();
	void RefreshBookOverlays();
	TSharedPtr<FACEUIElement> GetBookScrollbar() const;
	void RefreshMiniGameOverlays();
	void HideDialogOverlays();
	bool HandleDialogNamedClick(const FString& Name);
	void EnsureDialogEntryBoxes();
	void RequestBookPageIfNeeded(int32 PageIndex);
	/** Retail map coords for a position: "12.3N, 45.6W", or empty when indoors. */
	static FString FormatMapCoords(const FACEPosition& Pos, bool bIncludeInteriors = false);
	/** Derethian calendar string from raw PortalYearTicks (hour phased via SkyDesc). */
	FString FormatDerethDateTime(double GameTicks) const;
	void EnsureSocialEntryBoxes();
	int32 GetExternalContainerItemCapacity() const;
	void SyncExternalContainerScrollbars();
	void RefreshAttributeOverlays();
	void RefreshSkillOverlays();
	int32 FindUpperEquippedItem(int64 Mask) const;
	int32 ResolvePaperDollUseTarget(const FString& SlotName, int64 Mask) const;
	bool IsUseTargetCompatible(int32 SourceGuid, int32 TargetGuid) const;
	void RefreshTitleOverlays();
	void RaiseSelectedStat(int32 Multiplier);
	void DropInventoryAmount(int32 Guid,int32 Amount);
	void PlaceTextUnder(UTextBlock* Text, const FString& AncestorName, const FString& ElementName,
		const FString& Contents, int32 FontSize, const FLinearColor& Color, int32 ZOrder);
	void RefreshSpellHotbarOverlays();
	void RefreshSpellbookOverlays();
	void RefreshSpellbookChromeLabels();
	void RefreshChatChromeOverlays();
	void RefreshShortcutOverlays();
	void RefreshSkillTabLabels();
	/** Roman numerals on Spellcast_Tab1..8 — the DAT tab art carries no text layer. */
	void RefreshSpellcastTabLabels();
	void RefreshPanelTitleOverlay();
	void CastSelectedHotbarSpell();
	void ApplyCombatMode(int32 Mode);
	void ApplyCombatModeInternal(int32 Mode, bool bSendToServer);
	void SyncCombatModeButtons();
	void SyncCombatAmmoCount();
	UPROPERTY() TObjectPtr<UTextBlock> CombatAmmoLabel;
	void SyncInventoryButtonVisual();
	void RefreshCombatPanelOverlays();
	void TickCombatAutoAttack(float DeltaSeconds);
	bool IsCombatPoseReady() const;
	void FireCombatAttack();
	void BeginCombatPowerCharge(uint32 AttackHeight, bool bHeld = false);
	void ReleaseCombatPowerCharge();
	float GetCombatPowerChargeDuration() const;
	void TryAutoTargetOnCombatEnter(int32 ExcludeGuid = 0);
	int32 PendingAutoTargetLoss = 0;
	void SyncExamineBodyVisibility(bool bCreature);
	bool HasEquippedCaster() const;
	bool HasEquippedMissileWeapon() const;
	void RefreshInventoryBurdenOverlays();
	/** Spawn (once) the off-map capture rig that renders the character for the PaperDoll viewport. */
	bool EnsurePaperDollPreviewRig();
	/** Place / refresh the paperdoll render into the DAT PaperDoll element rect. */
	void RefreshPaperDollPreview();
	void ReleasePaperDollPreview();
	bool EnsureExamPaperDollPreviewRig();
	void RefreshExamPaperDollPreview();
	void ReleaseExamPaperDollPreview();
	bool ApplyPreviewCaptureAppearance(AActor* PreviewActor, USceneCaptureComponent2D* Capture,
		const FACEWorldObject& Obj, float WorldScale);
	void RefreshEffectsOverlays(bool bPositive);
	void RefreshVitaePanelOverlays();
	void RefreshLinkStatusPanelOverlays();
	void RefreshStatusIndicators();
	void TickStatusPanels(float DeltaSeconds);
	bool TryHandleEffectsListClick(FVector2D Absolute);
	static FString FormatEnchantmentRemaining(const FACEActiveEnchantment& E);
	static int32 VitaeXpRemaining(float VitaeMul, int32 Level, int32 CpPool);

	void EnsureOverlays();
	void RefreshVitalsOverlays();
	void RefreshRadarOverlays();
	void RefreshSelectionOverlay();
	void EnsureSelectionMarkers();
	void HideSelectionMarkers();
	void PlaceSelectionCorner(UBorder* Corner, int32 TexDid, float ScreenX, float ScreenY,
		const FLinearColor& Tint, int32 ZOrder);
	static FLinearColor ColorFromSelectionMarker(uint8 RadarColor);
	void RefreshExaminationOverlay();
	void RefreshInventoryOverlays();
	uint64 HashInventoryOverlayState() const;
	uint64 HashVendorOverlayState() const;
	uint64 HashLootOverlayState() const;
	void RefreshPanelBodyText();
	void PlaceTextOnElement(UTextBlock* Text, const FString& ElementName, const FString& Contents,
		int32 FontSize, const FLinearColor& Color, int32 ZOrder, bool bCenterInElement = false);
	/** Same, for element names that repeat across pages and must be resolved by scope. */
	void PlaceTextOnElement(UTextBlock* Text, TSharedPtr<FACEUIElement> Element,
		const FString& Contents, int32 FontSize, const FLinearColor& Color, int32 ZOrder,
		bool bCenterInElement = false);
	void PlaceRadarWidget(UWidget* Widget, float ScreenX, float ScreenY, float Size, int32 ZOrder);
	static FLinearColor ColorFromRadarBlip(uint8 RadarColor);
	static bool ShouldShowOnRadar(const FACEWorldObject& Obj, int32 SelfGuid);
	static uint8 ResolveRadarColor(const FACEWorldObject& Obj, const FACEFellowshipInfo* Fellowship = nullptr);
	UBorder* EnsureIconBorder(TArray<TObjectPtr<UBorder>>& Array, int32 Index);
	void SetIconDid(UBorder* Border, int32 IconDid, FLinearColor Tint = FLinearColor::White);
	void SetSpellIcon(UBorder* Border, int32 SpellId);
	/** Empty slot chrome, IconUnderlay, or UiEffects-tinted cell behind an inventory icon. */
	void SetItemSlotBackground(UBorder* Border, const FACEWorldObject* Item);
	void SetItemSlotForeground(UBorder* Border, const FACEWorldObject* Item);

	void CancelInventoryDrag();
	void CancelSpellDrag();
	bool HitTestSpellBarSlot(FVector2D Absolute, int32& OutSlotIndex) const;
	bool HitTestSpellbookRow(FVector2D Absolute, int32& OutSpellId) const;
	void UseInventoryItem(int32 Guid);
	bool ShouldPreserveShortcutSelection(int32 Guid) const;
	/** Prefer empty dual jewelry slots; armor uses full ValidLocations. */
	int64 ResolveWieldLocation(const FACEWorldObject& Obj, int64 PreferredSlotMask = 0) const;
	/** Retail client unequips conflicts, then GetAndWield (server does not auto-swap). */
	void UnequipConflictsAndWield(int32 Guid, const FACEWorldObject& Obj, int64 Loc);
	/** Resize combat floaty chrome + internal backgrounds to ContentW (600 melee / 800 magic). */
	void FitCombatFloatyContentWidth(int32 ContentW);
	void ExpandCombatFloatyForWideContent();
	void ExpandEnvFloatyForWideContent();
	bool HitTestInventorySlot(FVector2D Absolute, int32& OutGuid, int32& OutSlotIndex) const;
	bool HitTestPackSlot(FVector2D Absolute, int32& OutPackGuid, bool IncludeEmpty = false) const;
	bool HitTestDollSlot(FVector2D Absolute, FString& OutSlotName, int64& OutMask) const;
	void PostInventorySystemMessage(const FString& Text);
	/** Show the retail-style yellow center-top transient banner. */
	void ShowTransientInfo(const FString& Message);
	void TickTransientInfo();

	int32 ResolveEquippedCombatMode() const;
	void UseSelectedObject();
	void ExamineSelectedObject();
	void UseShortcutSlot(int32 SlotIndex1Based);
	void SelectInventoryGuid(int32 Guid);
};
