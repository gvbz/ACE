#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ACETypes.h"
#include "ACEOpcodes.h"
#include "ACEFrameDiagnostics.h"
#include "ACEClientSubsystem.generated.h"

class FACESession;

struct FACEBufferedChatMessage
{
	FString Text, Sender;
	int32 Type = 0;
	int32 FilterType = INDEX_NONE;
	uint8 ForcedWindows = 0;
	TOptional<FLinearColor> Color;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACESessionStateChangedDyn, EACESessionState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FACECharacterListDyn, const TArray<FACECharacterInfo>&, Characters, const FString&, ServerName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FACEEnteredWorldDyn, int32, PlayerGuid, const FACEPosition&, SpawnPosition);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEPlayerTeleportStartedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FACEPositionUpdateDyn, int32, ObjectGuid, const FACEPosition&, Position);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FACEMotionUpdateDyn, int32, ObjectGuid, const FACEObjectMotionState&, Motion);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FACEVectorUpdateDyn, int32, ObjectGuid, FVector, AceVelocity, FVector, AceOmega);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEObjectCreatedDyn, const FACEWorldObject&, Object);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEObjectDeletedDyn, int32, ObjectGuid);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FACEPhysicsStateUpdateDyn, int32, ObjectGuid, int32, PhysicsState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FACEPkStatusUpdatedDyn, int32, ObjectGuid, int32, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FACESoundDyn, int32, ObjectGuid, int32, SoundType, float, Volume);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FACEPlayScriptIdDyn, int32, ObjectGuid, int32, PhysicsScriptId, float, Intensity);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FACEPlayEffectDyn, int32, ObjectGuid, int32, ScriptType, float, Intensity);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FACEChatMessageDyn, const FString&, Text, const FString&, Sender, int32, ChatType);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FACERoutedChatMessageDyn, const FString&, Text, const FString&, Sender, int32, ChatType, int32, FilterType);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEVitalsUpdatedDyn, const FACEPlayerVitals&, Vitals);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FACEObjectHealthDyn, int32, ObjectGuid, float, HealthFraction);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEAppraisalDyn, const FACEAppraisalInfo&, Appraisal);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACESelectionChangedDyn, const FACESelectedObject&, Selection);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACELogMessageDyn, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEExternalContainerOpenedDyn, int32, Guid);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEExternalContainerClosedDyn, int32, Guid);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEVendorOpenedDyn, int32, Guid);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACETradeStateChangedDyn, int32, EventType);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACECharacterTitlesChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEFellowshipChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEAllegianceChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEFriendsChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACEChessEventDyn, const FACEChessEvent&, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEContractsChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACESquelchChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEHouseChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEBookChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FACEEnchantmentsChangedDyn);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACESpellLearnedDyn, int32, SpellId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FACELinkStatusChangedDyn, const FACELinkStatus&, Status);

/**
 * GameInstance subsystem — main Blueprint entry for ACE login / world / movement.
 */
UCLASS()
class ACECLIENT_API UACEClientSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	friend class FACEPortalLifetimeTest;
	friend class FACEFoundryTest;
	friend class FACEUIWorkloadTest;
	friend class FACEPluginEquipmentTest;
	friend class FACEPluginRequestsTest;
	friend class FACEEquipmentAssessmentCacheTest;
	friend class FACEUCMLootLatencyTest;
	friend class FACEUCMLootNotificationTest;
    friend class FACEPluginCompanionTest;
    friend class FACEPluginWorldPlateTest;
    friend class FACEThirdEyeTest;
    friend class FACEPluginPublicAPITest;
	friend class FACEItemToolsTest;
    friend class FACEAutoCraftTest;
    friend class FACEPluginRampRouteTest;
	GENERATED_BODY()
	friend class FACEVRRenderReplicationTest;
	friend class FACEVRRigTest;
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
	friend class FACEChatParityTest;
	friend class FACEArenaRecallTest;
	friend class FACERetailCommandAuditTest;
	friend class FACEUILayoutCommandsTest;
	friend class FACEUIInteractionParityTest;
	friend class FACEVendorTransactionsTest;
	friend class FACERetailSpellComponentsTest;
	friend class FACEPanelResizeSocialTest;
	friend class FACEMovementReviewTest;

	friend class FACERetailScreenTest;
	friend class FACECameraEdgeTest;
    friend class FACERetailCharacterCreationTest;
    friend class FACERetailCharacterCreationScreenTest;
	friend class FACERetailWorldEntryTest;

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UACEClientSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return !IsTemplate(); }
	virtual bool IsTickableInEditor() const override { return false; }

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACESessionStateChangedDyn OnSessionStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACECharacterListDyn OnCharacterList;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEEnteredWorldDyn OnEnteredWorld;

	/** Retail enter-portal-space cue (GameMessagePlayerTeleport 0xF751). */
	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEPlayerTeleportStartedDyn OnPlayerTeleportStarted;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEPositionUpdateDyn OnPositionUpdate;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEMotionUpdateDyn OnMotionUpdate;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEVectorUpdateDyn OnVectorUpdate;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEObjectCreatedDyn OnObjectCreated;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEObjectDeletedDyn OnObjectDeleted;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEPhysicsStateUpdateDyn OnPhysicsStateUpdate;
	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEPkStatusUpdatedDyn OnPkStatusUpdated;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Effects")
	FACESoundDyn OnSound;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Effects")
	FACEPlayScriptIdDyn OnPlayScriptId;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Effects")
	FACEPlayEffectDyn OnPlayEffect;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEChatMessageDyn OnChatMessage;
	// UI routing is independent of the wire type/color retained for plugin observers.
	UPROPERTY()
	FACERoutedChatMessageDyn OnRoutedChatMessage;

	// Retain server messages received before the gameplay HUD exists (world entry).
	const TArray<FACEBufferedChatMessage>& GetChatHistory() const { return ChatHistory; }
	void ReceiveChatMessage(const FString& Text, const FString& Sender, int32 Type, int32 FilterType = INDEX_NONE, uint8 ForcedWindows = 0, TOptional<FLinearColor> Color = {});
	uint8 CurrentChatWindows = 0;
	TOptional<FLinearColor> CurrentChatColor;
	TArray<FACEBufferedChatMessage> ChatHistory;


	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEVitalsUpdatedDyn OnVitalsUpdated;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEObjectHealthDyn OnObjectHealth;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACEAppraisalDyn OnAppraisal;
	/** All appraisal responses, including background plugin requests. */
	UPROPERTY() FACEAppraisalDyn OnAppraisalObserved;
	void RequestBackgroundAppraisal(int32 Guid);
	TSet<int32> BackgroundAppraisals;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACESelectionChangedDyn OnSelectionChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACELogMessageDyn OnLogMessage;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Inventory")
	FACEExternalContainerOpenedDyn OnExternalContainerOpened;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Inventory")
	FACEExternalContainerClosedDyn OnExternalContainerClosed;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Vendor")
	FACEVendorOpenedDyn OnVendorOpened;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Trade")
	FACETradeStateChangedDyn OnTradeStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|UI")
	FACEChessEventDyn OnChessEvent;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACECharacterTitlesChangedDyn OnCharacterTitlesChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Social")
	FACEFellowshipChangedDyn OnFellowshipChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Social")
	FACEAllegianceChangedDyn OnAllegianceChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Social")
	FACEFriendsChangedDyn OnFriendsChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Social")
	FACEContractsChangedDyn OnContractsChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Social")
	FACESquelchChangedDyn OnSquelchChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|World")
	FACEHouseChangedDyn OnHouseChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|UI")
	FACEBookChangedDyn OnBookChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Magic")
	FACEEnchantmentsChangedDyn OnEnchantmentsChanged;

	UPROPERTY(BlueprintAssignable, Category = "ACE|Magic")
	FACESpellLearnedDyn OnSpellLearned;

	UPROPERTY(BlueprintAssignable, Category = "ACE")
	FACELinkStatusChangedDyn OnLinkStatusChanged;

	UFUNCTION(BlueprintCallable, Category = "ACE")
	bool Login(const FString& Host, int32 Port, const FString& Account, const FString& Password, bool bGDLE = false);

	UFUNCTION(BlueprintCallable, Category = "ACE")
	void Logout();

	UFUNCTION(BlueprintCallable, Category = "ACE")
	bool EnterWorld(int32 CharacterId);

	UFUNCTION(BlueprintCallable, Category = "ACE")
	bool EnterWorldByName(const FString& CharacterName);

	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendMovement(float Forward, float Strafe, float Turn, bool bRunning);

	/** Same as SendMovement, with StandingLongJump and ground-contact bits. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendMovementEx(float Forward, float Strafe, float Turn, bool bRunning, bool bStandingLongJump, bool bContact, bool bUniformVRMovement = false, bool bMouseTurn = false);

	/** GameAction Jump (0xF61B) — Extent 0..1, velocity in ACE local space. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendJump(float Extent, const FVector& LocalAceVelocity);

	UFUNCTION(BlueprintCallable, Category = "ACE")
	void StopMovement();

	/** GameAction Use — ACE.Server approaches and activates the object. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendUseItem(int32 ObjectGuid);

	/** GameAction ChangeCombatMode — 1=peace, 2=melee, 4=missile, 8=magic. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendChangeCombatMode(int32 CombatMode);

	UFUNCTION(BlueprintCallable, Category = "ACE|Combat")
	void SendTargetedMeleeAttack(int32 TargetGuid, int32 AttackHeight, float PowerLevel);

	UFUNCTION(BlueprintCallable, Category = "ACE|Combat")
	void SendTargetedMissileAttack(int32 TargetGuid, int32 AttackHeight, float AccuracyLevel);

	/** GameAction Talk — local chat / server @commands. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendChatMessage(const FString& Message);

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendChatChannel(int32 ChannelId, const FString& Message);

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendTell(const FString& TargetName, const FString& Message);

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendTalkDirect(int32 TargetGuid, const FString& Message);

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendSoulEmote(const FString& EmoteText);

	/** Retail *pose* motion via MoveToState CommandList (paired with SendSoulEmote text). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendSoulEmoteMotion(int32 MotionCommand);

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendTeleToLifestone();

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendTeleToMarketplace();

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendTeleToHouse();

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendTeleToMansion();

	UFUNCTION(BlueprintCallable, Category = "ACE|Chat")
	void SendRecallAllegianceHometown();

	UFUNCTION(BlueprintPure, Category = "ACE|Chat")
	int32 GetLastTellSenderGuid() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Chat")
	FString GetLastTellSenderName() const;

	/** Select a world object (client-side + QueryHealth). Guid 0 clears. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SelectObject(int32 ObjectGuid);
	bool IsWorldObjectVisible(const FACEWorldObject& Object) const;

	/** GameAction IdentifyObject — appraisal / inspect panel. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendIdentifyObject(int32 ObjectGuid);
	uint64 GetIdentifyRequestSerial() const { return IdentifyRequestSerial; }
	int32 GetIdentifyRequestGuid() const { return IdentifyRequestGuid; }
private:
	uint64 IdentifyRequestSerial = 0;
	int32 IdentifyRequestGuid = 0;
public:
	void SendSetInscription(int32 ObjectGuid, const FString& Text);

	/** Spend AvailableExperience on Strength..Self (AttributeId 1..6). */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendRaiseAttribute(int32 AttributeId, int32 XpAmount);

	/** Spend AvailableExperience on Health/Stamina/Mana (VitalId 1/3/5). */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendRaiseVital(int32 VitalId, int32 XpAmount);

	/** Spend AvailableExperience on a trained/specialized skill. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendRaiseSkill(int32 SkillId, int32 XpAmount);

	/** Spend skill credits to train an untrained skill. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendTrainSkill(int32 SkillId, int32 CreditsSpent);

	/** Retail *emote → GameAction Emote (0x01DF). */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendEmote(const FString& EmoteText);

	/** Drag & drop: move an item into a pack/container at Placement (0 = front). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendPutItemInContainer(int32 ItemGuid, int32 ContainerGuid, int32 Placement);

	/** Merge stackable items (GameAction 0x0054). Amount of MergeFromGuid into MergeToGuid. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendStackableMerge(int32 MergeFromGuid, int32 MergeToGuid, int32 Amount);

	/** Split a stack into a new stack in a container (GameAction 0x0055). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendStackableSplitToContainer(int32 StackGuid, int32 ContainerGuid, int32 Placement, int32 Amount);

	/** Split a stack onto the ground (GameAction 0x0056). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendStackableSplitTo3D(int32 StackGuid, int32 Amount);

	/** Split a stack onto a wield location (GameAction 0x019B). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendStackableSplitToWield(int32 StackGuid, int64 WieldLocation, int32 Amount);

	/** Dual-use: source item with target (GameAction 0x0035). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendUseWithTarget(int32 SourceGuid, int32 TargetGuid);

	/** Set display character title (GameAction 0x002C). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Character")
	void SendSetTitle(int32 TitleId);

	/** Character options (retail PlayerOption index). */
	UFUNCTION(BlueprintPure, Category = "ACE|Character")
	bool IsCharacterOptionSet(int32 Option) const;

	/** Toggle one character option (GameAction 0x0005). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Character")
	void SendSetSingleCharacterOption(int32 Option, bool bValue);

	/** Push both option bitfields (GameAction 0x01A1) — Apply / Reset / Default. */
	void SendCharacterOptions(uint32 Options1, uint32 Options2);
	uint32 GetCharacterOptions1() const;
	uint32 GetCharacterOptions2() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Character")
	TArray<int32> GetCharacterTitleIds() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Character")
	int32 GetDisplayTitleId() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Inventory")
	bool IsUseBusy() const;

	/** The world view is in a portal transition; interface browsing remains available. */
	bool IsInPortalSpace() const;

	/** Drag & drop: equip an item onto the paper doll (EquipMask location). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendGetAndWieldItem(int32 ItemGuid, int64 WieldLocation);

	/** Drag & drop: drop an item on the ground. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendDropItem(int32 ItemGuid);

	/** Give item to a player or NPC (GameAction 0x00CD). Amount defaults to full stack when 0. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendGiveObjectRequest(int32 TargetGuid, int32 ItemGuid, int32 Amount = 0);

	/** Close an external corpse/chest viewing session. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendNoLongerViewingContents(int32 ContainerGuid);

	/** Buy from vendor — AmountAndObjectId pairs are (amount, objectID). */
	void SendBuyItems(int32 VendorGuid, const TArray<TPair<int32, int32>>& AmountAndObjectId);

	/** Sell to vendor — AmountAndObjectId pairs are (amount, objectID). */
	void SendSellItems(int32 VendorGuid, const TArray<TPair<int32, int32>>& AmountAndObjectId);

	/** Open trade negotiations with another player. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Trade")
	void SendOpenTrade(int32 OtherPlayerGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|Trade")
	void SendCloseTrade();

	/** TradeSlotSide 0 = self side of the trade window. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Trade")
	void SendAddToTrade(int32 ItemGuid, int32 TradeSlotSide = 0);

	UFUNCTION(BlueprintCallable, Category = "ACE|Trade")
	void SendAcceptTrade();

	UFUNCTION(BlueprintCallable, Category = "ACE|Trade")
	void SendDeclineTrade();

	UFUNCTION(BlueprintCallable, Category = "ACE|Trade")
	void SendResetTrade();

	UFUNCTION(BlueprintPure, Category = "ACE|Inventory")
	int32 GetOpenExternalContainerGuid() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Vendor")
	int32 GetOpenVendorGuid() const;

	/** Merchandise from ApproachVendor (icons/names). Prefer over GetPackItems for vendors. */
	UFUNCTION(BlueprintPure, Category = "ACE|Vendor")
	TArray<FACEWorldObject> GetVendorMerchandise() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Vendor")
	float GetVendorBuyRate() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Vendor")
	float GetVendorSellRate() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Trade")
	int32 GetTradePartnerGuid() const;
	int32 GetTradeAcceptedGuid() const;
	bool HasAcceptedTrade() const;
	bool HasPartnerAcceptedTrade() const;

	/** Item guids currently offered on our side of the trade window. */
	UFUNCTION(BlueprintPure, Category = "ACE|Trade")
	TArray<int32> GetTradeSelfItems() const;

	/** Item guids currently offered on the partner's side of the trade window. */
	UFUNCTION(BlueprintPure, Category = "ACE|Trade")
	TArray<int32> GetTradePartnerItems() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Social")
	FACEFellowshipInfo GetFellowship() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Social")
	FACEAllegianceInfo GetAllegiance() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Social")
	TArray<FACEFriendInfo> GetFriends() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Social")
	TArray<FACEContractEntry> GetContracts() const;

	/** Drop a contract (GameAction 0x0316). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendAbandonContract(int32 ContractId);

	/** Squelch list (SetSquelchDB). */
	UFUNCTION(BlueprintPure, Category = "ACE|Social")
	TArray<FACESquelchEntry> GetSquelches() const;

	/** Squelch/unsquelch one character (GameAction 0x0058). MessageType 0 = all channels. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendModifyCharacterSquelch(bool bSquelch, int32 PlayerGuid, const FString& PlayerName,
		int32 MessageType);

	/** Squelch/unsquelch a whole account by character name (GameAction 0x0059). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendModifyAccountSquelch(bool bSquelch, const FString& PlayerName);

	/** Housing panel state (HouseData / HouseStatus). */
	UFUNCTION(BlueprintPure, Category = "ACE|World")
	FACEHouseInfo GetHouseInfo() const;

	/** Ask the server to (re)send the housing panel payload — GameAction 0x021E. */
	UFUNCTION(BlueprintCallable, Category = "ACE|World")
	void SendHouseQuery();

	/** Open book panel state (BookDataResponse / BookPageDataResponse). */
	UFUNCTION(BlueprintPure, Category = "ACE|UI")
	FACEBookInfo GetBookInfo() const;

	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void ClearBook();

	/** Report abuse — GameAction 0x0140. */
	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendAbuseLogRequest(const FString& CharacterName, int32 StatusMask, const FString& Complaint);

	/** Request book TOC / a specific page — GameActions 0x00AA / 0x00AE. */
	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendBookData(int32 BookGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendBookPageData(int32 BookGuid, int32 PageIndex);

	/** Chess resign / pass / stalemate offer. */
	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendChessQuit();

	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendChessMovePass();

	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendChessStalemate(bool bStalemate);

	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendChessJoin(int32 BoardGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|UI")
	void SendChessMove(int32 FromX, int32 FromY, int32 ToX, int32 ToY);

	/** Fill component book: component wcid → quantity to rebuy at a vendor. */
	UFUNCTION(BlueprintPure, Category = "ACE|Spells")
	TMap<int32, int32> GetDesiredComponents() const;

	/** Set (or clear, with Amount 0) one fill component entry — GameAction 0x0224. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Spells")
	void SendSetDesiredComponentLevel(int32 ComponentWcid, int32 Amount);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendFellowshipCreate(const FString& Name, bool bShareXP = true);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendFellowshipQuit(bool bDisband = false);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendFellowshipDismiss(int32 MemberGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendFellowshipRecruit(int32 PlayerGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendFellowshipUpdateRequest(bool bPanelOpen);
	void SetVRFellowshipUpdates(bool bEnabled);
	void SetPluginFellowshipUpdates(bool bEnabled);
	bool bRetailFellowshipUpdates = false;
	bool bVRFellowshipUpdates = false;
	bool bPluginFellowshipUpdates = false;

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendFellowshipAssignNewLeader(int32 MemberGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendFellowshipChangeOpenness(bool bOpen);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendSwearAllegiance(int32 TargetGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendBreakAllegiance(int32 TargetGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendAllegianceUpdateRequest(bool bPanelOpen);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendAddFriend(const FString& Name);

	UFUNCTION(BlueprintCallable, Category = "ACE|Social")
	void SendRemoveFriend(int32 FriendGuid);

	/** Salvage with Ust — CreateTinkeringTool (0x027D). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Crafting")
	void SendCreateTinkeringTool(int32 ToolGuid, const TArray<int32>& ItemGuids);

	/** Drag & drop: place a known spell on a spell bar slot. */
	UFUNCTION(BlueprintCallable, Category = "ACE|Magic")
	void SendAddSpellToBar(int32 SpellId, int32 SlotIndex, int32 BarIndex);

	UFUNCTION(BlueprintCallable, Category = "ACE|Magic")
	void SendRemoveSpellFromBar(int32 SpellId, int32 BarIndex);

	/** Drag inventory item onto a toolbar shortcut slot (0–17). */
	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendAddShortcut(int32 SlotIndex, int32 ObjectGuid);

	UFUNCTION(BlueprintCallable, Category = "ACE|Inventory")
	void SendRemoveShortcut(int32 SlotIndex);

	/**
	 * Cast a spell using SpellTable targeting rules.
	 * Untargeted (NonComponentTargetType==None) → CastUntargetedSpell.
	 * SelfTargeted (or name ends with " Self") → CastTargetedSpell at the local player
	 * without requiring the player to click themselves first.
	 * Otherwise requires a selected target; returns false when missing.
	 * When CasterItemGuid != 0 (wand BuiltInSpell), retail uses UseWithTarget(item, target)
	 * so the server treats it as a weapon spell (mana from the item, no spellbook check).
	 */
	UFUNCTION(BlueprintCallable, Category = "ACE|Magic")
	bool SendCastSpell(int32 SpellId, int32 CasterItemGuid = 0);
	/** Shared retail target eligibility, used by UI, keyboard and VR trigger casting. */
	bool ResolveSpellCastTarget(int32 SpellId, int32 SelectedGuid, int32& OutTarget, bool bAllowFreeAim = false) const;

	UFUNCTION(BlueprintPure, Category = "ACE|Magic")
	TArray<int32> GetKnownSpells() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Magic")
	TArray<int32> GetSpellBar(int32 BarIndex) const;

	UFUNCTION(BlueprintCallable, Category = "ACE|Magic")
	void SetActiveSpellBar(int32 BarIndex);

	UFUNCTION(BlueprintPure, Category = "ACE|Magic")
	int32 GetActiveSpellBar() const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	int32 GetShortcutObject(int32 SlotIndex) const;

	/** Local player attributes + vitals (valid after PlayerDescription arrives). */
	UFUNCTION(BlueprintPure, Category = "ACE")
	FACEPlayerVitals GetPlayerVitals() const;
	/** Read-only native view; valid until session replacement. Do not retain across callbacks. */
	const FACEPlayerVitals& GetPlayerVitalsView() const;

	/** True when PropertyInt.EncumbranceVal has been received for the local player. */
	bool TryGetPlayerEncumbrance(int32& OutEncumbrance) const;

	UFUNCTION(BlueprintPure, Category = "ACE|Magic")
	TArray<FACEActiveEnchantment> GetActiveEnchantments(bool bIncludeCooldowns = false) const;

	UFUNCTION(BlueprintPure, Category = "ACE|Magic")
	bool TryGetVitaeMultiplier(float& OutMultiplier) const;

	UFUNCTION(BlueprintPure, Category = "ACE|Magic")
	int32 GetVitaeCpPool() const;

	UFUNCTION(BlueprintPure, Category = "ACE|Magic")
	int32 GetDeathLevel() const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	FACELinkStatus GetLinkStatus() const;

	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SendPingRequest();

	UFUNCTION(BlueprintPure, Category = "ACE")
	FACESelectedObject GetSelectedObject() const;

	int32 GetPreviousSelectedObjectGuid() const;
	int32 GetGiveRecipientGuid() const;
	void SelectInventoryObject(int32 ObjectGuid);

	/** Send LoginComplete (0x00A1) — tell the server we exited portal space / arrived. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void NotifyExitedPortalSpace();

	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SetReportedPosition(const FACEPosition& Position);

	/** Keep AutonomousPosition flowing while standing still (Use approach hold). */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void SetForcePositionReporting(bool bForce);

	/** Send one AutonomousPosition now so the server has our pose before a follow-up Use. */
	UFUNCTION(BlueprintCallable, Category = "ACE")
	void FlushAutonomousPosition(bool bContact = true);

	UFUNCTION(BlueprintPure, Category = "ACE")
	EACESessionState GetSessionState() const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	TArray<FACECharacterInfo> GetCharacters() const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	FString GetServerName() const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	int32 GetPlayerGuid() const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	FACEPosition GetPlayerPosition() const;

	/** Current TeleportSeq from the last server UpdatePosition (portal / forced moves). */
	UFUNCTION(BlueprintPure, Category = "ACE")
	int32 GetTeleportSeq() const;

	/**
	 * Fraction [0,1) of the current Dereth day (7620s retail). GetSky uses
	 * Fmod(Ticks + ZeroTimeOfYear, DayLength) / DayLength so Dawnsong lines up with retail.
	 */
	UFUNCTION(BlueprintPure, Category = "ACE")
	float GetGameDayFraction(float DefaultIfUnknown = 0.5f) const;

	/** floor((Ticks + ZeroTimeOfYear) / DayLength) — rebuilds GameSky when the Dereth day rolls. */
	UFUNCTION(BlueprintPure, Category = "ACE")
	int32 GetGameDayNumber() const;

	/** Raw PortalYearTicks (Dereth seconds since Morningthaw 1, 10 P.Y.); 0 when unknown. */
	UFUNCTION(BlueprintPure, Category = "ACE")
	double GetGameTimeTicks() const;
	bool HasGameTime() const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	bool GetWorldObject(int32 Guid, FACEWorldObject& OutObject) const;

	UFUNCTION(BlueprintPure, Category = "ACE")
	TArray<FACEWorldObject> GetWorldObjects() const;

	/** Equipped items on the local player (wielded location set). */
	UFUNCTION(BlueprintPure, Category = "ACE|Inventory")
	TArray<FACEWorldObject> GetEquippedItems() const;

	/** Items inside a pack/container (ViewContents order when known). PlayerGuid = main pack. */
	UFUNCTION(BlueprintPure, Category = "ACE|Inventory")
	TArray<FACEWorldObject> GetPackItems(int32 ContainerGuid) const;

	/** Drop deferred ViewContents cache when loot UI closes. */
	void ClearContainerContents(int32 ContainerGuid);

	/** Top-level backpacks/side packs on the player. */
	UFUNCTION(BlueprintPure, Category = "ACE|Inventory")
	TArray<FACEWorldObject> GetPlayerPacks() const;

	/** Retail pickup preference/fallback; explicit inventory drops do not use this.
	 * Amount == 0 means the whole stack. An existing mergeable stack takes priority. */
	int32 ResolvePickupContainer(int32 ItemGuid, int32 OpenPackGuid, int32 Amount = 0) const;
	/** Same retail search, but returns zero when no owned container can accept the item. */
	int32 FindPickupContainer(int32 ItemGuid, int32 OpenPackGuid, int32 Amount = 0) const;

	/** F/hand: merge into and select one compatible stack, or move to main pack if none fits. */
	bool SortInventoryItem(int32 Guid);

	/** Live load/capacity used by both locomotion speed and jump eligibility. */
	float GetMovementBurden() const;
	/** ACE MovementSystem.GetRunRate — used for client prediction speed. */
	UFUNCTION(BlueprintPure, Category = "ACE|Movement")
	float GetRunRate() const;

	/** Unscaled locomotion speed in AC units/sec. Ground prediction applies creature scale separately. */
	UFUNCTION(BlueprintPure, Category = "ACE|Movement")
	float GetLocomotionSpeed(bool bRunning) const;

	/**
	 * MotionInterp sidestep after adjust_motion: base 1.25 × (0.5×3.12/1.25),
	 * run multiplies by GetRunRate and clamps |rate| ≤ 3 → max ≈ 3.75 AC/s.
	 */
	UFUNCTION(BlueprintPure, Category = "ACE|Movement")
	float GetSidestepSpeed(bool bRunning) const;

	UFUNCTION(BlueprintCallable, Category = "ACE|Movement")
	void SetRunSkill(int32 InRunSkill) { RunSkill = FMath::Max(0, InRunSkill); }

	UFUNCTION(BlueprintCallable, Category = "ACE|Movement")
	void SetJumpSkill(int32 InJumpSkill) { JumpSkill = FMath::Max(0, InJumpSkill); }

	UFUNCTION(BlueprintPure, Category = "ACE|Movement")
	int32 GetJumpSkill() const { return JumpSkill; }

	/** ACE MovementSystem.GetJumpHeight — returns AC height units for Extent 0..1. */
	UFUNCTION(BlueprintPure, Category = "ACE|Movement")
	float GetJumpHeight(float Extent) const;

	UFUNCTION(BlueprintCallable, Category = "ACE|Movement")
	void SetBurden(float InBurden) { Burden = FMath::Max(0.f, InBurden); }

	TSharedPtr<FACESession> GetSession() const { return Session; }

	/** Retail UI framework (800x600 canvas). No gm*UI panels until LayoutDesc inflater is ready. */
	UFUNCTION(BlueprintPure, Category = "ACE|UI")
	class UACEUIElementManager* GetUIElementManager() const { return UIElementManager; }

	UFUNCTION(BlueprintPure, Category = "ACE|UI")
	class UACEUIFlow* GetUIFlow() const { return UIFlow; }

	UFUNCTION(BlueprintPure, Category = "ACE|UI")
	class UACEUILayoutResolver* GetUILayoutResolver() const { return UILayoutResolver; }

	UFUNCTION(BlueprintPure, Category = "ACE|UI")
	class UACEUIResourceResolver* GetUIResourceResolver() const { return UIResourceResolver; }

	/** True for items held by the player, including items inside nested packs. */
	bool IsOwnedInventoryItem(const FACEWorldObject& Object) const;
private:
	void TickInventorySort(float DeltaTime);
	FACEFrameReportGate FrameReportGate;
	double LastFrameReportTime = 0;
	void TickWorldObjectVisibility(float DeltaTime);
	float VisibilityUpdateSeconds = 0.f;
	uint32 VisibilityPlayerCell = 0;
	bool bVisibilitySeesOutside = false;
	TSet<int32> VisibilityIndoorCells;
	TMap<uint32, bool> VisibilityCellSeenOutside;
	int32 SortSourceGuid=0, SortPlayerGuid=0, SortMergeTargetGuid=0;
	int32 SortExpectedSource=0, SortExpectedTarget=0;
	float SortWaitSeconds=0.f;

	TSharedPtr<FACESession> Session;

	UPROPERTY()
	TObjectPtr<class UACEUIElementManager> UIElementManager;

	UPROPERTY()
	TObjectPtr<class UACEUIFlow> UIFlow;

	UPROPERTY()
	TObjectPtr<class UACEUILayoutResolver> UILayoutResolver;

	UPROPERTY()
	TObjectPtr<class UACEUIResourceResolver> UIResourceResolver;

	/** Preview defaults, replaced by PlayerDescription and subsequent skill updates. */
	int32 RunSkill = 200;
	int32 JumpSkill = 200;
	float Burden = 0.f;
};
