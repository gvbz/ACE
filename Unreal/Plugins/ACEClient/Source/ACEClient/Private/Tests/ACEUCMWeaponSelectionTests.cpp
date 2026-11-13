#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEPluginCombat.h"
#include "Mods/ACEPluginEquipment.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEUCMProfiles.h"
#include "Mods/ACEVTProfile.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMWeaponSelectionTest,"ACE.Plugins.VTankWeaponSelection",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMWeaponSelectionTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* T){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(T),O);return O;};
    FString Script,Error;
    FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto State=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"combat_mode":8,"components_required":false,"skills":{"34":{"training":3,"current":400},"44":{"training":3,"current":400},"47":{"training":3,"current":400}},"spells":[],"targets":[{"id":50,"name":"Snow Tusker","identified":true,"distance":1,"creature_type":1,"line_of_sight":true}],"inventory":[]})"));};
    auto Profile=[&](){return Json(TEXT(R"({"buffing":false,"recovery":false,"looting":false,"combat":"auto","attack_spell":0})"));};
    auto SetItems=[&](auto S,const TCHAR* T){S->SetArrayField(TEXT("inventory"),Json(T)->GetArrayField(TEXT("items")));};
    auto Step=[&](auto S,auto P)
    {
        FACEPluginVM VM;TestTrue(TEXT("Shipped UCM compiles"),VM.Load(Script,Error));TSharedPtr<FJsonObject> I;
        TestTrue(*FString::Printf(TEXT("Shipped UCM evaluates: %s"),*Error),VM.Step(S,P,I,Error));
        if(!Error.IsEmpty())AddError(Error);return I?I:MakeShared<FJsonObject>();
    };
    auto Item=[&](auto I,int32 Expected,const TCHAR* Why){TestEqual(Why,I->GetNumberField(TEXT("item")),double(Expected));};
    // VT eDamageElement.Cold=5/Fire=6 are not the protocol's Cold=8/Fire=16.
    // Use the same appraisal bits the server sends, never UI-derived fixtures.
    auto S=State(),P=Profile();auto Target=S->GetArrayField(TEXT("targets"))[0]->AsObject();
    Target->SetArrayField(TEXT("preferred_elements"),ACEPluginCombat::PreferredElements(TEXT("Snow Tusker")));
    TestEqual(TEXT("VT Snow Tusker weakness maps to retail fire bit"),Target->GetArrayField(TEXT("preferred_elements"))[0]->AsNumber(),16.);
    SetItems(S,TEXT(R"({"items":[{"id":90,"name":"Cold sword","type":1,"damage_type":8,"damage":100,"identified":true,"can_wield":true,"equipped":true},{"id":91,"name":"Fire sword","type":1,"damage_type":16,"damage":20,"identified":true,"can_wield":true}]})"));
    Item(Step(S,P),91,TEXT("Fire weakness beats higher displayed cold damage"));
    auto Items=S->GetArrayField(TEXT("inventory"));auto Cold=Items[0]->AsObject(),Fire=Items[1]->AsObject();
    Cold->SetBoolField(TEXT("equipped"),false);Cold->SetNumberField(TEXT("slayer"),1);Cold->SetNumberField(TEXT("damage"),1);
    Item(Step(S,P),90,TEXT("Matching species slayer has VT's first priority"));
    Cold->RemoveField(TEXT("slayer"));Target->SetArrayField(TEXT("preferred_elements"),{MakeShared<FJsonValueNumber>(16),MakeShared<FJsonValueNumber>(8)});
    Cold->SetObjectField(TEXT("int_properties"),Json(TEXT(R"({"179":128})")));
    Item(Step(S,P),90,TEXT("Listed elemental rend outranks an unimbued preferred element"));
    Fire->SetObjectField(TEXT("int_properties"),Json(TEXT(R"({"263":16})")));
    Item(Step(S,P),91,TEXT("Fire cleave and cold rend follow ordered monster elements"));
    Fire->SetObjectField(TEXT("int_properties"),Json(TEXT(R"({"179":4})")));
    Item(Step(S,P),90,TEXT("Elemental rend precedes armor rend before vulnerability"));
    S->SetArrayField(TEXT("debuffs"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"target":50,"category":110,"power":100,"expires":101})")))});
    Item(Step(S,P),91,TEXT("Confirmed fire vulnerability promotes matching armor-rend weapon even inside refresh window"));
    S->SetArrayField(TEXT("debuffs"),{});P->SetNumberField(TEXT("damage_type"),16);
    Item(Step(S,P),91,TEXT("Explicit fire rule cannot equip a cold rend"));
    P->RemoveField(TEXT("damage_type"));Cold->RemoveField(TEXT("int_properties"));Fire->RemoveField(TEXT("int_properties"));
    Target->SetObjectField(TEXT("resists"),Json(TEXT(R"({"8":2,"16":0.5})")));
    Item(Step(S,P),90,TEXT("Custom server resistance data overrides stock monster database"));

    S=State();P=Profile();P->SetStringField(TEXT("combat"),TEXT("melee"));P->SetNumberField(TEXT("melee_weapon"),90);P->SetNumberField(TEXT("melee_secondary_equip"),2);
    Target=S->GetArrayField(TEXT("targets"))[0]->AsObject();Target->SetArrayField(TEXT("preferred_elements"),{MakeShared<FJsonValueNumber>(16),MakeShared<FJsonValueNumber>(8)});
    SetItems(S,TEXT(R"({"items":[{"id":90,"name":"Main hand","type":1,"damage_type":1,"identified":true,"can_wield":true,"equipped":true,"equipped_slot":1048576},{"id":91,"name":"Cold-rending offhand","type":1,"damage_type":8,"damage":10,"identified":true,"can_wield":true,"int_properties":{"179":128}},{"id":92,"name":"Fire offhand","type":1,"damage_type":16,"damage":100,"identified":true,"can_wield":true}]})"));
    auto Offhand=Step(S,P);Item(Offhand,91,TEXT("Automatic offhand applies the same rend priority"));TestTrue(TEXT("Secondary weapon goes in left hand"),Offhand->GetBoolField(TEXT("offhand")));
    S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetObjectField(TEXT("int_properties"),Json(TEXT(R"({"263":16})")));
    Item(Step(S,P),92,TEXT("Offhand cleave follows ordered elements alongside rends"));
    auto Side=S->GetArrayField(TEXT("inventory"))[1]->AsObject(),OtherSide=S->GetArrayField(TEXT("inventory"))[2]->AsObject();
    Side->RemoveField(TEXT("int_properties"));OtherSide->RemoveField(TEXT("int_properties"));
    Side->SetNumberField(TEXT("damage_type"),16);Side->SetNumberField(TEXT("damage"),10);Side->SetNumberField(TEXT("damage_mod"),20);
    Item(Step(S,P),91,TEXT("Offhand tie-break retains server damage modifier"));
    auto Main=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Main->SetNumberField(TEXT("type"),256);Main->SetNumberField(TEXT("ammo_type"),0);Main->SetNumberField(TEXT("equipped_slot"),ACEEquipMask::MissileWeapon);
    P->SetStringField(TEXT("combat"),TEXT("missile"));
    auto Thrown=Step(S,P);Item(Thrown,91,TEXT("Thrown main hand supports automatic offhand like VT"));TestTrue(TEXT("Thrown secondary goes in offhand"),Thrown->GetBoolField(TEXT("offhand")));

    // All seven elemental casters, through the native appraisal snapshot and
    // actual Lua policy. Wand elements live in quality 45, with no weapon profile.
    auto* GI=NewObject<UGameInstance>(GEngine);GI->InitializeStandalone();
    ON_SCOPE_EXIT{GI->Shutdown();};
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    const int32 Bits[]={32,4,8,64,16,2,1};
    for(int32 Index=0;Index<7;++Index)
    {
        S=State();P=Profile();P->SetStringField(TEXT("combat"),TEXT("magic"));
        Target=S->GetArrayField(TEXT("targets"))[0]->AsObject();Target->SetArrayField(TEXT("preferred_elements"),{MakeShared<FJsonValueNumber>(Bits[Index])});
        TArray<TSharedPtr<FJsonValue>> Wands,Spells;
        for(int32 N=0;N<7;++N)
        {
            FACEWorldObject W;W.Guid=90+N;W.Name=TEXT("Elemental wand");W.ItemType=ACEItemType::Caster;
            FACEAppraisalInfo A;A.bSuccess=true;A.IntProperties.Add(45,Bits[N]);A.FloatProperties.Add(152,1.2);
            Host->Appraisals.Add(W.Guid,A);auto J=Host->PluginItemSnapshot(W)->AsObject();
            TestEqual(TEXT("Native wand snapshot reads integer quality 45"),J->GetNumberField(TEXT("damage_type")),double(Bits[N]));
            J->SetBoolField(TEXT("can_wield"),true);J->SetBoolField(TEXT("equipped"),N==Index);Wands.Add(MakeShared<FJsonValueObject>(J));
            auto Spell=Json(TEXT(R"({"school":1,"power":100,"skill":400})"));Spell->SetNumberField(TEXT("id"),1000+N);Spell->SetNumberField(TEXT("category"),117+N);
            Spells.Add(MakeShared<FJsonValueObject>(Spell));
        }
        S->SetArrayField(TEXT("inventory"),Wands);S->SetArrayField(TEXT("spells"),Spells);
        auto I=Step(S,P);TestEqual(TEXT("Selected wand casts corresponding retail element"),I->GetNumberField(TEXT("spell")),double(1000+Index));
        Wands[Index]->AsObject()->SetBoolField(TEXT("equipped"),false);
        Item(Step(S,P),90+Index,TEXT("Monster element selects the matching elemental wand"));
    }
    // A background refresh must not make an assessed pierce wand disappear
    // from the loadout, and force-Pierce must not silently use a blunt wand.
    S=State();P=Profile();P->SetStringField(TEXT("combat"),TEXT("magic"));P->SetNumberField(TEXT("damage_type"),2);
    SetItems(S,TEXT(R"({"items":[{"id":90,"name":"Blunt wand","type":32768,"damage_type":4,"identified":true,"can_wield":true,"equipped":true,"element_mod":3},{"id":91,"name":"Pierce wand","type":32768,"damage_type":2,"identified":false,"combat_identified":true,"can_wield":true,"combat_int_properties":{"179":16},"element_mod":2}]})"));
    S->SetArrayField(TEXT("spells"),Json(TEXT(R"({"items":[{"id":1,"category":122,"school":1,"power":100,"skill":400},{"id":2,"category":118,"school":1,"power":100,"skill":400}]})"))->GetArrayField(TEXT("items")));
    Item(Step(S,P),91,TEXT("Force pierce selects cached piercing wand over equipped blunt wand"));
    S->GetArrayField(TEXT("inventory"))[1]->AsObject()->SetBoolField(TEXT("combat_identified"),false);
    auto Unknown=Step(S,P);TestEqual(TEXT("Never-assessed piercing candidate is inspected before wrong-element attack"),Unknown->GetStringField(TEXT("action")),FString(TEXT("identify")));
    Item(Unknown,91,TEXT("Inspect the unknown candidate rather than silently using blunt"));
    // Native editor expressions use the same bounded AST as imported VT rules.
    S=State();P=Profile();P->SetStringField(TEXT("combat"),TEXT("melee"));
    S->SetObjectField(TEXT("species_names"),Json(TEXT(R"({"1":"Tusker"})")));
    SetItems(S,TEXT(R"({"items":[{"id":90,"name":"Sword","type":1,"damage_type":1,"identified":true,"can_wield":true,"equipped":true,"damage":30}]})"));
    for(const TCHAR* Expression:{TEXT("species==Tusker"),TEXT("name==Snow Tusker"),TEXT("species==Tusker && name==Snow Tusker"),TEXT("name==No match || species==Tusker"),TEXT("name#`^Snow.*`")})
    {
        auto Rule=MakeShared<FJsonObject>();TestTrue(TEXT("Native mob expression compiles"),ACEVTProfile::SetMonsterMatch(Rule,Expression,Error));
        Rule->SetNumberField(TEXT("priority"),-1);P->SetArrayField(TEXT("monsters"),{MakeShared<FJsonValueObject>(Rule)});
        FString Action;Step(S,P)->TryGetStringField(TEXT("action"),Action);
        TestNotEqual(TEXT("Matching expression skips target marked ignored"),Action,FString(TEXT("attack")));
        S->GetArrayField(TEXT("targets"))[0]->AsObject()->SetStringField(TEXT("name"),TEXT("Other creature"));
        S->GetArrayField(TEXT("targets"))[0]->AsObject()->SetNumberField(TEXT("creature_type"),2);
        TestEqual(TEXT("Nonmatching expression leaves target attackable"),Step(S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        S->GetArrayField(TEXT("targets"))[0]->AsObject()->SetStringField(TEXT("name"),TEXT("Snow Tusker"));
        S->GetArrayField(TEXT("targets"))[0]->AsObject()->SetNumberField(TEXT("creature_type"),1);
    }
    auto Editable=MakeShared<FJsonObject>();ACEVTProfile::SetMonsterMatch(Editable,TEXT("species==Tusker"),Error);
    TestFalse(TEXT("Incomplete expression rejected"),ACEVTProfile::SetMonsterMatch(Editable,TEXT("species=="),Error));
    TestEqual(TEXT("Invalid edit retains prior rule"),Editable->GetStringField(TEXT("name")),FString(TEXT("species==Tusker")));
    TestFalse(TEXT("Action calls cannot run in a mob expression"),ACEVTProfile::SetMonsterMatch(Editable,TEXT("name==actiontryuseitem[1]"),Error));
    TestTrue(TEXT("Expression can be changed back to a plain name"),ACEVTProfile::SetMonsterMatch(Editable,TEXT("Snow"),Error));
    TestFalse(TEXT("Plain prefix no longer carries old expression"),Editable->HasField(TEXT("expression")));

    // Reported custom wand identity, through the native snapshot and real Lua.
    // Zero physical weapon-profile fields are normal for a caster. Custom GUIDs
    // and bonuses must not require a retail WCID/name whitelist.
    FACEWorldObject Custom;Custom.Guid=int32(0xF06A016Au);Custom.WeenieClassId=29260;
    Custom.Name=TEXT("Blunt Sceptre");Custom.ItemType=ACEItemType::Caster;Custom.ValidLocations=0x1000000;
    // Gameplay properties from the user's UB dump; character counts are a
    // separate fixture, since the dump describes the item, not its owner.
    FACEAppraisalInfo CustomAppraisal;CustomAppraisal.bSuccess=true;
    CustomAppraisal.IntProperties={{45,4},{179,32},{166,82},{273,13},{274,9007},{275,5500},{276,13},{277,9009},{278,3500}};
    CustomAppraisal.FloatProperties={{152,2.9275233745575},{144,0.200000002980232},{29,1.20000004768372},{9056,1.91069092857143}};
    Host->Appraisals.Add(Custom.Guid,CustomAppraisal);
    auto Wand=Host->PluginItemSnapshot(Custom)->AsObject();
    TestFalse(TEXT("Pre-login zero owner IDs do not mark a wand equipped"),Wand->GetBoolField(TEXT("equipped")));
    TestEqual(TEXT("Custom wand GUID survives the native snapshot"),Wand->GetNumberField(TEXT("id")),double(0xF06A016Au));
    TestEqual(TEXT("Custom caster bonus survives without a weapon profile"),Wand->GetNumberField(TEXT("element_mod")),2.9275233745575);
    TestTrue(TEXT("Unreported custom augmentation counts permit a server-validated equip attempt"),Wand->GetBoolField(TEXT("can_wield")));
    TestTrue(TEXT("Native snapshot distinguishes unknown counts from confirmed eligibility"),Wand->GetBoolField(TEXT("wield_requires_server_check")));
    FACEPlayerVitals Qualifying;Qualifying.QualityInt64s={{9007,5500},{9009,3500}};
    for(bool CountsKnown:{false,true})for(const TCHAR* Mode:{TEXT("auto"),TEXT("magic")})for(int32 Choice=0;Choice<3;++Choice)
    {
        bool RequiresServerCheck=false;
        Wand->SetBoolField(TEXT("can_wield"),ACEPluginEquipment::WieldFailure(CustomAppraisal,CountsKnown?Qualifying:FACEPlayerVitals(),&RequiresServerCheck).IsEmpty());
        Wand->SetBoolField(TEXT("wield_requires_server_check"),RequiresServerCheck);
        TestEqual(TEXT("Only missing counts defer the requirement check"),RequiresServerCheck,!CountsKnown);
        S=State();P=Profile();auto Rule=Json(TEXT(R"({"default":true,"bolt":true,"damage_type":4})"));Rule->SetStringField(TEXT("combat"),Mode);
        if(Choice==1)P->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(uint32(Custom.Guid))});
        if(Choice==2)Rule->SetNumberField(TEXT("weapon"),uint32(Custom.Guid));
        P->SetArrayField(TEXT("monsters"),{MakeShared<FJsonValueObject>(Rule)});
        auto Melee=Json(TEXT(R"({"id":90,"name":"Melee weapon","type":1,"damage_type":1,"damage":5,"identified":true,"can_wield":true})"));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Melee),MakeShared<FJsonValueObject>(Wand)});
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":1001,"category":118,"school":1,"power":100,"skill":400})")))});
        Wand->SetBoolField(TEXT("equipped"),false);
        TestEqual(TEXT("Auto, selected pool and explicit custom caster all equip"),Step(S,P)->GetNumberField(TEXT("item")),double(uint32(Custom.Guid)));
        Wand->SetBoolField(TEXT("equipped"),true);
        TestEqual(TEXT("Custom blunt caster attacks with bludgeoning magic"),Step(S,P)->GetNumberField(TEXT("spell")),1001.);
        if(Choice==2&&FString(Mode)==TEXT("magic"))
        {
            Rule->SetNumberField(TEXT("damage_type"),0);
            Rule->SetNumberField(TEXT("weapon"),90);
            auto I=Step(S,P);
            TestEqual(TEXT("Magic-only mode never equips an explicitly conflicting melee weapon"),I->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
            TestTrue(TEXT("Conflicting weapon mode is explained"),I->GetStringField(TEXT("status")).Contains(TEXT("does not match magic mode")));
        }
    }
    Wand->SetBoolField(TEXT("equipped"),false);Wand->SetBoolField(TEXT("can_wield"),false);
    Wand->SetStringField(TEXT("wield_reason"),TEXT("Wield requirement 2: boolean property 999 must be true (have false)"));
    P->SetArrayField(TEXT("monsters"),{});P->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(uint32(Custom.Guid))});
    auto Blocked=Step(S,P);
    TestTrue(TEXT("A blocked custom wand names its failed requirement"),Blocked->GetStringField(TEXT("status")).Contains(TEXT("Blunt Sceptre: Wield requirement 2: boolean property 999")));
    // Neutral rend wand chooses rend's spell, not its absent damage profile.
    S=State();P=Profile();P->SetStringField(TEXT("combat"),TEXT("magic"));
    Target=S->GetArrayField(TEXT("targets"))[0]->AsObject();Target->SetArrayField(TEXT("preferred_elements"),{MakeShared<FJsonValueNumber>(16)});
    SetItems(S,TEXT(R"({"items":[{"id":90,"name":"Fire-rending wand","type":32768,"damage_type":0,"identified":true,"can_wield":true,"equipped":true,"int_properties":{"179":512}}]})"));
    S->SetArrayField(TEXT("spells"),Json(TEXT(R"({"items":[{"id":1,"category":119,"school":1,"power":300,"skill":400},{"id":2,"category":121,"school":1,"power":100,"skill":400}]})"))->GetArrayField(TEXT("items")));
    TestEqual(TEXT("Fire-rending wand uses fire despite higher-tier cold bolt"),Step(S,P)->GetNumberField(TEXT("spell")),2.);
    P->SetBoolField(TEXT("debuff_vuln"),true);
    auto Spells=S->GetArrayField(TEXT("spells"));Spells.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":3,"category":110,"school":2,"power":100,"skill":400,"duration":300})"))));S->SetArrayField(TEXT("spells"),Spells);
    TestEqual(TEXT("Wand plan selects the matching fire vulnerability"),Step(S,P)->GetNumberField(TEXT("spell")),3.);

    // Bows, crossbows and atlatls: select a complete loadout, then keep its ammo.
    for(int32 Kind:{1,2,4})
    {
        S=State();P=Profile();P->SetStringField(TEXT("combat"),TEXT("missile"));
        Target=S->GetArrayField(TEXT("targets"))[0]->AsObject();Target->SetArrayField(TEXT("preferred_elements"),{MakeShared<FJsonValueNumber>(16),MakeShared<FJsonValueNumber>(8)});
        SetItems(S,TEXT(R"({"items":[{"id":90,"name":"Cold launcher","type":256,"damage_type":8,"damage_mod":5,"identified":true,"can_wield":true},{"id":91,"name":"Fire launcher","type":256,"damage_type":16,"damage_mod":2,"identified":true,"can_wield":true},{"id":92,"name":"Prismatic ammo","type":256,"slots":8388608,"damage_type":268435456,"damage":20,"identified":true,"can_wield":true}]})"));
        auto Inv=S->GetArrayField(TEXT("inventory"));for(auto V:Inv)V->AsObject()->SetNumberField(TEXT("ammo_type"),Kind);
        Item(Step(S,P),91,TEXT("Prismatic ammo inherits the selected fire launcher"));
        Inv[1]->AsObject()->SetBoolField(TEXT("equipped"),true);
        Item(Step(S,P),92,TEXT("Selected loadout equips its prismatic ammunition"));
        Inv[2]->AsObject()->SetBoolField(TEXT("equipped"),true);
        TestEqual(TEXT("Equipped matching loadout attacks"),Step(S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
        P->SetNumberField(TEXT("damage_type"),16);Inv[1]->AsObject()->SetNumberField(TEXT("damage_type"),2);
        TestEqual(TEXT("Neutral bow cannot pretend its prismatic ammo is fire"),Step(S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    }
    // Imported VT settings use enum indices, native settings use protocol bits.
    TArray<FString> Issues;
    auto Converted=ACEVTProfile::Convert(Json(TEXT(R"({"format":"usd","tables":[{"name":"MyMonsters","columns":["MonsterName","DamageType","SecondaryVuln"],"rows":[["name == Cold",5,6],["name == Fire",6,5]]}]})")),Issues);
    TestTrue(TEXT("VT element rules import without warnings"),Converted.IsValid()&&Issues.IsEmpty());
    if(Converted)
    {
        const auto& Rules=Converted->GetArrayField(TEXT("monsters"));
        if(TestEqual(TEXT("Both VT monster rules import"),Rules.Num(),2))for(int32 N=0;N<2;++N)
        {
            TestEqual(TEXT("VT cold/fire enum converts to retail damage"),Rules[N]->AsObject()->GetNumberField(TEXT("damage_type")),N?16.:8.);
            TestEqual(TEXT("VT secondary vulnerability uses the same mapping"),Rules[N]->AsObject()->GetNumberField(TEXT("secondary_vuln")),N?8.:16.);
        }
        ACEUCMProfiles::NormalizeDamageTypes(Converted);
        TestEqual(TEXT("New imports carry a current damage format"),Converted->GetNumberField(TEXT("monster_damage_version")),2.);
    }
    using namespace ACEUCMProfiles;
    auto Legacy=Json(TEXT(R"({"damage_type":8,"monsters":[{"name":"Rat","damage_type":16,"secondary_vuln":8,"pet_damage_type":6}],"loot_rules":[{"conditions":[{"key":45,"value":8}]}]})"));
    NormalizeDamageTypes(Legacy);
    TestEqual(TEXT("Old Fire choice migrates to retail Fire"),Legacy->GetNumberField(TEXT("damage_type")),16.);
    auto Rule=Legacy->GetArrayField(TEXT("monsters"))[0]->AsObject();
    TestEqual(TEXT("Old Cold monster choice migrates"),Rule->GetNumberField(TEXT("damage_type")),8.);
    TestEqual(TEXT("Secondary Fire vulnerability migrates"),Rule->GetNumberField(TEXT("secondary_vuln")),16.);
    TestEqual(TEXT("Pet VT enum is not a protocol mask"),Rule->GetNumberField(TEXT("pet_damage_type")),6.);
    TestEqual(TEXT("Loot property comparisons remain raw server values"),Legacy->GetArrayField(TEXT("loot_rules"))[0]->AsObject()->GetArrayField(TEXT("conditions"))[0]->AsObject()->GetNumberField(TEXT("value")),8.);
    NormalizeDamageTypes(Legacy);TestEqual(TEXT("Repeated migration cannot flip Fire back"),Legacy->GetNumberField(TEXT("damage_type")),16.);
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/WeaponSelection")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Root,false,true);};
    TestTrue(TEXT("Shared defaults created"),EnsureDefaults(Root,Legacy,Error));
    TSharedPtr<FJsonObject> Refs=MakeShared<FJsonObject>();for(const auto& Section:Sections())TestTrue(TEXT("Shared sections selected"),Select(Root,Section,TEXT("Default"),Legacy,Refs,Error));
    TestTrue(TEXT("Migrated selection saved"),SaveSelection(Root,Legacy,Refs,Error));
    TSharedPtr<FJsonObject> Reloaded,Links;TestTrue(TEXT("Migrated selection reloads"),Load(Root,ActiveName(),Reloaded,Links,Error));
    if(Reloaded)TestEqual(TEXT("Fire choice survives save/reload without another swap"),Reloaded->GetNumberField(TEXT("damage_type")),16.);
    FFileHelper::SaveStringToFile(TEXT(R"({"monsters":[{"name":"Legacy","damage_type":8,"secondary_vuln":16}]})"),*(Root/TEXT("Profiles/Monsters/Old.json")));
    TestTrue(TEXT("Switching to an old independent Monsters file migrates it"),Select(Root,TEXT("Monsters"),TEXT("Old"),Reloaded,Links,Error));
    if(Reloaded)
    {
        auto Migrated=Reloaded->GetArrayField(TEXT("monsters"))[0]->AsObject();
        TestEqual(TEXT("Old shared Fire rule maps correctly"),Migrated->GetNumberField(TEXT("damage_type")),16.);
        TestEqual(TEXT("Old shared Cold secondary maps correctly"),Migrated->GetNumberField(TEXT("secondary_vuln")),8.);
        TestEqual(TEXT("Selecting legacy Monsters does not re-migrate Character"),Reloaded->GetNumberField(TEXT("damage_type")),16.);
        TestTrue(TEXT("Migrated shared file saves"),Save(Root,ActiveName(),Reloaded,Links,nullptr,Error));
        TestTrue(TEXT("Saved shared file reloads"),Load(Root,ActiveName(),Reloaded,Links,Error));
        TestEqual(TEXT("Shared version survives reload without flipping again"),Reloaded->GetArrayField(TEXT("monsters"))[0]->AsObject()->GetNumberField(TEXT("damage_type")),16.);
    }
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMWieldRequirementsTest,"ACE.Plugins.CustomWieldRequirements",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMWieldRequirementsTest::RunTest(const FString&)
{
    FACEPlayerVitals V;V.Level=300;V.HeritageGroup=2;V.StatQualityInts.Add(2,31);V.StatQualityInts.Add(999,12);V.QualityBools.Add(999,true);
    V.Strength=400;V.BaseMaxHealth=500;V.MaxHealth=600;
    FACESkillInfo Skill;Skill.SkillId=34;Skill.Base=600;Skill.Current=650;Skill.AdvancementClass=3;V.Skills.Add(Skill);
    // ACE checks all four requirement slots, including BoolStat and CreatureType
    // which are uncommon in retail loot but supported on custom servers.
    V.QualityInt64s.Add(9007,5500);
    const int32 Kinds[]={1,2,4,5,6,7,8,9,10,11,12,13};
    const int32 Ids[]={34,34,1,1,1,0,34,999,999,0,0,9007};
    const int32 Values[]={650,600,400,600,500,300,3,12,1,31,2,5500};
    for(int32 Key:{158,270,273,276})for(int32 N=0;N<UE_ARRAY_COUNT(Kinds);++N)
    {
        FACEAppraisalInfo A;A.bSuccess=true;A.IntProperties.Add(Key,Kinds[N]);A.IntProperties.Add(Key+1,Ids[N]);A.IntProperties.Add(Key+2,Values[N]);
        TestTrue(*FString::Printf(TEXT("Requirement %d in slot %d accepts eligible custom gear"),Kinds[N],Key),ACEPluginEquipment::WieldFailure(A,V).IsEmpty());
        A.IntProperties[Key+2]=Kinds[N]==10?0:Values[N]+1;
        TestFalse(*FString::Printf(TEXT("Requirement %d in slot %d still rejects unmet requirements"),Kinds[N],Key),ACEPluginEquipment::WieldFailure(A,V).IsEmpty());
    }
    FACEAppraisalInfo A;A.bSuccess=true;A.IntProperties.Add(158,10);A.IntProperties.Add(159,998);A.IntProperties.Add(160,0);
    TestTrue(TEXT("Absent boolean quality follows ACE's false default"),ACEPluginEquipment::WieldFailure(A,V).IsEmpty());
    A.IntProperties={{273,13},{274,9007},{275,5500},{276,13},{277,9009},{278,3500}};
    V.StatQualityInts.Add(9009,MAX_int32);
    bool RequiresServerCheck=false;
    TestTrue(TEXT("Unreported Int64 counts defer to server equip validation"),ACEPluginEquipment::WieldFailure(A,V,&RequiresServerCheck).IsEmpty());
    TestTrue(TEXT("Deferred validation is exposed"),RequiresServerCheck);
    V.QualityInt64s.Add(9009,0);
    TestTrue(TEXT("ILT gates use Int64, not a same-numbered Int property"),ACEPluginEquipment::WieldFailure(A,V).Contains(TEXT("property 9009 requires 3500 (have 0)")));
    V.QualityInt64s.Add(9009,3499);
    TestFalse(TEXT("One augmentation below the fourth requirement fails"),ACEPluginEquipment::WieldFailure(A,V).IsEmpty());
    V.QualityInt64s[9009]=3500;
    TestTrue(TEXT("Both exact UB dump thresholds pass"),ACEPluginEquipment::WieldFailure(A,V,&RequiresServerCheck).IsEmpty());
    TestFalse(TEXT("Known counts clear deferred validation"),RequiresServerCheck);
    V.QualityInt64s[9007]=int64(1)<<40;
    TestTrue(TEXT("Augmentation counts retain all 64 bits during comparison"),ACEPluginEquipment::WieldFailure(A,V).IsEmpty());
    V.QualityInt64s[9007]=-1;
    TestFalse(TEXT("Negative Int64 values do not become large unsigned counts"),ACEPluginEquipment::WieldFailure(A,V).IsEmpty());
    V.QualityInt64s.Remove(9007);
    A.IntProperties[276]=7;A.IntProperties[278]=V.Level+1;
    TestTrue(TEXT("An unreported custom count does not bypass a later failed level check"),ACEPluginEquipment::WieldFailure(A,V).Contains(TEXT("Wield requirement 4: level")));
    A.IntProperties.Reset();
    A.IntProperties.Add(158,99);
    TestTrue(TEXT("Unknown custom requirements are reported, not silently bypassed"),ACEPluginEquipment::WieldFailure(A,V).Contains(TEXT("unsupported type 99")));
    return !HasAnyErrors();
}
#endif
