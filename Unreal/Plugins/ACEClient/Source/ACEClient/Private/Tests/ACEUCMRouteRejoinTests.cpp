#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginSight.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMRouteRejoinTest,"ACE.Plugins.LongCombatRouteRejoin",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMRouteRejoinTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    FString Script,Error;FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"teleport_sequence":1,"movement_blocked_serial":0,"position":{"cell":30998795,"x":30,"y":0,"z":0},"spells":[],"targets":[],"inventory":[{"id":99,"type":256,"equipped":true,"identified":true,"can_wield":true,"weapon_skill":48,"damage":10,"name":"Thrown weapon"}]})"));};
    auto Profile=[&](){return Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"missile","approach_range":60,"radius":6,"navigation":true,"route":[{"cell":30998795,"x":25,"y":0,"z":0},{"cell":30998795,"x":45,"y":0,"z":0}]})"));};
    auto Step=[&](FACEPluginVM& VM,const auto& S,const auto& P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto At=[](const auto& S,double X,double Y){auto Pos=S->GetObjectField(TEXT("position"));Pos->SetNumberField(TEXT("x"),X);Pos->SetNumberField(TEXT("y"),Y);S->SetNumberField(TEXT("time"),S->GetNumberField(TEXT("time"))+.5);};
    for(bool Override:{false,true})
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("navigation"),false);P->SetBoolField(TEXT("manual_combat"),true);
        P->SetNumberField(TEXT("monster_default_priority"),5);P->SetNumberField(TEXT("monster_default_range"),12);
        auto Rule=Json(TEXT(R"({"name":"Rat"})"));
        if(Override){Rule->SetNumberField(TEXT("priority"),0);Rule->SetNumberField(TEXT("range"),6);}
        P->SetArrayField(TEXT("monsters"),{MakeShared<FJsonValueObject>(Rule)});
        auto Rat=Json(TEXT(R"({"id":50,"name":"Rat","identified":true,"distance":10,"cell":30998795,"x":30,"y":10,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Rat)});
        auto I=Step(VM,S,P);
        FString Action;I->TryGetStringField(TEXT("action"),Action);
        TestEqual(TEXT("Stationary missile attack respects the monster's effective range"),Action,FString(Override?TEXT(""):TEXT("attack")));
        TestEqual(TEXT("Inheritance never writes defaults into saved rule"),Rule->HasField(TEXT("priority")),Override);
        FACEPluginVM Ranking;Ranking.Load(Script,Error);
        auto Other=Json(TEXT(R"({"id":51,"name":"Tusker","identified":true,"distance":11,"cell":30998795,"x":30,"y":11,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Rat),MakeShared<FJsonValueObject>(Other)});
        I=Step(Ranking,S,P);
        TestEqual(TEXT("Explicit zero priority overrides positive global default"),I->GetNumberField(TEXT("target")),Override?51.:50.);
    }
    for(bool Manual:{false,true})for(bool EmptyRoute:{false,true})
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("manual_combat"),Manual);
        P->SetBoolField(TEXT("navigation"),EmptyRoute);
        if(EmptyRoute)P->SetArrayField(TEXT("route"),{});
        P->SetBoolField(TEXT("looting"),true);
        P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"action":"keep"})")))});
        P->SetArrayField(TEXT("monsters"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"name":"Boss","priority":10})")))});
        auto Boss=Json(TEXT(R"({"id":50,"name":"Boss","identified":false,"distance":40,"cell":30998795})"));
        auto Near=Json(TEXT(R"({"id":51,"name":"Guard","identified":true,"distance":6,"cell":30998795})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Boss),MakeShared<FJsonValueObject>(Near)});
        auto I=Step(VM,S,P);
        TestEqual(TEXT("An enemy at firing range wins over a distant higher-priority enemy"),I->GetStringField(TEXT("action")),FString(TEXT("attack")));
        TestEqual(TEXT("Fires at the reachable guard"),I->GetNumberField(TEXT("target")),51.);
        if(EmptyRoute)
            S->SetArrayField(TEXT("combat_events"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"serial":1,"time":100,"target":51,"missile_rejected_distance":6})")))});
        else Near->SetNumberField(TEXT("distance"),7);
        TestEqual(TEXT("Stops shooting when the locked enemy leaves range"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
        S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":100,"name":"Corpse","identified":true,"distance":2})")))});
        TestEqual(TEXT("Opens the corpse with both enemies still visible"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        S->SetBoolField(TEXT("loot_approaching"),true);S->SetBoolField(TEXT("busy"),true);
        TestFalse(TEXT("Distant enemies do not interrupt walking to the corpse"),Step(VM,S,P)->HasField(TEXT("action")));
        Near->SetNumberField(TEXT("distance"),4);
        TestEqual(TEXT("Enemy entering range becomes actionable again"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("pause_loot_approach")));
    }
    for(const FString Mode:{TEXT("melee"),TEXT("missile"),TEXT("magic")})
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        P->SetStringField(TEXT("combat"),Mode);P->SetBoolField(TEXT("manual_combat"),true);
        S->SetBoolField(TEXT("components_required"),false);
        auto Weapon=S->GetArrayField(TEXT("inventory"))[0]->AsObject();
        Weapon->SetNumberField(TEXT("type"),Mode==TEXT("magic")?32768:Mode==TEXT("melee")?1:256);
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":20,"name":"Acid Bolt","category":117,"school":1,"power":100,"skill":300})")))});
        auto Enemy=Json(TEXT(R"({"id":50,"name":"Tusker Guard","identified":true,"distance":60,"cell":30998795,"x":30,"y":60,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Enemy)});
        auto I=Step(VM,S,P);
        TestEqual(*FString::Printf(TEXT("%s acquires at 60m before the firing/melee distance"),*Mode),I->GetStringField(TEXT("action")),FString(TEXT("move")));
        TestEqual(TEXT("Pursuit identifies the selected mob before firing"),I->GetNumberField(TEXT("target")),50.);
        TestTrue(TEXT("Live search diagnostics show the actual effective range"),I->GetStringField(TEXT("targeting_status")).Contains(TEXT("Search: 60 m")));
        Enemy->SetNumberField(TEXT("distance"),1);At(S,30,59);I=Step(VM,S,P);
        TestEqual(TEXT("Attacks once inside the weapon range"),I->GetStringField(TEXT("action")),FString(Mode==TEXT("magic")?TEXT("cast"):TEXT("attack")));
    }
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("navigation"),false);P->SetNumberField(TEXT("radius"),90);
        auto Enemy=Json(TEXT(R"({"id":50,"name":"Tusker Guard","identified":true,"distance":61,"cell":30998795,"x":30,"y":61,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Enemy)});
        TestFalse(TEXT("Long firing distance must not enlarge the explicit 60m acquisition radius"),Step(VM,S,P)->HasField(TEXT("action")));
        P->SetNumberField(TEXT("approach_range"),0);
        TestEqual(TEXT("VT stationary zero approach distance retains attack-radius selection"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
    }
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("approach"),false);
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":50,"name":"Tusker Guard","identified":true,"distance":60,"cell":30998795,"x":30,"y":60,"z":0,"line_of_sight":true})")))});
        auto I=Step(VM,S,P);
        TestFalse(TEXT("Explicit pursuit off never chases the distant mob"),I->HasField(TEXT("target")));
        TestTrue(TEXT("Search diagnostics explain why acquisition is not a chase"),I->GetStringField(TEXT("targeting_status")).Contains(TEXT("Pursuit is off")));
        P->SetBoolField(TEXT("approach"),true);I=Step(VM,S,P);
        TestEqual(TEXT("Enabling pursuit takes effect without restarting the route"),I->GetNumberField(TEXT("target")),50.);
    }
    for(bool Joined:{false,true})for(bool Legacy:{false,true})
    {
        FACEPluginVM VM;TestTrue(TEXT("UCM policy loads"),VM.Load(Script,Error));auto S=Snapshot(),P=Profile();
        for(const auto& V:P->GetArrayField(TEXT("route"))){V->AsObject()->SetBoolField(TEXT("legacy"),Legacy);V->AsObject()->SetBoolField(TEXT("walk_first"),true);}
        S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":true})")));
        if(Joined)TestEqual(TEXT("Already following the ordered route"),Step(VM,S,P)->GetNumberField(TEXT("x")),45.);
        auto Target=Json(TEXT(R"({"id":50,"name":"Tusker Guard","identified":true,"distance":60,"cell":30998795,"x":30,"y":60,"z":0,"attack_height":2,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});
        auto I=Step(VM,S,P);TestEqual(TEXT("60m acquisition approaches before joining if combat is active"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
        TestTrue(TEXT("Host records movement during combat waits"),I->GetBoolField(TEXT("route_track_movement")));
        for(double Y:{10.,20.,30.,40.,50.,55.})
        {At(S,30,Y);Target->SetNumberField(TEXT("distance"),60-Y);I=Step(VM,S,P);}
        TestEqual(TEXT("Fires after reaching the requested 6m range"),I->GetStringField(TEXT("action")),FString(TEXT("attack")));
        S->SetArrayField(TEXT("targets"),{});
        // Nothing has direct visibility to the route from the end of the chase.
        S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":false})")));
        I=Step(VM,S,P);TestEqual(TEXT("Cancels the defeated target"),I->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
        I=Step(VM,S,P);
        for(double Y:{50.,40.,30.,20.,10.,0.})
        {
            TestEqual(TEXT("Long chase returns over its observed path instead of losing navigation"),I->GetStringField(TEXT("status")),FString(TEXT("Returning to route")));
            TestEqual(TEXT("Return follows the next visited point"),I->GetNumberField(TEXT("y")),Y);
            At(S,30,Y);
            if(Y==0)S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":true})")));
            I=Step(VM,S,P);
        }
        TestEqual(TEXT("Resumes the route after a 55m combat excursion"),I->GetNumberField(TEXT("x")),45.);
        TestFalse(TEXT("Return does not disable navigation"),I->GetBoolField(TEXT("route_join_pending")));
    }

    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        auto Target=Json(TEXT(R"({"id":50,"name":"Tusker Guard","identified":true,"distance":20,"cell":30998795,"x":36,"y":6,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});Step(VM,S,P);
        TArray<TSharedPtr<FJsonValue>> Samples;
        auto Sample=[&](double X,double Y){auto J=Json(TEXT(R"({"cell":30998795,"z":0})"));J->SetNumberField(TEXT("x"),X);J->SetNumberField(TEXT("y"),Y);Samples.Add(MakeShared<FJsonValueObject>(J));};
        for(int32 I=0;I<=6;++I)Sample(30,I);
        for(int32 I=1;I<=6;++I)Sample(30+I,6);
        S->SetArrayField(TEXT("route_motion"),Samples);At(S,36,6);Target->SetNumberField(TEXT("distance"),0);Step(VM,S,P);
        S->RemoveField(TEXT("route_motion"));S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);
        auto I=Step(VM,S,P);
        TestEqual(TEXT("Dense chase samples return directly along the straight corridor"),I->GetNumberField(TEXT("x")),30.);
        TestEqual(TEXT("Return retains the corner rather than cutting across a wall"),I->GetNumberField(TEXT("y")),6.);
        At(S,30,6);I=Step(VM,S,P);
        TestEqual(TEXT("After the corner returns to the actual route departure"),I->GetNumberField(TEXT("y")),0.);
        TestEqual(TEXT("Departure stays on the observed corridor"),I->GetNumberField(TEXT("x")),30.);
    }

    // Looting partway along a leg must continue forward. An excursion away
    // from that leg still follows its observed return path, including floors.
    for(bool Legacy:{false,true})for(const FVector Offset:{FVector(6,0,0),FVector(6,3,0),FVector(6,0,6)})
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        P->SetStringField(TEXT("combat"),TEXT("off"));P->SetBoolField(TEXT("looting"),true);
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("wcid"),99);
        P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"action":"keep"})")))});
        for(const auto& V:P->GetArrayField(TEXT("route"))){V->AsObject()->SetBoolField(TEXT("legacy"),Legacy);V->AsObject()->SetBoolField(TEXT("walk_first"),true);}
        S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":true})")));
        TestEqual(TEXT("Loot route starts toward the next ordered waypoint"),Step(VM,S,P)->GetNumberField(TEXT("x")),45.);
        S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":88,"name":"Corpse of rat","distance":6,"identified":true,"line_of_sight":true})")))});
        TestEqual(TEXT("Nearby corpse interrupts route travel"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        At(S,30+Offset.X,Offset.Y);S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("z"),Offset.Z);
        S->SetNumberField(TEXT("container"),88);S->SetArrayField(TEXT("contents"),{});
        TestEqual(TEXT("Finished corpse closes before travel resumes"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
        S->SetNumberField(TEXT("container"),0);auto I=Step(VM,S,P);
        const bool OnRoute=Offset.Y==0&&Offset.Z==0;
        TestEqual(TEXT("Loot on the route continues forward; off-route loot retains its return trail"),I->GetNumberField(TEXT("x")),OnRoute?45.:30.);
        TestEqual(TEXT("Only an off-route excursion retraces the departure point"),I->GetStringField(TEXT("status")).StartsWith(TEXT("Returning")),!OnRoute);
    }

    for(bool Legacy:{false,true})
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        P->SetStringField(TEXT("combat"),TEXT("off"));
        for(const auto& V:P->GetArrayField(TEXT("route"))){V->AsObject()->SetBoolField(TEXT("legacy"),Legacy);V->AsObject()->SetBoolField(TEXT("walk_first"),true);}
        At(S,25,0);
        auto I=Step(VM,S,P);
        TestEqual(TEXT("Reached entry immediately aims for the next waypoint"),I->GetNumberField(TEXT("x")),45.);
        P->SetBoolField(TEXT("reverse_route"),true);At(S,44.5,0);
        I=Step(VM,S,P);
        TestEqual(TEXT("Reached route end reverses without another idle poll"),I->GetNumberField(TEXT("x")),25.);
    }
    for(double Offset:{.3,.6})
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":true})")));
        Step(VM,S,P);
        auto Target=Json(TEXT(R"({"id":50,"name":"Rat","identified":true,"distance":20,"cell":30998795,"x":30,"y":10,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});Step(VM,S,P);
        At(S,30,10);Target->SetNumberField(TEXT("distance"),0);Step(VM,S,P);
        S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);Step(VM,S,P);
        At(S,30,Offset);auto I=Step(VM,S,P);
        TestEqual(TEXT("Near the departure point resumes forward instead of circling its exact center"),I->GetNumberField(TEXT("x")),45.);
        TestTrue(TEXT("Rejoined route continues in the same decision"),I->GetStringField(TEXT("status")).StartsWith(TEXT("Waypoint ")));
    }

    for(bool Clear:{false,true})
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        auto Destination=P->GetArrayField(TEXT("route"))[1]->AsObject();
        Destination->SetStringField(TEXT("kind"),TEXT("pause"));
        S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":true})")));
        S->SetBoolField(TEXT("route_shortcuts"),true);
        Step(VM,S,P);
        auto Target=Json(TEXT(R"({"id":50,"name":"Rat","identified":true,"distance":20,"cell":30998795,"x":36,"y":10,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});Step(VM,S,P);
        At(S,30,6);Target->SetNumberField(TEXT("distance"),10);Step(VM,S,P);
        At(S,36,10);Target->SetNumberField(TEXT("distance"),0);Step(VM,S,P);
        S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);
        auto I=Step(VM,S,P);
        TestEqual(TEXT("Recovery requests visibility from the actual departure"),I->GetNumberField(TEXT("route_shortcut_x")),30.);
        TestEqual(TEXT("Recovery retains the pending pause waypoint"),I->GetNumberField(TEXT("route_point")),2.);
        S->SetBoolField(TEXT("route_shortcut_checked"),true);
        if(Clear)S->SetObjectField(TEXT("route_shortcut"),Json(TEXT(R"({"cell":30998795,"x":36,"y":0,"z":0})")));
        I=Step(VM,S,P);
        S->RemoveField(TEXT("route_shortcut_checked"));S->RemoveField(TEXT("route_shortcut"));
        TestEqual(TEXT("Only a verified shortcut displaces the breadcrumb"),I->GetNumberField(TEXT("x")),Clear?36.:30.);
        TestEqual(TEXT("Recovery selects the projected point or the visited corner"),I->GetNumberField(TEXT("y")),Clear?0.:6.);
        if(Clear)
        {
            At(S,36,0);I=Step(VM,S,P);
            TestEqual(TEXT("Projection leads to the same ordered waypoint"),I->GetNumberField(TEXT("x")),45.);
            Destination->SetNumberField(TEXT("x"),49);
            P->SetNumberField(TEXT("route_position_revision"),1);
            I=Step(VM,S,P);
            TestEqual(TEXT("A live edit redirects the current waypoint"),I->GetNumberField(TEXT("x")),49.);
            At(S,49,0);
            TestEqual(TEXT("The edited waypoint still performs its action"),Step(VM,S,P)->GetStringField(TEXT("status")),FString(TEXT("Pausing on route")));
        }
    }

    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);auto* GI=NewObject<UGameInstance>(GEngine);
    World->SetGameInstance(GI);Context.OwningGameInstance=GI;Context.SetCurrentWorld(World);GI->OnWorldChanged(nullptr,World);GI->Init();
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);};
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    // Record a bent 60m approach while the policy is waiting for an action;
    // the old 15m gap heuristic discarded the entire return path here.
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot(),P=Profile();
        auto Target=Json(TEXT(R"({"id":50,"name":"Tusker Guard","identified":true,"distance":60,"cell":30998795,"x":60,"y":30,"z":0,"line_of_sight":true})"));
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});Step(VM,S,P);
        Host->bTrackRouteMovement=true;
        for(const FVector& V:{FVector(30,0,0),FVector(30,10,0),FVector(30,20,0),FVector(30,30,0),FVector(40,30,0),FVector(50,30,0),FVector(60,30,0)})
        {FACEPosition Pos;Pos.CellId=30998795;Pos.Location=V;Host->RecordRouteMovement(Pos,1);}
        TArray<TSharedPtr<FJsonValue>> Samples;
        for(const auto& Pos:Host->RouteMovementSamples){auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("cell"),uint32(Pos.CellId));J->SetNumberField(TEXT("x"),Pos.Location.X);J->SetNumberField(TEXT("y"),Pos.Location.Y);J->SetNumberField(TEXT("z"),Pos.Location.Z);Samples.Add(MakeShared<FJsonValueObject>(J));}
        S->SetArrayField(TEXT("route_motion"),Samples);At(S,60,30);Target->SetNumberField(TEXT("distance"),0);Step(VM,S,P);
        S->RemoveField(TEXT("route_motion"));S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);
        auto I=Step(VM,S,P);
        for(const FVector2D& V:{FVector2D(50,30),FVector2D(40,30),FVector2D(30,30),FVector2D(30,20),FVector2D(30,10),FVector2D(30,0)})
        {
            TestEqual(TEXT("Return retains corners traversed between decisions"),I->GetNumberField(TEXT("x")),V.X);
            TestEqual(TEXT("Return retains corner depth"),I->GetNumberField(TEXT("y")),V.Y);
            At(S,V.X,V.Y);I=Step(VM,S,P);
        }
        FACEPosition Teleported;Teleported.CellId=0x0143014F;Teleported.Location=FVector(2,3,0);
        Host->RecordRouteMovement(Teleported,2);
        TestEqual(TEXT("Teleport discards previous-area movement samples"),Host->RouteMovementSamples.Num(),1);
        Host->bTrackRouteMovement=false;
    }
    // Test the real floor/obstruction query, not a stubbed route_visible table.
    FACEPosition Pos;Pos.CellId=30998795;Pos.Location=FVector::ZeroVector;
    const FVector Origin=Pos.ToUnrealLocation();
    auto Box=[&](const FVector& Center,const FVector& Extent)
    {
        auto* Actor=World->SpawnActor<AActor>();auto* C=NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(C);Actor->AddInstanceComponent(C);C->SetBoxExtent(Extent);
        C->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);C->SetCollisionObjectType(ECC_WorldStatic);
        C->SetCollisionResponseToAllChannels(ECR_Block);C->RegisterComponent();Actor->SetActorLocation(Origin+Center);return Actor;
    };
    Box(FVector(0,0,-50),FVector(60000,60000,50));
    Box(FVector(-29000,0,100),FVector(100,500,100));
    FACEPluginSightQuery Sight(*World,nullptr);
    {
        FACEPosition From=Pos,End=Pos;
        From.Location=FVector(10,10,0);End.Location=FVector(30,0,0);
        auto Direct=Host->FindRouteShortcut(From,Pos,End,Sight);
        TestTrue(TEXT("Open ground allows steering directly to the waypoint"),Direct&&Direct->Location.Equals(End.Location));
        Box(FVector(-2000,500,100),FVector(100,100,100));
        auto Join=Host->FindRouteShortcut(From,Pos,End,Sight);
        TestTrue(TEXT("A blocked diagonal can rejoin a clear section of the leg"),Join&&Join->Location.Equals(FVector(10,0,0),.01));
        Box(FVector(-2000,500,100),FVector(1800,100,100));
        TestFalse(TEXT("A wall across both paths leaves the breadcrumb in control"),Host->FindRouteShortcut(From,Pos,End,Sight).IsSet());
        From.Location=FVector(0,20,0);End.Location=FVector(10,20,6);
        TestFalse(TEXT("An upstairs route needs a continuous walkable connection"),Host->FindRouteShortcut(From,Pos,End,Sight).IsSet());
    }
    auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("combat"),TEXT("off"));At(S,0,0);
    TArray<TSharedPtr<FJsonValue>> Points;
    for(int32 Index=0;Index<17;++Index)
    {
        auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("cell"),uint32(Pos.CellId));
        J->SetNumberField(TEXT("x"),Index<16?300+Index:0);J->SetNumberField(TEXT("y"),Index<16?0:330);J->SetNumberField(TEXT("z"),0);
        Points.Add(MakeShared<FJsonValueObject>(J));
    }
    P->SetArrayField(TEXT("route"),Points);FACEPluginVM VM;VM.Load(Script,Error);
    bool Rejoined=false;int32 PendingBatches=0;
    for(int32 Batch=0;Batch<20;++Batch)
    {
        Host->UpdateRouteVisibility(Pos,Points,Sight,S);
        const bool Pending=S->GetBoolField(TEXT("route_visible_pending"));auto I=Step(VM,S,P);
        if(Pending){++PendingBatches;TestEqual(TEXT("Budget exhaustion is still scanning, not unreachable"),I->GetStringField(TEXT("status")),FString(TEXT("Finding nearest route waypoint")));}
        else {Rejoined=I->HasField(TEXT("y"))&&I->GetNumberField(TEXT("y"))==330;break;}
    }
    TestTrue(TEXT("Long candidate paths require several bounded batches"),PendingBatches>=4);
    TestTrue(TEXT("Can join the 17th entry at 330m after rejecting nearer wall-separated points"),Rejoined);
    // A genuinely blocked different floor is still rejected; never bypass collision.
    Points.SetNum(1);Points[0]->AsObject()->SetNumberField(TEXT("x"),0);Points[0]->AsObject()->SetNumberField(TEXT("z"),6);
    Host->UpdateRouteVisibility(Pos,Points,Sight,S);
    TestFalse(TEXT("A different floor is not a reachable entry"),S->GetObjectField(TEXT("route_visible"))->GetBoolField(TEXT("1")));
    TestFalse(TEXT("Completed rejection is distinct from untested entries"),S->GetBoolField(TEXT("route_visible_pending")));
    return !HasAnyErrors();
}
#endif
