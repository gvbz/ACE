#include "Mods/ACEPluginPresentation.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEPluginVM.h"
#include "ACEClientChatCommands.h"
#include "ACEWaypoint.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEDatSubsystem.h"
#include "ACEWorldPresenterComponent.h"
#include "ACEWorldEntityActor.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "ProceduralMeshComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
namespace ACEPluginCompanionPrivate
{
    // Callers use explicit local S/N declarations: a unity translation unit
    // can also contain the automation host's identically named helpers.
    FString S(const TSharedPtr<FJsonObject>& O,const TCHAR* Key)
    {FString V;if(O)O->TryGetStringField(Key,V);return V;}
    double N(const TSharedPtr<FJsonObject>& O,const TCHAR* Key)
    {double V=0;if(O)O->TryGetNumberField(Key,V);return FMath::IsFinite(V)?V:0;}
    int32 Id(const TSharedPtr<FJsonObject>& O,const TCHAR* Key)
    {const double V=N(O,Key);return V>0&&V<=MAX_uint32&&V==FMath::FloorToDouble(V)?int32(uint32(V)):0;}
    FString Encode(const TSharedPtr<FJsonObject>& O)
    {FString Text;FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));return Text;}
    bool List(const TSharedPtr<FJsonObject>& O,const TCHAR* Key,int32 Max,const TArray<TSharedPtr<FJsonValue>>*& Out)
    {return O->TryGetArrayField(Key,Out)&&Out->Num()<=Max;}
    bool SameArea(const FACEPosition& A,const FACEPosition& B)
    {return A.IsValid()&&B.IsValid()&&((uint32(A.CellId)>>16)==(uint32(B.CellId)>>16)||((uint32(A.CellId)&65535)<256&&(uint32(B.CellId)&65535)<256));}
    TSharedPtr<FJsonObject> Position(const FACEPosition& P)
    {auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("cell"),uint32(P.CellId));O->SetNumberField(TEXT("x"),P.Location.X);O->SetNumberField(TEXT("y"),P.Location.Y);O->SetNumberField(TEXT("z"),P.Location.Z);return O;}
    void Event(TArray<TSharedPtr<FJsonValue>>& Queue,const TSharedPtr<FJsonObject>& O,uint32 Serial)
    {O->SetNumberField(TEXT("serial"),Serial);if(Queue.Num()>=64)Queue.RemoveAt(0);Queue.Add(MakeShared<FJsonValueObject>(O));}
    bool Color(const FString& Name,FLinearColor& Out)
    {
        static const TMap<FString,FLinearColor> Colors={{TEXT("green"),FLinearColor(.15f,1,.3f)},{TEXT("yellow"),FLinearColor(1,.8f,.1f)},
            {TEXT("red"),FLinearColor(1,.15f,.1f)},{TEXT("blue"),FLinearColor(.15f,.6f,1)},{TEXT("white"),FLinearColor::White},{TEXT("purple"),FLinearColor(.8f,.3f,1)}};
        if(const auto* C=Colors.Find(Name)){Out=*C;return true;}return false;
    }
}
} // anonymous namespace

void UACEPluginSubsystem::UpdateCompanionChatContext()
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const int32 Player=(C->GetSessionState()==EACESessionState::InWorld||C->GetSessionState()==EACESessionState::EnteringWorld)?C->GetPlayerGuid():0;
    if(PluginChatSession.Pin()!=C->GetSession()||PluginChatPlayer!=Player||PluginChatServer!=C->GetServerName())
    {
        PluginChatEvents.Reset();PluginChatTextBytes=0;PluginChatContextStartSerial=PluginChatSerial;
        // Logging back into the same character can reuse the session object.
        // Abandoned pages belong to the old login, not this reader's backlog.
        for(const auto& P:Plugins){P->ChatCursor=PluginChatContextStartSerial;P->ChatFilters.Reset();P->ChatInbox.Empty();P->ChatInboxBytes=0;P->ChatInboxDropped=0;}
        PluginChatSession=C->GetSession();PluginChatPlayer=Player;PluginChatServer=C->GetServerName();
    }
}

void UACEPluginSubsystem::ObservePluginChat(const FString& Text,const FString& Sender,int32 Type)
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    UpdateCompanionChatContext();
    if(!PluginChatPlayer||!Plugins.ContainsByPredicate([](const auto& P){return P->HasPresentation()&&P->Enabled&&P->Permissions.Contains(TEXT("chat"));}))return;
    // Subscribe to the client feed at initialization, before its first world
    // maintenance tick. This is independent of UCM, appraisals and UI filters.
    FString Line=Text;
    if(Type==ACEChatMessageType::Tell)Line=FString::Printf(TEXT("%s tells you, \"%s\""),Sender.IsEmpty()?TEXT("Someone"):*Sender,*Text);
    else if(Type==ACEChatMessageType::Speech&&!Sender.IsEmpty())Line=Sender.Equals(TEXT("You"),ESearchCase::IgnoreCase)?FString::Printf(TEXT("You say, \"%s\""),*Text):FString::Printf(TEXT("%s says, \"%s\""),*Sender,*Text);
    else if(Type==ACEChatMessageType::Emote&&!Sender.IsEmpty())Line=Sender+TEXT(" ")+Text;
    Line=Line.Left(4096);const FString Raw=Text.Left(4096),Speaker=Sender.Left(256);
    const int32 Bytes=FTCHARToUTF8(*Line).Length()+FTCHARToUTF8(*Raw).Length()+FTCHARToUTF8(*Speaker).Length();
    auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("serial"),++PluginChatSerial);E->SetStringField(TEXT("text"),Line);
    E->SetStringField(TEXT("raw_text"),Raw);E->SetStringField(TEXT("sender"),Speaker);E->SetNumberField(TEXT("color"),Type);E->SetNumberField(TEXT("time"),FPlatformTime::Seconds());
    E->SetStringField(TEXT("sender_type"),PluginChatSenderType(Sender));E->SetNumberField(TEXT("chat_type"),Type);
    // Receive bursts independently of Lua's callback budget. A deque avoids
    // shifting thousands of JSON references on every message once full.
    while(!PluginChatEvents.IsEmpty()&&(PluginChatEvents.Num()>=ACEPluginChatLimits::RetainedEvents
        ||PluginChatTextBytes+Bytes>ACEPluginChatLimits::RetainedTextBytes))
    {PluginChatTextBytes-=PluginChatEvents.First().TextBytes;PluginChatEvents.PopFirst();}
    PluginChatEvents.EmplaceLast(FACEPluginChatEvent{PluginChatSerial,Bytes,MakeShared<FJsonValueObject>(E)});
    PluginChatTextBytes+=Bytes;
    for(const auto& P:Plugins)if(P->Enabled&&P->Running&&P->SelectiveChat&&P->Permissions.Contains(TEXT("chat")))DeliverPluginChat(*P,PluginChatEvents.Last());
}

void UACEPluginSubsystem::DeliverPluginChat(FACEClientPlugin& P,const FACEPluginChatEvent& Event)
{
    const auto E=Event.Value->AsObject();
    if(!P.ChatSubscriptions.ContainsByPredicate([&](const auto& F){return MatchesPluginChat(F,E->GetStringField(TEXT("raw_text")),E->GetStringField(TEXT("sender")),int32(E->GetNumberField(TEXT("color"))),E->GetStringField(TEXT("sender_type")));}))return;
    while(!P.ChatInbox.IsEmpty()&&(P.ChatInbox.Num()>=2048||P.ChatInboxBytes+Event.TextBytes>2*1024*1024))
    {P.ChatInboxBytes-=P.ChatInbox.First().TextBytes;P.ChatInbox.PopFirst();++P.ChatInboxDropped;}
    P.ChatInbox.EmplaceLast(Event);P.ChatInboxBytes+=Event.TextBytes;
}

bool UACEPluginSubsystem::StartCompanion(FACEClientPlugin& P)
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!P.Enabled||C->GetSessionState()!=EACESessionState::InWorld||!C->GetPlayerGuid())
    {Notice=TEXT("Enable this companion and enter the world first.");return false;}
    if(P.Running)return true;
    if(P.Api>=2&&!ValidatePublicProfile(P.Profile)){P.CompanionStopped=true;P.Status=TEXT("Invalid public profile structures");return false;}
    if(!InitializePluginState(P)){P.CompanionStopped=true;return false;}
    FString Source,Error;auto VM=MakeShared<FACEPluginVM>();
    if(IFileManager::Get().FileSize(*(P.Directory/TEXT("main.lua")))>256*1024||!FFileHelper::LoadFileToString(Source,*(P.Directory/TEXT("main.lua")))||!VM->Load(Source,Error))
    {P.CompanionStopped=true;P.Status=TEXT("Script load failed: ")+Error;return false;}
    P.VM=VM;P.VMSession=C->GetSession();P.Player=C->GetPlayerGuid();P.Server=C->GetServerName();
    P.Running=true;P.CompanionStopped=false;P.NextDecision=0;P.NextCommand=0;P.NextStateSave=FPlatformTime::Seconds()+5;
    // Start with events received since entering this character, not a previous login.
    P.Status=TEXT("Running");P.CompanionError.Empty();return true;
}

bool UACEPluginSubsystem::InitializePluginState(FACEClientPlugin& P)
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    UpdateCompanionChatContext();
    const bool NewCharacter=P.VMSession.Pin()!=C->GetSession()||P.Player!=C->GetPlayerGuid()||P.Server!=C->GetServerName();
    FlushPluginState(P);
    if(P.StateDirty){Notice=P.CompanionError;return false;}
    const FString Character=FString::Printf(TEXT("%s_%08X"),*FMD5::HashAnsiString(*C->GetServerName()),uint32(C->GetPlayerGuid()));
    P.StatePath=UserDirectory()/TEXT("State")/P.Id/(Character+TEXT(".json"));P.SavedState=MakeShared<FJsonObject>();
    if(P.Permissions.Contains(TEXT("state"))&&IFileManager::Get().FileExists(*P.StatePath))
    {
        FString Text;TSharedPtr<FJsonObject> Loaded;
        if(IFileManager::Get().FileSize(*P.StatePath)>65536||!FFileHelper::LoadFileToString(Text,*P.StatePath)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Loaded))
        {P.Status=TEXT("Cannot read saved state; original file preserved");return false;}
        P.SavedState=Loaded;
    }
    if(NewCharacter){P.ChatCursor=FMath::Max(P.ChatCursor,P.ChatHistory?PluginChatContextStartSerial:PluginChatSerial);P.Presentation.Reset();P.ChatFilters.Reset();}
    if(!P.ChatHistory)P.ChatCursor=PluginChatSerial;
    if(P.SelectiveChat)
    {
        P.ChatInbox.Empty();P.ChatInboxBytes=0;P.ChatInboxDropped=0;
        if(P.ChatHistory)for(const auto& E:PluginChatEvents)if(E.Serial>P.ChatCursor)DeliverPluginChat(P,E);
    }
    P.NextStateSave=FPlatformTime::Seconds()+5;
    return true;
}

void UACEPluginSubsystem::FlushPluginState(FACEClientPlugin& P)
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    if(!P.StateDirty||!P.SavedState||P.StatePath.IsEmpty())return;
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(P.StatePath),true);
    const FString Temp=P.StatePath+TEXT(".tmp");
    if(FFileHelper::SaveStringToFile(Encode(P.SavedState),*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        &&IFileManager::Get().Move(*P.StatePath,*Temp,true,true))P.StateDirty=false;
    else P.CompanionError=TEXT("Could not save plugin state; will retry");
}

void UACEPluginSubsystem::StopCompanion(FACEClientPlugin& P,const FString& Reason)
{
    ResetClientMessaging(P);
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    FlushPluginState(P);P.Running=false;P.CompanionStopped=true;P.VM.Reset();
    P.ChatInbox.Empty();P.ChatInboxBytes=0;P.ChatInboxDropped=0;
    P.Terrain={};P.Markers.Reset();P.WorldPlates.Reset();++P.WorldPlateRevision;P.PanelRows.Reset();P.PluginEvents.Reset();P.ChatFilters.Reset();++P.PanelRevision;P.Status=Reason+(P.StateDirty?TEXT("; ")+P.CompanionError:FString());
    // Observational plugin lifecycle never cancels UCM movement, casts or attacks.
}

TSharedPtr<FJsonObject> UACEPluginSubsystem::CompanionSnapshot(FACEClientPlugin& P)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginObserverSnapshot);
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Out=MakeShared<FJsonObject>();
    FACEPosition Player=C->GetPlayerPosition();if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))PC->TryGetLocallyPredictedPosition(Player);
    Out->SetNumberField(TEXT("api"),P.Api);Out->SetNumberField(TEXT("unix_time"),FDateTime::UtcNow().ToUnixTimestamp());Out->SetNumberField(TEXT("time"),FPlatformTime::Seconds());Out->SetNumberField(TEXT("player"),uint32(C->GetPlayerGuid()));Out->SetStringField(TEXT("server"),C->GetServerName());
    Out->SetObjectField(TEXT("position"),Position(Player));Out->SetNumberField(TEXT("selected"),uint32(C->GetSelectedObject().Guid));
    const auto& V=C->GetPlayerVitalsView();Out->SetNumberField(TEXT("health"),V.Health);Out->SetNumberField(TEXT("max_health"),V.MaxHealth);
    Out->SetNumberField(TEXT("mana"),V.Mana);Out->SetNumberField(TEXT("max_mana"),V.MaxMana);Out->SetNumberField(TEXT("stamina"),V.Stamina);Out->SetNumberField(TEXT("max_stamina"),V.MaxStamina);
    Out->SetObjectField(TEXT("state"),P.SavedState?P.SavedState:MakeShared<FJsonObject>());Out->SetStringField(TEXT("error"),P.CompanionError);P.CompanionError.Empty();
    Out->SetArrayField(TEXT("events"),P.PluginEvents);P.PluginEvents.Reset();
    TArray<TSharedPtr<FJsonValue>> Chat;
    if(P.Permissions.Contains(TEXT("chat")))
    {
        uint64 Dropped=0;int32 Bytes=0;
        if(P.SelectiveChat)
        {
            Dropped=P.ChatInboxDropped;P.ChatInboxDropped=0;
            while(!P.ChatInbox.IsEmpty()&&Chat.Num()<ACEPluginChatLimits::BatchEvents)
            {
                const auto E=P.ChatInbox.First();
                if(!Chat.IsEmpty()&&Bytes+E.TextBytes>ACEPluginChatLimits::BatchTextBytes)break;
                Chat.Add(E.Value);Bytes+=E.TextBytes;P.ChatCursor=E.Serial;P.ChatInboxBytes-=E.TextBytes;P.ChatInbox.PopFirst();
            }
            if(P.ChatInbox.IsEmpty())P.ChatCursor=PluginChatSerial;
        }
        else if(!PluginChatEvents.IsEmpty())
        {
            const uint64 First=PluginChatEvents.First().Serial;
            if(First>P.ChatCursor+1)Dropped=First-P.ChatCursor-1;
            // Serials are contiguous within this character's retained deque.
            // Index directly to this reader's cursor; do not rescan the backlog.
            const int32 Start=P.ChatCursor<First?0:int32(FMath::Min<uint64>(P.ChatCursor-First+1,PluginChatEvents.Num()));
            for(int32 I=Start;I<PluginChatEvents.Num()&&Chat.Num()<ACEPluginChatLimits::BatchEvents;++I)
            {
                const auto& E=PluginChatEvents[I];
                if(!Chat.IsEmpty()&&Bytes+E.TextBytes>ACEPluginChatLimits::BatchTextBytes)break;
                Chat.Add(E.Value);Bytes+=E.TextBytes;P.ChatCursor=E.Serial;
            }
        }
        const uint64 Pending=P.SelectiveChat?P.ChatInbox.Num():PluginChatSerial>P.ChatCursor?PluginChatSerial-P.ChatCursor:0;
        Out->SetBoolField(TEXT("chat_gap"),Dropped>0);Out->SetNumberField(TEXT("chat_dropped"),Dropped);
        Out->SetNumberField(TEXT("chat_serial"),P.ChatCursor);Out->SetNumberField(TEXT("chat_latest_serial"),PluginChatSerial);
        Out->SetNumberField(TEXT("chat_pending"),Pending);Out->SetBoolField(TEXT("chat_more"),Pending>0);
    }
    Out->SetArrayField(TEXT("chat_events"),Chat);
    // No inventory appraisals, spell indexing or visibility raycasts for companions.
    if(!P.WantsSnapshotField(TEXT("objects")) && !P.WantsSnapshotField(TEXT("objects_truncated"))){}
    else if(CompanionReadBatch && CompanionReadBatch->Objects)
    {
        Out->SetField(TEXT("objects"),CompanionReadBatch->Objects->Values[TEXT("objects")]);
        Out->SetBoolField(TEXT("objects_truncated"),CompanionReadBatch->Objects->GetBoolField(TEXT("objects_truncated")));
    }
    else
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginNearbyObjects);
        TArray<TPair<double,const FACEWorldObject*>> Nearby;
        if(auto Session=C->GetSession())for(const auto& Pair:Session->GetWorldObjects())
        {
            const auto& O=Pair.Value;if(O.Guid==C->GetPlayerGuid()||!O.IsSelectableWorldObject()||!SameArea(Player,O.Position))continue;
            const double D=FVector::DistSquared(Player.ToUnrealLocation(),O.Position.ToUnrealLocation());
            if(D<=FMath::Square(24000.))Nearby.Emplace(D,&O);
        }
        Nearby.Sort([](const auto& A,const auto& B){return A.Key==B.Key?uint32(A.Value->Guid)<uint32(B.Value->Guid):A.Key<B.Key;});
        TArray<TSharedPtr<FJsonValue>> Objects;
        Out->SetBoolField(TEXT("objects_truncated"),Nearby.Num()>512);
        for(int I=0;I<FMath::Min(Nearby.Num(),512);++I)
        {
            const auto& O=*Nearby[I].Value;auto Row=MakeShared<FJsonObject>();Row->SetNumberField(TEXT("id"),uint32(O.Guid));Row->SetStringField(TEXT("name"),O.Name.Left(256));
            Row->SetNumberField(TEXT("wcid"),O.WeenieClassId);Row->SetNumberField(TEXT("distance"),FMath::Sqrt(Nearby[I].Key)/100.);
            Row->SetBoolField(TEXT("monster"),O.IsAttackable()&&!O.bIsPlayer&&!O.PetOwnerId&&(O.ItemType&ACEItemType::Creature)&&!O.bDying);
            Row->SetStringField(TEXT("object_type"),ACEPluginWorldPlate::ObjectType(O));
            Row->SetBoolField(TEXT("player"),O.bIsPlayer);Row->SetBoolField(TEXT("corpse"),O.IsCorpse());Row->SetBoolField(TEXT("dead"),O.bDying);
            Row->SetObjectField(TEXT("position"),Position(O.Position));Objects.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("objects"),Objects);
        if(CompanionReadBatch)
        {
            CompanionReadBatch->Objects=MakeShared<FJsonObject>();
            CompanionReadBatch->Objects->SetField(TEXT("objects"),Out->Values[TEXT("objects")]);
            CompanionReadBatch->Objects->SetBoolField(TEXT("objects_truncated"),Nearby.Num()>512);
        }
    }
    if(P.WantsSnapshotField(TEXT("selection")) || P.WantsSnapshotField(TEXT("tracked_targets")) || P.WantsSnapshotField(TEXT("target_effects")))AddTargetDetails(Out);
    auto Peers=MakeShared<FJsonObject>();for(const auto& Other:Plugins){auto Info=MakeShared<FJsonObject>();Info->SetBoolField(TEXT("enabled"),Other->Enabled);Info->SetBoolField(TEXT("running"),Other->Running);Peers->SetObjectField(Other->Id,Info);}Out->SetObjectField(TEXT("plugins"),Peers);
    return Out;
}

bool UACEPluginSubsystem::CompanionAction(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& A,FString& Error)
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    const FString Kind=S(A,TEXT("action"));if(Kind.IsEmpty())return true;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!P.Enabled||!P.Running||C->GetSessionState()!=EACESessionState::InWorld||C->GetPlayerGuid()!=P.Player||C->GetServerName()!=P.Server)
    {Error=TEXT("Plugin is not active on this character");return false;}
    const FString Permission=Kind==TEXT("identify")?TEXT("identify"):(Kind==TEXT("select")||Kind==TEXT("select_item"))?TEXT("ui"):Kind==TEXT("waypoint")?TEXT("waypoint"):Kind==TEXT("command")?TEXT("commands"):FString();
    if(Permission.IsEmpty()||!P.Permissions.Contains(Permission)){Error=TEXT("Companion action not permitted: ")+Kind;return false;}
    if(Kind==TEXT("identify"))
    {
        FACEWorldObject Item;const double Now=FPlatformTime::Seconds();
        if(Now<P.NextCommand){Error=TEXT("Identify rate limit (one per second)");return false;}
        if(!C->GetWorldObject(Id(A,TEXT("target")),Item)||(!C->IsOwnedInventoryItem(Item)
            &&(!Item.IsSelectableWorldObject()||!SameArea(C->GetPlayerPosition(),Item.Position)
                ||FVector::DistSquared(C->GetPlayerPosition().ToUnrealLocation(),Item.Position.ToUnrealLocation())>FMath::Square(24000.))))
        {Error=TEXT("Identify target is no longer nearby or owned");return false;}
        P.NextCommand=Now+1;
        // Observers request one appraisal without taking UCM's action ownership
        // or changing the player's selection/examination panel.
        C->GetSession()->SendIdentifyObject(Item.Guid,true);return true;
    }
    if(Kind==TEXT("select")||Kind==TEXT("select_item")||Kind==TEXT("waypoint"))
    {
        FACEWorldObject O;
        const bool Selecting=Kind!=TEXT("waypoint");
        const bool Found=C->GetWorldObject(Id(A,TEXT("target")),O),Owned=Found&&P.Api>=2&&Selecting&&IsOwnedPluginItem(O);
        bool InContainer=false;
        if(Found && P.Api>=2 && Selecting)
        {
            const auto Session=C->GetSession();
            int32 Container=O.ContainerId;
            const int32 Requested=Id(A,TEXT("container"));
            if(A->HasField(TEXT("container")))
            {
                if(!Requested||!A->HasTypedField<EJson::Number>(TEXT("container")))
                {Error=TEXT("container must be a positive object ID");return false;}
                const auto* Contents=Session->GetContainerContents(Requested);
                // When present, the server's contents list is authoritative.
                const bool Member=Contents?Contents->ContainsByPredicate([&](const auto& Item){return Item.ItemGuid==O.Guid;}):Container==Requested;
                if(!Member){Error=TEXT("Item is no longer in the requested container");return false;}
                Container=Requested;
            }
            TSet<int32> Seen;int32 Child=O.Guid;
            for(int32 Depth=0;Container && Depth<32 && !Seen.Contains(Container);++Depth)
            {
                Seen.Add(Container);
                if(const auto* Contents=Session->GetContainerContents(Container))
                    if(!Contents->ContainsByPredicate([&](const auto& Item){return Item.ItemGuid==Child;}))
                    {Error=TEXT("Item is no longer in the container");return false;}
                if(Container==C->GetPlayerGuid() || Container==C->GetOpenExternalContainerGuid())
                {InContainer=true;break;}
                FACEWorldObject Parent;
                if(!C->GetWorldObject(Container,Parent))break;
                Child=Container;Container=Parent.ContainerId;
            }
        }
        if(Kind==TEXT("select_item")&&!InContainer)
        {Error=TEXT("Item must be in your inventory or a currently open container");return false;}
        if(!Found||(!Owned&&!InContainer&&(!O.IsSelectableWorldObject()||!SameArea(C->GetPlayerPosition(),O.Position)
            ||FVector::DistSquared(C->GetPlayerPosition().ToUnrealLocation(),O.Position.ToUnrealLocation())>FMath::Square(24000.))))
        {Error=TEXT("Target is no longer nearby");return false;}
        if(Selecting)
        {
            if(O.IsUiHidden()||O.IsUnnamedDecoration()){Error=TEXT("Item cannot be selected");return false;}
            if(Owned||InContainer)C->SelectInventoryObject(O.Guid);else C->SelectObject(O.Guid);
            return C->GetSelectedObject().Guid==O.Guid;
        }
        // The navigation HUD is a host service, including when the atlas is absent.
        const FVector2D Coordinates=ACEWaypoint::Coordinates(O.Position);const uint32 Dungeon=(uint32(O.Position.CellId)&65535)>=256?(uint32(O.Position.CellId)&0xFFFF0000):0;
        FVector2D Previous;uint32 PreviousDungeon=0;
        if(!IsWaypointEnabled()||!GetWaypoint(Previous,PreviousDungeon)||PreviousDungeon!=Dungeon||!Coordinates.Equals(Previous,.002))SetWaypoint(Coordinates,Dungeon,true);
        return true;
    }
    if(FPlatformTime::Seconds()<P.NextCommand){Error=TEXT("Command rate limit (one per second)");return false;}
    const FString Text=ACEClientChatCommands::CanonicalLine(S(A,TEXT("text")).TrimStartAndEnd());FString Command,Arguments;
    Text.Split(TEXT(" "),&Command,&Arguments);if(Command.IsEmpty())Command=Text;Command=Command.ToLower();Arguments=Arguments.TrimStartAndEnd();
    if(Text.Len()>512||Text.Contains(TEXT("\n"))||Text.Contains(TEXT("\r"))){Error=TEXT("Invalid command");return false;}
    P.NextCommand=FPlatformTime::Seconds()+1;
    bool Ok=false;
    if(Command==TEXT("/acueucm")){auto Peer=Find(TEXT("ucm"));if(Peer&&Peer->Enabled)Ok=RunUCMCommand(Arguments);}
    else if(Command==TEXT("/acueitems")){auto Peer=Find(TEXT("itemtools"));if(Peer&&Peer->Enabled)Ok=RunItemToolsCommand(Arguments);}
    else if(Command==TEXT("/myquests")&&Arguments.IsEmpty()){C->SendChatMessage(TEXT("/myquests"));return true;}
    else {Error=TEXT("Supported commands: /acueucm, /acueitems, /myquests");return false;}
    if(!Ok)Error=Notice.IsEmpty()?TEXT("Command target must be enabled and configured"):Notice;return Ok;
}

bool UACEPluginSubsystem::ApplyCompanionOutput(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& O,FString& Error)
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    // Validate the complete presentation before replacing the current one.
    const TSharedPtr<FJsonObject>* Presentation=nullptr;
    if(O->HasField(TEXT("presentation")))
    {
        if(!P.Permissions.Contains(TEXT("ui"))||!O->TryGetObjectField(TEXT("presentation"),Presentation))
        {Error=TEXT("presentation requires ui permission and an object");return false;}
        if(!ACEPluginPresentation::Valid(*Presentation,Error))return false;
    }
    const TSharedPtr<FJsonObject>* State=nullptr;const TArray<TSharedPtr<FJsonValue>> *Rows=nullptr,*Markers=nullptr,*Messages=nullptr;
    if(O->HasField(TEXT("state"))&&(!P.Permissions.Contains(TEXT("state"))||!O->TryGetObjectField(TEXT("state"),State)||FTCHARToUTF8(*Encode(*State)).Length()>65536))
    {Error=TEXT("State requires state permission and an object of at most 64 KiB");return false;}
    if(O->HasField(TEXT("ui")))
    {
        if(!P.Permissions.Contains(TEXT("ui"))||!List(O,TEXT("ui"),128,Rows)){Error=TEXT("UI requires ui permission and at most 128 rows");return false;}
        if(!ValidatePluginUI(*Rows,Error))return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* ChatFilters=nullptr;
    TArray<FACEPluginChatFilter> ParsedFilters;
    const TArray<TSharedPtr<FJsonValue>>* Subscriptions=nullptr;TArray<FACEPluginChatFilter> ParsedSubscriptions;
    if(O->HasField(TEXT("chat_subscriptions")))
    {
        if(!P.Permissions.Contains(TEXT("chat"))||!List(O,TEXT("chat_subscriptions"),32,Subscriptions))
        {Error=TEXT("chat_subscriptions requires chat permission and at most 32 rules");return false;}
        if(!ValidatePluginChatFilters(*Subscriptions,ParsedSubscriptions,Error))return false;
    }
    if(O->HasField(TEXT("chat_filters")))
    {
        if(!P.Permissions.Contains(TEXT("chat_filter"))||!List(O,TEXT("chat_filters"),32,ChatFilters))
        {Error=TEXT("chat_filters requires chat_filter permission and at most 32 rules");return false;}
        if(!ValidatePluginChatFilters(*ChatFilters,ParsedFilters,Error))return false;
    }
    FACEPluginTerrain ParsedTerrain;
    if(O->HasField(TEXT("terrain_overlay")))
    {
        const TSharedPtr<FJsonObject>* Terrain=nullptr;
        if(!P.Permissions.Contains(TEXT("terrain")) || !O->TryGetObjectField(TEXT("terrain_overlay"),Terrain))
        {Error=TEXT("terrain_overlay requires terrain permission and an object");return false;}
        if(!ACEPluginTerrain::Parse(*Terrain,ParsedTerrain,Error))return false;
    }
    TArray<FACEPluginMarker> Parsed;
    const TArray<TSharedPtr<FJsonValue>>* Plates=nullptr;
    TArray<FACEPluginWorldPlate> ParsedPlates;
    if(O->HasField(TEXT("world_plates")))
    {
        if(!P.Permissions.Contains(TEXT("ui"))||!P.Permissions.Contains(TEXT("markers"))||!List(O,TEXT("world_plates"),4,Plates))
        {Error=TEXT("world_plates requires ui and markers permissions and at most four plates");return false;}
        if(!ACEPluginWorldPlate::Parse(*Plates,ParsedPlates,Error,FPlatformTime::Seconds()))return false;
    }
    if(O->HasField(TEXT("markers")))
    {
        if(!P.Permissions.Contains(TEXT("markers"))||!List(O,TEXT("markers"),32,Markers)){Error=TEXT("Markers require markers permission and at most 32 entries");return false;}
        for(const auto& V:*Markers)
        {
            if(V->Type!=EJson::Object){Error=TEXT("Invalid marker");return false;}const auto M=V->AsObject();FACEPluginMarker Result;Result.Id=Id(M,TEXT("id"));Result.Shape=S(M,TEXT("shape"));
            if(!Result.Id||!TArray<FString>{TEXT("arrow"),TEXT("!"),TEXT("diamond")}.Contains(Result.Shape)||!Color(S(M,TEXT("color")),Result.Color)){Error=TEXT("Invalid marker ID, shape or color");return false;}
            M->TryGetBoolField(TEXT("world"),Result.World);M->TryGetBoolField(TEXT("map"),Result.Map);Parsed.Add(Result);
        }
    }
    if(O->HasField(TEXT("messages")))
    {
        if(!P.Permissions.Contains(TEXT("messaging"))||!List(O,TEXT("messages"),8,Messages)){Error=TEXT("Messaging requires permission and at most eight messages per tick");return false;}
        for(const auto& V:*Messages)if(V->Type!=EJson::Object||S(V->AsObject(),TEXT("to")).Len()>64||S(V->AsObject(),TEXT("topic")).Len()>64||S(V->AsObject(),TEXT("text")).Len()>1024)
        {Error=TEXT("Invalid plugin message");return false;}
    }
    if(O->HasField(TEXT("terrain_overlay")))P.Terrain=ParsedTerrain;
    if(Presentation)P.Presentation=*Presentation;
    if(ChatFilters)P.ChatFilters=MoveTemp(ParsedFilters);
    if(Subscriptions)
    {
        const bool Same=P.SelectiveChat&&P.ChatSubscriptions.Num()==ParsedSubscriptions.Num()&&[&]()
        {
            for(int32 I=0;I<ParsedSubscriptions.Num();++I)
            {
                const auto& A=P.ChatSubscriptions[I];const auto& B=ParsedSubscriptions[I];
                if(A.Id!=B.Id||A.Contains!=B.Contains||A.Prefix!=B.Prefix||A.Exact!=B.Exact||A.Pattern!=B.Pattern
                    ||A.Sender!=B.Sender||A.HasSender!=B.HasSender||A.SenderType!=B.SenderType||A.ChatType!=B.ChatType||A.CaseSensitive!=B.CaseSensitive)return false;
            }
            return true;
        }();
        P.ChatSubscriptions=MoveTemp(ParsedSubscriptions);P.SelectiveChat=true;
        if(!Same){P.ChatInbox.Empty();P.ChatInboxBytes=0;P.ChatInboxDropped=0;P.ChatCursor=PluginChatSerial;}
    }
    if(State&&(!P.SavedState||!FJsonValue::CompareEqual(FJsonValueObject(P.SavedState),FJsonValueObject(*State)))){P.SavedState=*State;P.StateDirty=true;}
    if(Rows&&!FJsonValue::CompareEqual(FJsonValueArray(P.PanelRows),FJsonValueArray(*Rows))){P.PanelRows=*Rows;++P.PanelRevision;}
    if(Markers)P.Markers=MoveTemp(Parsed);
    if(Plates){P.WorldPlates=MoveTemp(ParsedPlates);++P.WorldPlateRevision;}
    if(Messages)for(const auto& V:*Messages)
    {
        const auto M=V->AsObject();auto Peer=Find(S(M,TEXT("to")));
        if(!Peer||!Peer->HasPresentation()||!Peer->Running||!Peer->Permissions.Contains(TEXT("messaging"))){P.CompanionError=TEXT("Message recipient is unavailable or lacks messaging permission");continue;}
        auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("type"),TEXT("message"));E->SetStringField(TEXT("sender"),P.Id);E->SetStringField(TEXT("topic"),S(M,TEXT("topic")));E->SetStringField(TEXT("text"),S(M,TEXT("text")));Event(Peer->PluginEvents,E,++PluginEventSerial);
    }
    if(O->HasField(TEXT("status")))P.Status=S(O,TEXT("status"));
    if(P.Companion&&S(O,TEXT("action"))==TEXT("stop")){StopCompanion(P,P.Status);return true;}
    FString ActionError;if((P.Companion||S(O,TEXT("action"))==TEXT("command")||S(O,TEXT("action"))==TEXT("waypoint")||S(O,TEXT("action"))==TEXT("select_item"))&&!CompanionAction(P,O,ActionError))P.CompanionError=ActionError;
    return true;
}

bool UACEPluginSubsystem::RunPluginCommand(const FString& Arguments)
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    FString Id,Text;Arguments.TrimStartAndEnd().Split(TEXT(" "),&Id,&Text);if(Id.IsEmpty())Id=Arguments.TrimStartAndEnd();
    auto P=Find(Id);
    if(!P||!P->HasPresentation()){Notice=TEXT("/acueplugin <companion-id> start | stop | <command text>");return false;}
    if(Text==TEXT("start"))return Start(P->Id);
    if(Text==TEXT("stop")){Stop(P->Id,TEXT("Stopped"));return true;}
    if(!P->Enabled||!P->Running||Text.Len()>512){Notice=TEXT("Start the enabled companion before sending commands (512 characters maximum).");return false;}
    auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("type"),TEXT("command"));E->SetStringField(TEXT("text"),Text);Event(P->PluginEvents,E,++PluginEventSerial);P->NextDecision=0;Notice=TEXT("Command sent to ")+P->Name;return true;
}

void UACEPluginSubsystem::TickCompanions()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginObservers);
    FCompanionReadBatch LocalBatch;
    TGuardValue<FCompanionReadBatch*> ReadGuard(CompanionReadBatch,CompanionReadBatch?CompanionReadBatch:&LocalBatch);
    auto& Batch=*CompanionReadBatch;
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    UpdateCompanionChatContext();
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const bool InWorld=C->GetSessionState()==EACESessionState::InWorld;
    const bool Changed=CompanionSession.Pin()!=C->GetSession()||CompanionPlayer!=C->GetPlayerGuid()||CompanionServer!=C->GetServerName();
    for(const auto& P:Plugins)if(P->Companion)
    {
        if(Changed||!InWorld)
        {
            if(P->Running)StopCompanion(*P,TEXT("Waiting for character"));
            if(Changed)P->CompanionStopped=false;
        }
        if(InWorld&&P->Enabled&&!P->Running&&!P->CompanionStopped)StartCompanion(*P);
    }
    // One observer callback per frame, round-robin. Starting several companions
    // together must not concentrate their JSON/Lua/GC work into a half-second hitch.
    const int32 Count=Plugins.Num(),Start=Count?NextCompanionIndex%Count:0;
    for(int32 Offset=0;Offset<Count;++Offset)
    {
        const int32 Index=(Start+Offset)%Count;const auto P=Plugins[Index];
        if(!P->Companion||!P->Running)continue;
        const double Now=FPlatformTime::Seconds();if(Now<P->NextDecision)continue;
        NextCompanionIndex=(Index+1)%Count;P->NextDecision=Now+P->UpdateInterval;
        TSharedPtr<FJsonObject> Output;FString Error;
        bool Ok=P->VM->Step(P->Api>=2?PublicSnapshot(*P):CompanionSnapshot(*P),P->Profile,Output,Error);
        // Commands and services can change selection, inventory, policy or
        // session context synchronously, including nested snapshot consumers.
        const bool Mutates=Output && (Output->HasField(TEXT("action")) || Output->HasField(TEXT("requests")) || Output->HasField(TEXT("profile_patch")));
        if(Mutates)Batch.Reset();
        if(Ok&&Output)Ok=P->Api>=2?ApplyPublicOutput(*P,Output,Error):ApplyCompanionOutput(*P,Output,Error);
        if(Mutates)Batch.Reset();
        if(!Ok)StopCompanion(*P,TEXT("Script error: ")+Error);
        if(Now>=P->NextStateSave){FlushPluginState(*P);P->NextStateSave=Now+5;}
        break;
    }
    CompanionSession=C->GetSession();CompanionPlayer=InWorld?C->GetPlayerGuid():0;CompanionServer=C->GetServerName();
    DrawPluginMarkers();
    UpdatePluginWorldPlates();
    UpdatePluginTerrain();
}

TArray<FACEPluginMarker> UACEPluginSubsystem::GetPluginMarkers() const
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    TArray<FACEPluginMarker> Result;auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(C->GetSessionState()!=EACESessionState::InWorld)return Result;
    for(const auto& P:Plugins)if(P->HasPresentation()&&P->Running&&P->Enabled&&P->Player==C->GetPlayerGuid())for(const auto& M:P->Markers)
    {FACEWorldObject O;if(C->GetWorldObject(M.Id,O)&&O.IsSelectableWorldObject()&&!O.bDying&&SameArea(C->GetPlayerPosition(),O.Position)&&FVector::DistSquared(C->GetPlayerPosition().ToUnrealLocation(),O.Position.ToUnrealLocation())<=FMath::Square(24000.)){Result.Add(M);if(Result.Num()>=128)return Result;}}
    return Result;
}

void UACEPluginSubsystem::DrawPluginMarkers()
{
    using namespace ACEPluginCompanionPrivate; using ACEPluginCompanionPrivate::S; using ACEPluginCompanionPrivate::N;
    const double Now=FPlatformTime::Seconds();if(Now<NextPluginMarkerDraw)return;NextPluginMarkerDraw=Now+.1;
    const auto Markers=GetPluginMarkers();auto* W=GetWorld();
    if(!W||Markers.IsEmpty()){if(PluginMarkerMesh&&PluginMarkerMesh->GetNumSections())PluginMarkerMesh->ClearAllMeshSections();return;}
    if(!PluginMarkerMesh||PluginMarkerMesh->GetWorld()!=W)
    {
        if(PluginMarkerActor)PluginMarkerActor->Destroy();PluginMarkerActor=W->SpawnActor<AActor>();if(!PluginMarkerActor)return;
        PluginMarkerMesh=NewObject<UProceduralMeshComponent>(PluginMarkerActor);PluginMarkerActor->SetRootComponent(PluginMarkerMesh);
        PluginMarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);PluginMarkerMesh->SetCastShadow(false);PluginMarkerMesh->SetCanEverAffectNavigation(false);
        PluginMarkerMesh->SetMaterial(0,GetGameInstance()->GetSubsystem<UACEDatSubsystem>()->GetVertexColorMaterial());PluginMarkerMesh->RegisterComponentWithWorld(W);
    }
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();const FVector Origin=C->GetPlayerPosition().ToUnrealLocation();
    const auto* PC=GetGameInstance()->GetFirstLocalPlayerController();const FVector Right=PC?PC->GetControlRotation().RotateVector(FVector::RightVector):FVector::RightVector;
    const auto* Presenter=W->GetAuthGameMode()?W->GetAuthGameMode()->FindComponentByClass<UACEWorldPresenterComponent>():nullptr;
    TArray<FVector> Vertices,Normals;TArray<FLinearColor> Colors;TArray<int32> Indices;
    for(const auto& M:Markers)if(M.World)
    {
        FACEWorldObject O;if(!C->GetWorldObject(M.Id,O))continue;
        FVector Center=O.Position.ToUnrealLocation()-Origin+FVector(0,0,220);
        if(auto* Actor=Presenter?Presenter->FindEntityActor(M.Id):nullptr)Center=Actor->GetActorLocation()+FVector(0,0,FMath::Max(30.f,Actor->GetMeleeBodyHeight())+35)-Origin;
        auto Tri=[&](FVector2D A,FVector2D B,FVector2D D){int Base=Vertices.Num();for(auto V:{A,B,D}){Vertices.Add(Center+Right*V.X+FVector::UpVector*V.Y);Normals.Add(FVector::ForwardVector);Colors.Add(M.Color);}Indices.Append({Base,Base+1,Base+2,Base+2,Base+1,Base});};
        auto Rect=[&](float L,float B,float R,float T){Tri({L,B},{R,B},{R,T});Tri({L,B},{R,T},{L,T});};
        if(M.Shape==TEXT("!")){Rect(-4,0,4,24);Rect(-4,-12,4,-5);}
        else if(M.Shape==TEXT("arrow")){Rect(-4,0,4,22);Tri({-14,0},{14,0},{0,-16});}
        else{Tri({0,16},{14,0},{0,-16});Tri({0,16},{0,-16},{-14,0});}
    }
    PluginMarkerMesh->SetWorldLocation(Origin);PluginMarkerMesh->CreateMeshSection_LinearColor(0,Vertices,Indices,Normals,{},Colors,{},false);
}
