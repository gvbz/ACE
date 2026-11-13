#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"
namespace
{
TSharedPtr<FJsonObject> SettingsJSON(const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVTSettingsTest,"ACE.Plugins.CharacterSettings",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEVTSettingsTest::RunTest(const FString&)
{
    FString Script,Error;TArray<FString> Issues;
    FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Document=SettingsJSON(TEXT(R"({"format":"usd","tables":[
     {"name":"Settings","columns":["Setting","Value"],"rows":[["EnableBuffing",false],["EnableCombat",false],["AttackDistance",0.1],["RechargeHandlerSet",{"columns":["Vital","HandlerString","MinPercent","MaxPercent","Stance"],"rows":[[1,"Regular Spell",0,30,1],[1,"Recharge With Food",0,100,1],[1,"Kit Recharge",0,100,2],[1,"Recharge With Food",0,10,2]]}]]},
     {"name":"AssistItems","columns":["Object","Type"],"rows":[["Bread",1],["Healing Kit",0]]},
     {"name":"BuffedItems","columns":["Object","Spell"],"rows":[[-2147483647,-1]]},
     {"name":"ExtraBuffSpells","columns":["ExemplarId"],"rows":[[101]]},
     {"name":"AntiExtraBuffSpells","columns":["ExemplarId"],"rows":[[102]]},
     {"name":"GemFoodItems","columns":["Name","Spell"],"rows":[["Blessing Gem",103]]},
     {"name":"MyMonsters","columns":["MonsterName","AttackPriority","DamageType","WeaponToUse","Attack","Streak"],"rows":[["<DEFAULT>",1,8,-1,true,false],["Olthoi",2,3,-1,true,true]]}
    ]})"));
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["CastDispelSelf",true],["UseDispelItems",true]]}]})"));
        auto Dispels=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Requested dispel policies have runtime adapters"),Issues.Num(),0);
        TestTrue(TEXT("Self dispel import enabled"),Dispels->GetBoolField(TEXT("dispel_self")));TestTrue(TEXT("Dispel supply import enabled"),Dispels->GetBoolField(TEXT("dispel_items")));
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["AutoCraftItems",true],["AutoFellowManagement",true],["IdleCraftCount_HealthKits",5],["IdleCraftCount_StamKits",6],["IdleCraftCount_ManaKits",7],["IdleCraftCount_HealthFood",8],["IdleCraftCount_StamFood",9],["IdleCraftCount_ManaFood",10]]}]})"));
        auto Services=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Automatic services import has no unsupported settings"),Issues.Num(),0);
        TestTrue(TEXT("Automatic crafting imported"),Services->GetBoolField(TEXT("auto_craft")));TestTrue(TEXT("Automatic fellowship imported"),Services->GetBoolField(TEXT("auto_fellowship")));
        TestEqual(TEXT("Health kit stock imported"),Services->GetNumberField(TEXT("craft_health_kits")),5.);TestEqual(TEXT("Mana food stock imported"),Services->GetNumberField(TEXT("craft_mana_food")),10.);
    }
    auto P=ACEVTProfile::Convert(Document,Issues);TestEqual(TEXT("Supported character tables convert"),Issues.Num(),0);
    TestTrue(TEXT("Omitted auto crafting uses VT resource default"),P->GetBoolField(TEXT("auto_craft")));TestTrue(TEXT("Omitted fellowship manager uses VT default"),P->GetBoolField(TEXT("auto_fellowship")));
    TestEqual(TEXT("Omitted idle kit target uses VT resource default"),P->GetNumberField(TEXT("craft_health_kits")),2.);TestEqual(TEXT("Omitted idle food target uses VT resource default"),P->GetNumberField(TEXT("craft_health_food")),15.);
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["DoorLockpickDiffExcessThreshold",10]]},{"name":"AssistItems","columns":["Object","Type"],"rows":[["Intricate Lockpick",10]]}]})"));
        auto Doors=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Lockpick assistance imports without dropping entries"),Issues.Num(),0);
        TestEqual(TEXT("Imported lockpick retains category ten"),Doors->GetArrayField(TEXT("assist_items"))[0]->AsObject()->GetNumberField(TEXT("type")),10.);
        TestEqual(TEXT("Lockpick excess is skill units, not map distance"),Doors->GetNumberField(TEXT("door_lockpick_margin")),10.);
    }
    {
        auto VoidDocument=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"MyMonsters","columns":["MonsterName","DamageType","Corruption"],"rows":[["<DEFAULT>",9,true]]}]})"));
        auto Converted=ACEVTProfile::Convert(VoidDocument,Issues);
        TestEqual(TEXT("VT Nether profile imports without gaps"),Issues.Num(),0);
        const auto& Rules=Converted->GetArrayField(TEXT("monsters"));
        if(TestEqual(TEXT("Void import retains the default monster rule"),Rules.Num(),1))
        {
            const auto Rule=Rules[0]->AsObject();
            TestEqual(TEXT("VT Nether uses the retail damage bit"),Rule->GetNumberField(TEXT("damage_type")),1024.);
            TestTrue(TEXT("VT Void damage-over-time choice survives import"),Rule->GetBoolField(TEXT("debuff_corruption")));
        }
    }
    TestEqual(TEXT("VT map distance becomes world meters"),P->GetNumberField(TEXT("radius")),24.);
    TestEqual(TEXT("Signed object GUID preserved"),P->GetArrayField(TEXT("weapon_items"))[0]->AsNumber(),2147483649.);
    auto Snapshot=[](){return SettingsJSON(TEXT(R"({"time":100,"player":1,"health":20,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"combat_mode":8,"nearest":0,"action_serial":0,"action_error":0,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"trained_skills":[21],"skills":{"21":{"current":400}},"inventory":[{"id":1,"name":"Orb","type":32768,"equipped":true},{"id":2,"name":"Bread","type":0,"identified":true,"usable":true,"boost_vital":2,"boost":20},{"id":3,"name":"Unlisted Food","type":0,"identified":true,"usable":true,"boost_vital":2,"boost":90},{"id":4,"name":"Healing Kit","type":0,"identified":true,"usable":true,"healing_kit":true,"structure":5,"boost_vital":2,"boost":30}],"spells":[{"id":10,"name":"Heal Self I","category":79,"skill":300,"power":50,"caster_target":true,"duration":0}],"enchantments":[]})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto Profile){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,Profile,I,Error));return I?I:MakeShared<FJsonObject>();};
    {
        auto Supplies=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"AssistItems","columns":["Object","Type"],"rows":[["Major Mana Stone",8],["Titan Mana Charge",6]]},{"name":"Settings","columns":["Setting","Value"],"rows":[["ReadUnknownScrolls",true],["RefillWornMana",true],["RefillWornMana-Item-ManaPercent",40],["SpellDiffExcessThreshold-Buff",10],["SpellDiffExcessThreshold-Hunt",20]]}]})"));
        auto Converted=ACEVTProfile::Convert(Supplies,Issues);TestEqual(TEXT("Reusable and disposable mana supplies convert"),Issues.Num(),0);
        TestTrue(TEXT("Scroll learning option imported"),Converted->GetBoolField(TEXT("read_unknown_scrolls")));
        TestEqual(TEXT("Buff skill margin imported separately"),Converted->GetNumberField(TEXT("buff_skill_margin")),10.);
        TestEqual(TEXT("Hunt skill margin imported separately"),Converted->GetNumberField(TEXT("skill_margin")),20.);
        Converted->SetBoolField(TEXT("buffing"),false);Converted->SetBoolField(TEXT("recovery"),false);Converted->SetStringField(TEXT("combat"),TEXT("off"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"values":[{"id":20,"type":0,"equipped":true,"mana":30,"max_mana":100},{"id":21,"name":"Major Mana Stone","type":524288,"mana":5000,"wcid":1,"identified":true,"usable":true}]})"))->GetArrayField(TEXT("values")));
        FACEPluginVM Refill;Refill.Load(Script,Error);auto I=Step(Refill,S,Converted);TestEqual(TEXT("Type eight supply refills at configured threshold"),I->GetNumberField(TEXT("item")),21.);
        S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("action_serial"),1);I=Step(Refill,S,Converted);TestFalse(TEXT("UseDone alone cannot repeat an unconfirmed equipment recharge"),I->HasField(TEXT("action")));
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("mana"),90);I=Step(Refill,S,Converted);TestFalse(TEXT("Observed mana increase completes refill"),I->HasField(TEXT("action")));
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("mana"),30);
        FACEPluginVM Rejected;Rejected.Load(Script,Error);S->SetNumberField(TEXT("time"),200);Step(Rejected,S,Converted);
        S->SetNumberField(TEXT("time"),211);I=Step(Rejected,S,Converted);TestFalse(TEXT("Missing confirmation cannot immediately repeat"),I->HasField(TEXT("action")));
        Converted->SetBoolField(TEXT("item_mana"),false);FACEPluginVM Disabled;Disabled.Load(Script,Error);I=Step(Disabled,S,Converted);TestFalse(TEXT("Refill disable respected"),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;TestTrue(TEXT("Settings runtime loads"),VM.Load(Script,Error));auto S=Snapshot();
        TestEqual(TEXT("Magic stance prioritizes spell below 30 percent"),Step(VM,S,P)->GetNumberField(TEXT("spell")),10.);
        S->SetNumberField(TEXT("time"),109);
        TestEqual(TEXT("Missing spell acknowledgement falls through to named food"),Step(VM,S,P)->GetNumberField(TEXT("item")),2.);
        S->SetNumberField(TEXT("time"),110);TestFalse(TEXT("Food waits for confirmation"),Step(VM,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("health"),80);
        TestFalse(TEXT("Actual vital improvement finishes recovery"),Step(VM,S,P)->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot();S->SetNumberField(TEXT("combat_mode"),2);S->SetNumberField(TEXT("health"),50);
        TestEqual(TEXT("Melee stance uses its kit order"),Step(VM,S,P)->GetNumberField(TEXT("item")),4.);
        FACEPluginVM Untrained;Untrained.Load(Script,Error);S->SetArrayField(TEXT("trained_skills"),{});
        TestEqual(TEXT("Unavailable kit falls through to final handler outside percentage range"),Step(Untrained,S,P)->GetNumberField(TEXT("item")),2.);
    }
    {
        FACEPluginVM VM;VM.Load(Script,Error);auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        auto B=SettingsJSON(TEXT(R"({"buffing":true,"auto_buffs":false,"recovery":false,"combat":"off","excluded_buff_spells":[102],"buffs":[102],"buff_items":[{"name":"Blessing Gem","spell":103}]})"));
        auto Spells=SettingsJSON(TEXT(R"({"values":[{"id":102,"name":"Excluded exemplar","category":1,"known":false},{"id":104,"name":"Strength","category":1,"power":100,"skill":300,"self_buff":true,"duration":300},{"id":103,"name":"Blessing","category":999,"power":100,"duration":300,"known":false,"flags":4}]})"));
        S->SetArrayField(TEXT("spells"),Spells->GetArrayField(TEXT("values")));auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"id":9,"name":"Blessing Gem","type":0,"usable":true})"))));S->SetArrayField(TEXT("inventory"),Inventory);
        TestEqual(TEXT("Unknown exclusion exemplar excludes the entire known family; gem remains usable"),Step(VM,S,B)->GetNumberField(TEXT("item")),9.);
        S->SetNumberField(TEXT("action_serial"),1);TestFalse(TEXT("UseDone alone cannot confirm a gem buff"),Step(VM,S,B)->HasField(TEXT("action")));
        S->SetArrayField(TEXT("enchantments"),{MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"category":999,"power":100,"remaining":200})")))});
        TestFalse(TEXT("Confirmed gem is not consumed again"),Step(VM,S,B)->HasField(TEXT("action")));
        S->SetArrayField(TEXT("enchantments"),{});S->GetArrayField(TEXT("spells"))[2]->AsObject()->SetNumberField(TEXT("flags"),0x2004);
        FACEPluginVM Fellow;Fellow.Load(Script,Error);
        TestFalse(TEXT("Fellowship consumable is skipped outside fellowship"),Step(Fellow,S,B)->HasField(TEXT("action")));
        S->SetBoolField(TEXT("in_fellowship"),true);
        TestEqual(TEXT("Fellowship membership permits its consumable buff"),Step(Fellow,S,B)->GetNumberField(TEXT("item")),9.);
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["RingDistance",0.025],["MinimumRingTargets",3]]},{"name":"MyMonsters","columns":["MonsterName","DamageType","WeaponToUse","Attack","Streak","Ring"],"rows":[["<DEFAULT>",3,-1,true,false,true],["Ignored",3,-1,false,false,false],["No ring",3,-1,true,false,false]]}]})"));
        auto B=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Ring profile converts"),Issues.Num(),0);
        TestEqual(TEXT("Ring distance converts map units to meters"),B->GetNumberField(TEXT("ring_range")),6.);
        B->SetBoolField(TEXT("buffing"),false);B->SetBoolField(TEXT("recovery"),false);B->SetStringField(TEXT("combat"),TEXT("magic"));B->SetNumberField(TEXT("radius"),30);
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        auto Orb=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Orb->SetBoolField(TEXT("identified"),true);Orb->SetBoolField(TEXT("can_wield"),true);
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Acid Bolt","category":117,"power":200,"skill":300},{"id":21,"name":"Acid Ring","category":222,"power":100,"skill":300,"scarabs":{"Scarab":1}}]})"))->GetArrayField(TEXT("v")));
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"id":55,"name":"Scarab","type":0,"count":10})"))));S->SetArrayField(TEXT("inventory"),Items);
        auto Targets=SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Target","distance":1,"identified":true},{"id":101,"name":"Target","distance":2,"identified":true},{"id":102,"name":"Target","distance":3,"identified":true}]})"))->GetArrayField(TEXT("v"));S->SetArrayField(TEXT("targets"),Targets);
        auto Choose=[&](){FACEPluginVM VM;VM.Load(Script,Error);return Step(VM,S,B);};
        TestEqual(TEXT("Eligible group prefers ring over stronger single-target spell"),Choose()->GetNumberField(TEXT("spell")),21.);
        Targets[2]->AsObject()->SetBoolField(TEXT("line_of_sight"),false);TestEqual(TEXT("Blocked monster does not satisfy ring count"),Choose()->GetNumberField(TEXT("spell")),20.);
        Targets[2]->AsObject()->SetBoolField(TEXT("line_of_sight"),true);Targets[2]->AsObject()->SetNumberField(TEXT("distance"),6);TestEqual(TEXT("Ring distance uses strict boundary"),Choose()->GetNumberField(TEXT("spell")),20.);
        Targets[2]->AsObject()->SetNumberField(TEXT("distance"),3);Targets[2]->AsObject()->SetStringField(TEXT("name"),TEXT("No ring"));TestEqual(TEXT("Only ring-enabled monster rules count"),Choose()->GetNumberField(TEXT("spell")),20.);
        Targets[2]->AsObject()->SetStringField(TEXT("name"),TEXT("Ignored"));TestEqual(TEXT("Ignored monsters do not count"),Choose()->GetNumberField(TEXT("spell")),20.);
        auto Negative=SettingsJSON(TEXT(R"({"name":"Negative","exact":true,"priority":-1,"bolt":true,"streak":true,"ring":true,"ignore":false})"));
        auto MonsterRules=B->GetArrayField(TEXT("monsters"));MonsterRules.Add(MakeShared<FJsonValueObject>(Negative));B->SetArrayField(TEXT("monsters"),MonsterRules);
        Targets[2]->AsObject()->SetStringField(TEXT("name"),TEXT("Negative"));TestEqual(TEXT("Negative priority excludes even ring-enabled monsters"),Choose()->GetNumberField(TEXT("spell")),20.);
        S->SetArrayField(TEXT("targets"),{Targets[2]});TestFalse(TEXT("Negative priority is never attacked when it is the only target"),Choose()->HasField(TEXT("action")));
        S->SetArrayField(TEXT("targets"),Targets);
        Targets[2]->AsObject()->SetStringField(TEXT("name"),TEXT("Target"));Items.Last()->AsObject()->SetNumberField(TEXT("count"),0);TestEqual(TEXT("Missing ring scarab falls back to ordinary attack"),Choose()->GetNumberField(TEXT("spell")),20.);
        Items.Last()->AsObject()->SetNumberField(TEXT("count"),10);
        auto Rule=B->GetArrayField(TEXT("monsters"))[0]->AsObject();Rule->SetBoolField(TEXT("bolt"),false);S->SetArrayField(TEXT("targets"),{Targets[0]});TestEqual(TEXT("Ring-only profile attacks one nearby target"),Choose()->GetNumberField(TEXT("spell")),21.);
        auto MonsterTable=D->GetArrayField(TEXT("tables"))[1]->AsObject();auto Rows=MonsterTable->GetArrayField(TEXT("rows"));auto FirstRow=Rows[0]->AsArray();FirstRow[3]=MakeShared<FJsonValueBoolean>(false);Rows[0]=MakeShared<FJsonValueArray>(FirstRow);MonsterTable->SetArrayField(TEXT("rows"),Rows);
        auto RingOnly=ACEVTProfile::Convert(D,Issues);TestFalse(TEXT("Disabling ordinary attacks does not ignore ring targets"),RingOnly->GetArrayField(TEXT("monsters"))[0]->AsObject()->GetBoolField(TEXT("ignore")));
        ACEVTProfile::CompileCommand(TEXT("/ucm opt set MinimumRingTargets 1.5"),Issues);TestTrue(TEXT("Ring count must be integral"),Issues.Num()>0);
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"MyMonsters","columns":["MonsterName","DamageType","WeaponToUse","Attack","Streak","Imperil","Vuln","Yield"],"rows":[["<DEFAULT>",3,-1,true,false,true,true,true]]}]})"));
        auto B=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Core debuff strategies convert"),Issues.Num(),0);B->SetBoolField(TEXT("buffing"),false);B->SetBoolField(TEXT("recovery"),false);B->SetStringField(TEXT("combat"),TEXT("magic"));B->SetNumberField(TEXT("radius"),30);
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);auto Orb=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Orb->SetBoolField(TEXT("identified"),true);Orb->SetBoolField(TEXT("can_wield"),true);
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Target","identified":true,"distance":10}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Acid Bolt","category":117,"power":200,"skill":300},{"id":21,"name":"Yield","category":42,"power":100,"skill":300,"duration":120},{"id":22,"name":"Imperil","category":116,"power":100,"skill":300,"duration":120},{"id":23,"name":"Acid Vulnerability","category":102,"power":100,"skill":300,"duration":120}]})"))->GetArrayField(TEXT("v")));
        FACEPluginVM VM;VM.Load(Script,Error);TestEqual(TEXT("Yield precedes other debuffs"),Step(VM,S,B)->GetNumberField(TEXT("spell")),21.);
        {
            FACEPluginVM DeadRecipient;DeadRecipient.Load(Script,Error);Step(DeadRecipient,S,B);
            const auto OriginalTargets=S->GetArrayField(TEXT("targets"));
            S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":101,"name":"Next target","identified":true,"distance":10}]})"))->GetArrayField(TEXT("v")));
            auto Next=Step(DeadRecipient,S,B);
            TestEqual(TEXT("Dead debuff recipient does not stall the next opponent for ten seconds"),Next->GetNumberField(TEXT("target")),101.);
            S->SetArrayField(TEXT("targets"),OriginalTargets);
        }
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("last_spell"),21);TestFalse(TEXT("Successful cast action without effect confirmation waits"),Step(VM,S,B)->HasField(TEXT("action")));
        TArray<TSharedPtr<FJsonValue>> Confirmed;auto Confirm=[&](int Category){auto E=SettingsJSON(TEXT(R"({"target":100,"power":100,"expires":220})"));E->SetNumberField(TEXT("category"),Category);Confirmed.Add(MakeShared<FJsonValueObject>(E));S->SetArrayField(TEXT("debuffs"),Confirmed);};
        Confirm(42);TestEqual(TEXT("Confirmed Yield advances to Imperil"),Step(VM,S,B)->GetNumberField(TEXT("spell")),22.);
        Confirm(116);TestEqual(TEXT("Vulnerability matches selected acid attack"),Step(VM,S,B)->GetNumberField(TEXT("spell")),23.);
        Confirm(102);TestEqual(TEXT("All confirmed debuffs advance to damage"),Step(VM,S,B)->GetNumberField(TEXT("spell")),20.);
        S->SetNumberField(TEXT("time"),216);TestEqual(TEXT("Precast threshold refreshes debuff"),Step(VM,S,B)->GetNumberField(TEXT("spell")),21.);
        S->SetNumberField(TEXT("time"),100);S->SetArrayField(TEXT("debuffs"),{});FACEPluginVM Resists;Resists.Load(Script,Error);Step(Resists,S,B);
        for(int Attempt=1;Attempt<=3;++Attempt){S->SetNumberField(TEXT("debuff_revision"),Attempt);S->SetNumberField(TEXT("debuff_target"),100);S->SetNumberField(TEXT("debuff_spell"),21);auto I=Step(Resists,S,B);TestEqual(TEXT("Resisted debuff retries are bounded"),I->GetStringField(TEXT("action")),Attempt<3?FString(TEXT("cast")):FString(TEXT("activity_failed")));}
        B->SetBoolField(TEXT("debuff_fallback"),true);S->SetArrayField(TEXT("spells"),{S->GetArrayField(TEXT("spells"))[0]});FACEPluginVM Fallback;Fallback.Load(Script,Error);TestEqual(TEXT("Explicit fallback permits damage with unavailable debuffs"),Step(Fallback,S,B)->GetNumberField(TEXT("spell")),20.);
        B->SetBoolField(TEXT("debuff_fallback"),false);FACEPluginVM Strict;Strict.Load(Script,Error);TestEqual(TEXT("Strict debuff policy reports missing spells"),Step(Strict,S,B)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"AssistItems","columns":["Object","Type"],"rows":[["[All Peas]",11]]}]})"));
        ACEVTProfile::Convert(D,Issues);TestTrue(TEXT("Missing recipe data cannot silently accept pea supplies"),Issues.Num()>0);
        auto Recipes=SettingsJSON(TEXT(R"({"v":[{"tool":"Splitting Tool","input":"Pyreal Pea","output":"Pyreal Scarab","count":50}]})"))->GetArrayField(TEXT("v"));D->SetArrayField(TEXT("pea_recipes"),Recipes);
        auto B=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("All Peas imports with recipe data"),Issues.Num(),0);
        B->SetBoolField(TEXT("buffing"),false);B->SetBoolField(TEXT("recovery"),false);B->SetStringField(TEXT("combat"),TEXT("off"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);S->SetArrayField(TEXT("spells"),{});
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Splitting Tool","type":0,"usable":true},{"id":21,"name":"Pyreal Pea","type":0,"count":3},{"id":22,"name":"Pyreal Scarab","type":0,"count":0}]})"))->GetArrayField(TEXT("v")));
        FACEPluginVM VM;VM.Load(Script,Error);TestEqual(TEXT("Splitting first enters peace"),Step(VM,S,B)->GetStringField(TEXT("action")),FString(TEXT("combat_mode")));
        S->SetNumberField(TEXT("combat_mode"),1);auto I=Step(VM,S,B);TestEqual(TEXT("Tool applied through ordinary item action"),I->GetStringField(TEXT("action")),FString(TEXT("apply_item")));TestEqual(TEXT("Tool GUID used"),I->GetNumberField(TEXT("item")),20.);TestEqual(TEXT("Pea GUID targeted"),I->GetNumberField(TEXT("target")),21.);
        S->SetNumberField(TEXT("action_serial"),1);TestFalse(TEXT("UseDone without component increase cannot confirm split"),Step(VM,S,B)->HasField(TEXT("action")));
        S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetNumberField(TEXT("count"),50);TestFalse(TEXT("Component increase completes split without repeat"),Step(VM,S,B)->HasField(TEXT("action")));
        S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetNumberField(TEXT("count"),0);FACEPluginVM Timeout;Timeout.Load(Script,Error);Step(Timeout,S,B);S->SetNumberField(TEXT("time"),116);TestEqual(TEXT("Unconfirmed split pauses its activity rather than consuming more peas"),Step(Timeout,S,B)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        B->SetBoolField(TEXT("split_peas"),false);FACEPluginVM Disabled;Disabled.Load(Script,Error);TestFalse(TEXT("SplitPeas off preserves all supplies"),Step(Disabled,S,B)->HasField(TEXT("action")));
        B->SetBoolField(TEXT("split_peas"),true);B->SetArrayField(TEXT("assist_items"),{MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"name":"Black Opal","type":9})")))});FACEPluginVM Named;Named.Load(Script,Error);TestFalse(TEXT("Specific component excludes other pea recipes"),Step(Named,S,B)->HasField(TEXT("action")));
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["FastCastBuffs",true]]}]})"));
        auto B=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Fast-casting preference imports"),Issues.Num(),0);B->SetBoolField(TEXT("recovery"),false);B->SetStringField(TEXT("combat"),TEXT("off"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":10,"category":1,"power":100,"skill":300,"self_buff":true,"beneficial":true,"caster_target":true,"duration":300,"school":4}]})"))->GetArrayField(TEXT("v")));
        FACEPluginVM VM;VM.Load(Script,Error);auto I=Step(VM,S,B);TestTrue(TEXT("Fast buff uses ordinary cast with explicit movement preference"),I->GetStringField(TEXT("action"))==TEXT("cast")&&I->GetBoolField(TEXT("fast_cast")));
        B->SetBoolField(TEXT("fast_cast_buffs"),false);FACEPluginVM Off;Off.Load(Script,Error);TestFalse(TEXT("Disabled fast buff leaves movement untouched"),Step(Off,S,B)->GetBoolField(TEXT("fast_cast")));
        B->RemoveField(TEXT("fast_cast_buffs"));FACEPluginVM Default;Default.Load(Script,Error);
        TestTrue(TEXT("Existing native profiles without a preference default to fast buff movement"),Step(Default,S,B)->GetBoolField(TEXT("fast_cast")));
        auto Missing=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[]}]})"));
        TestTrue(TEXT("Imports without FastCastBuffs use UCM's enabled default"),ACEVTProfile::Convert(Missing,Issues)->GetBoolField(TEXT("fast_cast_buffs")));
        D->GetArrayField(TEXT("tables"))[0]->AsObject()->SetArrayField(TEXT("rows"),SettingsJSON(TEXT(R"({"v":[["FastCastBuffs",false]]})"))->GetArrayField(TEXT("v")));
        TestFalse(TEXT("An imported explicit opt-out is preserved"),ACEVTProfile::Convert(D,Issues)->GetBoolField(TEXT("fast_cast_buffs")));
    }
    auto Bad=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["UnknownFutureOption",true]]}]})"));
    ACEVTProfile::Convert(Bad,Issues);TestTrue(TEXT("Unsupported active setting blocks whole import"),Issues.Num()>0);
    {
        auto B=SettingsJSON(TEXT(R"({"buffing":true,"auto_buffs":false,"recovery":false,"combat":"off"})"));auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        TArray<TSharedPtr<FJsonValue>> Items,Spells,Buffs;
        for(int Index=0;Index<1020;++Index){auto Item=SettingsJSON(TEXT(R"({"type":0,"usable":true})"));Item->SetNumberField(TEXT("id"),Index+1);Item->SetStringField(TEXT("name"),FString::Printf(TEXT("Carried %d"),Index));Items.Add(MakeShared<FJsonValueObject>(Item));}
        for(int Index=0;Index<512;++Index)
        {
            auto Spell=SettingsJSON(TEXT(R"({"known":false,"flags":4,"power":100})"));Spell->SetNumberField(TEXT("id"),Index+1);Spell->SetNumberField(TEXT("category"),Index+1);Spells.Add(MakeShared<FJsonValueObject>(Spell));
            auto Buff=MakeShared<FJsonObject>();Buff->SetNumberField(TEXT("spell"),Index+1);Buff->SetStringField(TEXT("name"),FString::Printf(TEXT("Absent buff supply %d"),Index));Buffs.Add(MakeShared<FJsonValueObject>(Buff));
        }
        S->SetArrayField(TEXT("inventory"),Items);S->SetArrayField(TEXT("spells"),Spells);B->SetArrayField(TEXT("buff_items"),Buffs);
        FACEPluginVM Large;Large.Load(Script,Error);
        TestFalse(TEXT("512 buff supplies against 1020 items stay within budget without repeated inventory scans"),Step(Large,S,B)->HasField(TEXT("action")));
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"MyMonsters","columns":["MonsterName","AttackPriority","DamageType","WeaponToUse","Attack","Streak"],"rows":[["<DEFAULT>",1,8,-1,true,false],["name == Olthoi && range < 25",10,3,-1,true,true]]}]})"));
        auto M=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Monster predicate compiles"),Issues.Num(),0);
        M->SetBoolField(TEXT("buffing"),false);M->SetBoolField(TEXT("recovery"),false);M->SetNumberField(TEXT("approach_range"),60);M->SetNumberField(TEXT("radius"),60);M->SetStringField(TEXT("combat"),TEXT("magic"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        auto Orb=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Orb->SetBoolField(TEXT("identified"),true);Orb->SetBoolField(TEXT("can_wield"),true);
        auto Spells=SettingsJSON(TEXT(R"({"values":[{"id":11,"name":"Acid Streak","category":243,"power":100,"skill":300},{"id":12,"name":"Fire Bolt","category":121,"power":200,"skill":300}]})"));S->SetArrayField(TEXT("spells"),Spells->GetArrayField(TEXT("values")));
        auto Targets=SettingsJSON(TEXT(R"({"values":[{"id":100,"name":"Olthoi Worker","distance":5,"identified":true},{"id":101,"name":"Olthoi","distance":20,"identified":true}]})"));S->SetArrayField(TEXT("targets"),Targets->GetArrayField(TEXT("values")));
        FACEPluginVM VM;VM.Load(Script,Error);auto I=Step(VM,S,M);
        TestEqual(TEXT("Exact-name predicate outranks nearer prefix name"),I->GetNumberField(TEXT("target")),101.);TestEqual(TEXT("Imported acid choice allows configured streak"),I->GetNumberField(TEXT("spell")),11.);
        S->GetArrayField(TEXT("targets"))[1]->AsObject()->SetNumberField(TEXT("distance"),30);
        FACEPluginVM Far;Far.Load(Script,Error);I=Step(Far,S,M);TestEqual(TEXT("Range predicate falls back to default rule"),I->GetNumberField(TEXT("target")),100.);TestEqual(TEXT("Default disables streak while keeping bolt"),I->GetNumberField(TEXT("spell")),12.);
    }
    {
        auto B=SettingsJSON(TEXT(R"({"buffing":true,"auto_buffs":false,"recovery":false,"combat":"off","item_buff_targets":[{"item":2,"spell":103},{"item":3,"spell":103},{"item":4,"spell":105},{"item":999,"spell":105}]})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"values":[{"id":103,"category":154,"known":false},{"id":104,"category":154,"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":1},{"id":105,"category":155,"power":100,"skill":300,"school":3,"duration":300,"beneficial":true,"target_type":8}]})"))->GetArrayField(TEXT("values")));
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"values":[{"id":1,"name":"Orb","type":32768,"equipped":true},{"id":2,"name":"Sword","type":1},{"id":3,"name":"Sword Two","type":1},{"id":4,"name":"Ring","type":8}]})"))->GetArrayField(TEXT("values")));
        FACEPluginVM VM;VM.Load(Script,Error);TestEqual(TEXT("Imported weapon aura targets player once"),Step(VM,S,B)->GetNumberField(TEXT("target")),1.);
        S->SetArrayField(TEXT("item_buffs"),{MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"category":154,"target":1,"power":100,"expires":1000})")))});
        TestEqual(TEXT("Non-redirectable explicit item enchantment retains item target"),Step(VM,S,B)->GetNumberField(TEXT("target")),4.);
        auto Confirmed=S->GetArrayField(TEXT("item_buffs"));Confirmed.Add(MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"category":155,"target":4,"power":100,"expires":1000})"))));S->SetArrayField(TEXT("item_buffs"),Confirmed);
        TestFalse(TEXT("Confirmed bindings complete and absent inventory GUID is skipped"),Step(VM,S,B)->HasField(TEXT("action")));
    }
    {
        auto Profile=SettingsJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","auto_attack_power":true,"damage_type":2})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Target","distance":1,"identified":true}]})"))->GetArrayField(TEXT("v")));
        auto Sword=SettingsJSON(TEXT(R"({"id":10,"name":"Sword","type":1,"equipped":true,"identified":true,"can_wield":true,"weapon_type":2,"damage":30,"damage_type":3,"attack_type":0})"));
        // This tests swing power with two eligible piercing weapons. Missing
        // damage metadata now correctly requests an equipment change first.
        auto Offhand=SettingsJSON(TEXT(R"({"id":11,"name":"Offhand","type":1,"equipped":true,"equipped_slot":2097152,"identified":true,"can_wield":true,"damage_type":2})"));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Sword)});
        auto Power=[&](){FACEPluginVM V;V.Load(Script,Error);auto I=Step(V,S,Profile);return I->GetNumberField(TEXT("power"));};
        TestEqual(TEXT("Slash/pierce single strike uses 20 percent for pierce"),Power(),.2);
        Sword->SetNumberField(TEXT("attack_type"),64);S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Sword),MakeShared<FJsonValueObject>(Offhand)});
        TestEqual(TEXT("Dual wield multi-strike pierce uses 49 percent"),Power(),.49);
        Offhand->SetNumberField(TEXT("type"),2);TestEqual(TEXT("Shield multi-strike pierce uses full power"),Power(),1.);
        Profile->SetBoolField(TEXT("use_recklessness"),true);S->SetArrayField(TEXT("trained_skills"),{MakeShared<FJsonValueNumber>(50)});TestEqual(TEXT("Trained Recklessness caps automatic power"),Power(),.9);
        Profile->SetBoolField(TEXT("auto_attack_power"),false);Profile->SetNumberField(TEXT("power_percent"),70);TestEqual(TEXT("Manual power remains available"),Power(),.7);
    }
    {
        auto Profile=SettingsJSON(TEXT(R"({"buffing":true,"auto_buffs":true,"recovery":false,"combat":"off","refresh_seconds":60,"idle_buff_topoff":true,"idle_buff_seconds":1200})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":30,"name":"Strength Self","self_buff":true,"category":1,"power":50,"skill":300,"duration":3600,"caster_target":true,"beneficial":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("enchantments"),SettingsJSON(TEXT(R"({"v":[{"id":30,"category":1,"power":50,"remaining":600}]})"))->GetArrayField(TEXT("v")));
        FACEPluginVM Idle;Idle.Load(Script,Error);auto I=Step(Idle,S,Profile);TestEqual(TEXT("Idle topoff refreshes before regular threshold"),I->GetStringField(TEXT("action")),FString(TEXT("cast")));
        Profile->SetBoolField(TEXT("idle_buff_topoff"),false);FACEPluginVM Normal;Normal.Load(Script,Error);TestFalse(TEXT("Disabling topoff retains regular expiry"),Step(Normal,S,Profile)->HasField(TEXT("action")));
    }
    {
        auto Doc=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["EnableBuffing",false],["EnableCombat",false],["MinimumHealKitSuccessChance",95],["UseKitsInMagicMode",true],["GoToPeaceModeToUseKits",true]]}]})"));
        TArray<FString> KitIssues;auto Profile=ACEVTProfile::Convert(Doc,KitIssues);TestEqual(TEXT("Kit options convert"),KitIssues.Num(),0);
        Profile->SetArrayField(TEXT("assist_items"),SettingsJSON(TEXT(R"({"v":[{"name":"Kit","type":0},{"name":"Spare Kit","type":0}]})"))->GetArrayField(TEXT("v")));
        Profile->SetArrayField(TEXT("recharge_handlers"),SettingsJSON(TEXT(R"({"v":[{"vital":2,"stance":1,"handler":"Kit Recharge","min":0,"max":100},{"vital":2,"stance":2,"handler":"Kit Recharge","min":0,"max":100}]})"))->GetArrayField(TEXT("v")));
        auto S=Snapshot();S->SetArrayField(TEXT("spells"),{});
        auto Kit=SettingsJSON(TEXT(R"({"id":10,"name":"Kit","type":0,"identified":true,"usable":true,"healing_kit":true,"structure":20,"boost_vital":2,"boost":30,"heal_mod":2})"));
        auto Spare=SettingsJSON(TEXT(R"({"id":11,"name":"Spare Kit","type":0,"identified":true,"usable":true,"healing_kit":true,"structure":5,"boost_vital":2,"boost":30,"heal_mod":2})"));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Kit),MakeShared<FJsonValueObject>(Spare)});
        auto Choose=[&](){FACEPluginVM V;V.Load(Script,Error);return Step(V,S,Profile);};
        auto I=Choose();TestEqual(TEXT("Kit enters peace before use"),I->GetStringField(TEXT("action")),FString(TEXT("combat_mode")));TestEqual(TEXT("Peace mode request"),I->GetNumberField(TEXT("mode")),1.);
        S->SetNumberField(TEXT("combat_mode"),1);I=Choose();TestEqual(TEXT("Equal quality kits use lowest remaining charges first"),I->GetNumberField(TEXT("item")),11.);
        Kit->SetNumberField(TEXT("heal_mod"),3);I=Choose();TestEqual(TEXT("Healing modifier outranks remaining charges"),I->GetNumberField(TEXT("item")),10.);
        S->GetObjectField(TEXT("skills"))->GetObjectField(TEXT("21"))->SetNumberField(TEXT("current"),130);
        TestFalse(TEXT("Fifty percent estimate rejects a kit below configured minimum"),Choose()->HasField(TEXT("action")));
        Profile->SetNumberField(TEXT("kit_min_success"),50);TestEqual(TEXT("Exact chance threshold accepts kit"),Choose()->GetNumberField(TEXT("item")),10.);
        S->SetNumberField(TEXT("combat_mode"),8);TestFalse(TEXT("Combat difficulty can put same kit below threshold"),Choose()->HasField(TEXT("action")));
        Profile->SetNumberField(TEXT("kit_min_success"),0);Profile->SetBoolField(TEXT("kits_in_magic"),false);TestFalse(TEXT("Magic stance kit prohibition wins over peace preference"),Choose()->HasField(TEXT("action")));
        S->SetNumberField(TEXT("combat_mode"),1);S->SetNumberField(TEXT("stamina"),14);TestFalse(TEXT("Kit needs stamina before attempting health recovery"),Choose()->HasField(TEXT("action")));
    }
    {
        auto Profile=SettingsJSON(TEXT(R"({"buffing":false,"recovery":true,"combat":"off","helper_health_threshold":60,"helper_stamina_threshold":40,"helper_mana_threshold":20,"helper_health_range":30})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("stamina"),100);
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":30,"name":"Heal Other","category":79,"power":50,"skill":300,"duration":0,"caster_target":false,"beneficial":true},{"id":31,"name":"Revitalize Other","category":81,"power":50,"skill":300,"duration":0,"caster_target":false,"beneficial":true}]})"))->GetArrayField(TEXT("v")));
        auto F=SettingsJSON(TEXT(R"({"valid":true,"members":[{"id":2,"name":"Ally","health":20,"max_health":100,"stamina":10,"max_stamina":100,"mana":100,"max_mana":100,"vitals_age":1,"distance":10,"line_of_sight":true},{"id":3,"name":"Far Ally","health":5,"max_health":100,"vitals_age":1,"distance":60,"line_of_sight":true}]})"));S->SetObjectField(TEXT("fellowship"),F);
        FACEPluginVM Help;Help.Load(Script,Error);auto I=Step(Help,S,Profile);
        TestTrue(TEXT("Helper requests server updates"),I->GetBoolField(TEXT("helper_updates")));TestEqual(TEXT("Helper skips more injured out-of-range fellow"),I->GetNumberField(TEXT("target")),2.);TestEqual(TEXT("Helper prioritizes health"),I->GetNumberField(TEXT("spell")),30.);
        S->SetNumberField(TEXT("action_serial"),1);TestFalse(TEXT("UseDone without improved fellow vital cannot repeat cast"),Step(Help,S,Profile)->HasField(TEXT("action")));
        auto Ally=F->GetArrayField(TEXT("members"))[0]->AsObject();Ally->SetNumberField(TEXT("health"),100);I=Step(Help,S,Profile);TestEqual(TEXT("Observed heal allows stamina recovery next"),I->GetNumberField(TEXT("spell")),31.);
        auto Choose=[&](){FACEPluginVM V;V.Load(Script,Error);return Step(V,S,Profile);};
        Ally->SetNumberField(TEXT("vitals_age"),10);TestFalse(TEXT("Ten-second-old fellowship data rejected"),Choose()->HasField(TEXT("action")));
        Ally->SetNumberField(TEXT("vitals_age"),1);Ally->SetBoolField(TEXT("line_of_sight"),false);TestFalse(TEXT("Helper does not cast through wall"),Choose()->HasField(TEXT("action")));
        Ally->SetBoolField(TEXT("line_of_sight"),true);Ally->SetNumberField(TEXT("health"),0);TestFalse(TEXT("Dead fellow is not a recovery target"),Choose()->HasField(TEXT("action")));
        Ally->SetNumberField(TEXT("health"),100);F->SetBoolField(TEXT("valid"),false);TestFalse(TEXT("Disbanded fellowship stops helping"),Choose()->HasField(TEXT("action")));
        Profile->SetNumberField(TEXT("helper_health_threshold"),0);Profile->SetNumberField(TEXT("helper_stamina_threshold"),0);Profile->SetNumberField(TEXT("helper_mana_threshold"),0);TestFalse(TEXT("Disabled helper releases update subscription"),Choose()->GetBoolField(TEXT("helper_updates")));
    }
    {
        auto Doc=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["SummonPets",true],["PetRangeMode",1],["PetCustomRange",1],["PetMonsterDensity",2]]}]})"));
        TArray<FString> PetIssues;auto Profile=ACEVTProfile::Convert(Doc,PetIssues);TestEqual(TEXT("Pet settings convert"),PetIssues.Num(),0);
        TestEqual(TEXT("Pet custom range uses map units"),Profile->GetNumberField(TEXT("pet_range")),240.);
        Profile->SetBoolField(TEXT("buffing"),false);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetNumberField(TEXT("radius"),1);Profile->SetNumberField(TEXT("approach_range"),1);
        Profile->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(20),MakeShared<FJsonValueNumber>(21)});
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("level"),200);S->SetNumberField(TEXT("combat_mode"),1);
        S->SetArrayField(TEXT("spells"),{});S->GetObjectField(TEXT("skills"))->SetObjectField(TEXT("54"),SettingsJSON(TEXT(R"({"current":400,"training":2})")));
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Target","distance":10,"resists":{"16":1.5,"8":0.5}},{"id":101,"name":"Other","distance":20}]})"))->GetArrayField(TEXT("v")));
        auto Fire=SettingsJSON(TEXT(R"({"id":20,"name":"Fire Essence","type":0,"wcid":1000,"identified":true,"usable":true,"structure":20,"max_structure":50,"icon_effects":32,"int_properties":{"280":213,"369":150,"366":54,"367":350}})"));
        auto Cold=SettingsJSON(TEXT(R"({"id":21,"name":"Cold Essence","type":0,"wcid":1001,"identified":true,"usable":true,"structure":20,"max_structure":50,"icon_effects":128,"int_properties":{"280":213,"369":180,"366":54,"367":350}})"));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Fire),MakeShared<FJsonValueObject>(Cold)});
        auto Choose=[&](){FACEPluginVM V;V.Load(Script,Error);return Step(V,S,Profile);};
        FACEPluginVM Pet;Pet.Load(Script,Error);auto I=Step(Pet,S,Profile);TestEqual(TEXT("Element suitability outranks pet level"),I->GetNumberField(TEXT("item")),20.);
        S->SetNumberField(TEXT("action_serial"),1);TestFalse(TEXT("UseDone alone does not repeat a pet summon"),Step(Pet,S,Profile)->HasField(TEXT("action")));
        S->SetObjectField(TEXT("cooldowns"),SettingsJSON(TEXT(R"({"213":40})")));TestFalse(TEXT("Server cooldown confirms and blocks repeated summoning"),Step(Pet,S,Profile)->HasField(TEXT("action")));
        S->SetObjectField(TEXT("cooldowns"),MakeShared<FJsonObject>());S->SetBoolField(TEXT("owned_pet"),true);TestFalse(TEXT("Living pet is not dismissed by automation"),Choose()->HasField(TEXT("action")));
        S->SetBoolField(TEXT("owned_pet"),false);
        for(int32 Mode:{1,2,4,8})
        {
            S->SetNumberField(TEXT("combat_mode"),Mode);const auto Summon=Choose();
            TestEqual(TEXT("Pet use preserves peace/melee/missile/magic stance"),Summon->GetStringField(TEXT("action")),FString(TEXT("use_item")));
            TestEqual(TEXT("Combat summon still selects the best eligible essence"),Summon->GetNumberField(TEXT("item")),20.);
        }
        S->SetNumberField(TEXT("combat_mode"),1);
        Fire->SetNumberField(TEXT("structure"),0);TestEqual(TEXT("Empty essence falls back to charged essence"),Choose()->GetNumberField(TEXT("item")),21.);
        Cold->GetObjectField(TEXT("int_properties"))->SetNumberField(TEXT("362"),1);TestFalse(TEXT("Wrong mastery excluded"),Choose()->HasField(TEXT("action")));
        Cold->GetObjectField(TEXT("int_properties"))->RemoveField(TEXT("362"));Cold->GetObjectField(TEXT("int_properties"))->SetNumberField(TEXT("368"),54);TestFalse(TEXT("Skill specialization requirement is a skill ID"),Choose()->HasField(TEXT("action")));
        Cold->GetObjectField(TEXT("int_properties"))->RemoveField(TEXT("368"));S->SetNumberField(TEXT("level"),100);TestFalse(TEXT("Insufficient level excluded"),Choose()->HasField(TEXT("action")));S->SetNumberField(TEXT("level"),200);
        S->GetObjectField(TEXT("skills"))->GetObjectField(TEXT("54"))->SetNumberField(TEXT("current"),300);TestFalse(TEXT("Insufficient summoning excluded"),Choose()->HasField(TEXT("action")));S->GetObjectField(TEXT("skills"))->GetObjectField(TEXT("54"))->SetNumberField(TEXT("current"),400);
        S->GetArrayField(TEXT("targets"))[1]->AsObject()->SetBoolField(TEXT("line_of_sight"),false);TestFalse(TEXT("Wall-hidden monster does not satisfy density"),Choose()->HasField(TEXT("action")));S->GetArrayField(TEXT("targets"))[1]->AsObject()->SetBoolField(TEXT("line_of_sight"),true);
        Profile->SetNumberField(TEXT("pet_range_mode"),0);TestFalse(TEXT("Attack-distance range respected"),Choose()->HasField(TEXT("action")));Profile->SetNumberField(TEXT("pet_range_mode"),1);
        Profile->SetBoolField(TEXT("summon_pets"),false);TestFalse(TEXT("Summoning disabled"),Choose()->HasField(TEXT("action")));Profile->SetBoolField(TEXT("summon_pets"),true);
        FACEPluginVM Timeout;Timeout.Load(Script,Error);Step(Timeout,S,Profile);S->SetNumberField(TEXT("time"),116);
        TestFalse(TEXT("Unconfirmed summon does not disable other activities"),Step(Timeout,S,Profile)->HasField(TEXT("action")));
        // Every retail tier/mastery uses its own advertised requirements, not a
        // hardcoded minimum or a mandatory membership in the equipment pool.
        Profile->SetArrayField(TEXT("weapon_items"),{});S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Cold)});
        S->SetObjectField(TEXT("char_ints"),MakeShared<FJsonObject>());
        for(int32 Level:{15,30,50,80,100,125,150,180,200})for(int32 Mastery:{0,1,2,3})
        {
            auto Props=Cold->GetObjectField(TEXT("int_properties"));Props->SetNumberField(TEXT("369"),Level);Props->SetNumberField(TEXT("362"),Mastery);
            S->GetObjectField(TEXT("char_ints"))->SetNumberField(TEXT("362"),Mastery);S->SetNumberField(TEXT("level"),Level);
            TestEqual(TEXT("Essence casts at its exact level/mastery boundary with automatic pool"),Choose()->GetNumberField(TEXT("item")),21.);
            S->SetNumberField(TEXT("level"),Level-1);TestFalse(TEXT("Essence above character level is excluded"),Choose()->HasField(TEXT("action")));
        }
        auto Props=Cold->GetObjectField(TEXT("int_properties"));S->SetNumberField(TEXT("level"),200);Props->SetNumberField(TEXT("280"),999);
        TestEqual(TEXT("Custom summoning cooldown is recognized by skill requirement"),Choose()->GetNumberField(TEXT("item")),21.);
        S->SetObjectField(TEXT("cooldowns"),SettingsJSON(TEXT(R"({"999":30})")));
        TestFalse(TEXT("Custom cooldown prevents reuse"),Choose()->HasField(TEXT("action")));S->SetObjectField(TEXT("cooldowns"),MakeShared<FJsonObject>());
        Props->RemoveField(TEXT("366"));Props->SetNumberField(TEXT("368"),54);
        auto Skill=S->GetObjectField(TEXT("skills"))->GetObjectField(TEXT("54"));Skill->SetNumberField(TEXT("training"),3);Skill->SetNumberField(TEXT("current"),350);
        TestEqual(TEXT("Specialization-only requirement supports exact skill boundary"),Choose()->GetNumberField(TEXT("item")),21.);
        Skill->SetNumberField(TEXT("current"),349);TestFalse(TEXT("Specialization-only essence checks skill level too"),Choose()->HasField(TEXT("action")));Skill->SetNumberField(TEXT("current"),350);
        Cold->SetNumberField(TEXT("structure"),0);TestFalse(TEXT("Exhausted essence is unavailable"),Choose()->HasField(TEXT("action")));
        Cold->SetNumberField(TEXT("max_structure"),0);TestEqual(TEXT("Uncharged unlimited custom device is usable"),Choose()->GetNumberField(TEXT("item")),21.);
        Cold->SetNumberField(TEXT("structure"),20);Cold->SetNumberField(TEXT("max_structure"),50);
        Fire->SetNumberField(TEXT("structure"),20);S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Fire),MakeShared<FJsonValueObject>(Cold)});
        FACEPluginVM Fallback;Fallback.Load(Script,Error);auto First=Step(Fallback,S,Profile);S->SetNumberField(TEXT("action_serial"),2);S->SetNumberField(TEXT("action_error"),1);
        auto Second=Step(Fallback,S,Profile);TestTrue(TEXT("Rejected summon falls back to a different eligible essence"),First->GetNumberField(TEXT("item"))!=Second->GetNumberField(TEXT("item")));
        // Server acknowledgement can precede the spawn/cooldown messages.
        // Keep fighting while that confirmation is pending.
        Profile->SetNumberField(TEXT("approach_range"),60);Profile->SetNumberField(TEXT("radius"),30);
        Profile->SetNumberField(TEXT("pet_min_targets"),1);Profile->SetStringField(TEXT("combat"),TEXT("melee"));
        auto Sword=SettingsJSON(TEXT(R"({"id":40,"name":"Sword","type":1,"identified":true,"can_wield":true,"equipped":true,"damage":30,"damage_type":1})"));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Fire),MakeShared<FJsonValueObject>(Sword)});
        for(auto T:S->GetArrayField(TEXT("targets"))) {T->AsObject()->SetNumberField(TEXT("distance"),1);T->AsObject()->SetBoolField(TEXT("identified"),true);}
        S->SetNumberField(TEXT("action_error"),0);S->SetNumberField(TEXT("combat_mode"),2);
        FACEPluginVM Pending;Pending.Load(Script,Error);
        TestEqual(TEXT("Summon is submitted before continuing combat"),Step(Pending,S,Profile)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
        S->SetNumberField(TEXT("action_serial"),3);
        TestEqual(TEXT("Unconfirmed pet spawn never stalls available combat"),Step(Pending,S,Profile)->GetStringField(TEXT("action")),FString(TEXT("attack")));
    }
    {
        TArray<FString> LocalIssues;auto Doc=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["BuffProfile_Prots",1],["BuffProfile-Prots","BPS"],["BuffProfile_Banes",3]]}]})"));
        auto Profile=ACEVTProfile::Convert(Doc,LocalIssues);TestEqual(TEXT("Element buff profiles convert including custom letters"),LocalIssues.Num(),0);
        Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("off"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);S->SetArrayField(TEXT("inventory"),{});
        auto Acid=SettingsJSON(TEXT(R"({"id":80,"name":"Acid Protection Self","category":101,"skill":300,"power":50,"beneficial":true,"self_buff":true,"caster_target":true,"duration":300})"));
        auto Bludgeon=SettingsJSON(TEXT(R"({"id":81,"name":"Bludgeon Protection Self","category":103,"skill":300,"power":50,"beneficial":true,"self_buff":true,"caster_target":true,"duration":300})"));
        auto Bane=SettingsJSON(TEXT(R"({"id":82,"name":"Acid Bane","category":162,"school":3,"skill":300,"power":50,"beneficial":true,"caster_target":true,"duration":300})"));
        auto Armor=SettingsJSON(TEXT(R"({"id":83,"name":"Armor Self","category":115,"skill":300,"power":50,"beneficial":true,"self_buff":true,"caster_target":true,"duration":300})"));
        S->RemoveField(TEXT("inventory"));S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Acid),MakeShared<FJsonValueObject>(Bludgeon),MakeShared<FJsonValueObject>(Bane),MakeShared<FJsonValueObject>(Armor)});
        auto Choose=[&](){FACEPluginVM V;V.Load(Script,Error);return Step(V,S,Profile);};
        TestEqual(TEXT("Custom BPS skips acid but includes bludgeon"),Choose()->GetNumberField(TEXT("spell")),81.);
        Profile->SetNumberField(TEXT("protection_profile"),3);TestEqual(TEXT("None skips element buffs but keeps armor"),Choose()->GetNumberField(TEXT("spell")),83.);
        Profile->SetNumberField(TEXT("bane_profile"),2);TestEqual(TEXT("Banes can be enabled independently"),Choose()->GetNumberField(TEXT("spell")),82.);
        Profile->SetArrayField(TEXT("buffs"),{MakeShared<FJsonValueNumber>(80)});TestEqual(TEXT("Explicit extra buff overrides automatic element profile"),Choose()->GetNumberField(TEXT("spell")),80.);
    }
    {
        auto Profile=SettingsJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","melee_range":5,"target_lock":false,"target_select":1,"target_angle_range":5,"radius":30})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);S->SetArrayField(TEXT("spells"),{});
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Sword","type":1,"identified":true,"can_wield":true,"equipped":true,"damage_type":1,"damage":10}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Near left","distance":2,"angle":90,"identified":true},{"id":101,"name":"Forward","distance":3,"angle":0,"identified":true}]})"))->GetArrayField(TEXT("v")));
        auto Choose=[&](){FACEPluginVM V;V.Load(Script,Error);return Step(V,S,Profile)->GetNumberField(TEXT("target"));};
        TestEqual(TEXT("Distance preference selects nearer monster"),Choose(),100.);Profile->SetNumberField(TEXT("target_select"),2);TestEqual(TEXT("Angle preference selects forward monster"),Choose(),101.);
        Profile->SetNumberField(TEXT("target_select"),3);TestEqual(TEXT("Both uses angle inside threshold"),Choose(),101.);Profile->SetNumberField(TEXT("target_angle_range"),1);TestEqual(TEXT("Both uses distance outside threshold"),Choose(),100.);
        Profile->SetNumberField(TEXT("minimum_range"),2.5);TestEqual(TEXT("Minimum range excludes too-close monster"),Choose(),101.);
        TArray<FString> LocalIssues;auto C=ACEVTProfile::CompileCommand(TEXT("/vt opt set TargetSelectAngleRange .1"),LocalIssues);TestEqual(TEXT("Target angle range converts map distance"),C->GetNumberField(TEXT("value")),24.);
    }
    {
        TArray<FString> LocalIssues;auto C=ACEVTProfile::CompileCommand(TEXT("/vt opt set BlacklistCorpseOpenAttemptCount 2"),LocalIssues);TestEqual(TEXT("Corpse attempt limit converts"),C->GetNumberField(TEXT("value")),2.);
        auto Profile=SettingsJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[{"action":"keep"}],"corpse_attempts":2,"corpse_blacklist_seconds":50})"));auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("corpses"),SettingsJSON(TEXT(R"({"v":[{"id":200,"name":"Corpse","distance":1,"identified":true}]})"))->GetArrayField(TEXT("v")));
        FACEPluginVM Loot;Loot.Load(Script,Error);TestEqual(TEXT("First corpse use"),Step(Loot,S,Profile)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        auto Receipt=SettingsJSON(TEXT(R"({"type":"use","source":200,"action_serial":1,"requested_at":100,"error":1})"));S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});
        S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),1);TestFalse(TEXT("Server rejection releases corpse approach without immediate retry"),Step(Loot,S,Profile)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),110);TestEqual(TEXT("Corpse retry after attempt spacing"),Step(Loot,S,Profile)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
        S->SetNumberField(TEXT("action_serial"),2);Receipt->SetNumberField(TEXT("action_serial"),2);Receipt->SetNumberField(TEXT("requested_at"),110);Step(Loot,S,Profile);S->SetNumberField(TEXT("time"),159);TestFalse(TEXT("Configured blacklist duration observed"),Step(Loot,S,Profile)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),160);TestEqual(TEXT("Expired blacklist permits new attempt cycle"),Step(Loot,S,Profile)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
    }
    {
        auto Profile=SettingsJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"magic","radius":30,"use_arcs":1,"arc_range":5})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Target","distance":10,"identified":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":30,"name":"Bolt","category":117,"power":100,"skill":400},{"id":31,"name":"Arc","category":117,"flags":4096,"power":100,"skill":400}]})"))->GetArrayField(TEXT("v")));
        auto Choose=[&](){FACEPluginVM VM;VM.Load(Script,Error);return Step(VM,S,Profile)->GetNumberField(TEXT("spell"));};
        TestEqual(TEXT("No arcs prefers equal-power bolt"),Choose(),30.);
        Profile->SetNumberField(TEXT("use_arcs"),2);TestEqual(TEXT("Range preference uses distant arc"),Choose(),31.);
        S->GetArrayField(TEXT("targets"))[0]->AsObject()->SetNumberField(TEXT("distance"),4);TestEqual(TEXT("Range preference uses nearby bolt"),Choose(),30.);
        Profile->SetNumberField(TEXT("use_arcs"),3);TestEqual(TEXT("Yes prefers equal-power arc nearby"),Choose(),31.);
        Profile->SetNumberField(TEXT("use_arcs"),1);S->GetArrayField(TEXT("spells"))[1]->AsObject()->SetNumberField(TEXT("power"),200);TestEqual(TEXT("Higher-quality arc wins even with No preference"),Choose(),31.);
        Profile->SetNumberField(TEXT("attack_spell"),30);TestEqual(TEXT("Explicit lower spell survives compaction"),Choose(),30.);
        TArray<FString> LocalIssues;auto C=ACEVTProfile::CompileCommand(TEXT("/vt opt set ArcRange .1"),LocalIssues);TestEqual(TEXT("Arc range converts map units"),C->GetNumberField(TEXT("value")),24.);
    }
    {
        auto Profile=SettingsJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"magic","radius":30,"debuff_each_first":3,"debuff_imperil":true,"monsters":[{"name":"First","exact":true,"priority":2},{"name":"Second","exact":true,"priority":1}]})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"First","distance":2,"identified":true},{"id":101,"name":"Second","distance":3,"identified":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":30,"category":117,"power":100,"skill":400},{"id":31,"category":116,"power":100,"skill":400,"duration":120}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("debuffs"),SettingsJSON(TEXT(R"({"v":[{"target":100,"category":116,"power":100,"expires":220}]})"))->GetArrayField(TEXT("v")));
        FACEPluginVM All;All.Load(Script,Error);auto I=Step(All,S,Profile);TestEqual(TEXT("All debuffs lower-priority target before attacking confirmed target"),I->GetNumberField(TEXT("target")),101.);TestEqual(TEXT("All casts requested debuff"),I->GetNumberField(TEXT("spell")),31.);
        S->SetNumberField(TEXT("action_serial"),1);TestFalse(TEXT("Group selection cannot bypass pending confirmation"),Step(All,S,Profile)->HasField(TEXT("action")));
        auto Effects=S->GetArrayField(TEXT("debuffs"));Effects.Add(MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"target":101,"category":116,"power":100,"expires":220})"))));S->SetArrayField(TEXT("debuffs"),Effects);
        I=Step(All,S,Profile);TestEqual(TEXT("After group debuffs attacks highest-priority target"),I->GetNumberField(TEXT("target")),100.);TestEqual(TEXT("After group debuffs casts damage"),I->GetNumberField(TEXT("spell")),30.);
        Effects.Pop();S->SetArrayField(TEXT("debuffs"),Effects);Profile->SetNumberField(TEXT("debuff_each_first"),2);FACEPluginVM Priority;Priority.Load(Script,Error);TestEqual(TEXT("Priority does not pre-debuff a lower-priority target"),Step(Priority,S,Profile)->GetNumberField(TEXT("spell")),30.);
        Profile->GetArrayField(TEXT("monsters"))[1]->AsObject()->SetNumberField(TEXT("priority"),2);FACEPluginVM Same;Same.Load(Script,Error);TestEqual(TEXT("Priority pre-debuffs another equal-priority target"),Step(Same,S,Profile)->GetNumberField(TEXT("target")),101.);
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["EnableBuffing",false]]},{"name":"MyMonsters","columns":["MonsterName","AttackPriority"],"rows":[["hasshield==1",9],["<DEFAULT>",1]]}]})"));
        auto Profile=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Attached armor predicate imports"),Issues.Num(),0);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("magic"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Bare","distance":2,"identified":true,"has_shield":false},{"id":101,"name":"Armored","distance":3,"identified":true,"has_shield":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":30,"category":117,"power":100,"skill":400}]})"))->GetArrayField(TEXT("v")));
        FACEPluginVM VM;VM.Load(Script,Error);TestEqual(TEXT("Armor predicate changes monster priority"),Step(VM,S,Profile)->GetNumberField(TEXT("target")),101.);
        TArray<FString> LocalIssues;auto Say=ACEVTProfile::CompileCommand(TEXT("Now summoning Aetheria Dungeon"),LocalIssues);TestEqual(TEXT("Plain meta chat retains speech semantics"),Say->GetStringField(TEXT("op")),FString(TEXT("say")));TestEqual(TEXT("Plain speech accepted"),LocalIssues.Num(),0);
        ACEVTProfile::CompileCommand(TEXT("/unknownplugin do something"),LocalIssues);TestTrue(TEXT("Unknown slash command still blocks import"),LocalIssues.Num()>0);
        Profile->SetBoolField(TEXT("debuff_imperil"),true);Profile->SetArrayField(TEXT("monsters"),{});Profile->SetBoolField(TEXT("switch_debuff_wand"),true);
        auto Items=S->GetArrayField(TEXT("inventory"));auto Better=SettingsJSON(TEXT(R"({"id":21,"name":"Better wand","type":32768,"identified":true,"can_wield":true,"equipped":false,"damage_type":32,"element_mod":2})"));Items.Add(MakeShared<FJsonValueObject>(Better));S->SetArrayField(TEXT("inventory"),Items);
        auto Spells=S->GetArrayField(TEXT("spells"));Spells.Add(MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"id":31,"category":116,"duration":120,"power":100,"skill":400})"))));S->SetArrayField(TEXT("spells"),Spells);
        FACEPluginVM Switch;Switch.Load(Script,Error);TestEqual(TEXT("Enabled wand choice equips combat wand before debuff"),Step(Switch,S,Profile)->GetNumberField(TEXT("item")),21.);
        Profile->SetBoolField(TEXT("switch_debuff_wand"),false);FACEPluginVM Keep;Keep.Load(Script,Error);TestEqual(TEXT("Disabled wand choice casts with current wand"),Step(Keep,S,Profile)->GetNumberField(TEXT("spell")),31.);
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["EnableBuffing",false]]},{"name":"MyMonsters","columns":["MonsterName","AttackPriority"],"rows":[["species==Olthoi&&maxhp>1000",9],["<DEFAULT>",1]]}]})"));
        ACEVTProfile::Convert(D,Issues);TestTrue(TEXT("Species predicate requires its database dependency"),Issues.Num()>0);
        D->SetObjectField(TEXT("monster_catalog"),SettingsJSON(TEXT(R"({"Veteran":{"species":1,"max_health":2000},"Scout":{"species":2,"max_health":100}})")));
        auto Profile=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Species and maxhp import with catalog"),Issues.Num(),0);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("magic"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);S->SetObjectField(TEXT("species_names"),SettingsJSON(TEXT(R"({"1":"Olthoi","2":"Rabbit"})")));
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Scout","distance":2,"identified":true},{"id":101,"name":"Veteran","distance":3,"identified":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":30,"category":117,"power":100,"skill":400}]})"))->GetArrayField(TEXT("v")));
        auto Choose=[&](){FACEPluginVM VM;VM.Load(Script,Error);return Step(VM,S,Profile)->GetNumberField(TEXT("target"));};
        TestEqual(TEXT("Imported database drives species and maxhp rule"),Choose(),101.);
        S->GetArrayField(TEXT("targets"))[1]->AsObject()->SetNumberField(TEXT("max_health"),50);TestEqual(TEXT("Server maximum health overrides legacy database"),Choose(),100.);
        S->GetArrayField(TEXT("targets"))[1]->AsObject()->SetNumberField(TEXT("max_health"),2000);S->GetArrayField(TEXT("targets"))[1]->AsObject()->SetNumberField(TEXT("creature_type"),2);TestEqual(TEXT("Server creature type overrides legacy database"),Choose(),100.);
        Profile->SetObjectField(TEXT("monster_catalog"),MakeShared<FJsonObject>());TestEqual(TEXT("Unknown catalog entries preserve VT unknown defaults"),Choose(),100.);
    }
    {
        auto Profile=SettingsJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"magic","radius":30,"debuff_each_first":3,"debuff_imperil":true,"target_lock":false})"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        TArray<TSharedPtr<FJsonValue>> Targets,Effects,Items,Spells;
        for(int32 Index=0;Index<128;++Index)
        {
            auto Target=SettingsJSON(TEXT(R"({"name":"Crowd monster","identified":true})"));Target->SetNumberField(TEXT("id"),1000+Index);Target->SetNumberField(TEXT("distance"),2+Index*.01);Targets.Add(MakeShared<FJsonValueObject>(Target));
            if(Index<127){auto Effect=SettingsJSON(TEXT(R"({"category":116,"power":100,"expires":500})"));Effect->SetNumberField(TEXT("target"),1000+Index);Effects.Add(MakeShared<FJsonValueObject>(Effect));}
        }
        for(int32 Index=0;Index<64;++Index)
        {auto Item=SettingsJSON(TEXT(R"({"name":"Wand","type":32768,"identified":true,"can_wield":true})"));Item->SetNumberField(TEXT("id"),20+Index);Item->SetBoolField(TEXT("equipped"),Index==0);Items.Add(MakeShared<FJsonValueObject>(Item));}
        for(int32 Index=0;Index<512;++Index)
        {auto Spell=SettingsJSON(TEXT(R"({"category":117,"power":100,"skill":400})"));Spell->SetNumberField(TEXT("id"),300+Index);Spells.Add(MakeShared<FJsonValueObject>(Spell));}
        Spells.Add(MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"id":31,"category":116,"power":100,"skill":400,"duration":120})"))));
        S->SetArrayField(TEXT("targets"),Targets);S->SetArrayField(TEXT("debuffs"),Effects);S->SetArrayField(TEXT("inventory"),Items);S->SetArrayField(TEXT("spells"),Spells);
        FACEPluginVM VM;VM.Load(Script,Error);TSharedPtr<FJsonObject> I;
        int32 Slices=0;for(;Slices<128;++Slices){I=Step(VM,S,Profile);if(I->HasField(TEXT("action")))break;}
        TestEqual(TEXT("Confirmed fixed families avoid crowd loadout scans"),Slices,0);
        TestEqual(TEXT("Crowd analysis finishes within bounded slices"),I->GetStringField(TEXT("action")),FString(TEXT("cast")));
        TestEqual(TEXT("Crowd analysis finds last monster missing debuff"),I->GetNumberField(TEXT("target")),1127.);
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["EnableBuffing",false],["SummonPets",false]]},{"name":"MyMonsters","columns":["MonsterName","WeaponToUse","SecondaryEquip","SecondaryVuln"],"rows":[["<DEFAULT>",-2147483600,1,3]]}]})"));
        auto Profile=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Explicit equipment and secondary vuln convert"),Issues.Num(),0);Profile->SetBoolField(TEXT("recovery"),false);
        auto Rule=Profile->GetArrayField(TEXT("monsters"))[0]->AsObject();
        TestEqual(TEXT("Signed primary GUID normalized"),Rule->GetNumberField(TEXT("weapon")),2147483696.);
        TestEqual(TEXT("Secondary acid enum maps to protocol element"),Rule->GetNumberField(TEXT("secondary_vuln")),32.);
        Rule->RemoveField(TEXT("secondary_vuln"));Profile->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(21)});
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Monster","distance":1,"identified":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":2147483696,"name":"Chosen sword","type":1,"slots":1048576,"identified":true,"can_wield":true,"damage":10,"damage_type":1},{"id":21,"name":"Strong sword","type":1,"slots":1048576,"identified":true,"can_wield":true,"damage":100,"damage_type":1,"equipped":true,"equipped_slot":1048576},{"id":22,"name":"Shield","type":2,"slots":2097152,"identified":true,"can_wield":true}]})"))->GetArrayField(TEXT("v")));
        auto Choose=[&](){FACEPluginVM VM;VM.Load(Script,Error);return Step(VM,S,Profile);};
        TestEqual(TEXT("Explicit owned weapon overrides automatic inventory filter and score"),Choose()->GetNumberField(TEXT("item")),2147483696.);
        Rule->SetNumberField(TEXT("weapon"),999);TestEqual(TEXT("Missing explicit weapon falls back to automatic"),Choose()->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Rule->SetNumberField(TEXT("weapon"),2147483696.);Profile->SetArrayField(TEXT("weapon_items"),{});
        auto Items=S->GetArrayField(TEXT("inventory"));Items[0]->AsObject()->SetBoolField(TEXT("equipped"),true);Items[0]->AsObject()->SetNumberField(TEXT("equipped_slot"),1048576);Items[1]->AsObject()->SetBoolField(TEXT("equipped"),false);
        auto I=Choose();TestEqual(TEXT("Automatic shield equips into offhand"),I->GetNumberField(TEXT("item")),22.);TestTrue(TEXT("Offhand request is explicit"),I->GetBoolField(TEXT("offhand")));
        Items[2]->AsObject()->SetBoolField(TEXT("equipped"),true);Items[2]->AsObject()->SetNumberField(TEXT("equipped_slot"),2097152);
        TestEqual(TEXT("Observed offhand permits attack"),Choose()->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Rule->SetNumberField(TEXT("secondary_equip"),3);TestTrue(TEXT("None removes currently equipped shield"),Choose()->GetBoolField(TEXT("unequip")));
        Rule->SetNumberField(TEXT("secondary_equip"),2);I=Choose();TestEqual(TEXT("Dual wield chooses other eligible weapon"),I->GetNumberField(TEXT("item")),21.);TestTrue(TEXT("Secondary weapon requests shield location"),I->GetBoolField(TEXT("offhand")));
        Rule->SetNumberField(TEXT("secondary_equip"),22);TestEqual(TEXT("Explicit shield already equipped does not loop"),Choose()->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Items[0]->AsObject()->SetNumberField(TEXT("slots"),33554432);TestEqual(TEXT("Two handed loadout skips offhand"),Choose()->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Items[0]->AsObject()->SetNumberField(TEXT("slots"),1048576);Items[0]->AsObject()->SetNumberField(TEXT("equipped_slot"),2097152);TestEqual(TEXT("Explicit primary in offhand moves to main hand"),Choose()->GetNumberField(TEXT("item")),2147483696.);
        Items[0]->AsObject()->SetNumberField(TEXT("equipped_slot"),1048576);
        Items[1]->AsObject()->SetBoolField(TEXT("auto_wield_left"),true);
        Rule->RemoveField(TEXT("weapon"));Rule->RemoveField(TEXT("secondary_equip"));
        I=Choose();TestEqual(TEXT("Stronger tethered weapon remains in the offhand"),I->GetNumberField(TEXT("item")),21.);TestTrue(TEXT("Tether chooses offhand without a monster rule"),I->GetBoolField(TEXT("offhand")));
        Items[1]->AsObject()->SetBoolField(TEXT("equipped"),true);Items[1]->AsObject()->SetNumberField(TEXT("equipped_slot"),2097152);Items[2]->AsObject()->SetBoolField(TEXT("equipped"),false);
        TestEqual(TEXT("Confirmed dual wield loadout attacks without swapping hands"),Choose()->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Profile->SetNumberField(TEXT("melee_weapon"),2147483696.);Profile->SetNumberField(TEXT("melee_secondary_equip"),3);
        TestTrue(TEXT("Default empty offhand overrides automatic tether"),Choose()->GetBoolField(TEXT("unequip")));
        Profile->SetNumberField(TEXT("melee_secondary_equip"),21);TestEqual(TEXT("Default explicit offhand already equipped proceeds"),Choose()->GetStringField(TEXT("action")),FString(TEXT("attack")));
        Profile->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(21)});
        Items[0]->AsObject()->SetBoolField(TEXT("equipped"),false);
        TestEqual(TEXT("Default main hand overrides imported weapon filter"),Choose()->GetNumberField(TEXT("item")),2147483696.);
        Items[0]->AsObject()->SetBoolField(TEXT("equipped"),true);Profile->SetArrayField(TEXT("weapon_items"),{});
        Rule->SetNumberField(TEXT("secondary_equip"),22);TestEqual(TEXT("Monster offhand override wins over default loadout"),Choose()->GetNumberField(TEXT("item")),22.);
        Rule->RemoveField(TEXT("secondary_equip"));Profile->RemoveField(TEXT("melee_secondary_equip"));Profile->RemoveField(TEXT("melee_weapon"));
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("spells"),SettingsJSON(TEXT(R"({"v":[{"id":30,"category":117,"power":100,"skill":400},{"id":31,"category":102,"power":100,"skill":400,"duration":120}]})"))->GetArrayField(TEXT("v")));
        Rule->SetNumberField(TEXT("weapon"),0);Rule->SetNumberField(TEXT("secondary_vuln"),32);
        TestEqual(TEXT("Secondary vulnerability casts independently of primary toggle"),Choose()->GetNumberField(TEXT("spell")),31.);
        S->SetArrayField(TEXT("debuffs"),SettingsJSON(TEXT(R"({"v":[{"target":100,"category":102,"power":100,"expires":500}]})"))->GetArrayField(TEXT("v")));
        TestEqual(TEXT("Confirmed secondary vulnerability proceeds to damage"),Choose()->GetNumberField(TEXT("spell")),30.);
    }
    {
        auto D=SettingsJSON(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["EnableBuffing",false]]},{"name":"MyMonsters","columns":["MonsterName","Yield","WeakeningCurse","FesteringCurse","Corruption","DestructiveCurse","Corrosion","Imperil","GravityW","Broadside","Fester"],"rows":[["<DEFAULT>",true,true,true,true,true,true,true,true,true,true]]}]})"));
        auto Profile=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Creature and void enchantment debuffs import"),Issues.Num(),0);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("magic"));
        auto S=Snapshot();S->SetNumberField(TEXT("health"),100);
        S->SetArrayField(TEXT("inventory"),SettingsJSON(TEXT(R"({"v":[{"id":20,"name":"Wand","type":32768,"identified":true,"can_wield":true,"equipped":true}]})"))->GetArrayField(TEXT("v")));
        S->SetArrayField(TEXT("targets"),SettingsJSON(TEXT(R"({"v":[{"id":100,"name":"Monster","distance":2,"identified":true}]})"))->GetArrayField(TEXT("v")));
        TArray<TSharedPtr<FJsonValue>> Spells,Effects;
        // DAT-verified in VoidCombat: Corruption=638, Destructive Curse=636,
        // Corrosion=637. Keep this imported-profile chain in the same order.
        const int32 Categories[]={42,642,643,638,636,637,116,78,40,96};
        for(int32 Cat:Categories){auto Spell=SettingsJSON(TEXT(R"({"power":100,"skill":400,"duration":120})"));Spell->SetNumberField(TEXT("category"),Cat);Spell->SetNumberField(TEXT("id"),Cat+1000);Spells.Add(MakeShared<FJsonValueObject>(Spell));}
        Spells.Add(MakeShared<FJsonValueObject>(SettingsJSON(TEXT(R"({"id":30,"category":117,"power":100,"skill":400})"))));S->SetArrayField(TEXT("spells"),Spells);
        FACEPluginVM VM;VM.Load(Script,Error);
        for(int32 Cat:Categories)
        {
            TestEqual(*FString::Printf(TEXT("Retail debuff order category %d"),Cat),Step(VM,S,Profile)->GetNumberField(TEXT("spell")),double(Cat+1000));
            auto Effect=SettingsJSON(TEXT(R"({"target":100,"power":100,"expires":500})"));Effect->SetNumberField(TEXT("category"),Cat);Effects.Add(MakeShared<FJsonValueObject>(Effect));S->SetArrayField(TEXT("debuffs"),Effects);
        }
        TestEqual(TEXT("Confirmed debuff chain finishes with attack"),Step(VM,S,Profile)->GetNumberField(TEXT("spell")),30.);
        Effects[3]->AsObject()->SetNumberField(TEXT("expires"),101);S->SetArrayField(TEXT("debuffs"),Effects);
        FACEPluginVM BeforeDOTExpiry;BeforeDOTExpiry.Load(Script,Error);
        TestEqual(TEXT("DOT is not refreshed early by ordinary debuff threshold"),Step(BeforeDOTExpiry,S,Profile)->GetNumberField(TEXT("spell")),30.);
        Effects[3]->AsObject()->SetNumberField(TEXT("expires"),100);S->SetArrayField(TEXT("debuffs"),Effects);
        FACEPluginVM AtDOTExpiry;AtDOTExpiry.Load(Script,Error);
        TestEqual(TEXT("DOT is recast at expiry before normal damage"),Step(AtDOTExpiry,S,Profile)->GetNumberField(TEXT("spell")),1638.);
        Effects[3]->AsObject()->SetNumberField(TEXT("expires"),500);
        Effects[7]->AsObject()->SetNumberField(TEXT("expires"),104);S->SetArrayField(TEXT("debuffs"),Effects);
        FACEPluginVM Refresh;Refresh.Load(Script,Error);TestEqual(TEXT("Gravity family refresh threshold honored"),Step(Refresh,S,Profile)->GetNumberField(TEXT("spell")),1078.);
    }
    return true;
}
#endif
