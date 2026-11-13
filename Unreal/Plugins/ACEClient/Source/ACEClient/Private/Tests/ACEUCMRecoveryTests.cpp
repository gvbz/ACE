#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/GameInstance.h"
#include "ACEDatSubsystem.h"
#include "Mods/ACEPluginVM.h"

namespace
{
TSharedPtr<FJsonObject> RecoveryJson(const TCHAR* Text)
{TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMFullBookTest,"ACE.Plugins.FullSpellbookTransitions",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMFullBookTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto* Dat=NewObject<UACEDatSubsystem>(NewObject<UGameInstance>());
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if(!TestTrue(TEXT("Retail DAT opens for full spellbook regression"),Dat->LoadDatDirectory(DatDirectory)))return false;
    ON_SCOPE_EXIT{Dat->Deinitialize();};
    TArray<TSharedPtr<FJsonValue>> Book,Active;
    TSet<uint32> Categories;
    for(uint32 Id=1;Id<7000;++Id)
    {
        FString Name;uint32 Icon=0,School=0,Power=0,Category=0,Flags=0,Level=0,TargetFlags=0,TargetType=0;double Duration=0;bool Projectile=false;
        TMap<FString,int32> Counts;
        if(!Dat->TryGetSpellInfo(Id,Name,Icon)||!Dat->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration)
            ||!Dat->TryGetSpellSchoolAndLevel(Id,School,Level)||Level==0||Level>8||!Dat->TryGetPluginSpellScarabs(Id,Counts))continue;
        Dat->TryGetRetailSpellTargeting(Id,TargetFlags,TargetType,Projectile);
        auto V=MakeShared<FJsonObject>();V->SetNumberField(TEXT("id"),Id);V->SetStringField(TEXT("name"),Name);
        V->SetNumberField(TEXT("school"),School);V->SetNumberField(TEXT("category"),Category);V->SetNumberField(TEXT("power"),Power);
        V->SetNumberField(TEXT("skill"),600);V->SetNumberField(TEXT("flags"),Flags);V->SetNumberField(TEXT("level"),Level);
        V->SetNumberField(TEXT("duration"),Duration);V->SetNumberField(TEXT("target_type"),TargetType);V->SetBoolField(TEXT("projectile"),Projectile);
        V->SetBoolField(TEXT("known"),true);V->SetBoolField(TEXT("components_known"),true);
        V->SetBoolField(TEXT("caster_target"),(Flags&8)!=0);V->SetBoolField(TEXT("beneficial"),(Flags&4)!=0);
        V->SetBoolField(TEXT("self_buff"),(Flags&12)==12&&Duration>0);
        auto Scarabs=MakeShared<FJsonObject>();for(const auto& Count:Counts)Scarabs->SetNumberField(Count.Key,Count.Value);V->SetObjectField(TEXT("scarabs"),Scarabs);
        Book.Add(MakeShared<FJsonValueObject>(V));
        if(!Categories.Contains(Category))
        {
            Categories.Add(Category);auto E=RecoveryJson(TEXT(R"({"power":10000,"remaining":3600,"duration":7200})"));
            E->SetNumberField(TEXT("category"),Category);Active.Add(MakeShared<FJsonValueObject>(E));
        }
    }
    AddInfo(FString::Printf(TEXT("Full retail spellbook: %d spells"),Book.Num()));
    TestTrue(TEXT("Fixture exercises thousands of spells"),Book.Num()>2000);
    for(bool Buffing:{false,true})for(const TCHAR* Combat:{TEXT("magic"),TEXT("melee"),TEXT("missile")})
    {
        auto S=RecoveryJson(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"combat_mode":8,"components_required":false,"position":{"cell":2103705613,"x":25,"y":97,"z":12},"trained_skills":[31,32,33,34,43],"skills":{"31":{"training":3,"current":600},"32":{"training":3,"current":600},"33":{"training":3,"current":600},"34":{"training":3,"current":600},"43":{"training":3,"current":600}},"targets":[{"id":500,"name":"Rat","distance":5,"health_fraction":1,"line_of_sight":true,"identified":true}],"inventory":[{"id":99,"name":"Orb","type":32768,"equipped":true,"identified":true,"can_wield":true},{"id":98,"name":"Sword","type":1,"damage":20,"damage_type":1,"skill":45,"identified":true,"can_wield":true},{"id":97,"name":"Atlatl","type":256,"damage":20,"damage_type":2,"skill":47,"identified":true,"can_wield":true}]})"));
        auto P=RecoveryJson(TEXT(R"({"recovery":true,"health_threshold":75,"buff_skill_margin":30,"skill_margin":30,"navigation":false,"looting":false,"debuffs":[],"use_vulnerability":false})"));
        S->SetArrayField(TEXT("spells"),Book);S->SetArrayField(TEXT("enchantments"),Active);
        P->SetBoolField(TEXT("buffing"),Buffing);P->SetStringField(TEXT("combat"),Combat);
        FACEPluginVM VM;if(!TestTrue(TEXT("Full-book policy loads"),VM.Load(Source,Error)))continue;
        for(int Tick=0;Tick<4;++Tick)
        {
            S->SetNumberField(TEXT("time"),100+Tick*20);S->SetNumberField(TEXT("health"),Tick==1?20:100);
            S->SetNumberField(TEXT("action_serial"),Tick);
            S->SetBoolField(TEXT("last_spell_confirmed"),Tick>0);
            TSharedPtr<FJsonObject> I;const bool Worked=VM.Step(S,P,I,Error);
            if(!TestTrue(*FString::Printf(TEXT("Full book %s buffing=%d tick=%d fits budget: %s"),Combat,Buffing,Tick,*Error),Worked))break;
            if(!TestTrue(TEXT("Full-book decision emits an action"),I.IsValid()&&I->HasField(TEXT("action"))))break;
            if(Tick==1)TestEqual(TEXT("Low health still preempts combat"),I->GetNumberField(TEXT("target")),1.);
            else
            {
                const FString Action=I->GetStringField(TEXT("action"));
                TestTrue(TEXT("Healthy fully buffed character proceeds to combat"),Action==TEXT("cast")||Action==TEXT("attack")||Action==TEXT("equip"));
                if(Action==TEXT("cast"))TestEqual(TEXT("Attack targets the monster, not a redundant self buff"),I->GetNumberField(TEXT("target")),500.);
            }
            if(I->HasField(TEXT("spell")))S->SetNumberField(TEXT("last_spell"),I->GetNumberField(TEXT("spell")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMRecoveryTest,"ACE.Plugins.Recovery",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMRecoveryTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return RecoveryJson(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"nearest":0,"ready":true,"busy":false,"action_serial":0,"action_error":0,"combat_mode":8,"container":0,"components_required":false,"position":{"cell":2103705613,"x":25,"y":97,"z":12},"spells":[],"targets":[],"inventory":[{"id":99,"name":"Orb","type":32768,"equipped":true,"identified":true,"can_wield":true}]})"));};
    auto Profile=[](){return RecoveryJson(TEXT(R"({"buffing":false,"recovery":true,"combat":"off","skill_margin":30})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    auto* Dat=NewObject<UACEDatSubsystem>(NewObject<UGameInstance>());
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if(!TestTrue(TEXT("Retail DAT opens for real recovery spell metadata"),Dat->LoadDatDirectory(DatDirectory)))return false;
    ON_SCOPE_EXIT{Dat->Deinitialize();};
    TMap<FString,TSharedPtr<FJsonObject>> Spells;
    for(uint32 Id=1;Id<7000;++Id)
    {
        FString Name;uint32 Icon=0,School=0,Power=0,Category=0,Flags=0;double Duration=0;
        if(!Dat->TryGetSpellInfo(Id,Name,Icon)||!Dat->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration))continue;
        if(!(Flags&8))continue;
        auto Spell=MakeShared<FJsonObject>();Spell->SetNumberField(TEXT("id"),Id);Spell->SetStringField(TEXT("name"),Name);
        Spell->SetNumberField(TEXT("category"),Category);Spell->SetNumberField(TEXT("school"),School);Spell->SetNumberField(TEXT("power"),Power);
        Spell->SetNumberField(TEXT("skill"),600);Spell->SetNumberField(TEXT("duration"),Duration);Spell->SetBoolField(TEXT("caster_target"),true);
        Spell->SetBoolField(TEXT("self_buff"),(Flags&4)!=0&&Duration>0);
        Spell->SetBoolField(TEXT("beneficial"),(Flags&4)!=0);Spell->SetBoolField(TEXT("known"),true);
        uint32 Level=0;Dat->TryGetSpellSchoolAndLevel(Id,School,Level);Spell->SetNumberField(TEXT("level"),Level);
        TMap<FString,int32> Counts;auto Scarabs=MakeShared<FJsonObject>();
        Spell->SetBoolField(TEXT("components_known"),Dat->TryGetPluginSpellScarabs(Id,Counts));
        for(const auto& Count:Counts)Scarabs->SetNumberField(Count.Key,Count.Value);
        Spell->SetObjectField(TEXT("scarabs"),Scarabs);Spells.Add(Name,Spell);
    }
    for(const auto& Case:TArray<TPair<FString,FString>>{
        {TEXT("health"),TEXT("Heal Self I")},{TEXT("health"),TEXT("Heal Self II")},{TEXT("health"),TEXT("Heal Self III")},{TEXT("health"),TEXT("Heal Self IV")},{TEXT("health"),TEXT("Heal Self V")},{TEXT("health"),TEXT("Heal Self VI")},{TEXT("health"),TEXT("Adja's Intervention")},{TEXT("health"),TEXT("Incantation of Heal Self")},
        {TEXT("stamina"),TEXT("Revitalize Self I")},{TEXT("stamina"),TEXT("Revitalize Self II")},{TEXT("stamina"),TEXT("Revitalize Self III")},{TEXT("stamina"),TEXT("Revitalize Self IV")},{TEXT("stamina"),TEXT("Revitalize Self V")},{TEXT("stamina"),TEXT("Revitalize Self VI")},{TEXT("stamina"),TEXT("Robustification")},{TEXT("stamina"),TEXT("Incantation of Revitalize Self")},
        {TEXT("mana"),TEXT("Stamina to Mana Self I")},{TEXT("mana"),TEXT("Stamina to Mana Self II")},{TEXT("mana"),TEXT("Stamina to Mana Self III")},{TEXT("mana"),TEXT("Stamina to Mana Self IV")},{TEXT("mana"),TEXT("Stamina to Mana Self V")},{TEXT("mana"),TEXT("Stamina to Mana Self VI")},{TEXT("mana"),TEXT("Meditative Trance")},{TEXT("mana"),TEXT("Incantation of Stamina to Mana Self")}})
    {
        const auto* Spell=Spells.Find(Case.Value);if(!TestNotNull(*Case.Value,Spell))continue;
        for(bool Imported:{false,true})for(bool Combat:{false,true})
        {
            FACEPluginVM VM;TestTrue(TEXT("Recovery policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();S->SetNumberField(*Case.Key,20);
            S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(*Spell)});
            if(Imported){auto H=RecoveryJson(TEXT(R"({"stance":1,"handler":"Regular Spell","min":0,"max":100})"));H->SetNumberField(TEXT("vital"),Case.Key==TEXT("health")?2:Case.Key==TEXT("stamina")?4:6);P->SetArrayField(TEXT("recharge_handlers"),{MakeShared<FJsonValueObject>(H)});}
            if(Combat){P->SetStringField(TEXT("combat"),TEXT("magic"));S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"id":500,"name":"Rat","distance":5,"line_of_sight":true,"identified":true})")))});}
            const auto I=Step(VM,S,P);double CastSpell=0;I->TryGetNumberField(TEXT("spell"),CastSpell);
            TestEqual(*(TEXT("Recovery precedes combat and supports imported handler: ")+Case.Value),CastSpell,(*Spell)->GetNumberField(TEXT("id")));
            TestEqual(TEXT("Recovery targets the player"),I->GetNumberField(TEXT("target")),1.);
        }
    }
    for(int Vital:{2,4,6})
    {
        FACEPluginVM V;V.Load(Source,Error);auto State=Snapshot(),Settings=Profile();State->SetNumberField(Vital==2?TEXT("health"):Vital==4?TEXT("stamina"):TEXT("mana"),20);
        auto Item=RecoveryJson(TEXT(R"({"id":42,"wcid":42,"name":"Potion","type":32,"usable":true,"identified":false,"count":10})"));
        State->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Item)});
        auto I=Step(V,State,Settings);TestEqual(TEXT("Unappraised recovery supply is identified"),I->GetStringField(TEXT("action")),FString(TEXT("identify")));
        Item->SetBoolField(TEXT("identified"),true);Item->SetNumberField(TEXT("boost_vital"),Vital);Item->SetNumberField(TEXT("boost"),50);
        TestEqual(TEXT("Appraised supply is automatically used"),Step(V,State,Settings)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
    }
    {
        auto State=Snapshot(),Settings=Profile();State->SetNumberField(TEXT("mana"),20);
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Spells[TEXT("Meditative Trance")]),MakeShared<FJsonValueObject>(Spells[TEXT("Incantation of Stamina to Mana Self")])});
        auto Choose=[&](){FACEPluginVM V;V.Load(Source,Error);return Step(V,State,Settings);};
        TestEqual(TEXT("Strongest eligible recovery tier is selected"),Choose()->GetNumberField(TEXT("spell")),Spells[TEXT("Incantation of Stamina to Mana Self")]->GetNumberField(TEXT("id")));
        Settings->SetNumberField(TEXT("skill_margin"),250);
        TestEqual(TEXT("Skill buffer still restricts recovery tier"),Choose()->GetNumberField(TEXT("spell")),Spells[TEXT("Meditative Trance")]->GetNumberField(TEXT("id")));
        State->SetNumberField(TEXT("stamina"),20);TestFalse(TEXT("Mana conversion preserves depleted stamina"),Choose()->HasField(TEXT("action")));
        TestTrue(TEXT("No available recovery method explains the problem"),Choose()->GetStringField(TEXT("status")).StartsWith(TEXT("Recovery unavailable")));
        State->SetNumberField(TEXT("stamina"),100);Settings->SetBoolField(TEXT("recovery"),false);TestFalse(TEXT("Recovery toggle remains authoritative"),Choose()->HasField(TEXT("action")));
    }
    // Incoming hits can outweigh a successful heal before the next snapshot.
    // Reevaluate the current HP every completion, including repeated below-threshold hits.
    {
        FACEPluginVM V;V.Load(Source,Error);auto State=Snapshot(),Settings=Profile();
        auto Heal=Spells[TEXT("Heal Self II")],Low=Spells[TEXT("Heal Self I")];
        Settings->SetStringField(TEXT("combat"),TEXT("magic"));Settings->SetNumberField(TEXT("health_threshold"),75);
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Heal),MakeShared<FJsonValueObject>(Low)});
        State->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"id":500,"name":"Rat","distance":5,"identified":true,"line_of_sight":true})")))});
        for(int32 Attempt=0;Attempt<6;++Attempt)
        {
            State->SetNumberField(TEXT("health"),60-Attempt*5);State->SetNumberField(TEXT("time"),100+Attempt);
            if(Attempt){State->SetNumberField(TEXT("action_serial"),Attempt);State->SetNumberField(TEXT("last_spell"),Heal->GetNumberField(TEXT("id")));State->SetBoolField(TEXT("last_spell_confirmed"),true);}
            auto I=Step(V,State,Settings);
            TestEqual(TEXT("Confirmed healing masked by damage keeps healing, never waits or attacks"),I->GetNumberField(TEXT("spell")),Heal->GetNumberField(TEXT("id")));
            TestEqual(TEXT("Under-threshold recovery always targets self"),I->GetNumberField(TEXT("target")),1.);
        }
        State->SetNumberField(TEXT("action_serial"),6);State->SetNumberField(TEXT("time"),106);State->SetBoolField(TEXT("last_spell_confirmed"),false);
        TestEqual(TEXT("Fizzle falls back to another healing tier immediately"),Step(V,State,Settings)->GetNumberField(TEXT("spell")),Low->GetNumberField(TEXT("id")));
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Heal)});
        FACEPluginVM Fizzles;Fizzles.Load(Source,Error);State->SetNumberField(TEXT("time"),200);
        Step(Fizzles,State,Settings);
        for(int32 Attempt=0;Attempt<6;++Attempt)
        {
            State->SetNumberField(TEXT("time"),201+Attempt*2);State->SetNumberField(TEXT("action_serial"),7+Attempt);
            State->SetNumberField(TEXT("last_spell"),Heal->GetNumberField(TEXT("id")));State->SetBoolField(TEXT("last_spell_confirmed"),false);
            Step(Fizzles,State,Settings); // Reject this tier; retain its short retry delay.
            State->SetNumberField(TEXT("time"),202+Attempt*2);
            TestEqual(TEXT("Repeated fizzles never pause all healing"),Step(Fizzles,State,Settings)->GetNumberField(TEXT("spell")),Heal->GetNumberField(TEXT("id")));
        }
    }
    {
        FACEPluginVM V;V.Load(Source,Error);auto State=Snapshot(),Settings=Profile();
        State->SetNumberField(TEXT("health"),20);State->SetNumberField(TEXT("max_health"),150);State->SetNumberField(TEXT("stamina"),150);State->SetNumberField(TEXT("max_stamina"),150);
        Settings->SetNumberField(TEXT("health_threshold"),75);
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Spells[TEXT("Heal Self I")]),
            MakeShared<FJsonValueObject>(Spells[TEXT("Stamina to Health Self I")]),MakeShared<FJsonValueObject>(Spells[TEXT("Stamina to Health Self II")]),
            MakeShared<FJsonValueObject>(Spells[TEXT("Incantation of Stamina to Health Self")])});
        TestEqual(TEXT("Emergency conversion uses the lowest tier covering the health deficit"),Step(V,State,Settings)->GetNumberField(TEXT("spell")),Spells[TEXT("Stamina to Health Self II")]->GetNumberField(TEXT("id")));
        Settings->SetNumberField(TEXT("emergency_health_threshold"),0);FACEPluginVM Disabled;Disabled.Load(Source,Error);
        TestEqual(TEXT("Emergency conversion can be disabled"),Step(Disabled,State,Settings)->GetNumberField(TEXT("spell")),Spells[TEXT("Heal Self I")]->GetNumberField(TEXT("id")));
    }
    // Conversion's own slider must work above the ordinary healing threshold,
    // including when a regular heal could cover the whole deficit.
    {
        auto State=Snapshot(),Settings=Profile();Settings->SetNumberField(TEXT("health_threshold"),35);
        Settings->SetNumberField(TEXT("emergency_health_threshold"),70);
        State->SetNumberField(TEXT("health"),60);
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Spells[TEXT("Incantation of Heal Self")]),
            MakeShared<FJsonValueObject>(Spells[TEXT("Stamina to Health Self II")])});
        FACEPluginVM V;V.Load(Source,Error);
        TestEqual(TEXT("Conversion slider independently triggers stamina-to-health"),Step(V,State,Settings)->GetNumberField(TEXT("spell")),Spells[TEXT("Stamina to Health Self II")]->GetNumberField(TEXT("id")));
        State->SetNumberField(TEXT("health"),80);State->SetNumberField(TEXT("action_serial"),1);State->SetNumberField(TEXT("time"),101);
        TestFalse(TEXT("Recovery stops when both thresholds are satisfied"),Step(V,State,Settings)->HasField(TEXT("spell")));
    }
    // Buff priority must not depend on DAT IDs or spellbook enumeration.
    // Match VT's opening sequence, then the remaining magic schools.
    for(bool Forced:{false,true})
    {
        const TArray<int32> Order={43,9,11,51,47,45,49,645,1};
        TArray<TSharedPtr<FJsonValue>> Book,Enchantments;
        TMap<int32,TSharedPtr<FJsonObject>> Families;
        for(int32 Category:Order)
        {
            for(const auto& Pair:Spells)
            {
                const auto V=Pair.Value;
                if(V->GetNumberField(TEXT("category"))==Category&&V->GetNumberField(TEXT("level"))==1
                    &&V->GetNumberField(TEXT("duration"))>=300&&V->GetNumberField(TEXT("school"))==4)
                {Families.Add(Category,V);break;}
            }
            if(!TestTrue(TEXT("Retail casting-priority buff metadata exists"),Families.Contains(Category)))return false;
        }
        for(int32 Index=Order.Num()-1;Index>=0;--Index)Book.Add(MakeShared<FJsonValueObject>(Families[Order[Index]]));
        auto State=Snapshot(),Settings=Profile();Settings->SetBoolField(TEXT("buffing"),true);
        State->SetArrayField(TEXT("spells"),Book);
        State->SetArrayField(TEXT("trained_skills"),{MakeShared<FJsonValueNumber>(31),MakeShared<FJsonValueNumber>(32),MakeShared<FJsonValueNumber>(33),MakeShared<FJsonValueNumber>(34),MakeShared<FJsonValueNumber>(16),MakeShared<FJsonValueNumber>(43)});
        if(Forced)State->SetNumberField(TEXT("force_buff_request"),1);
        FACEPluginVM Policy;Policy.Load(Source,Error);
        for(int32 Index=0;Index<Order.Num();++Index)
        {
            const auto Spell=Families[Order[Index]];auto Intent=Step(Policy,State,Settings);
            double Actual=0;Intent->TryGetNumberField(TEXT("spell"),Actual);
            TestEqual(*(TEXT("Casting support buffs precede ordinary buffs: ")+Spell->GetStringField(TEXT("name"))),Actual,Spell->GetNumberField(TEXT("id")));
            auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("id"),Actual);E->SetNumberField(TEXT("category"),Order[Index]);
            E->SetNumberField(TEXT("power"),Spell->GetNumberField(TEXT("power")));E->SetNumberField(TEXT("remaining"),Spell->GetNumberField(TEXT("duration"))-1);
            Enchantments.Add(MakeShared<FJsonValueObject>(E));State->SetArrayField(TEXT("enchantments"),Enchantments);
            State->SetNumberField(TEXT("action_serial"),Index+1);State->SetNumberField(TEXT("last_spell"),Actual);State->SetNumberField(TEXT("time"),101+Index);
        }
        auto Finished=Step(Policy,State,Settings);
        TestFalse(TEXT("Completed casting support families are not repeated"),Finished->HasField(TEXT("spell")));
    }
    {
        // Server-reported improvements can unlock a higher tier during the cycle.
        auto State=Snapshot(),Settings=Profile();Settings->SetBoolField(TEXT("buffing"),true);
        auto Low=RecoveryJson(TEXT(R"({"id":8001,"name":"Creature mastery VI","category":43,"school":4,"duration":1800,"skill":310,"power":250,"level":6,"self_buff":true})"));
        auto High=RecoveryJson(TEXT(R"({"id":8002,"name":"Creature mastery VII","category":43,"school":4,"duration":3600,"skill":310,"power":300,"level":7,"self_buff":true})"));
        auto Focus=RecoveryJson(TEXT(R"({"id":8003,"name":"Focus VI","category":9,"school":4,"duration":1800,"skill":310,"power":250,"level":6,"self_buff":true})"));
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Focus),MakeShared<FJsonValueObject>(High),MakeShared<FJsonValueObject>(Low)});
        State->SetArrayField(TEXT("trained_skills"),{MakeShared<FJsonValueNumber>(31)});
        FACEPluginVM Policy;Policy.Load(Source,Error);
        TestEqual(TEXT("Begins with currently castable Creature mastery"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),8001.);
        State->SetNumberField(TEXT("action_serial"),1);State->SetNumberField(TEXT("last_spell"),8001);State->SetNumberField(TEXT("time"),101);
        State->SetArrayField(TEXT("enchantments"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"id":8001,"category":43,"power":250,"remaining":1799})")))});
        // The host refreshes spell skills from replicated effective skill values.
        Low->SetNumberField(TEXT("skill"),350);High->SetNumberField(TEXT("skill"),350);Focus->SetNumberField(TEXT("skill"),350);
        TestEqual(TEXT("Newly usable mastery upgrade does not wait out stale retry delay"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),8002.);
    }
    {
        auto S=Snapshot(),P=Profile();S->SetNumberField(TEXT("force_buff_request"),1);
        P->SetBoolField(TEXT("fast_cast_buffs"),true);
        TArray<TSharedPtr<FJsonValue>> Book;
        for(int School:{2,3,4})
        {
            auto Spell=RecoveryJson(TEXT(R"({"power":100,"skill":500,"duration":300,"self_buff":true})"));
            Spell->SetNumberField(TEXT("id"),School);Spell->SetNumberField(TEXT("category"),School==2?101:School==3?160:1);Spell->SetNumberField(TEXT("school"),School);
            Book.Add(MakeShared<FJsonValueObject>(Spell));
        }
        S->SetArrayField(TEXT("spells"),Book);FACEPluginVM Policy;Policy.Load(Source,Error);
        auto I=Step(Policy,S,P);TestEqual(TEXT("Force Buff starts with first available school"),I->GetNumberField(TEXT("spell")),2.);
        TestTrue(TEXT("Fast-cast option reaches the cast request"),I->GetBoolField(TEXT("fast_cast")));
        S->SetNumberField(TEXT("last_spell"),2);S->SetNumberField(TEXT("action_serial"),1);S->SetBoolField(TEXT("last_spell_confirmed"),false);
        S->SetNumberField(TEXT("time"),100.5);I=Step(Policy,S,P);
        TestEqual(TEXT("Completed fizzle retries promptly, without twelve-second confirmation wait"),I->GetNumberField(TEXT("spell")),2.);
        int Serial=1;
        for(int School:{2,3,4})
        {
            S->SetNumberField(TEXT("last_spell"),School);S->SetNumberField(TEXT("action_serial"),++Serial);
            S->SetBoolField(TEXT("last_spell_confirmed"),true);S->SetNumberField(TEXT("time"),100.5+Serial*.5);
            I=Step(Policy,S,P);
            if(School<4)TestEqual(TEXT("Forced cycle continues across all three schools"),I->GetNumberField(TEXT("spell")),double(School+1));
        }
        TestEqual(TEXT("Force Buff finishes only after all available families confirm"),I->GetStringField(TEXT("action")),FString(TEXT("force_buff_done")));
        // A fresh manual request must retry a previously rejected spell rather
        // than silently skipping it due to the prior pass's cooldown.
        S->SetNumberField(TEXT("force_buff_request"),2);I=Step(Policy,S,P);
        S->SetNumberField(TEXT("last_spell"),2);S->SetNumberField(TEXT("action_serial"),++Serial);S->SetNumberField(TEXT("action_error"),0x400);
        S->SetBoolField(TEXT("last_spell_confirmed"),false);I=Step(Policy,S,P);
        TestEqual(TEXT("Rejected family allows other buffs to continue"),I->GetNumberField(TEXT("spell")),3.);
        S->SetNumberField(TEXT("force_buff_request"),3);S->SetNumberField(TEXT("action_error"),0);I=Step(Policy,S,P);
        TestEqual(TEXT("New Force Buff retries the previously unavailable family"),I->GetNumberField(TEXT("spell")),2.);
    }
    // Use real DAT duration/power: Tusker Leap is stronger than Jump VIII,
    // but expires in 10s, inside the default 60s renewal window.
    {
        const auto Leap=Spells.FindRef(TEXT("Tusker Leap"));
        const auto Jump=Spells.FindRef(TEXT("Incantation of Jumping Mastery Self"));
        if(!TestTrue(TEXT("Real Jump and Tusker Leap metadata found"),Leap.IsValid()&&Jump.IsValid()))return false;
        TestEqual(TEXT("Tusker Leap is a ten-second burst"),Leap->GetNumberField(TEXT("duration")),10.);
        auto State=Snapshot(),Settings=Profile();Settings->SetBoolField(TEXT("buffing"),true);
        State->SetArrayField(TEXT("trained_skills"),{MakeShared<FJsonValueNumber>(22)});
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Leap),MakeShared<FJsonValueObject>(Jump)});
        FACEPluginVM Policy;Policy.Load(Source,Error);
        TestEqual(TEXT("Automatic buff chooses sustained Jump instead of higher-power Tusker Leap"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),Jump->GetNumberField(TEXT("id")));
        auto Active=RecoveryJson(TEXT(R"({"category":69,"power":400,"remaining":5399})"));Active->SetNumberField(TEXT("id"),Jump->GetNumberField(TEXT("id")));
        State->SetArrayField(TEXT("enchantments"),{MakeShared<FJsonValueObject>(Active)});
        State->SetNumberField(TEXT("action_serial"),1);State->SetNumberField(TEXT("last_spell"),Jump->GetNumberField(TEXT("id")));
        State->SetNumberField(TEXT("time"),101);
        TestFalse(TEXT("Acknowledged Jump completes this buff family"),Step(Policy,State,Settings)->HasField(TEXT("action")));
        Settings->SetBoolField(TEXT("auto_buffs"),false);Settings->SetArrayField(TEXT("buffs"),{MakeShared<FJsonValueNumber>(Leap->GetNumberField(TEXT("id")))});
        for(bool ItemConfirmation:{false,true})
        {
            FACEPluginVM Explicit;Explicit.Load(Source,Error);State->RemoveField(TEXT("enchantments"));State->RemoveField(TEXT("item_buffs"));
            TestEqual(TEXT("Explicit short buff remains available"),Step(Explicit,State,Settings)->GetNumberField(TEXT("spell")),Leap->GetNumberField(TEXT("id")));
            State->SetNumberField(TEXT("action_serial"),2);State->SetNumberField(TEXT("last_spell"),Leap->GetNumberField(TEXT("id")));
            auto Confirmed=RecoveryJson(TEXT(R"({"target":1,"category":69,"power":401,"duration":10,"remaining":9,"expires":111})"));Confirmed->SetNumberField(TEXT("id"),Leap->GetNumberField(TEXT("id")));
            State->SetArrayField(ItemConfirmation?TEXT("item_buffs"):TEXT("enchantments"),{MakeShared<FJsonValueObject>(Confirmed)});
            auto Next=Step(Explicit,State,Settings);
            TestFalse(TEXT("Fresh short buff is not immediately recast"),Next->HasField(TEXT("action")));
            TestFalse(TEXT("Fresh short buff does not stall remaining cycle"),Next->GetStringField(TEXT("status")).Contains(TEXT("Waiting for buff")));
            State->SetNumberField(TEXT("action_serial"),1);
        }
    }
    // VT m/dg/he can use a stronger innate recovery spell without learning it
    // or meeting its school skill/component requirements. Activate the item.
    for(bool Imported:{false,true})for(bool Equipped:{false,true})
    {
        auto S=Snapshot(),P=Profile();S->SetNumberField(TEXT("health"),50);
        auto Heal=MakeShared<FJsonObject>(*Spells[TEXT("Heal Self I")]);
        auto Innate=MakeShared<FJsonObject>(*Spells[TEXT("Adja's Intervention")]);Innate->SetBoolField(TEXT("known"),false);Innate->SetNumberField(TEXT("skill"),0);
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Heal)});
        auto Wand=S->GetArrayField(TEXT("inventory"))[0]->AsObject();Wand->SetObjectField(TEXT("innate_recovery"),Innate);
        Wand->SetBoolField(TEXT("innate_recovery_available"),true);Wand->SetBoolField(TEXT("equipped"),Equipped);
        if(Imported)P->SetArrayField(TEXT("recharge_handlers"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"vital":2,"stance":1,"handler":"Regular Spell","min":0,"max":100})")))});
        FACEPluginVM V;V.Load(Source,Error);auto I=Step(V,S,P);
        TestEqual(TEXT("Recovery equips the specific implement before using its innate"),I->GetStringField(TEXT("action")),FString(Equipped?TEXT("cast_item"):TEXT("equip")));
        if(!Equipped){Wand->SetBoolField(TEXT("equipped"),true);I=Step(V,S,P);}
        TestEqual(TEXT("Innate recovery names the source item"),I->GetNumberField(TEXT("item")),99.);
        TestEqual(TEXT("Stronger innate beats learned recovery spell"),I->GetNumberField(TEXT("spell")),Innate->GetNumberField(TEXT("id")));
        TestFalse(TEXT("No repeated innate before vital update"),Step(V,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("health"),100);TestFalse(TEXT("Authoritative health rise completes innate recovery"),Step(V,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("health"),50);Wand->SetBoolField(TEXT("innate_recovery_available"),false);
        FACEPluginVM Fallback;Fallback.Load(Source,Error);TestEqual(TEXT("Ineligible innate falls back to learned recovery"),Step(Fallback,S,P)->GetNumberField(TEXT("spell")),Heal->GetNumberField(TEXT("id")));
        Wand->SetBoolField(TEXT("innate_recovery_available"),true);
        Wand->SetObjectField(TEXT("int_properties"),RecoveryJson(TEXT(R"({"280":17})")));S->SetObjectField(TEXT("cooldowns"),RecoveryJson(TEXT(R"({"17":5})")));
        FACEPluginVM Cooling;Cooling.Load(Source,Error);TestEqual(TEXT("Innate on cooldown yields to learned recovery"),Step(Cooling,S,P)->GetStringField(TEXT("action")),FString(TEXT("cast")));
        S->RemoveField(TEXT("cooldowns"));Innate->SetNumberField(TEXT("power"),Heal->GetNumberField(TEXT("power")));
        FACEPluginVM Equal;Equal.Load(Source,Error);TestEqual(TEXT("Equal-power innate does not replace learned spell"),Step(Equal,S,P)->GetStringField(TEXT("action")),FString(TEXT("cast")));
    }
    // Imported handler intervals use VT's integer vital percentage. Conversion
    // source exactly at its reserve is eligible, not considered below reserve.
    {
        auto S=Snapshot(),P=Profile();S->SetNumberField(TEXT("health"),101);S->SetNumberField(TEXT("max_health"),200);
        auto Food=RecoveryJson(TEXT(R"({"id":51,"name":"Food","type":32,"usable":true,"identified":true,"count":5,"boost_vital":2,"boost":100})"));
        auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Food));S->SetArrayField(TEXT("inventory"),Items);
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Spells[TEXT("Heal Self I")])});
        P->SetArrayField(TEXT("recharge_handlers"),{
            MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"vital":2,"stance":1,"handler":"Recharge With Food","min":0,"max":50})"))),
            MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"vital":2,"stance":1,"handler":"Regular Spell","min":0,"max":100})")))});
        FACEPluginVM V;V.Load(Source,Error);TestEqual(TEXT("50.5 percent falls in VT's integer 0-50 interval"),Step(V,S,P)->GetNumberField(TEXT("item")),51.);
        S=Snapshot();S->SetNumberField(TEXT("mana"),10);S->SetNumberField(TEXT("health"),65);
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Spells[TEXT("Health to Mana Self I")])});
        P=Profile();P->SetArrayField(TEXT("recharge_handlers"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"vital":6,"stance":1,"handler":"Health to Mana","min":0,"max":100})")))});
        FACEPluginVM Boundary;Boundary.Load(Source,Error);TestEqual(TEXT("Source at the reserve can recover another vital"),Step(Boundary,S,P)->GetStringField(TEXT("action")),FString(TEXT("cast")));
    }
    // Regular mana recovery prefers conversion even over a higher-tier boost;
    // replenished mana then pays for Revitalize when stamina falls below threshold.
    for(bool Imported:{false,true})
    {
        auto State=Snapshot(),Settings=Profile();State->SetNumberField(TEXT("mana"),20);
        const auto Convert=Spells.FindRef(TEXT("Stamina to Mana Self VI")),Revitalize=Spells.FindRef(TEXT("Robustification")),Boost=Spells.FindRef(TEXT("Incantation of Mana Boost Self"));
        if(!TestTrue(TEXT("Real recovery ordering spell metadata found"),Convert.IsValid()&&Revitalize.IsValid()&&Boost.IsValid()))return false;
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Boost),MakeShared<FJsonValueObject>(Convert),MakeShared<FJsonValueObject>(Revitalize)});
        if(Imported)Settings->SetArrayField(TEXT("recharge_handlers"),{
            MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"vital":4,"stance":1,"handler":"Regular Spell","min":0,"max":100})"))),
            MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"vital":6,"stance":1,"handler":"Regular Spell","min":0,"max":100})")))});
        FACEPluginVM Policy;Policy.Load(Source,Error);
        TestEqual(TEXT("Conversion preferred to stronger Mana Boost"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),Convert->GetNumberField(TEXT("id")));
        State->SetNumberField(TEXT("mana"),90);State->SetNumberField(TEXT("stamina"),20);State->SetNumberField(TEXT("time"),101);
        State->SetNumberField(TEXT("action_serial"),1);State->SetNumberField(TEXT("last_spell"),Convert->GetNumberField(TEXT("id")));
        TestEqual(TEXT("Revitalize follows successful Stamina to Mana"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),Revitalize->GetNumberField(TEXT("id")));
        State->SetNumberField(TEXT("stamina"),90);State->SetNumberField(TEXT("mana"),70);State->SetNumberField(TEXT("time"),102);
        State->SetNumberField(TEXT("action_serial"),2);State->SetNumberField(TEXT("last_spell"),Revitalize->GetNumberField(TEXT("id")));
        TestFalse(TEXT("Recovered vitals stop the conversion/revitalize cycle"),Step(Policy,State,Settings)->HasField(TEXT("action")));
        State->SetNumberField(TEXT("mana"),20);State->SetNumberField(TEXT("stamina"),20);
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Boost),MakeShared<FJsonValueObject>(Convert)});
        FACEPluginVM Fallback;Fallback.Load(Source,Error);
        TestEqual(TEXT("Low stamina preserves direct mana spell fallback"),Step(Fallback,State,Settings)->GetNumberField(TEXT("spell")),Boost->GetNumberField(TEXT("id")));
    }
    // The live inventory contains kits even when the player expects spell recovery.
    // Exercise actual DAT formula components, all stances, and both preference modes.
    for(int32 Mode:{1,2,4,8})for(bool SuppliesFirst:{false,true})
    {
        auto S=Snapshot(),P=Profile();S->SetNumberField(TEXT("health"),20);S->SetNumberField(TEXT("combat_mode"),Mode);
        P->SetBoolField(TEXT("recovery_supplies_first"),SuppliesFirst);S->SetBoolField(TEXT("components_required"),true);
        auto Heal=Spells[TEXT("Incantation of Heal Self")];S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Heal)});
        auto Inventory=S->GetArrayField(TEXT("inventory"));
        Inventory.Add(MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"id":42,"name":"Kit","type":128,"usable":true,"identified":true,"healing_kit":true,"structure":50,"boost_vital":2,"boost":100})"))));
        S->SetArrayField(TEXT("trained_skills"),{MakeShared<FJsonValueNumber>(21)});
        for(const auto& C:Heal->GetObjectField(TEXT("scarabs"))->Values)
        {auto Item=MakeShared<FJsonObject>();Item->SetNumberField(TEXT("id"),1000+Inventory.Num());Item->SetStringField(TEXT("name"),C.Key);Item->SetNumberField(TEXT("count"),C.Value->AsNumber());Inventory.Add(MakeShared<FJsonValueObject>(Item));}
        S->SetArrayField(TEXT("inventory"),Inventory);
        FACEPluginVM VM;VM.Load(Source,Error);auto I=Step(VM,S,P);
        TestEqual(TEXT("Default spells-first recovery works with kits present in every stance; preference reverses it"),I->GetStringField(TEXT("action")),FString(SuppliesFirst?TEXT("use_item"):TEXT("cast")));
        if(SuppliesFirst)
        {
            // A failed or timed-out kit must yield to spells, not just retry the kit.
            for(bool Timeout:{false,true})
            {
                FACEPluginVM Fallback;Fallback.Load(Source,Error);
                TestEqual(TEXT("Kit preference starts with the kit"),Step(Fallback,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
                if(Timeout)S->SetNumberField(TEXT("time"),109);
                else
                {
                    S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),1);
                    S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":42,"action_serial":1,"requested_at":100,"error":1})")))});
                }
                TestEqual(TEXT("Failed or unconfirmed kit falls back to a recovery spell"),Step(Fallback,S,P)->GetStringField(TEXT("action")),FString(TEXT("cast")));
                S->SetNumberField(TEXT("time"),100);S->SetNumberField(TEXT("action_serial"),0);S->SetNumberField(TEXT("action_error"),0);S->RemoveField(TEXT("action_events"));
            }
            Inventory[1]->AsObject()->SetNumberField(TEXT("structure"),0);
            FACEPluginVM Empty;Empty.Load(Source,Error);
            TestEqual(TEXT("Exhausted kit falls back to spell"),Step(Empty,S,P)->GetStringField(TEXT("action")),FString(TEXT("cast")));
            Inventory[1]->AsObject()->SetNumberField(TEXT("structure"),50);
        }
        // Explicit priority overrides an imported handler list, in both directions.
        P->SetArrayField(TEXT("recharge_handlers"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"vital":2,"stance":1,"handler":"Regular Spell","min":0,"max":100})")))});
        P->SetBoolField(TEXT("use_imported_recovery_order"),false);
        FACEPluginVM Override;Override.Load(Source,Error);
        TestEqual(TEXT("Explicit preference applies to imported profiles"),Step(Override,S,P)->GetStringField(TEXT("action")),FString(SuppliesFirst?TEXT("use_item"):TEXT("cast")));
        P->RemoveField(TEXT("recharge_handlers"));
        // Remove components: ordinary players fall back to the kit; exempt players still cast.
        Inventory.SetNum(2);S->SetArrayField(TEXT("inventory"),Inventory);P->SetBoolField(TEXT("recovery_supplies_first"),false);
        FACEPluginVM Missing;Missing.Load(Source,Error);TestEqual(TEXT("Missing components retain kit fallback"),Step(Missing,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
        S->SetBoolField(TEXT("components_required"),false);
        FACEPluginVM Exempt;Exempt.Load(Source,Error);TestEqual(TEXT("Server component exemption keeps recovery spells available"),Step(Exempt,S,P)->GetStringField(TEXT("action")),FString(TEXT("cast")));
        S->SetNumberField(TEXT("last_spell"),Heal->GetNumberField(TEXT("id")));S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),1);
        TestEqual(TEXT("Rejected recovery spell falls back without waiting eight seconds or stopping"),Step(Exempt,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
    }
    // Recovery confirmations are independent: waiting for a mana potion must
    // not hold up health, nor may another action's result acknowledge that potion.
    {
        FACEPluginVM Priority;Priority.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("recovery_supplies_first"),true);
        S->SetArrayField(TEXT("inventory"),RecoveryJson(TEXT(R"({"items":[{"id":42,"wcid":42,"name":"Health potion","type":32,"usable":true,"identified":true,"boost_vital":2,"boost":100,"count":10},{"id":44,"wcid":44,"name":"Stamina potion","type":32,"usable":true,"identified":true,"boost_vital":4,"boost":100,"count":10},{"id":46,"wcid":46,"name":"Mana potion","type":32,"usable":true,"identified":true,"boost_vital":6,"boost":100,"count":10}]})"))->GetArrayField(TEXT("items")));
        S->SetArrayField(TEXT("action_events"),{});S->SetNumberField(TEXT("mana"),20);
        TestEqual(TEXT("Begin mana recovery"),Step(Priority,S,P)->GetNumberField(TEXT("item")),46.);
        S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("health"),20);S->SetBoolField(TEXT("busy"),true);
        TestFalse(TEXT("Health cannot bypass an active native action gate"),Step(Priority,S,P)->HasField(TEXT("action")));
        S->SetBoolField(TEXT("busy"),false);S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),1);
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":99,"action_serial":1,"requested_at":100,"error":1})")))});
        TestEqual(TEXT("Health preempts unconfirmed mana without waiting eight seconds"),Step(Priority,S,P)->GetNumberField(TEXT("item")),42.);
        S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("time"),102);S->SetNumberField(TEXT("action_serial"),2);
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":42,"action_serial":2,"requested_at":101,"error":0})")))});
        TestFalse(TEXT("Health receipt cannot complete or duplicate pending mana"),Step(Priority,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("stamina"),20);S->SetNumberField(TEXT("time"),103);
        TestEqual(TEXT("Stamina also outranks pending mana"),Step(Priority,S,P)->GetNumberField(TEXT("item")),44.);
        S->SetNumberField(TEXT("stamina"),100);S->SetNumberField(TEXT("time"),104);
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":46,"action_serial":3,"requested_at":99,"error":1})")))});
        TestFalse(TEXT("Late receipt for an earlier use of the same item is ignored"),Step(Priority,S,P)->HasField(TEXT("action")));
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":46,"action_serial":4,"requested_at":100,"error":0})")))});
        TestFalse(TEXT("UseDone alone cannot invent a restored vital"),Step(Priority,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("mana"),100);S->SetNumberField(TEXT("time"),105);
        TestFalse(TEXT("Late vital replication completes the original mana operation"),Step(Priority,S,P)->HasField(TEXT("action")));
    }
    {
        FACEPluginVM Priority;Priority.Load(Source,Error);auto S=Snapshot(),P=Profile();
        auto Mana=Spells.FindRef(TEXT("Stamina to Mana Self I")),Heal=Spells.FindRef(TEXT("Heal Self I"));
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Mana),MakeShared<FJsonValueObject>(Heal)});
        S->SetArrayField(TEXT("action_events"),{});S->SetNumberField(TEXT("mana"),20);
        TestEqual(TEXT("Begin mana spell recovery"),Step(Priority,S,P)->GetNumberField(TEXT("spell")),Mana->GetNumberField(TEXT("id")));
        S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("health"),20);S->SetNumberField(TEXT("action_serial"),1);
        TestEqual(TEXT("Health spell preempts mana awaiting result"),Step(Priority,S,P)->GetNumberField(TEXT("spell")),Heal->GetNumberField(TEXT("id")));
        auto FailedMana=RecoveryJson(TEXT(R"({"type":"cast","target":1,"action_serial":2,"requested_at":100,"error":1,"confirmed":false})"));FailedMana->SetNumberField(TEXT("spell"),Mana->GetNumberField(TEXT("id")));
        S->SetNumberField(TEXT("time"),102);S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(FailedMana)});
        TestFalse(TEXT("Delayed mana failure does not reject or repeat in-flight healing"),Step(Priority,S,P)->HasField(TEXT("action")));
        auto ConfirmedHeal=RecoveryJson(TEXT(R"({"type":"cast","target":1,"action_serial":3,"requested_at":101,"error":0,"confirmed":true})"));ConfirmedHeal->SetNumberField(TEXT("spell"),Heal->GetNumberField(TEXT("id")));
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(FailedMana),MakeShared<FJsonValueObject>(ConfirmedHeal)});
        TestEqual(TEXT("Confirmed heal can repeat when incoming damage keeps net HP low"),Step(Priority,S,P)->GetNumberField(TEXT("spell")),Heal->GetNumberField(TEXT("id")));
    }
    // Three consecutive successful recovery cycles must not exhaust a lifetime retry counter.
    FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"id":42,"wcid":42,"name":"Potion","type":32,"usable":true,"identified":true,"boost_vital":2,"boost":100,"count":10})")))});
    for(int Cycle=0;Cycle<5;++Cycle)
    {
        S->SetNumberField(TEXT("health"),20);S->SetNumberField(TEXT("time"),100+Cycle*20);
        auto I=Step(VM,S,P);FString Action;I->TryGetStringField(TEXT("action"),Action);TestEqual(TEXT("Repeated recovery remains active"),Action,FString(TEXT("use_item")));
        S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("time"),110+Cycle*20);
        // Vitals and UseDone are independent network updates; observe vitals first.
        Step(VM,S,P);S->SetNumberField(TEXT("action_serial"),Cycle+1);
    }
    for(int32 School:{2,3,4})for(int32 Requested=0;Requested<=8;++Requested)
    {
        auto State=Snapshot(),Settings=Profile();Settings->SetBoolField(TEXT("buffing"),true);Settings->SetBoolField(TEXT("recovery"),false);
        const TCHAR* Key=School==2?TEXT("life_buff_level"):School==3?TEXT("item_buff_level"):TEXT("creature_buff_level");
        Settings->SetNumberField(Key,Requested);TArray<TSharedPtr<FJsonValue>> Book;
        for(int32 Level=8;Level>=1;--Level)
        {
            auto Spell=RecoveryJson(TEXT(R"({"category":1,"duration":300,"skill":600,"self_buff":true,"beneficial":true,"caster_target":true})"));
            Spell->SetNumberField(TEXT("id"),Level);Spell->SetNumberField(TEXT("level"),Level);
            Spell->SetNumberField(TEXT("power"),Level*40+17);Spell->SetNumberField(TEXT("school"),School);Book.Add(MakeShared<FJsonValueObject>(Spell));
        }
        State->SetArrayField(TEXT("spells"),Book);FACEPluginVM Policy;Policy.Load(Source,Error);
        TestEqual(TEXT("Each school selects exact formula tier or automatic highest"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),double(Requested?Requested:8));
        if(Requested)
        {
            Book.RemoveAll([Requested](const auto& Spell){return Spell->AsObject()->GetNumberField(TEXT("level"))==Requested;});
            State->SetArrayField(TEXT("spells"),Book);FACEPluginVM Missing;Missing.Load(Source,Error);
            TestFalse(TEXT("Missing explicit tier is skipped instead of casting a different level"),Step(Missing,State,Settings)->HasField(TEXT("action")));
        }
    }
    {
        auto State=Snapshot(),Settings=Profile();State->SetNumberField(TEXT("force_buff_request"),1);
        auto Bad=RecoveryJson(TEXT(R"({"id":1,"name":"Failing buff","category":1,"school":4,"duration":300,"skill":500,"power":100,"self_buff":true})"));
        auto Good=RecoveryJson(TEXT(R"({"id":2,"name":"Working buff","category":101,"school":2,"duration":300,"skill":500,"power":100,"self_buff":true})"));
        State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Bad),MakeShared<FJsonValueObject>(Good)});
        FACEPluginVM Policy;Policy.Load(Source,Error);
        for(int32 Attempt=0;Attempt<3;++Attempt)
        {State->SetNumberField(TEXT("time"),100+13*Attempt);TestEqual(TEXT("Unconfirmed buff has bounded retries"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),1.);}
        State->SetNumberField(TEXT("time"),140);auto I=Step(Policy,State,Settings);
        TestFalse(TEXT("Exhausted buff is skipped without stopping UCM"),I->HasField(TEXT("action")));
        TestEqual(TEXT("Next family casts after skipped buff"),Step(Policy,State,Settings)->GetNumberField(TEXT("spell")),2.);
        State->SetNumberField(TEXT("action_serial"),1);State->SetNumberField(TEXT("last_spell"),2);I=Step(Policy,State,Settings);
        TestEqual(TEXT("Failed family does not prevent forced cycle completion"),I->GetStringField(TEXT("action")),FString(TEXT("force_buff_done")));
        TestTrue(TEXT("Completion discloses skipped buffs"),I->GetStringField(TEXT("status")).Contains(TEXT("skipped")));
        // A wholly unsupplied family must not block the available family or stop the run.
        State=Snapshot();State->SetNumberField(TEXT("force_buff_request"),2);State->SetBoolField(TEXT("components_required"),true);
        Bad->SetBoolField(TEXT("components_known"),false);State->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Bad),MakeShared<FJsonValueObject>(Good)});
        FACEPluginVM Missing;Missing.Load(Source,Error);
        TestEqual(TEXT("Missing components skip only the affected family"),Step(Missing,State,Settings)->GetNumberField(TEXT("spell")),2.);
        State->SetNumberField(TEXT("action_serial"),1);State->SetNumberField(TEXT("last_spell"),2);I=Step(Missing,State,Settings);
        TestEqual(TEXT("Unavailable family still allows completion"),I->GetStringField(TEXT("action")),FString(TEXT("force_buff_done")));
        TestTrue(TEXT("Unavailable completion reports skips"),I->GetStringField(TEXT("status")).Contains(TEXT("skipped")));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMResourcesTest,"ACE.Plugins.ResourceMaintenance",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMResourcesTest::RunTest(const FString&)
{
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[](){return RecoveryJson(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"combat_mode":1,"ready":true,"busy":false,"action_serial":0,"action_error":0,"spells":[],"enchantments":[],"targets":[],"inventory":[{"id":20,"wcid":20,"name":"Essence","type":128,"identified":true,"usable":true,"structure":2,"max_structure":50,"int_properties":{"280":213}},{"id":21,"wcid":49485,"name":"Encapsulated Spirit","type":128,"usable":true},{"id":30,"wcid":30,"name":"Mana Stone","type":524288,"identified":true,"usable":true,"mana_empty":true,"mana":0},{"id":31,"wcid":31,"name":"Loot Ring","type":8,"identified":true,"mana":5000,"max_mana":5000},{"id":32,"wcid":31,"name":"Keep Ring","type":8,"identified":true,"mana":5000,"max_mana":5000},{"id":33,"wcid":33,"name":"Armor","type":2,"identified":true,"equipped":true,"mana":10,"max_mana":100}]})"));};
    auto Profile=[](){return RecoveryJson(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":false,"refill_summons":true,"summon_refill_charges":5,"weapon_items":[20],"item_mana":false,"fill_mana_stones":false,"mana_source_items":[31]})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject>I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
    for(int Case=0;Case<8;++Case)
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("ucm_mana_only"),true);P->SetBoolField(TEXT("item_mana"),true);
        auto Inv=S->GetArrayField(TEXT("inventory"));auto First=Inv[2]->AsObject();
        First->SetNumberField(TEXT("mana"),5000);First->SetBoolField(TEXT("mana_empty"),false);
        auto Second=MakeShared<FJsonObject>(*First);Second->SetNumberField(TEXT("id"),40);Inv.Add(MakeShared<FJsonValueObject>(Second));S->SetArrayField(TEXT("inventory"),Inv);
        if(Case==0)First->SetBoolField(TEXT("identified"),false);
        if(Case==1)First->SetBoolField(TEXT("usable"),false);
        if(Case==2)First->SetBoolField(TEXT("equipped"),true);
        if(Case==3)First->SetBoolField(TEXT("mana_supply_available"),false);
        if(Case==4)First->SetNumberField(TEXT("count"),0);
        if(Case==5){First->SetObjectField(TEXT("int_properties"),RecoveryJson(TEXT(R"({"280":213})")));S->SetObjectField(TEXT("cooldowns"),RecoveryJson(TEXT(R"({"213":12})")));}
        if(Case==6){Second->SetNumberField(TEXT("workmanship"),7);P->SetNumberField(TEXT("mana_tank_minimum"),1000);}
        if(Case==7){First->SetBoolField(TEXT("retained"),true);First->SetBoolField(TEXT("resource_available"),false);First->SetBoolField(TEXT("mana_supply_available"),true);}
        auto I=Step(VM,S,P);
        TestEqual(TEXT("Recharge skips unavailable supplies, prefers a filled tank, and permits retained charges"),I->GetNumberField(TEXT("item")),Case==7?30.:40.);
    }
    for(bool ReceiptBeforeMana:{false,true})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("ucm_mana_only"),true);P->SetBoolField(TEXT("item_mana"),true);
        auto Charge=S->GetArrayField(TEXT("inventory"))[2]->AsObject();Charge->SetNumberField(TEXT("mana"),5000);
        Step(VM,S,P);
        auto Success=RecoveryJson(TEXT(R"({"type":"use","source":30,"target":1,"requested_at":100,"action_serial":1,"error":0})"));
        auto Unrelated=RecoveryJson(TEXT(R"({"type":"use","source":21,"requested_at":101,"action_serial":2,"error":1})"));
        auto OldAttempt=RecoveryJson(TEXT(R"({"type":"use","source":30,"requested_at":90,"action_serial":3,"error":1})"));
        S->SetNumberField(TEXT("time"),102);S->SetNumberField(TEXT("action_serial"),3);S->SetNumberField(TEXT("action_error"),1);
        if(ReceiptBeforeMana)S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Success),MakeShared<FJsonValueObject>(Unrelated),MakeShared<FJsonValueObject>(OldAttempt)});
        else S->SetArrayField(TEXT("action_events"),{});
        TestFalse(TEXT("Use receipt or unrelated failure cannot issue another recharge"),Step(VM,S,P)->HasField(TEXT("action")));
        auto Gear=S->GetArrayField(TEXT("inventory"))[5]->AsObject();Gear->SetNumberField(TEXT("mana"),100);
        S->SetNumberField(TEXT("time"),103);Step(VM,S,P);
        // A mana update may precede UseDone on GDLE; ACE may return the
        // appraisal after UseDone. Neither ordering loses successful recovery.
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Success),MakeShared<FJsonValueObject>(Unrelated)});
        Gear->SetNumberField(TEXT("mana"),0);S->SetNumberField(TEXT("time"),130);
        TestEqual(TEXT("Fresh mana confirms the cycle despite unrelated or stale failures"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("ucm_mana_only"),true);P->SetBoolField(TEXT("item_mana"),true);
        S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetNumberField(TEXT("mana"),5000);Step(VM,S,P);
        S->SetNumberField(TEXT("time"),102);S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":30,"requested_at":100,"action_serial":1,"error":1})")))});
        Step(VM,S,P);S->SetNumberField(TEXT("time"),130);
        TestFalse(TEXT("A matching recharge failure applies its own bounded cooldown"),Step(VM,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),133);
        TestEqual(TEXT("A rejected recharge resumes without restarting UCM"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
    }
    for(const TCHAR* Supply:{TEXT("Mana Stone"),TEXT("Titan Mana Charge"),TEXT("Custom Mana Reservoir")})
    {
        FACEPluginVM VM;TestTrue(TEXT("Stopped recharge policy loads"),VM.Load(Source,Error));
        auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("ucm_mana_only"),true);P->SetBoolField(TEXT("item_mana"),true);
        P->SetNumberField(TEXT("item_mana_threshold"),33);
        auto Inv=S->GetArrayField(TEXT("inventory"));auto Charge=Inv[2]->AsObject(),Jewel=Inv[5]->AsObject();
        Charge->SetStringField(TEXT("name"),Supply);Charge->SetNumberField(TEXT("mana"),5000);Charge->SetBoolField(TEXT("mana_empty"),false);
        Jewel->SetStringField(TEXT("name"),TEXT("Encrusted Bloodstone Jewel"));Jewel->SetNumberField(TEXT("type"),8);Jewel->SetNumberField(TEXT("mana"),34);
        TestFalse(TEXT("Stopped service waits while every worn item exceeds 33 percent"),Step(VM,S,P)->HasField(TEXT("action")));
        Jewel->SetNumberField(TEXT("mana"),32);S->SetNumberField(TEXT("time"),102);
        auto I=Step(VM,S,P);TestEqual(TEXT("Any worn jewelry crossing the threshold recharges with each supported supply"),I->GetStringField(TEXT("action")),FString(TEXT("use_item")));
        TestEqual(TEXT("Selected recharge supply is used"),I->GetNumberField(TEXT("item")),30.);
        S->SetNumberField(TEXT("time"),104);S->SetNumberField(TEXT("action_serial"),1);
        TestFalse(TEXT("UseDone alone cannot trigger another charge"),Step(VM,S,P)->HasField(TEXT("action")));
        Jewel->SetNumberField(TEXT("mana"),100);Charge->SetNumberField(TEXT("mana"),0);
        S->SetNumberField(TEXT("time"),106);TestFalse(TEXT("Fresh full suit stops further use"),Step(VM,S,P)->HasField(TEXT("action")));
        // A replacement supply and another drain cycle work without restarting.
        Charge->SetNumberField(TEXT("mana"),5000);Jewel->SetNumberField(TEXT("mana"),0);S->SetNumberField(TEXT("time"),140);
        TestEqual(TEXT("Empty jewel triggers recharge again while UCM remains stopped"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
        auto Failure=RecoveryJson(TEXT(R"({"activity":"item_mana","status":"Temporary server rejection"})"));
        P->SetObjectField(TEXT("ucm_activity_failure"),Failure);S->SetNumberField(TEXT("time"),142);
        TestEqual(TEXT("Idle failure uses a bounded activity pause"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        P->RemoveField(TEXT("ucm_activity_failure"));S->SetNumberField(TEXT("time"),144);
        TestFalse(TEXT("Idle retry pause does not hammer supplies"),Step(VM,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),174);
        TestEqual(TEXT("Stopped recharge resumes after the failure cooldown"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("use_item")));
    }
    {
        FACEPluginVM VM;TestTrue(TEXT("Resource maintenance policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();
        auto I=Step(VM,S,P);TestEqual(TEXT("Low essence gets a spirit independently of automatic combat/summoning"),I->GetStringField(TEXT("action")),FString(TEXT("apply_item")));
        TestEqual(TEXT("Spirit is source"),I->GetNumberField(TEXT("item")),21.);TestEqual(TEXT("Essence is target"),I->GetNumberField(TEXT("target")),20.);
        S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("action_serial"),1);
        TestFalse(TEXT("UseDone alone cannot consume another spirit"),Step(VM,S,P)->HasField(TEXT("action")));
        S->GetArrayField(TEXT("inventory"))[0]->AsObject()->SetNumberField(TEXT("structure"),50);
        TestFalse(TEXT("Confirmed full essence stops refilling"),Step(VM,S,P)->HasField(TEXT("action")));
    }
    for(int Case=0;Case<5;++Case)
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Inv=S->GetArrayField(TEXT("inventory"));
        if(Case==0)P->SetBoolField(TEXT("refill_summons"),false);
        if(Case==1)Inv[0]->AsObject()->SetNumberField(TEXT("structure"),6);
        if(Case==2)Inv[1]->AsObject()->SetNumberField(TEXT("wcid"),999);
        if(Case==3)P->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(99)});
        if(Case==4)Inv[0]->AsObject()->SetBoolField(TEXT("identified"),false);
        TestFalse(TEXT("Disabled, sufficient, missing spirit, excluded or unassessed essences are not refilled"),Step(VM,S,P)->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Step(VM,S,P);
        S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),1);
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":999,"target":20,"action_serial":1,"requested_at":100,"error":1})")))});
        TestFalse(TEXT("Unrelated use cannot reject or repeat pet refill"),Step(VM,S,P)->HasField(TEXT("action")));
        S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"type":"use","source":21,"target":20,"action_serial":2,"requested_at":100,"error":1})")))});
        TestFalse(TEXT("Rejected refill backs off without stopping UCM"),Step(VM,S,P)->HasField(TEXT("action")));
        S->SetNumberField(TEXT("time"),132);S->SetNumberField(TEXT("action_error"),0);
        TestEqual(TEXT("Refill can retry after backoff"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("apply_item")));
    }
    const auto PrepareLoot=[&](auto S,auto P)
    {
        P->SetBoolField(TEXT("refill_summons"),false);P->SetBoolField(TEXT("fill_mana_stones"),true);P->SetBoolField(TEXT("looting"),true);
        P->RemoveField(TEXT("mana_source_items")); // No manual pool and no loot profile required.
        P->SetArrayField(TEXT("consumables"),{MakeShared<FJsonValueNumber>(999)}); // Other supplies must not exclude empty stones.
        auto Inv=S->GetArrayField(TEXT("inventory"));auto Donor=Inv[3];Inv.RemoveAt(3);
        Donor->AsObject()->SetNumberField(TEXT("count"),1);Donor->AsObject()->SetNumberField(TEXT("icon_effects"),1);Donor->AsObject()->SetNumberField(TEXT("workmanship"),7);
        S->SetArrayField(TEXT("inventory"),Inv);S->SetArrayField(TEXT("contents"),{Donor});S->SetNumberField(TEXT("container"),700);S->SetBoolField(TEXT("container_is_corpse"),true);
        return Donor;
    };
    const auto ReceiveLoot=[&](FACEPluginVM& VM,auto S,auto P,auto Donor)
    {
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.Add(Donor);S->SetArrayField(TEXT("inventory"),Inv);
        S->SetArrayField(TEXT("contents"),{});S->SetNumberField(TEXT("container"),0);S->SetNumberField(TEXT("time"),101);
        Step(VM,S,P); // Confirm the exact new inventory item before any consumption.
    };
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Donor=PrepareLoot(S,P);
        P->SetBoolField(TEXT("item_mana"),true);
        auto I=Step(VM,S,P);TestEqual(TEXT("Junk is automatically looted without a source pool or loot rules"),I->GetStringField(TEXT("action")),FString(TEXT("loot")));
        TestEqual(TEXT("Only one donor is needed for one empty stone"),I->GetNumberField(TEXT("amount")),1.);
        S->SetNumberField(TEXT("container"),0);S->SetArrayField(TEXT("contents"),{});
        TestFalse(TEXT("Closing the corpse does not authorize destroying a carried same-WCID ring"),Step(VM,S,P)->HasField(TEXT("action")));
        ReceiveLoot(VM,S,P,Donor);
        I=Step(VM,S,P);TestEqual(TEXT("Empty stone fills via normal targeted use"),I->GetStringField(TEXT("action")),FString(TEXT("apply_item")));
        TestEqual(TEXT("Only the confirmed junk pickup is consumed"),I->GetNumberField(TEXT("target")),31.);
        S->SetNumberField(TEXT("time"),102);S->SetNumberField(TEXT("action_serial"),1);
        TestFalse(TEXT("Stone filling waits for actual contents"),Step(VM,S,P)->HasField(TEXT("action")));
        auto Stone=S->GetArrayField(TEXT("inventory"))[2]->AsObject();Stone->SetNumberField(TEXT("mana"),5000);Stone->SetBoolField(TEXT("mana_empty"),false);
        P->RemoveField(TEXT("consumables")); // Gear recharge retains its separate selected-consumable policy.
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.Pop();S->SetArrayField(TEXT("inventory"),Inv);
        I=Step(VM,S,P);TestEqual(TEXT("Newly filled stone recharges gear through player-targeting use"),I->GetStringField(TEXT("action")),FString(TEXT("use_item")));
        TestEqual(TEXT("Uses the filled stone"),I->GetNumberField(TEXT("item")),30.);
        Inv[4]->AsObject()->SetNumberField(TEXT("mana"),100);
        TestFalse(TEXT("Full gear does not consume another charge"),Step(VM,S,P)->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Donor=PrepareLoot(S,P);
        TestEqual(TEXT("First donor is requested"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.Add(Donor);S->SetArrayField(TEXT("inventory"),Inv);
        auto Extra=RecoveryJson(TEXT(R"({"id":34,"wcid":34,"name":"Extra junk","type":8,"count":1,"identified":true,"icon_effects":1,"workmanship":8,"mana":10000,"max_mana":10000})"));
        S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Extra)});
        TestEqual(TEXT("Enough donors closes the corpse without over-collecting, even without loot rules"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
        TestEqual(TEXT("The reserved donor still fills the stone"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("apply_item")));
    }
    for(int Case=0;Case<12;++Case)
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Donor=PrepareLoot(S,P);
        TestEqual(TEXT("Acquire source for post-pickup protection check"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
        ReceiveLoot(VM,S,P,Donor);auto Item=Donor->AsObject();
        if(Case==0)P->SetBoolField(TEXT("fill_mana_stones"),false);
        if(Case==1)Item->SetBoolField(TEXT("equipped"),true);
        if(Case==2)Item->SetBoolField(TEXT("retained"),true);
        if(Case==3)Item->SetBoolField(TEXT("resource_available"),false);
        if(Case==4)S->GetArrayField(TEXT("inventory"))[2]->AsObject()->SetBoolField(TEXT("mana_empty"),false);
        if(Case==5)Item->SetBoolField(TEXT("identified"),false);
        if(Case==6)Item->SetObjectField(TEXT("int_properties"),RecoveryJson(TEXT(R"({"171":1})")));
        if(Case==7)Item->SetObjectField(TEXT("string_properties"),RecoveryJson(TEXT(R"({"8":"Player"})")));
        if(Case==8)Item->SetNumberField(TEXT("count"),2);
        if(Case==9)Item->SetNumberField(TEXT("wcid"),999);
        if(Case==10)P->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(31)});
        if(Case==11)P->SetNumberField(TEXT("mana_tank_minimum"),6000);
        const auto I=Step(VM,S,P);
        if(Case==5)TestEqual(TEXT("A pickup awaiting fresh appraisal is identified, never discarded"),I->GetStringField(TEXT("action")),FString(TEXT("identify")));
        else TestFalse(*FString::Printf(TEXT("Changed/protected pickup is not consumed (case %d)"),Case),I->HasField(TEXT("action")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Donor=PrepareLoot(S,P);
        TestEqual(TEXT("Acquire exactly one mana donor before its ownership update"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
        // CreateObject on pickup invalidates the appraisal. Existing fixtures
        // reused the already-identified corpse JSON and hid this real ordering.
        Donor->AsObject()->SetBoolField(TEXT("identified"),false);
        Donor->AsObject()->RemoveField(TEXT("mana"));Donor->AsObject()->RemoveField(TEXT("max_mana"));
        auto Inv=S->GetArrayField(TEXT("inventory"));Inv.Add(Donor);S->SetArrayField(TEXT("inventory"),Inv);
        auto Extra=RecoveryJson(TEXT(R"({"id":34,"wcid":34,"name":"More junk","type":8,"count":1,"identified":true,"icon_effects":1,"workmanship":8,"mana":10000,"max_mana":10000})"));
        S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Extra)});
        auto I=Step(VM,S,P);TestTrue(TEXT("Unassessed reserved donor prevents another junk pickup"),!I->HasField(TEXT("action"))||I->GetStringField(TEXT("action"))!=TEXT("loot"));
        S->SetNumberField(TEXT("container"),0);S->SetArrayField(TEXT("contents"),{});
        I=Step(VM,S,P);TestEqual(TEXT("Post-pickup appraisal is requested before consumption"),I->GetStringField(TEXT("action")),FString(TEXT("identify")));
        TestEqual(TEXT("Appraises only the reserved pickup"),I->GetNumberField(TEXT("item")),31.);
        S->SetNumberField(TEXT("time"),102);I=Step(VM,S,P);
        TestFalse(TEXT("No repeated identify or use while appraisal is pending"),I->HasField(TEXT("action")));
        Donor->AsObject()->SetBoolField(TEXT("identified"),true);Donor->AsObject()->SetNumberField(TEXT("mana"),5000);Donor->AsObject()->SetNumberField(TEXT("max_mana"),5000);
        I=Step(VM,S,P);TestEqual(TEXT("Fresh appraisal resumes filling without another corpse"),I->GetStringField(TEXT("action")),FString(TEXT("apply_item")));
        TestEqual(TEXT("The freshly assessed donor is consumed"),I->GetNumberField(TEXT("target")),31.);
    }
    for(int Case=0;Case<9;++Case)
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Item=PrepareLoot(S,P)->AsObject();
        if(Case==0)Item->SetNumberField(TEXT("mana"),999);
        if(Case==1)Item->SetNumberField(TEXT("workmanship"),0);
        if(Case==2)Item->SetNumberField(TEXT("icon_effects"),0);
        if(Case==3)Item->SetObjectField(TEXT("int_properties"),RecoveryJson(TEXT(R"({"171":1})")));
        if(Case==4)Item->SetObjectField(TEXT("string_properties"),RecoveryJson(TEXT(R"({"8":"Player"})")));
        if(Case==5)Item->SetBoolField(TEXT("retained"),true);
        if(Case==6)Item->SetNumberField(TEXT("max_mana"),0);
        if(Case==7)Item->SetNumberField(TEXT("type"),524288);
        if(Case==8)P->SetNumberField(TEXT("mana_tank_minimum"),5001);
        TestEqual(*FString::Printf(TEXT("Unsuitable junk remains on corpse (case %d)"),Case),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
    }
    for(const FString Action:{TEXT("keep"),TEXT("skip"),TEXT("limit")})
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Donor=PrepareLoot(S,P);
        auto Rule=RecoveryJson(TEXT(R"({"action":"keep"})"));
        if(Action==TEXT("skip"))Rule->SetStringField(TEXT("action"),Action);
        if(Action==TEXT("limit"))Rule->SetNumberField(TEXT("keep_up_to"),1); // Already carrying a same-WCID ring.
        P->SetArrayField(TEXT("loot_rules"),{MakeShared<FJsonValueObject>(Rule)});
        TestEqual(TEXT("Keep or otherwise unwanted mana loot is picked up"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
        ReceiveLoot(VM,S,P,Donor);auto I=Step(VM,S,P);
        if(Action==TEXT("keep"))TestFalse(TEXT("Keep takes precedence over automatic mana filling"),I->HasField(TEXT("action")));
        else TestEqual(TEXT("VT NoLoot and excess KeepUpTo can supply stones"),I->GetStringField(TEXT("action")),FString(TEXT("apply_item")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Donor=PrepareLoot(S,P);
        Donor->AsObject()->SetBoolField(TEXT("identified"),false);
        TestEqual(TEXT("Magical junk is appraised before being selected"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("identify")));
        Donor->AsObject()->SetBoolField(TEXT("identified"),true);Donor->AsObject()->SetNumberField(TEXT("mana"),1000);
        TestEqual(TEXT("VT default minimum includes exactly 1000 mana"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();PrepareLoot(S,P);
        S->SetArrayField(TEXT("contents"),{});S->SetNumberField(TEXT("container"),0);
        S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(RecoveryJson(TEXT(R"({"id":700,"name":"Corpse","distance":2})")))});
        TestEqual(TEXT("Empty stones enable corpse search with no loot profile"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
        P->SetBoolField(TEXT("refill_summons"),false);P->SetBoolField(TEXT("fill_mana_stones"),true);
        TestFalse(TEXT("Legacy manual source IDs never authorize consuming existing possessions"),Step(VM,S,P)->HasField(TEXT("action")));
    }
    return !HasAnyErrors();
}
#endif
