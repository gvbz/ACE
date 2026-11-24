#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootHandoffTest,"ACE.Plugins.LootSalvageHandoff",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootHandoffTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    FString Source,Error;
    FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    FACEPluginVM VM;if(!TestTrue(TEXT("UCM policy loads"),VM.Load(Source,Error)))return false;
    auto S=Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"contents_ready":true,"position":{"cell":30998795,"x":30,"y":-34,"z":0},"spells":[],"targets":[],"inventory":[{"id":9,"object_class":40,"salvage_tool_available":true}],"corpses":[{"id":700,"name":"First corpse","distance":1,"identified":true},{"id":701,"name":"Second corpse","distance":2,"identified":true}]})"));
    auto P=Json(TEXT(R"({"combat":"off","buffing":false,"recovery":false,"looting":true,"idle_peace":false,"loot_rules":[{"action":"salvage","material":59}],"navigation":true,"route":[{"cell":30998795,"x":45,"y":-34,"z":0}]})"));
    auto Tool=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Tool->SetNumberField(TEXT("wcid"),900);Tool->SetStringField(TEXT("name"),TEXT("Ust"));Tool->SetNumberField(TEXT("count"),1);
    auto Step=[&]()
    {
        S->SetNumberField(TEXT("time"),S->GetNumberField(TEXT("time"))+.05);
        TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();
    };
    auto Action=[](auto I){FString A;I->TryGetStringField(TEXT("action"),A);return A;};
    auto Next=Step();
    for(int32 Corpse:{700,701})
    {
        TestEqual(TEXT("Next corpse starts on the next eligible decision"),Action(Next),FString(TEXT("open_corpse")));
        TestEqual(TEXT("Completed corpse is not reopened"),Next->GetNumberField(TEXT("item")),double(Corpse));
        TestTrue(TEXT("No repeated open while waiting for the server"),Action(Step()).IsEmpty());
        S->SetNumberField(TEXT("container"),Corpse);
        auto Item=Json(TEXT(R"({"id":20,"wcid":200,"name":"Copper Ring","material":59,"workmanship":7.5,"count":1,"identified":true,"resource_available":true})"));
        Item->SetNumberField(TEXT("id"),Corpse+100);Item->SetNumberField(TEXT("container"),Corpse);
        S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Item)});
        TestEqual(TEXT("Loot rule takes the matched item"),Action(Step()),FString(TEXT("loot")));
        TestTrue(TEXT("No repeated pickup before ownership confirmation"),Action(Step()).IsEmpty());
        auto Inventory=S->GetArrayField(TEXT("inventory"));Item->SetNumberField(TEXT("container"),1);
        Inventory.Add(MakeShared<FJsonValueObject>(Item));S->SetArrayField(TEXT("inventory"),Inventory);S->SetArrayField(TEXT("contents"),{});
        auto Close=Step();TestEqual(TEXT("Confirmed pickup closes the completed corpse"),Action(Close),FString(TEXT("close_corpse")));
        TestTrue(TEXT("Close requests a prompt continuation"),Close->GetBoolField(TEXT("continue_work")));
        S->SetNumberField(TEXT("container"),0);
        auto Salvage=Step();TestEqual(TEXT("Queued loot is salvaged immediately after close"),Action(Salvage),FString(TEXT("salvage")));
        TestEqual(TEXT("Only the confirmed received item is salvaged"),Salvage->GetNumberField(TEXT("item")),double(Corpse+100));
        S->SetNumberField(TEXT("action_serial"),Corpse);
        TestTrue(TEXT("Unrelated successful use cannot confirm salvage"),Action(Step()).IsEmpty());
        Inventory.RemoveAt(1);S->SetArrayField(TEXT("inventory"),Inventory);
        Next=Step(); // authoritative removal; no three-second simulated time jump
    }
    TestEqual(TEXT("Final salvage hands back to navigation"),Action(Next),FString(TEXT("move")));
    TestTrue(TEXT("Policy handoffs require no elapsed multi-second delays"),S->GetNumberField(TEXT("time"))<102.);
    // ACE can report TooBusy on the world Use, or reject an inventory move with
    // SaveFailed(item, 0) after emitting the busy message on a different packet.
    S->SetNumberField(TEXT("time"),200);S->SetNumberField(TEXT("action_serial"),0);
    P->SetBoolField(TEXT("navigation"),false);P->SetNumberField(TEXT("corpse_attempts"),1);
    P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"action":"keep"})")))});
    S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":710,"name":"Busy corpse","identified":true,"distance":1})")))});
    TestEqual(TEXT("Starts opening busy corpse"),Action(Step()),FString(TEXT("open_corpse")));
    S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"use","source":710,"action_serial":1,"requested_at":200.1,"error":29})")))});
    S->SetNumberField(TEXT("action_serial"),1);
    TestTrue(TEXT("Busy open waits briefly"),Action(Step()).IsEmpty());
    S->SetNumberField(TEXT("time"),201);
    TestEqual(TEXT("Busy did not consume corpse blacklist allowance"),Action(Step()),FString(TEXT("open_corpse")));
    S->SetNumberField(TEXT("container"),710);
    auto Pea=MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":801,"wcid":501,"name":"Gold Pea","count":1,"identified":true})")));
    auto Coin=MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":802,"wcid":502,"name":"Coin","count":1,"identified":true})")));
    S->SetArrayField(TEXT("contents"),{Pea,Coin});
    TestEqual(TEXT("Starts pickup"),Step()->GetNumberField(TEXT("item")),801.);
    S->SetNumberField(TEXT("inventory_failure_serial"),1);S->SetNumberField(TEXT("inventory_failure_item"),999);
    TestTrue(TEXT("Unrelated failure cannot retry the pickup"),Action(Step()).IsEmpty());
    for(int Attempt=1;Attempt<=3;++Attempt)
    {
        S->SetNumberField(TEXT("inventory_failure_serial"),Attempt+1);S->SetNumberField(TEXT("inventory_failure_item"),801);
        S->SetNumberField(TEXT("inventory_failure_error"),Attempt==1?0:0x1d);
        TestTrue(TEXT("Repeated busy replies respect retry spacing"),Action(Step()).IsEmpty());
        S->SetNumberField(TEXT("time"),202+Attempt);
        auto Retry=Step();
        TestEqual(TEXT("Inventory rejection stays within looting"),Action(Retry),FString(TEXT("loot")));
        TestEqual(TEXT("Retry same item, then try other contents"),Retry->GetNumberField(TEXT("item")),Attempt<3?801.:802.);
    }
    auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(Coin);S->SetArrayField(TEXT("inventory"),Inventory);S->SetArrayField(TEXT("contents"),{Pea});
    auto Incomplete=Step();
    TestEqual(TEXT("Temporarily releases corpse after other loot"),Action(Incomplete),FString(TEXT("close_corpse")));
    TestFalse(TEXT("A rejected pickup never marks a corpse complete"),Incomplete->HasField(TEXT("completed")));
    S->SetNumberField(TEXT("container"),0);S->SetNumberField(TEXT("time"),236);
    TestEqual(TEXT("Returns for the remaining item without restarting UCM"),Action(Step()),FString(TEXT("open_corpse")));
    S->SetNumberField(TEXT("container"),710);
    TestEqual(TEXT("Rechecks outstanding item"),Step()->GetNumberField(TEXT("item")),801.);
    S->SetNumberField(TEXT("time"),247);
    TestTrue(TEXT("Unacknowledged transfer waits for late ownership"),Action(Step()).IsEmpty());
    S->SetNumberField(TEXT("time"),248);
    TestEqual(TEXT("Unchanged corpse item retries after a missing packet"),Step()->GetNumberField(TEXT("item")),801.);
    S->SetNumberField(TEXT("inventory_failure_serial"),5);Step();
    Inventory.Add(Pea);S->SetArrayField(TEXT("inventory"),Inventory);S->SetArrayField(TEXT("contents"),{});
    auto Received=Step();TestTrue(TEXT("Late delivery wins over the scheduled retry"),Received->GetBoolField(TEXT("completed")));
    TestTrue(TEXT("Only confirmed pickup produces a notice"),Received->HasField(TEXT("loot_notification")));
    return !HasAnyErrors();
}
#endif
