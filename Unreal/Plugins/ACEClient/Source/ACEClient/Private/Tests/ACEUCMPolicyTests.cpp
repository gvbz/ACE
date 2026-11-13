#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEVTProfile.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEDatSubsystem.h"
#include "Engine/GameInstance.h"
#include "Misc/ScopeExit.h"

namespace
{
    TSharedPtr<FJsonObject> ParseUCM(const TCHAR* S)
    {TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(S),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMFailureIsolationTest,"ACE.Plugins.ActivityFailureIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMFailureIsolationTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"nearest":0,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"position":{"cell":2103705613,"x":25,"y":97,"z":12},"spells":[],"enchantments":[],"targets":[],"inventory":[{"id":50,"wcid":50,"name":"Sword","type":1,"identified":true,"can_wield":true,"equipped":true,"damage":30,"damage_type":1},{"id":51,"wcid":51,"name":"Potion","type":32,"identified":true,"usable":true,"count":5,"boost_vital":2,"boost":50}]})"));};
    auto Profile=[](){return ParseUCM(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","looting":true,"loot_rules":[{"action":"salvage","name":"Ring"}]})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;const bool OK=VM.Step(S,P,I,Error);TestTrue(*Error,OK);return I?I:MakeShared<FJsonObject>();};
    auto Targets=[](auto S){S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true})")))});};
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Targets(S);
        P->SetStringField(TEXT("initial_state"),TEXT("Missing"));P->SetArrayField(TEXT("states"),{});
        TestEqual(TEXT("Invalid state reports a scoped failure"),Step(VM,S,P)->GetStringField(TEXT("activity")),FString(TEXT("meta")));
        TestEqual(TEXT("Invalid state cannot keep blocking base combat settings"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
    }
    // Trigger a real failed salvage, not just a synthetic error result.
    {
        FACEPluginVM VM;TestTrue(TEXT("Failure isolation policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();
        const auto Ring=ParseUCM(TEXT(R"({"id":701,"wcid":701,"name":"Ring","count":1,"identified":true,"material":59,"workmanship":7})"));
        S->SetNumberField(TEXT("container"),700);S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Ring)});
        TestEqual(TEXT("Loot starts normally"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
        auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(MakeShared<FJsonValueObject>(Ring));S->SetArrayField(TEXT("inventory"),Inventory);
        S->SetNumberField(TEXT("container"),0);S->SetArrayField(TEXT("contents"),{});
        auto I=Step(VM,S,P);TestEqual(TEXT("Missing Ust pauses only loot"),I->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        TestEqual(TEXT("Salvage failure retains its activity identity"),I->GetStringField(TEXT("activity")),FString(TEXT("loot")));
        Targets(S);S->SetNumberField(TEXT("time"),101);
        TestEqual(TEXT("Combat proceeds immediately after loot failure"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        S->SetNumberField(TEXT("health"),10);S->SetNumberField(TEXT("time"),102);
        TestEqual(TEXT("Critical-health recovery still uses supplies"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
        S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("time"),132);
        S->SetArrayField(TEXT("targets"),{});Step(VM,S,P); // cancel old attack
        I=Step(VM,S,P);TestFalse(TEXT("Discarded destructive job is never replayed after cooldown"),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Targets(S);
        P->SetBoolField(TEXT("buffing"),true);S->SetNumberField(TEXT("mana"),5);
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":20,"category":1,"power":100,"skill":300,"school":4,"self_buff":true})")))});
        auto I=Step(VM,S,P);TestEqual(TEXT("Low mana pauses only the buff activity"),I->GetStringField(TEXT("activity")),FString(TEXT("buffs")));
        TestEqual(TEXT("Buff failure cannot starve melee combat"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Targets(S);S->SetNumberField(TEXT("health"),10);
        for(int Attempt=0;Attempt<3;++Attempt)
        {
            S->SetNumberField(TEXT("time"),100+16*Attempt);
            TestEqual(TEXT("Recovery attempts remain bounded"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
            S->SetNumberField(TEXT("time"),101+16*Attempt);S->SetNumberField(TEXT("action_serial"),Attempt+1);S->SetNumberField(TEXT("action_error"),1);
            auto Receipt=ParseUCM(TEXT(R"({"type":"use","source":51,"error":1})"));
            Receipt->SetNumberField(TEXT("action_serial"),Attempt+1);Receipt->SetNumberField(TEXT("requested_at"),100+16*Attempt);
            S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});
            auto I=Step(VM,S,P);
            TestEqual(TEXT("Failed supply yields to combat during that method's cooldown"),I->GetStringField(TEXT("action")),FString(TEXT("attack")));
        }
        S->SetNumberField(TEXT("time"),134);
        TestEqual(TEXT("Recovery failure does not stop fighting at critical health"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        S->SetNumberField(TEXT("time"),149);S->SetNumberField(TEXT("action_error"),0);
        TestEqual(TEXT("Recovery retries after cooldown instead of remaining disabled"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
    }
    // Native execution may reject stale items after the Lua decision. Those
    // failures must return to the same isolation mechanism, across activities.
    for(const TCHAR* Activity:{TEXT("loot"),TEXT("vendors"),TEXT("components"),TEXT("item_mana"),TEXT("pets"),TEXT("combine"),TEXT("helper"),TEXT("dispel"),TEXT("buffs"),TEXT("buff_others"),TEXT("meta"),TEXT("inventory")})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Targets(S);
        auto Failure=MakeShared<FJsonObject>();Failure->SetStringField(TEXT("activity"),Activity);Failure->SetStringField(TEXT("status"),TEXT("Server rejected action"));P->SetObjectField(TEXT("ucm_activity_failure"),Failure);
        TestEqual(*FString::Printf(TEXT("%s rejection pauses activity"),Activity),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        P->RemoveField(TEXT("ucm_activity_failure"));S->SetNumberField(TEXT("time"),101);
        TestEqual(*FString::Printf(TEXT("Combat survives %s rejection"),Activity),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        S->SetNumberField(TEXT("health"),10);
        TestEqual(*FString::Printf(TEXT("Recovery survives %s rejection"),Activity),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Targets(S);S->SetArrayField(TEXT("inventory"),{});
        TestEqual(TEXT("Unavailable combat loadout pauses combat"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":700,"name":"Corpse","distance":1,"identified":true})")))});
        TestEqual(TEXT("Other activities continue when combat is unavailable"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Targets(S);
        // No usable heal: still fight rather than stopping at stop_health.
        S->SetNumberField(TEXT("health"),1);P->SetBoolField(TEXT("recovery"),false);
        TestEqual(TEXT("Critical health alone never cancels available combat"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        P->SetArrayField(TEXT("ucm_commands"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"op":"stop"})")))});
        TestEqual(TEXT("Explicit user Stop still stops UCM"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("stop")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMBuffMarginTest,"ACE.Plugins.BuffSkillMargin",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMBuffMarginTest::RunTest(const FString&)
{
    FString Source,Error;
    FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    // Exercise the actual policy, including the distinct imported buff margin.
    // Spell IDs are deliberately unordered: difficulty determines the tier.
    for(int32 School:{2,3,4})for(const auto& Case:TArray<FIntVector>{{200,150,50},{200,30,100},{200,0,200},{230,30,200},{229,30,100},{330,30,300}})
    {
        FACEPluginVM VM;
        if(!TestTrue(TEXT("Buff policy loads"),VM.Load(Source,Error)))return false;
        auto S=ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"enchantments":[],"trained_skills":[],"targets":[],"inventory":[{"id":99,"type":32768,"equipped":true,"identified":true,"can_wield":true}]})"));
        auto P=ParseUCM(TEXT(R"({"initial_state":"Default","states":[{"name":"Default"}],"buffing":true,"recovery":false,"combat":"off","skill_margin":500})"));
        P->SetNumberField(TEXT("buff_skill_margin"),Case.Y);
        TArray<TSharedPtr<FJsonValue>> Spells;
        for(int32 Power:{300,50,200,100})
        {
            auto Spell=ParseUCM(TEXT(R"({"category":1,"duration":300,"self_buff":true,"beneficial":true,"known":true})"));
            Spell->SetNumberField(TEXT("id"),1000-Power);Spell->SetNumberField(TEXT("power"),Power);
            Spell->SetNumberField(TEXT("school"),School);Spell->SetNumberField(TEXT("skill"),Case.X);
            Spells.Add(MakeShared<FJsonValueObject>(Spell));
        }
        S->SetArrayField(TEXT("spells"),Spells);
        TSharedPtr<FJsonObject> Intent;
        if(!TestTrue(*Error,VM.Step(S,P,Intent,Error))||!Intent)return false;
        TestEqual(*FString::Printf(TEXT("School %d skill %d buffer %d selects strongest qualifying tier"),School,Case.X,Case.Y),Intent->GetNumberField(TEXT("spell")),double(1000-Case.Z));
        // Profiles without a dedicated buff margin retain the existing fallback.
        FACEPluginVM Legacy;Legacy.Load(Source,Error);
        P->RemoveField(TEXT("buff_skill_margin"));P->SetNumberField(TEXT("skill_margin"),Case.Y);
        if(TestTrue(TEXT("Legacy margin policy runs"),Legacy.Step(S,P,Intent,Error))&&Intent)
            TestEqual(TEXT("Legacy shared margin produces the same buff tier"),Intent->GetNumberField(TEXT("spell")),double(1000-Case.Z));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMRouteJoinTest,"ACE.Plugins.RouteJoin",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMRouteJoinTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"nearest":0,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"position":{"cell":2103705613,"x":90,"y":97,"z":12},"spells":[],"targets":[],"inventory":[]})"));};
    auto Profile=[](){return ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","navigation":true,"route":[{"cell":2103705613,"x":0,"y":97,"z":12},{"cell":2103705613,"x":40,"y":97,"z":12},{"cell":2103705613,"x":100,"y":97,"z":12},{"cell":2103705613,"x":150,"y":97,"z":12}]})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    for(int Heading:{0,90,180,270})
    {
        FACEPluginVM VM;TestTrue(TEXT("Route policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();S->SetNumberField(TEXT("heading"),Heading);
        auto I=Step(VM,S,P);TestEqual(TEXT("Start approaches closest waypoint regardless of camera/heading"),I->GetNumberField(TEXT("x")),100.);
        TestEqual(TEXT("Overlay receives chosen route index"),I->GetNumberField(TEXT("route_point")),3.);
        S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),40);
        TestEqual(TEXT("Movement or combat detours do not continually reselect nearest point"),Step(VM,S,P)->GetNumberField(TEXT("x")),100.);
        S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),100);Step(VM,S,P);
        TestEqual(TEXT("After joining follows the next ordered point"),Step(VM,S,P)->GetNumberField(TEXT("x")),150.);
        P->SetNumberField(TEXT("vt_revision"),1);P->SetStringField(TEXT("vt_loaded_kind"),TEXT("nav"));S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),3);
        TestEqual(TEXT("Newly loaded route selects nearest again"),Step(VM,S,P)->GetNumberField(TEXT("x")),0.);
    }
    for(bool Reverse:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("loop_route"),true);P->SetBoolField(TEXT("reverse_route"),Reverse);
        S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),150);Step(VM,S,P);
        TestEqual(TEXT("Joining final point retains loop or reverse semantics"),Step(VM,S,P)->GetNumberField(TEXT("x")),Reverse?100.:0.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Point=P->GetArrayField(TEXT("route"))[1]->AsObject();Point->SetNumberField(TEXT("x"),90);Point->SetNumberField(TEXT("z"),50);
        TestEqual(TEXT("Nearest includes height instead of choosing another floor"),Step(VM,S,P)->GetNumberField(TEXT("x")),100.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Pos=S->GetObjectField(TEXT("position"));Pos->SetNumberField(TEXT("x"),190);
        auto Point=P->GetArrayField(TEXT("route"))[3]->AsObject();Point->SetNumberField(TEXT("cell"),2103705613u+0x1000000u);Point->SetNumberField(TEXT("x"),1);
        TestEqual(TEXT("Outdoor route joins across landblocks in world coordinates"),Step(VM,S,P)->GetNumberField(TEXT("cell")),double(2103705613u+0x1000000u));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Pos=S->GetObjectField(TEXT("position"));Pos->SetNumberField(TEXT("cell"),0x01430171);Pos->SetNumberField(TEXT("x"),49);Pos->SetNumberField(TEXT("y"),-75);Pos->SetNumberField(TEXT("z"),0);
        P->SetArrayField(TEXT("route"),ParseUCM(TEXT(R"({"route":[{"cell":2116833,"x":49,"y":20,"z":0,"legacy":true,"walk_first":true},{"cell":2113537,"x":49,"y":120,"z":0,"legacy":true,"walk_first":true}]})"))->GetArrayField(TEXT("route")));
        // Imported NAV stores absolute coordinates even when a dungeon extends
        // beyond its nominal block. 0x01420001 + y=120 is local y=-72 in 0x0143.
        P->GetArrayField(TEXT("route"))[0]->AsObject()->SetNumberField(TEXT("cell"),0x01430001);
        P->GetArrayField(TEXT("route"))[1]->AsObject()->SetNumberField(TEXT("cell"),0x01420001);
        TestEqual(TEXT("Imported negative dungeon coordinates choose correct anchor"),Step(VM,S,P)->GetNumberField(TEXT("y")),120.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("cell"),0x01430171);
        TestEqual(TEXT("Different native dungeon does not send player to unrelated start"),Step(VM,S,P)->GetStringField(TEXT("status")),FString(TEXT("No reachable route waypoint in this area")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Point=P->GetArrayField(TEXT("route"))[2]->AsObject();Point->SetStringField(TEXT("kind"),TEXT("jump"));Point->SetNumberField(TEXT("charge"),.8);
        TestEqual(TEXT("Join approaches jump launch point first"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("move")));
        S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),100);
        TestEqual(TEXT("Join retains launch action"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("jump")));
        S->SetNumberField(TEXT("time"),104);S->SetBoolField(TEXT("jumping"),false);
        Step(VM,S,P);TestEqual(TEXT("Landing continues forward instead of rejoining launch"),Step(VM,S,P)->GetNumberField(TEXT("x")),150.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Point=P->GetArrayField(TEXT("route"))[1]->AsObject();Point->SetBoolField(TEXT("legacy"),true);Point->SetBoolField(TEXT("walk_first"),false);Point->SetStringField(TEXT("kind"),TEXT("jump"));Point->SetNumberField(TEXT("x"),90);
        TestEqual(TEXT("Imported action placeholder is not a join destination"),Step(VM,S,P)->GetNumberField(TEXT("x")),100.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();TArray<TSharedPtr<FJsonValue>> Points;
        for(int I=0;I<2048;++I){auto Point=ParseUCM(TEXT(R"({"cell":2103705613,"x":0,"y":97,"z":12})"));Point->SetNumberField(TEXT("x"),I);Points.Add(MakeShared<FJsonValueObject>(Point));}P->SetArrayField(TEXT("route"),Points);S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),2000.5);
        TSharedPtr<FJsonObject> I;for(int Tick=0;Tick<10;++Tick){I=Step(VM,S,P);if(I->GetStringField(TEXT("status"))!=TEXT("Finding nearest route waypoint"))break;}
        TestEqual(TEXT("Maximum route joins and skips both reached points within the instruction budget"),I->GetNumberField(TEXT("route_point")),2003.);
        TestEqual(TEXT("Maximum route continues toward the first point outside arrival tolerance"),I->GetNumberField(TEXT("x")),2002.);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMPolicyTest,"ACE.Plugins.Decisions",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMPolicyTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"nearest":0,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"position":{"cell":2103705613,"x":25,"y":97,"z":12},"spells":[],"enchantments":[],"trained_skills":[],"targets":[],"inventory":[{"id":99,"wcid":99,"name":"Orb","type":32768,"equipped":true,"identified":true,"can_wield":true,"count":1}]})"));};
    auto Profile=[](){return ParseUCM(TEXT(R"({"initial_state":"Default","states":[{"name":"Default"}],"buffing":false,"recovery":false,"combat":"off","skill_margin":30})"));};
    auto Array=[](auto O,const TCHAR* Key,const TCHAR* Entries){O->SetArrayField(Key,ParseUCM(Entries)->GetArrayField(TEXT("values")));};
    auto Check=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;const bool OK=VM.Step(S,P,I,Error);TestTrue(*Error,OK);return I?I:MakeShared<FJsonObject>();};
    auto Start=[&](FACEPluginVM& VM){TestTrue(TEXT("Policy loads"),VM.Load(Source,Error));};
    for(bool Navigation:{false,true})for(bool Priority:{false,true})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetStringField(TEXT("combat"),TEXT("magic"));P->SetBoolField(TEXT("idle_peace"),true);P->SetBoolField(TEXT("navigation"),Navigation);P->SetBoolField(TEXT("nav_priority"),Priority);
        S->SetNumberField(TEXT("combat_mode"),8);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":105,"z":12}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Idle peace works with route and navigation priority"),I->GetStringField(TEXT("action")),FString(TEXT("combat_mode")));TestEqual(TEXT("Idle stance is peace"),I->GetNumberField(TEXT("mode")),1.);
        S->SetNumberField(TEXT("combat_mode"),1);I=Check(VM,S,P);
        TestFalse(TEXT("Confirmed peace does not repeatedly request stance changes"),I->GetStringField(TEXT("action"))==TEXT("combat_mode"));
        if(Navigation)TestEqual(TEXT("Walking resumes after switching to peace"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
        P->SetBoolField(TEXT("nav_priority"),false);
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":201,"category":117,"power":100,"skill":300,"school":4}]})"));
        Array(S,TEXT("targets"),TEXT(R"({"values":[{"id":500,"name":"Rat","distance":10,"identified":true,"line_of_sight":true}]})"));
        I=Check(VM,S,P);TestEqual(TEXT("New nearby target produces a magic cast from peace mode"),I->GetStringField(TEXT("action")),FString(TEXT("cast")));
        // Native cast dispatch enters magic mode; simulate its optimistic mode
        // update and server cast completion before the target disappears.
        S->SetNumberField(TEXT("combat_mode"),8);S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),201);S->SetArrayField(TEXT("targets"),{});
        TestEqual(TEXT("Target loss cancels attack before resting"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
        TestEqual(TEXT("Returns to peace after the target is gone"),Check(VM,S,P)->GetNumberField(TEXT("mode")),1.);
        P->SetBoolField(TEXT("idle_peace"),false);I=Check(VM,S,P);
        TestFalse(TEXT("Toggle off leaves current stance alone"),I->GetStringField(TEXT("action"))==TEXT("combat_mode"));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("idle_peace"),true);P->SetBoolField(TEXT("looting"),true);S->SetNumberField(TEXT("combat_mode"),8);
        Array(P,TEXT("loot_rules"),TEXT(R"({"values":[{"action":"keep"}]})"));
        Array(S,TEXT("corpses"),TEXT(R"({"values":[{"id":700,"name":"Corpse","distance":5,"line_of_sight":true}]})"));
        auto Idle=Check(VM,S,P);
        TestEqual(TEXT("Enabled idle peace precedes looting in a clear area"),Idle->GetStringField(TEXT("action")),FString(TEXT("combat_mode")));
        TestEqual(TEXT("Clear-area looting requests peace stance"),Idle->GetNumberField(TEXT("mode")),1.);
        S->SetNumberField(TEXT("combat_mode"),1);
        TestEqual(TEXT("Corpse looting continues after peace is confirmed"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        S->SetBoolField(TEXT("busy"),true);TestFalse(TEXT("Idle toggle does not interrupt pending game action"),Check(VM,S,P)->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetStringField(TEXT("combat"),TEXT("missile"));P->SetBoolField(TEXT("navigation"),true);P->SetNumberField(TEXT("monster_attempts"),1);P->SetNumberField(TEXT("monster_blacklist_seconds"),120);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":105,"z":12}]})"));
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":50,"name":"Thrown weapon","type":256,"identified":true,"can_wield":true,"equipped":true,"damage":20}]})"));
        Array(S,TEXT("targets"),TEXT(R"({"values":[{"id":500,"name":"Wasp","distance":20,"identified":true,"line_of_sight":true}]})"));
        TestEqual(TEXT("Unhittable policy starts with an ordinary attack"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Array(S,TEXT("combat_events"),TEXT(R"({"values":[{"serial":1,"target":500,"hit":false}]})"));
        TestEqual(TEXT("Exactly the failure limit still permits a retry"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        TestEqual(TEXT("Old outcomes are not counted twice"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Array(S,TEXT("combat_events"),TEXT(R"({"values":[{"serial":2,"target":500,"hit":true},{"serial":3,"target":500,"hit":false}]})"));
        TestEqual(TEXT("An actual damage hit clears previous environment failures"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Array(S,TEXT("combat_events"),TEXT(R"({"values":[{"serial":4,"target":500,"hit":false}]})"));
        TestEqual(TEXT("Exceeding failure limit cancels a repeating attack"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
        TestEqual(TEXT("Blacklisted target yields to navigation"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("move")));
        S->SetNumberField(TEXT("time"),219);TestEqual(TEXT("Target remains excluded before expiry"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("move")));
        S->SetNumberField(TEXT("time"),220);TestEqual(TEXT("Exact timeout re-enables the target"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Array(S,TEXT("combat_events"),TEXT(R"({"values":[{"serial":5,"target":500,"hit":false}]})"));Check(VM,S,P);
        auto Targets=S->GetArrayField(TEXT("targets"));S->SetArrayField(TEXT("targets"),{});Check(VM,S,P);S->SetArrayField(TEXT("targets"),Targets);
        Array(S,TEXT("combat_events"),TEXT(R"({"values":[{"serial":6,"target":500,"hit":false}]})"));
        TestEqual(TEXT("Leaving awareness clears failure history for reused IDs"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Array(S,TEXT("combat_events"),TEXT(R"({"values":[{"serial":7,"target":500,"hit":false,"unhittable":true}]})"));
        TestEqual(TEXT("Permanent spell target failure bypasses retry threshold"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
        TestEqual(TEXT("Permanent spell failure lets the route continue"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("dispel_items"),true);
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":80,"name":"Chocolate Gromnie","usable":true,"count":5},{"id":81,"name":"Society Gem of Dispelling","usable":true,"count":5,"int_properties":{"280":222}}]})"));
        Array(S,TEXT("harmful_enchantments"),TEXT(R"({"values":[{"id":77,"category":102,"power":400,"remaining":100}]})"));
        TestEqual(TEXT("350-power supply is skipped for a 400-power vulnerability"),Check(VM,S,P)->GetNumberField(TEXT("item")),81.);
        S->SetNumberField(TEXT("action_serial"),1);
        TestFalse(TEXT("UseDone alone does not authorize another dispel"),Check(VM,S,P)->HasField(TEXT("item")));
        S->SetArrayField(TEXT("harmful_enchantments"),{});
        TestFalse(TEXT("Confirmed removal completes dispel without using another supply"),Check(VM,S,P)->HasField(TEXT("item")));
        Array(S,TEXT("harmful_enchantments"),TEXT(R"({"values":[{"id":78,"category":116,"power":100,"remaining":100}]})"));
        TestFalse(TEXT("Imperil alone is not VT's elemental-vulnerability trigger"),Check(VM,S,P)->HasField(TEXT("item")));
        Array(S,TEXT("harmful_enchantments"),TEXT(R"({"values":[{"id":79,"category":104,"power":300,"remaining":100}]})"));
        S->SetObjectField(TEXT("cooldowns"),ParseUCM(TEXT(R"({"222":30})")));
        TestEqual(TEXT("Cooling-down gem falls back to eligible chocolate"),Check(VM,S,P)->GetNumberField(TEXT("item")),80.);
        S->SetNumberField(TEXT("time"),109);
        TestFalse(TEXT("Missing effect removal does not rapidly consume supplies"),Check(VM,S,P)->HasField(TEXT("item")));
        S->SetNumberField(TEXT("time"),124);
        TestEqual(TEXT("Unconfirmed dispel can retry after bounded backoff"),Check(VM,S,P)->GetNumberField(TEXT("item")),80.);
        FACEPluginVM Caster;Start(Caster);P->SetBoolField(TEXT("dispel_items"),false);P->SetBoolField(TEXT("dispel_self"),true);
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":2500,"category":250,"school":2,"power":350,"skill":500,"caster_target":true,"beneficial":true}]})"));
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":99,"name":"Orb","type":32768,"equipped":true,"identified":true,"can_wield":true}]})"));
        TestFalse(TEXT("Self dispel requires Chorizite even with enough casting skill"),Check(Caster,S,P)->HasField(TEXT("spell")));
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":100,"name":"Chorizite","count":5})"))));S->SetArrayField(TEXT("inventory"),Items);
        TestEqual(TEXT("Eligible Life self dispel uses the player target"),Check(Caster,S,P)->GetNumberField(TEXT("target")),1.);
    }
    {
        // One continuous run crosses activity boundaries. Isolated policy tests
        // missed lost final-item acknowledgements and routes starved by looting.
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("buffing"),true);P->SetBoolField(TEXT("navigation"),true);P->SetBoolField(TEXT("looting"),true);P->SetStringField(TEXT("combat"),TEXT("magic"));
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":110,"z":12}]})"));
        Array(P,TEXT("loot_rules"),TEXT(R"({"values":[{"action":"salvage","min_workmanship":7}]})"));
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":11,"category":1,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true},{"id":12,"category":115,"power":100,"skill":300,"school":2,"duration":300,"self_buff":true},{"id":13,"category":154,"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":1},{"id":201,"category":117,"power":100,"skill":300,"school":4},{"id":202,"category":121,"power":200,"skill":300,"school":4}]})"));
        Array(S,TEXT("targets"),TEXT(R"({"values":[{"id":500,"name":"Monster","distance":20,"identified":true,"line_of_sight":false,"resists":{"8":0.1,"32":1.5}}]})"));
        TArray<TSharedPtr<FJsonValue>> Confirmed;int Serial=0;
        for(int Spell:{11,12,13})
        {
            auto I=Check(VM,S,P);TestEqual(TEXT("Integrated run casts creature/life/item buffs before hunting"),I->GetNumberField(TEXT("spell")),double(Spell));
            auto Buff=ParseUCM(TEXT(R"({"target":1,"power":100,"expires":1000})"));Buff->SetNumberField(TEXT("category"),Spell==11?1:Spell==12?115:154);Confirmed.Add(MakeShared<FJsonValueObject>(Buff));S->SetArrayField(TEXT("item_buffs"),Confirmed);
            S->SetNumberField(TEXT("action_serial"),++Serial);S->SetNumberField(TEXT("last_spell"),Spell);S->SetNumberField(TEXT("time"),100+Serial*4);
        }
        TestEqual(TEXT("Integrated run follows route past an occluded monster"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("move")));
        S->GetArrayField(TEXT("targets"))[0]->AsObject()->SetBoolField(TEXT("line_of_sight"),true);
        TestEqual(TEXT("Integrated run chooses damage by resistance, not spell tier alone"),Check(VM,S,P)->GetNumberField(TEXT("spell")),201.);
        S->SetNumberField(TEXT("action_serial"),++Serial);S->SetNumberField(TEXT("last_spell"),201);S->SetArrayField(TEXT("targets"),{});
        Array(S,TEXT("corpses"),TEXT(R"({"values":[{"id":700,"name":"Corpse","distance":5,"line_of_sight":true}]})"));
        TestEqual(TEXT("Death cancels combat before looting"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
        TestEqual(TEXT("Hunt opens the resulting corpse"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        S->SetNumberField(TEXT("container"),700);S->SetBoolField(TEXT("container_is_corpse"),true);S->SetBoolField(TEXT("contents_ready"),true);
        Array(S,TEXT("contents"),TEXT(R"({"values":[{"id":701,"wcid":701,"name":"Copper Ring","count":1,"identified":false}]})"));
        TestEqual(TEXT("Appraises loot before applying workmanship rule"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("identify")));
        auto Ring=S->GetArrayField(TEXT("contents"))[0]->AsObject();Ring->SetBoolField(TEXT("identified"),true);Ring->SetNumberField(TEXT("workmanship"),8);Ring->SetNumberField(TEXT("material"),59);
        TestEqual(TEXT("Matching appraised item is looted"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Ring));Items.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":9,"wcid":9,"name":"Ust","count":1,"object_class":40})"))));S->SetArrayField(TEXT("inventory"),Items);
        S->SetNumberField(TEXT("container"),0);S->SetArrayField(TEXT("contents"),{});
        TestEqual(TEXT("Auto-closed corpse preserves the queued salvage action"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("salvage")));
        Items.RemoveAt(1);S->SetArrayField(TEXT("inventory"),Items);
        TestEqual(TEXT("Completed hunt resumes its original route without reopening corpse"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("buffing"),true);
        Array(S,TEXT("usable_skills"),TEXT(R"({"values":[24]})"));
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":77,"category":77,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true}]})"));
        TestFalse(TEXT("Innate usable Run skill is not automatically buffed without training"),Check(VM,S,P)->HasField(TEXT("spell")));
        Array(P,TEXT("buffs"),TEXT(R"({"values":[77]})"));
        TestEqual(TEXT("An explicit Run buff overrides the automatic training filter"),Check(VM,S,P)->GetNumberField(TEXT("spell")),77.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("buffing"),true);P->SetBoolField(TEXT("auto_buffs"),false);P->SetBoolField(TEXT("buff_others"),true);
        S->SetObjectField(TEXT("buff_request"),ParseUCM(TEXT(R"({"id":1,"player":2,"name":"Visitor","role":"mage","present":true,"distance":5,"expires":1000})")));
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":11,"name":"Strength Other","category":1,"power":100,"skill":300,"school":4,"duration":300,"beneficial":true}]})"));
        for(int Attempt=0;Attempt<3;++Attempt)
        {
            S->SetNumberField(TEXT("time"),100+Attempt*16);
            TestEqual(TEXT("Missing acknowledgement retries the requested buff with a bound"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("cast")));
            TestFalse(TEXT("A retry waits for acknowledgement instead of flooding casts"),Check(VM,S,P)->HasField(TEXT("action")));
        }
        S->SetNumberField(TEXT("time"),148);auto I=Check(VM,S,P);
        TestEqual(TEXT("Three missing acknowledgements release the request queue"),I->GetStringField(TEXT("action")),FString(TEXT("buff_request_done")));
        TestTrue(TEXT("Missing acknowledgements never count as a successful buff"),I->GetStringField(TEXT("status")).Contains(TEXT("0 buffs confirmed")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("navigation"),true);P->SetNumberField(TEXT("waypoint_radius"),.2);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":26,"y":97,"z":12}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Host receives the profile's tight arrival tolerance"),I->GetNumberField(TEXT("arrival_radius")),.2);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("navigation"),true);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":97,"z":12,"kind":"command","command":{"op":"forcebuff","value":true}}]})"));
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":11,"category":1,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true}]})"));
        Array(S,TEXT("enchantments"),TEXT(R"({"values":[{"category":1,"power":100,"remaining":1000}]})"));
        Check(VM,S,P);auto I=Check(VM,S,P);
        TestEqual(TEXT("Imported forcebuff commands also ignore fresh timers"),I->GetNumberField(TEXT("spell")),11.);
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),11);I=Check(VM,S,P);
        TestEqual(TEXT("Imported forced cycle terminates after confirmation"),I->GetStringField(TEXT("status")),FString(TEXT("Force Buff complete")));
        I=Check(VM,S,P);TestFalse(TEXT("Imported force command does not loop forever"),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":11,"name":"Strength Self","category":1,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true},{"id":12,"name":"Armor Self","category":115,"power":100,"skill":300,"school":2,"duration":300,"self_buff":true},{"id":13,"name":"Blood Drinker","category":154,"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":1},{"id":14,"category":3,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true}]})"));
        Array(P,TEXT("excluded_buffs"),TEXT(R"({"values":[3]})"));
        Array(S,TEXT("enchantments"),TEXT(R"({"values":[{"category":1,"power":100,"remaining":1000},{"category":115,"power":100,"remaining":1000}]})"));
        Array(S,TEXT("item_buffs"),TEXT(R"({"values":[{"target":1,"category":154,"power":100,"expires":2000}]})"));
        S->SetNumberField(TEXT("force_buff_request"),1);int Serial=0;
        for(int Id:{11,12,13})
        {
            auto I=Check(VM,S,P);TestEqual(TEXT("Forced cycle refreshes creature, life and item buffs despite fresh timers and Buff off"),I->GetNumberField(TEXT("spell")),double(Id));
            TestEqual(TEXT("Forced item buffs also target the player once"),I->GetNumberField(TEXT("target")),1.);
            I=Check(VM,S,P);TestFalse(TEXT("Old timers cannot masquerade as forced-cast completion"),I->HasField(TEXT("action")));
            S->SetNumberField(TEXT("action_serial"),++Serial);S->SetNumberField(TEXT("last_spell"),Id);
        }
        auto I=Check(VM,S,P);TestEqual(TEXT("Cycle completes once each eligible family is confirmed"),I->GetStringField(TEXT("action")),FString(TEXT("force_buff_done")));
        TestEqual(TEXT("Completion identifies the request"),I->GetNumberField(TEXT("request")),1.);
        S->SetNumberField(TEXT("force_buff_request"),0);I=Check(VM,S,P);TestFalse(TEXT("Returns to normal Buff-off behavior"),I->HasField(TEXT("action")));
        S->SetNumberField(TEXT("force_buff_request"),2);I=Check(VM,S,P);TestEqual(TEXT("A later manual refresh starts a new cycle"),I->GetNumberField(TEXT("spell")),11.);
        S->SetNumberField(TEXT("action_serial"),++Serial);S->SetNumberField(TEXT("last_spell"),11);S->SetNumberField(TEXT("action_error"),1);
        I=Check(VM,S,P);TestEqual(TEXT("Failed cast retries the same family rather than counting it refreshed"),I->GetNumberField(TEXT("spell")),11.);
        S->SetNumberField(TEXT("time"),113);I=Check(VM,S,P);TestEqual(TEXT("Missing retry reply retains the confirmation watchdog"),I->GetNumberField(TEXT("spell")),11.);
        S->SetNumberField(TEXT("force_buff_request"),0);I=Check(VM,S,P);TestFalse(TEXT("Cancel does not start another buff family"),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetStringField(TEXT("combat"),TEXT("missile"));P->SetBoolField(TEXT("navigation"),true);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":105,"z":12}]})"));
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":50,"name":"Thrown weapon","type":256,"identified":true,"can_wield":true,"equipped":true,"damage":20}]})"));
        Array(S,TEXT("targets"),TEXT(R"({"values":[{"id":500,"name":"Wasp","distance":20,"identified":true,"line_of_sight":false,"cell":2103705613,"x":45,"y":97,"z":12}]})"));
        auto Target=S->GetArrayField(TEXT("targets"))[0]->AsObject();
        auto I=Check(VM,S,P);TestEqual(TEXT("Blocked monster yields to recorded route"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
        TestEqual(TEXT("Route follows its corner instead of chasing through wall"),I->GetNumberField(TEXT("y")),105.);
        Array(S,TEXT("route_objects"),TEXT(R"({"values":[{"id":77,"object_class":26,"door_open":false,"distance":1,"cell":2103705613,"x":25,"y":98,"z":12}]})"));
        I=Check(VM,S,P);TestEqual(TEXT("Closed door along the route is opened instead of shot through"),I->GetStringField(TEXT("action")),FString(TEXT("use_world")));
        Target->SetBoolField(TEXT("line_of_sight"),true);
        I=Check(VM,S,P);TestEqual(TEXT("Combat resumes when target becomes unobstructed"),I->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Target->SetBoolField(TEXT("line_of_sight"),false);
        I=Check(VM,S,P);TestEqual(TEXT("Losing sight cancels any repeating attack"),I->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
        I=Check(VM,S,P);TestEqual(TEXT("Then continues walking the route"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
        Target->SetBoolField(TEXT("line_of_sight"),true);P->SetNumberField(TEXT("radius"),10);
        I=Check(VM,S,P);TestEqual(TEXT("Global attack range stops premature firing"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
        Array(P,TEXT("monsters"),TEXT(R"({"values":[{"name":"Wasp","range":25}]})"));
        I=Check(VM,S,P);TestEqual(TEXT("Per-monster attack range overrides global range"),I->GetStringField(TEXT("action")),FString(TEXT("attack")));
        P->GetArrayField(TEXT("monsters"))[0]->AsObject()->SetNumberField(TEXT("range"),0);
        I=Check(VM,S,P);TestEqual(TEXT("Zero override uses global range"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("navigation"),true);P->SetBoolField(TEXT("nav_priority"),true);P->SetBoolField(TEXT("looting"),true);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":105,"z":12}]})"));
        Array(P,TEXT("loot_rules"),TEXT(R"({"values":[{"name":"Copper","action":"keep"}]})"));
        Array(S,TEXT("corpses"),TEXT(R"({"values":[{"id":700,"name":"Corpse","distance":8,"line_of_sight":false}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Corpse behind wall does not interrupt route"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
        S->GetArrayField(TEXT("corpses"))[0]->AsObject()->SetBoolField(TEXT("line_of_sight"),true);
        I=Check(VM,S,P);TestEqual(TEXT("Navigation priority does not starve nearby loot"),I->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        S->SetNumberField(TEXT("time"),103);I=Check(VM,S,P);TestFalse(TEXT("Route cannot cancel corpse approach"),I->HasField(TEXT("action")));
        S->SetNumberField(TEXT("container"),700);S->SetBoolField(TEXT("container_is_corpse"),true);S->SetBoolField(TEXT("contents_ready"),false);
        I=Check(VM,S,P);TestFalse(TEXT("Wait for server contents instead of closing early"),I->HasField(TEXT("action")));
        S->SetBoolField(TEXT("contents_ready"),true);
        Array(S,TEXT("contents"),TEXT(R"({"values":[{"id":701,"wcid":701,"name":"Copper Ring","count":1,"identified":true}]})"));
        I=Check(VM,S,P);TestEqual(TEXT("Matching drop is collected"),I->GetStringField(TEXT("action")),FString(TEXT("loot")));
        I=Check(VM,S,P);TestFalse(TEXT("Route waits for loot acknowledgement"),I->HasField(TEXT("action")));
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":701,"wcid":701,"name":"Copper Ring","count":1}]})"));
        S->SetArrayField(TEXT("contents"),{});I=Check(VM,S,P);
        TestEqual(TEXT("Acknowledged loot completes corpse"),I->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
        S->SetNumberField(TEXT("container"),0);I=Check(VM,S,P);
        TestEqual(TEXT("Route resumes without reopening completed corpse"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("buffing"),true);
        TArray<TSharedPtr<FJsonValue>> Spells,Selected,Excluded;
        for(int Id=1;Id<=2200;++Id)
        {
            auto Spell=ParseUCM(TEXT(R"({"category":1,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true})"));
            Spell->SetNumberField(TEXT("id"),Id);Spells.Add(MakeShared<FJsonValueObject>(Spell));
            Selected.Add(MakeShared<FJsonValueNumber>(Id));Excluded.Add(MakeShared<FJsonValueNumber>(Id+10000));
        }
        S->SetArrayField(TEXT("spells"),Spells);P->SetArrayField(TEXT("buffs"),Selected);P->SetArrayField(TEXT("excluded_buffs"),Excluded);
        auto I=Check(VM,S,P);TestTrue(TEXT("Large spellbook and exception lists stay within unchanged instruction budget"),I->HasField(TEXT("spell")));
        Array(S,TEXT("enchantments"),TEXT(R"({"values":[{"category":1,"power":100,"remaining":1000}]})"));
        I=Check(VM,S,P);TestFalse(TEXT("Next snapshot sees new enchantment; no stale buff plan"),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        S->SetNumberField(TEXT("force_buff_request"),1);
        TArray<TSharedPtr<FJsonValue>> Spells;
        for(int Id=1;Id<=5000;++Id)
        {
            auto Spell=ParseUCM(TEXT(R"({"category":1,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true,"known":true,"components_known":true,"scarabs":{"Scarab":1}})"));
            Spell->SetNumberField(TEXT("id"),Id);
            Spell->SetNumberField(TEXT("category"),700+(Id-1)%100);
            Spell->SetNumberField(TEXT("power"),100+((Id-1)/100)%8*20);
            Spells.Add(MakeShared<FJsonValueObject>(Spell));
        }
        S->SetArrayField(TEXT("spells"),Spells);
        // Special families are maintained only when explicitly requested.
        TArray<TSharedPtr<FJsonValue>> Selected;
        for(int Id=1;Id<=100;++Id)Selected.Add(MakeShared<FJsonValueNumber>(Id));
        P->SetArrayField(TEXT("buffs"),Selected);
        auto Inventory=S->GetArrayField(TEXT("inventory"));
        Inventory.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":100,"name":"Scarab","count":100})"))));S->SetArrayField(TEXT("inventory"),Inventory);
        auto I=Check(VM,S,P);
        TestTrue(TEXT("Force Buff with a full supplied spellbook stays within the sandbox budget"),I->HasField(TEXT("spell")));
        TSet<int32> Categories;
        for(int Cycle=1;Cycle<=100&&I->HasField(TEXT("spell"));++Cycle)
        {
            const int Id=int(I->GetNumberField(TEXT("spell")));
            const auto Spell=Spells[Id-1]->AsObject();const int Category=int(Spell->GetNumberField(TEXT("category")));
            TestFalse(TEXT("Manual refresh does not repeat a completed family"),Categories.Contains(Category));Categories.Add(Category);
            TestEqual(TEXT("Strongest supplied tier is selected"),Spell->GetNumberField(TEXT("power")),240.);
            S->SetNumberField(TEXT("action_serial"),Cycle);S->SetNumberField(TEXT("last_spell"),Id);S->SetNumberField(TEXT("time"),100+Cycle*3);
            I=Check(VM,S,P);
        }
        TestEqual(TEXT("Large-book refresh completes every family"),Categories.Num(),100);
        TestEqual(TEXT("Large-book refresh reports completion"),I->HasField(TEXT("action"))?I->GetStringField(TEXT("action")):FString(),FString(TEXT("force_buff_done")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("buffing"),true);
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":10,"name":"Item mastery","category":45,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true},{"id":11,"name":"Strength Self","category":1,"power":100,"skill":300,"school":4,"duration":300,"self_buff":true}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Untrained skill family excluded from automatic buffs"),I->GetNumberField(TEXT("spell")),11.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("buffing"),true);
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":12,"name":"Blood Drinker","category":154,"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":1}]})"));
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":55,"wcid":55,"name":"Sword","type":1,"equipped":false,"count":1})"))));S->SetArrayField(TEXT("inventory"),Items);
        P->SetArrayField(TEXT("weapons"),{MakeShared<FJsonValueNumber>(55)});
        auto I=Check(VM,S,P);TestEqual(TEXT("Weapon aura targets the player even with an explicit weapon pool"),I->GetNumberField(TEXT("target")),1.);
        Array(S,TEXT("item_buffs"),TEXT(R"({"values":[{"target":1,"category":154,"power":100,"expires":500}]})"));
        I=Check(VM,S,P);TestFalse(TEXT("Confirmed item buff suppresses recast"),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("buffing"),true);
        auto Items=S->GetArrayField(TEXT("inventory"));
        for(int Id=100;Id<300;++Id)
        {
            auto Item=ParseUCM(TEXT(R"({"type":2,"equipped":true,"count":1})"));Item->SetNumberField(TEXT("id"),Id);
            Items.Add(MakeShared<FJsonValueObject>(Item));
        }
        S->SetArrayField(TEXT("inventory"),Items);
        TArray<TSharedPtr<FJsonValue>> Spells,Confirmed;
        for(int Category:{160,162,164,166,168,170,172,174,152,154,156,158,195,695})
        {
            auto Spell=ParseUCM(TEXT(R"({"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":7})"));
            Spell->SetNumberField(TEXT("id"),Category);Spell->SetNumberField(TEXT("category"),Category);
            Spells.Add(MakeShared<FJsonValueObject>(Spell));
        }
        S->SetArrayField(TEXT("spells"),Spells);
        for(const auto& Spell:Spells)
        {
            auto I=Check(VM,S,P);
            TestEqual(TEXT("Each item buff family casts exactly once on the player"),I->GetNumberField(TEXT("target")),1.);
            TestEqual(TEXT("Confirmation advances to the next family rather than another item"),I->GetNumberField(TEXT("spell")),Spell->AsObject()->GetNumberField(TEXT("id")));
            auto E=ParseUCM(TEXT(R"({"target":1,"power":100,"expires":500})"));
            E->SetNumberField(TEXT("category"),Spell->AsObject()->GetNumberField(TEXT("category")));Confirmed.Add(MakeShared<FJsonValueObject>(E));
            S->SetArrayField(TEXT("item_buffs"),Confirmed);
        }
        auto I=Check(VM,S,P);TestFalse(TEXT("Full item-buff cycle completes despite hundreds of inventory targets"),I->HasField(TEXT("action")));
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":700,"category":192,"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":640}]})"));
        I=Check(VM,S,P);TestFalse(TEXT("Lock spells are not redirected to the player"),I->HasField(TEXT("action")));
    }
    for(int Vital:{2,4,6})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("recovery"),true);
        const TCHAR* Key=Vital==2?TEXT("health"):Vital==4?TEXT("stamina"):TEXT("mana");S->SetNumberField(Key,20);
        auto Item=ParseUCM(TEXT(R"({"id":42,"wcid":42,"name":"Supply","type":32,"usable":true,"identified":true,"boost":100,"count":10})"));Item->SetNumberField(TEXT("boost_vital"),Vital);
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Item));S->SetArrayField(TEXT("inventory"),Items);
        auto I=Check(VM,S,P);TestEqual(TEXT("Recovery uses supply for correct vital"),I->GetStringField(TEXT("action")),FString(TEXT("use_item")));
        S->SetNumberField(TEXT("time"),102);I=Check(VM,S,P);TestFalse(TEXT("Recovery waits for actual vital update"),I->HasField(TEXT("action")));
        S->SetNumberField(Key,100);S->SetNumberField(TEXT("action_serial"),1);I=Check(VM,S,P);TestFalse(TEXT("Recovered vital needs no more supply"),I->HasField(TEXT("action")));
    }
    for(bool Trained:{false,true})for(int Uses:{0,10})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("recovery"),true);S->SetNumberField(TEXT("health"),30);
        if(Trained)Array(S,TEXT("trained_skills"),TEXT(R"({"values":[21]})"));
        auto Kit=ParseUCM(TEXT(R"({"id":42,"wcid":42,"name":"Kit","healing_kit":true,"usable":true,"identified":true,"boost_vital":2,"boost":100,"count":1})"));Kit->SetNumberField(TEXT("structure"),Uses);
        auto Potion=ParseUCM(TEXT(R"({"id":43,"wcid":43,"name":"Potion","usable":true,"identified":true,"boost_vital":2,"boost":50,"count":1})"));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Kit),MakeShared<FJsonValueObject>(Potion)});
        TestEqual(TEXT("Recovery skips untrained or exhausted healing kits and uses a viable potion"),Check(VM,S,P)->GetNumberField(TEXT("item")),Trained&&Uses>0?42.:43.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("recovery"),true);S->SetNumberField(TEXT("mana"),20);
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":20,"name":"Stamina to Mana VI","category":91,"power":200,"skill":300,"school":2,"caster_target":true,"duration":0}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Mana recovery can use stamina conversion"),I->GetNumberField(TEXT("spell")),20.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("buffing"),true);
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetBoolField(TEXT("equipped"),false);
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":11,"category":1,"power":100,"skill":300,"self_buff":true}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Casting equipment is equipped before buff request"),I->GetStringField(TEXT("action")),FString(TEXT("equip")));
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetBoolField(TEXT("equipped"),true);I=Check(VM,S,P);
        TestEqual(TEXT("Cast begins after server equipment confirmation"),I->GetStringField(TEXT("action")),FString(TEXT("cast")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("combat"),TEXT("melee"));
        Array(S,TEXT("targets"),TEXT(R"({"values":[{"id":500,"name":"Target","distance":1,"identified":true,"resists":{"8":0.1,"32":1.5}}]})"));
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":50,"wcid":50,"name":"Fire","type":1,"identified":true,"can_wield":true,"damage":100,"damage_type":8},{"id":51,"wcid":51,"name":"Acid","type":1,"identified":true,"can_wield":true,"damage":50,"damage_type":32},{"id":52,"wcid":52,"name":"Unusable","type":1,"identified":true,"can_wield":false,"damage":500,"damage_type":32}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Weakness scoring penalizes resisted weapon and excludes unmet requirements"),I->GetNumberField(TEXT("item")),51.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("looting"),true);S->SetNumberField(TEXT("container"),700);
        Array(P,TEXT("loot_rules"),TEXT(R"({"values":[{"name":"Sword","min_rating":10,"keep_up_to":2}]})"));
        Array(S,TEXT("contents"),TEXT(R"({"values":[{"id":701,"wcid":701,"name":"Sword of fire","type":1,"value":100,"count":5,"identified":false}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Loot rating filter requests missing appraisal"),I->GetStringField(TEXT("action")),FString(TEXT("identify")));
        auto Item=S->GetArrayField(TEXT("contents"))[0]->AsObject();Item->SetBoolField(TEXT("identified"),true);Item->SetNumberField(TEXT("damage_rating"),20);
        I=Check(VM,S,P);TestEqual(TEXT("Keep-up-to takes only needed quantity"),I->GetNumberField(TEXT("amount")),2.);
        S->SetNumberField(TEXT("time"),102);I=Check(VM,S,P);TestFalse(TEXT("Corpse stays open while transfer is pending"),I->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),111);I=Check(VM,S,P);TestEqual(TEXT("Missing loot acknowledgement pauses loot without stopping UCM"),I->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        S->SetNumberField(TEXT("container"),0);S->SetNumberField(TEXT("time"),112);
        Array(S,TEXT("contents"),TEXT(R"({"values":[]})"));
        Array(S,TEXT("corpses"),TEXT(R"({"values":[{"id":700,"name":"Timed out corpse","distance":1,"identified":true,"can_loot":true}]})"));
        I=Check(VM,S,P);TestFalse(TEXT("Unconfirmed corpse is not immediately retried"),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("navigation"),true);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":97,"z":12,"kind":"portal","wcid":123},{"cell":2103771149,"x":5,"y":97,"z":12}]})"));
        Array(S,TEXT("route_objects"),TEXT(R"({"values":[{"id":77,"wcid":123,"name":"Portal","distance":1}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Portal uses normal object interaction"),I->GetStringField(TEXT("action")),FString(TEXT("use_world")));
        S->SetNumberField(TEXT("time"),104);I=Check(VM,S,P);TestFalse(TEXT("Portal is not used repeatedly while transit is pending"),I->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),150);I=Check(VM,S,P);TestEqual(TEXT("Failed portal pauses navigation while UCM stays active"),I->GetStringField(TEXT("action")),FString(TEXT("pause_navigation")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("buffing"),true);
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":10,"category":1,"power":100,"skill":300,"self_buff":true},{"id":11,"category":1,"power":200,"skill":300,"self_buff":true}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Preferred buff starts at highest usable tier"),I->GetNumberField(TEXT("spell")),11.);
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),11);S->SetNumberField(TEXT("action_error"),0x400);S->SetNumberField(TEXT("time"),104);
        I=Check(VM,S,P);TestEqual(TEXT("Server missing-components result falls back to a lower tier"),I->GetNumberField(TEXT("spell")),10.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("combat"),TEXT("missile"));
        Array(S,TEXT("targets"),TEXT(R"({"values":[{"id":500,"name":"Target","distance":20,"identified":true,"resists":{"8":0.1,"32":1.5}}]})"));
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":50,"wcid":50,"name":"Bow","type":256,"identified":true,"can_wield":true,"equipped":true,"ammo_type":1,"damage_mod":2},{"id":51,"wcid":51,"name":"Fire arrow","type":256,"slots":8388608,"ammo_type":1,"identified":true,"can_wield":true,"damage":100,"damage_type":8},{"id":52,"wcid":52,"name":"Acid arrow","type":256,"slots":8388608,"ammo_type":1,"identified":true,"can_wield":true,"damage":50,"damage_type":32}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Launcher chooses compatible ammunition by effective element damage"),I->GetNumberField(TEXT("item")),52.);
        S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetBoolField(TEXT("equipped"),true);I=Check(VM,S,P);
        TestEqual(TEXT("Missile attack waits for ammunition equipment confirmation"),I->GetStringField(TEXT("action")),FString(TEXT("attack")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("navigation"),true);S->SetNumberField(TEXT("teleport_sequence"),4);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":97,"z":12,"kind":"portal","wcid":123},{"cell":2103705613,"x":27,"y":97,"z":12}]})"));
        Array(S,TEXT("route_objects"),TEXT(R"({"values":[{"id":77,"wcid":123,"name":"Portal","distance":1}]})"));
        Check(VM,S,P);S->SetNumberField(TEXT("time"),104);auto I=Check(VM,S,P);
        TestFalse(TEXT("Nearby destination is not mistaken for a completed teleport"),I->HasField(TEXT("action")));
        S->SetNumberField(TEXT("teleport_sequence"),5);Check(VM,S,P);I=Check(VM,S,P);
        TestEqual(TEXT("Server teleport sequence advances the route"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("navigation"),true);
        Array(P,TEXT("route"),TEXT(R"({"values":[{"cell":2103705613,"x":25,"y":97,"z":12,"kind":"jump","heading":90,"charge":0.8},{"cell":2103705613,"x":35,"y":97,"z":12}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Jump uses charge intent"),I->GetStringField(TEXT("action")),FString(TEXT("jump")));
        S->SetBoolField(TEXT("jumping"),true);S->SetNumberField(TEXT("time"),103);I=Check(VM,S,P);TestFalse(TEXT("Jump is not restarted midair"),I->HasField(TEXT("action")));
        S->SetBoolField(TEXT("jumping"),false);S->SetNumberField(TEXT("time"),105);Check(VM,S,P);I=Check(VM,S,P);TestEqual(TEXT("Route resumes after landing"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    for(const FString Role:{TEXT("mage"),TEXT("heavy"),TEXT("missile"),TEXT("finesse"),TEXT("light"),TEXT("unarmed"),TEXT("twohanded"),TEXT("melee")})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("buffing"),true);P->SetBoolField(TEXT("auto_buffs"),false);P->SetBoolField(TEXT("buff_others"),true);
        auto Request=ParseUCM(TEXT(R"({"id":1,"player":1342177290,"name":"Visitor","present":true,"distance":5,"expires":1000,"equipment":[{"id":400,"type":2,"count":1},{"id":401,"type":1,"count":1}]})"));
        Request->SetStringField(TEXT("role"),Role);S->SetObjectField(TEXT("buff_request"),Request);
        Request->SetBoolField(TEXT("started"),false);
        TestEqual(TEXT("A queued request announces its turn before casting"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("buff_request_start")));
        Request->SetBoolField(TEXT("started"),true);
        TArray<TSharedPtr<FJsonValue>> Spells;
        for(int Cat:{1,3,5,7,9,11,37,39,41,43,45,47,49,31,19,17,23,593,668,665,671,677,101,115,160,154})
        {
            auto Spell=ParseUCM(TEXT(R"({"power":100,"skill":300,"beneficial":true,"duration":500,"target_type":16,"school":4})"));
            Spell->SetNumberField(TEXT("id"),Cat+1000);Spell->SetNumberField(TEXT("category"),Cat);
            if(Cat==160||Cat==154){Spell->SetNumberField(TEXT("school"),3);Spell->SetNumberField(TEXT("target_type"),Cat==160?2:1);}
            Spells.Add(MakeShared<FJsonValueObject>(Spell));
        }
        auto Self=ParseUCM(TEXT(R"({"id":999,"category":1,"power":250,"skill":300,"beneficial":true,"duration":500,"caster_target":true,"self_buff":true})"));
        Spells.Add(MakeShared<FJsonValueObject>(Self));S->SetArrayField(TEXT("spells"),Spells);
        TSet<int32> Casts;bool Finished=false;int Serial=0;
        for(int Tick=0;Tick<40;++Tick)
        {
            auto I=Check(VM,S,P);const FString Action=I->GetStringField(TEXT("action"));
            if(Action==TEXT("buff_request_done")){Finished=true;break;}
            if(!TestEqual(TEXT("Other buff sequence casts instead of swapping gear"),Action,FString(TEXT("cast"))))break;
            const int32 Spell=I->GetNumberField(TEXT("spell"));Casts.Add(Spell);
            TestTrue(TEXT("Other buff is never redirected to caster"),I->GetNumberField(TEXT("target"))!=1);
            if(Spell==1160)TestEqual(TEXT("Bane targets requester's server-known armor"),I->GetNumberField(TEXT("target")),400.);
            if(Spell==1154)TestEqual(TEXT("Weapon enchantment targets requester's weapon"),I->GetNumberField(TEXT("target")),401.);
            auto Waiting=Check(VM,S,P);TestFalse(TEXT("Request waits for UseDone before next spell"),Waiting->HasField(TEXT("action")));
            S->SetNumberField(TEXT("action_serial"),++Serial);S->SetNumberField(TEXT("last_spell"),Spell);S->SetNumberField(TEXT("time"),105+Tick*5);
        }
        TestTrue(TEXT("Requested role finishes with confirmed casts"),Finished);
        for(int Cat:{1,3,5,7,9,11,37,39,41,101,115,160,154})TestTrue(*FString::Printf(TEXT("%s includes shared buff family %d"),*Role,Cat),Casts.Contains(1000+Cat));
        const int Skill=Role==TEXT("mage")?49:Role==TEXT("heavy")||Role==TEXT("melee")?31:Role==TEXT("missile")?19:Role==TEXT("finesse")?23:Role==TEXT("twohanded")?593:17;
        TestTrue(TEXT("Role-specific attack skill included"),Casts.Contains(1000+Skill));
        if(Role!=TEXT("mage")&&Role!=TEXT("missile"))for(int Cat:{668,665,671,677})
            TestTrue(*FString::Printf(TEXT("%s includes melee support family %d"),*Role,Cat),Casts.Contains(1000+Cat));
        if(Role==TEXT("melee"))for(int Cat:{17,23,31,593})TestTrue(TEXT("General melee includes every weapon skill"),Casts.Contains(1000+Cat));
        TestFalse(TEXT("Stronger Self spell is not used for Other request"),Casts.Contains(999));
        TestFalse(TEXT("Unrelated attack skill is excluded"),Casts.Contains(Role==TEXT("missile")?1031:1019));
        P->SetBoolField(TEXT("buffing"),false);auto I=Check(VM,S,P);TestEqual(TEXT("Disabling buffs cancels queued request"),I->GetStringField(TEXT("action")),FString(TEXT("buff_request_done")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("combat"),TEXT("missile"));P->SetNumberField(TEXT("damage_type"),16);
        Array(S,TEXT("targets"),TEXT(R"({"values":[{"id":500,"name":"Monster","distance":20,"identified":true}]})"));
        Array(S,TEXT("inventory"),TEXT(R"({"values":[{"id":50,"wcid":50,"name":"Bow","type":256,"identified":true,"can_wield":true,"equipped":true,"ammo_type":1,"damage_type":2,"damage_mod":2},{"id":51,"wcid":51,"name":"Fire arrows","type":256,"slots":8388608,"ammo_type":1,"identified":false,"count":50}]})"));
        TestEqual(TEXT("Unappraised compatible ammo is inspected before declaring loadout unavailable"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("identify")));
        auto Ammo=S->GetArrayField(TEXT("inventory"))[1]->AsObject();Ammo->SetBoolField(TEXT("identified"),true);Ammo->SetBoolField(TEXT("can_wield"),true);Ammo->SetNumberField(TEXT("damage"),30);Ammo->SetNumberField(TEXT("damage_type"),16);
        auto I=Check(VM,S,P);TestEqual(TEXT("A piercing bow can select a requested elemental arrow loadout"),I->GetStringField(TEXT("action")),FString(TEXT("equip")));TestEqual(TEXT("Equips requested fire ammunition"),I->GetNumberField(TEXT("item")),51.);
        Ammo->SetBoolField(TEXT("equipped"),true);TestEqual(TEXT("Correct loadout attacks after ammo confirmation"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Ammo->SetNumberField(TEXT("damage_type"),8);TestEqual(TEXT("Missing requested ammunition does not fire the previous loadout"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("buffing"),true);P->SetBoolField(TEXT("auto_buffs"),false);P->SetBoolField(TEXT("buff_others"),true);
        S->SetObjectField(TEXT("buff_request"),ParseUCM(TEXT(R"({"id":1,"player":1342177290,"name":"Visitor","role":"heavy","present":true,"distance":5,"equipment":[{"id":55,"type":1,"count":1}]})")));
        Array(S,TEXT("spells"),TEXT(R"({"values":[{"id":10,"category":154,"power":100,"skill":300,"beneficial":true,"duration":500,"school":3,"target_type":1},{"id":11,"category":154,"power":200,"skill":300,"beneficial":true,"duration":500,"school":3,"target_type":1}]})"));
        auto I=Check(VM,S,P);TestEqual(TEXT("Other item buff selects strongest tier"),I->GetNumberField(TEXT("spell")),11.);
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),11);S->SetNumberField(TEXT("action_error"),0x400);S->SetNumberField(TEXT("time"),104);
        I=Check(VM,S,P);TestEqual(TEXT("Missing components on Other item buff falls back without stopping self buffs"),I->GetNumberField(TEXT("spell")),10.);
        S->SetNumberField(TEXT("action_serial"),2);S->SetNumberField(TEXT("last_spell"),10);S->SetNumberField(TEXT("action_error"),0);S->SetNumberField(TEXT("time"),108);
        I=Check(VM,S,P);TestEqual(TEXT("Request completes after lower tier confirmed"),I->GetStringField(TEXT("action")),FString(TEXT("buff_request_done")));
        S->GetObjectField(TEXT("buff_request"))->SetBoolField(TEXT("present"),false);
        I=Check(VM,S,P);TestEqual(TEXT("Requester leaving awareness is cancelled immediately"),I->GetStringField(TEXT("action")),FString(TEXT("buff_request_done")));
    }
    for(bool Present:{false,true})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("buffing"),true);P->SetBoolField(TEXT("auto_buffs"),false);P->SetBoolField(TEXT("buff_others"),true);P->SetNumberField(TEXT("buff_other_range"),12);
        auto Request=ParseUCM(TEXT(R"({"id":1,"player":2,"name":"Visitor","role":"mage","present":true,"distance":12.1,"started":false})"));
        Request->SetBoolField(TEXT("present"),Present);S->SetObjectField(TEXT("buff_request"),Request);
        auto I=Check(VM,S,P);
        TestEqual(TEXT("Unavailable queue head is removed without a turn notification or grace delay"),I->GetStringField(TEXT("action")),FString(TEXT("buff_request_done")));
        TestEqual(TEXT("Cancellation addresses the unavailable request"),I->GetNumberField(TEXT("request")),1.);
        // The host pops the completed head; the same VM must accept the next
        // nearby player without inheriting a wait or pending spell.
        Request->SetNumberField(TEXT("id"),2);Request->SetNumberField(TEXT("player"),3);Request->SetBoolField(TEXT("present"),true);Request->SetNumberField(TEXT("distance"),12);
        I=Check(VM,S,P);TestEqual(TEXT("Next requester at the configured range boundary starts immediately"),I->GetStringField(TEXT("action")),FString(TEXT("buff_request_start")));
        TestEqual(TEXT("Turn belongs to next queued request"),I->GetNumberField(TEXT("request")),2.);
        Request->SetBoolField(TEXT("started"),true);Request->SetNumberField(TEXT("distance"),13);
        TestEqual(TEXT("Leaving range during a turn also releases the queue immediately"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("buff_request_done")));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMInventorySalvageTest,"ACE.Plugins.InventorySalvage",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMInventorySalvageTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Profile=[](){return ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","salvage_inventory":true,"loot_rules":[{"action":"keep","name":"Keepsake","name_mode":"exact"},{"action":"salvage","material":59,"min_workmanship":7}]})"));};
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"inventory_revision":1,"appraisal_revision":1,"inventory":[{"id":9,"name":"Ust","object_class":40,"resource_available":true},{"id":20,"wcid":200,"name":"Copper Ring","material":59,"workmanship":7.5,"count":1,"identified":true,"container":99,"resource_available":true}]})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    for(int Protection=0;Protection<9;++Protection)
    {
        FACEPluginVM VM;TestTrue(TEXT("Inventory salvage policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();auto Item=S->GetArrayField(TEXT("inventory"))[1]->AsObject();
        if(Protection==0)P->RemoveField(TEXT("salvage_inventory"));
        if(Protection==1)Item->SetStringField(TEXT("name"),TEXT("Keepsake"));
        if(Protection==2)Item->SetBoolField(TEXT("equipped"),true);
        if(Protection==3)Item->SetBoolField(TEXT("retained"),true);
        if(Protection==4)Item->SetBoolField(TEXT("resource_available"),false);
        if(Protection==5)Item->SetObjectField(TEXT("int_properties"),ParseUCM(TEXT(R"({"171":1})")));
        if(Protection==6)Item->SetObjectField(TEXT("string_properties"),ParseUCM(TEXT(R"({"8":"Owner"})")));
        if(Protection==7)Item->SetNumberField(TEXT("object_class"),39);
        if(Protection==8)Item->RemoveField(TEXT("workmanship"));
        for(int Tick=0;Tick<2;++Tick)TestFalse(TEXT("Existing possessions remain protected without opt-in or when excluded"),Step(VM,S,P)->HasField(TEXT("action")));
    }
    for(bool Available:{true,false})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Tool=S->GetArrayField(TEXT("inventory"))[0]->AsObject();
        Tool->SetBoolField(TEXT("resource_available"),false);Tool->SetBoolField(TEXT("retained"),true);
        Tool->SetNumberField(TEXT("object_class"),0);Tool->SetBoolField(TEXT("salvage_tool_available"),Available);
        Step(VM,S,P);auto I=Step(VM,S,P);
        TestEqual(TEXT("UCM uses explicit native Ust eligibility instead of retention/category"),I->GetStringField(TEXT("action")),FString(Available?TEXT("salvage"):TEXT("activity_failed")));
        if(Available)TestEqual(TEXT("Automatically detected retained tool is selected"),I->GetNumberField(TEXT("tool")),9.);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Item=S->GetArrayField(TEXT("inventory"))[1]->AsObject();
        Item->SetBoolField(TEXT("identified"),false);
        TestEqual(TEXT("Existing item is appraised for tinkering/inscription before destruction"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("identify")));
        Item->SetBoolField(TEXT("identified"),true);S->SetNumberField(TEXT("appraisal_revision"),2);
        auto Queued=Step(VM,S,P);
        TestTrue(TEXT("Matching item in a side pack is queued"),Queued->GetStringField(TEXT("status")).StartsWith(TEXT("Queued inventory salvage:")));
        TestTrue(TEXT("Queued inventory salvage uses a continuation instead of an idle poll"),Queued->GetBoolField(TEXT("continue_work")));
        auto I=Step(VM,S,P);TestEqual(TEXT("Existing inventory uses normal salvage action"),I->GetStringField(TEXT("action")),FString(TEXT("salvage")));TestEqual(TEXT("Correct side-pack item is salvaged"),I->GetNumberField(TEXT("item")),20.);
        TestFalse(TEXT("Pending salvage is not resubmitted"),Step(VM,S,P)->HasField(TEXT("action")));
        auto Items=S->GetArrayField(TEXT("inventory"));Items.RemoveAt(1);S->SetArrayField(TEXT("inventory"),Items);S->SetNumberField(TEXT("inventory_revision"),2);
        TestFalse(TEXT("Server removal completes salvage"),Step(VM,S,P)->HasField(TEXT("action")));
        Item->SetNumberField(TEXT("id"),21);Items.Add(MakeShared<FJsonValueObject>(Item));S->SetArrayField(TEXT("inventory"),Items);S->SetNumberField(TEXT("inventory_revision"),3);
        TestTrue(TEXT("Inventory changes scan new items"),Step(VM,S,P)->GetStringField(TEXT("status")).StartsWith(TEXT("Queued inventory salvage:")));
        Item->SetBoolField(TEXT("retained"),true);
        auto Protected=Step(VM,S,P);
        TestFalse(TEXT("New retention is rechecked before the queued action"),Protected->HasField(TEXT("action")));
        TestTrue(TEXT("Skipping a protected queued item promptly continues other work"),Protected->GetBoolField(TEXT("continue_work")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();TArray<TSharedPtr<FJsonValue>> Rules;
        for(int Index=0;Index<1500;++Index)Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"skip","material":60})"))));
        Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"salvage","material":59})"))));P->SetArrayField(TEXT("loot_rules"),Rules);
        bool Queued=false;for(int Tick=0;Tick<128&&!Queued;++Tick)Queued=Step(VM,S,P)->GetStringField(TEXT("status")).StartsWith(TEXT("Queued inventory salvage:"));
        TestTrue(TEXT("Large imported inventory rules finish within the unchanged sandbox budget"),Queued);
    }
    for(int Mode=0;Mode<4;++Mode)
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("salvage_inventory"),false);P->SetBoolField(TEXT("salvage_combine"),true);
        S->SetArrayField(TEXT("inventory"),ParseUCM(TEXT(R"({"v":[{"id":9,"object_class":40},{"id":30,"object_class":39,"material":59,"structure":60,"workmanship":6.9,"value":100},{"id":31,"object_class":39,"material":59,"structure":60,"workmanship":6,"value":100},{"id":32,"object_class":39,"material":59,"structure":20,"workmanship":7,"value":100},{"id":33,"object_class":39,"material":61,"structure":20,"workmanship":6,"value":100},{"id":34,"object_class":39,"material":59,"structure":100,"workmanship":6,"value":100},{"id":35,"object_class":39,"material":59,"structure":20,"workmanship":6,"value":100,"retained":true},{"id":36,"object_class":39,"material":59,"structure":20,"workmanship":6,"value":100,"resource_available":false}]})"))->GetArrayField(TEXT("v")));
        auto Tool=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Tool->SetBoolField(TEXT("resource_available"),false);Tool->SetBoolField(TEXT("salvage_tool_available"),true);
        if(Mode>0)P->SetObjectField(TEXT("salvage_policy"),ParseUCM(TEXT(R"({"default":[{"min":1,"max":6},{"min":7,"max":8},{"min":9,"max":9},{"min":10,"max":10}],"values":{"59":10000}})")));
        if(Mode==2)P->GetObjectField(TEXT("salvage_policy"))->GetObjectField(TEXT("values"))->SetNumberField(TEXT("59"),0);
        if(Mode==3)P->GetObjectField(TEXT("salvage_policy"))->SetObjectField(TEXT("materials"),ParseUCM(TEXT(R"({"59":[{"min":1,"max":6},{"min":6.5,"max":10}]})")));
        auto I=Step(VM,S,P);
        if(Mode==1)TestFalse(TEXT("Value target prevents producing low-value full bags"),I->HasField(TEXT("action")));
        else
        {
            TestEqual(TEXT("Eligible bags combine"),I->GetStringField(TEXT("action")),FString(TEXT("combine_salvage")));
            TestEqual(TEXT("Classic fractional ranges, material overrides, protected/full bags and zero value mode"),I->GetStringField(TEXT("items")),Mode==3?FString(TEXT("32,30")):FString(TEXT("31,30")));
        }
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginEquipmentTest,"ACE.Plugins.EquipmentBuffCycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginEquipmentTest::RunTest(const FString&)
{
    auto* GI=NewObject<UGameInstance>();GI->Init();
    ON_SCOPE_EXIT {GI->Shutdown();};
    auto* C=GI->GetSubsystem<UACEClientSubsystem>();
    auto* H=GI->GetSubsystem<UACEPluginSubsystem>();
    C->Session=MakeShared<FACESession>();auto& Session=*C->Session;
    Session.State=EACESessionState::InWorld;Session.PlayerGuid=0x50000001;
    auto Add=[&](int32 Id,int32 Type,int64 Slots,bool Equipped)
    {
        FACEWorldObject O;O.Guid=Id;O.Name=FString::Printf(TEXT("Item %u"),uint32(Id));
        O.ItemType=Type;O.ValidLocations=Slots;O.StackSize=1;
        O.ContainerId=Equipped?0:Session.PlayerGuid;
        O.WielderId=Equipped?Session.PlayerGuid:0;O.CurrentWieldedLocation=Equipped?Slots:0;
        Session.WorldObjects.Add(Id,O);
        FACEAppraisalInfo A;A.ObjectGuid=Id;A.bSuccess=true;H->ObserveAppraisal(A);
    };
    const int32 WandA=int32(0x80000E82u),WandB=int32(0x80000A68u),Armor=int32(0x80001000u);
    Add(WandA,ACEItemType::Caster,ACEEquipMask::Held,false);
    Add(WandB,ACEItemType::Caster,ACEEquipMask::Held,false);
    Add(Armor,ACEItemType::Armor,ACEEquipMask::ChestWear,true);
    Session.WorldObjects[Armor].SalvageWorkmanship=7.25f;
    auto Foreign=Session.WorldObjects[Armor];Foreign.Guid=700;Foreign.WielderId=0x50000002;
    Session.WorldObjects.Add(Foreign.Guid,Foreign);
    auto Ground=Session.WorldObjects[WandA];Ground.Guid=701;Ground.ContainerId=0;
    Session.WorldObjects.Add(Ground.Guid,Ground);
    TestFalse(TEXT("Another player's equipment is not plugin-owned"),H->IsOwnedPluginItem(Foreign));
    TestFalse(TEXT("Ground items are not plugin-owned"),H->IsOwnedPluginItem(Ground));
    TestFalse(TEXT("Pack-only drag/merge ownership still excludes worn armor"),C->IsOwnedInventoryItem(Session.WorldObjects[Armor]));
    auto Attached=Session.WorldObjects[Armor];Attached.WielderId=0;Attached.ParentGuid=Session.PlayerGuid;
    TestTrue(TEXT("Acknowledged equipment with a player parent is plugin-owned"),H->IsOwnedPluginItem(Attached));
    auto S=ParseUCM(TEXT(R"({"time":100,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"nearest":0,"ready":true,"busy":false,"position":{},"enchantments":[],"spells":[{"id":11,"category":1,"power":100,"skill":300,"self_buff":true},{"id":12,"category":160,"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":7}]})"));
    auto P=ParseUCM(TEXT(R"({"buffing":true,"recovery":false,"combat":"off","navigation":false})"));
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    FACEPluginVM VM;TestTrue(TEXT("UCM loads"),VM.Load(Source,Error));TSharedPtr<FJsonObject> Intent;
    auto Step=[&](){H->ExtendSnapshot(S);TestTrue(*Error,VM.Step(S,P,Intent,Error));return Intent.IsValid();};
    if(!Step())return false;
    TestEqual(TEXT("Both carried wands and worn armor appear in real host snapshot"),S->GetArrayField(TEXT("inventory")).Num(),3);
    for(auto V:S->GetArrayField(TEXT("inventory")))if(uint32(V->AsObject()->GetNumberField(TEXT("id")))==uint32(Armor))
    {
        TestEqual(TEXT("Real host snapshot retains public fractional workmanship"),V->AsObject()->GetNumberField(TEXT("workmanship")),7.25);
        TestFalse(TEXT("Missing appraisal rating is not invented as zero"),V->AsObject()->HasField(TEXT("damage_rating")));
    }
    TestEqual(TEXT("Buff cycle first requests casting equipment"),Intent->GetStringField(TEXT("action")),FString(TEXT("equip")));
    const int32 Chosen=int32(uint32(Intent->GetNumberField(TEXT("item"))));
    FACEBinaryWriter W;W.WriteUInt32(uint32(Chosen));W.WriteInt32(int32(ACEEquipMask::Held));
    FACEBinaryReader Reader(W.GetData());Session.HandleWieldItem(Reader);
    S->SetNumberField(TEXT("time"),103);
    if(!Step())return false;
    TestEqual(TEXT("Server wield acknowledgement retains the wand in the snapshot"),S->GetArrayField(TEXT("inventory")).Num(),3);
    TestEqual(TEXT("Buff casts instead of swapping to the other wand"),Intent->GetStringField(TEXT("action")),FString(TEXT("cast")));
    S->SetArrayField(TEXT("enchantments"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"category":1,"power":100,"remaining":500})")))});
    S->SetNumberField(TEXT("time"),107);
    if(!Step())return false;
    TestEqual(TEXT("Worn armor is buffed via a single player-target cast"),Intent->GetNumberField(TEXT("target")),double(uint32(Session.PlayerGuid)));
    auto E=ParseUCM(TEXT(R"({"category":160,"power":100})"));
    E->SetNumberField(TEXT("target"),uint32(Session.PlayerGuid));E->SetNumberField(TEXT("expires"),FPlatformTime::Seconds()+500);
    H->ItemBuffs.Add(MakeShared<FJsonValueObject>(E));
    S->SetNumberField(TEXT("time"),111);
    if(!Step())return false;
    TestFalse(TEXT("Confirmed self and equipment buffs complete the cycle"),Intent->HasField(TEXT("action")));
    S->SetNumberField(TEXT("time"),130);if(!Step())return false;
    TestFalse(TEXT("Later ticks do not restart equipment swapping after buff completion"),Intent->HasField(TEXT("action")));
    {
        // Exercise the session-to-policy boundary, not a hand-authored container
        // flag. An early owned-bag packet used to block UCM forever.
        FACEBinaryWriter Contents;Contents.WriteUInt32(900);Contents.WriteUInt32(0);
        FACEBinaryReader ContentsReader(Contents.GetData());Session.HandleViewContents(ContentsReader);
        FACEWorldObject Bag;Bag.Guid=900;Bag.Name=TEXT("Pack");Bag.ContainerId=Session.PlayerGuid;Bag.ItemsCapacity=24;
        Session.UpsertWorldObject(Bag);
        FACEWorldObject Player;Player.Guid=Session.PlayerGuid;Player.Name=TEXT("Loot Tester");Session.UpsertWorldObject(Player);
        FACEWorldObject Corpse;Corpse.Guid=int32(0x800001AEu);Corpse.Name=TEXT("Corpse of Brown Rat");
        Corpse.bHasPosition=true;Corpse.Position=Session.PlayerPosition;Corpse.ObjectDescriptionFlags=ACEObjectDescFlag::Corpse;
        Session.UpsertWorldObject(Corpse);
        FACEAppraisalInfo Info;Info.ObjectGuid=Corpse.Guid;Info.bSuccess=true;Info.StringProperties.Add(16,TEXT("Killed by Loot Tester."));H->ObserveAppraisal(Info);
        auto LootProfile=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[{"action":"keep"}]})"));
        FACEPluginVM LootVM;TestTrue(TEXT("Corpse lifecycle policy loads"),LootVM.Load(Source,Error));
        H->ExtendSnapshot(S);
        TestEqual(TEXT("Host no longer exposes owned bag as open container"),S->GetNumberField(TEXT("container")),0.);
        TestTrue(TEXT("Loot policy executes after bag ownership arrives"),LootVM.Step(S,LootProfile,Intent,Error));
        if(Intent)TestEqual(TEXT("UCM approaches own corpse after phantom container clears"),Intent->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        // ACE deliberately strips '+' from killer.Name in the corpse's LongDesc.
        // Test the real snapshot parser and policy together for admins and pets.
        for(const TCHAR* Description:{TEXT("Killed by Loot Tester."),TEXT("Killed by loot tester's Fire Elemental.")})
        {
            Player.Name=TEXT("+Loot Tester");Session.UpsertWorldObject(Player);
            H->Appraisals.Remove(Corpse.Guid);
            FACEPluginVM AdminLoot;AdminLoot.Load(Source,Error);H->ExtendSnapshot(S);
            TestTrue(TEXT("New admin corpse starts with appraisal"),AdminLoot.Step(S,LootProfile,Intent,Error));
            if(Intent)TestEqual(TEXT("Unidentified corpse requests ownership appraisal"),Intent->GetStringField(TEXT("action")),FString(TEXT("identify")));
            Info.StringProperties.Add(16,Description);H->ObserveAppraisal(Info);H->ExtendSnapshot(S);
            TestTrue(TEXT("Admin corpse ownership policy executes"),AdminLoot.Step(S,LootProfile,Intent,Error));
            if(Intent)TestEqual(TEXT("Admin and summoned-pet kills approach without enabling others' corpses"),Intent->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        }
    }
    // Exercise the native spell and inventory snapshots with real Void data,
    // including high-bit item GUIDs, selected equipment and component stacks.
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    if(!TestTrue(TEXT("DAT loads for native Void snapshot"),Dat->LoadDatDirectory(ACETestDatDirectory())))return false;
    FACESkillInfo Void;Void.SkillId=43;Void.AdvancementClass=3;Void.Current=500;
    FACESkillInfo War;War.SkillId=34;War.AdvancementClass=1;War.Current=600;
    Session.PlayerVitals.Skills={Void,War};Session.PlayerVitals.bValid=true;
    Session.PlayerVitals.Health=Session.PlayerVitals.MaxHealth=100;
    Session.PlayerVitals.Stamina=Session.PlayerVitals.MaxStamina=100;
    Session.PlayerVitals.Mana=Session.PlayerVitals.MaxMana=100;
    Add(WandA,ACEItemType::Caster,ACEEquipMask::Held,true);
    for(uint32 Family:{640u,636u})
    {
        uint32 SpellId=0;
        for(uint32 Id=1;Id<6500;++Id)
        {
            uint32 School,Power,Category,Flags;double Duration;
            if(Dat->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration)&&School==5&&Category==Family&&Power<=300){SpellId=Id;break;}
        }
        if(!TestTrue(TEXT("Native fixture finds a real Void spell"),SpellId!=0))continue;
        Session.KnownSpells={int32(SpellId)};TMap<FString,int32> Scarabs;
        TestTrue(TEXT("Native Void components are known"),Dat->TryGetPluginSpellScarabs(SpellId,Scarabs));
        int32 SupplyId=1000;
        for(const auto& Pair:Scarabs)
        {
            FACEWorldObject Item;Item.Guid=SupplyId++;Item.Name=Pair.Key;Item.StackSize=100;
            Item.ContainerId=Session.PlayerGuid;Item.ItemType=ACEItemType::SpellComponents;Session.UpsertWorldObject(Item);
        }
        auto Native=H->Snapshot();
        Native->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":500,"name":"Void target","identified":true,"distance":10,"line_of_sight":true})")))});
        auto Profile=ParseUCM(TEXT(R"({"combat":"auto","buffing":false,"recovery":false,"looting":false,"debuff_destructive":true})"));
        Profile->SetBoolField(TEXT("debuff_destructive"),Family==636);
        Profile->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(uint32(WandA))});
        FACEPluginVM NativeVM;TestTrue(TEXT("Native Void policy loads"),NativeVM.Load(Source,Error));
        TestTrue(*Error,NativeVM.Step(Native,Profile,Intent,Error));
        if(TestTrue(TEXT("Native Void snapshot yields an intent"),Intent.IsValid()))
        {
            FString Action;Intent->TryGetStringField(TEXT("action"),Action);
            TestEqual(*FString::Printf(TEXT("Native Void family %u: %s"),Family,*Intent->GetStringField(TEXT("status"))),Action,FString(TEXT("cast")));
            if(Action==TEXT("cast"))TestEqual(TEXT("Native Void chooses its known spell"),uint32(Intent->GetNumberField(TEXT("spell"))),SpellId);
        }
    }
    // Custom wield eligibility must refresh when server boolean qualities or
    // raw maximum vitals change, even with no new inventory or appraisal event.
    FACEWorldObject Custom;Custom.Guid=int32(0xF06A016Au);Custom.WeenieClassId=29260;Custom.Name=TEXT("Blunt Sceptre");
    Custom.ItemType=ACEItemType::Caster;Custom.ContainerId=Session.PlayerGuid;Custom.ValidLocations=ACEEquipMask::Held;
    Session.UpsertWorldObject(Custom);
    FACEBinaryWriter Contain;Contain.WriteUInt32(uint32(Custom.Guid));Contain.WriteUInt32(uint32(Session.PlayerGuid));Contain.WriteInt32(0);Contain.WriteInt32(0);
    FACEBinaryReader ContainReader(Contain.GetData());Session.HandleInventoryPutObjInContainer(ContainReader);
    FACEAppraisalInfo CustomInfo;CustomInfo.ObjectGuid=Custom.Guid;CustomInfo.bSuccess=true;
    CustomInfo.IntProperties.Add(158,10);CustomInfo.IntProperties.Add(159,999);CustomInfo.IntProperties.Add(160,1);
    H->ObserveAppraisal(CustomInfo);
    auto CustomSnapshot=[&]()->TSharedPtr<FJsonObject>{auto Snapshot=MakeShared<FJsonObject>();H->ExtendInventorySnapshot(Snapshot);
        for(auto Item:Snapshot->GetArrayField(TEXT("inventory")))if(Item->AsObject()->GetNumberField(TEXT("id"))==double(uint32(Custom.Guid)))return Item->AsObject();
        AddError(TEXT("Custom wand missing from inventory snapshot"));return MakeShared<FJsonObject>();};
    auto Row=CustomSnapshot();TestFalse(TEXT("Unmet boolean requirement blocks custom wand"),Row->GetBoolField(TEXT("can_wield")));
    TestTrue(TEXT("Native eligibility exposes which custom requirement failed"),Row->GetStringField(TEXT("wield_reason")).Contains(TEXT("boolean property 999")));
    Session.PlayerVitals.QualityBools.Add(999,true);
    TestTrue(TEXT("Server boolean change invalidates cached eligibility"),CustomSnapshot()->GetBoolField(TEXT("can_wield")));
    CustomInfo.IntProperties[158]=6;CustomInfo.IntProperties[159]=1;CustomInfo.IntProperties[160]=500;
    Session.PlayerVitals.BaseMaxHealth=499;H->ObserveAppraisal(CustomInfo);
    TestFalse(TEXT("Raw vital requirement rejects lower base health"),CustomSnapshot()->GetBoolField(TEXT("can_wield")));
    Session.PlayerVitals.BaseMaxHealth=500;
    TestTrue(TEXT("Raw max-vital change invalidates cached eligibility"),CustomSnapshot()->GetBoolField(TEXT("can_wield")));
    // Exact custom requirements from the Blunt Sceptre UB dump. Exercise real
    // private Int64 packets, including eligibility changes with no new appraisal.
    CustomInfo.IntProperties={{45,4},{179,32},{273,13},{274,9007},{275,5500},{276,13},{277,9009},{278,3500}};
    CustomInfo.FloatProperties.Add(152,2.9275233745575);H->ObserveAppraisal(CustomInfo);
    auto UpdateCount=[&](int32 Key,int64 Count){FACEBinaryWriter W;W.WriteUInt8(1);W.WriteUInt32(Key);W.WriteUInt64(uint64(Count));
        FACEBinaryReader R(W.GetData());Session.HandlePrivateUpdatePropertyInt64(R);};
    Row=CustomSnapshot();TestTrue(TEXT("Unreported augmentation counts permit a server-validated equip attempt"),Row->GetBoolField(TEXT("can_wield")));
    TestTrue(TEXT("Unreported counts are marked for server validation"),Row->GetBoolField(TEXT("wield_requires_server_check")));
    UpdateCount(9007,0);
    TestFalse(TEXT("Receiving zero invalidates the previously unknown count"),CustomSnapshot()->GetBoolField(TEXT("can_wield")));
    UpdateCount(9007,5500);
    Row=CustomSnapshot();TestTrue(TEXT("Known Creature count permits server validation of missing Life count"),Row->GetBoolField(TEXT("can_wield")));
    TestTrue(TEXT("Life count still requires server validation"),Row->GetBoolField(TEXT("wield_requires_server_check")));
    UpdateCount(9009,3499);
    Row=CustomSnapshot();TestFalse(TEXT("Creature augmentation alone does not satisfy a known low Life count"),Row->GetBoolField(TEXT("can_wield")));
    TestTrue(TEXT("Low Life count is explained in slot four"),Row->GetStringField(TEXT("wield_reason")).Contains(TEXT("Wield requirement 4: 64-bit integer property 9009 requires 3500")));
    UpdateCount(9009,3500);
    Row=CustomSnapshot();TestTrue(TEXT("Both reported Int64 requirements make the wand eligible"),Row->GetBoolField(TEXT("can_wield")));
    TestFalse(TEXT("Both known counts remove the server-check note"),Row->GetBoolField(TEXT("wield_requires_server_check")));
    UpdateCount(1,123456);UpdateCount(6,789);
    TestTrue(TEXT("Unrelated XP/luminance packets reuse cached inventory rows"),CustomSnapshot()==Row);
    UpdateCount(9007,5499);
    TestFalse(TEXT("Losing a required augmentation invalidates eligibility without reassessment"),CustomSnapshot()->GetBoolField(TEXT("can_wield")));
    UpdateCount(9007,int64(1)<<40);
    TestTrue(TEXT("Large Int64 update restores eligibility without truncation"),CustomSnapshot()->GetBoolField(TEXT("can_wield")));
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootMemoryTest,"ACE.Plugins.LootMemory",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootMemoryTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    TArray<TSharedPtr<FJsonObject>> Profiles;
    auto Synthetic=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true})"));
    TArray<TSharedPtr<FJsonValue>> Rules;
    for(int I=0;I<1637;++I)
    {
        auto Rule=ParseUCM(TEXT(R"({"action":"keep","label":"Complex salvage rule","conditions":[{"field":"int","key":1,"op":"ge","value":0},{"field":"int","key":2,"op":"ge","value":0},{"field":"int","key":3,"op":"ge","value":0},{"field":"int","key":4,"op":"ge","value":0}]})"));
        Rule->SetNumberField(TEXT("material"),I==1636?59:60);Rules.Add(MakeShared<FJsonValueObject>(Rule));
    }
    Synthetic->SetArrayField(TEXT("loot_rules"),Rules);Profiles.Add(Synthetic);
    const FString Folder=FPaths::ProjectSavedDir()/TEXT("ClientPlugins/ImportInbox");
    for(const TCHAR* Name:{TEXT("LootSnobV4.utl"),TEXT("Gardener_LootSnobV4.utl")})if(FPaths::FileExists(Folder/Name))
    {
        TArray<FString> Issues;auto Profile=ACEVTProfile::ConvertFile(Folder/Name,Issues);
        if(!TestTrue(TEXT("Large loot fixture converts"),Profile.IsValid()&&Issues.IsEmpty()))return false;
        Profile->SetBoolField(TEXT("buffing"),false);Profile->SetBoolField(TEXT("recovery"),false);
        Profile->SetStringField(TEXT("combat"),TEXT("off"));Profile->SetBoolField(TEXT("looting"),true);Profiles.Add(Profile);
    }
    for(int ProfileIndex=0;ProfileIndex<Profiles.Num();++ProfileIndex)
    {
        FACEPluginVM VM;if(!TestTrue(TEXT("Loot memory policy loads"),VM.Load(Source,Error)))return false;
        bool Looted=false;int FirstDecision=-1;
        for(int Tick=0;Tick<80;++Tick)
        {
            auto Snapshot=ParseUCM(TEXT(R"({"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"inventory":[],"targets":[],"container":700,"container_is_corpse":true,"contents_ready":true,"contents":[{"id":701,"wcid":42,"name":"Copper Bracelet","type":8,"material":59,"workmanship":7.25,"value":20000,"burden":50,"count":1,"identified":true,"int_properties":{"1":1,"2":1,"3":1,"4":1}}]})"));
            Snapshot->SetNumberField(TEXT("time"),100+Tick);
            TArray<TSharedPtr<FJsonValue>> Spells;
            for(int I=1;I<=4000;++I)
            {
                auto Spell=ParseUCM(TEXT(R"({"known":true,"name":"Learned spell","icon":100663297,"school":4,"power":200,"category":1,"target_type":1,"flags":12,"duration":300,"projectile":false,"beneficial":true,"caster_target":true,"self_buff":true,"skill":400,"components_known":true,"scarabs":{"Scarab":1}})"));
                Spell->SetNumberField(TEXT("id"),I);Spells.Add(MakeShared<FJsonValueObject>(Spell));
            }
            Snapshot->SetArrayField(TEXT("spells"),Spells);
            TArray<TSharedPtr<FJsonValue>> Inventory;
            for(int I=0;I<250;++I)
            {
                auto Item=ParseUCM(TEXT(R"({"wcid":42,"name":"Carried equipment","type":8,"count":1,"identified":true,"material":59,"value":20000,"burden":50,"int_properties":{"1":1,"2":2,"3":3,"4":4,"5":5,"6":6,"7":7,"8":8,"9":9,"10":10}})"));
                Item->SetNumberField(TEXT("id"),10000+I);Inventory.Add(MakeShared<FJsonValueObject>(Item));
            }
            Snapshot->SetArrayField(TEXT("inventory"),Inventory);
            TSharedPtr<FJsonObject> Intent;
            if(!VM.Step(Snapshot,MakeShared<FJsonObject>(*Profiles[ProfileIndex]),Intent,Error))
            {AddError(FString::Printf(TEXT("Loot profile %d, tick %d, peak %llu bytes: %s"),ProfileIndex,Tick,uint64(VM.GetPeakMemoryUsage()),*Error));return false;}
            FString Action;if(Intent&&Intent->TryGetStringField(TEXT("action"),Action))
            {if(Action==TEXT("loot"))Looted=true;if(FirstDecision<0)FirstDecision=Tick;}
        }
        if(ProfileIndex==0)TestTrue(TEXT("Large profile reaches its last matching rule"),Looted);
        TestTrue(TEXT("Large loot profile decides within four callbacks, including a full spellbook"),FirstDecision>=0&&FirstDecision<4);
        AddInfo(FString::Printf(TEXT("Loot profile %d: first decision after %d callbacks"),ProfileIndex,FirstDecision+1));
        AddInfo(FString::Printf(TEXT("Loot profile %d: %llu bytes peak, %llu bytes after 80 callbacks"),ProfileIndex,uint64(VM.GetPeakMemoryUsage()),uint64(VM.GetMemoryUsage())));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMHuntMemoryTest,"ACE.Plugins.HuntMemory",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMHuntMemoryTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    // Observe only retained policy bookkeeping, without exposing test hooks to
    // plugins or confusing ordinary Lua GC garbage with a live-object leak.
    const int32 Return=Source.Find(TEXT("return intent"),ESearchCase::CaseSensitive,ESearchDir::FromEnd);
    if(!TestTrue(TEXT("Policy return found"),Return!=INDEX_NONE))return false;
    Source.InsertAt(Return,TEXT("local count=0;for _,t in ipairs({retries,until_time,corpses,item_attempts,buff_skips,unavailable_spells}) do for _ in pairs(t) do count=count+1 end end;intent=intent or {};intent.history_count=count;\n "));
    FACEPluginVM VM;if(!TestTrue(TEXT("Hunting policy loads"),VM.Load(Source,Error)))return false;
    auto S=ParseUCM(TEXT(R"({"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"position":{"cell":16974092,"x":49,"y":-91,"z":-30},"spells":[],"enchantments":[],"targets":[],"inventory":[]})"));
    auto P=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","looting":true,"loot_rules":[{"action":"keep","name":"Collected loot"}],"navigation":false,"idle_peace":false})"));
    auto Sword=ParseUCM(TEXT(R"({"id":50,"wcid":50,"name":"Sword","type":1,"identified":true,"can_wield":true,"equipped":true,"damage":30,"damage_type":1})"));
    auto Target=ParseUCM(TEXT(R"({"id":500,"name":"Shadow Annihilator","distance":1,"identified":false,"line_of_sight":true})"));
    int32 Assessed=0,Attacked=0,Closed=0;double PeakHistory=0;
    for(int32 Hunt=0;Hunt<600;++Hunt)
    {
        TArray<TSharedPtr<FJsonValue>> Inventory={MakeShared<FJsonValueObject>(Sword)};
        for(int32 I=0;I<10;++I)
        {
            auto Item=ParseUCM(TEXT(R"({"wcid":42,"name":"Collected loot","type":8,"count":1,"identified":true})"));
            Item->SetNumberField(TEXT("id"),10000+Hunt*10+I);Inventory.Add(MakeShared<FJsonValueObject>(Item));
        }
        S->SetArrayField(TEXT("inventory"),Inventory);Target->SetNumberField(TEXT("id"),100000+Hunt);
        for(int32 Phase=0;Phase<5;++Phase)
        {
            S->SetNumberField(TEXT("time"),100+Hunt*20+Phase);
            Target->SetBoolField(TEXT("identified"),Phase!=0);
            S->SetArrayField(TEXT("targets"),Phase<2?TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueObject>(Target)}:TArray<TSharedPtr<FJsonValue>>{});
            S->SetNumberField(TEXT("container"),Phase>=2?200000+Hunt:0);
            S->SetBoolField(TEXT("container_is_corpse"),Phase>=2);S->SetBoolField(TEXT("contents_ready"),true);S->SetArrayField(TEXT("contents"),{});
            TSharedPtr<FJsonObject> Intent;
            if(!VM.Step(S,P,Intent,Error)){AddError(FString::Printf(TEXT("Hunt %d phase %d: %s"),Hunt,Phase,*Error));return false;}
            FString Action;Intent->TryGetStringField(TEXT("action"),Action);
            Assessed+=Action==TEXT("identify");Attacked+=Action==TEXT("attack");Closed+=Action==TEXT("close_corpse");
            PeakHistory=FMath::Max(PeakHistory,Intent->GetNumberField(TEXT("history_count")));
            if(!TestTrue(TEXT("Retry/corpse history stays bounded as GUIDs change"),PeakHistory<100))return false;
        }
    }
    TestEqual(TEXT("Each newly spawned monster is assessed"),Assessed,600);
    TestEqual(TEXT("Each assessed monster can be attacked"),Attacked,600);
    TestTrue(TEXT("Empty corpses finish looting throughout the hunt"),Closed>=600);
    AddInfo(FString::Printf(TEXT("600 hunt cycles / 3000 decisions / 6000 replaced inventory objects: retained history peak %.0f; heap peak %.2f MiB"),PeakHistory,VM.GetPeakMemoryUsage()/1048576.));
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootTest,"ACE.Plugins.LootRules",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":0,"inventory":[],"spells":[],"targets":[],"container":700,"container_is_corpse":true,"contents_ready":true,"contents":[{"id":701,"wcid":42,"name":"Copper Bracelet","type":8,"material":59,"workmanship":7.25,"value":20000,"burden":50,"count":5,"identified":false}]})"));};
    auto Profile=[](){return ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[]})"));};
    auto Check=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto Rule=[&](const TCHAR* Json,const FString& Expected,const TCHAR* Label)
    {
        FACEPluginVM VM;TestTrue(TEXT("Loot policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();
        P->SetArrayField(TEXT("loot_rules"),ParseUCM(Json)->GetArrayField(TEXT("rules")));
        TestEqual(Label,Check(VM,S,P)->GetStringField(TEXT("action")),Expected);
    };
    Rule(TEXT(R"({"rules":[{"material":59,"min_workmanship":7,"max_workmanship":8,"min_value":20000,"max_burden":50}]})"),TEXT("loot"),TEXT("Public material/workmanship/value/burden match without appraisal"));
    Rule(TEXT(R"({"rules":[{"material":60,"min_rating":10}]})"),TEXT("close_corpse"),TEXT("Wrong material avoids unnecessary appraisal"));
    Rule(TEXT(R"({"rules":[{"min_workmanship":8}]})"),TEXT("close_corpse"),TEXT("Low workmanship stays on corpse"));
    Rule(TEXT(R"({"rules":[{"max_workmanship":7}]})"),TEXT("close_corpse"),TEXT("Workmanship upper bound preserves fractional precision"));
    Rule(TEXT(R"({"rules":[{"name":"copper bracelet","name_mode":"exact"}]})"),TEXT("loot"),TEXT("Exact matching is case insensitive"));
    Rule(TEXT(R"({"rules":[{"name":"Copper","name_mode":"exact"}]})"),TEXT("close_corpse"),TEXT("Exact is not prefix matching"));
    Rule(TEXT(R"({"rules":[{"name":"brace","name_mode":"contains"}]})"),TEXT("loot"),TEXT("Substring name matches"));
    Rule(TEXT(R"({"rules":[{"name":".*","name_mode":"contains"}]})"),TEXT("close_corpse"),TEXT("Name text is literal, not executable patterns"));
    Rule(TEXT(R"({"rules":[{"action":"skip","material":59},{}]})"),TEXT("close_corpse"),TEXT("Skip beats a later broad Keep rule"));
    Rule(TEXT(R"({"rules":[{"action":"skip","enabled":false},{}]})"),TEXT("loot"),TEXT("Disabled rule is ignored"));
    Rule(TEXT(R"({"rules":[{"max_value":19999}]})"),TEXT("close_corpse"),TEXT("Maximum value is honored"));
    Rule(TEXT(R"({"rules":[{"type":2}]})"),TEXT("close_corpse"),TEXT("Item type filter is honored"));
    for(bool Legacy:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        auto R=ParseUCM(TEXT(R"({"keep_up_to":4})"));R->SetBoolField(TEXT("count_by_name"),Legacy);P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(R)});
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":2,"wcid":999,"name":"Copper Bracelet","count":3})")))});
        TestEqual(TEXT("Imported KeepUpTo uses exact item name across WCIDs; native rule retains WCID scope"),Check(VM,S,P)->GetNumberField(TEXT("amount")),Legacy?1.:4.);
    }
    for(bool Confirm:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"salvage"})")))});
        auto Item=S->GetArrayField(TEXT("contents"))[0]->AsObject();Item->SetNumberField(TEXT("count"),1);Item->SetBoolField(TEXT("identified"),true);
        auto Ust=MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":9,"wcid":9,"name":"Ust","count":1,"object_class":40})")));
        S->SetArrayField(TEXT("inventory"),{Ust});
        TestEqual(TEXT("Salvage rule starts by taking the item"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
        S->SetNumberField(TEXT("container"),0);S->SetArrayField(TEXT("contents"),{});
        TestFalse(TEXT("Auto-closed corpse still waits for ownership confirmation"),Check(VM,S,P)->HasField(TEXT("action")));
        if(!Confirm)
        {
            S->SetNumberField(TEXT("time"),111);
            TestEqual(TEXT("Closure never turns a failed transfer into success"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        }
        else
        {
            S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":700,"name":"Corpse","distance":2})")))});
            S->SetArrayField(TEXT("inventory"),{Ust,MakeShared<FJsonValueObject>(Item)});
            auto I=Check(VM,S,P);TestEqual(TEXT("Final corpse item retains its salvage action after auto-close"),I->GetStringField(TEXT("action")),FString(TEXT("salvage")));
            TestEqual(TEXT("Confirmed final pickup remembers an auto-closed corpse"),I->GetNumberField(TEXT("completed_corpse")),700.);
            TestEqual(TEXT("Only the newly acquired item is salvaged"),I->GetNumberField(TEXT("item")),701.);
            TestFalse(TEXT("Salvage is issued once while its result is pending"),Check(VM,S,P)->HasField(TEXT("action")));
            S->SetArrayField(TEXT("inventory"),{Ust});TestFalse(TEXT("Completed salvage job is not replayed"),Check(VM,S,P)->HasField(TEXT("action")));
        }
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Item=S->GetArrayField(TEXT("contents"))[0]->AsObject();
        P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"min_workmanship":7})")))});
        Item->RemoveField(TEXT("workmanship"));TestEqual(TEXT("Absent workmanship requests appraisal"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("identify")));
        Item->SetBoolField(TEXT("identified"),true);TestEqual(TEXT("Missing appraisal property does not match"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetArrayField(TEXT("loot_rules"),ParseUCM(TEXT(R"({"rules":[{"keep_up_to":4},{}]})"))->GetArrayField(TEXT("rules")));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":2,"wcid":42,"count":2,"equipped":true})")))});
        TestEqual(TEXT("Keep limit includes equipped and bag counts"),Check(VM,S,P)->GetNumberField(TEXT("amount")),2.);
        S->GetArrayField(TEXT("contents"))[0]->AsObject()->SetNumberField(TEXT("count"),3);
        TestFalse(TEXT("Corpse change alone is not inventory confirmation"),Check(VM,S,P)->HasField(TEXT("action")));
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("count"),4);
        TestEqual(TEXT("Reached keep limit does not fall through to broad Keep"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())});
        S->SetBoolField(TEXT("contents_ready"),false);TestFalse(TEXT("Late descriptions are not mistaken for empty corpse"),Check(VM,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),S->GetNumberField(TEXT("time"))+21);
        auto Timeout=Check(VM,S,P);
        TestEqual(TEXT("Missing descriptions cannot hang the scheduler indefinitely"),Timeout->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
        TestFalse(TEXT("Timed-out corpse is not marked fully looted"),Timeout->HasField(TEXT("completed")));
        S->SetBoolField(TEXT("contents_ready"),true);S->SetBoolField(TEXT("container_is_corpse"),false);
        TestFalse(TEXT("Loot rules do not take items from other containers"),Check(VM,S,P)->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();TArray<TSharedPtr<FJsonValue>> Rules;
        for(int32 Index=0;Index<511;++Index)Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"material":60})"))));
        Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"material":59})"))));P->SetArrayField(TEXT("loot_rules"),Rules);
        TestEqual(TEXT("512 simple rules finish in one callback"),Check(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();TArray<TSharedPtr<FJsonValue>> Rules;
        auto Item=S->GetArrayField(TEXT("contents"))[0]->AsObject();Item->SetBoolField(TEXT("identified"),true);
        Item->SetObjectField(TEXT("int_properties"),ParseUCM(TEXT(R"({"1":1})")));
        // Force continuation partway through rules; reverse contents every tick
        // as unordered server-object enumeration can do. First match still wins.
        TArray<TSharedPtr<FJsonValue>> Conditions;
        for(int I=0;I<33;++I)Conditions.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"field":"int","key":1,"op":"eq","value":1})"))));
        Conditions.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"field":"int","key":1,"op":"eq","value":2})"))));
        for(int I=0;I<64;++I){auto R=MakeShared<FJsonObject>();R->SetArrayField(TEXT("conditions"),Conditions);Rules.Add(MakeShared<FJsonValueObject>(R));}
        Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"skip"})"))));
        Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"keep"})"))));P->SetArrayField(TEXT("loot_rules"),Rules);
        auto Other=MakeShared<FJsonObject>(*Item);Other->SetNumberField(TEXT("id"),702);
        bool Finished=false,Yielded=false;
        for(int Tick=0;Tick<8&&!Finished;++Tick)
        {
            S->SetArrayField(TEXT("contents"),Tick%2?TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueObject>(Other),MakeShared<FJsonValueObject>(Item)}:TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueObject>(Item),MakeShared<FJsonValueObject>(Other)});
            auto Intent=Check(VM,S,P);FString Action;
            if(Intent->TryGetStringField(TEXT("action"),Action)){TestEqual(TEXT("Skip precedes keep after continued evaluation"),Action,FString(TEXT("close_corpse")));Finished=true;}
            else {Yielded=true;TestTrue(TEXT("Continuation reports rule progress"),Intent->GetStringField(TEXT("status")).Contains(TEXT("rule ")));}
        }
        TestTrue(TEXT("Complex scan uses cooperative continuation"),Yielded);
        TestTrue(TEXT("Reordered corpse completes without restarting"),Finished);
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        auto Item=S->GetArrayField(TEXT("contents"))[0]->AsObject();Item->SetBoolField(TEXT("identified"),true);
        TArray<TSharedPtr<FJsonValue>> Names,Conditions;
        // Cached predicates are much cheaper; use enough work to exercise the
        // instruction checkpoint rather than relying on regex compilation time.
        for(int I=0;I<512;++I)Names.Add(MakeShared<FJsonValueString>(TEXT("Spell")));
        for(int I=0;I<64;++I)Conditions.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"field":"spells","pattern":"^Spell$","value":512})"))));
        Item->SetArrayField(TEXT("spell_names"),Names);
        auto R=MakeShared<FJsonObject>();R->SetArrayField(TEXT("conditions"),Conditions);R->SetStringField(TEXT("action"),TEXT("skip"));
        P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(R),MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"keep"})")))});
        bool Finished=false,Yielded=false;
        for(int Tick=0;Tick<32&&!Finished;++Tick)
        {
            auto Intent=Check(VM,S,P);FString Action;
            if(Intent->TryGetStringField(TEXT("action"),Action)){TestEqual(TEXT("Spell count survives continuation inside a condition"),Action,FString(TEXT("close_corpse")));Finished=true;}
            else Yielded=true;
        }
        TestTrue(TEXT("Expensive spell rule cooperatively yields"),Yielded);
        TestTrue(TEXT("Expensive spell rule finishes without quota errors"),Finished);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootThroughputTest,"ACE.Plugins.LootThroughput",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootThroughputTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    TArray<TSharedPtr<FJsonObject>> Profiles;
    auto Synthetic=ParseUCM(TEXT(R"({"loot_rules":[]})"));
    TArray<TSharedPtr<FJsonValue>> Rules;
    // The expensive condition appears first in the editor, as in imported
    // profiles. Rejecting the later numeric condition must avoid spell scans.
    for(int I=0;I<1000;++I)Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"skip","conditions":[{"field":"spells","pattern":"^Incantation","value":30},{"field":"int","key":1,"op":"eq","value":999}]})"))));
    Rules.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"action":"keep","conditions":[{"field":"spells","pattern":"^Incantation","value":1}]})"))));
    Synthetic->SetArrayField(TEXT("loot_rules"),Rules);Profiles.Add(Synthetic);
    for(const FString& Path:{FPaths::ProjectSavedDir()/TEXT("ClientPlugins/ImportInbox/LootSnobV4.utl"),FPaths::ProjectSavedDir()/TEXT("ClientPlugins/ImportInbox/Gardener_LootSnobV4.utl"),FString(TEXT("C:/Users/orent/Downloads/PhaelaeCustom_v6.utl"))})if(FPaths::FileExists(Path))
    {
        TArray<FString> Issues;auto P=ACEVTProfile::ConvertFile(Path,Issues);
        if(!TestTrue(*FString::Printf(TEXT("Import %s"),*FPaths::GetCleanFilename(Path)),P.IsValid()&&Issues.IsEmpty()))return false;
        Profiles.Add(P);
    }
    for(int Index=0;Index<Profiles.Num();++Index)
    {
        auto P=Profiles[Index];P->SetBoolField(TEXT("buffing"),false);P->SetBoolField(TEXT("recovery"),false);P->SetStringField(TEXT("combat"),TEXT("off"));P->SetBoolField(TEXT("looting"),true);
        auto S=ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"inventory":[],"targets":[],"spells":[],"container":700,"container_is_corpse":true,"contents_ready":true})"));
        TArray<TSharedPtr<FJsonValue>> Contents,Inventory,Names;
        for(int I=0;I<128;++I)Names.Add(MakeShared<FJsonValueString>(I==0?TEXT("Incantation of Strength Self"):TEXT("Other Spell")));
        for(int I=0;I<20;++I)
        {
            auto Item=ParseUCM(TEXT(R"({"id":701,"wcid":42,"name":"Copper Bracelet","type":8,"object_class":8,"material":59,"workmanship":7.25,"value":20000,"burden":50,"count":1,"identified":true,"int_properties":{"1":1},"string_properties":{"1":"Copper Bracelet"}})"));
            Item->SetNumberField(TEXT("id"),701+I);Item->SetArrayField(TEXT("spell_names"),Names);Contents.Add(MakeShared<FJsonValueObject>(Item));
        }
        FACEPluginVM VM;if(!TestTrue(TEXT("Throughput policy loads"),VM.Load(Source,Error)))return false;
        int Steps=0,Looted=0,Yields=0;bool Finished=false;double Work=0,Peak=0;
        for(;Steps<256&&!Finished;++Steps)
        {
            S->SetArrayField(TEXT("contents"),Contents);S->SetArrayField(TEXT("inventory"),Inventory);S->SetNumberField(TEXT("time"),100+Steps*.25);
            TSharedPtr<FJsonObject> Intent;const double Started=FPlatformTime::Seconds();const bool OK=VM.Step(S,P,Intent,Error);
            const double Elapsed=FPlatformTime::Seconds()-Started;Work+=Elapsed;Peak=FMath::Max(Peak,Elapsed);
            if(!TestTrue(*Error,OK))return false;
            FString Action;if(Intent)Intent->TryGetStringField(TEXT("action"),Action);
            if(Action==TEXT("close_corpse"))Finished=true;
            else if(Action==TEXT("loot"))
            {
                const double Id=Intent->GetNumberField(TEXT("item"));
                const int At=Contents.IndexOfByPredicate([Id](const auto& V){return V->AsObject()->GetNumberField(TEXT("id"))==Id;});
                if(!TestTrue(TEXT("Transfer targets remaining corpse item"),At!=INDEX_NONE))return false;
                Inventory.Add(Contents[At]);Contents.RemoveAt(At);++Looted;
            }
            else {++Yields;TestTrue(TEXT("Rule continuation requests prompt bounded rescheduling"),Intent&&Intent->HasField(TEXT("continue_work")));}
        }
        TestTrue(TEXT("Twenty-item corpse completes within bounded work"),Finished);
        if(Index==0)TestEqual(TEXT("Late cheap rejects preserve final matching rule for every item"),Looted,20);
        AddInfo(FString::Printf(TEXT("Loot throughput profile %d (%d rules): %d items taken, %d decisions, %d yields, %.2fms total VM work, %.2fms peak, %llu bytes peak Lua"),Index,P->GetArrayField(TEXT("loot_rules")).Num(),Looted,Steps,Yields,Work*1000,Peak*1000,uint64(VM.GetPeakMemoryUsage())));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootPriorityTest,"ACE.Plugins.LootCombatPriority",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootPriorityTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"nearest":0,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"spells":[],"targets":[],"inventory":[{"id":50,"wcid":50,"name":"Sword","type":1,"identified":true,"can_wield":true,"equipped":true,"damage":30,"damage_type":1}],"corpses":[{"id":700,"name":"Corpse of Rat","distance":2,"identified":true}],"contents":[{"id":701,"wcid":701,"name":"Ring","count":1,"identified":true}]})"));};
    auto Profile=[](){return ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","looting":true,"loot_priority":false,"loot_rules":[{"action":"keep"}]})"));};
    auto Enemies=[](auto S,bool Visible=true){S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(ParseUCM(Visible?TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})"):TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":false})")))});};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto Action=[](auto I){FString A;I->TryGetStringField(TEXT("action"),A);return A;};
    for(bool Priority:{false,true})for(bool NavPriority:{false,true})for(bool Open:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Enemies(S);
        P->SetBoolField(TEXT("loot_priority"),Priority);P->SetBoolField(TEXT("nav_priority"),NavPriority);
        if(Open)S->SetNumberField(TEXT("container"),700);
        TestEqual(*FString::Printf(TEXT("Priority=%d nav=%d open=%d honors combat/loot order"),Priority,NavPriority,Open),Action(Step(VM,S,P)),FString(Priority?(Open?TEXT("loot"):TEXT("open_corpse")):TEXT("attack")));
    }
    for(bool Priority:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("loot_priority"),Priority);
        TestEqual(TEXT("Without an enemy approach the corpse"),Action(Step(VM,S,P)),FString(TEXT("open_corpse")));
        Enemies(S);S->SetBoolField(TEXT("loot_approaching"),true);S->SetBoolField(TEXT("busy"),true);S->SetBoolField(TEXT("ready"),false);
        TestEqual(TEXT("A new enemy interrupts only low-priority corpse approach"),Action(Step(VM,S,P)),FString(Priority?TEXT(""):TEXT("pause_loot_approach")));
        if(Priority)continue;
        S->SetBoolField(TEXT("loot_approaching"),false);S->SetBoolField(TEXT("busy"),false);S->SetBoolField(TEXT("ready"),true);
        TestEqual(TEXT("Fight after cancelling approach"),Action(Step(VM,S,P)),FString(TEXT("attack")));
        S->SetArrayField(TEXT("targets"),{});Step(VM,S,P); // release combat target
        TestEqual(TEXT("Interrupted corpse can be retried without a blacklist or timeout"),Action(Step(VM,S,P)),FString(TEXT("open_corpse")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();S->SetNumberField(TEXT("container"),700);
        TestEqual(TEXT("Loot starts when the area is clear"),Action(Step(VM,S,P)),FString(TEXT("loot")));
        Enemies(S);
        TestEqual(TEXT("Pending transfer does not force another loot cycle before combat"),Action(Step(VM,S,P)),FString(TEXT("attack")));
        auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(S->GetArrayField(TEXT("contents"))[0]);S->SetArrayField(TEXT("inventory"),Inventory);
        S->SetArrayField(TEXT("contents"),{});S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);
        TestEqual(TEXT("Confirmed transfer survives combat and is not requested twice"),Action(Step(VM,S,P)),FString(TEXT("close_corpse")));
    }
    for(bool Combat:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Enemies(S,!Combat);
        if(!Combat)P->SetStringField(TEXT("combat"),TEXT("off"));
        TestEqual(TEXT("Manual combat or enemies behind walls do not prevent looting"),Action(Step(VM,S,P)),FString(TEXT("open_corpse")));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMTargetHandoffTest,"ACE.Plugins.TargetHandoff",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMTargetHandoffTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    for(const FString Mode:{TEXT("melee"),TEXT("missile"),TEXT("magic")})for(int32 Vital:{0,2,4,6})
    {
        auto S=ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"nearest":0,"ready":true,"busy":false,"action_serial":0,"combat_mode":8,"components_required":false,"spells":[{"id":20,"name":"Acid Bolt","category":117,"school":1,"power":100,"skill":300}],"inventory":[{"id":50,"name":"Weapon","type":1,"identified":true,"can_wield":true,"equipped":true,"damage":30,"damage_type":1}],"targets":[{"id":500,"name":"First","distance":1,"identified":true,"line_of_sight":true}]})"));
        auto P=ParseUCM(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","looting":true,"loot_priority":false,"loot_rules":[{"action":"keep"}]})"));
        P->SetStringField(TEXT("combat"),Mode);auto Items=S->GetArrayField(TEXT("inventory"));Items[0]->AsObject()->SetNumberField(TEXT("type"),Mode==TEXT("magic")?32768:Mode==TEXT("missile")?256:1);
        FACEPluginVM VM;VM.Load(Source,Error);auto First=Step(VM,S,P);
        TestEqual(TEXT("Initial attack targets the first enemy"),First->GetNumberField(TEXT("target")),500.);
        S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":501,"name":"Next","distance":1,"identified":true,"line_of_sight":true})")))});
        S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":700,"name":"Corpse of First","distance":1,"identified":true})")))});
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),20);S->SetBoolField(TEXT("last_spell_confirmed"),true);
        if(Vital)
        {
            S->SetNumberField(Vital==2?TEXT("health"):Vital==4?TEXT("stamina"):TEXT("mana"),20);
            auto Potion=ParseUCM(TEXT(R"({"id":51,"name":"Potion","type":32,"identified":true,"usable":true,"count":5,"boost":50})"));Potion->SetNumberField(TEXT("boost_vital"),Vital);
            Items.Add(MakeShared<FJsonValueObject>(Potion));S->SetArrayField(TEXT("inventory"),Items);
            auto Recovery=Step(VM,S,P);
            TestEqual(TEXT("Target switch checks all vital recovery priorities first"),Recovery->GetStringField(TEXT("activity")),FString::Printf(TEXT("recovery%d"),Vital));
        }
        else
        {
            auto Next=Step(VM,S,P);
            TestEqual(TEXT("Next living target attacks at the same timestamp without loot or idle delay"),Next->GetNumberField(TEXT("target")),501.);
            TestEqual(TEXT("Correct attack type is retained across kills"),Next->GetStringField(TEXT("action")),FString(Mode==TEXT("magic")?TEXT("cast"):TEXT("attack")));
        }
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMVoidCombatTest,"ACE.Plugins.VoidCombat",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMVoidCombatTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"combat_mode":8,"components_required":false,"spells":[{"id":20,"name":"Nether Bolt","category":640,"school":5,"power":300,"skill":500},{"id":21,"name":"Acid Bolt","category":117,"school":1,"power":300,"skill":500}],"inventory":[{"id":50,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}],"targets":[{"id":500,"name":"Enemy","distance":10,"identified":true,"line_of_sight":true,"resists":{"1024":2,"32":0.1}}]})"));};
    for(bool Exact:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
        auto P=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"magic","monsters":[{"name":"En","combat":"ignore"},{"name":"Enemy","bolt":false,"streak":false,"ring":false,"debuff_imperil":true}]})"));
        P->GetArrayField(TEXT("monsters"))[1]->AsObject()->SetBoolField(TEXT("exact"),Exact);
        auto Book=S->GetArrayField(TEXT("spells"));Book.Add(MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"id":22,"name":"Imperil","category":116,"school":2,"duration":120,"power":100,"skill":500})"))));S->SetArrayField(TEXT("spells"),Book);
        TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));
        if(TestTrue(TEXT("Debuff-only mob produces an intent"),I.IsValid()))TestEqual(TEXT("A/S/R off still casts Imperil; specific name beats earlier prefix"),I->GetNumberField(TEXT("spell")),22.);
        S->SetArrayField(TEXT("debuffs"),{MakeShared<FJsonValueObject>(ParseUCM(TEXT(R"({"target":500,"category":116,"power":100,"expires":220})")))});
        S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),22);
        TestTrue(*Error,VM.Step(S,P,I,Error));
        TestTrue(TEXT("Debuff-only mode does not fall back to a bolt"),I.IsValid()&&!I->HasField(TEXT("action")));
    }
    for(int32 Element:{0,128,1024})
    {
        FACEPluginVM VM;TestTrue(TEXT("Void policy loads"),VM.Load(Source,Error));auto S=Snapshot();
        auto P=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"magic","looting":false})"));P->SetNumberField(TEXT("damage_type"),Element);
        TSharedPtr<FJsonObject> Intent;TestTrue(*Error,VM.Step(S,P,Intent,Error));
        if(TestTrue(TEXT("Void attack produces an intent"),Intent.IsValid()))
        {TestEqual(TEXT("Nether spell can be selected automatically or explicitly"),Intent->GetNumberField(TEXT("spell")),20.);}
    }
    const TCHAR* Options[]={TEXT("debuff_corruption"),TEXT("debuff_destructive"),TEXT("debuff_corrosion")};
    const int32 Categories[]={638,636,637};
    const TCHAR* Names[]={TEXT("Corruption"),TEXT("Destructive Curse"),TEXT("Corrosion")};
    auto* Dat=NewObject<UACEDatSubsystem>(NewObject<UGameInstance>());
    if (!TestTrue(TEXT("Retail spells load"),Dat->LoadDatDirectory(ACETestDatDirectory()))) return false;
    ON_SCOPE_EXIT { Dat->Deinitialize(); };
    // Use the actual DAT flags, spell families and component requirements. The
    // hand-authored fixtures below cannot catch errors at that boundary.
    TArray<TSharedPtr<FJsonValue>> RetailVoid;
    TMap<FString,int32> Supplies;
    for(uint32 Id=1;Id<6500;++Id)
    {
        uint32 School=0,Power=0,Category=0,Flags=0,Icon=0,TargetFlags=0,TargetType=0;double Duration=0;bool Projectile=false;
        if(!Dat->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration)||School!=5||Category<636||Category>643)continue;
        FString Name;Dat->TryGetSpellInfo(Id,Name,Icon);Dat->TryGetRetailSpellTargeting(Id,TargetFlags,TargetType,Projectile);
        auto Spell=MakeShared<FJsonObject>();Spell->SetNumberField(TEXT("id"),Id);Spell->SetStringField(TEXT("name"),Name);
        Spell->SetNumberField(TEXT("school"),School);Spell->SetNumberField(TEXT("power"),Power);Spell->SetNumberField(TEXT("category"),Category);
        Spell->SetNumberField(TEXT("flags"),Flags);Spell->SetNumberField(TEXT("duration"),Duration);Spell->SetNumberField(TEXT("skill"),500);
        Spell->SetBoolField(TEXT("known"),true);Spell->SetBoolField(TEXT("beneficial"),(Flags&4)!=0);Spell->SetBoolField(TEXT("caster_target"),(Flags&8)!=0);
        Spell->SetBoolField(TEXT("projectile"),Projectile);Spell->SetNumberField(TEXT("target_type"),TargetType);
        auto Scarabs=MakeShared<FJsonObject>();TMap<FString,int32> Requirements;
        const bool Known=Dat->TryGetPluginSpellScarabs(Id,Requirements);Spell->SetBoolField(TEXT("components_known"),Known);
        TestTrue(*FString::Printf(TEXT("%s has known components"),*Name),Known);
        for(const auto& Part:Requirements){Scarabs->SetNumberField(Part.Key,Part.Value);Supplies.FindOrAdd(Part.Key)=100;}
        Spell->SetObjectField(TEXT("scarabs"),Scarabs);RetailVoid.Add(MakeShared<FJsonValueObject>(Spell));
    }
    TestTrue(TEXT("Retail Void spell catalog is populated"),RetailVoid.Num()>30);
    for(bool OnlyCurses:{false,true})for(bool Rings:{false,true})for(bool Dots:{false,true})
    {
        if(OnlyCurses&&!Dots)continue;
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
        auto Book=RetailVoid;
        if(OnlyCurses)Book.RemoveAll([](const auto& V){return V->AsObject()->GetNumberField(TEXT("category"))>638;});
        S->SetArrayField(TEXT("spells"),Book);S->SetBoolField(TEXT("components_required"),true);
        S->SetObjectField(TEXT("skills"),ParseUCM(TEXT(R"({"43":{"current":500,"training":3},"34":{"current":0,"training":1}})")));
        auto Inventory=S->GetArrayField(TEXT("inventory"));
        for(const auto& Supply:Supplies)
        {
            auto Item=MakeShared<FJsonObject>();Item->SetNumberField(TEXT("id"),1000+Inventory.Num());
            Item->SetStringField(TEXT("name"),Supply.Key);Item->SetNumberField(TEXT("count"),Supply.Value);Inventory.Add(MakeShared<FJsonValueObject>(Item));
        }
        S->SetArrayField(TEXT("inventory"),Inventory);
        auto P=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"auto","looting":false,"weapon_items":[50],"damage_type":0})"));
        P->SetBoolField(TEXT("ring"),Rings);
        for(const auto* Option:Options)P->SetBoolField(Option,Dots);
        TArray<TSharedPtr<FJsonValue>> Effects;
        for(int32 Step=0;Step<(Dots?4:1);++Step)
        {
            TSharedPtr<FJsonObject> Intent;TestTrue(*Error,VM.Step(S,P,Intent,Error));
            if(!TestTrue(TEXT("Retail Void combat produces an intent"),Intent.IsValid()))break;
            FString Action;Intent->TryGetStringField(TEXT("action"),Action);
            if(OnlyCurses&&Step==3)
            {
                TestTrue(TEXT("Active curses wait for expiry without disabling combat"),Action.IsEmpty());
                TestTrue(TEXT("Curse-only wait explains the active timers"),Intent->GetStringField(TEXT("status")).Contains(TEXT("Void curses active")));
                S->SetNumberField(TEXT("time"),140);S->SetArrayField(TEXT("debuffs"),{});
                TestTrue(*Error,VM.Step(S,P,Intent,Error));
                if(Intent){Intent->TryGetStringField(TEXT("action"),Action);TestEqual(TEXT("Expired curses resume casting without a restart"),Action,FString(TEXT("cast")));}
                break;
            }
            if(!TestEqual(*FString::Printf(TEXT("Retail Void casts (rings=%d dots=%d step=%d): %s"),Rings,Dots,Step,*Intent->GetStringField(TEXT("status"))),Action,FString(TEXT("cast"))))break;
            uint32 School,Power,Category,Flags;double Duration;
            Dat->TryGetPluginSpellInfo(uint32(Intent->GetNumberField(TEXT("spell"))),School,Power,Category,Flags,Duration);
            if(Dots&&Step<3)TestEqual(TEXT("Retail Void cycles all three curse families"),Category,uint32(Categories[Step]));
            else TestTrue(TEXT("After curses, Void casts a direct attack"),Category==639||Category==640);
            auto Effect=MakeShared<FJsonObject>();Effect->SetNumberField(TEXT("target"),500);Effect->SetNumberField(TEXT("category"),Category);
            Effect->SetNumberField(TEXT("power"),Power);Effect->SetNumberField(TEXT("expires"),100+Duration);
            Effects.Add(MakeShared<FJsonValueObject>(Effect));S->SetArrayField(TEXT("debuffs"),Effects);
            S->SetNumberField(TEXT("time"),101+Step);S->SetNumberField(TEXT("action_serial"),Step+1);
        }
    }
    // Skill, supplies and explicit switches remain gates for curse-only builds.
    for(int32 Blocker=0;Blocker<3;++Blocker)
    {
        auto S=Snapshot();auto Book=RetailVoid;
        Book.RemoveAll([](const auto& V){return V->AsObject()->GetNumberField(TEXT("category"))!=636;});
        S->SetArrayField(TEXT("spells"),Book);S->SetBoolField(TEXT("components_required"),Blocker==1);
        auto P=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"auto","looting":false,"debuff_destructive":true})"));
        if(Blocker==0)P->SetBoolField(TEXT("debuff_destructive"),false);
        if(Blocker==2)P->SetNumberField(TEXT("skill_margin"),1000);
        FACEPluginVM VM;VM.Load(Source,Error);TSharedPtr<FJsonObject> Intent;TestTrue(*Error,VM.Step(S,P,Intent,Error));
        if(!TestTrue(TEXT("Unavailable attack yields an explanation"),Intent.IsValid()))continue;
        TestEqual(TEXT("Unavailable curse never bypasses eligibility"),Intent->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        TestTrue(TEXT("Missing attack is distinguished from missing equipment"),Intent->GetStringField(TEXT("status")).Contains(TEXT("No usable attack spell")));
        S->SetNumberField(TEXT("time"),101);TestTrue(*Error,VM.Step(S,P,Intent,Error));
        if(Intent)TestTrue(TEXT("Combat failure remains visible during retry cooldown"),Intent->GetStringField(TEXT("status")).Contains(TEXT("No usable attack spell")));
    }
    bool Verified[3]={};
    for(uint32 Id=1;Id<6500;++Id)
    {
        FString Name;uint32 Icon;
        if(!Dat->TryGetSpellInfo(Id,Name,Icon))continue;
        for(int32 I=0;I<3;++I) if(!Verified[I] && Name.StartsWith(Names[I]))
        {
            uint32 School,Power,Category,Flags;double Duration;
            if(Dat->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration))
            { TestEqual(*FString::Printf(TEXT("Retail category for %s"),Names[I]),Category,uint32(Categories[I]));Verified[I]=true; }
        }
    }
    for(int32 I=0;I<3;++I)TestTrue(Names[I],Verified[I]);
    for(int32 Index=0;Index<3;++Index)
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
        auto P=ParseUCM(TEXT(R"({"buffing":false,"recovery":false,"combat":"magic","looting":false,"damage_type":1024})"));P->SetBoolField(Options[Index],true);
        auto Spells=S->GetArrayField(TEXT("spells"));
        for(int32 Tier:{1,2})
        {
            auto Dot=ParseUCM(TEXT(R"({"name":"Void curse","school":5,"skill":500,"duration":30})"));
            Dot->SetNumberField(TEXT("id"),100+Tier);Dot->SetNumberField(TEXT("category"),Categories[Index]);Dot->SetNumberField(TEXT("power"),Tier*100);
            Spells.Add(MakeShared<FJsonValueObject>(Dot));
        }
        S->SetArrayField(TEXT("spells"),Spells);TSharedPtr<FJsonObject> Intent;TestTrue(*Error,VM.Step(S,P,Intent,Error));
        if(TestTrue(TEXT("Enabled Void curse produces an intent"),Intent.IsValid()))TestEqual(TEXT("Highest usable Void DoT casts first"),Intent->GetNumberField(TEXT("spell")),102.);
        auto Confirmed=ParseUCM(TEXT(R"({"target":500,"power":200,"expires":130})"));Confirmed->SetNumberField(TEXT("category"),Categories[Index]);
        S->SetArrayField(TEXT("debuffs"),{MakeShared<FJsonValueObject>(Confirmed)});S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("action_serial"),1);
        TestTrue(*Error,VM.Step(S,P,Intent,Error));
        if(Intent)TestEqual(TEXT("Confirmed DoT allows direct Nether attacks instead of recasting"),Intent->GetNumberField(TEXT("spell")),20.);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMStackCombiningTest,"ACE.Plugins.StackCombining",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMStackCombiningTest::RunTest(const FString&)
{
    FString Source,Error;
    FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return ParseUCM(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"container":0,"spells":[],"enchantments":[],"targets":[],"inventory":[]})"));};
    auto Profile=[](){return ParseUCM(TEXT(R"({"combat":"off","buffing":false,"recovery":false,"looting":false,"navigation":false,"autostack":true,"idle_peace":false})"));};
    auto Item=[](int32 Id,int32 Count,int32 Class=100){auto I=ParseUCM(TEXT(R"({"name":"Arrow","max_stack":100,"resource_available":true,"container":10})"));
        I->SetNumberField(TEXT("id"),Id);I->SetNumberField(TEXT("wcid"),Class);I->SetNumberField(TEXT("count"),Count);return I;};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto Action=[](auto I){FString A;I->TryGetStringField(TEXT("action"),A);return A;};
    {
        FACEPluginVM VM;TestTrue(TEXT("Stacking policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();
        auto A=Item(10,30),Full=Item(11,100),B=Item(12,20);B->SetNumberField(TEXT("container"),20);
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(A),MakeShared<FJsonValueObject>(Full),MakeShared<FJsonValueObject>(B)});
        auto I=Step(VM,S,P);TestEqual(TEXT("Partial stacks combine across packs"),Action(I),FString(TEXT("merge")));
        TestEqual(TEXT("Full stack between partial stacks is skipped"),I->GetNumberField(TEXT("item")),12.);
        TestEqual(TEXT("Destination is the earlier partial stack"),I->GetNumberField(TEXT("target")),10.);
        S->SetNumberField(TEXT("time"),101);TestTrue(TEXT("Lag does not repeat the merge"),Action(Step(VM,S,P)).IsEmpty());
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),3);
        S->SetNumberField(TEXT("inventory_failure_serial"),1);S->SetNumberField(TEXT("inventory_failure_item"),999);
        TestTrue(TEXT("Unrelated action and inventory failures cannot reject the merge"),Action(Step(VM,S,P)).IsEmpty());
        A->SetNumberField(TEXT("count"),50);S->SetNumberField(TEXT("time"),102);
        TestTrue(TEXT("Target update alone does not release the pending merge"),Action(Step(VM,S,P)).IsEmpty());
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(A),MakeShared<FJsonValueObject>(Full)});
        S->SetNumberField(TEXT("time"),103);
        TestTrue(TEXT("One full and one partial stack are already compact; no ping-pong"),Action(Step(VM,S,P)).IsEmpty());
    }
    for(const TCHAR* Flag:{TEXT("equipped"),TEXT("retained"),TEXT("resource_available")})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto A=Item(10,20),B=Item(11,30);
        B->SetBoolField(Flag,FString(Flag)!=TEXT("resource_available"));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(A),MakeShared<FJsonValueObject>(B)});
        TestTrue(*FString::Printf(TEXT("Protected stack is skipped: %s"),Flag),Action(Step(VM,S,P)).IsEmpty());
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto A=Item(10,20),B=Item(11,30,101);
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(A),MakeShared<FJsonValueObject>(B)});
        TestTrue(TEXT("Matching names with different templates never merge"),Action(Step(VM,S,P)).IsEmpty());
        B->SetNumberField(TEXT("wcid"),100);S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("container"),300);
        TestTrue(TEXT("Open loot container defers inventory stacking"),Action(Step(VM,S,P)).IsEmpty());
        S->SetNumberField(TEXT("container"),0);P->SetBoolField(TEXT("autostack"),false);
        TestTrue(TEXT("Stacking is opt-in"),Action(Step(VM,S,P)).IsEmpty());
        P->SetBoolField(TEXT("autostack"),true);
        TestEqual(TEXT("Enabling the option begins stacking"),Action(Step(VM,S,P)),FString(TEXT("merge")));
        S->SetNumberField(TEXT("time"),117);
        auto Failure=Step(VM,S,P);
        TestEqual(TEXT("Missing confirmation produces a bounded activity failure"),Action(Failure),FString(TEXT("activity_failed")));
        TestEqual(TEXT("Only inventory management is paused"),Failure->GetStringField(TEXT("activity")),FString(TEXT("inventory")));
        S->SetNumberField(TEXT("time"),118);
        TestTrue(TEXT("Timeout does not immediately resend an ambiguous merge"),Action(Step(VM,S,P)).IsEmpty());
        S->SetNumberField(TEXT("time"),148);
        TestTrue(TEXT("Cooldown alone cannot retry an unchanged rejected pair"),Action(Step(VM,S,P)).IsEmpty());
        A->SetNumberField(TEXT("count"),21);B->SetNumberField(TEXT("count"),31);
        S->SetNumberField(TEXT("time"),149);
        TestEqual(TEXT("Changed stacks are eligible again"),Action(Step(VM,S,P)),FString(TEXT("merge")));
    }
    return true;
}
#endif
