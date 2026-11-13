#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"
namespace
{
TSharedPtr<FJsonObject> RestockJSON(const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMRestockTest,"ACE.Plugins.VendorRestock",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMRestockTest::RunTest(const FString&)
{
    FString Script,Error;FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto S=RestockJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"action_serial":0,"world_name":"Test","vendor":2,"vendor_name":"Arcanist","vendor_wcid":200,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"inventory":[{"id":9,"wcid":300,"name":"Scarab","count":7}],"vendor_stock":[{"id":3,"wcid":300,"name":"Scarab","limit":100}]})"));
    auto P=RestockJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","vendor_restock":true,"vendor_rules":[{"server":"Test","vendor_name":"Arcanist","vendor_wcid":200,"item_name":"Scarab","item_wcid":300,"quantity":10}]})"));
    TSharedPtr<FJsonObject> I;
    auto Step=[&](FACEPluginVM& V){const bool OK=V.Step(S,P,I,Error);TestTrue(*Error,OK);if(!I)I=MakeShared<FJsonObject>();return I;};
    FACEPluginVM VM;TestTrue(TEXT("Policy loads"),VM.Load(Script,Error));
    Step(VM);TestEqual(TEXT("Buys missing quantity only"),I->GetNumberField(TEXT("count")),3.);TestEqual(TEXT("Uses current stock GUID"),I->GetNumberField(TEXT("item")),3.);
    S->SetNumberField(TEXT("time"),101);Step(VM);TestFalse(TEXT("No duplicate purchase before inventory update"),I->HasField(TEXT("action")));
    S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("count"),10);Step(VM);TestFalse(TEXT("Satisfied quantity does not buy"),I->HasField(TEXT("action")));
    FACEPluginVM Relog;Relog.Load(Script,Error);S->SetNumberField(TEXT("vendor"),22);S->GetArrayField(TEXT("vendor_stock"))[0]->AsObject()->SetNumberField(TEXT("id"),33);
    S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("count"),8);Step(Relog);TestEqual(TEXT("New session uses new vendor GUID"),I->GetNumberField(TEXT("vendor")),22.);
    TestEqual(TEXT("New session uses new stock GUID"),I->GetNumberField(TEXT("item")),33.);
    S->SetNumberField(TEXT("time"),117);Step(Relog);TestEqual(TEXT("Unconfirmed purchase pauses its activity rather than rebuying"),I->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    FACEPluginVM WrongServer;WrongServer.Load(Script,Error);S->SetStringField(TEXT("world_name"),TEXT("Other"));Step(WrongServer);TestFalse(TEXT("Profiles cannot buy on another server"),I->HasField(TEXT("action")));
    S->SetStringField(TEXT("world_name"),TEXT("Test"));P->SetBoolField(TEXT("vendor_restock"),false);FACEPluginVM Disabled;Disabled.Load(Script,Error);Step(Disabled);TestFalse(TEXT("Toggle disables buying"),I->HasField(TEXT("action")));
    {
        const auto SavedS=S,SavedP=P;
        S=RestockJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"world_name":"Test","vendor":2,"vendor_name":"Arcanist","vendor_wcid":200,"vendor_uses_pyreals":true,"pyreals":100,"action_serial":0,"inventory":[{"id":9,"wcid":300,"name":"Scarab","count":7},{"id":40,"wcid":400,"name":"Trade Note","count":10}],"vendor_stock":[{"id":3,"wcid":300,"name":"Scarab","limit":100,"unit_value":1000,"sell_rate":1.2}],"vendor_trade_notes":[{"id":40,"wcid":400,"name":"Trade Note","count":10,"unit_value":1000}]})"));
        P=RestockJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","vendor_restock":true,"vendor_rules":[{"server":"Test","vendor_name":"Arcanist","vendor_wcid":200,"item_name":"Scarab","item_wcid":300,"quantity":10}]})"));
        FACEPluginVM Redeem;TestTrue(TEXT("Trade-note policy loads"),Redeem.Load(Script,Error));
        Step(Redeem);TestEqual(TEXT("Partial redemption splits first"),I->GetStringField(TEXT("action")),FString(TEXT("split_note")));
        TestEqual(TEXT("Only four notes cover the 3500 shortfall"),I->GetNumberField(TEXT("count")),4.);
        S->SetNumberField(TEXT("time"),101);Step(Redeem);TestFalse(TEXT("No repeated split while waiting"),I->HasField(TEXT("action")));
        auto NewNote=RestockJSON(TEXT(R"({"id":41,"wcid":400,"name":"Trade Note","count":4,"unit_value":1000})"));
        auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory[1]->AsObject()->SetNumberField(TEXT("count"),6);Inventory.Add(MakeShared<FJsonValueObject>(NewNote));S->SetArrayField(TEXT("inventory"),Inventory);
        auto Notes=S->GetArrayField(TEXT("vendor_trade_notes"));Notes[0]->AsObject()->SetNumberField(TEXT("count"),6);Notes.Add(MakeShared<FJsonValueObject>(NewNote));S->SetArrayField(TEXT("vendor_trade_notes"),Notes);
        Step(Redeem);TestEqual(TEXT("Sells the confirmed new stack"),I->GetNumberField(TEXT("item")),41.);TestEqual(TEXT("Confirmed split becomes redemption"),I->GetStringField(TEXT("action")),FString(TEXT("sell_note")));
        S->SetNumberField(TEXT("pyreals"),4100);Step(Redeem);TestFalse(TEXT("Money arriving first cannot cause a duplicate sale or premature buy"),I->HasField(TEXT("action")));
        Inventory.Pop();Notes.Pop();S->SetArrayField(TEXT("inventory"),Inventory);S->SetArrayField(TEXT("vendor_trade_notes"),Notes);
        Step(Redeem);TestEqual(TEXT("Restocking resumes after sale replication"),I->GetStringField(TEXT("action")),FString(TEXT("buy")));TestEqual(TEXT("Original missing quantity preserved"),I->GetNumberField(TEXT("count")),3.);
        Step(Redeem);TestFalse(TEXT("Purchase waits for inventory confirmation"),I->HasField(TEXT("action")));
        S->SetNumberField(TEXT("pyreals"),100);S->GetArrayField(TEXT("inventory"))[1]->AsObject()->SetNumberField(TEXT("count"),1);S->GetArrayField(TEXT("vendor_trade_notes"))[0]->AsObject()->SetNumberField(TEXT("count"),1);
        FACEPluginVM Poor;Poor.Load(Script,Error);Step(Poor);TestEqual(TEXT("Insufficient combined funds do not liquidate notes pointlessly"),I->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        S->GetArrayField(TEXT("vendor_trade_notes"))[0]->AsObject()->SetNumberField(TEXT("unit_value"),5000);
        FACEPluginVM Whole;Whole.Load(Script,Error);Step(Whole);TestEqual(TEXT("Whole note sells without splitting"),I->GetStringField(TEXT("action")),FString(TEXT("sell_note")));
        S->SetNumberField(TEXT("vendor"),99);Step(Whole);TestFalse(TEXT("Changing vendor cancels pending redemption"),I->HasField(TEXT("action")));S->SetNumberField(TEXT("vendor"),2);
        S->SetBoolField(TEXT("vendor_uses_pyreals"),false);FACEPluginVM Alternate;Alternate.Load(Script,Error);Step(Alternate);TestEqual(TEXT("Alternate currencies never sell notes"),I->GetStringField(TEXT("action")),FString(TEXT("buy")));
        S->SetBoolField(TEXT("vendor_uses_pyreals"),true);FACEPluginVM Timeout;Timeout.Load(Script,Error);Step(Timeout);S->SetNumberField(TEXT("time"),120);Step(Timeout);TestEqual(TEXT("No confirmation pauses its activity instead of repeatedly selling notes"),I->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        auto Rules=P->GetArrayField(TEXT("vendor_rules"));Rules.Add(MakeShared<FJsonValueObject>(RestockJSON(TEXT(R"({"server":"Test","vendor_name":"Arcanist","vendor_wcid":200,"item_name":"Trade Note","item_wcid":400,"quantity":10})"))));P->SetArrayField(TEXT("vendor_rules"),Rules);
        FACEPluginVM Reserved;Reserved.Load(Script,Error);Step(Reserved);TestEqual(TEXT("Notes configured for restocking cannot be sold in a buy/sell loop"),I->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        S=SavedS;P=SavedP;
    }
    // Exemption bypasses supplies, not skill/known checks or server failures.
    P=RestockJSON(TEXT(R"({"buffing":true,"combat":"off","recovery":false,"buff_skill_margin":0})"));
    S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(RestockJSON(TEXT(R"({"id":99,"name":"Orb","type":32768,"equipped":true})")))});S->SetNumberField(TEXT("vendor"),0);S->SetNumberField(TEXT("combat_mode"),8);
    auto Spell=RestockJSON(TEXT(R"({"id":10,"name":"Buff","power":100,"skill":300,"category":1,"self_buff":true,"caster_target":true,"components_known":false,"scarabs":{"Scarab":1}})"));S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Spell)});
    S->SetBoolField(TEXT("components_required"),false);FACEPluginVM Admin;Admin.Load(Script,Error);Step(Admin);TestEqual(TEXT("Admin can buff without components"),I->GetStringField(TEXT("action")),FString(TEXT("cast")));
    Spell->SetBoolField(TEXT("components_known"),true);
    S->SetBoolField(TEXT("components_required"),true);FACEPluginVM Normal;Normal.Load(Script,Error);Step(Normal);
    FString NormalAction;I->TryGetStringField(TEXT("action"),NormalAction);
    TestFalse(TEXT("Normal character skips casts without required components"),NormalAction==TEXT("cast"));
    auto Supplied=S->GetArrayField(TEXT("inventory"));Supplied.Add(MakeShared<FJsonValueObject>(RestockJSON(TEXT(R"({"id":100,"name":"Scarab","count":1})"))));S->SetArrayField(TEXT("inventory"),Supplied);
    FACEPluginVM Restocked;Restocked.Load(Script,Error);Step(Restocked);TestEqual(TEXT("Normal character can cast after restocking components"),I->GetStringField(TEXT("action")),FString(TEXT("cast")));
    TArray<FString> Issues;
    auto Command=ACEVTProfile::CompileCommand(TEXT("/og summon off"),Issues);TestEqual(TEXT("Octagram pet command has adapter"),Issues.Num(),0);
    TestEqual(TEXT("Canonical command uses client namespace"),Command->GetStringField(TEXT("source_command")),FString(TEXT("/acueucm opt set SummonPets false")));
    TestFalse(TEXT("Pet summoning disabled"),Command->GetBoolField(TEXT("value")));
    ACEVTProfile::CompileCommand(TEXT("/og unsupported"),Issues);TestTrue(TEXT("Unknown commands still rejected"),!Issues.IsEmpty());
    auto D=RestockJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["AutoFellowManagement",false],["DoJiggle",false],["ManaChargesWhenOff",true],["ManaStoneLootCount",8],["ManaTankMinimumMana",3000],["GhostMonsterSpellAttemptCount",20],["StaminaToHealthMultiplier",0.5],["GhostDeleteHPTrackerSeconds",600]]}]})"));
    auto Converted=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Reported companion settings convert"),Issues.Num(),0);
    TestEqual(TEXT("Recovery multiplier retained"),Converted->GetNumberField(TEXT("stamina_health_multiplier")),.5);
    TestTrue(TEXT("Off-state recharge enabled"),Converted->GetBoolField(TEXT("mana_charges_when_off")));
    // Idle-only execution cannot run a meta, navigation, or combat.
    P=RestockJSON(TEXT(R"({"ucm_mana_only":true,"buffing":true,"combat":"magic","navigation":true,"item_mana":true,"mana_tank_minimum":3000})"));
    S->SetArrayField(TEXT("inventory"),RestockJSON(TEXT(R"({"v":[{"id":20,"name":"Gear","equipped":true,"mana":1,"max_mana":100},{"id":21,"name":"Stone","type":524288,"identified":true,"usable":true,"mana_supply_available":true,"mana":5000,"workmanship":1,"wcid":1}]})"))->GetArrayField(TEXT("v")));
    FACEPluginVM Idle;Idle.Load(Script,Error);Step(Idle);TestEqual(TEXT("Idle recharge uses stone"),I->GetNumberField(TEXT("item")),21.);
    Step(Idle);TestFalse(TEXT("Idle recharge waits for observation"),I->HasField(TEXT("action")));
    S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("mana"),90);Step(Idle);TestFalse(TEXT("Idle-only policy never falls through to combat or buffs"),I->HasField(TEXT("action")));
    return true;
}
#endif
