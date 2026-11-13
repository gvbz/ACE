#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMCombineRecoveryTest, "ACE.Plugins.CombineFailureRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEUCMCombineRecoveryTest::RunTest(const FString&)
{
    auto Json = [](const TCHAR* Text) { TSharedPtr<FJsonObject> O; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O); return O; };
    FString Source, Error;
    FFileHelper::LoadFileToString(Source, *(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir() / TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot = [&]() { return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"position":{"cell":30998795,"x":30,"y":-34,"z":0},"spells":[],"targets":[],"inventory":[]})")); };
    auto Profile = [&]() { return Json(TEXT(R"({"combat":"off","buffing":false,"recovery":false,"looting":false,"navigation":true,"autostack":true,"salvage_combine":true,"idle_peace":false,"route":[{"cell":30998795,"x":45,"y":-34,"z":0}]})")); };
    auto Item = [&](int32 Id, bool Bag) {
        auto I = Json(TEXT(R"({"name":"Combine fixture","wcid":100,"count":20,"max_stack":100,"resource_available":true,"container":1,"identified":true})"));
        I->SetNumberField(TEXT("id"), Id);
        if (Bag) { I->SetNumberField(TEXT("object_class"), 39); I->SetNumberField(TEXT("material"), 59); I->SetNumberField(TEXT("structure"), 20); I->SetNumberField(TEXT("workmanship"), 5); }
        return I;
    };
    auto Step = [&](FACEPluginVM& VM, auto S, auto P) { TSharedPtr<FJsonObject> I; TestTrue(*Error, VM.Step(S, P, I, Error)); return I ? I : MakeShared<FJsonObject>(); };
    auto Action = [](auto I) { FString A; I->TryGetStringField(TEXT("action"), A); return A; };
    for (bool Bag : {false, true}) for (int32 Failure = 0; Failure < 4; ++Failure)
    {
        FACEPluginVM VM; TestTrue(TEXT("Combine policy loads"), VM.Load(Source, Error));
        auto S = Snapshot(), P = Profile(), A = Item(10, Bag), B = Item(11, Bag);
        TArray<TSharedPtr<FJsonValue>> Items = {MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":9,"object_class":40,"salvage_tool_available":true})"))), MakeShared<FJsonValueObject>(A), MakeShared<FJsonValueObject>(B)};
        S->SetArrayField(TEXT("inventory"), Items);
        TestEqual(TEXT("Eligible combination starts"), Action(Step(VM, S, P)), FString(Bag ? TEXT("combine_salvage") : TEXT("merge")));
        S->SetNumberField(TEXT("time"), 101);
        TestTrue(TEXT("Pending operation is not duplicated"), Action(Step(VM, S, P)).IsEmpty());
        if (Failure == 0)
        {
            S->SetNumberField(TEXT("action_serial"), 1); S->SetNumberField(TEXT("action_error"), 5);
            TestTrue(TEXT("An unrelated use error cannot reject the combination"), Action(Step(VM, S, P)).IsEmpty());
            S->SetNumberField(TEXT("inventory_failure_serial"), 1); S->SetNumberField(TEXT("inventory_failure_item"), 99);
            TestTrue(TEXT("An unrelated inventory error cannot reject the combination"), Action(Step(VM, S, P)).IsEmpty());
            S->SetNumberField(TEXT("inventory_failure_serial"), 2); S->SetNumberField(TEXT("inventory_failure_item"), 10);
        }
        else if (Failure == 1) S->SetNumberField(TEXT("time"), 116);
        else if (Failure == 2)
        {
            auto F = Json(TEXT(R"({"status":"Server-specific item cannot combine"})"));
            F->SetStringField(TEXT("activity"), Bag ? TEXT("combine") : TEXT("inventory")); P->SetObjectField(TEXT("ucm_activity_failure"), F);
        }
        else
        {
            // One input disappears, but the rest never complete. Do not
            // loop on the remainder or infer success from unrelated changes.
            Items.RemoveAt(2); S->SetArrayField(TEXT("inventory"), Items); S->SetNumberField(TEXT("time"), 116);
        }
        TestEqual(TEXT("Rejection/timeout is reported once"), Action(Step(VM, S, P)), FString(TEXT("activity_failed")));
        P->RemoveField(TEXT("ucm_activity_failure"));
        for (int32 Tick = 0; Tick < 8; ++Tick)
        {
            S->SetNumberField(TEXT("time"), 117 + Tick * 60);
            S->SetNumberField(TEXT("inventory_revision"), Tick + 2); S->SetNumberField(TEXT("appraisal_revision"), Tick + 3);
            TestEqual(TEXT("Unchanged rejected items cannot stall navigation after cooldowns or unrelated revisions"), Action(Step(VM, S, P)), FString(TEXT("move")));
        }
        // Failed bags must not fall through to ordinary stack merging.
        auto C = Item(12, Bag), D = Item(13, Bag);
        C->SetNumberField(TEXT("wcid"), 200); D->SetNumberField(TEXT("wcid"), 200);
        Items.Add(MakeShared<FJsonValueObject>(C)); Items.Add(MakeShared<FJsonValueObject>(D)); S->SetArrayField(TEXT("inventory"), Items);
        S->SetNumberField(TEXT("time"), 700);
        auto Next = Step(VM, S, P);
        TestEqual(TEXT("Other valid inventory can still combine"), Action(Next), FString(Bag ? TEXT("combine_salvage") : TEXT("merge")));
        if (Bag) TestEqual(TEXT("Rejected bags excluded from next batch"), Next->GetStringField(TEXT("items")), FString(TEXT("12,13")));
        else TestEqual(TEXT("Rejected stacks excluded from next pair"), Next->GetNumberField(TEXT("item")), 13.);
    }
    // Known incompatible salvage is rejected before asking the server.
    for (int32 Case = 0; Case < 4; ++Case)
    {
        FACEPluginVM VM; VM.Load(Source, Error); auto S = Snapshot(), P = Profile(), A = Item(10, true), B = Item(11, true);
        if (Case == 0) A->SetNumberField(TEXT("material"), 3); // excluded material
        if (Case == 1) A->SetObjectField(TEXT("int_properties"), Json(TEXT(R"({"171":1})")));
        if (Case == 2) A->SetObjectField(TEXT("string_properties"), Json(TEXT(R"({"8":"Keep this"})")));
        if (Case == 3) P->SetBoolField(TEXT("salvage_combine"), false);
        if (Case == 0) B->SetNumberField(TEXT("material"), 3);
        S->SetArrayField(TEXT("inventory"), {MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":9,"object_class":40,"salvage_tool_available":true})"))), MakeShared<FJsonValueObject>(A), MakeShared<FJsonValueObject>(B)});
        TestEqual(TEXT("Unsupported/protected bags and disabled bag combining leave navigation running"), Action(Step(VM, S, P)), FString(TEXT("move")));
    }
    return !HasAnyErrors();
}
#endif
