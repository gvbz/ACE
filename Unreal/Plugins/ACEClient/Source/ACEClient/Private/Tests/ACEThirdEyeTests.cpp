#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Interfaces/IPluginManager.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEWaypoint.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "Audio.h"
#include "ACESession.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    TSharedPtr<FJsonObject> EyeJSON(const FString& Text)
    {TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
    TSharedPtr<FJsonObject> EyeControl(const TArray<TSharedPtr<FJsonValue>>& Rows,const FString& Id)
    {
        for(const auto& V:Rows)
        {
            auto R=V->AsObject();if(R->GetStringField(TEXT("id"))==Id)return R;
            const TArray<TSharedPtr<FJsonValue>>* Children=nullptr;
            if(R->TryGetArrayField(TEXT("children"),Children))if(auto Found=EyeControl(*Children,Id))return Found;
        }
        return nullptr;
    }
    TArray<TSharedPtr<FJsonObject>> EyeRequests(const TSharedPtr<FJsonObject>& Output,const FString& Service)
    {
        TArray<TSharedPtr<FJsonObject>> Found;
        const TArray<TSharedPtr<FJsonValue>>* Requests=nullptr;
        if(Output&&Output->TryGetArrayField(TEXT("requests"),Requests))
            for(const auto& Request:*Requests)if(Request->AsObject()->GetStringField(TEXT("service"))==Service)Found.Add(Request->AsObject());
        return Found;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEThirdEyeTest,"ACE.Plugins.ThirdEye",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEThirdEyeTest::RunTest(const FString&)
{
    const FString Base=IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir();
    FString Script,Error;TSharedPtr<FJsonObject> Out;auto Empty=MakeShared<FJsonObject>();
    if(!TestTrue(TEXT("Third Eye Lua is bundled"),FFileHelper::LoadFileToString(Script,*(Base/TEXT("ClientMods/thirdeye/main.lua")))))return false;
    FACEPluginVM VM;if(!TestTrue(TEXT("Third Eye compiles in the real sandbox"),VM.Load(Script,Error)))return false;
    auto Snapshot=EyeJSON(TEXT(R"({"time":100,"state":{},"selected":1,"plugins":{"waypoint":{"enabled":true}},"objects":[
        {"id":1,"name":"Black Coral Golem","object_type":"creature","wcid":10,"distance":5,"dead":false},
        {"id":2,"name":"Black Coral Crystal","wcid":11,"distance":12,"dead":false},
        {"id":3,"name":"Red Rat","wcid":12,"distance":15,"dead":false},
        {"id":4,"name":"Black Coral Golem","wcid":10,"distance":18,"dead":true}]})"));
    auto Step=[&](const TCHAR* Events=TEXT("[]"))
    {
        auto E=EyeJSON(FString(TEXT("{\"events\":"))+Events+TEXT("}"));Snapshot->SetArrayField(TEXT("events"),E->GetArrayField(TEXT("events")));
        Snapshot->SetNumberField(TEXT("time"),Snapshot->GetNumberField(TEXT("time"))+1);
        const bool Ok=VM.Step(Snapshot,Empty,Out,Error);TestTrue(TEXT("Third Eye tick fits sandbox limits"),Ok);if(!Ok)AddError(Error);return Ok;
    };
    if(!Step())return false;
    TestTrue(TEXT("Empty watch list is explained"),EyeControl(Out->GetArrayField(TEXT("ui")),TEXT("empty")).IsValid());
    if(!Step(TEXT(R"([{"type":"change","id":"draft","value":"  bLaCk Coral  "},{"type":"click","id":"add"}])")))return false;
    TestEqual(TEXT("Partial name matches ignore case; dying objects excluded"),Out->GetStringField(TEXT("status")),FString(TEXT("2 matches")));
    TestEqual(TEXT("New spawn burst uses one local notification"),Out->GetArrayField(TEXT("requests")).Num(),1);
    TestEqual(TEXT("Default markers include both matches"),Out->GetArrayField(TEXT("markers")).Num(),2);
    TestTrue(TEXT("Third Eye clears its old plates"),Out->GetArrayField(TEXT("world_plates")).IsEmpty());
    for(const TCHAR* Id:{TEXT("add_plate"),TEXT("plate_1"),TEXT("plates_tab"),TEXT("opacity"),TEXT("vr_opacity")})
        TestFalse(TEXT("Third Eye has no plate or opacity controls"),EyeControl(Out->GetArrayField(TEXT("ui")),Id).IsValid());
    FACEPluginVM Migrated;Migrated.Load(Script,Error);auto RestoreInput=MakeShared<FJsonObject>(*Snapshot);
    RestoreInput->SetObjectField(TEXT("state"),EyeJSON(TEXT(R"({"rules":[{"name":"Black Coral","plate":true}],"opacity":0.5,"vr_opacity":0.25})")));
    RestoreInput->SetArrayField(TEXT("events"),{});TSharedPtr<FJsonObject> RestoreOutput;
    TestTrue(TEXT("Older Third Eye preferences still load"),Migrated.Step(RestoreInput,Empty,RestoreOutput,Error));
    TestEqual(TEXT("Migration preserves watch matches"),RestoreOutput->GetStringField(TEXT("status")),FString(TEXT("2 matches")));
    TestTrue(TEXT("Old enabled plate preferences cannot create plates"),RestoreOutput->GetArrayField(TEXT("world_plates")).IsEmpty());
    TestFalse(TEXT("Obsolete plate settings removed from saved rules"),RestoreOutput->GetObjectField(TEXT("state"))->GetArrayField(TEXT("rules"))[0]->AsObject()->HasField(TEXT("plate")));
    TestFalse(TEXT("Obsolete opacity preferences removed"),RestoreOutput->GetObjectField(TEXT("state"))->HasField(TEXT("opacity")));
    auto Saved=Out->GetObjectField(TEXT("state"));
    TestEqual(TEXT("Watch list saved without surrounding whitespace"),Saved->GetArrayField(TEXT("rules"))[0]->AsObject()->GetStringField(TEXT("name")),FString(TEXT("bLaCk Coral")));
    TestFalse(TEXT("Sound alerts are opt-in for existing and new users"),Saved->GetBoolField(TEXT("sound_alerts")));
    TestTrue(TEXT("Default detection does not play audio"),EyeRequests(Out,TEXT("sound")).IsEmpty());
    if(!Step(TEXT(R"([{"type":"result","id":"detected_alert","ok":false,"error":"Service rate limit; retry after 0.5 seconds"}])")))return false;
    TestEqual(TEXT("Rate-limited alerts retry without losing detections"),Out->GetArrayField(TEXT("requests")).Num(),1);
    if(!Step(TEXT(R"([{"type":"result","id":"detected_alert","ok":true}])")))return false;
    TestTrue(TEXT("Stationary matches do not spam chat"),Out->GetArrayField(TEXT("requests")).IsEmpty());
    const auto KnownObjects=Snapshot->GetArrayField(TEXT("objects"));Snapshot->SetArrayField(TEXT("objects"),{});Step();
    TestEqual(TEXT("Vanished objects leave the detected list"),Out->GetStringField(TEXT("status")),FString(TEXT("0 matches")));
    Snapshot->SetArrayField(TEXT("objects"),KnownObjects);Step();
    TestTrue(TEXT("Brief awareness flicker does not repeat alerts"),Out->GetArrayField(TEXT("requests")).IsEmpty());
    Snapshot->SetArrayField(TEXT("objects"),{});Snapshot->SetNumberField(TEXT("time"),Snapshot->GetNumberField(TEXT("time"))+31);Step();
    Snapshot->SetArrayField(TEXT("objects"),KnownObjects);Step();
    TestEqual(TEXT("Returning after a long absence is announced"),Out->GetArrayField(TEXT("requests")).Num(),1);
    Step(TEXT(R"([{"type":"result","id":"detected_alert","ok":true}])"));
    Step(TEXT(R"([{"type":"change","id":"range","value":10}])"));
    TestEqual(TEXT("Detection range is enforced"),Out->GetStringField(TEXT("status")),FString(TEXT("1 matches")));
    Step(TEXT(R"([{"type":"change","id":"watching","value":false}])"));
    TestTrue(TEXT("Pause clears markers and alerts"),Out->GetArrayField(TEXT("markers")).IsEmpty()&&Out->GetArrayField(TEXT("requests")).IsEmpty());
    Step(TEXT(R"([{"type":"change","id":"watching","value":true},{"type":"change","id":"range","value":240},{"type":"click","id":"remove_1"},{"type":"change","id":"exact","value":true},{"type":"click","id":"add_selected"}])"));
    TestEqual(TEXT("Add selected with exact matching excludes similar names"),Out->GetStringField(TEXT("status")),FString(TEXT("1 matches")));
    const auto ExactSaved=Out->GetObjectField(TEXT("state"));
    Step(TEXT(R"([{"type":"click","id":"add_selected"}])"));
    TestTrue(TEXT("Duplicate watch entries rejected"),EyeControl(Out->GetArrayField(TEXT("ui")),TEXT("feedback"))->GetStringField(TEXT("text")).Contains(TEXT("already")));
    Step(TEXT(R"([{"type":"change","id":"enabled_2","value":false}])"));
    TestEqual(TEXT("Rules can be disabled individually"),Out->GetStringField(TEXT("status")),FString(TEXT("0 matches")));
    Step(TEXT(R"([{"type":"change","id":"enabled_2","value":true}])"));
    // A new VM restores saved preferences, without restoring session-local GUIDs.
    FACEPluginVM Restored;Restored.Load(Script,Error);Snapshot->SetObjectField(TEXT("state"),ExactSaved);Snapshot->SetArrayField(TEXT("events"),{});
    TestTrue(TEXT("Saved watch list survives a new VM"),Restored.Step(Snapshot,Empty,Out,Error));
    TestEqual(TEXT("Exact matching restored"),Out->GetStringField(TEXT("status")),FString(TEXT("1 matches")));
    // Exercise sound separately from chat: these requests share a host rate
    // limit, but retrying one channel must not repeat the other channel.
    FACEPluginVM AudioVM;AudioVM.Load(Script,Error);
    auto AudioSnapshot=MakeShared<FJsonObject>(*Snapshot);AudioSnapshot->SetArrayField(TEXT("objects"),KnownObjects);
    AudioSnapshot->SetObjectField(TEXT("state"),EyeJSON(TEXT(R"({"rules":[{"name":"Black Coral"}],"chat_alerts":false})")));
    TSharedPtr<FJsonObject> AudioOutput;
    auto AudioStep=[&](const FString& Events=TEXT("[]"))
    {
        AudioSnapshot->SetArrayField(TEXT("events"),EyeJSON(TEXT("{\"events\":")+Events+TEXT("}"))->GetArrayField(TEXT("events")));
        AudioSnapshot->SetNumberField(TEXT("time"),AudioSnapshot->GetNumberField(TEXT("time"))+2);
        const bool Ok=AudioVM.Step(AudioSnapshot,Empty,AudioOutput,Error);
        TestTrue(TEXT("Audio watch callback stays within sandbox limits"),Ok);if(!Ok)AddError(Error);return Ok;
    };
    auto AudioReply=[&](const FString& Id,bool Ok,const FString& Message=TEXT(""),double Duration=0)
    {
        auto Event=MakeShared<FJsonObject>();Event->SetStringField(TEXT("type"),TEXT("result"));Event->SetStringField(TEXT("id"),Id);
        Event->SetBoolField(TEXT("ok"),Ok);Event->SetStringField(TEXT("error"),Message);
        Event->SetNumberField(TEXT("duration"),Duration);
        FString Encoded;FJsonSerializer::Serialize(Event,TJsonWriterFactory<>::Create(&Encoded));
        return AudioStep(TEXT("[")+Encoded+TEXT("]"));
    };
    if(!AudioStep())return false;
    TestTrue(TEXT("A saved watch rule is silent until audio is enabled"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    AudioStep(TEXT(R"([{"type":"change","id":"sound_alerts","value":true},{"type":"change","id":"sound_volume","value":50}])"));
    const auto AudioSaved=AudioOutput->GetObjectField(TEXT("state"));
    TestTrue(TEXT("Audio opt-in persists"),AudioSaved->GetBoolField(TEXT("sound_alerts")));
    TestEqual(TEXT("Volume percentage saves as normalized gain"),AudioSaved->GetNumberField(TEXT("sound_volume")),.5);
    TestTrue(TEXT("Enabling audio does not replay old sightings"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    AudioSnapshot->SetArrayField(TEXT("objects"),{});AudioStep();
    AudioSnapshot->SetNumberField(TEXT("time"),AudioSnapshot->GetNumberField(TEXT("time"))+31);AudioStep();
    AudioSnapshot->SetArrayField(TEXT("objects"),KnownObjects);AudioStep();
    auto Sounds=EyeRequests(AudioOutput,TEXT("sound"));
    if(!TestEqual(TEXT("One chime announces a burst of matching entities"),Sounds.Num(),1))return false;
    TestEqual(TEXT("Default chime is bundled"),Sounds[0]->GetStringField(TEXT("source")),FString(TEXT("bundled")));
    TestEqual(TEXT("Default chime uses included asset"),Sounds[0]->GetStringField(TEXT("asset")),FString(TEXT("alert.wav")));
    TestEqual(TEXT("Detection sound respects volume setting"),Sounds[0]->GetNumberField(TEXT("volume")),.5);
    TestTrue(TEXT("Sound can be enabled independently of chat"),EyeRequests(AudioOutput,TEXT("notice")).IsEmpty());
    AudioReply(Sounds[0]->GetStringField(TEXT("id")),false,TEXT("Service rate limit; retry after 0.5 seconds"));
    Sounds=EyeRequests(AudioOutput,TEXT("sound"));
    if(!TestEqual(TEXT("A rate-limited chime retries once without losing the burst"),Sounds.Num(),1))return false;
    AudioReply(Sounds[0]->GetStringField(TEXT("id")),true);
    TestTrue(TEXT("Acknowledged sightings do not repeat audio"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    AudioSnapshot->SetArrayField(TEXT("objects"),{});AudioStep();AudioSnapshot->SetArrayField(TEXT("objects"),KnownObjects);AudioStep();
    TestTrue(TEXT("Brief awareness flicker does not repeat the chime"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    AudioStep(TEXT(R"([{"type":"change","id":"watching","value":false}])"));
    AudioSnapshot->SetArrayField(TEXT("objects"),{MakeShared<FJsonValueObject>(EyeJSON(TEXT(R"({"id":50,"name":"Black Coral Spawn","wcid":10,"distance":5,"dead":false})")))});AudioStep();
    TestTrue(TEXT("Pausing the watcher suppresses newly matching sounds"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    AudioStep(TEXT(R"([{"type":"change","id":"watching","value":true},{"type":"change","id":"sound_alerts","value":false}])"));
    TestTrue(TEXT("Disabling audio clears pending automatic chimes"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    AudioStep(TEXT(R"([{"type":"sound_file","id":"choose_sound","ok":true,"asset":"selected-bell.wav","name":"My preferred bell.wav"}])"));
    const auto CustomSaved=AudioOutput->GetObjectField(TEXT("state"));
    TestEqual(TEXT("Custom sound token is saved"),CustomSaved->GetStringField(TEXT("sound_asset")),FString(TEXT("selected-bell.wav")));
    TestEqual(TEXT("Custom sound source is saved"),CustomSaved->GetStringField(TEXT("sound_source")),FString(TEXT("custom")));
    AudioStep(TEXT(R"([{"type":"click","id":"test_sound"}])"));Sounds=EyeRequests(AudioOutput,TEXT("sound"));
    if(!TestEqual(TEXT("Test sound works while automatic alerts are off"),Sounds.Num(),1))return false;
    TestEqual(TEXT("Preview uses chosen custom sound"),Sounds[0]->GetStringField(TEXT("asset")),FString(TEXT("selected-bell.wav")));
    TestEqual(TEXT("Preview reads custom storage"),Sounds[0]->GetStringField(TEXT("source")),FString(TEXT("custom")));
    AudioReply(Sounds[0]->GetStringField(TEXT("id")),false,TEXT("Selected sound is unavailable"));
    auto AudioFeedback=EyeControl(AudioOutput->GetArrayField(TEXT("ui")),TEXT("sound_feedback"));
    if(TestTrue(TEXT("Invalid sound reports feedback in Audio settings"),AudioFeedback.IsValid()))
    {
        TestTrue(TEXT("Sound failure remains readable"),AudioFeedback->GetStringField(TEXT("text")).Contains(TEXT("unavailable")));
        TestTrue(TEXT("Long sound feedback uses an ellipsis"),AudioFeedback->GetBoolField(TEXT("ellipsis")));
        TestEqual(TEXT("Full sound failure remains available in tooltip"),AudioFeedback->GetStringField(TEXT("tooltip")),FString(TEXT("Selected sound is unavailable")));
    }
    AudioStep();TestTrue(TEXT("Invalid custom audio does not retry forever"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    AudioStep(TEXT(R"([{"type":"sound_file","id":"choose_sound","ok":false,"error":"Choose a 16-bit PCM WAV file"}])"));
    TestFalse(TEXT("Failed import does not overwrite persisted sound preferences"),AudioOutput->HasField(TEXT("state")));
    TestEqual(TEXT("Failed import preserves the selected sound"),EyeControl(AudioOutput->GetArrayField(TEXT("ui")),TEXT("sound_name"))->GetStringField(TEXT("text")),FString(TEXT("Sound: My preferred bell.wav")));
    TestTrue(TEXT("Failed import explains the file requirement"),EyeControl(AudioOutput->GetArrayField(TEXT("ui")),TEXT("sound_feedback"))->GetStringField(TEXT("text")).Contains(TEXT("16-bit PCM")));
    AudioStep(TEXT(R"([{"type":"sound_file","id":"choose_sound","ok":false,"cancelled":true}])"));
    AudioStep(TEXT(R"([{"type":"click","id":"test_sound"}])"));Sounds=EyeRequests(AudioOutput,TEXT("sound"));
    if(TestEqual(TEXT("Cancelling file selection keeps the prior sound"),Sounds.Num(),1))
    {
        TestEqual(TEXT("Cancel preserves preferred asset"),Sounds[0]->GetStringField(TEXT("asset")),FString(TEXT("selected-bell.wav")));
        AudioReply(Sounds[0]->GetStringField(TEXT("id")),true,TEXT(""),8);
        TestFalse(TEXT("Test button waits until the full custom clip finishes"),EyeControl(AudioOutput->GetArrayField(TEXT("ui")),TEXT("test_sound"))->GetBoolField(TEXT("enabled")));
        AudioSnapshot->SetArrayField(TEXT("objects"),{MakeShared<FJsonValueObject>(EyeJSON(TEXT(R"({"id":51,"name":"Black Coral Spawn","wcid":10,"distance":5,"dead":false})")))});
        AudioStep(TEXT(R"([{"type":"change","id":"sound_alerts","value":true}])"));
        TestTrue(TEXT("A new burst does not overlap a long custom clip"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
        AudioSnapshot->SetArrayField(TEXT("objects"),{MakeShared<FJsonValueObject>(EyeJSON(TEXT(R"({"id":52,"name":"Black Coral Spawn","wcid":10,"distance":5,"dead":false})")))});
        AudioStep();TestTrue(TEXT("Additional sightings remain coalesced while the clip plays"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
        AudioStep();Sounds=EyeRequests(AudioOutput,TEXT("sound"));
        if(TestEqual(TEXT("Coalesced sightings play once after the eight-second clip"),Sounds.Num(),1))
        {
            AudioReply(Sounds[0]->GetStringField(TEXT("id")),true,TEXT(""),8);
            AudioStep(TEXT(R"([{"type":"change","id":"sound_alerts","value":false},{"type":"click","id":"test_sound"}])"));
            TestTrue(TEXT("Manual preview respects an already playing automatic clip"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
            AudioStep();TestTrue(TEXT("Preview stays queued until playback completes"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
            AudioStep();Sounds=EyeRequests(AudioOutput,TEXT("sound"));
            if(TestEqual(TEXT("Queued preview plays after cooldown with automatic alerts off"),Sounds.Num(),1))
            {
                TestTrue(TEXT("Cooldown preserves manual preview identity"),Sounds[0]->GetStringField(TEXT("id")).StartsWith(TEXT("preview_sound_")));
                AudioReply(Sounds[0]->GetStringField(TEXT("id")),true);
            }
        }
    }
    AudioStep(TEXT(R"([{"type":"click","id":"default_sound"}])"));
    TestEqual(TEXT("Default reset restores included chime"),AudioOutput->GetObjectField(TEXT("state"))->GetStringField(TEXT("sound_asset")),FString(TEXT("alert.wav")));
    TestEqual(TEXT("Default reset preserves volume"),AudioOutput->GetObjectField(TEXT("state"))->GetNumberField(TEXT("sound_volume")),.5);
    TestTrue(TEXT("Resetting a sound does not play it unexpectedly"),EyeRequests(AudioOutput,TEXT("sound")).IsEmpty());
    FACEPluginVM RestoredAudio;RestoredAudio.Load(Script,Error);AudioSnapshot->SetObjectField(TEXT("state"),CustomSaved);
    AudioSnapshot->SetArrayField(TEXT("events"),EyeJSON(TEXT(R"({"events":[{"type":"click","id":"test_sound"}]})"))->GetArrayField(TEXT("events")));
    TestTrue(TEXT("Custom audio preference survives a new plugin VM"),RestoredAudio.Step(AudioSnapshot,Empty,AudioOutput,Error));
    Sounds=EyeRequests(AudioOutput,TEXT("sound"));
    if(TestEqual(TEXT("Restored custom preference can be previewed"),Sounds.Num(),1))
        TestEqual(TEXT("Restored preview uses preferred asset"),Sounds[0]->GetStringField(TEXT("asset")),FString(TEXT("selected-bell.wav")));
    // Maximum list against 512 distinct names. The script must yield within the
    // real instruction/heap budgets, eventually evaluate every name, and paginate UI.
    FACEPluginVM Crowded;Crowded.Load(Script,Error);auto Stress=EyeJSON(TEXT(R"({"rules":[],"chat_alerts":false})"));
    TArray<TSharedPtr<FJsonValue>> Rules,Objects;
    for(int32 I=0;I<128;++I)Rules.Add(MakeShared<FJsonValueObject>(EyeJSON(FString::Printf(TEXT("{\"name\":\"%s\",\"exact\":false}"),I==127?TEXT("Target"):*FString::Printf(TEXT("Absent%d"),I)))));
    for(int32 I=0;I<512;++I)Objects.Add(MakeShared<FJsonValueObject>(EyeJSON(FString::Printf(TEXT("{\"id\":%d,\"name\":\"Target %d\",\"wcid\":10,\"distance\":%g,\"dead\":false}"),I+10,I,I*.4))));
    Stress->SetArrayField(TEXT("rules"),Rules);auto Busy=MakeShared<FJsonObject>(*Snapshot);Busy->SetObjectField(TEXT("state"),Stress);Busy->SetArrayField(TEXT("objects"),Objects);
    bool Finished=false;
    for(int32 Tick=0;Tick<20;++Tick)
    {
        if(!TestTrue(TEXT("Crowded watch list stays within VM budget"),Crowded.Step(Busy,Empty,Out,Error))){AddError(Error);return false;}
        if(Out->GetStringField(TEXT("status"))==TEXT("512 matches")){Finished=true;break;}
    }
    TestTrue(TEXT("Every crowded-area name is eventually scanned"),Finished);
    TestEqual(TEXT("Markers remain bounded"),Out->GetArrayField(TEXT("markers")).Num(),32);
    // Traverse full lists, including the final partial page. Pagination must
    // reach everything even in a short window, without requiring scrolling.
    Busy->SetObjectField(TEXT("panel"),EyeJSON(TEXT(R"({"width":480,"height":440})")));
    TSet<FString> VisitedRules,VisitedObjects;
    for(int32 Page=0;Page<260;++Page)
    {
        if(!TestTrue(TEXT("Pagination stays in VM budget"),Crowded.Step(Busy,Empty,Out,Error)))return false;
        const auto& UI=Out->GetArrayField(TEXT("ui"));
        for(const auto& V:EyeControl(UI,TEXT("setup_tab"))->GetArrayField(TEXT("children")))
        {const auto Id=V->AsObject()->GetStringField(TEXT("id"));if(Id.StartsWith(TEXT("rule_")))VisitedRules.Add(Id);}
        for(const auto& V:EyeControl(UI,TEXT("detected_tab"))->GetArrayField(TEXT("children")))
        {const auto Id=V->AsObject()->GetStringField(TEXT("id"));if(Id.StartsWith(TEXT("object_")))VisitedObjects.Add(Id);}
        if(!EyeControl(UI,TEXT("rules_next"))->GetBoolField(TEXT("enabled"))&&!EyeControl(UI,TEXT("detected_next"))->GetBoolField(TEXT("enabled")))break;
        Busy->SetArrayField(TEXT("events"),EyeJSON(TEXT(R"({"events":[{"type":"click","id":"rules_next"},{"type":"click","id":"detected_next"}]})"))->GetArrayField(TEXT("events")));
    }
    TestEqual(TEXT("All watch rules reachable by pages"),VisitedRules.Num(),128);
    TestEqual(TEXT("All detections reachable by pages"),VisitedObjects.Num(),512);
    Busy->SetArrayField(TEXT("events"),{});

    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* W=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);auto* GI=NewObject<UGameInstance>(GEngine);W->SetGameInstance(GI);
    Context.OwningGameInstance=GI;Context.SetCurrentWorld(W);GI->OnWorldChanged(nullptr,W);GI->Init();
    auto* H=GI->GetSubsystem<UACEPluginSubsystem>();auto* C=GI->GetSubsystem<UACEClientSubsystem>();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/ThirdEye")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    H->StorageRoot=Root;H->Settings=MakeShared<FJsonObject>();
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(W);W->DestroyWorld(false);IFileManager::Get().DeleteDirectory(*Root,false,true);};
    H->Discover();auto P=H->Find(TEXT("thirdeye"));if(!TestTrue(TEXT("Bundled optional plugin discovered"),P&&P->Companion&&!P->Enabled))return false;
    TestEqual(TEXT("Distinct Third Eye name"),P->Name,FString(TEXT("Third Eye")));
    TestFalse(TEXT("Observer does not require outgoing chat"),P->Permissions.Contains(TEXT("say")));
    TestTrue(TEXT("Third Eye declares its bounded sound permission"),P->Permissions.Contains(TEXT("sound")));
    TestTrue(TEXT("Crowded output is valid native UI"),H->ValidatePluginUI(Out->GetArrayField(TEXT("ui")),Error));
    C->Session=MakeShared<FACESession>();auto& Session=*C->Session;Session.State=EACESessionState::InWorld;Session.PlayerGuid=0x50000001;Session.ServerName=TEXT("Third Eye Test");
    Session.PlayerPosition.CellId=0x7D64000D;Session.PlayerPosition.Location=FVector(34,104,12);Session.PlayerVitals.bValid=true;Session.PlayerVitals.Health=100;Session.PlayerVitals.MaxHealth=100;
    FACEWorldObject Player;Player.Guid=Session.PlayerGuid;Player.Name=TEXT("Observer");Player.Position=Session.PlayerPosition;Player.bIsPlayer=true;Session.WorldObjects.Add(Player.Guid,Player);
    FACEWorldObject Golem;Golem.Guid=1;Golem.Name=TEXT("Black Coral Golem");Golem.ItemType=ACEItemType::Creature;Golem.bHasPosition=true;Golem.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;Golem.Position=Session.PlayerPosition;Golem.Position.Location.X+=5;Session.WorldObjects.Add(Golem.Guid,Golem);
    // Enable the existing local-chat recorder for this test. Third Eye itself
    // needs no chat-reading permission to display a notice.
    P->Permissions.Add(TEXT("chat"));
    H->SetEnabled(P->Id,true);H->TickCompanions();TestTrue(TEXT("Enabled Third Eye starts on login without Start"),P->Running);
    P->SavedState=ExactSaved;P->StateDirty=true;H->FlushPluginState(*P);H->Stop(P->Id);H->Start(P->Id);
    const int32 BeforePackets=Session.CachedC2SPackets.Num();
    H->MovementOwner=TEXT("ucm");H->FastCastOwner=TEXT("ucm");
    auto NativeStep=[&]()
    {
        P->NextService=0;
        const bool Ok=P->VM->Step(H->PublicSnapshot(*P),P->Profile,Out,Error)&&H->ApplyPublicOutput(*P,Out,Error);
        TestTrue(TEXT("Real host accepts Third Eye output"),Ok);if(!Ok)AddError(Error);return Ok;
    };
    if(!NativeStep())return false;
    bool GreenNotice=false;
    for(const auto& E:H->PluginChatEvents)GreenNotice|=E.Value->AsObject()->GetStringField(TEXT("raw_text")).StartsWith(TEXT("[Third Eye] Matching spawn detected:"))&&E.Value->AsObject()->GetNumberField(TEXT("color"))==ACEChatMessageType::Broadcast;
    TestTrue(TEXT("Green detection notice reaches local chat"),GreenNotice);
    TestEqual(TEXT("Detection never sends server packets"),Session.CachedC2SPackets.Num(),BeforePackets);
    TestEqual(TEXT("Detection leaves UCM movement ownership"),H->MovementOwner,FString(TEXT("ucm")));
    const int32 BeforeChat=H->PluginChatEvents.Num();NativeStep();TestEqual(TEXT("Host does not repeat detection chat"),H->PluginChatEvents.Num(),BeforeChat);
    P->Permissions.Remove(TEXT("chat"));
    TestTrue(TEXT("Detection button selects live object"),H->ClickPluginRow(P->Id,TEXT("select_1")));TestEqual(TEXT("Selected object is correct"),C->GetSelectedObject().Guid,1);
    Session.WorldObjects.Remove(1);TestFalse(TEXT("Vanished target cannot be selected"),H->ClickPluginRow(P->Id,TEXT("select_1")));Session.WorldObjects.Add(1,Golem);
    {
        const auto Waypoint=H->Find(TEXT("waypoint"));if(!TestTrue(TEXT("Waypoint is available for Third Eye arrows"),Waypoint.IsValid()))return false;
        const bool WasWaypointEnabled=Waypoint->Enabled;
        const auto OriginalPlayer=Session.PlayerPosition,OriginalTarget=Golem.Position;
        ON_SCOPE_EXIT
        {
            Session.PlayerPosition=OriginalPlayer;Player.Position=OriginalPlayer;Session.WorldObjects.Add(Player.Guid,Player);
            Golem.Position=OriginalTarget;Session.WorldObjects.Add(Golem.Guid,Golem);H->SetEnabled(TEXT("waypoint"),WasWaypointEnabled);
        };
        H->SetEnabled(TEXT("waypoint"),true);
        // Different room cells in one dungeon must share the masked landblock
        // used by Waypoint's arrow and destination markers.
        Session.PlayerPosition.CellId=0x01430100;Player.Position=Session.PlayerPosition;Session.WorldObjects.Add(Player.Guid,Player);
        Golem.Position.CellId=0x01430101;Session.WorldObjects.Add(Golem.Guid,Golem);
        if(!NativeStep())return false;
        TestTrue(TEXT("Third Eye Arrow accepts a target in another room of the same dungeon"),H->ClickPluginRow(P->Id,TEXT("nav_1")));
        FVector2D Destination;uint32 Dungeon=0;
        TestTrue(TEXT("Third Eye Arrow sets a destination"),H->GetWaypoint(Destination,Dungeon));
        TestEqual(TEXT("Dungeon target uses the canonical masked landblock"),Dungeon,uint32(0x01430000));
        TestEqual(TEXT("Third Eye saves the canonical landblock rather than relying on legacy read repair"),Waypoint->Profile->GetNumberField(TEXT("dungeon_landblock")),double(0x01430000));
        TestTrue(TEXT("Dungeon Arrow uses the target's actual coordinates"),Destination.Equals(ACEWaypoint::Coordinates(Golem.Position),1.e-6));
        Session.WorldObjects.Remove(Golem.Guid);
        TestFalse(TEXT("A vanished target cannot redirect the arrow"),H->ClickPluginRow(P->Id,TEXT("nav_1")));
        FVector2D Unchanged;uint32 UnchangedDungeon=0;
        TestTrue(TEXT("Rejected Arrow keeps the previous destination"),H->GetWaypoint(Unchanged,UnchangedDungeon)&&Unchanged.Equals(Destination,1.e-6)&&UnchangedDungeon==Dungeon);
        Session.PlayerPosition=OriginalPlayer;Player.Position=OriginalPlayer;Session.WorldObjects.Add(Player.Guid,Player);
        Golem.Position=OriginalTarget;Session.WorldObjects.Add(Golem.Guid,Golem);
        if(!NativeStep())return false;
        TestTrue(TEXT("Third Eye Arrow accepts a wilderness target after a dungeon target"),H->ClickPluginRow(P->Id,TEXT("nav_1")));
        TestTrue(TEXT("Wilderness Arrow clears the dungeon restriction"),H->GetWaypoint(Destination,Dungeon)&&Dungeon==0);
        TestTrue(TEXT("Wilderness Arrow uses the target's actual coordinates"),Destination.Equals(ACEWaypoint::Coordinates(Golem.Position),1.e-6));
    }
    // Exercise the actual native validator/importer against the shipped asset,
    // not a synthetic successful service result. Imported audio survives moving
    // or deleting the selected original and remains isolated to this plugin.
    FString PreferredSound;
    {
        const FString DefaultPath=Base/TEXT("ClientMods/thirdeye/sounds/alert.wav");
        TArray<uint8> Chime;FWaveModInfo Wave;
        if(!TestTrue(TEXT("Bundled default chime exists"),FFileHelper::LoadFileToArray(Chime,*DefaultPath)))return false;
        if(!TestTrue(TEXT("Bundled default chime is a valid WAV"),Wave.ReadWaveInfo(Chime.GetData(),Chime.Num())))return false;
        TestTrue(TEXT("Bundled chime contains non-silent PCM16 audio"),Wave.pBitsPerSample&&*Wave.pBitsPerSample==16&&Wave.SampleDataSize>0
            &&TArrayView<const uint8>(Wave.SampleDataStart,Wave.SampleDataSize).ContainsByPredicate([](uint8 B){return B!=0;}));
        const FString Source=Root/TEXT("User selection/My preferred bell.wav");IFileManager::Get().MakeDirectory(*FPaths::GetPath(Source),true);
        TestTrue(TEXT("Create user-selected test WAV"),FFileHelper::SaveArrayToFile(Chime,*Source));
        auto Imported=MakeShared<FJsonObject>();Error.Reset();
        if(!TestTrue(TEXT("A user-selected valid WAV can be imported"),H->ImportPluginSound(*P,Source,Imported,Error)))return false;
        PreferredSound=Imported->GetStringField(TEXT("asset"));
        TestEqual(TEXT("Import retains friendly basename"),Imported->GetStringField(TEXT("name")),FString(TEXT("My preferred bell.wav")));
        TestTrue(TEXT("Import yields an opaque filename rather than a file path"),PreferredSound==FPaths::GetCleanFilename(PreferredSound)&&PreferredSound!=FPaths::GetCleanFilename(Source));
        auto Play=EyeJSON(TEXT(R"({"requests":[{"id":"audio_test","service":"sound","operation":"play","source":"custom","asset":"","volume":0.5}]})"));
        auto Request=Play->GetArrayField(TEXT("requests"))[0]->AsObject();Request->SetStringField(TEXT("asset"),PreferredSound);
        auto PlayAccepted=[&]()
        {
            P->NextService=0;H->ApplyPluginServices(*P,Play,Error);
            return P->PluginEvents.Last()->AsObject()->GetBoolField(TEXT("ok"));
        };
        IFileManager::Get().Delete(*Source,false,true);
        TestTrue(TEXT("Imported audio plays after original file is removed"),PlayAccepted());
        H->ApplyPluginServices(*P,Play,Error);
        TestFalse(TEXT("Audio previews respect the service rate limit"),P->PluginEvents.Last()->AsObject()->GetBoolField(TEXT("ok")));
        P->Permissions.Remove(TEXT("sound"));
        TestFalse(TEXT("Playback requires sound permission"),PlayAccepted());
        Error.Reset();TestFalse(TEXT("Import requires sound permission"),H->ImportPluginSound(*P,DefaultPath,MakeShared<FJsonObject>(),Error));
        P->Permissions.Add(TEXT("sound"));
        Request->SetStringField(TEXT("source"),TEXT("bundled"));Request->SetStringField(TEXT("asset"),TEXT("alert.wav"));
        TestTrue(TEXT("Default shipped chime passes native playback validation"),PlayAccepted());
        Request->SetStringField(TEXT("asset"),TEXT("../alert.wav"));TestFalse(TEXT("Audio playback cannot traverse outside sounds directory"),PlayAccepted());
        Request->SetStringField(TEXT("asset"),DefaultPath);TestFalse(TEXT("Plugin sound requests cannot read arbitrary absolute paths"),PlayAccepted());
        Request->SetStringField(TEXT("source"),TEXT("custom"));Request->SetStringField(TEXT("asset"),TEXT("alert.wav"));
        TestFalse(TEXT("Custom playback requires a picker-issued token"),PlayAccepted());
        Request->SetStringField(TEXT("asset"),TEXT("sound-0000000000000000000000000000000000000000.wav"));
        TestFalse(TEXT("Missing imported sounds report failure"),PlayAccepted());
        auto Other=MakeShared<FACEClientPlugin>();Other->Id=TEXT("other_sound_plugin");Other->Permissions.Add(TEXT("sound"));
        Request->SetStringField(TEXT("asset"),PreferredSound);Error.Reset();
        TestFalse(TEXT("Another plugin cannot play Third Eye's imported file"),H->SoundService(*Other,Request,MakeShared<FJsonObject>(),Error));
        const FString Invalid=Root/TEXT("User selection/invalid.wav");
        TArray<uint8> Bad=Chime;Bad.SetNum(100);
        TestTrue(TEXT("Create truncated WAV fixture"),FFileHelper::SaveArrayToFile(Bad,*Invalid));Error.Reset();
        TestFalse(TEXT("Truncated WAV sample data is rejected"),H->ImportPluginSound(*P,Invalid,MakeShared<FJsonObject>(),Error));
        Bad=Chime;Bad[34]=8;Bad[35]=0;
        TestTrue(TEXT("Create unsupported-format WAV fixture"),FFileHelper::SaveArrayToFile(Bad,*Invalid));Error.Reset();
        TestFalse(TEXT("Unsupported sample format is rejected"),H->ImportPluginSound(*P,Invalid,MakeShared<FJsonObject>(),Error));
        Bad.SetNumZeroed(1024*1024+1);
        TestTrue(TEXT("Create oversized WAV fixture"),FFileHelper::SaveArrayToFile(Bad,*Invalid));Error.Reset();
        TestFalse(TEXT("Oversized custom files are rejected before playback"),H->ImportPluginSound(*P,Invalid,MakeShared<FJsonObject>(),Error));
        TestTrue(TEXT("Failed imports preserve the previous valid sound"),PlayAccepted());
        TestEqual(TEXT("Sound import and playback send no gameplay packets"),Session.CachedC2SPackets.Num(),BeforePackets);
    }
    // Notification permissions, line validation and rate limit all use the native service.
    auto Notice=EyeJSON(TEXT(R"({"requests":[{"id":"notice_test","service":"notice","operation":"show","text":"local only"}]})"));
    P->Permissions.Remove(TEXT("ui"));P->NextService=0;H->ApplyPluginServices(*P,Notice,Error);
    TestFalse(TEXT("No UI permission denies local notice"),P->PluginEvents.Last()->AsObject()->GetBoolField(TEXT("ok")));P->Permissions.Add(TEXT("ui"));
    P->NextService=0;H->ApplyPluginServices(*P,Notice,Error);H->ApplyPluginServices(*P,Notice,Error);
    TestFalse(TEXT("Local notices are rate limited"),P->PluginEvents.Last()->AsObject()->GetBoolField(TEXT("ok")));
    P->SavedState->SetBoolField(TEXT("sound_alerts"),true);P->SavedState->SetNumberField(TEXT("sound_volume"),.5);
    P->SavedState->SetStringField(TEXT("sound_source"),TEXT("custom"));P->SavedState->SetStringField(TEXT("sound_asset"),PreferredSound);
    P->SavedState->SetStringField(TEXT("sound_label"),TEXT("My preferred bell.wav"));P->StateDirty=true;H->FlushPluginState(*P);
    H->Stop(P->Id);const FString Path=P->StatePath;Session.PlayerGuid++;H->Start(P->Id);
    TestTrue(TEXT("Watch lists are character-scoped"),P->StatePath!=Path&&!P->SavedState->HasField(TEXT("rules")));
    TestFalse(TEXT("A different character does not inherit audio opt-in"),P->SavedState->HasField(TEXT("sound_alerts")));
    H->Stop(P->Id);Session.PlayerGuid--;H->Start(P->Id);NativeStep();
    TestEqual(TEXT("Original watch list restored from disk"),P->SavedState->GetArrayField(TEXT("rules")).Num(),1);
    TestTrue(TEXT("Original character audio opt-in restored from disk"),P->SavedState->GetBoolField(TEXT("sound_alerts")));
    TestEqual(TEXT("Original character preferred audio restored from disk"),P->SavedState->GetStringField(TEXT("sound_asset")),PreferredSound);
    TestEqual(TEXT("Original character alert volume restored from disk"),P->SavedState->GetNumberField(TEXT("sound_volume")),.5);
    if(FApp::CanEverRender())
    {
        GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(ACETestDatDirectory());
        TestFalse(TEXT("Third Eye opts out of host scrolling"),P->ScrollPanel());
        FWidgetRenderer Renderer(false,true);auto Panel=H->MakePanel(P->Id);
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName)> FindTag;
        FindTag=[&](TSharedRef<SWidget> Widget,FName Tag)->TSharedPtr<SWidget>
        {
            if(Widget->GetTag()==Tag)return Widget;
            auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=FindTag(Children->GetChildAt(I),Tag))return Found;
            return nullptr;
        };
        TFunction<bool(TSharedRef<SWidget>)> HasScroll=[&](TSharedRef<SWidget> Widget)
        {
            if(!Widget->GetVisibility().IsVisible())return false;
            if(Widget->GetType()==TEXT("SScrollBox")||Widget->GetType()==TEXT("SScrollBar"))return true;
            auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(HasScroll(Children->GetChildAt(I)))return true;
            return false;
        };
        // Fresh crowded VM starts on page one; resize the same retained panel.
        // Keep the real preferred basename in its tooltip while constraining
        // its label, including a physical small window at 135% Windows scale.
        const FString LongSoundName=TEXT("A very long preferred notification bell name with enough detail to exceed the narrow panel width.wav");
        Stress->SetStringField(TEXT("sound_source"),TEXT("custom"));Stress->SetStringField(TEXT("sound_asset"),TEXT("selected-bell.wav"));
        Stress->SetStringField(TEXT("sound_label"),LongSoundName);
        Crowded.Load(Script,Error);
        for(float UIScale:{1.f,1.35f})for(const FVector2D Size:{FVector2D(780,650),FVector2D(480,440),FVector2D(1000,360)})
        {
            auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            auto Frame=[&]()
            {
                auto Public=H->PublicSnapshot(*P);const TSharedPtr<FJsonObject>* Available=nullptr;
                if(Public->TryGetObjectField(TEXT("panel"),Available))Busy->SetObjectField(TEXT("panel"),*Available);
                if(Crowded.Step(Busy,Empty,Out,Error)){P->PanelRows=Out->GetArrayField(TEXT("ui"));++P->PanelRevision;}
                else AddError(Error);
                ++GFrameCounter;Renderer.DrawWidget(Target,Panel,UIScale,Size,0);FlushRenderingCommands();
            };
            for(int I=0;I<12;++I)Frame();
            for(const TCHAR* Page:{TEXT("detected"),TEXT("setup"),TEXT("options"),TEXT("audio")})
            {
                if(UIScale!=1.f&&FString(Page)!=TEXT("audio"))continue;
                auto Tab=FindTag(Panel,FName(*FString::Printf(TEXT("PluginTab.tabs.%s_tab"),Page)));
                if(!TestTrue(TEXT("Third Eye tab exists"),Tab.IsValid()))continue;
                StaticCastSharedPtr<SACEPluginTab>(Tab)->SimulateClick();for(int I=0;I<4;++I)Frame();
                TestFalse(TEXT("Third Eye has no visible scrollbar or scroll box"),HasScroll(Panel));
                const FString Prefix=FString(Page)==TEXT("setup")?TEXT("rules"):TEXT("detected");
                if(FString(Page)==TEXT("detected")||FString(Page)==TEXT("setup"))
                {
                    auto Prev=FindTag(Panel,FName(*(TEXT("PluginRow.")+Prefix+TEXT("_prev"))));
                    auto Next=FindTag(Panel,FName(*(TEXT("PluginRow.")+Prefix+TEXT("_next"))));
                    if(TestTrue(TEXT("Both pagination buttons present"),Prev&&Next))
                    {
                        TestTrue(TEXT("Pagination arrows keep compact widths"),Prev->GetCachedGeometry().GetLocalSize().X<60&&Next->GetCachedGeometry().GetLocalSize().X<60);
                        TestTrue(TEXT("Pagination arrows stay together"),Next->GetCachedGeometry().GetAbsolutePosition().X-Prev->GetCachedGeometry().GetAbsolutePosition().X<180);
                    }
                    const auto TabId=FString(Page)==TEXT("setup")?TEXT("setup_tab"):TEXT("detected_tab");
                    const auto& Children=EyeControl(P->PanelRows,TabId)->GetArrayField(TEXT("children"));
                    const FString LastId=Children.Last()->AsObject()->GetStringField(TEXT("id"));
                    auto LastRow=FindTag(Panel,FName(*(TEXT("PluginControl.")+LastId)));
                    if(TestTrue(TEXT("Last page row is arranged"),LastRow.IsValid()))
                    {
                        const auto& G=LastRow->GetCachedGeometry();
                        TestTrue(FString::Printf(TEXT("%s rows fit %.0fx%.0f"),Page,Size.X,Size.Y),G.LocalToAbsolute(G.GetLocalSize()).Y<=Size.Y+1);
                    }
                }
                else if(FString(Page)==TEXT("audio"))
                {
                    for(const TCHAR* Control:{TEXT("sound_alerts"),TEXT("sound_name"),TEXT("sound_buttons"),TEXT("sound_volume"),TEXT("sound_feedback"),TEXT("sound_help")})
                    {
                        auto Row=FindTag(Panel,FName(*(FString(TEXT("PluginControl."))+Control)));
                        if(TestTrue(FString::Printf(TEXT("Audio control %s exists"),Control),Row.IsValid()))
                        {
                            const auto& G=Row->GetCachedGeometry();const auto Start=G.GetAbsolutePosition();const auto End=G.LocalToAbsolute(G.GetLocalSize());
                            TestTrue(FString::Printf(TEXT("Audio %s fits %.0fx%.0f at %.0f%%"),Control,Size.X,Size.Y,UIScale*100),
                                Start.X>=-1&&Start.Y>=-1&&End.X<=Size.X+1&&End.Y<=Size.Y+1);
                        }
                    }
                    for(const TCHAR* Button:{TEXT("choose_sound"),TEXT("default_sound"),TEXT("test_sound")})
                    {
                        auto Widget=FindTag(Panel,FName(*(FString(TEXT("PluginRow."))+Button)));
                        if(TestTrue(TEXT("Audio action button is arranged"),Widget.IsValid()))
                            TestEqual(TEXT("Audio action uses the retail-themed button"),Widget->GetType(),FName(TEXT("SACEPluginButton")));
                    }
                    const auto Name=EyeControl(P->PanelRows,TEXT("sound_name"));
                    if(TestTrue(TEXT("Sound filename control remains present"),Name.IsValid()))
                    {
                        TestFalse(TEXT("Long sound name stays on one line"),Name->GetBoolField(TEXT("wrap")));
                        TestTrue(TEXT("Long sound name uses an ellipsis"),Name->GetBoolField(TEXT("ellipsis")));
                        TestEqual(TEXT("Full custom basename remains available in tooltip"),Name->GetStringField(TEXT("tooltip")),LongSoundName);
                    }
                    for(const TCHAR* TabName:{TEXT("detected"),TEXT("setup"),TEXT("options"),TEXT("audio")})
                    {
                        auto Widget=FindTag(Panel,FName(*FString::Printf(TEXT("PluginTab.tabs.%s_tab"),TabName)));
                        if(TestTrue(TEXT("Four settings tabs remain accessible"),Widget.IsValid()))
                        {
                            const auto& G=Widget->GetCachedGeometry();const auto End=G.LocalToAbsolute(G.GetLocalSize());
                            TestTrue(TEXT("Tabs fit narrow scaled settings panel"),G.GetAbsolutePosition().X>=-1&&End.X<=Size.X+1&&End.Y<=Size.Y+1);
                        }
                    }
                }
                TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
                TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(int32(Size.X),int32(Size.Y),Pixels,Png);
                FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("ThirdEye-%s-%.0f-%.0f.png"),Page,Size.X,UIScale*100)));
            }
            Target->ReleaseResource();
        }
    }
    H->SetEnabled(P->Id,false);TestFalse(TEXT("Manager disable stops Third Eye"),P->Running);
    TestEqual(TEXT("Disabling Third Eye leaves UCM alone"),H->MovementOwner,FString(TEXT("ucm")));
    return true;
}
#endif
