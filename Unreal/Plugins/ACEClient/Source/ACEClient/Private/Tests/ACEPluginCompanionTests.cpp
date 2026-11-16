#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Interfaces/IPluginManager.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "ProceduralMeshComponent.h"
#include "Widgets/Input/SButton.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "ACERuntimeOptions.h"
#include "ACEDatSubsystem.h"
#include "ACETestDatDirectory.h"
#include "Mods/ACEPluginStyle.h"

namespace
{
    TSharedPtr<FJsonObject> J(const TCHAR* Text)
    {TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginCompanionTest,"ACE.Plugins.Companions",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginCompanionTest::RunTest(const FString&)
{
    auto Empty=MakeShared<FJsonObject>();TSharedPtr<FJsonObject> Output;FString Error;
    for(const TCHAR* Source:{TEXT("return function() local t={};t.x=t;return {state=t} end"),
        TEXT("return function() return {state={x=0/0}} end"),TEXT("return function() return {state={x=function() end}} end"),
        TEXT("return function() return {ui={[1]={id='a'},[3]={id='b'}}} end"),TEXT("return function() local t={};for i=1,1025 do t[i]=i end;return {ui=t} end")})
    {FACEPluginVM VM;TestTrue(TEXT("Invalid-output fixture loads"),VM.Load(Source,Error));TestFalse(TEXT("Invalid structured output is bounded and rejected"),VM.Step(Empty,Empty,Output,Error));}
    FACEPluginVM VM;TestTrue(TEXT("Structured fixture loads"),VM.Load(TEXT("return function(s) return {state={quest={count=12,goal=50}},ui={{id='target',text='Red Rat',action='select',target=12}},markers={},messages={}} end"),Error));
    TestTrue(TEXT("Structured output works"),VM.Step(Empty,Empty,Output,Error));
    TestEqual(TEXT("Nested state preserved"),Output->GetObjectField(TEXT("state"))->GetObjectField(TEXT("quest"))->GetNumberField(TEXT("count")),12.);
    TestTrue(TEXT("Empty Lua table normalized as marker array"),Output->GetArrayField(TEXT("markers")).IsEmpty());

    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* W=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);auto* GI=NewObject<UGameInstance>(GEngine);W->SetGameInstance(GI);
    Context.OwningGameInstance=GI;Context.SetCurrentWorld(W);GI->OnWorldChanged(nullptr,W);GI->Init();
    auto* H=GI->GetSubsystem<UACEPluginSubsystem>();auto* C=GI->GetSubsystem<UACEClientSubsystem>();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/Companions")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    H->StorageRoot=Root;H->Settings=MakeShared<FJsonObject>();
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(W);W->DestroyWorld(false);IFileManager::Get().DeleteDirectory(*Root,false,true);};
    const FString Installed=Root/TEXT("Installed/test_tracker");IFileManager::Get().MakeDirectory(*Installed,true);
    FFileHelper::SaveStringToFile(TEXT(R"({"api":1,"mode":"companion","id":"test_tracker","name":"Test Tracker","permissions":["chat","state","ui","markers","waypoint","messaging","commands"]})"),*(Installed/TEXT("plugin.json")));
    FFileHelper::SaveStringToFile(TEXT("{}"),*(Installed/TEXT("default.json")));
    FFileHelper::SaveStringToFile(TEXT("return function(s) return {status='active'} end"),*(Installed/TEXT("main.lua")));
    const TCHAR* ManifestOptions[]={TEXT(R"({"update_interval":2,"snapshot_fields":["char_ints","health"]})"),
        TEXT(R"({"update_interval":0.1})"),TEXT(R"({"update_interval":61})"),TEXT(R"({"update_interval":"2"})"),
        TEXT(R"({"snapshot_fields":[7]})"),TEXT(R"({"snapshot_fields":"health"})"),
        TEXT(R"({"chat_history":false,"chat_subscriptions":[{"id":"kills","prefix":"You have killed"}]})"),
        TEXT(R"({"chat_history":"false"})"),TEXT(R"({"chat_subscriptions":[{"id":"bad","pattern":"["}]})")};
    for(int32 I=0;I<UE_ARRAY_COUNT(ManifestOptions);++I)
    {
        auto M=J(ManifestOptions[I]);const FString Id=FString::Printf(TEXT("snapshot_fixture_%d"),I);
        M->SetStringField(TEXT("id"),Id);M->SetStringField(TEXT("name"),Id);M->SetNumberField(TEXT("api"),2);M->SetStringField(TEXT("mode"),TEXT("companion"));
        M->SetArrayField(TEXT("permissions"),{MakeShared<FJsonValueString>(TEXT("data")),MakeShared<FJsonValueString>(TEXT("chat")),MakeShared<FJsonValueString>(TEXT("identify")),MakeShared<FJsonValueString>(TEXT("sound"))});
        const FString Dir=Root/TEXT("Installed")/Id;IFileManager::Get().MakeDirectory(*Dir,true);
        FString Text;FJsonSerializer::Serialize(M.ToSharedRef(),TJsonWriterFactory<>::Create(&Text));
        FFileHelper::SaveStringToFile(Text,*(Dir/TEXT("plugin.json")));
        FFileHelper::SaveStringToFile(TEXT("{}"),*(Dir/TEXT("default.json")));
        FFileHelper::SaveStringToFile(TEXT("return function() end"),*(Dir/TEXT("main.lua")));
    }
    H->Discover();auto P=H->Find(TEXT("test_tracker"));if(!TestTrue(TEXT("Companion manifest discovered"),P&&P->Companion))return false;
    for(int32 I=0;I<UE_ARRAY_COUNT(ManifestOptions);++I)
        TestEqual(FString::Printf(TEXT("Manifest observation fixture %d accepts only valid options"),I),H->Find(FString::Printf(TEXT("snapshot_fixture_%d"),I)).IsValid(),I==0||I==6);
    if(auto Selected=H->Find(TEXT("snapshot_fixture_6")))
        TestTrue(TEXT("Manifest subscription and fresh-start option are applied"),Selected->SelectiveChat&&!Selected->ChatHistory&&Selected->ChatSubscriptions.Num()==1);
    if(auto Selected=H->Find(TEXT("snapshot_fixture_0")))
        TestTrue(TEXT("Manifest choices reach the scheduler and snapshot filter"),Selected->UpdateInterval==2&&Selected->SelectiveSnapshot&&Selected->WantsSnapshotField(TEXT("health"))&&!Selected->WantsSnapshotField(TEXT("inventory")));
    C->Session=MakeShared<FACESession>();auto& Session=*C->Session;Session.State=EACESessionState::InWorld;Session.PlayerGuid=0x50000001;Session.ServerName=TEXT("Companion Test");
    Session.PlayerPosition.CellId=0x7D64000D;Session.PlayerPosition.Location=FVector(34,104,12);
    Session.PlayerVitals.bValid=true;Session.PlayerVitals.Health=100;Session.PlayerVitals.MaxHealth=100;
    FACEWorldObject Player;Player.Guid=Session.PlayerGuid;Player.Name=TEXT("Test character");Player.Position=Session.PlayerPosition;Player.bIsPlayer=true;Session.WorldObjects.Add(Player.Guid,Player);
    FACEWorldObject Rat;Rat.Guid=12;Rat.Name=TEXT("Red Rat");Rat.ItemType=ACEItemType::Creature;Rat.bHasPosition=true;Rat.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;Rat.Position=Session.PlayerPosition;Rat.Position.Location.X+=5;Session.WorldObjects.Add(Rat.Guid,Rat);
    H->SetEnabled(P->Id,true);
    C->OnChatMessage.Broadcast(TEXT("Early quest response"),TEXT(""),ACEChatMessageType::System);
    TestEqual(TEXT("Client feed records before first maintenance tick"),H->PluginChatEvents.Num(),1);
    H->RefreshAutomationData();H->TickCompanions();
    TestTrue(TEXT("Enabled companion starts automatically in world"),P->Running);
    {
        auto Second=MakeShared<FACEClientPlugin>();Second->Id=TEXT("schedule_fixture");Second->Name=TEXT("Schedule fixture");
        Second->Api=2;Second->Companion=true;Second->Running=Second->Enabled=true;
        Second->Player=P->Player;Second->Server=P->Server;Second->Profile=Empty;
        Second->VM=MakeShared<FACEPluginVM>();Second->UpdateInterval=4;
        Second->VM->Load(TEXT("return function() return {status='scheduled'} end"),Error);
        H->Plugins.Add(Second);P->NextDecision=0;Second->NextDecision=0;
        H->NextCompanionIndex=0;
        H->TickCompanions();
        TestTrue(TEXT("Only one due observer executes in a frame"),(P->NextDecision>0)!=(Second->NextDecision>0));
        H->TickCompanions();
        TestTrue(TEXT("The next observer runs on the next frame"),P->NextDecision>0&&Second->NextDecision>0);
        TestEqual(TEXT("Scheduled callback actually executes"),Second->Status,FString(TEXT("scheduled")));
        TestTrue(TEXT("Custom interval delays the next poll"),Second->NextDecision-FPlatformTime::Seconds()>3.8);
        H->Plugins.Remove(Second);
    }
    auto UCM=H->Find(TEXT("ucm"));H->SetEnabled(TEXT("ucm"),true);TestTrue(TEXT("UCM starts alongside companion"),H->Start(TEXT("ucm")));
    TestTrue(TEXT("Starting UCM does not stop companion"),P->Running);
    H->MovementOwner=TEXT("ucm");H->FastCastOwner=TEXT("ucm");
    H->Stop(P->Id);TestTrue(TEXT("Stopping companion leaves UCM running and movement owned"),UCM->Running&&H->MovementOwner==TEXT("ucm")&&H->FastCastOwner==TEXT("ucm"));
    H->Start(P->Id);H->Stop(TEXT("ucm"));
    TestTrue(TEXT("UCM stop leaves tracker running"),P->Running);
    C->OnChatMessage.Broadcast(TEXT("You have killed 12 of 50 Red Rats."),TEXT(""),ACEChatMessageType::System);
    auto Snapshot=H->CompanionSnapshot(*P);TestEqual(TEXT("Quest chat works with UCM stopped"),Snapshot->GetArrayField(TEXT("chat_events")).Num(),1);
    TestEqual(TEXT("Raw chat preserved"),Snapshot->GetArrayField(TEXT("chat_events"))[0]->AsObject()->GetStringField(TEXT("raw_text")),FString(TEXT("You have killed 12 of 50 Red Rats.")));
    TestTrue(TEXT("Chat drained independently per companion"),H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).IsEmpty());
    H->Stop(P->Id);H->Start(P->Id);TestTrue(TEXT("Stop/start does not redeliver consumed chat"),H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).IsEmpty());
    H->Stop(P->Id);P->ChatHistory=false;
    for(int32 I=0;I<300;++I)C->OnChatMessage.Broadcast(TEXT("Before restart"),TEXT(""),6);
    H->Start(P->Id);
    TestTrue(TEXT("History opt-out starts at newest chat on restart"),H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).IsEmpty());
    C->OnChatMessage.Broadcast(TEXT("After restart"),TEXT(""),6);
    TestEqual(TEXT("History opt-out still delivers new messages"),H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).Num(),1);
    P->ChatHistory=true;
    for(int I=0;I<140;++I)C->OnChatMessage.Broadcast(TEXT("quest line"),TEXT(""),ACEChatMessageType::System);
    Snapshot=H->CompanionSnapshot(*P);TestEqual(TEXT("Lua batch stays bounded"),Snapshot->GetArrayField(TEXT("chat_events")).Num(),128);
    TestFalse(TEXT("140-line burst no longer overflows"),Snapshot->GetBoolField(TEXT("chat_gap")));
    TestEqual(TEXT("Unconsumed burst is pending"),Snapshot->GetNumberField(TEXT("chat_pending")),12.);
    Snapshot=H->CompanionSnapshot(*P);TestEqual(TEXT("Next callback receives the rest"),Snapshot->GetArrayField(TEXT("chat_events")).Num(),12);
    TestFalse(TEXT("Last page clears backlog flag"),Snapshot->GetBoolField(TEXT("chat_more")));
    {
        // ACE emits a separate SystemChat for every quest, all before the next
        // Lua callback. Exercise the actual VM and two independent readers.
        FACEClientPlugin Slow;Slow.Permissions={TEXT("chat")};Slow.ChatCursor=P->ChatCursor;
        const FString Source=TEXT("local count=0; return function(s) assert(not s.chat_gap); for _,e in ipairs(s.chat_events) do count=count+1; assert(tonumber(string.sub(e.raw_text,6,10))==count); end; return {status=tostring(count)} end");
        FACEPluginVM FastVM,SlowVM;TestTrue(TEXT("Burst readers load"),FastVM.Load(Source,Error)&&SlowVM.Load(Source,Error));
        auto ReceiveQuest=[&](int32 I){C->OnChatMessage.Broadcast(FString::Printf(TEXT("quest%05d - 1 solves (1234567890)\"Quest message\" -1 0"),I),TEXT(""),ACEChatMessageType::System);};
        for(int32 I=1;I<=20000;++I)ReceiveQuest(I);
        auto Drain=[&](FACEClientPlugin& Reader,FACEPluginVM& ReaderVM,int32 Expected)
        {
            TSharedPtr<FJsonObject> Result;
            for(int32 Page=0;Page<200;++Page)
            {
                const auto Batch=H->CompanionSnapshot(Reader);const auto& Events=Batch->GetArrayField(TEXT("chat_events"));
                if(!TestTrue(TEXT("Burst page stays within the Lua event budget"),Events.Num()<=128))return false;
                if(!TestTrue(TEXT("Quest parser receives every line once, in order, within VM limits"),ReaderVM.Step(Batch,Empty,Result,Error)))
                {AddError(Error);return false;}
                if(!Events.IsEmpty())TestEqual(TEXT("Cursor ends at delivered page, not latest receipt"),Batch->GetNumberField(TEXT("chat_serial")),Events.Last()->AsObject()->GetNumberField(TEXT("serial")));
                TestEqual(TEXT("Latest received serial remains visible while paging"),Batch->GetNumberField(TEXT("chat_latest_serial")),double(H->PluginChatSerial));
                if(!Batch->GetBoolField(TEXT("chat_more")))
                {
                    TestEqual(TEXT("Complete quest burst delivered"),Result->GetStringField(TEXT("status")),FString::FromInt(Expected));
                    TestTrue(TEXT("Draining never redelivers the last page"),H->CompanionSnapshot(Reader)->GetArrayField(TEXT("chat_events")).IsEmpty());return true;
                }
            }
            AddError(TEXT("Quest burst failed to drain"));return false;
        };
        if(!Drain(*P,FastVM,20000))return false;
        ReceiveQuest(20001);
        if(!Drain(Slow,SlowVM,20001)||!Drain(*P,FastVM,20001))return false;
        for(int32 I=0;I<ACEPluginChatLimits::RetainedEvents+5;++I)C->OnChatMessage.Broadcast(TEXT("overflow"),TEXT(""),ACEChatMessageType::System);
        TestEqual(TEXT("Native retention has a fixed event ceiling"),H->PluginChatEvents.Num(),ACEPluginChatLimits::RetainedEvents);
        Snapshot=H->CompanionSnapshot(*P);
        TestTrue(TEXT("Actual backlog loss reports a gap"),Snapshot->GetBoolField(TEXT("chat_gap")));
        TestEqual(TEXT("Exact lost event count reported"),Snapshot->GetNumberField(TEXT("chat_dropped")),5.);
        while(Snapshot->GetBoolField(TEXT("chat_more")))
        {Snapshot=H->CompanionSnapshot(*P);TestFalse(TEXT("A gap is reported only on the affected page"),Snapshot->GetBoolField(TEXT("chat_gap")));}
        const FString LongLine=FString::ChrN(4096,TCHAR(0x754C));
        for(int32 I=0;I<600;++I)C->OnChatMessage.Broadcast(LongLine,TEXT(""),ACEChatMessageType::System);
        TestTrue(TEXT("Long Unicode bursts obey native byte budget"),H->PluginChatTextBytes<=ACEPluginChatLimits::RetainedTextBytes&&H->PluginChatEvents.Num()<600);
        Snapshot=H->CompanionSnapshot(*P);TestTrue(TEXT("Byte-budget overflow is also reported"),Snapshot->GetBoolField(TEXT("chat_gap")));
        TestEqual(TEXT("Long Unicode messages are paged by UTF-8 bytes"),Snapshot->GetArrayField(TEXT("chat_events")).Num(),2);
        FACEPluginVM LongVM;LongVM.Load(TEXT("return function(s) for _,e in ipairs(s.chat_events) do assert(#e.raw_text==12288) end; return {} end"),Error);
        TSharedPtr<FJsonObject> Result;
        TestTrue(TEXT("Unicode page fits real Lua heap"),LongVM.Step(Snapshot,Empty,Result,Error));
        // Leave this backlog pending to verify character/server isolation below.
    }
    TestTrue(TEXT("Companion output accepted"),H->ApplyCompanionOutput(*P,Output,Error));H->FlushPluginState(*P);
    TestTrue(TEXT("State saved"),IFileManager::Get().FileExists(*P->StatePath)&&!P->StateDirty);
    H->Stop(P->Id);H->Start(P->Id);TestEqual(TEXT("Stop/start restores quest count"),P->SavedState->GetObjectField(TEXT("quest"))->GetNumberField(TEXT("count")),12.);
    const FString FirstPath=P->StatePath;H->Stop(P->Id);Session.ServerName=TEXT("Another server");H->Start(P->Id);
    TestTrue(TEXT("Server-scoped state isolated"),P->StatePath!=FirstPath&&!P->SavedState->HasField(TEXT("quest")));
    TestTrue(TEXT("Changing servers clears the unread chat backlog and byte accounting"),H->PluginChatEvents.IsEmpty()&&H->PluginChatTextBytes==0&&H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).IsEmpty());
    H->Stop(P->Id);Session.ServerName=TEXT("Companion Test");Session.PlayerGuid++;H->Start(P->Id);
    TestTrue(TEXT("Character-scoped state isolated"),P->StatePath!=FirstPath&&!P->SavedState->HasField(TEXT("quest")));
    H->Stop(P->Id);Session.PlayerGuid--;H->Start(P->Id);
    TestEqual(TEXT("Original character restores persisted state"),P->SavedState->GetObjectField(TEXT("quest"))->GetNumberField(TEXT("count")),12.);
    TestTrue(TEXT("UI restored"),H->ApplyCompanionOutput(*P,Output,Error));
    TestTrue(TEXT("Click selects current nearby object"),H->ClickPluginRow(P->Id,TEXT("target")));TestEqual(TEXT("Game selection updated"),C->GetSelectedObject().Guid,12);
    Session.WorldObjects.Remove(12);TestFalse(TEXT("Stale click cannot select removed object"),H->ClickPluginRow(P->Id,TEXT("target")));Session.WorldObjects.Add(12,Rat);
    TestFalse(TEXT("Unknown row rejected"),H->ClickPluginRow(P->Id,TEXT("absent")));
    TestTrue(TEXT("Duplicate rows rejected"),!H->ApplyCompanionOutput(*P,J(TEXT(R"({"ui":[{"id":"x","text":"a"},{"id":"x","text":"b"}]})")),Error));
    auto Markers=J(TEXT(R"({"markers":[{"id":12,"shape":"arrow","color":"green"}],"action":"waypoint","target":12})"));
    H->SetWaypointOption(TEXT("arrow"),false);
    TestTrue(TEXT("Markers and waypoint accepted"),H->ApplyCompanionOutput(*P,Markers,Error));TestEqual(TEXT("Visible marker exists"),H->GetPluginMarkers().Num(),1);
    H->NextPluginMarkerDraw=0;H->DrawPluginMarkers();
    TestTrue(TEXT("World marker creates packaged-game geometry"),H->PluginMarkerMesh&&H->PluginMarkerMesh->GetProcMeshSection(0)&&H->PluginMarkerMesh->GetProcMeshSection(0)->ProcVertexBuffer.Num()>0);
    if(H->PluginMarkerMesh)TestEqual(TEXT("Markers cannot block movement"),H->PluginMarkerMesh->GetCollisionEnabled(),ECollisionEnabled::NoCollision);
    FVector2D Destination;uint32 LB=0;TestTrue(TEXT("Waypoint destination set"),H->GetWaypoint(Destination,LB));
    TestFalse(TEXT("Background plugin waypoint leaves the player's arrow hidden"),H->WaypointOption(TEXT("arrow")));
    Session.WorldObjects[12].bDying=true;TestTrue(TEXT("Dead object markers disappear"),H->GetPluginMarkers().IsEmpty());Session.WorldObjects[12].bDying=false;
    auto Peer=MakeShared<FACEClientPlugin>();Peer->Id=TEXT("peer");Peer->Companion=true;Peer->Running=true;Peer->Enabled=true;Peer->Permissions={TEXT("messaging")};H->Plugins.Add(Peer);
    TestTrue(TEXT("Message accepted"),H->ApplyCompanionOutput(*P,J(TEXT(R"({"messages":[{"to":"peer","topic":"kill","text":"12"}]})")),Error));
    TestEqual(TEXT("Native sender cannot be spoofed"),Peer->PluginEvents[0]->AsObject()->GetStringField(TEXT("sender")),P->Id);
    for(int I=0;I<80;++I)H->ApplyCompanionOutput(*P,J(TEXT(R"({"messages":[{"to":"peer","topic":"kill","text":"12"}]})")),Error);
    TestEqual(TEXT("Message inbox bounded"),Peer->PluginEvents.Num(),64);
    TestTrue(TEXT("Local command routed"),H->RunPluginCommand(TEXT("test_tracker reset")));Snapshot=H->CompanionSnapshot(*P);
    TestTrue(TEXT("Local command arrives as event"),Snapshot->GetArrayField(TEXT("events")).ContainsByPredicate([](const auto& V){return V->AsObject()->GetStringField(TEXT("type"))==TEXT("command");}));
    TArray<FString> Issues;auto Compiled=ACEVTProfile::CompileCommand(TEXT("/acueplugin test_tracker from_meta"),Issues);
    TestTrue(TEXT("Native meta compiler accepts companion command"),Issues.IsEmpty()&&Compiled->GetStringField(TEXT("op"))==TEXT("plugin"));
    auto Legacy=ACEVTProfile::CompileCommand(TEXT("/plugin test_tracker from_meta"),Issues);
    TestTrue(TEXT("Saved companion command uses the same adapter"),Issues.IsEmpty()&&Legacy->GetStringField(TEXT("op"))==TEXT("plugin")&&Legacy->GetStringField(TEXT("source_command"))==Compiled->GetStringField(TEXT("source_command")));
    P->Permissions.Add(TEXT("commands"));
    auto Items=H->Find(TEXT("itemtools"));const bool ItemsEnabled=Items&&Items->Enabled;if(Items)Items->Enabled=true;
    for(const TCHAR* Text:{TEXT("/acueucm help"),TEXT("/ucm help"),TEXT("/acueitems stop"),TEXT("/items stop"),TEXT("/it stop")})
    {
        P->NextCommand=0;auto Action=MakeShared<FJsonObject>();Action->SetStringField(TEXT("action"),TEXT("command"));Action->SetStringField(TEXT("text"),Text);
        TestTrue(*(FString(TEXT("Companion command bridge accepts "))+Text),H->CompanionAction(*P,Action,Error));
    }
    if(Items)Items->Enabled=ItemsEnabled;
    FString UCMSource;FFileHelper::LoadFileToString(UCMSource,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    FACEPluginVM MetaVM;TestTrue(TEXT("UCM meta fixture loads"),MetaVM.Load(UCMSource,Error));
    auto MetaProfile=J(TEXT(R"({"buffing":false,"combat":"off","navigation":false,"looting":false,"recovery":false,"meta_enabled":true})"));
    MetaProfile->SetArrayField(TEXT("ucm_commands"),{MakeShared<FJsonValueObject>(Compiled)});
    TestTrue(TEXT("UCM queues native plugin command"),MetaVM.Step(H->Snapshot(),MetaProfile,Output,Error));
    if(TestTrue(TEXT("UCM emitted plugin intent"),Output&&Output->GetStringField(TEXT("action"))==TEXT("plugin_command")))H->Execute(*UCM,Output);
    TestTrue(TEXT("Meta command reaches companion inbox"),P->PluginEvents.ContainsByPredicate([](const auto& V){FString Text;V->AsObject()->TryGetStringField(TEXT("text"),Text);return Text==TEXT("from_meta");}));
    // Restore state-bearing output for the permission checks below.
    VM.Step(Empty,Empty,Output,Error);
    P->Permissions.Remove(TEXT("chat"));P->Permissions.Remove(TEXT("state"));P->Permissions.Remove(TEXT("waypoint"));
    TestTrue(TEXT("No chat permission means no chat"),H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).IsEmpty());
    TestFalse(TEXT("No state permission rejects state"),H->ApplyCompanionOutput(*P,Output,Error));
    TestFalse(TEXT("No waypoint permission rejects action"),H->CompanionAction(*P,J(TEXT(R"({"action":"waypoint","target":12})")),Error));
    TestFalse(TEXT("Companion cannot cast or acquire movement ownership"),H->CompanionAction(*P,J(TEXT(R"({"action":"cast","spell":1})")),Error));
    P->Permissions.Append({TEXT("state"),TEXT("chat"),TEXT("waypoint")});
    Peer->Running=false;
    const FString Base=IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir();
    for(const TCHAR* Example:{TEXT("kill_tracker"),TEXT("vsense")})
    {
        FString Source,ProfileText;FFileHelper::LoadFileToString(Source,*(Base/TEXT("Docs/Plugins/Examples")/Example/TEXT("main.lua")));
        FFileHelper::LoadFileToString(ProfileText,*(Base/TEXT("Docs/Plugins/Examples")/Example/TEXT("default.json")));
        FACEPluginVM ExampleVM;TestTrue(TEXT("Shipped example loads"),ExampleVM.Load(Source,Error));
        C->OnChatMessage.Broadcast(TEXT("You have killed 12 of 50 Red Rats."),TEXT(""),ACEChatMessageType::System);
        Snapshot=H->CompanionSnapshot(*P);const bool Stepped=ExampleVM.Step(Snapshot,J(*ProfileText),Output,Error);
        TestTrue(*FString::Printf(TEXT("Shipped %s example runs on native snapshot: %s"),Example,*Error),Stepped);
        TestTrue(TEXT("Shipped example output accepted"),Output&&H->ApplyCompanionOutput(*P,Output,Error));
    }
    if(FApp::CanEverRender())
    {
        const float OriginalScale=ACERuntimeOptions::Get(TEXT("PluginTextScale"));
        ON_SCOPE_EXIT{ACERuntimeOptions::Set(TEXT("PluginTextScale"),OriginalScale);};
        TestTrue(TEXT("DAT available for plugin text scaling"),GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(ACETestDatDirectory()));
        ACERuntimeOptions::Set(TEXT("PluginTextScale"),1.f);
        H->ApplyCompanionOutput(*P,J(TEXT(R"({"ui":[{"id":"rat","text":"Red Rat - 5 m","action":"select","target":12},{"id":"progress","text":"Kill task: 12 / 50"},{"id":"nav","text":"Point arrow to nearest Red Rat","action":"waypoint","target":12}]})")),Error);
        FWidgetRenderer Renderer(false,true);auto Panel=H->MakePanel(P->Id);const FVector2D Size(540,720);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        for(int I=0;I<3;++I){Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}
        TSharedPtr<SACEPluginText> Label,ButtonLabel;
        TFunction<void(TSharedRef<SWidget>)> FindLabel=[&](TSharedRef<SWidget> Widget)
        {
            if(Widget->GetType()==TEXT("SACEPluginText"))
            {
                auto Text=StaticCastSharedRef<SACEPluginText>(Widget);
                if(Text->GetText().ToString()==TEXT("Kill task: 12 / 50"))Label=Text;
                if(Text->GetText().ToString()==TEXT("Red Rat - 5 m"))ButtonLabel=Text;
            }
            auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)FindLabel(Children->GetChildAt(I));
        };
        FindLabel(Panel);
        if(TestTrue(TEXT("Lua label found in live companion tree"),Label.IsValid()))
        {
            const float OriginalFont=Label->GetFont().Size;const float OriginalHeight=Label->GetDesiredSize().Y;
            ACERuntimeOptions::Set(TEXT("PluginTextScale"),2.f);
            for(int I=0;I<4;++I){Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}
            TestEqual(TEXT("Existing Lua label adopts 200% font without rebuild"),Label->GetFont().Size,OriginalFont*2.f);
            TestTrue(TEXT("Layout reserves space for enlarged Lua text"),Label->GetDesiredSize().Y>=OriginalHeight*1.9f);
            if(TestTrue(TEXT("Lua action label found"),ButtonLabel.IsValid()))
                TestTrue(TEXT("Short enlarged button label uses the available row width"),ButtonLabel->GetDesiredSize().Y<60.f);
            ACERuntimeOptions::Set(TEXT("PluginTextScale"),.75f);
            for(int I=0;I<3;++I){Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}
            TestEqual(TEXT("Existing Lua label shrinks without compounding"),Label->GetFont().Size,OriginalFont*.75f);
            ACERuntimeOptions::Set(TEXT("PluginTextScale"),1.f);
        }
        auto Input=SNew(SACEPluginEditableTextBox).Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(TEXT("Unsaved draft")));
        auto Number=SNew(SACEPluginSpinBox<double>).Font(FACEPluginStyle::NativeFont("Regular",15)).Value(42.);
        auto Editor=SNew(SACEPluginMultiLineEditableTextBox).Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(TEXT("First command\nSecond command")));
        auto CheckControls=[&](){for(const TSharedRef<SWidget>& Control:{StaticCastSharedRef<SWidget>(Input),StaticCastSharedRef<SWidget>(Number),StaticCastSharedRef<SWidget>(Editor)})
            for(int I=0;I<3;++I){Renderer.DrawWidget(Target,Control,Size,0);FlushRenderingCommands();}};
        CheckControls();const float InputHeight=Input->GetDesiredSize().Y,NumberHeight=Number->GetDesiredSize().Y,EditorHeight=Editor->GetDesiredSize().Y;
        ACERuntimeOptions::Set(TEXT("PluginTextScale"),2.f);CheckControls();
        TestTrue(TEXT("Editable fields reserve enlarged font height"),Input->GetDesiredSize().Y>InputHeight+8);
        TestTrue(TEXT("Numeric fields reserve enlarged font height"),Number->GetDesiredSize().Y>NumberHeight+8);
        TestTrue(TEXT("Multiline editors reserve enlarged line height"),Editor->GetDesiredSize().Y>EditorHeight+8);
        TestEqual(TEXT("Scaling preserves unsaved input"),Input->GetText().ToString(),FString(TEXT("Unsaved draft")));
        TestEqual(TEXT("Scaling preserves numeric input"),Number->GetValue(),42.);
        TestEqual(TEXT("Scaling preserves unsaved multiline input"),Editor->GetText().ToString(),FString(TEXT("First command\nSecond command")));
        for(int I=0;I<3;++I){Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}
        TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
        TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(540,720,Pixels,Png);FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Companion-panel.png")));Target->ReleaseResource();
    }
    H->TickCompanions();C->OnChatMessage.Broadcast(TEXT("Unread before logout"),TEXT(""),ACEChatMessageType::System);
    Session.State=EACESessionState::CharacterSelect;H->TickCompanions();
    TestFalse(TEXT("Logout stops companion"),P->Running);TestTrue(TEXT("Logout clears UI and markers"),P->PanelRows.IsEmpty()&&H->GetPluginMarkers().IsEmpty());
    TestTrue(TEXT("Logout drops pending chat and resets reader cursor"),H->PluginChatEvents.IsEmpty()&&P->ChatCursor==H->PluginChatSerial);
    Session.State=EACESessionState::InWorld;H->TickCompanions();
    Snapshot=H->CompanionSnapshot(*P);TestFalse(TEXT("Same-character relog does not leave a phantom backlog"),Snapshot->GetBoolField(TEXT("chat_more")));
    C->OnChatMessage.Broadcast(TEXT("New login quest"),TEXT(""),ACEChatMessageType::System);Snapshot=H->CompanionSnapshot(*P);
    TestFalse(TEXT("Old unread pages are not reported as overflow on relog"),Snapshot->GetBoolField(TEXT("chat_gap")));
    TestEqual(TEXT("New login receives only its own chat"),Snapshot->GetArrayField(TEXT("chat_events")).Num(),1);
    if(!Snapshot->GetArrayField(TEXT("chat_events")).IsEmpty())TestEqual(TEXT("New login chat preserved"),Snapshot->GetArrayField(TEXT("chat_events"))[0]->AsObject()->GetStringField(TEXT("raw_text")),FString(TEXT("New login quest")));
    const FString TrackerId=P->Id;const uint64 DeliveredCursor=P->ChatCursor;
    H->Discover();P=H->Find(TrackerId);
    TestTrue(TEXT("Refresh preserves the delivered chat cursor"),P&&P->ChatCursor==DeliveredCursor);
    if(P)
    {
        TestTrue(TEXT("Refreshed tracker starts"),H->Start(P->Id));
        TestTrue(TEXT("Refresh does not replay previously delivered login chat"),H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).IsEmpty());
        C->OnChatMessage.Broadcast(TEXT("After refresh"),TEXT(""),ACEChatMessageType::System);
        TestEqual(TEXT("Refreshed tracker receives new chat immediately"),H->CompanionSnapshot(*P)->GetArrayField(TEXT("chat_events")).Num(),1);
    }
    return true;
}
#endif
