#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMComponentEquipmentTest,"ACE.Plugins.ComponentEquipment",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMComponentEquipmentTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> J;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),J);return J;};
    FString Script,Error;
    FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    for(const TCHAR* Missing:{TEXT("Prismatic Taper"),TEXT("Hyssop"),TEXT("Powdered Onyx")})
    {
        FACEPluginVM VM;TestTrue(TEXT("UCM loads"),VM.Load(Script,Error));
        auto S=Json(TEXT(R"({"time":100,"player":1,"health":40,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"combat_mode":2,"components_required":true,"skills":{"44":{"training":3,"current":600}},"targets":[{"id":50,"name":"Rat","distance":1,"identified":true,"line_of_sight":true}],"inventory":[{"id":90,"name":"Sword","type":1,"damage_type":1,"damage":20,"identified":true,"can_wield":true,"equipped":true},{"id":91,"name":"Wand","type":32768,"identified":true,"can_wield":true},{"id":92,"name":"Scarab","count":100},{"id":93,"name":"Supply","count":0}],"spells":[{"id":10,"name":"Heal Self VI","category":67,"school":2,"power":100,"skill":600,"duration":0,"caster_target":true,"beneficial":true,"known":true,"components_known":true,"scarabs":{"Scarab":1}}]})"));
        auto P=Json(TEXT(R"({"combat":"melee","buffing":false,"recovery":true,"looting":false,"idle_peace":false,"health_threshold":65,"stop_health":15})"));
        auto Spell=S->GetArrayField(TEXT("spells"))[0]->AsObject();
        auto Components=Json(TEXT(R"({"Scarab":1})"));Components->SetNumberField(Missing,4);Spell->SetObjectField(TEXT("components"),Components);
        auto Supply=S->GetArrayField(TEXT("inventory"))[3]->AsObject();Supply->SetStringField(TEXT("name"),Missing);
        auto Sword=S->GetArrayField(TEXT("inventory"))[0]->AsObject(),Wand=S->GetArrayField(TEXT("inventory"))[1]->AsObject();
        auto Step=[&](){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
        auto Action=[](const auto& I){FString A;I->TryGetStringField(TEXT("action"),A);return A;};
        for(int32 Tick=0;Tick<12;++Tick)
        {
            S->SetNumberField(TEXT("time"),100+Tick);
            Supply->SetNumberField(TEXT("count"),Tick%4); // All below the formula's required quantity.
            const auto I=Step();
            TestEqual(FString::Printf(TEXT("Missing %s does not swap away from a usable physical weapon"),Missing),Action(I),FString(TEXT("attack")));
        }
        Supply->SetNumberField(TEXT("count"),4);S->SetNumberField(TEXT("time"),113);
        auto I=Step();TestEqual(TEXT("Restocking permits one useful wand switch"),Action(I),FString(TEXT("equip")));
        TestEqual(TEXT("Recovery equips the wand"),I->GetNumberField(TEXT("item")),91.);
        Sword->SetBoolField(TEXT("equipped"),false);Wand->SetBoolField(TEXT("equipped"),true);S->SetNumberField(TEXT("combat_mode"),8);
        I=Step();TestEqual(TEXT("After the equipment receipt recovery actually casts"),Action(I),FString(TEXT("cast")));
        TestEqual(TEXT("Recovery casts the eligible heal"),I->GetNumberField(TEXT("spell")),10.);
        // Explicit component exemptions remain authoritative on custom servers.
        FACEPluginVM Exempt;Exempt.Load(Script,Error);Supply->SetNumberField(TEXT("count"),0);S->SetBoolField(TEXT("components_required"),false);
        TestTrue(*Error,Exempt.Step(S,P,I,Error));TestEqual(TEXT("Exempt characters still cast without ingredients"),Action(I),FString(TEXT("cast")));
        // A pure caster with no usable attack must stop before any wand change.
        FACEPluginVM Caster;Caster.Load(Script,Error);S->SetBoolField(TEXT("components_required"),true);S->SetNumberField(TEXT("health"),100);
        P->SetStringField(TEXT("combat"),TEXT("magic"));P->SetBoolField(TEXT("manual_combat"),true);P->SetBoolField(TEXT("recovery"),false);
        Spell->SetNumberField(TEXT("category"),121);Spell->SetNumberField(TEXT("school"),1);Spell->SetBoolField(TEXT("caster_target"),false);Spell->SetBoolField(TEXT("beneficial"),false);
        Wand->SetBoolField(TEXT("equipped"),false);Sword->SetBoolField(TEXT("equipped"),true);
        for(int32 Tick=0;Tick<6;++Tick)
        {
            S->SetNumberField(TEXT("time"),120+Tick);
            TestTrue(*Error,Caster.Step(S,P,I,Error));
            if(Tick==0)TestEqual(TEXT("Unsupplied caster reports its blocked action"),Action(I),FString(TEXT("activity_failed")));
            TestTrue(TEXT("Missing supplies never dispatch an equipment change or attack, including during retry backoff"),
                Action(I).IsEmpty()||Action(I)==TEXT("activity_failed"));
            TestTrue(TEXT("Blocked casting explains component eligibility"),I->GetStringField(TEXT("status")).Contains(TEXT("components")));
        }
    }
    return !HasAnyErrors();
}
#endif
