#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Mods/ACEPluginClientBus.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    TArray<TSharedPtr<FJsonValue>> Sub(const TCHAR* Plugin=TEXT("test"),const TCHAR* Channel=TEXT("team"),const TCHAR* Epoch=TEXT("one"))
    {
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("plugin"),Plugin);O->SetStringField(TEXT("channel"),Channel);O->SetStringField(TEXT("epoch"),Epoch);
        return {MakeShared<FJsonValueObject>(O)};
    }
    TSharedPtr<FJsonObject> Msg(const FString& To=FString(),const TCHAR* Plugin=TEXT("test"),const TCHAR* Channel=TEXT("team"))
    {
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("send"));O->SetStringField(TEXT("plugin"),Plugin);O->SetStringField(TEXT("channel"),Channel);
        O->SetStringField(TEXT("epoch"),TEXT("one"));if(!To.IsEmpty())O->SetStringField(TEXT("to"),To);
        auto Data=MakeShared<FJsonObject>();Data->SetStringField(TEXT("command"),TEXT("hello"));O->SetObjectField(TEXT("data"),Data);return O;
    }
    uint16 FreePort()
    {
        auto* SS=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);auto A=SS->CreateInternetAddr();bool Valid=false;A->SetIp(TEXT("127.0.0.1"),Valid);A->SetPort(0);
        auto* S=SS->CreateSocket(NAME_Stream,TEXT("channel test port"),false);if(!S)return 0;
        const bool OK=S->Bind(*A);if(OK)S->GetAddress(*A);S->Close();SS->DestroySocket(S);return OK?A->GetPort():0;
    }
    void Pump(const TArray<FACEPluginClientBus*>& Buses,int32 Count=40)
    {for(int32 I=0;I<Count;++I){for(auto* B:Buses)if(B)B->Tick(FPlatformTime::Seconds());FPlatformProcess::Sleep(.005f);}}
    TSharedPtr<FJsonObject> Parse(const TCHAR* Text)
    {TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginClientBusTest,"ACE.Plugins.ClientBus.Transport",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginClientBusTest::RunTest(const FString&)
{
    const uint16 Port=FreePort();if(!TestTrue(TEXT("Free loopback port"),Port!=0))return false;
    auto A=MakeUnique<FACEPluginClientBus>(TEXT("test-user"),Port);
    FACEPluginClientBus B(TEXT("test-user"),Port),C(TEXT("test-user"),Port),OtherServer(TEXT("test-user"),Port),OtherPlugin(TEXT("test-user"),Port),OtherChannel(TEXT("test-user"),Port);
    A->Configure(TEXT("server"),TEXT("Alice"),1,Sub());B.Configure(TEXT("server"),TEXT("Bob"),2,Sub());C.Configure(TEXT("server"),TEXT("Carol"),3,Sub());
    OtherServer.Configure(TEXT("elsewhere"),TEXT("Elsewhere"),4,Sub());OtherPlugin.Configure(TEXT("server"),TEXT("Plugin"),5,Sub(TEXT("other")));OtherChannel.Configure(TEXT("server"),TEXT("Channel"),6,Sub(TEXT("test"),TEXT("different")));
    TArray<FACEPluginClientBus*> Buses={A.Get(),&B,&C,&OtherServer,&OtherPlugin,&OtherChannel};Pump(Buses);
    for(auto* Bus:Buses)TestTrue(TEXT("Participants handshake"),Bus->Connected());
    TestEqual(TEXT("Same server/plugin peer discovery"),A->Peers(TEXT("test")).Num(),3);
    TestTrue(TEXT("Broadcast accepted"),A->Send(Msg()));Pump(Buses);
    auto Received=B.Drain();TestEqual(TEXT("Broadcast delivered"),Received.Num(),1);
    if(!Received.IsEmpty())
    {
        TestEqual(TEXT("Host stamps sender"),Received[0]->GetStringField(TEXT("sender")),A->Instance());
        TestEqual(TEXT("Character metadata"),Received[0]->GetStringField(TEXT("character")),FString(TEXT("Alice")));
        TestEqual(TEXT("Recipient subscription epoch"),Received[0]->GetStringField(TEXT("epoch")),FString(TEXT("one")));
        TestEqual(TEXT("Payload intact"),Received[0]->GetObjectField(TEXT("data"))->GetStringField(TEXT("command")),FString(TEXT("hello")));
    }
    TestEqual(TEXT("Second recipient receives broadcast"),C.Drain().Num(),1);
    TestEqual(TEXT("No self echo"),A->Drain().Num(),0);TestEqual(TEXT("Server isolation"),OtherServer.Drain().Num(),0);
    TestEqual(TEXT("Plugin isolation"),OtherPlugin.Drain().Num(),0);TestEqual(TEXT("Channel isolation"),OtherChannel.Drain().Num(),0);
    TestTrue(TEXT("Target send"),B.Send(Msg(C.Instance())));Pump(Buses);
    TestEqual(TEXT("Target only"),C.Drain().Num(),1);TestEqual(TEXT("Non-target excluded"),A->Drain().Num(),0);
    C.Configure(TEXT("server"),TEXT("Carol"),3,{});Pump(Buses);A->Send(Msg());Pump(Buses);
    TestEqual(TEXT("Unsubscribed peer receives nothing"),C.Drain().Num(),0);B.Drain();
    C.Configure(TEXT("server"),TEXT("Carol"),3,Sub(TEXT("test"),TEXT("team"),TEXT("two")));Pump(Buses);A->Send(Msg(C.Instance()));Pump(Buses);
    Received=C.Drain();TestEqual(TEXT("Resubscribe delivery"),Received.Num(),1);
    if(!Received.IsEmpty())TestEqual(TEXT("New subscription generation"),Received[0]->GetStringField(TEXT("epoch")),FString(TEXT("two")));
    // A owns the relay. Its destruction must not require an external service restart.
    A.Reset();Buses.RemoveAt(0);Pump(Buses,280);
    TestTrue(TEXT("Relay failover reconnects Bob"),B.Connected());TestTrue(TEXT("Relay failover reconnects Carol"),C.Connected());
    B.Send(Msg(C.Instance()));Pump(Buses);TestEqual(TEXT("Delivery after relay exit"),C.Drain().Num(),1);
    TestEqual(TEXT("No departed relay in peer list"),B.Peers(TEXT("test")).Num(),2);
    // Oversized frame is rejected before JSON parsing; healthy peers stay connected.
    auto* SS=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);auto Addr=SS->CreateInternetAddr();bool Valid=false;Addr->SetIp(TEXT("127.0.0.1"),Valid);Addr->SetPort(Port);
    auto* Bad=SS->CreateSocket(NAME_Stream,TEXT("invalid channel peer"),false);
    if(Bad&&Bad->Connect(*Addr)){uint8 Bytes[]={0,1,0,0};int32 Sent=0;Bad->Send(Bytes,4,Sent);Pump(Buses);}
    if(Bad){Bad->Close();SS->DestroySocket(Bad);}
    TestTrue(TEXT("Malformed connection does not break valid participants"),B.Connected()&&C.Connected());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginClientMessagingTest,"ACE.Plugins.ClientBus.Host",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginClientMessagingTest::RunTest(const FString&)
{
    FString Error;TSharedPtr<FJsonObject> Output;auto Empty=MakeShared<FJsonObject>();FACEPluginVM VM;
    TestTrue(TEXT("Lua channel output loads"),VM.Load(TEXT("return function() return {client_channels={'team'},client_messages={{channel='team',data={route={},count=7}}}} end"),Error));
    if(!TestTrue(TEXT("Lua channel output marshals"),VM.Step(Empty,Empty,Output,Error)))return false;
    TestEqual(TEXT("Lua subscription survives"),Output->GetArrayField(TEXT("client_channels")).Num(),1);
    TestTrue(TEXT("Payload keys are opaque, not native policy collections"),Output->GetArrayField(TEXT("client_messages"))[0]->AsObject()->GetObjectField(TEXT("data"))->GetObjectField(TEXT("route")).IsValid());
    FACEPluginVM Leave;Leave.Load(TEXT("return function() return {client_channels={},client_messages={}} end"),Error);Leave.Step(Empty,Empty,Output,Error);
    TestTrue(TEXT("Empty Lua channel list unsubscribes"),Output->GetArrayField(TEXT("client_channels")).IsEmpty());
    TestTrue(TEXT("Empty Lua message batch accepted"),Output->GetArrayField(TEXT("client_messages")).IsEmpty());
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* W=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
    auto* GI=NewObject<UGameInstance>(GEngine);W->SetGameInstance(GI);Context.OwningGameInstance=GI;Context.SetCurrentWorld(W);GI->OnWorldChanged(nullptr,W);GI->Init();
    ON_SCOPE_EXIT{GI->Shutdown();GEngine->DestroyWorldContext(W);W->DestroyWorld(false);};
    auto* H=GI->GetSubsystem<UACEPluginSubsystem>();auto* C=GI->GetSubsystem<UACEClientSubsystem>();
    H->StorageRoot=FPaths::ProjectSavedDir()/TEXT("Automation/ClientMessagingHost")/FGuid::NewGuid().ToString(EGuidFormats::Digits);H->Settings=MakeShared<FJsonObject>();
    const FString Example=IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("Docs/Plugins/Examples/client_channel");
    const FString Installed=H->StorageRoot/TEXT("Installed/client_channel");IFileManager::Get().MakeDirectory(*Installed,true);
    for(const TCHAR* File:{TEXT("plugin.json"),TEXT("main.lua"),TEXT("default.json")})TestEqual(TEXT("Example file copied"),IFileManager::Get().Copy(*(Installed/File),*(Example/File)),COPY_OK);
    H->Discover();const auto ExamplePlugin=H->Find(TEXT("client_channel"));
    TestTrue(TEXT("API 2 companion manifest accepts client_messaging"),ExamplePlugin&&ExamplePlugin->Permissions.Contains(TEXT("client_messaging")));
    FString Source;FFileHelper::LoadFileToString(Source,*(Example/TEXT("main.lua")));FACEPluginVM ExampleVM;
    TestTrue(TEXT("Example loads in sandbox"),ExampleVM.Load(Source,Error));
    TestTrue(TEXT("Example joins without a connection"),ExampleVM.Step(Empty,Empty,Output,Error));
    TestTrue(TEXT("Example uses valid themed native UI"),H->ValidatePluginUI(Output->GetArrayField(TEXT("ui")),Error));
    auto ExampleSnapshot=Parse(TEXT(R"({"client_bus":{"connected":true,"channels":["team"],"peers":[],"error":""},"events":[{"type":"command","text":"broadcast test"}]})"));
    TestTrue(TEXT("Example emits a broadcast from chat input"),ExampleVM.Step(ExampleSnapshot,Empty,Output,Error));
    TestEqual(TEXT("Example broadcast payload"),Output->GetArrayField(TEXT("client_messages"))[0]->AsObject()->GetObjectField(TEXT("data"))->GetStringField(TEXT("text")),FString(TEXT("broadcast test")));
    C->Session=MakeShared<FACESession>();C->Session->State=EACESessionState::InWorld;C->Session->PlayerGuid=0x50000001;C->Session->ServerName=TEXT("Client bus test");
    auto P=MakeShared<FACEClientPlugin>();P->Id=TEXT("client_test");P->Api=2;P->Enabled=P->Running=P->Companion=true;
    P->Player=C->GetPlayerGuid();P->Server=C->GetServerName();P->VMSession=C->Session;P->Profile=MakeShared<FJsonObject>();H->Plugins.Add(P);
    auto Join=Parse(TEXT(R"({"client_channels":["team"]})"));
    TestFalse(TEXT("Permission required"),H->ApplyClientMessaging(*P,Join,Error));
    P->Permissions.Add(TEXT("client_messaging"));TestTrue(TEXT("Permission allows join"),H->ApplyClientMessaging(*P,Join,Error));
    TestEqual(TEXT("Join retained"),P->ClientChannels.Num(),1);
    TestFalse(TEXT("Invalid channel"),H->ApplyClientMessaging(*P,Parse(TEXT(R"({"client_channels":["bad channel"]})")),Error));
    TestFalse(TEXT("Unsubscribed send rejected"),H->ApplyClientMessaging(*P,Parse(TEXT(R"({"client_messages":[{"channel":"elsewhere","data":{}}]})")),Error));
    TestFalse(TEXT("Non-object payload rejected"),H->ApplyClientMessaging(*P,Parse(TEXT(R"({"client_messages":[{"channel":"team","data":"bad"}]})")),Error));
    auto Large=Parse(TEXT(R"({"client_messages":[{"channel":"team","data":{}}]})"));
    Large->GetArrayField(TEXT("client_messages"))[0]->AsObject()->GetObjectField(TEXT("data"))->SetStringField(TEXT("text"),FString::ChrN(4097,'x'));
    TestFalse(TEXT("Payload bound"),H->ApplyClientMessaging(*P,Large,Error));
    P->ClientMessageWindow=FPlatformTime::Seconds();P->ClientMessageCount=20;
    TestTrue(TEXT("Rate limiting is nonfatal"),H->ApplyClientMessaging(*P,Parse(TEXT(R"({"client_messages":[{"channel":"team","data":{}}]})")),Error));
    TestTrue(TEXT("Rate-limit error exposed"),P->ClientChannelError.Contains(TEXT("rate limit")));
    P->PluginEvents.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"type":"client_message","channel":"team"})"))));
    H->ApplyClientMessaging(*P,Parse(TEXT(R"({"client_channels":[]})")),Error);
    TestTrue(TEXT("Leaving also clears already queued channel events"),P->PluginEvents.IsEmpty());H->ApplyClientMessaging(*P,Join,Error);
    P->SelectiveSnapshot=true;P->SnapshotFields={TEXT("client_bus")};
    TestTrue(TEXT("Selective snapshot includes channel status"),H->PublicSnapshot(*P)->HasField(TEXT("client_bus")));
    P->Permissions.Reset();TestFalse(TEXT("No channel discovery without permission"),H->PublicSnapshot(*P)->HasField(TEXT("client_bus")));P->Permissions.Add(TEXT("client_messaging"));
    const FString OldEpoch=P->ClientChannelEpoch;H->StopCompanion(*P,TEXT("test"));
    TestTrue(TEXT("Stop clears subscriptions"),P->ClientChannels.IsEmpty());TestTrue(TEXT("Stop invalidates stale messages"),P->ClientChannelEpoch.IsEmpty());
    P->Running=true;H->ApplyClientMessaging(*P,Join,Error);TestNotEqual(TEXT("Restart has a new subscription epoch"),P->ClientChannelEpoch,OldEpoch);
    C->Session->PlayerGuid++;H->TickClientMessaging(0);TestTrue(TEXT("Character change unsubscribes"),P->ClientChannels.IsEmpty());TestFalse(TEXT("Character change disconnects"),H->ClientBus.IsValid());
    TestFalse(TEXT("Previous character cannot publish"),H->ApplyClientMessaging(*P,Join,Error));
    C->Session->State=EACESessionState::Disconnected;H->TickClientMessaging(0);TestFalse(TEXT("Logout disconnects"),H->ClientBus.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginClientBusProcessTest,"ACE.Plugins.ClientBus.Process",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginClientBusProcessTest::RunTest(const FString&)
{
    int32 ChildPort=0;
    if(FParse::Value(FCommandLine::Get(),TEXT("ClientBusPeer="),ChildPort))
    {
        FACEPluginClientBus Bus(TEXT("process-test"),uint16(ChildPort));Bus.Configure(TEXT("server"),TEXT("Child process"),2,Sub());
        const double Deadline=FPlatformTime::Seconds()+40;bool Ack=false;
        while(FPlatformTime::Seconds()<Deadline)
        {
            Bus.Tick(FPlatformTime::Seconds());
            for(const auto& E:Bus.Drain())
            {
                auto Reply=Msg(E->GetStringField(TEXT("sender")));Reply->GetObjectField(TEXT("data"))->SetStringField(TEXT("command"),TEXT("child-ack"));
                Ack=Bus.Send(Reply);
            }
            if(Ack){Pump({&Bus},40);return true;}
            FPlatformProcess::Sleep(.01f);
        }
        AddError(TEXT("Child process did not receive parent message"));return false;
    }
    const uint16 Port=FreePort();if(!Port)return false;
    FACEPluginClientBus Bus(TEXT("process-test"),Port);Bus.Configure(TEXT("server"),TEXT("Parent process"),1,Sub());Pump({&Bus});
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/ClientBusProcess")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Root,true);
    // Let the automation runner propagate its result. -TestExit forces Unix
    // RequestExit without a status override, which exits 1 even after success.
    const FString Args=FString::Printf(TEXT("\"%s\" -NullRHI -unattended -nosplash -nosound -nop4 -nohmd -ClientBusPeer=%u -UserDir=\"%s/User/\" -abslog=\"%s/child.log\" -ExecCmds=\"Automation RunTests ACE.Plugins.ClientBus.Process; Quit\""),
        *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),Port,*Root,*Root);
    auto Process=FPlatformProcess::CreateProc(FPlatformProcess::ExecutablePath(),*Args,true,true,true,nullptr,0,nullptr,nullptr);
    if(!TestTrue(TEXT("Independent client process launched"),Process.IsValid()))return false;
    ON_SCOPE_EXIT{if(FPlatformProcess::IsProcRunning(Process))FPlatformProcess::TerminateProc(Process,true);FPlatformProcess::CloseProc(Process);};
    const double Deadline=FPlatformTime::Seconds()+100;bool Sent=false,Ack=false;
    while(FPlatformTime::Seconds()<Deadline)
    {
        Bus.Tick(FPlatformTime::Seconds());const auto Peers=Bus.Peers(TEXT("test"));
        if(!Sent&&!Peers.IsEmpty())Sent=Bus.Send(Msg(Peers[0]->AsObject()->GetStringField(TEXT("instance"))));
        for(const auto& E:Bus.Drain())if(E->GetObjectField(TEXT("data"))->GetStringField(TEXT("command"))==TEXT("child-ack"))Ack=true;
        if(!FPlatformProcess::IsProcRunning(Process))break;
        FPlatformProcess::Sleep(.01f);
    }
    int32 ExitCode=-1;FPlatformProcess::GetProcReturnCode(Process,&ExitCode);
    TestTrue(TEXT("Cross-process targeted round trip"),Sent&&Ack);TestEqual(TEXT("Independent client exited successfully"),ExitCode,0);
    AddInfo(TEXT("Child process log: ")+Root/TEXT("child.log"));return true;
}
#endif
