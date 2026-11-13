#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "Containers/Deque.h"
#include "Dom/JsonObject.h"
#include "ACETypes.h"
#include "Mods/ACEUCMLog.h"
#include "Mods/ACELedger.h"
#include "Mods/ACEPluginLocalService.h"
#include "Mods/ACEPluginWorldPlate.h"
#include "Mods/ACEPluginTerrain.h"
#include "ACEPluginSubsystem.generated.h"

class FACEPluginVM;
namespace ACEWaypoint { struct FRoutePoint; }
namespace ACEUCMPanelPrivate { class FACEUCMImportUITest; }
class SWidget;
class AACEPlayerController;
class UGameViewportClient;
struct FACEWaypointLandmark { FVector2D Coordinates; FString Name, Type; };
struct FACEPluginRouteSegment { FVector Start,End; FLinearColor Color; float Thickness; };
struct FACEPluginMarker { int32 Id=0; FString Shape; FLinearColor Color; bool World=true, Map=true; };
namespace ACEPluginChatLimits
{
    constexpr int32 RetainedEvents=32768, RetainedTextBytes=8*1024*1024;
    constexpr int32 BatchEvents=128, BatchTextBytes=64*1024;
}
struct FACEPluginChatEvent
{
    uint64 Serial=0;
    int32 TextBytes=0;
    TSharedPtr<FJsonValue> Value;
};

struct FACEPluginChatFilter
{
    FString Id, Contains, Prefix, Exact, Sender, SenderType, Pattern, Action=TEXT("hide");
    TOptional<FLinearColor> Color;
    int32 ChatType=INDEX_NONE;
    uint8 Windows=0x1f;
    bool HasSender=false, CaseSensitive=false;
    double Expires=0;
};
struct FACEPluginChatPresentation
{
    uint8 Hidden=0, Forced=0;
    TMap<int32,FLinearColor> Colors;
};

struct FACEClientPlugin
{
    TSharedPtr<FJsonObject> ProfileLinks, ProfileBaseline;
    FString Id, Name, Version, Description, Directory, Status = TEXT("Stopped"), ProfileName = TEXT("Default");
    TArray<FString> Permissions;
    // Omitted fields preserve the original API; an explicit list is opt-in.
    TSet<FString> SnapshotFields;
    bool SelectiveSnapshot = false;
    double UpdateInterval = .5;
    bool WantsSnapshotField(const TCHAR* Field) const { return !SelectiveSnapshot || SnapshotFields.Contains(Field); }
    TSharedPtr<FJsonObject> Manifest, Profile, Presentation;
    TSharedPtr<class FACEPluginImageCache> Images;
    FString DisplayTitle() const;
    FString DisplayIcon() const;
    FString DisplayControls() const;
    FString DisplayStartLabel() const;
    FString DisplayStopLabel() const;
    bool ShowSettings() const;
    bool ScrollPanel() const;
    bool ShowStatus() const;
    TSharedPtr<FACEPluginVM> VM;
    TSharedPtr<FJsonObject> ActivityFailure;
    TSharedPtr<FJsonObject> CombatTransitionIntent;
    double CombatTransitionStarted = 0;
    int32 CombatTransitionMode = 0;
    uint32 CombatTransitionRevision = 0, AttackCancelRevision = 0;
    bool WaitingAttackCancel = false, CombatTransitionRequested = false;
    bool Enabled = false;
    bool Running = false;
    bool LocalService = false, LocalServiceStopped = false;
    bool Companion = false, CompanionStopped = false, StateDirty = false;
    int32 Api = 1;
    bool IsPolicy() const { return Id==TEXT("ucm") || (Api>=2 && !Companion && !LocalService); }
    bool HasPresentation() const { return Companion || Api>=2; }
    double NextService = 0;
    TArray<TSharedPtr<FJsonValue>> LogEntries;
    FString StatePath, CompanionError;
    TSharedPtr<FJsonObject> SavedState;
    TArray<TSharedPtr<FJsonValue>> PanelRows, PluginEvents;
    TArray<FACEPluginMarker> Markers;
    TArray<FACEPluginWorldPlate> WorldPlates;
    FACEPluginTerrain Terrain;
    uint32 WorldPlateRevision=0;
    uint64 ChatCursor=0;
    TArray<FACEPluginChatFilter> ChatFilters;
    bool ChatHistory=true, SelectiveChat=false;
    TArray<FACEPluginChatFilter> ChatSubscriptions;
    TDeque<FACEPluginChatEvent> ChatInbox;
    int32 ChatInboxBytes=0;
    uint64 ChatInboxDropped=0;
    uint32 PanelRevision=0;
    FVector2D PanelSize=FVector2D::ZeroVector;
    double NextStateSave=0, NextCommand=0;
    bool CanResumeMeta = false, ResumeMetaPending = false;
    uint32 VMSourceHash = 0, VMProfileHash = 0;
    TWeakPtr<class FACESession> VMSession;
    double NextAction = 0;
    double NextDecision = 0;
    FString WaitAction;
    FString TargetingStatus;
    int32 WaitItem = 0, WaitValue = 0;
    TArray<int32> WaitConsumedItems;
    uint32 WaitSerial = 0;
    double ActionSentAt = 0;
    int32 Player = 0;
    FString Server,MetaState,MetaStatus;
    FString LoggedActivity, LoggedRouteStatus;
};

/** Versioned, opt-in script plugins. No native injection or server protocol extensions. */
UCLASS()
class ACECLIENT_API UACEPluginSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
    friend class FACEFoundryTest;
    friend class FACEPluginHostTest;
    friend class FACEPluginLimitTest;
    friend class FACEUIWorkloadTest;
    friend class FACEUCMIdleWorkloadTest;
    friend class FACELedgerTest;
    friend class FACEPluginCompanionTest;
    friend class FACEPluginWorldPlateTest;
    friend class UACEPluginWorldPlateComponent;
    friend class FACEThirdEyeTest;
    friend class FACEDerethLensTest;
    friend class FACEPluginPublicAPITest;
    friend class FACEPluginEquipmentTest;
    friend class FACEPluginRequestsTest;
    friend class FACERunSpeedParityTest;
    friend class FACEPluginRampRouteTest;
    friend class FACEUCMRouteRejoinTest;
    friend class FACEUCMLogTest;
    friend class FACEItemToolsTest;
    friend class FACEAutoCraftTest;
    friend class FACEUCMProfileStorageTest;
    friend class FACEUCMRouteSaveEndToEndTest;
    friend class FACEUCMLootLatencyTest;
    friend class FACEUCMLootNotificationTest;
    friend class FACEPluginLootIconsTest;
    friend class FACEWeaponSkillLootTest;
    friend class FACEUCMLootBuilderTest;
    friend class FACEUCMWeaponSelectionTest;
    friend class FACEEquipmentAssessmentCacheTest;
    friend class FACEUCMProfilePanelTest;
    friend class FACEUCMSharedMetaTest;
    friend class ACEUCMPanelPrivate::FACEUCMImportUITest;
    friend class FACEPluginLocalServiceTest;
    friend class FACEPluginCosmeticsIntegrationTest;
    friend class FACEPluginCosmeticsTest;
    friend class FACEActionDeckTest;
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    void Discover();
    TSharedPtr<class FACEPluginStyle> RetailPluginStyle;
    TMap<FString,TSharedPtr<class FACEPluginStyle>> WindowPluginStyles;
    static bool IsLedger(const FString& Id) { return Id==TEXT("supplyledger")||Id==TEXT("huntledger"); }
    TSharedRef<SWidget> MakeLedgerPanel(const FString& Id);
    const TArray<FACESupplyEntry>& GetSupplies() const { return LedgerSupplies; }
    const FACEHuntLedger& GetHuntLedger() const { return HuntLedger; }
    uint32 GetSupplyRevision() const { return SupplyRevision; }
    void EditSupplySelection(bool Add);
    void RemoveSupply(int32 Index);
    void ToggleSupplyName(int32 Index);
    bool AreSupplyNamesVisible() const { return SupplyNamesVisible; }
    void ResetHuntLedger();
    bool LedgerOption(const FString& Id,const FString& Key,bool Default=false) const;
    void SetLedgerOption(const FString& Id,const FString& Key,bool Value);
    FVector2D LedgerSize(const FString& Id) const;
    double LedgerChromeHeight(const FString& Id, bool Hidden) const;
    UFUNCTION() void ObserveLedgerVitals(const FACEPlayerVitals& Vitals);
    TArray<TSharedPtr<FACEClientPlugin>> Plugins;
    FString Notice;
    FString SupplyNotice;
    bool Start(const FString& Id);
    bool RunPluginCommand(const FString& Arguments);
    bool ClickPluginRow(const FString& Id, const FString& RowId);
    bool ChangePluginControl(const FString& Id, const FString& ControlId, const TSharedPtr<FJsonValue>& Value);
    UFUNCTION() void ObservePluginChat(const FString& Text, const FString& Sender, int32 Type);
    // Display-only mask: raw chat still reaches every reader and the chat log file.
    uint8 FilterPluginChatWindows(const FString& Text, const FString& Sender, int32 Type) const;
    bool ValidatePluginChatFilters(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FACEPluginChatFilter>& Filters, FString& Error) const;
    FString PluginChatSenderType(const FString& Sender) const;
    static bool MatchesPluginChat(const FACEPluginChatFilter& Filter,const FString& Text,const FString& Sender,int32 Type,const FString& SenderType);
    static void DeliverPluginChat(FACEClientPlugin& Plugin,const FACEPluginChatEvent& Event);
    FACEPluginChatPresentation PresentPluginChat(const FString& Text,const FString& Sender,int32 Type) const;
    TSharedRef<SWidget> MakeCompanionPanel(const FString& Id);
    TArray<FACEPluginMarker> GetPluginMarkers() const;
    void ToggleUCM();
    void StopAll(const FString& Reason, bool PreserveMeta = false);
    void Stop(const FString& Id, const FString& Reason = TEXT("Stopped"), bool PreserveMeta = false);
    void SetEnabled(const FString& Id, bool Enabled);
    int32 GetEnabledPluginCount() const;
    int32 GetEnabledPluginLimit() const;
    bool SetEnabledPluginLimit(int32 Limit);
    void RegisterLocalService(const FString& Id, const TSharedRef<FACEPluginLocalService>& Service);
    void UnregisterLocalService(const FString& Id);
    TSharedRef<SWidget> MakeLocalServicePanel(const FString& Id);
    class UACEPluginCosmetics* GetLocalCosmetic(const FString& Id) const;
    void ClaimLocalCosmetic(const FString& Id);
    bool SaveProfile(const FString& Id, const FString& Name, const FString& Json, bool bStopRunning = true);
    bool SaveCurrentProfile(const FString& Id, bool bStopRunning = true);
    bool LoadProfile(const FString& Id, const FString& Name);
    TArray<FString> UCMSectionProfiles(const FString& Section) const;
    FString UCMSectionName(const FString& Section) const;
    bool SelectUCMSection(const FString& Section, const FString& Name);
    bool SaveUCMSectionAs(const FString& Section, const FString& Name);
    bool NewUCMSection(const FString& Section, const FString& Name);
    bool DeleteUCMSection(const FString& Section, const FString& Name);
    bool InitializeUCMSelection();
    bool MaterializeUCMMetaProfiles(const TSharedPtr<FJsonObject>& Profile, const FString& Name);
    bool ImportUCMSection(const FString& Filename);
    TSharedPtr<FJsonObject> ImportedUCMProfiles(const FString& Source) const;
    bool RememberUCMImport(const FString& Source, const TSharedPtr<FJsonObject>& Links);
    bool ReuseUCMImport(const FString& Source);
    TSharedPtr<FJsonObject> InspectLegacyProfile(const FString& Path);
    bool SaveLegacyLoot(const FString& Name,const TSharedPtr<FJsonObject>& Document);
    bool ImportLegacyProfile(const FString& Path,const FString& Name);
    bool SaveImportedProfile(const FString& Name,const TSharedPtr<FJsonObject>& Profile,const FString& Source);
    bool RunUCMCommand(const FString& Arguments);
    FString GetProfileFolder() const;
    bool SetProfileFolder(const FString& Folder);
    TArray<FString> GetProfileFiles(const FString& Extension) const;
    bool SelectProfileFile(const FString& Filename);
    bool ClearProfileFile(const FString& Extension);
    bool SaveLootProfile(const FString& Name);
    bool LoadLootProfile(const FString& Name);
    FString ProfileJson(const FString& Id) const;
    FString UserDirectory() const;
    TSharedRef<SWidget> MakePanel(const FString& PluginId = FString());
    void TogglePluginWindow(const FString& Id);
    bool IsPluginWindowOpen(const FString& Id) const;
    bool IsPointInsidePluginWindow(const FString& Id, FVector2D Absolute) const;
    bool WasPluginWindowOpen(const FString& Id) const;
    void SavePluginWindowVisibility(const FString& Id, bool Open);
    FVector2D GetPluginWindowPosition(const FString& Id, const FVector2D& Default) const;
    void SavePluginWindowPosition(const FString& Id, const FVector2D& Position);
    float GetPluginWindowOpacity(const FString& Id = FString()) const;
    void SetPluginWindowOpacity(const FString& Id, float Opacity, bool Persist = true);
    void ResetPluginWindowOpacity(const FString& Id);
    bool HasPluginWindowOpacity(const FString& Id) const;
    bool IsPluginOpacityGlobal() const;
    void SetPluginOpacityGlobal(bool Global);
    bool IsPluginWindowPinned(const FString& Id) const;
    void SetPluginWindowPinned(const FString& Id, bool Pinned);
    bool IsPluginQuickShow() const;
    bool IsPluginWindowChromeHidden(const FString& Id) const;
    bool CanManipulatePluginWindow(const FString& Id) const;
    TSharedRef<SWidget> MakePluginWindowControls(const FString& Id);
    bool IsPluginBarVisible() const;
    void SetPluginBarVisible(bool Visible);
    bool IsPluginBarMinimized() const;
    void SetPluginBarMinimized(bool Minimized);
    bool IsPluginBarHorizontal() const;
    void SetPluginBarHorizontal(bool Horizontal);
    TArray<FString> PluginDisplayOrder() const;
    bool MovePluginInOrder(const FString& Id, const FString& Target, bool After);
    uint32 GetPluginOrderRevision() const { return PluginOrderRevision; }
    bool IsPluginBarUnlocked() const;
    void TogglePanel();
    TSharedRef<SWidget> MakeUCMPanel(const FString& InitialPage=FString(), bool bShowTitle=true);
    TSharedRef<SWidget> MakeUCMMicroPanel();
    TSharedRef<SWidget> MakeItemToolsPanel(bool bShowTitle=true);
    bool RunItemToolsCommand(const FString& Arguments);
    bool StartItemTools(const FString& Mode);
    bool SelectItemToolsObject(const FString& Key, const FString& Kind);
    bool LoadItemToolsRules(const FString& Key, const FString& Filename);
    TSharedPtr<FJsonObject> ItemToolsProgress;
    // Built-in crafting keeps the large recipe catalog out of per-tick Lua snapshots.
    TSharedPtr<struct FACEAutoCraftState> AutoCraft;
    TSharedRef<SWidget> MakeAutoCraftPanel(bool bShowTitle=true);
    void RefreshAutoCraft(bool Force=false);
    bool ImportAutoCraftCatalog(const FString& Path);
    bool RemoveAutoCraftCatalog();
    void LoadAutoCraftCatalog(struct FACEAutoCraftState& State);
    TSharedPtr<FJsonObject> UCMGrenadeSpell(const FACEWorldObject& Item) const;
    void ExtendUCMCombatSnapshot(const TSharedPtr<FJsonObject>& Out);
    bool StartAutoCraft();
    void TickAutoCraft(FACEClientPlugin* Policy=nullptr);
    void ExtendUCMCraftSnapshot(const TSharedPtr<FJsonObject>& Out);
    void ExecuteUCMCraft(FACEClientPlugin& Policy,const TSharedPtr<FJsonObject>& Intent);
    TSharedPtr<struct FACEAutoCraftState> UCMCraft;
    TArray<TSharedPtr<FJsonValue>> FellowshipTells;
    uint32 FellowshipTellSerial=0;
    FString FellowshipManagerStatus;
    TSharedPtr<FJsonObject> UCMServicesProfile;
    void QueueFellowshipTell(const FString& Text,const FString& Sender,int32 SenderId,bool Local=false);
    void PauseAutoCraft();
    TSharedRef<SWidget> MakeUCMLogPanel();
    const FACEUCMLog& GetUCMLog() const { return UCMLog; }
    void ClearUCMLog();
    FString UCMLogSaveError;
    TSharedRef<SWidget> MakeWaypointPanel(bool bMap=false, bool bArrowOnly=false, bool bShowTitle=true);
    TSharedRef<SWidget> MakeWaypointMapIcon();
    TSharedRef<SWidget> MakeWaypointDungeonOverlay();
    bool IsWaypointMapUnlocked() const;
    bool IsWaypointEnabled() const;
    bool IsWaypointUnlocked() const;
    FACEPosition WaypointPlayerPosition() const;
    TArray<ACEWaypoint::FRoutePoint> GetWaypointMapRoute(bool Dungeon) const;
    bool SetWaypoint(const FString& Coordinates);
    void SetWaypoint(FVector2D Coordinates, uint32 DungeonLandblock=0);
    bool GetWaypoint(FVector2D& Coordinates, uint32& DungeonLandblock) const;
    bool WaypointOption(const FString& Key, bool Default=true) const;
    void SetWaypointOption(const FString& Key,bool Value);
    float WaypointMapOpacity() const;
    void SetWaypointMapOpacity(float Value);
    double WaypointMapZoom(bool Overlay, bool Dungeon) const;
    void SetWaypointMapZoom(bool Overlay, bool Dungeon, double Value);
    void ClearWaypoint();
    bool ImportWaypointLocations(const FString& Path);
    const TArray<FACEWaypointLandmark>& GetWaypointLocations();
    uint64 WaypointLocationsRevision=0;
    void RecordRouteAction(const FString& Type, float Charge = .5f);
    void RefreshAutomationData();
    /** Estimated remaining duration of our confirmed harmful cast on this target. */
    bool GetTargetEffectTimer(int32 Target,int32 Spell,double& Remaining,double& Duration) const;
    bool InventoryInteractionPending() const;
    const TMap<int32, FACEAppraisalInfo>& GetPluginAppraisals() const { return Appraisals; }
    const FACEAppraisalInfo* GetEquipmentAppraisal(const FACEWorldObject& Item) const;
    uint64 DataRevision = 0;
    UFUNCTION() void ObserveAppraisal(const FACEAppraisalInfo& Info);
    void ObserveUseDone(uint32 Error);
    void ObserveItemUse(int32 Source,int32 Target);
    void EmitPluginAction(const TSharedPtr<FJsonObject>& Event);
    TSharedPtr<FJsonObject> ObservedAction;
    FDelegateHandle ItemUseHandle;
    void ExtendInventorySnapshot(const TSharedPtr<FJsonObject>& Out);
    void ExtendItemInventorySnapshot(const TSharedPtr<FJsonObject>& Out);
    void ExtendSnapshot(const TSharedPtr<FJsonObject>& Out);
    TSharedPtr<FJsonValue> PluginItemSnapshot(const FACEWorldObject& Item);
    void QueueLootMatch(int32 Guid);
    void TickLootNotifications();
    void ShowLootNotification(const FString& Text);
    TSharedPtr<class FACEPluginVM> LootMatchVM;
    TWeakPtr<FJsonObject> LootMatchProfile;
    TWeakPtr<class FACESession> LootMatchSession;
    TSharedPtr<FJsonObject> LootMatchSnapshot;
    TArray<int32> LootMatchQueue;
    TMap<int32,FString> LootMatchReported;
    int32 LootMatchSelection=0, LootMatchPlayer=0;
    uint32 LootMatchSerial=0;
    double LootMatchRevision=-1;
    bool HasClearCombatSight(const FACEWorldObject& Target) const;
    /** Plugin inspection/buffing includes both carried and equipped gear. */
    bool IsOwnedPluginItem(const FACEWorldObject& Item) const;
    bool ExecuteInventory(FACEClientPlugin& P, const TSharedPtr<FJsonObject>& Intent, const FString& Action);
    void TrackActionWait(FACEClientPlugin& P,const FString& Action,int32 Item=0,int32 Value=0);
    void RefreshActionWait(FACEClientPlugin& P);
    bool PrepareCombatAction(FACEClientPlugin& P, const TSharedPtr<FJsonObject>& Intent, int32 Mode);
    void UpdateCombatTransition(FACEClientPlugin& P);
    void DrawRoute();
    bool IsDrivingMovement() const { return !MovementOwner.IsEmpty()||!FastCastOwner.IsEmpty()||!OptionalCastOwner.IsEmpty(); }
    bool IsFastBuffMovementActive() const { return !FastCastOwner.IsEmpty() && FastCastStarted; }
    void ObservePlayerTell(const FString& Text,const FString& Sender,int32 SenderId);
    TArray<TSharedPtr<FJsonValue>> BuffCommands() const;
    FString BuffRequestStatus;
    int32 QueuedBuffRequests() const { return BuffRequests.Num(); }
    void ClearBuffRequests();
    FString BuffQueueSummary() const;
    void ReplyBuffRequest(int32 Player, const FString& Name, const FString& Message, bool RateLimit = false);
    void NotifyBuffQueuePositions();
    bool RequestForceBuff();
    void CancelForceBuff();
    bool IsForceBuffRequested() const { return ForceBuffRequest!=0; }
    bool SetUCMFollowSelected(bool Enabled);
    bool IsUCMFollowEnabled() const;
    FString UCMCurrentStatus() const;

    void RecordWaypoint(const FString& Id, const TSharedPtr<FJsonObject>& Details = nullptr);
    void AddBuff(const FString& Id, int32 SpellId);
    TSharedPtr<FJsonObject> Snapshot();
    /** Called before the normal prediction/network pipeline; never teleports the pawn. */
    void ApplyMovement(AACEPlayerController* Controller, float& Forward, float& Right, float& Turn,
        bool Manual, bool Blocked, bool VR, const FVector& Facing, float MaxFrameTravel = 0.f);
private:
    bool SaveProfileData(const FString& Id, const FString& Name, TSharedPtr<FJsonObject> Profile, bool bStopRunning);
    void UpdateLedgers();
    bool UpdateLedgerCharacter();
    void SaveSupplies(bool Rebuild=true);
    TArray<FACESupplyEntry> LedgerSupplies;
    bool SupplyNamesVisible=true;
    FACEHuntLedger HuntLedger;
    FString LedgerCharacter;
    uint64 SupplyInventoryRevision=MAX_uint64;
    uint32 SupplyRevision=0;

    TSharedPtr<FACEClientPlugin> PolicyPlugin() const;
    bool ValidatePublicProfile(const TSharedPtr<FJsonObject>& Profile) const;
    bool InitializePluginState(FACEClientPlugin& Plugin);
    bool ApplyPluginServices(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Output, FString& Error);
    bool ValidatePluginUI(const TArray<TSharedPtr<FJsonValue>>& Rows, FString& Error) const;
    void QueuePluginEvent(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Event);
    TSharedPtr<FJsonObject> PublicSnapshot(FACEClientPlugin& Plugin);
    bool ApplyPublicOutput(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Output, FString& Error);
    bool StartCompanion(FACEClientPlugin& Plugin);
    void TickCompanions();
    int32 NextCompanionIndex = 0;
    void UpdateCompanionChatContext();
    TWeakPtr<class FACESession> PluginChatSession;
    int32 PluginChatPlayer=0;
    FString PluginChatServer;
    TDeque<FACEPluginChatEvent> PluginChatEvents;
    int32 PluginChatTextBytes=0;
    uint64 PluginChatSerial=0, PluginChatContextStartSerial=0;
    void StopCompanion(FACEClientPlugin& Plugin, const FString& Reason);
    void FlushPluginState(FACEClientPlugin& Plugin);
    TSharedPtr<FJsonObject> CompanionSnapshot(FACEClientPlugin& Plugin);
    // A synchronous observer pass shares read-only native data, never Lua
    // tables, reader cursors, events or state. Discard after any action/request.
    struct FCompanionReadBatch
    {
        TSharedPtr<FJsonObject> Data, Objects, TargetDetails;
        void Reset() { Data.Reset(); Objects.Reset(); TargetDetails.Reset(); }
    };
    FCompanionReadBatch* CompanionReadBatch = nullptr;
    bool ApplyCompanionOutput(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Output, FString& Error);
    bool CompanionAction(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Action, FString& Error);
    void DrawPluginMarkers();
    void AddTargetDetails(const TSharedPtr<FJsonObject>& Snapshot);
    void UpdatePluginWorldPlates();
    void RefreshPlateAppraisals();
    TArray<int32> ActivePlateTargets() const;
    FString ContentRoot(const FACEClientPlugin& Plugin) const;
    bool ChooseContentFolder(FACEClientPlugin& Plugin,FString& Error);
    bool ContentService(FACEClientPlugin& Plugin,const TSharedPtr<FJsonObject>& Request,const TSharedPtr<FJsonObject>& Reply,FString& Error);
    void UpdatePluginTerrain();
    void ClearPluginTerrain();
    double NextTerrainBuild=0, NextPlateAppraisal=0;
    FACEPluginTerrain TerrainBuiltSettings;
    FVector TerrainBuiltOrigin=FVector::ZeroVector;
    uint64 TerrainBuiltRevision=0;
    bool TerrainGeometryValid=false, RadarGeometryValid=false, TerrainBuiltIndoors=false, RadarBuiltIndoors=false;
    uint32 TerrainGeometryBuilds=0, RadarGeometryBuilds=0;
    UPROPERTY(Transient) TObjectPtr<AActor> TerrainOverlayActor;
    UPROPERTY(Transient) TObjectPtr<class UProceduralMeshComponent> TerrainOverlayMesh;
    UPROPERTY(Transient) TObjectPtr<class UProceduralMeshComponent> TerrainRadarMesh;
    UPROPERTY(Transient) TObjectPtr<class UPostProcessComponent> TerrainDungeonLight;
    void ClearPluginTarget(int32 Guid);
    struct FTargetHealthSample { float Fraction=0; double ReceivedAt=0; };
    TMap<int32,FTargetHealthSample> TargetHealthSamples;
    TMap<int32,double> TargetAppraisalTimes;
    UPROPERTY(Transient) TObjectPtr<AActor> WorldPlateActor;
    UPROPERTY(Transient) TArray<TObjectPtr<UACEPluginWorldPlateComponent>> WorldPlateComponents;
    TWeakPtr<class FACESession> CompanionSession;
    int32 CompanionPlayer=0;
    FString CompanionServer;
    uint32 PluginEventSerial=0;
    double NextPluginMarkerDraw=0;
    UPROPERTY(Transient) TObjectPtr<class UProceduralMeshComponent> PluginMarkerMesh;
    UPROPERTY(Transient) TObjectPtr<AActor> PluginMarkerActor;
    void UpdateItemToolsCharacter();
    TSharedPtr<FJsonObject> ItemToolsSnapshot(FACEClientPlugin* Policy = nullptr);
    bool ExecuteItemTools(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Intent);
    FString ItemToolsCharacter;
    int32 ItemToolsLastVendor=0;
    TMap<FString,TSharedRef<FACEPluginLocalService>> LocalServices;
    UPROPERTY() TMap<FString,TObjectPtr<class UACEPluginCosmetics>> LocalCosmetics;
    void CreateLocalCosmetic(FACEClientPlugin& Plugin);
    void ReleaseLocalCosmetics();
    bool StartLocalService(FACEClientPlugin& Plugin);
    void TickLocalServices();
    void UpdateRouteVisibility(const FACEPosition& Position, const TArray<TSharedPtr<FJsonValue>>& Route,
        const class FACEPluginSightQuery& Sight, const TSharedPtr<FJsonObject>& Snapshot);
    void RecordRouteMovement(const FACEPosition& Position, uint32 TeleportSequence);
    bool bTrackRouteMovement=false, bRouteMovementOverflow=false;
    TArray<FACEPosition> RouteMovementSamples;
    uint32 RouteMovementTeleport=0;
    void ObserveFollowPosition(int32 Guid,const FACEPosition& Position);
    TArray<FACEPosition> FollowMovementSamples;
    int32 FollowMovementTarget=0;
    uint32 FollowMovementTeleport=0;
    bool bFollowMovementOverflow=false;
    FACEUCMLog UCMLog;
    double NextUCMLogSave = 0;
    void FlushUCMLog();
    void LogUCMEvent(const FACEClientPlugin& Plugin, const FString& Message,
        EACEUCMLogLevel Level = EACEUCMLogLevel::Info, const TSharedPtr<FJsonObject>& Intent = nullptr);
    void LogUCMIntent(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Intent);
    void UpdateWorldRendering();
    void RestoreWorldRendering();
    TWeakObjectPtr<UGameViewportClient> SuspendedWorldViewport;
    bool PreviousWorldRenderingDisabled=false;
    bool Tick(float DeltaTime);
    void TickIdleEquipmentMana();
    void Execute(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Intent);
    void ReportActivityFailure(FACEClientPlugin& Plugin, const TSharedPtr<FJsonObject>& Intent, const FString& Reason, bool NotifyPolicy = true);
    void CheckPendingSpellTimeout();
    void SaveSettings();
    TSharedPtr<FACEClientPlugin> Find(const FString& Id) const;
    FTSTicker::FDelegateHandle Ticker;
    FTSTicker::FDelegateHandle DecisionWakeTicker;
    double DecisionWakeAt = 0, NextMaintenanceAt = 0;
    void ScheduleDecisionWake(double ReadyAt);
    bool RunDecisionWake(float DeltaTime);
    bool CanPauseLootApproach(const FACEClientPlugin& P) const;
    uint32 PluginOrderRevision = 0;
    TSharedPtr<class SACEPluginDesktop> DesktopPanel;
    TSharedPtr<class SACEPluginDesktop> DesktopDock;
    void UpdateDesktopDock();
    void RemoveDesktopDock();
    FString MovementOwner;
    FString FastCastOwner;
    FString OptionalCastOwner;
    double OptionalCastAt=0;
    bool OptionalCastItem=false,OptionalCastJump=false,OptionalCastStarted=false;
    void ConfigureCastMotion(const FACEClientPlugin& Plugin,const TSharedPtr<FJsonObject>& Intent,int32 Spell,bool Item);
    FString PendingSpellOwner, PendingSpellActivity;
    bool FastCastStarted = false;
    double FastCastStartedAt = 0;
    bool FastCastMovementApplied = false;
    FString PendingSpellConfirmation;
    TArray<FString> PendingSpellItemConfirmations;
    TArray<TPair<FString,FString>> PendingSpellRecoveryConfirmations;
    void PrepareSpellConfirmation(const FString& SpellName);
    bool PendingSpellConfirmed = false, PendingSpellFizzled = false;
    bool PendingBuffResultDriven = false, PendingSpellWords = false;
    double ConfirmedBuffAckAt = 0, BuffRetryAt = 0;
    FString ConfirmedBuffOwner, DeferredBuffOwner;
    TSharedPtr<FJsonObject> DeferredBuffIntent;
    void CompletePendingAction(uint32 Error);
    void CompleteConfirmedBuff();
    bool ResolvePluginSpellTarget(int32 Spell,int32 Selected,int32& Target) const;
    bool IsBuffCastIntent(const TSharedPtr<FJsonObject>& Intent) const;
    void UpdateBuffDispatch();
    bool LastSpellConfirmed = false;
    FString UseApproachOwner;
    uint32 ForceBuffSerial=0,ForceBuffRequest=0;
    bool bForceBuffOnly=false;
    bool bUCMCommandOnly=false;
    TArray<TSharedPtr<FJsonValue>> UCMCommands;
    TOptional<float> FaceHeading;
    FACEPosition MoveTarget;
    float MoveArrivalRadius = 70.f;
    bool bMoveArrived = false;
    FVector LastMovePosition = FVector::ZeroVector;
    double MoveExpires = 0, LastProgress = 0;
    TSharedPtr<FJsonObject> Settings;
    FString StorageRoot;
    TArray<FACEWaypointLandmark> WaypointLocations;
    bool bWaypointLocationsLoaded=false;
    TMap<int32, FACEAppraisalInfo> Appraisals;
    struct FEquipmentAssessment
    {
        FString Name;
        int32 WCID=0,Type=0,Setup=0,Icon=0,Material=0;
        double SavedAt=0;
        FACEAppraisalInfo Info;
    };
    TMap<int32,FEquipmentAssessment> EquipmentAssessments;
    bool EquipmentAssessmentsDirty=false;
    double NextEquipmentAssessmentSave=0;
    void RememberEquipmentAppraisal(const FACEWorldObject& Item,const FACEAppraisalInfo& Info);
    void SaveEquipmentAppraisals();
    void LoadEquipmentAppraisals();
    void AddEquipmentSnapshot(const FACEWorldObject& Item,const TSharedPtr<FJsonObject>& Out) const;
    TSharedPtr<FJsonObject> CachedSpeciesNames;
    TMap<int32, double> CorpseFirstSeen;
    struct FLootedCorpse { FACEPosition Position; double LastSeen=0; };
    TMap<int32, FLootedCorpse> LootedCorpses;
    TMap<int32, double> AppraisalRequests;
    TArray<TSharedPtr<FJsonValue>> CachedInventory;
    TMap<int32,int32> CachedInventoryIndices;
    TSet<int32> InventoryWieldInt64Keys;
    TSet<int32> DirtyInventoryAppraisals;
    uint64 InventoryRevision = MAX_uint64, InventoryAppraisalRevision = MAX_uint64;
    uint32 InventoryEligibility = 0;
    int32 DataPlayer = 0;
    FString DataServer;
    TWeakPtr<class FACESession> ObservedSession;
    FDelegateHandle UseDoneHandle;
    FDelegateHandle SpellCastHandle;
    int32 LastTargetEffectSpell=0,LastTargetEffectTarget=0;
    void ObserveTargetSpellCast(int32 Spell,int32 Target);
    void ForgetTargetEffects(int32 Target);
    FDelegateHandle TellHandle, MetaChatHandle, CombatFeedbackHandle;
    FDelegateHandle TargetHealthHandle, TargetMotionHandle, TargetDeletedHandle, TargetCreatedHandle, CombatVitalsHandle, FollowPositionHandle;
    int32 CombatTarget=0;
    TSet<int32> DefeatedTargets;
    TSharedPtr<FJsonObject> LastCombatDeath;
    void RecordCombatDeath(int32 Guid);
    FIntVector LastCombatVitals=FIntVector(-1,-1,-1);
    void ObserveTargetHealth(int32 Guid,float Fraction);
    void ObserveTargetMotion(int32 Guid,const FACEObjectMotionState& Motion);
    void ObserveTargetDeleted(int32 Guid);
    void ObserveTargetCreated(const FACEWorldObject& Object);
    void ObserveCombatVitals(const FACEPlayerVitals& Vitals);
    void ReleaseCombatTarget(int32 Guid);
    void WakeCombatDecision();
    void UnbindCombatEvents();
    void ObserveCombatFeedback(const FString& Name,int32 Damage,bool Incoming,bool Critical);
    void RecordCombatOutcome(bool Hit);
    void AddCombatOutcome(int32 Target,bool Hit,bool Unhittable=false,int32 Spell=0,int32 Damage=0);
    void ObserveOffensiveSpellChat(const FString& Text,const FString& Sender,int32 Type);
    void ExpireOffensiveCasts(double Now);
    struct FOffensiveCast
    {
        int32 Spell=0,Target=0;
        FString Owner,SpellName,TargetName;
        double Sent=0,Started=0;
    };
    TArray<FOffensiveCast> OffensiveCasts;
    int32 PhysicalAttackTarget=0;
    double PhysicalAttackUntil=0;
    double PhysicalAttackNextSendAt=0;
    int32 PhysicalAttackMode=0,PhysicalAttackHeight=0;
    float PhysicalAttackPower=0;
    FString PhysicalAttackOwner;
    uint32 CombatOutcomeSerial=0;
    TArray<TSharedPtr<FJsonValue>> CombatOutcomes;
    TArray<TSharedPtr<FJsonValue>> ActionOutcomes;
    void ObserveMetaChat(const FString& Text,const FString& Sender,int32 Type);
    TArray<TSharedPtr<FJsonValue>> MetaChatEvents,MetaPortalEvents;
    uint32 MetaChatSerial=0,MetaPortalSerial=0;
    bool MetaPortalSpace=false;
    TSharedPtr<FJsonObject> RuntimeRoute;
    TArray<TSharedPtr<FJsonValue>> BuffRequests;
    TMap<int32,double> LastBuffRequest;
    TMap<int32,double> LastBuffReply;
    uint32 BuffRequestSerial=0;
    double LastAppraisalRequest = 0;
    double NextAppraisalScan = 0;
    int32 PendingSpell = 0, PendingSpellTarget = 0;
    bool PendingManaRefresh = false;
    TSet<int32> PendingResourceRefresh;
    double PendingSpellAt = 0;
    TArray<TSharedPtr<FJsonValue>> ItemBuffs;
    void SaveItemBuffTimers();
    void LoadItemBuffTimers();
    void ValidateItemBuffTimers();
    int32 ItemBuffDeaths = 0;
    TArray<TSharedPtr<FJsonValue>> Debuffs, PendingDebuffCasts;
    uint32 DebuffRevision=0;
    int32 LastDebuffTarget=0,LastDebuffSpell=0;
    uint32 LastActionError = 0, ActionSerial = 0;
    int32 LastCompletedSpell = 0;
    UPROPERTY(Transient) TObjectPtr<class UProceduralMeshComponent> RouteMesh;
    UPROPERTY(Transient) TObjectPtr<AActor> RouteActor;
    TArray<FACEPluginRouteSegment> RouteSegments;
    int32 RoutePoint=1;
    uint32 MovementBlockedSerial=0;
    bool bRouteJoinRequested=true;
    int32 RouteVisibilityOffset=0;
    FVector RouteVisibilityOrigin=FVector::ZeroVector;
    uint32 RouteVisibilitySignature=0, RouteVisibilityCell=0;
    uint32 RouteSignature=0;
    double RouteRebuiltAt=0;
    double NextRouteCheck=0;

    TArray<int32> CachedSpellIds, CachedSchoolSkills;
    TArray<TSharedPtr<FJsonValue>> CachedSpells;
    TArray<TSharedPtr<FJsonValue>> CachedKnownSpellValues;
    uint64 CachedSpellRevision = MAX_uint64;
    TSharedPtr<class FACEPluginVM> IdleManaVM;
    FString IdleManaOwner;
    TWeakPtr<class FACESession> IdleManaSession;
    uint32 IdleManaProfileHash=0;
    int32 IdleManaPlayer=0;
    double NextIdleManaDecision=0;
    bool IdleManaFailed=false;
};
