#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"
#include "Mods/ACEPluginCombat.h"
#include "ACECreatureFixtures.h"
#include "Engine/GameInstance.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMDetourTest,"ACE.Plugins.AutomaticCombatAndRouteRecovery",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMDetourTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    FString Script,Error;FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto S=Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"teleport_sequence":1,"movement_blocked_serial":0,"position":{"cell":30998795,"x":30,"y":-34.515625,"z":0},"spells":[],"targets":[],"inventory":[{"id":99,"type":1,"equipped":true,"identified":true,"can_wield":true,"weapon_skill":47,"damage":10,"name":"Sword"}]})"));
    auto P=Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","navigation":true,"route":[{"cell":30998795,"x":25,"y":-34.515625,"z":0},{"cell":30998795,"x":45,"y":-34.515625,"z":0}]})"));
    FACEPluginVM VM;TestTrue(TEXT("Policy loads"),VM.Load(Script,Error));
    auto Step=[&](FACEPluginVM& V){TSharedPtr<FJsonObject> I;const bool OK=V.Step(S,P,I,Error);TestTrue(*Error,OK);return I?I:MakeShared<FJsonObject>();};
    // A closer waypoint through a wall is ineligible, regardless of camera.
    S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":true})")));
    TestEqual(TEXT("Join chooses visible waypoint over closer wall-separated point"),Step(VM)->GetNumberField(TEXT("x")),45.);
    S->RemoveField(TEXT("route_visible"));
    const auto Target=Json(TEXT(R"({"id":50,"name":"Red Rat","identified":true,"distance":1,"cell":30998795,"x":40,"y":-40,"z":0,"attack_height":3,"line_of_sight":true})"));
    S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});
    for(FVector2D At:{FVector2D(34.804504,-34.515625),FVector2D(34.804504,-37),FVector2D(34.804504,-40),FVector2D(40,-40)})
    {
        auto Pos=S->GetObjectField(TEXT("position"));Pos->SetNumberField(TEXT("x"),At.X);Pos->SetNumberField(TEXT("y"),At.Y);S->SetNumberField(TEXT("time"),S->GetNumberField(TEXT("time"))+1);
        TestEqual(TEXT("Low creature uses retail low attack value"),Step(VM)->GetNumberField(TEXT("height")),double(ACEAttackHeight::Low));
    }
    S->SetArrayField(TEXT("targets"),{});
    auto I=Step(VM);TestEqual(TEXT("Stops the completed attack before returning"),I->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
    I=Step(VM);TestEqual(TEXT("After kill retraces approach instead of cutting wall to route"),I->GetStringField(TEXT("status")),FString(TEXT("Returning to route")));
    TestEqual(TEXT("Returns around the observed corner"),I->GetNumberField(TEXT("x")),34.804504);
    TestEqual(TEXT("Retains corner depth"),I->GetNumberField(TEXT("y")),-40.);
    for(FVector2D At:{FVector2D(34.804504,-40),FVector2D(34.804504,-37),FVector2D(34.804504,-34.515625)})
    {auto Pos=S->GetObjectField(TEXT("position"));Pos->SetNumberField(TEXT("x"),At.X);Pos->SetNumberField(TEXT("y"),At.Y);I=Step(VM);}
    TestEqual(TEXT("Returns to ordered destination after detour"),I->GetNumberField(TEXT("x")),45.);
    // Strongest usable trained skill wins; damage alone formerly selected a bow.
    P->SetBoolField(TEXT("navigation"),false);P->SetStringField(TEXT("combat"),TEXT("auto"));
    S->SetObjectField(TEXT("skills"),Json(TEXT(R"({"47":{"training":3,"current":146},"48":{"training":2,"current":63}})")));
    auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":100,"type":256,"identified":true,"can_wield":true,"weapon_skill":48,"damage":1000,"name":"Thrown weapon"})"))));S->SetArrayField(TEXT("inventory"),Items);
    S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});
    for(int Height:{1,2,3})
    {Target->SetNumberField(TEXT("attack_height"),Height);FACEPluginVM Combat;Combat.Load(Script,Error);I=Step(Combat);TestEqual(TEXT("Uses strongest trained combat skill"),I->GetNumberField(TEXT("mode")),2.);TestEqual(TEXT("Automatic target height reaches attack intent"),I->GetNumberField(TEXT("height")),double(Height));}
    Items[0]->AsObject()->SetBoolField(TEXT("can_wield"),false);Items[0]->AsObject()->SetBoolField(TEXT("equipped"),false);Items[1]->AsObject()->SetBoolField(TEXT("equipped"),true);
    FACEPluginVM Fallback;Fallback.Load(Script,Error);TestEqual(TEXT("Unavailable best weapon falls back to usable skill"),Step(Fallback)->GetNumberField(TEXT("mode")),4.);
    // A missing terminator may be restored, but actual missing data is rejected.
    const FString Tail=TEXT("UTL\r\n1\r\n0\r\nSalvageCombine\r\n15\r\n1\r\n1-10\r\n0\r\n0");
    auto D=ACEVTProfile::Read(Tail,TEXT("utl"),Error);TestTrue(TEXT("Missing final CRLF on salvage policy accepted"),D.IsValid());
    if(D){TArray<FString> Issues;ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Recovered salvage policy validates"),Issues.Num(),0);}
    TestFalse(TEXT("Truncated policy contents remain rejected"),ACEVTProfile::Read(Tail.LeftChop(3),TEXT("utl"),Error).IsValid());
    auto* GI=NewObject<UGameInstance>();GI->Init();auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    if(TestTrue(TEXT("Retail DAT loads for creature attack heights"),Dat->LoadDatDirectory(ACETestDatDirectory())))
    {
        FACEPosition Player;Player.CellId=0x01D9010B;Player.Location=FVector(34.804504,-34.515625,0);
        int32 Index=0;for(const auto& Model:ACECreatureFixtures::Models)
        {
            FACEWorldObject Creature;ACECreatureFixtures::Apply(Creature,Index);Creature.Position=Player;
            const int32 Height=ACEPluginCombat::AttackHeight(*Dat,Creature,Player);
            AddInfo(FString::Printf(TEXT("%s auto height %d"),Model.Name,Height));
            const int32 Expected[]={ACEAttackHeight::Low,ACEAttackHeight::Medium,ACEAttackHeight::High,
                ACEAttackHeight::Low,ACEAttackHeight::Low,ACEAttackHeight::Low,ACEAttackHeight::Medium};
            static_assert(UE_ARRAY_COUNT(Expected)==UE_ARRAY_COUNT(ACECreatureFixtures::Models));
            TestEqual(*(FString(Model.Name)+TEXT(" selects correct attack quadrant")),Height,Expected[Index++]);
            Creature.Position.Location.Z+=3;
            TestEqual(TEXT("Elevated targets use high attack regardless of species"),ACEPluginCombat::AttackHeight(*Dat,Creature,Player),int32(ACEAttackHeight::High));
        }
    }
    GI->Shutdown();
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMElementSelectionTest,"ACE.Plugins.ElementAndPowerSelection",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMElementSelectionTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto S=Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"spells":[],"skills":{"44":{"training":3,"current":300}},"targets":[{"id":50,"name":"Rat","identified":true,"distance":1,"attack_height":3,"line_of_sight":true,"resists":{"16":0.5,"8":2}}],"inventory":[{"id":90,"name":"Flaming sword","type":1,"weapon_skill":44,"identified":true,"can_wield":true,"equipped":true,"equipped_slot":1048576,"damage_type":16,"damage":30},{"id":91,"name":"Cold sword","type":1,"weapon_skill":44,"identified":true,"can_wield":true,"damage_type":8,"damage":30}]})"));
    auto P=Json(TEXT(R"({"buffing":false,"recovery":false,"looting":false,"combat":"auto"})"));
    auto Step=[&](){FACEPluginVM VM;TestTrue(TEXT("Combat policy loads"),VM.Load(Source,Error));TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    TestEqual(TEXT("Cold vulnerability wins over equipped fire sword"),Step()->GetNumberField(TEXT("item")),91.);
    auto Items=S->GetArrayField(TEXT("inventory"));Items[0]->AsObject()->SetBoolField(TEXT("equipped"),false);Items[1]->AsObject()->SetBoolField(TEXT("equipped"),true);
    Items[1]->AsObject()->SetNumberField(TEXT("equipped_slot"),ACEEquipMask::MeleeWeapon);
    auto Attack=Step();TestEqual(TEXT("Chosen elemental sword uses automatic full power by default"),Attack->GetNumberField(TEXT("power")),1.);
    TestEqual(TEXT("Rat is attacked low on the wire"),Attack->GetNumberField(TEXT("height")),double(ACEAttackHeight::Low));
    auto Target=S->GetArrayField(TEXT("targets"))[0]->AsObject();Target->SetObjectField(TEXT("resists"),Json(TEXT(R"({"16":2,"8":0.5})")));
    TestEqual(TEXT("Different target resistances switch back to fire sword"),Step()->GetNumberField(TEXT("item")),90.);
    Target->SetArrayField(TEXT("preferred_elements"),ACEPluginCombat::PreferredElements(TEXT("Snow Tusker")));
    Target->SetObjectField(TEXT("resists"),Json(TEXT("{}")));
    TestEqual(TEXT("Stock server without resistance fields uses VT Snow Tusker fire preference"),Step()->GetNumberField(TEXT("item")),90.);
    Target->SetObjectField(TEXT("resists"),Json(TEXT(R"({"16":0.5,"8":2})")));
    TestEqual(TEXT("Custom server resistance data takes precedence over stock catalog"),Step()->GetStringField(TEXT("action")),FString(TEXT("attack")));
    TestTrue(TEXT("Unknown monsters have no invented elemental preferences"),ACEPluginCombat::PreferredElements(TEXT("Unregistered Custom Monster")).IsEmpty());
    Target->SetObjectField(TEXT("resists"),Json(TEXT(R"({"1":0.5,"2":2})")));
    S->SetArrayField(TEXT("inventory"),{Items[1]});auto Weapon=Items[1]->AsObject();Weapon->SetNumberField(TEXT("damage_type"),3);
    TestEqual(TEXT("Dual-damage sword uses thrust power for piercing weakness"),Step()->GetNumberField(TEXT("power")),.2);
    Target->SetObjectField(TEXT("resists"),Json(TEXT(R"({"1":2,"2":0.5})")));Weapon->SetNumberField(TEXT("weapon_type"),1);
    TestEqual(TEXT("Dagger uses VT's half-power slash instead of thrust"),Step()->GetNumberField(TEXT("power")),.5);
    P->SetBoolField(TEXT("auto_attack_power"),false);P->SetNumberField(TEXT("power"),.7);
    TestEqual(TEXT("Explicit manual power remains supported"),Step()->GetNumberField(TEXT("power")),.7);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMMonsterDamageTest,"ACE.Plugins.MonsterDamageOverrides",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMMonsterDamageTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"combat_mode":8,"components_required":false,"spells":[],"targets":[{"id":500,"name":"Rat","identified":true,"distance":1,"resists":{"16":0.5,"8":2}}],"inventory":[]})"));};
    auto Profile=[&](){return Json(TEXT(R"({"buffing":false,"recovery":false,"looting":false,"combat":"auto","damage_type":8,"monsters":[{"name":"Rat","damage_type":16}]})"));};
    auto Step=[&](auto S,auto P){FACEPluginVM VM;TestTrue(TEXT("Monster policy loads"),VM.Load(Source,Error));TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto Items=[&](auto S,const TCHAR* Value){S->SetArrayField(TEXT("inventory"),Json(Value)->GetArrayField(TEXT("items")));};
    for(bool Explicit:{false,true})
    {
        auto S=Snapshot(),P=Profile();
        Items(S,TEXT(R"({"items":[{"id":90,"name":"Cold sword","type":1,"damage_type":8,"damage":100,"equipped":true,"equipped_slot":1048576,"identified":true,"can_wield":true},{"id":91,"name":"Fire sword","type":1,"damage_type":16,"damage":20,"identified":true,"can_wield":true}]})"));
        if(Explicit)P->GetArrayField(TEXT("monsters"))[0]->AsObject()->SetNumberField(TEXT("weapon"),90);
        else P->SetNumberField(TEXT("melee_weapon"),90);
        TestEqual(TEXT("Monster fire overrides incompatible weapon preference and resistance"),Step(S,P)->GetNumberField(TEXT("item")),91.);
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv[0]->AsObject()->SetBoolField(TEXT("equipped"),false);Inv[1]->AsObject()->SetBoolField(TEXT("equipped"),true);
        TestEqual(TEXT("Confirmed fire weapon attacks"),Step(S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Inv[0]->AsObject()->SetBoolField(TEXT("equipped"),true);Inv[0]->AsObject()->SetNumberField(TEXT("equipped_slot"),ACEEquipMask::Shield);
        auto I=Step(S,P);TestEqual(TEXT("Incompatible offhand produces equipment action"),I->GetStringField(TEXT("action")),FString(TEXT("equip")));
        TestTrue(TEXT("Incompatible offhand is removed before attacking"),I->GetBoolField(TEXT("unequip")));
        Inv[0]->AsObject()->SetBoolField(TEXT("equipped"),false);Inv[1]->AsObject()->SetNumberField(TEXT("damage_type"),0);
        TestEqual(TEXT("Unknown or wrong damage never silently attacks"),Step(S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    }
    {
        auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("combat"),TEXT("magic"));P->SetNumberField(TEXT("attack_spell"),10);
        Items(S,TEXT(R"({"items":[{"id":90,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}]})"));
        S->SetArrayField(TEXT("spells"),Json(TEXT(R"({"items":[{"id":10,"name":"Cold bolt","category":119,"school":1,"power":200,"skill":300},{"id":11,"name":"Fire bolt","category":121,"school":1,"power":100,"skill":300}]})"))->GetArrayField(TEXT("items")));
        TestEqual(TEXT("Monster fire chooses fire spell over global cold spell"),Step(S,P)->GetNumberField(TEXT("spell")),11.);
        P->SetArrayField(TEXT("monsters"),{});
        TestEqual(TEXT("Unmatched monsters retain global spell selection"),Step(S,P)->GetNumberField(TEXT("spell")),10.);
    }
    for(int32 AmmoType:{1,2,4})
    {
        auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("combat"),TEXT("missile"));
        Items(S,TEXT(R"({"items":[{"id":90,"name":"Launcher","type":256,"damage_type":2,"damage_mod":2,"identified":true,"can_wield":true,"equipped":true},{"id":91,"name":"Cold ammo","type":256,"slots":8388608,"damage_type":8,"damage":100,"identified":true,"can_wield":true,"equipped":true},{"id":92,"name":"Fire ammo","type":256,"slots":8388608,"damage_type":16,"damage":20,"identified":true,"can_wield":true}]})"));
        for(auto V:S->GetArrayField(TEXT("inventory")))V->AsObject()->SetNumberField(TEXT("ammo_type"),AmmoType);
        TestEqual(TEXT("All launcher families switch to monster-requested ammunition"),Step(S,P)->GetNumberField(TEXT("item")),92.);
        S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetBoolField(TEXT("equipped"),true);
        TestEqual(TEXT("Confirmed matching ammo permits attack"),Step(S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetBoolField(TEXT("can_wield"),false);
        TestEqual(TEXT("Missing eligible fire ammo never fires cold ammo"),Step(S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    }
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMBlockedRouteTest,"ACE.Plugins.BlockedRouteSafety",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMBlockedRouteTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    FString Script,Error;FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto S=Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"teleport_sequence":1,"movement_blocked_serial":0,"position":{"cell":30998795,"x":30,"y":-34,"z":0},"spells":[],"targets":[],"inventory":[{"id":99,"type":1,"equipped":true,"identified":true,"can_wield":true,"weapon_skill":47,"damage":10,"name":"Sword"}]})"));
    auto P=Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","navigation":true,"route":[{"cell":30998795,"x":45,"y":-34,"z":0}]})"));
    auto Step=[&](FACEPluginVM& VM){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto Action=[&](FACEPluginVM& VM){FString A;Step(VM)->TryGetStringField(TEXT("action"),A);return A;};
    FACEPluginVM Blocked;TestTrue(TEXT("Policy loads"),Blocked.Load(Script,Error));
    TestEqual(TEXT("Route initially moves"),Action(Blocked),FString(TEXT("move")));
    for(int N=1;N<=3;++N)
    {
        S->SetNumberField(TEXT("movement_blocked_serial"),N);
        TestEqual(TEXT("Exhausted movement retries pause only navigation"),Action(Blocked),FString(N==3?TEXT("pause_navigation"):TEXT("move")));
    }
    P->SetBoolField(TEXT("navigation"),false); // Host applies the pause to the shared UI switch.
    P->SetArrayField(TEXT("states"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"name":"Default","navigation":true})")))});
    TestTrue(TEXT("A native state cannot override the safety pause"),Action(Blocked).IsEmpty());
    auto Target=Json(TEXT(R"({"id":50,"name":"Red Rat","identified":true,"distance":1,"cell":30998795,"x":31,"y":-34,"z":0,"attack_height":1,"line_of_sight":true})"));
    S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});
    TestEqual(TEXT("Combat remains active after a blocked route"),Action(Blocked),FString(TEXT("attack")));
    P->SetBoolField(TEXT("recovery"),true);S->SetNumberField(TEXT("health"),10);
    auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":43,"name":"Health potion","usable":true,"identified":true,"boost_vital":2,"boost":100,"count":1})"))));S->SetArrayField(TEXT("inventory"),Items);
    TestEqual(TEXT("Recovery remains active after a blocked route"),Action(Blocked),FString(TEXT("use_item")));
    P->SetBoolField(TEXT("recovery"),false);S->SetNumberField(TEXT("health"),100);S->SetArrayField(TEXT("targets"),{});
    Step(Blocked); // Finish/cancel the previous combat/recovery action.
    P->SetBoolField(TEXT("navigation"),true);
    TestEqual(TEXT("Explicit navigation toggle resumes the route"),Action(Blocked),FString(TEXT("move")));
    P->RemoveField(TEXT("states"));

    FACEPluginVM Doors;Doors.Load(Script,Error);S->SetNumberField(TEXT("movement_blocked_serial"),0);
    auto Door=Json(TEXT(R"({"id":77,"object_class":26,"door_open":false,"distance":1,"cell":30998795,"x":31,"y":-34,"z":0})"));
    S->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(Door)});
    TestEqual(TEXT("Opens a closed route door"),Action(Doors),FString(TEXT("use_world")));
    TestEqual(TEXT("Door retries respect cooldown"),Action(Doors),FString(TEXT("move")));
    Door->SetBoolField(TEXT("door_open"),true);S->SetNumberField(TEXT("time"),106);
    TestEqual(TEXT("Does not close an already open door"),Action(Doors),FString(TEXT("move")));
    Door->SetBoolField(TEXT("door_open"),false);
    TestEqual(TEXT("Reopens a previously used door after it closes"),Action(Doors),FString(TEXT("use_world")));
    for(int N=1;N<=3;++N)
    {S->SetNumberField(TEXT("time"),106+N*6);TestEqual(TEXT("A persistently closed door has bounded retries"),Action(Doors),FString(N==3?TEXT("pause_navigation"):TEXT("use_world")));}

    {
        auto OriginalItems=S->GetArrayField(TEXT("inventory"));
        S->SetNumberField(TEXT("time"),150);Door->SetBoolField(TEXT("door_identified"),false);
        FACEPluginVM Lock;Lock.Load(Script,Error);
        TestEqual(TEXT("Closed route door is assessed before choosing key or use"),Action(Lock),FString(TEXT("identify")));
        Door->SetBoolField(TEXT("door_identified"),true);Door->SetBoolField(TEXT("door_locked"),true);Door->SetNumberField(TEXT("lock_difficulty"),200);
        S->SetObjectField(TEXT("skills"),Json(TEXT(R"({"23":{"training":2,"current":210}})")));P->SetNumberField(TEXT("door_lockpick_margin"),10);
        S->SetArrayField(TEXT("inventory"),Json(TEXT(R"({"v":[{"id":81,"name":"Pick","object_class":30,"usable":true,"structure":15},{"id":82,"name":"Pick","object_class":30,"usable":true,"structure":2},{"id":83,"name":"Other","object_class":30,"usable":true,"structure":1},{"id":84,"name":"Pick","object_class":30,"usable":true,"structure":0}]})"))->GetArrayField(TEXT("v")));
        P->SetArrayField(TEXT("assist_items"),Json(TEXT(R"({"v":[{"name":"Pick","type":10}]})"))->GetArrayField(TEXT("v")));
        S->SetNumberField(TEXT("time"),156);auto Unlock=Step(Lock);
        TestEqual(TEXT("Uses a lockpick instead of repeatedly using a locked door"),Unlock->GetStringField(TEXT("action")),FString(TEXT("apply_item")));
        TestEqual(TEXT("Consumes configured pick with fewest remaining uses"),Unlock->GetNumberField(TEXT("item")),82.);
        TestEqual(TEXT("Pick targets the obstructing door"),Unlock->GetNumberField(TEXT("target")),77.);
        S->SetNumberField(TEXT("time"),162);Door->SetBoolField(TEXT("door_locked"),false);
        TestEqual(TEXT("Confirmed unlock is followed by normal door use"),Action(Lock),FString(TEXT("use_world")));
        Door->SetBoolField(TEXT("door_locked"),true);S->GetObjectField(TEXT("skills"))->GetObjectField(TEXT("23"))->SetNumberField(TEXT("current"),209);
        FACEPluginVM TooHard;TooHard.Load(Script,Error);TestEqual(TEXT("Insufficient lockpick margin pauses only route"),Action(TooHard),FString(TEXT("pause_navigation")));
        S->GetObjectField(TEXT("skills"))->GetObjectField(TEXT("23"))->SetNumberField(TEXT("current"),210);
        for(const auto& Pick:S->GetArrayField(TEXT("inventory")))Pick->AsObject()->SetBoolField(TEXT("lockpick_available"),false);
        FACEPluginVM NoPicks;NoPicks.Load(Script,Error);TestEqual(TEXT("Unavailable picks never start an unlock loop"),Action(NoPicks),FString(TEXT("pause_navigation")));
        S->SetArrayField(TEXT("inventory"),OriginalItems);P->RemoveField(TEXT("assist_items"));P->RemoveField(TEXT("door_lockpick_margin"));
        Door->RemoveField(TEXT("door_identified"));Door->RemoveField(TEXT("door_locked"));
    }

    // A door that closes during an off-route fight blocks the recorded return path.
    // Keep the detour off the active segment: an on-route fight now resumes forward.
    FACEPluginVM Return;Return.Load(Script,Error);S->SetArrayField(TEXT("route_objects"),{});S->SetNumberField(TEXT("time"),200);
    Step(Return);S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Target)});
    Step(Return);S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),34);
    S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("y"),-30);Step(Return);
    S->SetArrayField(TEXT("targets"),{});Step(Return); // Cancel attack.
    Door->SetNumberField(TEXT("x"),33);Door->SetNumberField(TEXT("y"),-31);Door->SetBoolField(TEXT("door_open"),false);
    S->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(Door)});
    TestEqual(TEXT("Return from combat checks doors too"),Action(Return),FString(TEXT("use_world")));
    TestEqual(TEXT("Return continues during the door retry cooldown"),Action(Return),FString(TEXT("move")));
    S->SetNumberField(TEXT("movement_blocked_serial"),1);
    TestEqual(TEXT("Blocked combat return pauses navigation rather than UCM"),Action(Return),FString(TEXT("pause_navigation")));
    return true;
}
#endif
