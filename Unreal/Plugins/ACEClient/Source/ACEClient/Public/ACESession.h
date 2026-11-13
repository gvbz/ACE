#pragma once
#include "ACEBarber.h"

#include "CoreMinimal.h"
#include "ACECharacterCreation.h"
#include "ACEOpcodes.h"
#include "ACETypes.h"
#include "VR/ACEVRPose.h"
#include "Protocol/ACEIsaac.h"
#include "Protocol/ACEBinaryWriter.h"
#include "Protocol/ACEBinaryReader.h"
#include "Containers/Queue.h"

class FSocket;

struct FACEConfirmation
{
	uint32 Type = 0;
	uint32 Context = 0;
	FString Prompt;
};

struct FACEServerPopup
{
	uint64 Id = 0;
	FString Message;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnSessionStateChanged, EACESessionState);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnCharacterList, const TArray<FACECharacterInfo>&, const FString& /*ServerName*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnEnteredWorld, int32 /*PlayerGuid*/, const FACEPosition& /*SpawnPos*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnPositionUpdate, int32 /*ObjectGuid*/, const FACEPosition&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnMotionUpdate, int32 /*ObjectGuid*/, const FACEObjectMotionState&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FACEOnVectorUpdate, int32 /*ObjectGuid*/, const FVector& /*AceVelocity*/, const FVector& /*AceOmega*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnObjectCreated, const FACEWorldObject&);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnObjectDeleted, int32 /*ObjectGuid*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnPhysicsStateUpdate, int32 /*ObjectGuid*/, int32 /*PhysicsState*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FACEOnSound, int32 /*ObjectGuid*/, int32 /*SoundType*/, float /*Volume*/);
DECLARE_MULTICAST_DELEGATE(FACEOnPlayerTeleportStarted);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FACEOnPlayScriptId, int32 /*ObjectGuid*/, int32 /*PhysicsScriptId*/, float /*Intensity*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FACEOnPlayEffect, int32 /*ObjectGuid*/, int32 /*ScriptType*/, float /*Intensity*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FACEOnChatMessage, const FString& /*Text*/, const FString& /*Sender*/, int32 /*Type*/);
DECLARE_MULTICAST_DELEGATE_FourParams(FACEOnRoutedChatMessage, const FString&, const FString&, int32 /*Type*/, int32 /*FilterType*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FACEOnPlayerTell, const FString&, const FString&, int32);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnVitalsUpdated, const FACEPlayerVitals&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnObjectHealth, int32 /*ObjectGuid*/, float /*HealthFraction*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnAppraisal, const FACEAppraisalInfo&);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnSelectionChanged, const FACESelectedObject&);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnLogMessage, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnViewContentsExternal, int32 /*ContainerGuid*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnCloseGroundContainer, int32 /*ContainerGuid*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnApproachVendor, int32 /*VendorGuid*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnTradeEvent, int32 /*EventType*/);
DECLARE_MULTICAST_DELEGATE(FACEOnCharacterTitlesChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnUseDone, uint32 /*WeenieError*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnSpellCastRequested, int32 /*SpellId*/, int32 /*TargetGuid*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnItemUseRequested, int32 /*SourceGuid*/, int32 /*TargetGuid*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnMoveToFailed, uint32 /*WeenieError*/);
DECLARE_MULTICAST_DELEGATE(FACEOnFellowshipChanged);
DECLARE_MULTICAST_DELEGATE(FACEOnAllegianceChanged);
DECLARE_MULTICAST_DELEGATE(FACEOnFriendsChanged);
DECLARE_MULTICAST_DELEGATE(FACEOnContractsChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnChessEvent, const FACEChessEvent&);
DECLARE_MULTICAST_DELEGATE(FACEOnSquelchChanged);
DECLARE_MULTICAST_DELEGATE(FACEOnHouseChanged);
DECLARE_MULTICAST_DELEGATE(FACEOnBookChanged);
DECLARE_MULTICAST_DELEGATE(FACEOnEnchantmentsChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnSpellLearned, int32 /*SpellId*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FACEOnLinkStatusChanged, const FACELinkStatus&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FACEOnNPCSpeech, int32, const FString&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FACEOnHealthFeedback, int32, int32, uint32);
DECLARE_MULTICAST_DELEGATE_FourParams(FACEOnCombatFeedback, const FString&, int32, bool, bool);

/**
 * Retail-compatible ACE UDP session (client version "1802").
 * Ports: C2S = Port, S2C = Port+1 after ConnectResponse.
 */
class ACECLIENT_API FACESession : public TSharedFromThis<FACESession>
{
    friend class FACEPluginWorldPlateTest;
    friend class FACEFoundryTest;
	friend class FACERetailLogoutTest;
	friend class FACECollisionSizingTest;
	friend class FACEPvPCombatParityTest;
	friend class FACEEmoteTransitionTest;
	friend class FACEPlayableOlthoiTest;
	friend class FACEVRProtocolTest;
	friend class FACEAmmoAttachmentTest;
	friend class FACEPortalLifetimeTest;
	friend class FACEDesktopHighlightTest;
	friend class FACEPluginEquipmentTest;
	friend class FACEPluginRequestsTest;
	friend class FACEEquipmentAssessmentCacheTest;
	friend class FACEAppraisalCommandContextTest;
	friend class FACEVendorTransactionsTest;
	friend class FACETradeParityTest;
	friend class FACESocialNetworkParityTest;
	friend class FACEBookContainerParityTest;
	friend class FACEBookWritingTest;
	friend class FACEContractNetworkParityTest;
	friend class FACEEnchantmentNetworkParityTest;
	friend class FACEHousingNetworkParityTest;
	friend class FACEServerDialogsTest;
	friend class FACEUCMLootLatencyTest;
	friend class FACEUCMLootNotificationTest;
	friend class FACEPluginLootIconsTest;
	friend class FACEWeaponSkillLootTest;
    friend class FACEPluginCompanionTest;
    friend class FACEThirdEyeTest;
    friend class FACEPluginPublicAPITest;
	friend class FACEItemToolsTest;
    friend class FACEAutoCraftTest;
    friend class FACELedgerTest;
    friend class FACEPluginRampRouteTest;
	friend class FACEDeferredWorldAppearanceTest;
	friend class FACEVRObserverProtocolTest;
	friend class FACEVRLocomotionTest;
	friend class FACEInteractionRecoveryTest;
	friend class FACEVRRenderReplicationTest;
	friend class FACEVRRigTest;
    friend class FACEMobileTouchTest;
	friend class FACESpellDataRevisionTest;
	friend class FACESalvageTest;
	friend class FACEHousingTest;
	friend class FACEChessTest;
	friend class FACEBarberTest;
	friend class FACEItemPresentationTest;
	friend class FACESelectionToolbarTest;
    friend class FACEActionDeckTest;
    friend class FACERetailCursorTest;
	friend class FACEWorldLootPlacementTest;
	friend class FACEChatParityTest;
	friend class FACEArenaRecallTest;
	friend class FACERetailCommandAuditTest;
	friend class FACECombatChatTest;
	friend class FACEPlayerModuleParityTest;
	friend class FACEUILayoutCommandsTest;
	friend class FACEUIInteractionParityTest;
	friend class FACERetailSpellComponentsTest;
	friend class FACEPanelResizeSocialTest;
	friend class FACEMovementReviewTest;
	friend class FACERemoteMovementReplayTest;
	friend class FACERemoteTurnTargetsTest;
	friend class FACEAvatarMotionTest;
	friend class FACEEmoteTest;
	friend class FACEVRInteriorTest;
	friend class FACEEntranceTransitionTest;
	friend class FACEVRWallContactTest;
    friend class FACEAcademyCornerTest;
	friend class FACEVRStairCeilingTest;
	friend class FACEVRInteriorNetworkTest;
	friend class FACEPerformanceScene;
	friend class FACEWaspLagLink;
	friend class FACEWaspNetworkLagTest;
	friend class FACEDungeonInterfacesTest;
	friend class FACENetworkPumpBudgetTest;
	friend class FACERetailInteriorStreamingTest;
	friend class FACERetailCombatProtocolTest;
	friend class FACERetailNetworkTest;
	friend class FACENetworkAnimationParityTest;
	friend class FACEAttackHeightMotionTest;
	friend class FACEGroundItemPlacementTest;
	friend class FACECustomObjectReplicationTest;
	friend class FACEInitialRestingMotionTest;
	friend class FACETailoringLayersTest;
	friend class FACECustomWorldPresentationTest;
	friend class FACEHouseHookAnimationTest;
	friend class FACELoginHandshakeTest;
	friend class FACEGDLEInteractionTransportTest;
	friend class FACERetailNetworkWeatherTest;
	friend class FACERetailWorldEntryTest;
	friend class FACEPortalDropTest;
	friend class FACEPortalMotionTest;
	friend class FACELoadingTransitionTest;
	friend class FACEPortalInterfaceTest;
	friend class FACELedgeStairsTest;
	friend class FACELedgeSafetyTest;
	friend class FACESlopeContactTest;
	friend class FACEMansionEdgesTest;
	friend class FACEFortTethStairsTest;
	friend class FACEBuildingPassageTest;
	friend class FACERunSpeedParityTest;
	friend class FACEWalkingStepTest;
	friend class FACERetailPkStatusTest;
	friend class FACERetailScreenTest;
	friend struct FACERetailVendorUIFixture;
	friend class FACECharacterInfoParityTest;
	friend class FACECameraEdgeTest;
	friend class FACERetailParticleTimingTest;
	friend class FACERetailStatsTest;
	friend class FACEPrivateStatParityTest;
	friend class FACELivePropertyUpdatesTest;
	friend class FACEQualityRemovalTest;
	friend class FACEAttackerIdentityTest;
	friend class FACERetailAugmentationStatsTest;
	friend class FACEAugmentationTest;
	friend class FACERetailInventoryOrderTest;
	friend class FACEClosedContainerLifetimeTest;
	friend class FACEEquippedLookupTest;
	friend class FACEInventoryUICostTest;
	friend class FACEUIWorkloadTest;
	friend class FACEUCMIdleWorkloadTest;

public:
	FACESession();
	~FACESession();

	FACEOnSessionStateChanged OnStateChanged;
	FACEOnCharacterList OnCharacterList;
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCharacterCreated, uint32, const FACECharacterInfo&);
	FOnCharacterCreated OnCharacterCreated;
	bool CreateCharacter(const FACECGSelection& Selection);
	bool DeleteCharacter(int32 CharacterId);
	bool RestoreCharacter(int32 CharacterId);
	bool IsCharacterManagementPending() const { return PendingCharacterMutation != 0; }
	const FString& GetCharacterManagementError() const { return CharacterManagementError; }
	bool IsCharacterCreationPending() const { return bCharacterCreationPending; }
	int32 GetCharacterSlotCount() const { return CharacterSlotCount; }
	FACEOnEnteredWorld OnEnteredWorld;
	FACEOnPositionUpdate OnPositionUpdate;
	FACEOnMotionUpdate OnMotionUpdate;
	FACEOnVectorUpdate OnVectorUpdate;
	FACEOnObjectCreated OnObjectCreated;
	FACEOnObjectDeleted OnObjectDeleted;
	FACEOnPhysicsStateUpdate OnPhysicsStateUpdate;
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPkStatusUpdated, int32 /*Guid*/, int32 /*Status*/);
	FOnPkStatusUpdated OnPkStatusUpdated;
	FACEOnSound OnSound;
	/** Retail "enter portal space" cue — GameMessagePlayerTeleport 0xF751. */
	FACEOnPlayerTeleportStarted OnPlayerTeleportStarted;
	FACEOnPlayScriptId OnPlayScriptId;
	FACEOnPlayEffect OnPlayEffect;
	FACEOnChatMessage OnChatMessage;
	FACEOnRoutedChatMessage OnRoutedChatMessage;
	void EmitChatMessage(const FString& Text, const FString& Sender, int32 Type, int32 FilterType = INDEX_NONE);
	FACEOnPlayerTell OnPlayerTell;
	FACEOnVitalsUpdated OnVitalsUpdated;
	TFunction<void(FACEPlayerVitals&, const TArray<FACEActiveEnchantment>&)> StatResolver;
	void NotifyVitalsChanged();
	FACEOnObjectHealth OnObjectHealth;
	FACEOnAppraisal OnAppraisal;
	FACEOnSelectionChanged OnSelectionChanged;
	FACEOnLogMessage OnLog;
	FACEOnViewContentsExternal OnViewContentsExternal;
	FACEOnCloseGroundContainer OnCloseGroundContainer;
	FACEOnApproachVendor OnApproachVendor;
	FACEOnTradeEvent OnTradeEvent;
	FACEOnCharacterTitlesChanged OnCharacterTitlesChanged;
	FACEOnUseDone OnUseDone;
	FACEOnSpellCastRequested OnSpellCastRequested;
	FACEOnItemUseRequested OnItemUseRequested;
	FACEOnMoveToFailed OnMoveToFailed;
	/** Shared local/server approach failure path: cancel movement and show the retail error. */
	void ReportMoveToFailure(uint32 Error);
	FACEOnFellowshipChanged OnFellowshipChanged;
	FACEOnAllegianceChanged OnAllegianceChanged;
	FACEOnFriendsChanged OnFriendsChanged;
	FACEOnContractsChanged OnContractsChanged;
	FACEOnChessEvent OnChessEvent;
	FACEOnSquelchChanged OnSquelchChanged;
	FACEOnHouseChanged OnHouseChanged;
	FACEOnBookChanged OnBookChanged;
	FACEOnEnchantmentsChanged OnEnchantmentsChanged;
	FACEOnSpellLearned OnSpellLearned;
	FACEOnLinkStatusChanged OnLinkStatusChanged;
	FACEOnNPCSpeech OnNPCSpeech;
	FACEOnCombatFeedback OnCombatFeedback;
	FACEOnHealthFeedback OnHealthFeedback;
	bool SupportsHealthFeedback() const { return (VRCapabilities & 64u) != 0; }
	bool IsNearbyHealthObject(int32 Guid) const;
	bool SupportsVRRecovery() const { return (VRCapabilities & 2048u) != 0; }
	bool SupportsVRCasting() const { return (VRCapabilities & 4096u) != 0; }
	uint32 GetVRCastPhase() const;
	float GetVRCastRemaining() const;
	float GetVRCastProgress() const;
	void SendVRAim(uint32 Cell, int32 Weapon, const FVector& Origin, const FVector& Direction);
	void SetVRMeleeBody(const FVector& Body) { VRMeleeBody = Body; }
	float GetVRRecoveryRemaining() const;
	float GetVRRecoveryProgress() const;

	bool Connect(const FACELoginCredentials& Credentials);
	void Disconnect();
	/**
	 * Clean character logoff: sends CharacterLogOff (0xF653) while in-world so the
	 * server releases the account/character, then returns to CharacterSelect when
	 * the CharacterList arrives. If not in-world, closes the session.
	 */
	void RequestLogOff();
	/** True while waiting for CharacterList after CharacterLogOff. */
	bool IsLogOffPending() const { return bLogOffPending; }
	/** Retail logoff presentation starts 3 seconds after requesting (23 for PK/PKLite). */
	double GetLogOffPortalStartTime() const { return LogOffPortalStartTime; }
	void Tick(float DeltaSeconds);
	/** Retail CObjectMaint grace period outside cell/PVS interest, independent of the camera. */
	void MaintainWorldObjectVisibility(double Now, TFunctionRef<bool(int32)> IsCellInPVS);
	const TArray<FACEConfirmation>& GetConfirmations() const { return Confirmations; }
	const TArray<FACEServerPopup>& GetServerPopups() const { return ServerPopups; }
	/** Informational server messages close locally; they are not confirmation replies. */
	bool DismissServerPopup(uint64 Id);
	bool RespondToConfirmation(uint32 Type, uint32 Context, bool Accept);

	EACESessionState GetState() const { return State; }
	const FString& GetConnectionError() const { return ConnectionError; }
	/**
	 * Server game time (ACE Timers.PortalYearTicks, seconds — 1 real second = 1 tick),
	 * extrapolated from ConnectRequest / TimeSync. Check HasServerTime to distinguish
	 * an unknown clock from the valid zero epoch.
	 */
	double GetPortalYearTicks() const;
	bool HasServerTime() const { return bHasServerTime; }
	const TArray<FACECharacterInfo>& GetCharacters() const { return Characters; }
	int32 GetPreferredCharacterId() const { return PreferredCharacterId; }
	void SetPreferredCharacterId(int32 Id) { PreferredCharacterId = Id; }
	int32 GetPlayerGuid() const { return PlayerGuid; }
	const FACEPosition& GetPlayerPosition() const { return PlayerPosition; }
	/** Server position qualities, including retail LastOutsideDeath (14). */
	const FACEPosition* GetPlayerPositionQuality(uint32 Key) const { return PlayerPositionQualities.Find(Key); }
	uint16 GetTeleportSeq() const { return TeleportSeq; }
	uint16 GetForcePositionSeq() const { return ForcePositionSeq; }
	const FString& GetAccountName() const { return AccountName; }
	const FString& GetServerName() const { return ServerName; }
	const TMap<int32, FACEWorldObject>& GetWorldObjects() const { return WorldObjects; }
	bool GetWorldObject(int32 Guid, FACEWorldObject& Out) const;
	const FACEPlayerVitals& GetPlayerVitals() const { return PlayerVitals; }
	const FACESelectedObject& GetSelectedObject() const { return SelectedObject; }
	int32 GetPreviousSelectedObjectGuid() const { return PreviousSelectedObjectGuid; }
	int32 GetGiveRecipientGuid() const { return GiveRecipientGuid; }

	/** PropertyInt.EncumbranceVal (key 5). False until the server has sent it this session. */
	bool TryGetPlayerEncumbrance(int32& OutEncumbrance) const
	{
		if (!bHasPlayerEncumbrance)
		{
			return false;
		}
		OutEncumbrance = PlayerEncumbranceVal;
		return true;
	}

	/** Active enchantments (attribute + skill + vitae + …); excludes cooldown entries. */
	void GetActiveEnchantments(TArray<FACEActiveEnchantment>& Out, bool bIncludeCooldowns = false) const;
	/** Retail's ten cooldown icon phases; zero means no active server cooldown. Now is monotonic time. */
	int32 GetItemCooldownPhase(const FACEWorldObject& Object, double Now) const;
	/** Vitae StatModValue (1.0 = none). False when no Vitae enchantment is active. */
	bool TryGetVitaeMultiplier(float& OutMultiplier) const;
	int32 GetVitaeCpPool() const { return VitaeCpPool; }
	int32 GetDeathLevel() const { return DeathLevel; }
	FACELinkStatus GetLinkStatus() const;
	/** On-demand /acueperf detail; times cover the most recent receive/dispatch pass. */
	FString DescribeNetworkPump() const;
	/** GameAction PingRequest (0x01E9) — LinkStatus panel RTT. */
	void SendPingRequest();

	/** Ordered contents from the last ViewContents for a pack/container GUID. */
	const TArray<FACEContainerItemRef>* GetContainerContents(int32 ContainerGuid) const;

	/** Equipped items on the local player (CurrentWieldedLocation != 0 or ParentGuid == self). */
	void GetEquippedItems(TArray<FACEWorldObject>& Out) const;
	/** Read-only lookup for per-frame VR queries. Valid only until session objects mutate. */
	const FACEWorldObject* FindEquippedItem(int64 LocationMask, int32 ItemTypeMask = 0, int32 AmmoTypeMask = 0) const;

	/**
	 * Pack inventory: prefers ViewContents order when known; otherwise falls back to
	 * ObjectCreate items whose ContainerId matches. PlayerGuid = main pack.
	 */
	void GetPackItems(int32 ContainerGuid, TArray<FACEWorldObject>& Out) const;
	/** Same ordered slots as GetPackItems, including unresolved side-pack item IDs.
	 * Query-local IDs let readers avoid copying appearance/property arrays. */
	void GetPackItemGuids(int32 ContainerGuid, TArray<int32>& Out) const;
	/** Count requested containers (map keys) together, without copying or sorting item models.
	 * Matches GetPackItems, including pending ViewContents entries and late main-pack items. */
	void GetPackItemCounts(TMap<int32, int32>& InOutCounts) const;

	/** Drop ViewContents cache after loot UI actually closes (CloseGroundContainer defers this). */
	void ClearContainerContents(int32 ContainerGuid);

	/** Top-level packs on the player (ContainerType packs / ItemsCapacity containers). */
	void GetPlayerPacks(TArray<FACEWorldObject>& Out) const;
	/** Same ordered pack IDs as GetPlayerPacks; query afresh after session changes. */
	void GetPlayerPackGuids(TArray<int32>& Out) const;
	void GetContainerPackGuids(int32 ContainerGuid, TArray<int32>& Out) const;

	/** GameAction Talk (0x0015) — local chat / @commands. */
	void SendTalk(const FString& Message);
	/** GameAction ChatChannel (0x0147) — fellowship / allegiance / etc. */
	void SendChatChannel(uint32 ChannelId, const FString& Message);
	/** Retail allegiance/MOTD command grammar and typed game actions. */
	void SendSocialCommand(const FString& Command, const FString& Arguments, FString& OutError);
	/** GameAction Tell (0x005D) — private tell by character name. */
	void SendTell(const FString& TargetName, const FString& Message);
	/** GameAction TalkDirect (0x0032) — private tell by object guid (reply). */
	void SendTalkDirect(int32 TargetGuid, const FString& Message);
	/** GameAction SoulEmote (0x01E1) — text only (OtherEmote fragment). */
	void SendSoulEmote(const FString& EmoteText);
	/**
	 * MoveToState with CommandListLength=1 MotionItem (autonomous) — retail *wave* / *bow*
	 * animation. MotionCommand is the full 32-bit value (Wave, BowDeepState, …).
	 */
	void SendSoulEmoteMotion(uint32 MotionCommand);
	/** GameAction TeleToLifestone (0x0063) — @ls / @lifestone. */
	void SendTeleToLifestone();
	/** GameAction TeleToMarketPlace (0x028D) — @mp / @marketplace. */
	void SendTeleToMarketplace();
	/** GameAction TeleToHouse (0x0262) — @house recall. */
	void SendTeleToHouse();
	/** GameAction TeleToMansion (0x0278) — @house mansion_recall / allegiance housing. */
	void SendTeleToMansion();
	/** GameAction RecallAllegianceHometown (0x02AB). */
	void SendRecallAllegianceHometown();
	/** Turbine global chat (0xF7DE NETBLOB_REQUEST_BINARY / SendToRoomByID). */
	void SendTurbineChat(uint32 ChannelId, uint32 ChatType, const FString& Message);
	uint32 GetTurbineChannelId(uint32 ChatType) const;
	/** Empty GameAction (die, abandon house, boot everyone, …). */
	void SendSimpleGameAction(uint32 ActionType);
	void SendGameActionU32(uint32 ActionType, uint32 Value);
	void SendGameActionString(uint32 ActionType, const FString& Value);
	void SendGameActionStringU32(uint32 ActionType, const FString& Value, uint32 Extra);
	/** Explicit rejected inventory attempts; a successful UseDone is not a transfer acknowledgement. */
	uint32 GetInventoryFailureSerial() const { return InventoryFailureSerial; }
	int32 GetInventoryFailureItem() const { return InventoryFailureItem; }
	uint32 GetInventoryFailureError() const { return InventoryFailureError; }
	FString GetPlayerName() const;
	bool IsAfk() const { return bAfkMode; }
	void SendLegacyFriendsRequest();
	void SetAfkLocal(bool bAfk) { bAfkMode = bAfk; }
	/** Last player who sent us a tell (for @r / @reply). */
	int32 GetLastTellSenderGuid() const { return LastTellSenderGuid; }
	const FString& GetLastTellSenderName() const { return LastTellSenderName; }
	const FString& GetLastPatronTellSenderName() const { return LastPatronTellSenderName; }
	const FString& GetLastMonarchTellSenderName() const { return LastMonarchTellSenderName; }

	/** Client selection and retail health/item-mana subscription. Guid 0 clears. */
	void SelectObject(int32 ObjectGuid, bool bInventorySelection = false);

	/** Background assessments must not become the user's server-command target. */
	void SendIdentifyObject(int32 ObjectGuid, bool bBackground = false);
	void SendSetInscription(int32 ObjectGuid, const FString& Text);

	/** GameAction RaiseAttribute (0x0045) — spend AvailableExperience on a primary attribute. */
	void SendRaiseAttribute(uint32 AttributeId, uint32 XpAmount);

	/** GameAction RaiseVital (0x0044) — spend AvailableExperience on Health/Stamina/Mana. */
	void SendRaiseVital(uint32 VitalId, uint32 XpAmount);

	/** GameAction RaiseSkill (0x0046) — spend AvailableExperience on a trained/specialized skill. */
	void SendRaiseSkill(uint32 SkillId, uint32 XpAmount);

	/** GameAction TrainSkill (0x0047) — spend skill credits to train an untrained skill. */
	void SendTrainSkill(uint32 SkillId, int32 CreditsSpent);

	/** GameAction Emote (0x01DF) — retail *bow style local emote text. */
	void SendEmote(const FString& EmoteText);

	/** GameAction PutItemInContainer (0x0019) — drag & drop into a pack (Placement = slot index). */
	void SendPutItemInContainer(int32 ItemGuid, int32 ContainerGuid, int32 Placement);
	/** Retail auto-merge: first destination stack that can hold the entire selected amount. */
	int32 FindContainerMergeTarget(int32 ItemGuid, int32 ContainerGuid, int32 Amount) const;

	/** GameAction StackableMerge (0x0054) — combine stackables by guid (amount of From into To). */
	void SendStackableMerge(int32 MergeFromGuid, int32 MergeToGuid, int32 Amount);

	/** GameAction StackableSplitToContainer (0x0055). */
	void SendStackableSplitToContainer(int32 StackGuid, int32 ContainerGuid, int32 Placement, int32 Amount);

	/** GameAction StackableSplitTo3D (0x0056) — split onto ground. */
	void SendStackableSplitTo3D(int32 StackGuid, int32 Amount);

	/** GameAction StackableSplitToWield (0x019B). */
	void SendStackableSplitToWield(int32 StackGuid, int64 WieldLocation, int32 Amount);

	/** GameAction UseWithTarget (0x0035) — dual-use (keys, dyes, alchemy). */
	void SendUseWithTarget(int32 SourceGuid, int32 TargetGuid);

	/** GameAction TitleSet (0x002C) — set display title. */
	void SendSetTitle(uint32 TitleId);

	/** Character options bitfields as last known from PlayerDescription / our own edits. */
	uint32 GetCharacterOptions1() const { return CharacterOptions1; }
	uint32 GetSpellbookFilters() const { return SpellbookFilters; }
	void SendSpellbookFilters(uint32 Filters);
	uint32 GetCharacterOptions2() const { return CharacterOptions2; }
	/** PlayerDescription IsAdmin/IsArch/IsSentinel or ObjectDesc Admin on self. */
	bool IsLocalPlayerAdmin() const { return bLocalPlayerIsAdmin; }
	/** True when the retail PlayerOption index is set. */
	bool IsCharacterOptionSet(int32 Option) const;
	/** GameAction SetSingleCharacterOption (0x0005) — retail per-checkbox update. */
	void SendSetSingleCharacterOption(int32 Option, bool bValue);
	/** GameAction SetCharacterOptions (0x01A1) — push both bitfields (Apply / Reset / Default). */
	void SendCharacterOptions(uint32 Options1, uint32 Options2);

	/** GameAction GetAndWieldItem (0x001A) — drag & drop onto the paper doll (EquipMask location). */
	void SendGetAndWieldItem(int32 ItemGuid, int64 WieldLocation);

	/** GameAction DropItem (0x001B) — drop on the ground at our feet. */
	void SendDropItem(int32 ItemGuid);
	/** GameAction GiveObjectRequest (0x00CD) — give to player/NPC. */
	void SendGiveObjectRequest(int32 TargetGuid, int32 ItemGuid, int32 Amount);

	/** GameAction NoLongerViewingContents (0x0195) — close corpse/chest UI. */
	void SendNoLongerViewingContents(int32 ContainerGuid);

	/** GameAction Buy (0x005F) — AmountAndObjectId pairs are (amount, objectID). */
	void SendBuyItems(int32 VendorGuid, const TArray<TPair<int32, int32>>& AmountAndObjectId);

	/** GameAction Sell (0x0060) — AmountAndObjectId pairs are (amount, objectID). */
	void SendSellItems(int32 VendorGuid, const TArray<TPair<int32, int32>>& AmountAndObjectId);

	/** GameAction OpenTradeNegotiations (0x01F6). */
	void SendOpenTrade(int32 OtherPlayerGuid);

	/** GameAction CloseTradeNegotiations (0x01F7). */
	void SendCloseTrade();

	/** GameAction AddToTrade (0x01F8) — TradeSlotSide 0 = self. */
	void SendAddToTrade(int32 ItemGuid, int32 TradeSlotSide = 0);

	/** GameAction AcceptTrade (0x01FA). */
	void SendAcceptTrade();

	/** GameAction DeclineTrade (0x01FB). */
	void SendDeclineTrade();

	/** GameAction ResetTrade (0x0204). */
	void SendResetTrade();

	int32 GetOpenExternalContainerGuid() const { return OpenExternalContainerGuid; }
	int32 GetOpenVendorGuid() const { return OpenVendorGuid; }
	int32 GetTradePartnerGuid() const { return TradePartnerGuid; }
	/** Independent retail trade acceptance; both players can accept simultaneously. */
	bool HasAcceptedTrade() const { return bTradeSelfAccepted; }
	bool HasPartnerAcceptedTrade() const { return bTradePartnerAccepted; }
	/** Compatibility accessor: self when accepted, otherwise partner, otherwise zero. */
	int32 GetTradeAcceptedGuid() const { return bTradeSelfAccepted ? PlayerGuid : bTradePartnerAccepted ? TradePartnerGuid : 0; }
	/** Merchandise from the last ApproachVendor (GameDataOnly PublicWeenieDesc list). */
	const TArray<FACEWorldObject>& GetVendorMerchandise() const { return VendorMerchandise; }
	float GetVendorBuyRate() const { return VendorBuyRate; }
	float GetVendorSellRate() const { return VendorSellRate; }
	FString GetVendorCurrencyName() const { return VendorCurrencyName; }
	int32 GetVendorCurrencyCount() const;
	bool VendorUsesPyreals() const { return VendorCurrencyWeenie == 0; }
	/** Retail UI reads CoinValue (20), not custom bank qualities or inferred coin stacks. */
	int32 GetVendorPurchaseFunds() const { return VendorUsesPyreals() ? FMath::Max(0, PlayerVitals.StatQualityInts.FindRef(20)) : GetVendorCurrencyCount(); }
	bool CanVendorBuyItem(const FACEWorldObject& Item) const;

	/** GameAction AddSpellFavorite (0x01E3) — place SpellId at BarIndex slot SlotIndex. Local bars update immediately. */
	void SendAddSpellToBar(int32 SpellId, int32 SlotIndex, int32 BarIndex);

	/** GameAction RemoveSpellFavorite (0x01E4). Local bars update immediately. */
	void SendRemoveSpellFromBar(int32 SpellId, int32 BarIndex);

	/** GameAction AddShortCut (0x019C) — drag inventory item onto hotbar slot. */
	void SendAddShortcut(int32 SlotIndex, int32 ObjectGuid);

	/** GameAction RemoveShortCut (0x019D). */
	void SendRemoveShortcut(int32 SlotIndex);

	bool EnterWorld(int32 CharacterId);
	bool EnterWorldByName(const FString& CharacterName);

	/**
	 * GameAction LoginComplete (0x00A1) — "exited portal space".
	 * Sent on first PlayerCreate and again after every portal/teleport when the
	 * destination is ready. Until the server receives this, Teleporting stays true
	 * and AutonomousPosition / MoveToState location updates are ignored.
	 */
	void SendLoginComplete();

	/** Alias used by portal transition code — always re-sends LoginComplete. */
	void NotifyExitedPortalSpace() { SendLoginComplete(); }

	/** GameAction Jump (0xF61B) — Extent 0..1 + local ACE velocity + position sequences. */
	void SendJump(float Extent, const FVector& LocalAceVelocity);

	/** Send locomotion. Speeds are typically ±1.0f. bRunning sets HoldKey::Run.
	 *  bStandingLongJump sets ContactLongJump bit1 while charging a standing jump. */
	void SendMoveToState(float Forward, float Strafe, float Turn, bool bRunning, bool bContact = true,
		bool bStandingLongJump = false, bool bUniformVRMovement = false, bool bMouseTurn = false);
	void SendAutonomousPosition(bool bContact = true, bool bTrackedVR = false);
	void SendStopMovement();
	/** GameAction Use (0x36) — server approaches and activates (doors, etc.). */
	void SendUseItem(int32 ObjectGuid);
	bool IsCombatTarget(int32 Guid) const;

	/** True while a Use / UseWithTarget is outstanding (cleared by UseDone 0x01C7). */
	bool IsUseBusy() const { return bUseBusy || PendingEquipmentGuid != 0; }
	/** Presentation only: retail UI waits must never block input or automation. */
	bool IsCursorBusy() const;
	void CancelCursorAppraisal() { CursorAppraisalGuid=0; CursorAppraisalUntil=0.; }
	int32 GetPendingEquipmentGuid() const { return PendingEquipmentGuid; }
	bool IsEquipmentSwapPending() const { return PendingEquipmentGuid != 0; }
	bool IsInventoryTransferPending() const { return !PendingInventoryTransfers.IsEmpty(); }
	bool IsPendingWorldUse(int32 Guid) const { return bUseBusy && UseSourceGuid == Guid && UseTargetGuid == 0 && PendingEquipmentGuid == 0; }
	/** Cancel only an outstanding direct world interaction when its approach stops. */
	void CancelWorldUseApproach();
	uint32 GetCombatEventRevision() const { return CombatEventRevision; }
	uint64 GetInventoryDataRevision() const { return InventoryDataRevision; }
	bool IsServerAttackInProgress() const { return bServerAttackInProgress; }
	uint32 GetLastAttackError() const { return LastAttackError; }
	/** Automation waits for ACE's mode quality; GDLE accepts ordered player requests
	 *  silently and publishes the quality only for corrections, so its raw value can be stale. */
	bool IsCombatModeConfirmed(int32 Mode) const { return PendingCombatMode == 0 && PlayerVitals.CombatMode == Mode && (Creds.bGDLE || PlayerVitals.StatQualityInts.FindRef(40) == Mode); }
	uint32 GetCombatModeRevision() const { return CombatModeRevision; }

	/** Owned character titles (from GameEvent CharacterTitle / UpdateTitle). */
	const TArray<uint32>& GetCharacterTitleIds() const { return CharacterTitleIds; }
	uint32 GetDisplayTitleId() const { return DisplayTitleId; }

	/** Trade window item guids (side 1 = self, 2 = partner — retail tradeSide). */
	const TArray<int32>& GetTradeSelfItems() const { return TradeSelfItems; }
	const TArray<int32>& GetTradePartnerItems() const { return TradePartnerItems; }

	/** Fellowship / allegiance / friends (DAT Social panel). */
	const FACEFellowshipInfo& GetFellowship() const { return Fellowship; }
	const FACEAllegianceInfo& GetAllegiance() const { return Allegiance; }
	const TArray<FACEFriendInfo>& GetFriends() const { return Friends; }
	/** Contract tracker rows; names/details come from the DAT ContractTable. */
	const TArray<FACEContractEntry>& GetContracts() const { return Contracts; }
	/** GameAction AbandonContract (0x0316). */
	void SendAbandonContract(int32 ContractId);
	/** Housing panel state from GameEvent HouseData / HouseStatus. */
	const FACEHouseInfo& GetHouseInfo() const { return House; }
	const FACEHouseProfile& GetHouseProfile() const { return HouseOffer; }
	const FACEBarberProfile& GetBarberProfile() const { return BarberProfile; }
	bool IsBarberOpen() const { return bBarberOpen; }
	uint64 GetBarberRevision() const { return BarberRevision; }
	void CloseBarber() { bBarberOpen=false; }
	bool FinishBarber(const FACEBarberProfile& Profile);
	uint64 GetHouseProfileRevision() const { return HouseOfferRevision; }
	const TArray<int32>& GetHousePaymentItems(bool bRent) const { return bRent ? HouseRentItems : HouseBuyItems; }
	bool StageHousePayment(int32 Guid, bool bRent);
	bool CanStageHousePayment(int32 Guid, bool bRent) const;
	void PruneHousePaymentItems();
	void RemoveHousePayment(int32 Guid, bool bRent);
	int64 GetHousePaymentAmount(int32 Wcid, bool bRent) const;
	bool CanPayHouse(bool bRent) const;
	bool SendHousePayment(bool bRent);
	void CloseHouseProfile();
	/** GameAction HouseQuery (0x021E) — refresh the housing panel. */
	void SendHouseQuery();
	/** Open book from BookDataResponse / BookPageDataResponse. */
	const FACEBookInfo& GetBookInfo() const { return Book; }
	void ClearBook();
	/** GameAction AbuseLogRequest (0x0140) — character name + status mask + complaint. */
	void SendAbuseLogRequest(const FString& CharacterName, uint32 StatusMask, const FString& Complaint);
	/** GameAction BookData (0x00AA) / BookPageData (0x00AE). */
	void SendBookData(int32 BookGuid);
	void SendBookPageData(int32 BookGuid, int32 PageIndex);
	bool CanEditBookPage() const;
	FString GetBookPageText() const;
	bool SetBookPageText(const FString& Text);
	void TurnBookPage(int32 PageIndex);
	bool IsBookWritePending() const { return BookWriteAction != 0; }
	/** Chess resign / pass / offer-or-confirm stalemate. */
	void SendChessQuit();
	void SendChessMovePass();
	void SendChessStalemate(bool bStalemate);
	/** Chess join (retail CM_Game::Event_Join, color -1 = any) and grid move. */
	void SendChessJoin(int32 BoardGuid);
	int32 ChessBoardGuid = 0;
	double ChessJoinPendingUntil = 0.0;
	void SendChessMove(int32 FromX, int32 FromY, int32 ToX, int32 ToY);
	/** Squelch list from GameEvent SetSquelchDB. */
	const TArray<FACESquelchEntry>& GetSquelches() const { return Squelches; }
	bool IsSenderSquelched(int32 Guid, const FString& Name, uint32 MessageType) const;
	int32 GetGlobalSquelchMask() const { return GlobalSquelchMask; }
	/** Fill-component book from PlayerDescription: component wcid → quantity to rebuy. */
	const TMap<int32, int32>& GetDesiredComponents() const { return DesiredComponents; }
	/** GameAction SetDesiredComponentLevel (0x0224). Amount 0 removes the entry. */
	void SendSetDesiredComponentLevel(int32 ComponentWcid, int32 Amount);
	/** GameAction ModifyCharacterSquelch (0x0058). MessageType 0 = all channels. */
	void SendModifyCharacterSquelch(bool bSquelch, int32 TargetGuid, const FString& PlayerName,
		uint32 MessageType);
	/** GameAction ModifyAccountSquelch (0x0059). */
	void SendModifyAccountSquelch(bool bSquelch, const FString& PlayerName);
	/** GameAction ModifyGlobalSquelch (0x005B). */
	void SendModifyGlobalSquelch(bool bSquelch, uint32 MessageType);

	void SendFellowshipCreate(const FString& Name, bool bShareXP);
	void SendFellowshipQuit(bool bDisband);
	void SendFellowshipDismiss(int32 MemberGuid);
	void SendFellowshipRecruit(int32 PlayerGuid);
	void SendFellowshipUpdateRequest(bool bPanelOpen);
	void SendFellowshipAssignNewLeader(int32 MemberGuid);
	void SendFellowshipChangeOpenness(bool bOpen);
	void SendSwearAllegiance(int32 TargetGuid);
	void SendBreakAllegiance(int32 TargetGuid);
	void SendAllegianceUpdateRequest(bool bPanelOpen);
	void SendAddFriend(const FString& Name);
	void SendRemoveFriend(int32 FriendGuid);
	/** GameAction CreateTinkeringTool (0x027D) — salvage queue with Ust. */
	void SendCreateTinkeringTool(int32 ToolGuid, const TArray<int32>& ItemGuids);

	/** GameAction ChangeCombatMode (0x0053) — NonCombat/Melee/Missile/Magic (ACECombatMode). */
	void SendChangeCombatMode(uint32 CombatMode);

	/** GameAction TargetedMeleeAttack (0x0008) — target, AttackHeight, power 0..1. */
	void SendTargetedMeleeAttack(int32 TargetGuid, uint32 AttackHeight, float PowerLevel);

	/** GameAction TargetedMissileAttack (0x000A) — target, AttackHeight, accuracy 0..1. */
	void SendTargetedMissileAttack(int32 TargetGuid, uint32 AttackHeight, float AccuracyLevel);

	/** Cast a known spell — targeted if TargetGuid != 0, else untargeted. */
	void SendCastSpell(int32 SpellId, int32 TargetGuid = 0);
	void RequestVRCapabilities(bool bLocalVR = true);
	bool SupportsVRPoses() const { return (VRCapabilities & 16u) != 0; }
	bool SupportsUniformVRMovement() const { return bLocalVRFeedback && (VRCapabilities & 262144u) != 0; }
	double LastVRPoseSent = -100.;
	bool SendVRPose(FACEVRPose Pose);
	bool SupportsVRDrops() const { return (VRCapabilities & 1024u) != 0; }
	bool SendVRDrop(uint32 Cell, int32 Item, int32 SplitAmount, const FVector& OriginAc);
	bool GetVRPose(int32 Guid, FACEVRPose& Pose) const;
	void SendCancelAttack();
	bool SupportsVRCombat() const { return (VRCapabilities & 7u) == 7u; }
	bool SupportsVRCombatPower() const { return (VRCapabilities & 65536u) != 0; }
	bool SupportsVRUnarmed() const { return SupportsVRCombat() && (VRCapabilities & 128u) != 0; }
	float GetVRMissileSpeed(int32 Weapon) const { return Weapon == VRMissileWeapon ? VRMissileSpeed : 0.f; }
	bool GetVRSpellProfile(int32 Spell, float& Speed, bool& Gravity, float* Radius = nullptr) const;
	void RequestVRSpellProfile(int32 Spell);
	bool SendVRCombat(uint32 Kind, uint32 Cell, int32 Weapon, int32 Subject, int32 Target,
		const FVector& OriginAc, const FVector& DirectionOrEndAc, float Amount, float Duration, float RequestedPower = -1.f);

	const TArray<int32>& GetKnownSpells() const { return KnownSpells; }
	uint64 GetSpellDataRevision() const { return SpellDataRevision; }
	const TArray<int32>& GetSpellBar(int32 BarIndex) const;
	void SetActiveSpellBar(int32 BarIndex);
	int32 GetActiveSpellBar() const { return ActiveSpellBar; }
	int32 GetShortcutObject(int32 SlotIndex) const;

	/** Last known MotionStance for this player (from server UpdateMotion). Sent in MoveToState. */
	void SetCurrentStance(uint32 Stance);
	uint32 GetCurrentStance() const { return CurrentStance; }
	uint32 GetCurrentForwardCommand() const { return CurrentForwardCommand; }
	void ClearHeldForwardCommand() { if (ACEMotion::IsHeldPoseSubstate(CurrentForwardCommand)) CurrentForwardCommand = ACEMotion::Ready; }

	/** Retail AutoTarget: GUID of last creature that damaged us (DefenderNotification), or 0. */
	int32 GetLastAttackerGuid() const { return LastAttackerGuid; }
	double GetLastAttackerTimeSeconds() const { return LastAttackerTimeSeconds; }

	/**
	 * Apply PropertyInt.CombatMode from the server. Ignores transient NonCombat while a
	 * client ChangeCombatMode request is pending (SwitchCombatStyles mid-animation).
	 */
	void ApplyServerCombatMode(int32 Mode);

	void SetLocalPosition(const FACEPosition& Pos) { PlayerPosition = Pos; }
	/** Keep sending AutonomousPosition while idle (Use approach hold at target). */
	void SetForcePositionReporting(bool bForce) { bForcePositionReporting = bForce; }
	/** Flush one AutonomousPosition immediately (before a follow-up Use at range). */
	void FlushAutonomousPosition(bool bContact = true, bool bTrackedVR = false);
	/** Update contact after local collision, before a same-frame VR action. */
	void SetReportedContact(bool bContact) { bAutoPosContact = bContact; }

private:
    friend class FACEUCMProfileStorageTest;
    friend class FACEUCMRouteSaveEndToEndTest;
	void AdoptPlayerPhysicsTimestamps(const FACEWorldObject& Object);
	struct FReceivedFragment
	{
		uint64 BlobId = 0;
		uint16 Count = 0;
		uint16 Index = 0;
		uint16 Queue = 0;
		TArray<uint8> Data;
	};

	struct FPartialMessage
	{
		uint16 Count = 0;
		uint16 Queue = 0;
		TMap<uint16, TArray<uint8>> Parts;
		int32 Bytes = 0;
	};
	struct FEphemeralMessage
	{
		uint64 BlobId = 0;
		double UpdatedAt = 0;
	};

	void SetState(EACESessionState NewState);
	void Log(const FString& Msg);

	bool CreateSockets();
	void CloseSockets();
	void PollSockets();
	void HandleDatagram(const uint8* Data, int32 Size, bool bFromS2CSocket);

	void HandleConnectRequest(FACEBinaryReader& Body);
	void HandleGameMessage(const TArray<uint8>& MessageBytes);
	void HandleCharacterList(FACEBinaryReader& Reader);
	friend class FACECharacterManagementTest;
	bool BuildCharacterMutation(int32 CharacterId, bool bRestore, TArray<uint8>& Payload) const;
	void HandleCharacterRestored(FACEBinaryReader& Reader);
	int32 PendingCharacterMutation = 0;
	bool bRestoringCharacter = false;
	double CharacterMutationSentAt = 0;
	FString CharacterManagementError;
	void HandleCharacterCreated(FACEBinaryReader& Reader);
	bool bCharacterCreationPending = false;
	int32 CharacterSlotCount = 11;
	friend class FACERetailCharacterCreationTest;
	friend class FACERetailCharacterCreationScreenTest;
	void HandleUpdatePosition(FACEBinaryReader& Reader);
	void HandlePositionQuality(FACEBinaryReader& Reader, bool bPublic);
	void HandleUpdateMotion(FACEBinaryReader& Reader);
	void HandleVectorUpdate(FACEBinaryReader& Reader);
	void HandleSetState(FACEBinaryReader& Reader);
	void HandlePlayerCreate(FACEBinaryReader& Reader);
	void HandleObjectCreate(FACEBinaryReader& Reader, bool bForceRecreate = false);
	void HandleObjDescEvent(FACEBinaryReader& Reader);
	void HandleObjectDelete(FACEBinaryReader& Reader);
	void DeleteWorldObject(int32 Guid);
	TMap<int32, double> ObjectVisibilityDeadlines;
	// Retail AddContentsToDestructionQueue: closed loot has a 25-second grace,
	// independently of world visibility (contained items have no world cell).
	TMap<int32, double> ClosedContainerItemDeadlines;
	double NextClosedContainerExpiryAt = 0.;
	void ExpireClosedContainerItems(double Now);
	struct FPendingObjectPhysicsEvent
	{
		uint32 Opcode = 0;
		double QueuedAt = 0.;
		TArray<uint8> Payload;
	};
	TMap<int32, TArray<FPendingObjectPhysicsEvent>> PendingObjectPhysicsEvents;
	int32 PendingObjectPhysicsBytes = 0;
	double LastObjectPhysicsExpiryAt = 0.;
	void ExpireObjectPhysicsEvents(double Now);
	bool DeferObjectPhysicsEvent(int32 Guid, uint16 Instance, uint32 Opcode, const FACEBinaryReader& Reader, int32 PayloadStart);
	void ReplayObjectPhysicsEvents(int32 Guid);
	void HandleSound(FACEBinaryReader& Reader);
	void HandlePlayerTeleport(FACEBinaryReader& Reader);
	void ApplyVRWorldSnapshot(FACEBinaryReader& Reader);
	void HandleVRPose(FACEBinaryReader& Reader);
	void SendVRSubscriptions();
	void HandleHealthFeedback(FACEBinaryReader& Reader);
	void HandleVRRecovery(FACEBinaryReader& Reader);
	void HandleVRCasting(FACEBinaryReader& Reader);
	void HandlePlayScriptId(FACEBinaryReader& Reader);
	void HandlePlayEffect(FACEBinaryReader& Reader);
	void HandleServerMessage(FACEBinaryReader& Reader);
	void HandleHearSpeech(FACEBinaryReader& Reader);
	void HandleHearRangedSpeech(FACEBinaryReader& Reader);
	void HandleSoulEmote(FACEBinaryReader& Reader);
	void HandleTurbineChat(FACEBinaryReader& Reader);
	void HandleSetTurbineChatChannels(FACEBinaryReader& Reader);
	void HandleGameEvent(FACEBinaryReader& Reader);
	void HandleCombatVictimNotification(FACEBinaryReader& Reader);
	void HandleCombatKillerNotification(FACEBinaryReader& Reader);
	void HandleCombatAttackerNotification(FACEBinaryReader& Reader);
	void HandleCombatDefenderNotification(FACEBinaryReader& Reader);
	void HandleCombatEvasionAttacker(FACEBinaryReader& Reader);
	void HandleCombatEvasionDefender(FACEBinaryReader& Reader);
	void HandleWeenieError(FACEBinaryReader& Reader);
	void HandleWeenieErrorWithString(FACEBinaryReader& Reader);
	void HandleTransientString(FACEBinaryReader& Reader);
	void HandleTell(FACEBinaryReader& Reader);
	void HandlePlayerDescription(FACEBinaryReader& Reader);
	void HandlePrivateUpdateAttribute(FACEBinaryReader& Reader);
	void HandlePrivateUpdateAttributeLevel(FACEBinaryReader& Reader);
	void HandlePrivateUpdateVital(FACEBinaryReader& Reader);
	void HandlePrivateUpdateAttribute2ndLevel(FACEBinaryReader& Reader);
	void HandlePrivateUpdateSkill(FACEBinaryReader& Reader);
	void HandlePrivateUpdateSkillValue(FACEBinaryReader& Reader, bool bAdvancementClass);
	FACESkillInfo& FindOrAddPlayerSkill(int32 SkillId);
	void UpdatePlayerSkillFallback(FACESkillInfo& Skill);
	void HandlePrivateUpdatePropertyInt(FACEBinaryReader& Reader);
	bool ApplyPlayerInventoryCapacity(FACEWorldObject& Object) const;
	void HandlePrivateUpdatePropertyInt64(FACEBinaryReader& Reader, bool bPublic = false);
	void HandleUpdatePropertyFloat(FACEBinaryReader& Reader, bool bPublic);
	void HandleUpdatePropertyString(FACEBinaryReader& Reader, bool bPublic);
	void ApplyPlayerIntQuality(uint32 Key, int32 Value);
	void HandleRemoveProperty(FACEBinaryReader& Reader, uint32 Family, bool bPublic);
	bool ReadStringPropertyHeader(FACEBinaryReader& Reader, bool bPublic, int32& Guid, uint32& Property) const;
	void HandleUpdatePropertyBool(FACEBinaryReader& Reader, bool bPublic);
	void HandleUpdateHealth(FACEBinaryReader& Reader);
	void HandleQueryItemManaResponse(FACEBinaryReader& Reader);
	bool CanQueryItemMana(int32 Guid) const;
	bool CanQueryObjectHealth(int32 Guid) const;
	void HandleIdentifyObjectResponse(FACEBinaryReader& Reader);
	void HandleViewContents(FACEBinaryReader& Reader);
	void HandleCloseGroundContainer(FACEBinaryReader& Reader);
	void HandleApproachVendor(FACEBinaryReader& Reader);
	void HandleRegisterTrade(FACEBinaryReader& Reader);
	void HandleChessGameEvent(uint32 EventType, FACEBinaryReader& Reader);
	void HandleOpenTrade(FACEBinaryReader& Reader);
	void HandleCloseTrade(FACEBinaryReader& Reader);
	void HandleAcceptTradeEvent(FACEBinaryReader& Reader);
	void HandleDeclineTradeEvent(FACEBinaryReader& Reader);
	void HandleResetTradeEvent(FACEBinaryReader& Reader);
	void HandleAddToTradeEvent(FACEBinaryReader& Reader);
	void HandleRemoveFromTradeEvent(FACEBinaryReader& Reader);
	void HandleTradeFailure(FACEBinaryReader& Reader);
	void HandleClearTradeAcceptance(FACEBinaryReader& Reader);
	void HandleChannelBroadcast(FACEBinaryReader& Reader);
	void HandleFellowshipFullUpdate(FACEBinaryReader& Reader);
	void HandleFellowshipUpdateFellow(FACEBinaryReader& Reader);
	void HandleFellowshipQuitEvent(FACEBinaryReader& Reader);
	void HandleFellowshipDismissEvent(FACEBinaryReader& Reader);
	void HandleFellowshipDisband(FACEBinaryReader& Reader);
	void HandleAllegianceUpdate(FACEBinaryReader& Reader);
	void HandleAllegianceInfoResponse(FACEBinaryReader& Reader);
	void HandleAllegianceLoginNotification(FACEBinaryReader& Reader);
	void HandleFriendsListUpdate(FACEBinaryReader& Reader);
	void HandleSalvageOperationsResult(FACEBinaryReader& Reader);
	void HandleContractTrackerTable(FACEBinaryReader& Reader);
	void HandleContractTracker(FACEBinaryReader& Reader);
	void HandleSetSquelchDB(FACEBinaryReader& Reader);
	void HandleHouseData(FACEBinaryReader& Reader);
	void HandleServerDialog(uint32 Event, FACEBinaryReader& Reader);
	void HandleHouseProfile(FACEBinaryReader& Reader);
	void HandleAvailableHouses(FACEBinaryReader& Reader);
	bool IsHousePaymentOwned(int32 Guid) const;
	TArray<int32> CollectHousePaymentItems(int32 Guid, bool bRent) const;
	void HandleHouseStatus(FACEBinaryReader& Reader);
	void HandleHouseUpdateRentTime(FACEBinaryReader& Reader);
	void HandleHouseUpdateRentPayment(FACEBinaryReader& Reader);
	void HandleHouseUpdateHAR(FACEBinaryReader& Reader);
	void HandleBookDataResponse(FACEBinaryReader& Reader);
	void HandleBookPageDataResponse(FACEBinaryReader& Reader);
	void HandleBookWriteResponse(uint32 Event, FACEBinaryReader& Reader);
	bool SaveBookPage(int32 NextPage);
	void OpenBookPage(int32 PageIndex);
	void ResetBookEditing();
	void CheckBookWriteTimeout();
	bool ReadAllegianceData(FACEBinaryReader& Reader, FACEAllegianceMember& Out);
	bool ReadAllegianceProfile(FACEBinaryReader& Reader, int32 TargetGuid, FACEAllegianceInfo& Out);
	void HandleCharacterTitle(FACEBinaryReader& Reader);
	void HandleUpdateTitle(FACEBinaryReader& Reader);
	void HandleUseDone(FACEBinaryReader& Reader);
	void HandleItemAppraiseDone(FACEBinaryReader& Reader);
	void HandlePublicUpdatePropertyInt(FACEBinaryReader& Reader);
	void ApplyPlayerKillerStatus(int32 Guid, int32 Status);
	void HandlePublicUpdatePropertyDataID(FACEBinaryReader& Reader);
	void HandlePrivateUpdatePropertyDataID(FACEBinaryReader& Reader);
	void HandlePublicUpdateInstanceId(FACEBinaryReader& Reader);
	void HandlePrivateUpdateInstanceId(FACEBinaryReader& Reader);
	void ApplyPropertyInstanceId(int32 Guid, uint32 Prop, int32 Value);
	void HandleInventoryRemoveObject(FACEBinaryReader& Reader);
	void HandleSetStackSize(FACEBinaryReader& Reader);
	void HandleInventoryPutObjInContainer(FACEBinaryReader& Reader);
	void HandleWieldItem(FACEBinaryReader& Reader);
	void HandleInventoryPutObjIn3D(FACEBinaryReader& Reader);
	void HandlePickupEvent(FACEBinaryReader& Reader);
	void HandleInventoryServerSaveFailed(FACEBinaryReader& Reader);
	uint32 InventoryFailureSerial = 0;
	int32 InventoryFailureItem = 0;
	uint32 InventoryFailureError = 0;
	void AdvanceEquipmentSwap();
	void CancelEquipmentSwap();
    int32 FindEquipmentStowContainer(const TMap<int32, int32>& Reserved) const;
	bool CanStowEquipmentInContainer(int32 Container, int32 Reserved = 0) const;
	bool HasPlayerInventoryState(const FACEWorldObject& Object) const;
	enum EEquipmentRemovalEvidence : uint8 { WielderCleared = 1, WieldLocationCleared = 2, WorldDeleted = 4 };
	// ACE consumes equipped items using UI quality clears plus a Smartbox delete,
	// which can arrive in either queue order. Neither alone removes ownership.
	TMap<int32, uint8> EquipmentRemovalEvidence;
	bool UpdateEquipmentRemovalEvidence(int32 Guid, uint8 Evidence, bool bSet = true);
	void RemoveInventoryObject(int32 ItemGuid);
	int32 PendingEquipmentGuid = 0;
	TMap<int32,double> PendingInventoryTransfers;
	uint32 PendingEquipmentCombatMode = 0;
	bool bPendingEquipmentWieldSent = false;
	int64 PendingEquipmentLocation = 0;
	TArray<int32> PendingEquipmentRemovals;
	double PendingEquipmentUntil = 0.0;

	void HandleParentEvent(FACEBinaryReader& Reader);
	/** Clear world placement and despawn the 3D actor; keep WorldObjects for inventory. */
	void RemoveObjectFromWorldVisual(int32 ObjectGuid);
	/** Drop an item guid from every tracked container list (before re-inserting). */
	void RemoveFromContainerLists(int32 ItemGuid);
	/** Side-pack / foci items share a separate PlacementPosition namespace from main-pack loot. */
	static bool IsPackSlotItem(const FACEWorldObject& Obj);
	/**
	 * Index into a (possibly mixed item/pack) ContainerContents list for a slot Placement
	 * in that ContainerType namespace — matches UI DestSlot / server PlacementPosition.
	 */
	static int32 ContentsInsertIndexForPlacement(const TArray<FACEContainerItemRef>& Contents,
		int32 Placement, int32 ContainerType);
	/**
	 * Mirror ACE Container.TryRemoveFromInventory: siblings with PP > RemovedPlacement
	 * decrement. Call after the removed item is no longer ContainerId-linked.
	 */
	void ShiftContainerPlacementsAfterRemove(int32 ContainerGuid, int32 RemovedPlacement,
		bool bPackSlots);
	/**
	 * Mirror ACE Container.TryAddToInventory: siblings with PP >= Placement increment,
	 * then ItemGuid gets its clamped dense insertion position.
	 */
	void ShiftContainerPlacementsForInsert(int32 ContainerGuid, int32 ItemGuid, int32 Placement,
		bool bPackSlots);
	/**
	 * Collision repair only: densify PlacementPosition for one pack-slot namespace.
	 * Put/ContainId normally uses the ordered remove/insert path.
	 */
	void NormalizeContainerPlacements(int32 ContainerGuid, bool bPackSlots,
		int32 InsertGuid = 0, int32 InsertAt = INDEX_NONE);
	/** Keep ViewContents list order ↔ PlacementPosition in sync after list mutations. */
	void RestampContainerListPlacements(int32 ContainerGuid);
	int32 ResolveContainerRoot(int32 ContainerGuid) const;
	void ReconcileOpenContainerOwnership();
	void ApplyPlayerInventoryProfile();
	void ApplyPropertyDataID(int32 ObjectGuid, uint32 PropertyId, uint32 Value);
	void HandleMagicUpdateSpell(FACEBinaryReader& Reader);
	void HandleMagicRemoveSpell(FACEBinaryReader& Reader);
	void HandleMagicUpdateEnchantment(FACEBinaryReader& Reader);
	void HandleMagicRemoveEnchantment(FACEBinaryReader& Reader);
	void HandleMagicUpdateMultipleEnchantments(FACEBinaryReader& Reader);
	void HandleMagicRemoveMultipleEnchantments(FACEBinaryReader& Reader);
	void HandleMagicPurgeEnchantments(FACEBinaryReader& Reader);
	void HandleMagicPurgeBadEnchantments(FACEBinaryReader& Reader);
	void HandleMagicDispelEnchantment(FACEBinaryReader& Reader);
	void HandleMagicDispelMultipleEnchantments(FACEBinaryReader& Reader);
	void HandlePingResponse(FACEBinaryReader& Reader);
	void RecomputeAttributeEnchantments(FACEPlayerVitals& OutVitals);
	bool ReadEnchantmentRecord(FACEBinaryReader& Reader, FACEActiveEnchantment& Out);
	bool ReadEnchantmentList(FACEBinaryReader& Reader, TArray<FACEActiveEnchantment>& Out);
	void UpsertEnchantment(const FACEActiveEnchantment& Entry);
	void RemoveEnchantment(uint16 SpellId, uint16 Layer);
	void NotifyEnchantmentsChanged();
	float RecordEchoRequest(double SentAt);
	void UpdateLinkStatusFromEcho(float ClientTimeSent, double ReceivedAt);
	void UpsertWorldObject(const FACEWorldObject& Object, bool bForceRecreate = false);
	void MaybeEnterWorldComplete();
	void SetSelectedObjectInternal(const FACESelectedObject& Sel, bool bInventorySelection = false);
	/** Drop in-world state after a successful logoff (keep sockets / account session). */
	void ClearWorldState();

	static TArray<uint8> BuildLoginRequestBody(const FACELoginCredentials& Credentials);
	void SendLoginRequest();
	void SendConnectResponse();
	void SendCharacterLogOff();
	/** PacketHeaderFlags.Disconnect — tells ACE to DropSession / force logoff. */
	void SendDisconnectPacket();
	void SendGameMessage(uint32 Opcode, const TArray<uint8>& PayloadAfterOpcode, uint16 Queue, bool bEncrypted = true);
	void SendGameAction(uint32 ActionType, const TArray<uint8>& ActionPayload, uint16 Queue);
	void SendRawPacket(EACEPacketHeaderFlags Flags, const TArray<uint8>& Body, const TArray<TArray<uint8>>& Fragments, bool bToS2CPort, bool bEncrypted, uint16 HeaderId);
	uint16 PacketIntervalAt(double Now) const;
	void SendAckIfNeeded();
	void SendEchoResponse(float ClientTime);
	/** Cleartext C2S NAK — retail PacketHeaderFlags.RequestRetransmit (no EncryptedChecksum). */
	void SendRequestRetransmit(const TArray<uint32>& MissingSequences);
	void HandleServerRequestRetransmit(const TArray<uint32>& Sequences);
	void CacheOutboundPacket(uint32 Sequence, EACEPacketHeaderFlags Flags, uint32 IsaacXor, uint32 PayloadHash, const TArray<uint8>& PayloadAfterHeader);
	void ProcessOrderedS2CPacket(uint32 Sequence, EACEPacketHeaderFlags Flags, const TArray<FReceivedFragment>& Fragments, double ServerTicks, double ReceivedAt);
	void ApplyServerTime(double ServerTicks, double ReceivedAt, const TCHAR* Source);
	void DrainOutOfOrderS2C(double FrameStartedAt = 0.0);
	double NetworkDispatchAge(double Now) const;
	double NetworkPumpBudgetSeconds(bool bSocketBacklog) const;
	void RequestMissingS2CPackets(double Now);
	void ProcessReceivedFragment(const FReceivedFragment& Fragment);
	void ResetReceivedMessages();
	void RemovePartialMessage(uint64 BlobId);
	void ExpireEphemeralMessages(double Now);
	void ReceiveGameMessage(const TArray<uint8>& Message);
	bool AcceptPropertyStamp(const TArray<uint8>& Message);
	void QueueReadyGameEvents(int32 Owner);
	void DispatchReadyGameEvent();

	static uint32 HeaderHash32(uint32 Sequence, EACEPacketHeaderFlags Flags, uint16 Id, uint16 Time, uint16 Size, uint16 Iteration);

	struct FCachedC2SPacket
	{
		EACEPacketHeaderFlags Flags = EACEPacketHeaderFlags::None;
		uint32 IsaacXor = 0;
		uint32 PayloadHash = 0;
		TArray<uint8> Payload; // bytes after 20-byte header
	};

	struct FPendingS2CPacket
	{
		bool bDispatched = false;
		EACEPacketHeaderFlags Flags = EACEPacketHeaderFlags::None;
		TArray<FReceivedFragment> Fragments;
		double ServerTicks = 0.0;
		double ReceivedAt = 0.0;
	};

	FACELoginCredentials Creds;
	FString AccountName;
	EACESessionState State = EACESessionState::Disconnected;
	TArray<FACEConfirmation> Confirmations;
	TArray<FACEServerPopup> ServerPopups;
	uint64 NextServerPopupId = 0;
	double LastServerPacketAt = 0.0;
	double LastServerDatagramAt = 0.0;
	double LastReceivePortKeepaliveAt = 0.0;
	// Mobile networks can expire the inbound UDP mapping after only 60 seconds.
	double ReceivePortKeepaliveInterval = PLATFORM_ANDROID ? 20.0 : 110.0;
	/** Retail ClientFlowQueue sends cmdNOP to port+1 every 220 half-second intervals. */
	void MaintainReceivePort(double Now);
#if WITH_DEV_AUTOMATION_TESTS
	friend class FACEReceiveKeepaliveTest;
#endif
	double LoginRequestAt = 0.0;
	double PacketTimeOrigin = 0.0;
	bool HasConnectionTimedOut(double Now) const;
	bool bRecoverLostConnection = false;
	FString ConnectionError;
	/** Encrypted replies can arrive before ConnectRequest supplies ISAAC seeds. */
	TArray<TArray<uint8>> PreHandshakeDatagrams;
	uint32 VRCapabilities = 0;
	bool bVRDiscoveryRequested = false;
	bool bLocalVRFeedback = false;
	int32 VRMissileWeapon = 0;
	float VRMissileSpeed = 0.f;
	TMap<int32, FVector> VRSpellProfiles; // speed (AC m/s), gravity enabled, projectile radius (m)
	uint32 VRSequence = 0;
	FVector VRMeleeBody = FVector::ZeroVector;
	uint32 VRCastSequence = 0, VRCastEpoch = 0, VRCastPhase = 0;
	uint32 VRAimCastSequence = 0;
	int32 VRAimSpell = 0, VRAimWeapon = 0;
	double VRCastReadyAt = 0, VRAimUntil = 0, VRNextAim = 0;
	float VRCastDuration = 0;
	uint32 VRRecoverySequence = 0, VRRecoveryTeleport = 0;
	double VRRecoveryReadyAt = 0;
	float VRRecoveryDuration = 0;
	uint32 VRPoseSequence = 0;
	TMap<int32, FACERemoteVRPose> VRPoses;

	FSocket* SocketC2S = nullptr; // send to :Port, recv ConnectRequest
	FSocket* SocketS2C = nullptr; // send ConnectResponse to :Port+1, recv game traffic

	TSharedPtr<FInternetAddr> ServerC2SAddr;
	TSharedPtr<FInternetAddr> ServerS2CAddr;

	uint16 ClientId = 0;
	uint64 ConnectionCookie = 0;
	uint8 ServerSeed[4] = {};
	uint8 ClientSeed[4] = {};

	TUniquePtr<FACEIsaac> IssacClient;      // C2S XOR keys we consume when sending
	TUniquePtr<FACECryptoSystem> IssacServer; // S2C verify

	/** ACE.Server lastReceived starts at 1 → first C2S data seq must be 2. */
	uint32 NextPacketSequence = 2;
	uint32 NextFragmentSequence = 1;
	uint32 NextGameActionSequence = 1;
	/** Contiguous S2C cursor (server PacketSequence first data is typically 2). */
	uint32 LastReceivedPacketSequence = 1;
	float AckTimer = 0.f;
	double LastRequestForRetransmitTime = 0.0;
	TMap<uint32, FCachedC2SPacket> CachedC2SPackets;
	TMap<uint32, FPendingS2CPacket> OutOfOrderS2CPackets;
	TQueue<uint32> PendingS2CDispatch;
	int32 PendingS2CDispatchCount = 0;
	uint32 LastTimeSyncPacketSequence = 0;
	/** Packet gaps affect ACK/NAK, not dispatch. Only F7B0 events block on their own stamps. */
	struct FOrderedGameEvents
	{
		struct FEvent { TArray<uint8> Message; double QueuedAt = 0; };
		uint32 LastStamp = 0;
		bool bReceivedFirst = false;
		bool bReadyQueued = false;
		TMap<uint32, FEvent> Pending;
	};
	TMap<int32, FOrderedGameEvents> OrderedGameEvents;
	TQueue<int32> ReadyGameEventOwners;
	int32 PendingGameEventBytes = 0;
	int32 PendingGameEventCount = 0;
	TMap<int32, TMap<uint64, uint8>> PropertyStamps;
	/** Socket receipt handles transport first; gameplay has its own bounded pass. */
	bool bDeferOrderedDispatch = false;
	double LastNetworkReceiveMs = 0.0;
	double LastNetworkDispatchMs = 0.0;
	double LastNetworkDispatchBudgetMs = 0.0;
	int32 LastNetworkReceivedPackets = 0, LastNetworkDispatchedWork = 0;
	bool bNetworkSocketBacklog = false, bNetworkDispatchLimited = false;
	TSet<uint32> RequestedS2CPackets;
	TSet<uint32> RejectedS2CPackets;
	float AutoPosTimer = 0.f;
	void TickPositionReporting(float DeltaSeconds);
	FACEPosition LastReportedPosition;
	bool bHaveReportedPosition = false;
	bool bLastReportedContact = true;
	bool bPositionReportingSuspended = false;
	float EchoTimer = 0.f;
	/** While awaiting CharacterList, retransmit ConnectResponse (UDP loss + auth race). */
	float ConnectResponseRetryTimer = 0.f;
	float CharacterListWaitTimer = 0.f;
	int32 ConnectResponseRetriesSent = 0;
	bool bNeedAck = false;
	bool bEnteredWorldSent = false;
	int32 PendingEnterCharacterId = 0;
	bool bLoginCompleteSent = false;
	bool bMoving = false;
	bool bForcePositionReporting = false;
	/** Contact flag for periodic AutonomousPosition (0 while airborne). */
	bool bAutoPosContact = true;
	/** True after RequestLogOff until CharacterList returns us to character select. */
	bool bLogOffPending = false;
	double LogOffPortalStartTime = 0.;
	/** Force Disconnect if the server never answers CharacterLogOff. */
	float LogOffTimeout = 0.f;
	/** Retransmit CharacterLogOff while waiting (UDP loss). */
	float LogOffRetransmitTimer = 0.f;

	TArray<FACECharacterInfo> Characters;
	int32 PreferredCharacterId = 0;
	FString ServerName;
	int32 PlayerGuid = 0;
	FACEPosition PlayerPosition;
	TMap<uint32, FACEPosition> PlayerPositionQualities;
	FACEPlayerVitals PlayerVitals;
	FACESelectedObject SelectedObject;
	int32 ManualAppraisalGuid = 0;
	bool bBackgroundAppraisalContext = false;
	void RestoreManualAppraisalContext();
	int32 SelectedManaQueryGuid = 0;
	friend class FACESelectionManaTest;
	int32 PreviousSelectedObjectGuid = 0;
	// Inventory/merchant browsing must not replace the world recipient for Give.
	int32 GiveRecipientGuid = 0;
	TMap<int32, FACEWorldObject> WorldObjects;
	/** containerGuid → ordered item refs from GameEvent ViewContents (0x0196). */
	TMap<int32, TArray<FACEContainerItemRef>> ContainerContents;
	// PlayerDescription normally precedes PlayerCreate on the UI queue.
	TMap<int32, TPair<int64, uint32>> LoginEquipment;
	/** Optimistic PutItemInContainer — restore source pack/corpse on InventoryServerSaveFailed. */
	/** Last external corpse/chest opened via ViewContents (0 = none). */
	int32 OpenExternalContainerGuid = 0;
	/** Last vendor from ApproachVendor (0 = none). */
	int32 OpenVendorGuid = 0;
	/** Merchandise list from ApproachVendor (icons/names for vendor UI). */
	TArray<FACEWorldObject> VendorMerchandise;
	/** Vendor BuyPrice (what vendor pays you) / SellPrice (what you pay vendor). */
	float VendorBuyRate = 1.f;
	float VendorSellRate = 1.f;
	FString VendorCurrencyName;
	int32 VendorCurrencyCount = 0;
	mutable uint64 VendorPurseRevision = MAX_uint64;
	mutable uint32 VendorPurseWeenie = 0;
	mutable int32 VendorPurseCount = 0;
	uint32 VendorCurrencyWeenie = 0;
	uint32 VendorItemTypes = MAX_uint32;
	int32 VendorMinValue = -1;
	int32 VendorMaxValue = -1;
	/** Current trade partner (0 = none). */
	int32 TradePartnerGuid = 0;
	bool bTradeSelfAccepted = false;
	bool bTradePartnerAccepted = false;
	double TradeStamp = 0.0;
	TArray<int32> TradeSelfItems;
	// A completed trade must not remove an offered item twice if ACE returned it.
	TSet<int32> TradeRemovedItems;
	TArray<int32> TradePartnerItems;
	bool bUseBusy = false;
	// Independent waits preserve the hourglass when one overlapping operation finishes.
	TArray<TPair<uint32,double>> CursorUseWaits;
	double CursorAttackUntil = 0., CursorAppraisalUntil = 0., CursorAllegianceUntil = 0.;
	int32 CursorAppraisalGuid = 0;
	void ResetCursorWaits();
	void ReleaseCursorUse(bool ItemOnly=false);
	double UseStartedAt = 0.0;
	int32 UseSourceGuid = 0, UseTargetGuid = 0;
	void BeginPendingUse(int32 Source, int32 Target);
	void ClearPendingUse();
	void CancelPendingUse(const TCHAR* Reason);
	void CheckPendingUseTimeout(double Now);
	uint32 CombatEventRevision = 0;
	uint64 InventoryDataRevision = 0;
	bool bServerAttackInProgress = false;
	uint32 LastAttackError = 0;
	int32 LastTellSenderGuid = 0;
	FString LastTellSenderName;
	FString LastPatronTellSenderName, LastMonarchTellSenderName;
	uint32 DisplayTitleId = 0;
	TArray<uint32> CharacterTitleIds;
	FACEFellowshipInfo Fellowship;
	bool bFellowshipUpdatesRequested = false;
	FACEAllegianceInfo Allegiance;
	TArray<FACEFriendInfo> Friends;
	TArray<FACEContractEntry> Contracts;
	TArray<FACESquelchEntry> Squelches;
	FACEHouseInfo House;
	FACEHouseProfile HouseOffer;
	FACEBarberProfile BarberProfile;
	bool bBarberOpen=false;
	uint64 BarberRevision=0;
	uint64 HouseOfferRevision = 0;
	TArray<int32> HouseBuyItems, HouseRentItems;
	FACEBookInfo Book;
	int32 PendingBookPage = INDEX_NONE;
	uint32 BookWriteAction = 0;
	int32 BookWritePage = INDEX_NONE, BookNextPage = INDEX_NONE;
	double BookWriteRequestedAt = 0.;
	FString BookDraft, BookWriteText;
	bool bBookDraft = false, bBookRefreshRequired = false;
	double BookPageRequestedAt = 0.0;
	int32 GlobalSquelchMask = 0;
	TMap<int32, int32> DesiredComponents;
	/** Initiator of the current trade window (from RegisterTrade). */
	int32 TradeInitiatorGuid = 0;
	TArray<int32> KnownSpells;
	uint64 SpellDataRevision = 0;
	TArray<TArray<int32>> SpellBars;
	int32 ActiveSpellBar = 0;
	/** Server PropertyInt.EncumbranceVal for the local player (not ObjectCreate Burden). */
	bool bHasPlayerEncumbrance = false;
	int32 PlayerEncumbranceVal = 0;

	/** Active enchantments (PlayerDescription + MagicUpdate/Remove/Purge). */
	TArray<FACEActiveEnchantment> ActiveEnchantments;
	int32 VitaeCpPool = 0;
	int32 DeathLevel = 0;
	FACELinkStatus LinkStatus;
	friend class FACELinkTimingTest;
	double EchoTimeOrigin = 0.0;
	TMap<float, double> PendingEchoTimes;
	double PingRequestSentAt = 0.0;
	struct FLinkTrafficBucket
	{
		int64 Second = -1;
		uint32 Sent = 0, Retransmits = 0;
	};
	FLinkTrafficBucket LinkTraffic[10];
	void RecordLinkTraffic(double Now, uint32 Sent, uint32 Retransmits);
	FACELinkStatus GetLinkStatusAt(double Now) const;
	/** Retail shortcut bar, zero-based slots. Value is an inventory object GUID. */
	TArray<int32> ShortcutObjects;

	/** PlayerDescription CharacterOptions1 / CharacterOptions2 bitfields. */
	uint32 CharacterOptions1 = 0;
	uint32 SpellbookFilters = 0x3FFFu;
	uint32 CharacterOptions2 = 0;
	bool bLocalPlayerIsAdmin = false;

	/** GameEvent SetTurbineChatChannels — allegiance/society ids are per-character. */
	uint32 TurbineAllegianceChannel = ACETurbineChat::Allegiance;
	uint32 TurbineGeneralChannel = ACETurbineChat::General;
	uint32 TurbineTradeChannel = ACETurbineChat::Trade;
	uint32 TurbineLfgChannel = ACETurbineChat::LFG;
	uint32 TurbineRoleplayChannel = ACETurbineChat::Roleplay;
	uint32 TurbineOlthoiChannel = ACETurbineChat::Olthoi;
	uint32 TurbineSocietyChannel = ACETurbineChat::Society;
	uint32 TurbineChatContextId = 1;
	bool bAfkMode = false;

	/** Last MotionStance for this player — must be echoed in MoveToState CurrentStyle. */
	uint32 CurrentStance = ACEMotion::StanceNonCombat;
	uint32 CurrentForwardCommand = ACEMotion::Ready;

	/** Creature that last damaged us (DefenderNotification name → GUID). */
	int32 LastAttackerGuid = 0;
	double LastAttackerTimeSeconds = 0.0;

	/**
	 * Optimistic combat mode from the last ChangeCombatMode we sent. Transient server
	 * NonCombat PropertyInt during SwitchCombatStyles is deferred until this expires.
	 */
	uint32 PendingCombatMode = 0;
	uint32 CombatModeRevision = 0;
	double PendingCombatModeUntil = 0.0;
	int32 DeferredServerCombatMode = INDEX_NONE;

	/** Authenticated server sample and arrival time; survives portals, never server changes. */
	bool bHasServerTime = false;
	double PortalYearTicksAtConnect = 0.0;
	double PortalYearTicksRealtime = 0.0;

	uint16 InstanceSeq = 0;
	uint16 ServerControlSeq = 0;
	uint16 TeleportSeq = 0;
	uint16 ForcePositionSeq = 0;
	/** Client MotionItem sequence for autonomous CommandList entries (soul emotes). */
	uint16 MotionActionSeq = 0;

	TMap<uint64, FPartialMessage> PartialFragments;
	TMap<uint64, FEphemeralMessage> EphemeralMessages;
	int32 PartialMessageBytes = 0;
	double LastEphemeralExpiry = 0;
	static constexpr int32 MaxPartialMessages = 1024;
	static constexpr int32 MaxPartialMessageBytes = 32 * 1024 * 1024;

	static constexpr int32 PacketHeaderSize = 20;
	static constexpr int32 FragmentHeaderSize = 16;
	static constexpr int32 MaxFragmentDataSize = 448;
	static constexpr uint32 HeaderChecksumMagic = 0xBADD70DDu;

	/**
	 * Timing (ACE server / retail client):
	 * - WorldManager UpdateGameWorld: 60 Hz
	 * - Physics MinQuantum / PhysicsObj.TickRate: 1/30 s (30 Hz)
	 * - Retail AutonomousPosition while moving: ~1 Hz (ACE GameActionAutonomousPosition)
	 * - Observer F748 broadcast threshold: 1 s (MoveToState_UpdatePosition_Threshold)
	 */
	static constexpr float AutonomousPositionInterval = 1.0f;
};
