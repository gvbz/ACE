#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

namespace
{
TSharedPtr<FJsonObject> BuffJson(const TCHAR* Text)
{TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMBuffParityTest,"ACE.Plugins.BuffParity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMBuffParityTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return BuffJson(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"combat_mode":8,"components_required":false,"inventory":[{"id":99,"type":32768,"equipped":true,"can_wield":true}],"spells":[{"id":10,"category":1,"power":100,"skill":300,"school":4,"duration":1800,"self_buff":true,"beneficial":true,"caster_target":true}],"enchantments":[]})"));};
    auto Profile=[](){return BuffJson(TEXT(R"({"buffing":true,"recovery":false,"combat":"off","refresh_seconds":60})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto Start=[&](FACEPluginVM& VM){TestTrue(TEXT("Buff policy loads"),VM.Load(Source,Error));};
    auto Enchant=[](auto S,double Remaining,int Power=100)
    {auto E=BuffJson(TEXT(R"({"id":10,"category":1,"power":100,"duration":1800})"));E->SetNumberField(TEXT("remaining"),Remaining);E->SetNumberField(TEXT("power"),Power);S->SetArrayField(TEXT("enchantments"),{MakeShared<FJsonValueObject>(E)});};
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("item_mana"),true);
        auto Items=S->GetArrayField(TEXT("inventory"));
        Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":30,"name":"Mana Charge","type":524288,"identified":true,"usable":true,"mana":5000})"))));
        Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":31,"name":"Jewel","type":8,"identified":true,"equipped":true,"mana":0,"max_mana":100})"))));S->SetArrayField(TEXT("inventory"),Items);
        TestEqual(TEXT("Low suit receives mana before buffing"),Step(VM,S,P)->GetNumberField(TEXT("item")),30.);
        S->SetBoolField(TEXT("busy"),true);S->SetNumberField(TEXT("time"),101);
        TestFalse(TEXT("Native item-use animation still excludes casting"),Step(VM,S,P)->HasField(TEXT("action")));
        S->SetBoolField(TEXT("busy"),false);S->SetNumberField(TEXT("time"),102);S->SetNumberField(TEXT("action_serial"),1);
        TestEqual(TEXT("Delayed equipment appraisals do not block buffing after the use gate releases"),Step(VM,S,P)->GetNumberField(TEXT("spell")),10.);
    }
    for(bool Other:{false,true})for(int Case=0;Case<4;++Case)
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        S->SetArrayField(TEXT("action_events"),{});
        if(Other)
        {
            P->SetBoolField(TEXT("auto_buffs"),false);P->SetBoolField(TEXT("buff_others"),true);
            auto Spell=S->GetArrayField(TEXT("spells"))[0]->AsObject();Spell->SetBoolField(TEXT("self_buff"),false);Spell->SetBoolField(TEXT("caster_target"),false);
            S->SetObjectField(TEXT("buff_request"),BuffJson(TEXT(R"({"id":1,"player":2,"name":"Visitor","present":true,"distance":5,"expires":1000,"started":true,"role":"mage"})")));
        }
        else S->SetNumberField(TEXT("force_buff_request"),1);
        TestEqual(TEXT("Starts the requested buff"),Step(VM,S,P)->GetNumberField(TEXT("spell")),10.);
        auto Receipt=BuffJson(TEXT(R"({"type":"cast","spell":10,"target":1,"requested_at":100,"action_serial":1,"error":0,"confirmed":true})"));
        Receipt->SetNumberField(TEXT("target"),Other?2:1);
        if(Case==1)Receipt->SetNumberField(TEXT("requested_at"),99);
        if(Case==2)Receipt->SetNumberField(TEXT("target"),999);
        if(Case==3)Receipt->SetNumberField(TEXT("spell"),999);
        auto Later=BuffJson(TEXT(R"({"type":"use","source":40,"requested_at":101,"action_serial":2,"error":1024})"));
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt),MakeShared<FJsonValueObject>(Later)});
        S->SetNumberField(TEXT("time"),102);S->SetNumberField(TEXT("action_serial"),2);S->SetNumberField(TEXT("action_error"),1024);
        // Deliberately misleading legacy fields: only the matching receipt may
        // complete the family, even if a later action overwrote last_spell.
        S->SetNumberField(TEXT("last_spell"),Case==0?0:10);S->SetBoolField(TEXT("last_spell_confirmed"),Case!=0);
        auto I=Step(VM,S,P);
        if(Case==0)TestEqual(TEXT("Completed buff survives a subsequent unrelated failure"),I->GetStringField(TEXT("action")),FString(Other?TEXT("buff_request_done"):TEXT("force_buff_done")));
        else TestFalse(TEXT("Old attempt, wrong target and other spell cannot complete a buff"),I->HasField(TEXT("action")));
    }
    // A fresh VM represents both restarting UCM and loading after login.
    for(double Remaining:{1800.,301.,61.,60.,59.,1.,0.})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();Enchant(S,Remaining);
        TestEqual(TEXT("Configured refresh threshold is honored before expiry on fresh login"),Step(VM,S,P)->HasField(TEXT("spell")),Remaining<=60.);
    }
    for(int Power:{99,100,200})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();Enchant(S,1000,Power);
        TestEqual(TEXT("Equal and stronger buffs suppress recasting; weaker buffs are upgraded"),Step(VM,S,P)->HasField(TEXT("spell")),Power<100);
    }
    for(int Duration:{-1,0})for(int Power:{100,200})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();Enchant(S,1.e9,Power);
        S->GetArrayField(TEXT("enchantments"))[0]->AsObject()->SetNumberField(TEXT("duration"),Duration);
        TestEqual(TEXT("Permanent equipment effects do not satisfy timed self buffs"),Step(VM,S,P)->GetNumberField(TEXT("spell")),10.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->RemoveField(TEXT("refresh_seconds"));Enchant(S,299);
        TestTrue(TEXT("New configuration uses VT's five-minute refresh default"),Step(VM,S,P)->HasField(TEXT("spell")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();Enchant(S,1000);
        S->SetNumberField(TEXT("force_buff_request"),1);
        TestTrue(TEXT("Explicit Force Buff still refreshes a healthy timer"),Step(VM,S,P)->HasField(TEXT("spell")));
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),10);S->SetBoolField(TEXT("last_spell_confirmed"),true);
        TestEqual(TEXT("Force Buff completes after confirmation instead of starting again"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("force_buff_done")));
        S->SetNumberField(TEXT("force_buff_request"),0); // Host consumes force_buff_done.
        TestFalse(TEXT("The same force request is not repeated"),Step(VM,S,P)->HasField(TEXT("spell")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();Step(VM,S,P);
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),10);S->SetBoolField(TEXT("last_spell_confirmed"),true);
        TestFalse(TEXT("Result before timer packet cannot immediately recast the same buff"),Step(VM,S,P)->HasField(TEXT("spell")));
        Enchant(S,1799);S->SetNumberField(TEXT("time"),101);
        TestFalse(TEXT("Delayed authoritative timer completes the buff"),Step(VM,S,P)->HasField(TEXT("spell")));
        S->SetArrayField(TEXT("enchantments"),{});S->SetNumberField(TEXT("time"),103);
        TestTrue(TEXT("A removed or dispelled character buff is not masked by a guessed timer"),Step(VM,S,P)->HasField(TEXT("spell")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();Step(VM,S,P);Enchant(S,1799);
        auto Spells=S->GetArrayField(TEXT("spells"));auto Next=MakeShared<FJsonObject>(*Spells[0]->AsObject());Next->SetNumberField(TEXT("id"),11);Next->SetNumberField(TEXT("category"),3);Spells.Add(MakeShared<FJsonValueObject>(Next));S->SetArrayField(TEXT("spells"),Spells);
        auto Active=S->GetArrayField(TEXT("enchantments"));auto E=BuffJson(TEXT(R"({"id":11,"category":3,"power":100,"remaining":80,"duration":1800})"));Active.Add(MakeShared<FJsonValueObject>(E));S->SetArrayField(TEXT("enchantments"),Active);
        S->SetNumberField(TEXT("time"),101);
        TestEqual(TEXT("An ongoing buff cycle groups a neighbouring expiration"),Step(VM,S,P)->GetNumberField(TEXT("spell")),11.);
        FACEPluginVM Fresh;Start(Fresh);TestFalse(TEXT("The grouping window does not survive restart"),Step(Fresh,S,P)->HasField(TEXT("spell")));
        S->SetNumberField(TEXT("time"),140);TestFalse(TEXT("Grouping ends thirty seconds after the last cast"),Step(VM,S,P)->HasField(TEXT("spell")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();auto Spell=S->GetArrayField(TEXT("spells"))[0]->AsObject();
        Spell->SetNumberField(TEXT("category"),69);S->SetArrayField(TEXT("usable_skills"),{MakeShared<FJsonValueNumber>(22)});
        TestFalse(TEXT("An untrained usable skill does not add automatic buffs"),Step(VM,S,P)->HasField(TEXT("spell")));
        S->SetArrayField(TEXT("trained_skills"),{MakeShared<FJsonValueNumber>(22)});
        TestTrue(TEXT("Training the skill enables its automatic buff"),Step(VM,S,P)->HasField(TEXT("spell")));
        Spell->SetNumberField(TEXT("category"),999);S->SetNumberField(TEXT("time"),120);
        TestFalse(TEXT("Unrelated beneficial families are not automatically cast"),Step(VM,S,P)->HasField(TEXT("spell")));
        P->SetArrayField(TEXT("buffs"),{MakeShared<FJsonValueNumber>(10)});
        TestTrue(TEXT("Explicitly included custom families still work"),Step(VM,S,P)->HasField(TEXT("spell")));
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();auto Spell=S->GetArrayField(TEXT("spells"))[0]->AsObject();Spell->SetNumberField(TEXT("flags"),0x2000);
        S->SetBoolField(TEXT("in_fellowship"),true);
        TestFalse(TEXT("A fellowship spell never silently replaces an automatic self buff"),Step(VM,S,P)->HasField(TEXT("spell")));
        P->SetArrayField(TEXT("buffs"),{MakeShared<FJsonValueNumber>(10)});
        TestTrue(TEXT("Explicit fellowship buff is allowed in a fellowship"),Step(VM,S,P)->HasField(TEXT("spell")));
    }
    // Missing worn pieces still use the retail all-armor target. A shield is
    // tested separately because equipping the casting wand removes it.
    for(bool Explicit:{false,true})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        auto Spell=S->GetArrayField(TEXT("spells"))[0]->AsObject();
        Spell->SetNumberField(TEXT("school"),3);Spell->SetNumberField(TEXT("category"),162);Spell->SetNumberField(TEXT("target_type"),6);
        auto Timer=BuffJson(TEXT(R"({"target":1,"category":162,"power":100,"duration":1800,"expires":1900,"missing_equipment":[50,51]})"));
        auto Items=S->GetArrayField(TEXT("inventory"));
        Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":50,"type":2,"slots":256,"equipped":true,"identified":true})"))));
        Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":51,"type":2,"slots":512,"equipped":true,"identified":true})"))));
        S->SetArrayField(TEXT("inventory"),Items);S->SetArrayField(TEXT("item_buffs"),{MakeShared<FJsonValueObject>(Timer)});
        if(Explicit)
        {
            P->SetBoolField(TEXT("auto_buffs"),false);
            P->SetArrayField(TEXT("item_buff_targets"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"item":50,"spell":10})")))});
        }
        TestEqual(TEXT("Multiple missing worn armor pieces receive one all-armor cast"),Step(VM,S,P)->GetNumberField(TEXT("target")),1.);
        Timer->RemoveField(TEXT("missing_equipment"));S->SetNumberField(TEXT("action_serial"),1);
        S->SetNumberField(TEXT("last_spell"),10);S->SetBoolField(TEXT("last_spell_confirmed"),true);
        S->SetNumberField(TEXT("time"),101);
        TestFalse(TEXT("Confirmed all-armor cast does not separately buff the next piece"),Step(VM,S,P)->HasField(TEXT("spell")));
    }
    // Consumable maintenance must use the same capped window as NeedBuff.
    for(int Case=0;Case<4;++Case)
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("auto_buffs"),false);
        P->SetArrayField(TEXT("buff_items"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"name":"First gem","spell":10})"))),MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"name":"Second gem","spell":10})")))});
        auto Items=S->GetArrayField(TEXT("inventory"));
        Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":50,"name":"First gem","usable":true,"count":10,"int_properties":{"280":213}})"))));
        Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":51,"name":"Second gem","usable":true,"count":10})"))));S->SetArrayField(TEXT("inventory"),Items);
        if(Case==1)S->SetObjectField(TEXT("cooldowns"),BuffJson(TEXT(R"({"213":30})")));
        else
        {
            TestEqual(TEXT("First available buff consumable is attempted"),Step(VM,S,P)->GetNumberField(TEXT("item")),50.);
            auto Receipt=BuffJson(TEXT(R"({"type":"use","source":50,"requested_at":100,"action_serial":1,"error":1})"));
            if(Case==2)Receipt->SetNumberField(TEXT("source"),99);
            if(Case==3)Receipt->SetNumberField(TEXT("requested_at"),90);
            S->SetNumberField(TEXT("time"),101);S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});
            if(Case>=2)
            {
                TestFalse(TEXT("Unrelated/old failure cannot cancel a consumable awaiting its enchantment"),Step(VM,S,P)->HasField(TEXT("action")));
                S->SetNumberField(TEXT("time"),113);
            }
        }
        TestEqual(TEXT("Rejected, timed-out or cooling-down gem yields to the next configured buff item"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("auto_buffs"),false);P->SetNumberField(TEXT("refresh_seconds"),300);
        P->SetArrayField(TEXT("buff_items"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"name":"Buff gem","spell":10})")))});
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":50,"name":"Buff gem","usable":true,"count":10})"))));S->SetArrayField(TEXT("inventory"),Items);
        S->GetArrayField(TEXT("spells"))[0]->AsObject()->SetNumberField(TEXT("duration"),120);Enchant(S,119);S->GetArrayField(TEXT("enchantments"))[0]->AsObject()->SetNumberField(TEXT("duration"),120);
        TestFalse(TEXT("Fresh short-duration consumable is not consumed again"),Step(VM,S,P)->HasField(TEXT("action")));
        FACEPluginVM Augmented;Start(Augmented);
        Enchant(S,100);S->GetArrayField(TEXT("enchantments"))[0]->AsObject()->SetNumberField(TEXT("duration"),240);
        TestEqual(TEXT("Consumable refresh uses the active server duration, not its shorter base duration"),Step(Augmented,S,P)->GetNumberField(TEXT("item")),50.);
        Enchant(S,59);S->GetArrayField(TEXT("enchantments"))[0]->AsObject()->SetNumberField(TEXT("duration"),120);
        TestEqual(TEXT("Consumable refreshes at its actual maintenance threshold"),Step(VM,S,P)->GetNumberField(TEXT("item")),50.);
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("auto_buffs"),false);
        P->SetArrayField(TEXT("buff_items"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"name":"Buff gem","spell":10})")))});
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"id":50,"name":"Buff gem","usable":true,"count":10})"))));S->SetArrayField(TEXT("inventory"),Items);
        Enchant(S,1.e9);S->GetArrayField(TEXT("enchantments"))[0]->AsObject()->SetNumberField(TEXT("duration"),-1);
        TestEqual(TEXT("Wield effects do not replace a configured consumable's timed buff"),Step(VM,S,P)->GetNumberField(TEXT("item")),50.);
        S->SetNumberField(TEXT("time"),101);
        TestEqual(TEXT("Equipment effect alone cannot confirm consumable success"),Step(VM,S,P)->GetStringField(TEXT("status")),FString(TEXT("Waiting for consumable buff confirmation")));
        Enchant(S,1799);
        TestFalse(TEXT("Observed timed consumable buff finishes the wait without another use"),Step(VM,S,P)->HasField(TEXT("action")));
    }
    for(bool Excluded:{false,true})for(int Margin:{5,30})
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("auto_buffs"),false);P->SetBoolField(TEXT("buff_others"),true);P->SetNumberField(TEXT("buff_skill_margin"),Margin);P->SetNumberField(TEXT("skill_margin"),60);
        if(Excluded)P->SetArrayField(TEXT("excluded_buff_spells"),{MakeShared<FJsonValueNumber>(10)});
        S->SetObjectField(TEXT("buff_request"),BuffJson(TEXT(R"({"id":1,"player":2,"name":"Visitor","present":true,"distance":5,"expires":1000,"started":true,"role":"mage"})")));
        auto Spell=S->GetArrayField(TEXT("spells"))[0]->AsObject();Spell->SetBoolField(TEXT("self_buff"),false);Spell->SetBoolField(TEXT("caster_target"),false);Spell->SetNumberField(TEXT("skill"),110);
        auto I=Step(VM,S,P);TestEqual(TEXT("Other buffs honor the buff margin and spell-family exclusions"),I->HasField(TEXT("spell")),!Excluded&&Margin==5);
        if(I->HasField(TEXT("spell")))
        {
            TestEqual(TEXT("Other buff targets the requester"),I->GetNumberField(TEXT("target")),2.);
            TestFalse(TEXT("Other buff waits for server confirmation"),Step(VM,S,P)->HasField(TEXT("spell")));
            S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),10);S->SetBoolField(TEXT("last_spell_confirmed"),true);
            TestEqual(TEXT("Other buff does not repeat a confirmed family"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("buff_request_done")));
        }
    }
    {
        FACEPluginVM VM;Start(VM);auto S=Snapshot(),P=Profile();
        auto Spell=S->GetArrayField(TEXT("spells"))[0]->AsObject();
        Spell->SetNumberField(TEXT("school"),3);Spell->SetNumberField(TEXT("category"),162);Spell->SetNumberField(TEXT("target_type"),6);
        auto ArmorTimer=BuffJson(TEXT(R"({"target":1,"category":162,"power":100,"duration":1800,"expires":1900,"missing_equipment":[50]})"));
        auto Shield=BuffJson(TEXT(R"({"id":50,"type":2,"slots":2097152,"name":"Shield","equipped":true,"can_wield":true})"));
        auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(MakeShared<FJsonValueObject>(Shield));S->SetArrayField(TEXT("inventory"),Inventory);
        S->SetArrayField(TEXT("item_buffs"),{MakeShared<FJsonValueObject>(ArmorTimer)});
        TestEqual(TEXT("New shield receives a direct buff without recasting all armor"),Step(VM,S,P)->GetNumberField(TEXT("target")),50.);
        Shield->SetBoolField(TEXT("equipped"),false);S->SetNumberField(TEXT("time"),113);
        TestEqual(TEXT("Shield remains the buff recipient after swapping to wand"),Step(VM,S,P)->GetNumberField(TEXT("target")),50.);
        auto ShieldTimer=MakeShared<FJsonObject>(*ArmorTimer);ShieldTimer->SetNumberField(TEXT("target"),50);ShieldTimer->RemoveField(TEXT("missing_equipment"));
        S->SetArrayField(TEXT("item_buffs"),{MakeShared<FJsonValueObject>(ArmorTimer),MakeShared<FJsonValueObject>(ShieldTimer)});
        for(int Time:{114,130,150})
        {
            Shield->SetBoolField(TEXT("equipped"),true);S->SetNumberField(TEXT("time"),Time);
            TestFalse(TEXT("Returning to combat gear cannot restart confirmed item buffs"),Step(VM,S,P)->HasField(TEXT("spell")));
        }
        P->SetBoolField(TEXT("auto_buffs"),false);
        P->SetArrayField(TEXT("item_buff_targets"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"item":50,"spell":10})")))});
        S->SetArrayField(TEXT("item_buffs"),{MakeShared<FJsonValueObject>(ArmorTimer)});
        FACEPluginVM Explicit;Start(Explicit);
        TestEqual(TEXT("An explicit armor binding also covers the newly equipped shield"),Step(Explicit,S,P)->GetNumberField(TEXT("target")),50.);
        S->SetArrayField(TEXT("item_buffs"),{});P->SetBoolField(TEXT("item_buffs_enabled"),false);
        P->SetBoolField(TEXT("auto_buffs"),true);
        P->SetArrayField(TEXT("buffs"),{MakeShared<FJsonValueNumber>(10)});
        P->SetArrayField(TEXT("item_buff_targets"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"item":50,"spell":10})")))});
        S->SetNumberField(TEXT("time"),180);
        TestFalse(TEXT("Item buff switch overrides automatic, included and explicit item buffs"),Step(VM,S,P)->HasField(TEXT("spell")));
        S->SetNumberField(TEXT("force_buff_request"),1);
        TestFalse(TEXT("Force Buff respects disabled item magic"),Step(VM,S,P)->HasField(TEXT("spell")));
        Shield->SetBoolField(TEXT("usable"),true);
        P->SetArrayField(TEXT("buff_items"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"name":"Shield","spell":10})")))});
        FACEPluginVM Consumable;Start(Consumable);
        TestFalse(TEXT("Item magic disabled also prevents a configured item from applying it"),Step(Consumable,S,P)->HasField(TEXT("item")));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMUnenchantableEquipmentTest,"ACE.Plugins.UnenchantableEquipment",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMUnenchantableEquipmentTest::RunTest(const FString&)
{
    FString Source,Error;
    FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    for(int Resist:{0,9998,9999,10000})for(bool Equipped:{false,true})
    {
        FACEPluginVM VM;TestTrue(TEXT("Buff policy loads"),VM.Load(Source,Error));
        auto S=BuffJson(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"combat_mode":8,"components_required":false,"spells":[{"id":10,"category":162,"power":100,"skill":300,"school":3,"duration":1800,"self_buff":true,"beneficial":true,"caster_target":true,"target_type":6}],"item_buffs":[{"target":1,"category":162,"power":100,"duration":1800,"expires":1900,"missing_equipment":[50]}],"inventory":[{"id":99,"type":32768,"name":"Wand","equipped":true,"identified":true,"can_wield":true}]})"));
        auto P=BuffJson(TEXT(R"({"buffing":true,"recovery":false,"combat":"off","refresh_seconds":60})"));
        auto Shield=BuffJson(TEXT(R"({"id":50,"type":2,"slots":2097152,"name":"Any shield","identified":true,"can_wield":true})"));
        auto Ints=MakeShared<FJsonObject>();Ints->SetNumberField(TEXT("36"),Resist);Shield->SetObjectField(TEXT("int_properties"),Ints);
        Shield->SetBoolField(TEXT("equipped"),Equipped);
        auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(MakeShared<FJsonValueObject>(Shield));S->SetArrayField(TEXT("inventory"),Inventory);
        auto Step=[&](){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
        auto I=Step();
        TestEqual(TEXT("Direct shield buffs honor the retail ResistMagic threshold in either slot"),I->HasField(TEXT("spell")),Resist<9999);
        if(Resist>=9999)for(int Time:{113,150,301,600})
        {
            S->SetNumberField(TEXT("time"),Time);
            TestFalse(TEXT("Unenchantable gear never starts a later buff retry or wand swap"),Step()->HasField(TEXT("action")));
        }
        // No appraisal yet: identify before trying a direct bane, even when a
        // cached missing-equipment queue survived a previous client version.
        FACEPluginVM Unknown;TestTrue(TEXT("Fresh appraisal policy loads"),Unknown.Load(Source,Error));
        Shield->RemoveField(TEXT("int_properties"));Shield->SetBoolField(TEXT("identified"),false);
        TSharedPtr<FJsonObject> Intent;TestTrue(*Error,Unknown.Step(S,P,Intent,Error));
        TestEqual(TEXT("Unknown shield is assessed before casting or switching equipment"),Intent->GetStringField(TEXT("action")),FString(TEXT("identify")));
        Shield->SetObjectField(TEXT("int_properties"),Ints);Shield->SetBoolField(TEXT("identified"),true);
        S->SetNumberField(TEXT("data_revision"),1);S->SetNumberField(TEXT("time"),601);
        TestTrue(*Error,Unknown.Step(S,P,Intent,Error));
        TestEqual(TEXT("Returned appraisal determines whether the direct buff is allowed"),Intent->HasField(TEXT("spell")),Resist<9999);
        // Explicit item-target bindings must respect the same restriction.
        FACEPluginVM Explicit;TestTrue(TEXT("Explicit buff policy loads"),Explicit.Load(Source,Error));
        P->SetBoolField(TEXT("auto_buffs"),false);
        P->SetArrayField(TEXT("item_buff_targets"),{MakeShared<FJsonValueObject>(BuffJson(TEXT(R"({"item":50,"spell":10})")))});
        S->SetArrayField(TEXT("item_buffs"),{});
        TestTrue(*Error,Explicit.Step(S,P,Intent,Error));
        TestEqual(TEXT("Explicit bindings also skip unenchantable equipment"),Intent->HasField(TEXT("spell")),Resist<9999);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMBuffEquipmentHandoffTest,"ACE.Plugins.BuffEquipmentHandoff",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMBuffEquipmentHandoffTest::RunTest(const FString&)
{
    FString Source,Error;
    FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    const auto Step=[&](FACEPluginVM& VM,auto S,auto P)
    {
        TSharedPtr<FJsonObject> I;
        TestTrue(*Error,VM.Step(S,P,I,Error));
        return I?I:MakeShared<FJsonObject>();
    };
    // Academy-style wands apply permanent Focus, Willpower and Mana Conversion
    // enchantments on wield. They vanish when the combat weapon replaces the wand.
    // Exercise ordinary and forced buffing with single/dual melee and missile gear.
    for(const FString Mode:{TEXT("melee"),TEXT("dual"),TEXT("missile")})for(bool Force:{false,true})
    {
        FACEPluginVM VM;TestTrue(TEXT("Equipment handoff policy loads"),VM.Load(Source,Error));
        auto S=BuffJson(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"combat_mode":2,"components_required":false,"trained_skills":[16,49],"spells":[],"enchantments":[],"targets":[{"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true}]})"));
        auto P=BuffJson(TEXT(R"({"buffing":true,"recovery":false,"combat":"melee","refresh_seconds":60,"weapon_items":[50,51]})"));
        auto Wand=BuffJson(TEXT(R"({"id":99,"name":"Buff wand","type":32768,"identified":true,"can_wield":true,"slots":16777216,"equipped":false})"));
        auto Weapon=BuffJson(TEXT(R"({"id":50,"name":"Combat weapon","type":1,"identified":true,"can_wield":true,"slots":1048576,"equipped":true,"equipped_slot":1048576,"damage":30,"damage_type":1})"));
        auto Secondary=BuffJson(TEXT(R"({"id":51,"name":"Offhand weapon","type":1,"identified":true,"can_wield":true,"slots":1048576,"equipped":false,"damage":10,"damage_type":1})"));
        if(Mode==TEXT("dual"))P->SetNumberField(TEXT("melee_secondary_equip"),51);
        if(Mode==TEXT("missile"))
        {
            P->SetStringField(TEXT("combat"),TEXT("missile"));
            Weapon->SetNumberField(TEXT("type"),256);Weapon->SetNumberField(TEXT("slots"),4194304);
            Weapon->SetNumberField(TEXT("equipped_slot"),4194304);Weapon->SetNumberField(TEXT("ammo_type"),1);
            Secondary->SetNumberField(TEXT("type"),256);Secondary->SetNumberField(TEXT("slots"),8388608);
            Secondary->SetNumberField(TEXT("ammo_type"),1);
        }
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Wand),MakeShared<FJsonValueObject>(Weapon),MakeShared<FJsonValueObject>(Secondary)});
        TArray<TSharedPtr<FJsonValue>> Spells,Permanent,Timed;
        for(int Category:{9,11,51})
        {
            auto Spell=BuffJson(TEXT(R"({"power":50,"skill":300,"school":4,"duration":1800,"self_buff":true,"beneficial":true,"caster_target":true})"));
            Spell->SetNumberField(TEXT("id"),Category);Spell->SetNumberField(TEXT("category"),Category);Spells.Add(MakeShared<FJsonValueObject>(Spell));
            auto E=BuffJson(TEXT(R"({"power":50,"duration":-1,"remaining":1000000000})"));
            E->SetNumberField(TEXT("id"),Category);E->SetNumberField(TEXT("category"),Category);Permanent.Add(MakeShared<FJsonValueObject>(E));
        }
        S->SetArrayField(TEXT("spells"),Spells);
        if(Force)S->SetNumberField(TEXT("force_buff_request"),1);
        TestEqual(TEXT("Missing buffs first equip the casting tool"),Step(VM,S,P)->GetNumberField(TEXT("item")),99.);
        Wand->SetBoolField(TEXT("equipped"),true);Weapon->SetBoolField(TEXT("equipped"),false);
        S->SetArrayField(TEXT("enchantments"),Permanent);
        for(int Index=0;Index<Spells.Num();++Index)
        {
            S->SetNumberField(TEXT("time"),101+Index);
            const auto I=Step(VM,S,P);
            const int Spell=Spells[Index]->AsObject()->GetIntegerField(TEXT("id"));
            if(!TestEqual(*FString::Printf(TEXT("%s: wield enchantment must not skip timed buff %d"),*Mode,Spell),I->GetStringField(TEXT("action")),FString(TEXT("cast"))))
                return false;
            TestEqual(TEXT("Casting families retain their order"),I->GetNumberField(TEXT("spell")),double(Spell));
            TestEqual(TEXT("Buff targets the player"),I->GetNumberField(TEXT("target")),1.);
            auto E=MakeShared<FJsonObject>(*Spells[Index]->AsObject());E->SetNumberField(TEXT("remaining"),1799);
            Timed.Add(MakeShared<FJsonValueObject>(E));
            auto Active=Permanent;Active.Append(Timed);S->SetArrayField(TEXT("enchantments"),Active);
            S->SetNumberField(TEXT("action_serial"),Index+1);S->SetNumberField(TEXT("last_spell"),Spell);S->SetBoolField(TEXT("last_spell_confirmed"),true);
        }
        S->SetNumberField(TEXT("time"),104);
        if(Force)
        {
            TestEqual(TEXT("Forced cycle completes with real cast confirmations"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("force_buff_done")));
            S->SetNumberField(TEXT("force_buff_request"),0);
        }
        TestEqual(TEXT("Buff completion restores the selected combat weapon"),Step(VM,S,P)->GetNumberField(TEXT("item")),50.);
        Wand->SetBoolField(TEXT("equipped"),false);Weapon->SetBoolField(TEXT("equipped"),true);
        S->SetArrayField(TEXT("enchantments"),Timed); // Server removes only the wand's layers.
        S->SetNumberField(TEXT("time"),105);
        if(Mode!=TEXT("melee"))
        {
            TestEqual(TEXT("Combat completes offhand/ammunition equipment"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
            Secondary->SetBoolField(TEXT("equipped"),true);
            Secondary->SetNumberField(TEXT("equipped_slot"),Mode==TEXT("dual")?2097152:8388608);
        }
        for(int Time:{106,107,110,130})
        {
            S->SetNumberField(TEXT("time"),Time);
            TestEqual(TEXT("Combat continues without re-equipping the buff wand"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        }
        // Genuine loss of a timed buff must still trigger a new buff cycle.
        Timed.RemoveAt(0);S->SetArrayField(TEXT("enchantments"),Timed);S->SetNumberField(TEXT("time"),140);
        TestEqual(TEXT("Dispelled timed buff still equips the wand for recovery"),Step(VM,S,P)->GetNumberField(TEXT("item")),99.);
    }
    return !HasAnyErrors();
}
#endif
