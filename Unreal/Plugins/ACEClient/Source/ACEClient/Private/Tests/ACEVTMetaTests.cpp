#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEVTProfile.h"
#include "Mods/ACEPluginVM.h"
namespace
{
using J=TSharedPtr<FJsonObject>;using V=TSharedPtr<FJsonValue>;using A=TArray<V>;
J JSONMeta(const TCHAR* Text){J O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
V StrMeta(const FString& Text){return MakeShared<FJsonValueString>(Text);}
V NumMeta(double N){return MakeShared<FJsonValueNumber>(N);}
V PropsMeta(const J& Props)
{
    auto T=MakeShared<FJsonObject>();A Rows;
    for(const auto& P:Props->Values)Rows.Add(MakeShared<FJsonValueArray>(A{StrMeta(FString(P.Key)),P.Value}));
    T->SetArrayField(TEXT("columns"),{StrMeta(TEXT("k")),StrMeta(TEXT("v"))});T->SetArrayField(TEXT("rows"),Rows);return MakeShared<FJsonValueObject>(T);
}
V RuleMeta(int C,int Act,const V& CD,const V& AD,const FString& State)
{return MakeShared<FJsonValueArray>(A{NumMeta(C),NumMeta(Act),CD,AD,StrMeta(State)});}
J MetaDocument(const A& Rows)
{
    auto T=JSONMeta(TEXT(R"({"name":"CondAct","columns":["CType","AType","CData","AData","State"]})"));T->SetArrayField(TEXT("rows"),Rows);
    auto D=MakeShared<FJsonObject>();D->SetStringField(TEXT("format"),TEXT("met"));D->SetArrayField(TEXT("tables"),{MakeShared<FJsonValueObject>(T)});return D;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVTMetaTest,"ACE.Plugins.VirindiRuntime",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEVTMetaTest::RunTest(const FString&)
{
    FString Script,Error;TArray<FString> Issues;J Intent;
    FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snap=JSONMeta(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":0,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"inventory":[],"action_serial":0,"teleport_sequence":0,"chat_serial":0,"portal_serial":0})"));
    auto Base=[](J P){P->SetStringField(TEXT("combat"),TEXT("off"));P->SetBoolField(TEXT("buffing"),false);P->SetBoolField(TEXT("recovery"),false);return P;};
    {
        auto P=Base(ACEVTProfile::Convert(MetaDocument({
            RuleMeta(1,2,NumMeta(0),StrMeta(TEXT("/vt opt set EnableMeta true")),TEXT("Default")),
            RuleMeta(6,1,NumMeta(10),StrMeta(TEXT("Done")),TEXT("Default"))}),Issues));
        auto S=MakeShared<FJsonObject>(*Snap);FACEPluginVM VM;VM.Load(Script,Error);
        TestTrue(TEXT("Meta enables its own rules"),VM.Step(S,P,Intent,Error));
        P->SetBoolField(TEXT("meta_enabled"),false);P->SetNumberField(TEXT("loot_revision"),1);S->SetNumberField(TEXT("time"),111);
        TestTrue(TEXT("Live UI pause supersedes the old meta option"),VM.Step(S,P,Intent,Error));
        if(Intent){TestEqual(TEXT("Paused meta does not transition"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Default")));TestTrue(TEXT("Diagnostics explain disabled execution"),Intent->GetStringField(TEXT("meta_status")).Contains(TEXT("paused")));}
        P->SetBoolField(TEXT("meta_enabled"),true);P->SetNumberField(TEXT("loot_revision"),2);
        TestTrue(TEXT("UI resume processes the pending timer"),VM.Step(S,P,Intent,Error));
        if(Intent)TestEqual(TEXT("Resumed meta transitions normally"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Done")));
    }
    {
        const FString Checks=TEXT("istrue[2]==1&&istrue[0]==0&&istrue[`text`]==0&&isfalse[0]==1&&isfalse[`text`]==0&&isfalse[getcharstringprop[999]]==1")
            TEXT("&&iif[1,3,4]==3&&iif[`text`,3,4]==4&&iif[1,false,true]==false")
            TEXT("&&round[2.5]==2&&round[3.5]==4&&round[-1.5]==-2&&round[-2.5]==-2&&round[2.6]==3")
            TEXT("&&floor[-1.1]==-2&&ceiling[-1.1]==-1&&abs[-3]==3&&strlen[`caf\u00e9`]==4&&strlen[`\U0001f600`]==2")
            TEXT("&&cnumber[`1,250.5`]==1250.5&&cnumber[`bad`]==0&&cnumber[`0x10`]==0&&cnumber[`1e2`]==100&&randint[3,4]==3&&randint[5,5]==5");
        auto Props=MakeShared<FJsonObject>();Props->SetStringField(TEXT("e"),Checks);
        auto Profile=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(26,1,PropsMeta(Props),StrMeta(TEXT("MathOK")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Scalar utility expressions compile"),Issues.Num(),0);
        FACEPluginVM VM;VM.Load(Script,Error);TestTrue(TEXT("Scalar utility expressions run"),VM.Step(Snap,Profile,Intent,Error));if(!Error.IsEmpty())AddError(Error);
        if(Intent)TestEqual(TEXT("Numeric predicates, midpoint rounding, UTF16 length and exclusive random bounds"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("MathOK")));
        auto Timer=Base(ACEVTProfile::Convert(MetaDocument({
            RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"setvar[t,stopwatchstart[stopwatchcreate[]]];setvar[t,stopwatchstop[getvar[t]]]"})"))),TEXT("Default")),
            RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"stopwatchelapsedseconds[getvar[t]]==0"})"))),StrMeta(TEXT("TimerOK")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Nested stopwatch methods compile"),Issues.Num(),0);FACEPluginVM Clock;Clock.Load(Script,Error);TestTrue(TEXT("Stopwatch methods retain object identity"),Clock.Step(Snap,Timer,Intent,Error));
        if(Intent)TestEqual(TEXT("Returned stopwatch can be reused"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("TimerOK")));
        for(const TCHAR* Invalid:{TEXT("floor[`oops`]"),TEXT("randint[5,2]"),TEXT("strlen[3]")})
        {
            auto P=MakeShared<FJsonObject>();P->SetStringField(TEXT("e"),Invalid);auto Bad=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(P),TEXT("Default"))}),Issues));
            FACEPluginVM Guard;Guard.Load(Script,Error);TestFalse(TEXT("Wrong scalar types and reversed bounds stop safely"),Guard.Step(Snap,Bad,Intent,Error));
        }
    }
    {
        auto Properties=JSONMeta(TEXT(R"({"char_ints":{"25":275},"char_quads":{"2":8000000000000},"char_doubles":{"3":0.25},"char_bools":{"10":1,"11":0},"char_strings":{"1":"Player","5":"","9000":"Custom"}})"));
        for(const auto& Field:Properties->Values)Snap->SetField(Field.Key,Field.Value);
        auto TestProps=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"getcharquadprop[2]==8000000000000&&getchardoubleprop[3]==0.25&&getcharboolprop[10]==1&&getcharboolprop[11]==0&&getcharboolprop[999]==0&&strlen[getcharstringprop[5]]==0&&getcharstringprop[9000]==`Custom`&&getcharintprop[25.9]==275"})"))),StrMeta(TEXT("PropertiesOK")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Character property expressions compile"),Issues.Num(),0);FACEPluginVM PropsVM;PropsVM.Load(Script,Error);
        if(!TestTrue(TEXT("Character property types run"),PropsVM.Step(Snap,TestProps,Intent,Error)))AddError(Error);
        if(Intent)TestEqual(TEXT("Server qualities preserve numeric types, empty strings and custom IDs"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("PropertiesOK")));
        for(const TCHAR* Invalid:{TEXT("getcharintprop[`25`]"),TEXT("getcharquadprop[`bad`]"),TEXT("getcharboolprop[2147483648]")})
        {
            auto P=MakeShared<FJsonObject>();P->SetStringField(TEXT("e"),Invalid);auto Bad=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(P),TEXT("Default"))}),Issues));
            FACEPluginVM Guard;Guard.Load(Script,Error);TestFalse(TEXT("Property queries reject nonnumeric or out of range keys"),Guard.Step(Snap,Bad,Intent,Error));
        }
    }
    {
        auto Empty=Base(ACEVTProfile::Convert(MetaDocument({}),Issues));TestEqual(TEXT("Empty retail meta is a valid inactive rule set"),Issues.Num(),0);
        FACEPluginVM Blank;Blank.Load(Script,Error);TestTrue(TEXT("Empty meta executes without actions"),Blank.Step(Snap,Empty,Intent,Error));
        if(Intent)TestFalse(TEXT("Empty meta does not fabricate actions"),Intent->HasField(TEXT("action")));
        auto Missing=JSONMeta(TEXT(R"({"format":"met","tables":[]})"));ACEVTProfile::Convert(Missing,Issues);TestTrue(TEXT("Missing schema still rejected"),Issues.Num()>0);
        auto Noop=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,2,NumMeta(0),StrMeta(TEXT("")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Blank command is an intentional no-op"),Issues.Num(),0);FACEPluginVM NoopVM;NoopVM.Load(Script,Error);TestTrue(TEXT("No-op meta runs"),NoopVM.Step(Snap,Noop,Intent,Error));
    }
    {
        auto ResumeProfile=Base(ACEVTProfile::Convert(MetaDocument({
            RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"setvar[count,getvar[count]+1];setvar[clock,stopwatchstart[stopwatchcreate[]]]"})"))),TEXT("Default")),
            RuleMeta(22,1,NumMeta(10),StrMeta(TEXT("Persistent")),TEXT("Default")),
            RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"getvar[count]==1&&stopwatchelapsedseconds[getvar[clock]]>=11"})"))),StrMeta(TEXT("Resumed")),TEXT("Persistent"))}),Issues));
        TestEqual(TEXT("Persistent state timer converts"),Issues.Num(),0);auto O=MakeShared<FJsonObject>(*Snap);O->SetNumberField(TEXT("time"),100);
        FACEPluginVM Paused;Paused.Load(Script,Error);TestTrue(TEXT("Meta starts before pause"),Paused.Step(O,ResumeProfile,Intent,Error));
        O->SetNumberField(TEXT("time"),111);ResumeProfile->SetBoolField(TEXT("ucm_resume"),true);
        TestTrue(TEXT("Persistent timer runs after restart"),Paused.Step(O,ResumeProfile,Intent,Error));
        if(Intent)TestEqual(TEXT("Persistent timer includes stopped time"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Persistent")));
        ResumeProfile->RemoveField(TEXT("ucm_resume"));TestTrue(TEXT("Variables survive restart"),Paused.Step(O,ResumeProfile,Intent,Error));
        if(Intent)TestEqual(TEXT("Restart retains fired rules and stopwatch identity"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Resumed")));
        auto Ordinary=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(6,1,NumMeta(10),StrMeta(TEXT("Elapsed")),TEXT("Default"))}),Issues));
        FACEPluginVM Normal;Normal.Load(Script,Error);O->SetNumberField(TEXT("time"),100);Normal.Step(O,Ordinary,Intent,Error);
        O->SetNumberField(TEXT("time"),111);Ordinary->SetBoolField(TEXT("ucm_resume"),true);Normal.Step(O,Ordinary,Intent,Error);
        if(Intent)TestEqual(TEXT("Ordinary timer resets on restart"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Default")));
        Ordinary->RemoveField(TEXT("ucm_resume"));O->SetNumberField(TEXT("time"),121);Normal.Step(O,Ordinary,Intent,Error);
        if(Intent)TestEqual(TEXT("Ordinary timer expires from resume time"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Elapsed")));
    }
    {
        auto O=MakeShared<FJsonObject>(*Snap);O->SetNumberField(TEXT("selected"),90);
        O->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":90,"name":"Far","distance":40,"object_class":16})"))),MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":91,"name":"Near","distance":10,"object_class":16})")))});
        auto Types=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"getobjectinternaltype[123]==1&&getobjectinternaltype[true]==1&&getobjectinternaltype[`text`]==3&&getobjectinternaltype[listcreate[]]==7&&getobjectinternaltype[stopwatchcreate[]]==7&&getobjectinternaltype[getplayercoordinates[]]==7&&getobjectinternaltype[wobjectgetselection[]]==7&&wobjectgetname[wobjectfindnearestmonster[]]==`Near`&&listcontains[listcreate[wobjectgetplayer[]],wobjectgetplayer[]]"})"))),StrMeta(TEXT("ObjectsOK")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Object type queries compile"),Issues.Num(),0);FACEPluginVM TypesVM;TypesVM.Load(Script,Error);
        TestTrue(TEXT("Typed objects execute"),TypesVM.Step(O,Types,Intent,Error));
        if(Intent)TestEqual(TEXT("Object lookups retain types and reference identity"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("ObjectsOK")));
        for(const TCHAR* Invalid:{TEXT("wobjectgetplayer[]==1"),TEXT("cstr[wobjectgetplayer[]]"),TEXT("`text`&&`other`")})
        {
            auto P=MakeShared<FJsonObject>();P->SetStringField(TEXT("e"),Invalid);auto Bad=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(P),TEXT("Default"))}),Issues));
            FACEPluginVM Guard;Guard.Load(Script,Error);TestFalse(TEXT("Objects are not numeric IDs and strings are not logical operands"),Guard.Step(O,Bad,Intent,Error));
        }
    }
    {
        auto Queries=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"getisspellknown[6150]==1 && getisspellknown[6151]==0"})"))),StrMeta(TEXT("Known")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Spell knowledge query compiles"),Issues.Num(),0);
        auto QuerySnap=MakeShared<FJsonObject>(*Snap);QuerySnap->SetArrayField(TEXT("known_spells"),{NumMeta(6150)});
        FACEPluginVM QueryVM;QueryVM.Load(Script,Error);TestTrue(TEXT("Spell knowledge uses learned IDs even without DAT metadata"),QueryVM.Step(QuerySnap,Queries,Intent,Error));
        if(Intent)TestEqual(TEXT("Spell queries select expected branch"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Known")));
        auto Lookup=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"actiontryuseitem[wobjectfindnearestbynameandobjectclass[37,`^Collector.*$`]]"})"))),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Regex world lookup compiles"),Issues.Num(),0);
        QuerySnap->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":10,"name":"Collector East","object_class":37,"distance":9})"))),MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":11,"name":"Collector West","object_class":37,"distance":2})"))),MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":12,"name":"Collector Door","object_class":26,"distance":1})")))});
        FACEPluginVM LookupVM;LookupVM.Load(Script,Error);TestTrue(TEXT("World lookup executes"),LookupVM.Step(QuerySnap,Lookup,Intent,Error));
        if(Intent)TestEqual(TEXT("Nearest matching name and class selected"),Intent->GetNumberField(TEXT("item")),11.);
        ACEVTProfile::CompileCommand(TEXT("/ucm mexec wobjectfindnearestbynameandobjectclass[37,`(?x)name`]"),Issues);TestEqual(TEXT("Extended-mode literal object-name regex imports"),Issues.Num(),0);
        ACEVTProfile::CompileCommand(TEXT("/ucm mexec wobjectfindnearestbynameandobjectclass[37,`(?<open>a)(?<-open>b)`]"),Issues);TestEqual(TEXT("Balancing object-name regex imports"),Issues.Num(),0);
        auto HistoryLookup=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"actiontryuseitem[wobjectfindnearestbynameandobjectclass[37,`^(?<open>a)+(?<-open>b)+(?(open)(?!))$`]]"})"))),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Capture-history world lookup imports"),Issues.Num(),0);
        QuerySnap->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":20,"name":"aab","object_class":37,"distance":1})"))),MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":21,"name":"aabb","object_class":37,"distance":3})")))});
        FACEPluginVM HistoryVM;HistoryVM.Load(Script,Error);TestTrue(TEXT("Imported balancing and conditional lookup executes in Lua"),HistoryVM.Step(QuerySnap,HistoryLookup,Intent,Error));
        if(TestTrue(TEXT("Balanced lookup requests an action"),Intent.IsValid()))TestEqual(TEXT("Unbalanced closer object is rejected"),Intent->GetNumberField(TEXT("item")),21.);
        auto DistanceOption=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,11,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"o":"AttackDistance","v":"range"})"))),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Option getter supports bounded map-unit distances"),Issues.Num(),0);
        TestEqual(TEXT("Option getter preserves meters-to-map ratio"),DistanceOption->GetArrayField(TEXT("vt_meta"))[0]->AsObject()->GetObjectField(TEXT("action"))->GetNumberField(TEXT("scale")),240.);
    }
    {
        auto CommandDoc=MetaDocument({RuleMeta(1,8,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"`/vt echo ` + `Argument /vt jump text`"})"))),TEXT("Default"))});
        auto CommandProfile=Base(ACEVTProfile::Convert(CommandDoc,Issues));TestEqual(TEXT("Command expression migration compiles"),Issues.Num(),0);
        auto Expression=CommandProfile->GetArrayField(TEXT("vt_meta"))[0]->AsObject()->GetObjectField(TEXT("action"))->GetObjectField(TEXT("expression"));
        TestEqual(TEXT("Command prefix changes but keeps separator"),Expression->GetObjectField(TEXT("left"))->GetStringField(TEXT("value")),FString(TEXT("/acueucm echo ")));
        TestEqual(TEXT("Command argument is not rewritten"),Expression->GetObjectField(TEXT("right"))->GetStringField(TEXT("value")),FString(TEXT("Argument /vt jump text")));
        FACEPluginVM Commands;Commands.Load(Script,Error);TestTrue(TEXT("Converted dynamic command executes"),Commands.Step(Snap,CommandProfile,Intent,Error));
        if(Intent)TestEqual(TEXT("Dynamic echo has correct payload"),Intent->GetStringField(TEXT("text")),FString(TEXT("Argument /vt jump text")));
    }
    // Real typed meta table -> compiler -> sandbox. Calls preserve ordered actions,
    {
        auto Expressions=Base(ACEVTProfile::Convert(MetaDocument({
            RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"setvar[a,listcreate[1,2,1]];setvar[b,listcopy[getvar[a]]];listinsert[getvar[a],4,1];listremove[getvar[a],1];listremoveat[getvar[a],1]"})"))),TEXT("Default")),
            RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"listcount[getvar[a]]==2 && listgetitem[getvar[a],0]==4 && listgetitem[getvar[a],1]==1 && listcount[getvar[b]]==3 && listindexof[getvar[b],1]==0 && listlastindexof[getvar[b],1]==2 && listcontains[getvar[b],2]==1 && listgetitem[listreverse[getvar[b]],1]==2"})"))),StrMeta(TEXT("Done")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("List expressions compile"),Issues.Num(),0);FACEPluginVM Lists;Lists.Load(Script,Error);
        TestTrue(TEXT("List operations execute"),Lists.Step(Snap,Expressions,Intent,Error));
        if(Intent)TestEqual(TEXT("List mutation, zero-based indexes and copy independence"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Done")));
        auto Cycle=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"setvar[a,listcreate[]];setvar[b,listcreate[getvar[a]]];listadd[getvar[a],getvar[b]]"})"))),TEXT("Default"))}),Issues));
        FACEPluginVM Cyclic;Cyclic.Load(Script,Error);TestFalse(TEXT("Indirect list cycle is rejected"),Cyclic.Step(Snap,Cycle,Intent,Error));
        auto FellowshipSnap=MakeShared<FJsonObject>(*Snap);FellowshipSnap->SetStringField(TEXT("world_name"),TEXT("TestWorld"));
        FellowshipSnap->SetObjectField(TEXT("fellowship"),JSONMeta(TEXT(R"({"valid":true,"name":"Test","leader":1,"open":false,"members":[{"id":2,"name":"Zed"},{"id":1,"name":"Alice"}]})")));
        auto Fellow=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"getworldname[]==TestWorld && getfellowshipisleader[]==1 && getfellowshipcanrecruit[]==1 && getfellowshipcount[]==2 && listgetitem[getfellownames[],0]==Alice && listgetitem[getfellowids[],0]==1"})"))),StrMeta(TEXT("Done")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Fellowship queries compile"),Issues.Num(),0);FACEPluginVM FellowVM;FellowVM.Load(Script,Error);
        TestTrue(TEXT("Fellowship snapshot queries run"),FellowVM.Step(FellowshipSnap,Fellow,Intent,Error));
        if(Intent)TestEqual(TEXT("World and fellowship values select branch"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Done")));
        auto Give=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"actiontrygiveitem[wobjectfindininventorybynamerx[`^Aerlinthe.*Note$`],wobjectfindnearestbyobjectclass[37]]"})"))),TEXT("Default"))}),Issues));
        auto GiveSnap=MakeShared<FJsonObject>(*Snap);GiveSnap->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":10,"name":"Aerlinthe C Note","equipped":false})")))});
        GiveSnap->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":20,"name":"Collector","object_class":37,"distance":2})")))});
        FACEPluginVM GiveVM;GiveVM.Load(Script,Error);TestTrue(TEXT("Give expression executes"),GiveVM.Step(GiveSnap,Give,Intent,Error));
        if(Intent){TestEqual(TEXT("Give emits normal give intent"),Intent->GetStringField(TEXT("action")),FString(TEXT("give")));TestEqual(TEXT("Give targets resolved NPC"),Intent->GetNumberField(TEXT("target")),20.);}
    }
    {
        auto CastProfile=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"actiontrycastbyid[157]"})"))),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Cast-by-id adapter compiles"),Issues.Num(),0);
        auto S=MakeShared<FJsonObject>(*Snap);S->SetNumberField(TEXT("combat_mode"),8);
        auto Wand=JSONMeta(TEXT(R"({"id":8,"type":32768,"name":"Orb","equipped":true,"can_wield":true,"identified":true})"));
        auto Spell=JSONMeta(TEXT(R"({"id":157,"power":50,"skill":200,"known":true,"caster_target":true,"scarabs":{"Lead Scarab":1}})"));
        S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Spell)});
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Wand)});
        FACEPluginVM Missing;Missing.Load(Script,Error);TestTrue(TEXT("Cast without scarab evaluates safely"),Missing.Step(S,CastProfile,Intent,Error));if(Intent)TestFalse(TEXT("Missing scarab prevents cast"),Intent->HasField(TEXT("action")));
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Wand),MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":9,"name":"Lead Scarab","count":1})")))});
        FACEPluginVM CastVM;CastVM.Load(Script,Error);TestTrue(TEXT("Eligible expression cast executes"),CastVM.Step(S,CastProfile,Intent,Error));
        if(Intent){TestEqual(TEXT("Cast emits normal spell request"),Intent->GetStringField(TEXT("action")),FString(TEXT("cast")));TestEqual(TEXT("Self expression targets player"),Intent->GetNumberField(TEXT("target")),1.);}
        S->SetNumberField(TEXT("time"),101);CastVM.Step(S,CastProfile,Intent,Error);if(Intent)TestFalse(TEXT("Expression cast waits for server completion"),Intent->HasField(TEXT("action")));
        auto EquipProfile=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"actiontryequipanywand[]"})"))),TEXT("Default"))}),Issues));
        Wand->SetBoolField(TEXT("equipped"),false);FACEPluginVM EquipVM;EquipVM.Load(Script,Error);EquipVM.Step(S,EquipProfile,Intent,Error);
        if(Intent)TestEqual(TEXT("Equip expression takes equipment step"),Intent->GetStringField(TEXT("action")),FString(TEXT("equip")));
        auto Inventory=S->GetArrayField(TEXT("inventory"));
        Inventory.Add(MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":20,"wcid":20,"name":"Essence","type":128,"identified":true,"pet_device":true})"))));
        S->SetArrayField(TEXT("inventory"),Inventory);
        for(uint32 Selected:{20u,999u})
        {
            EquipProfile->SetArrayField(TEXT("weapon_items"),{MakeShared<FJsonValueNumber>(Selected)});
            FACEPluginVM PetPool;TestTrue(TEXT("Meta pet-pool policy loads"),PetPool.Load(Script,Error));
            TestTrue(TEXT("Meta equipment expression executes"),PetPool.Step(S,EquipProfile,Intent,Error));
            if(!TestTrue(TEXT("Meta returns an intent"),Intent.IsValid()))continue;
            if(Selected==20)
            {
                TestEqual(TEXT("Essence-only selection permits a meta to equip a wand"),Intent->GetStringField(TEXT("action")),FString(TEXT("equip")));
                TestEqual(TEXT("Meta equips the wand, not the essence"),Intent->GetNumberField(TEXT("item")),8.);
            }
            else TestFalse(TEXT("Missing weapon selection stays restrictive for metas"),Intent->HasField(TEXT("action")));
        }
    }
    // Real typed meta table -> compiler -> sandbox. Calls preserve ordered actions,
    // timers fire once per state entry, expressions use the VT bracket syntax.
    auto Doc=MetaDocument({
        RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"setvar[count, 2+3*4]"})"))),TEXT("Default")),
        RuleMeta(26,5,PropsMeta(JSONMeta(TEXT(R"({"e":"getvar[count]==14"})"))),PropsMeta(JSONMeta(TEXT(R"({"st":"Worker","ret":"Done"})"))),TEXT("Default")),
        RuleMeta(6,6,NumMeta(2),NumMeta(0),TEXT("Worker")),
        RuleMeta(1,2,NumMeta(0),StrMeta(TEXT("/vt opt set EnableNav true")),TEXT("Done"))});
    auto P=Base(ACEVTProfile::Convert(Doc,Issues));TestEqual(TEXT("Executable meta compiles"),Issues.Num(),0);FACEPluginVM VM;TestTrue(TEXT("Runtime loads"),VM.Load(Script,Error));
    TestTrue(TEXT("Meta executes"),VM.Step(Snap,P,Intent,Error));if(!Intent){AddError(Error);return false;}
    TestEqual(TEXT("Expression and call enter Worker"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Worker")));
    Snap->SetNumberField(TEXT("time"),101);VM.Step(Snap,P,Intent,Error);TestEqual(TEXT("Timer waits"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Worker")));
    Snap->SetNumberField(TEXT("time"),102);VM.Step(Snap,P,Intent,Error);TestEqual(TEXT("Return resumes supplied state"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Done")));
    VM.Step(Snap,P,Intent,Error);TestEqual(TEXT("Empty route from a meta shows its state instead of recording advice"),Intent->GetStringField(TEXT("status")),FString(TEXT("Meta Done: waiting for a route or trigger")));
    TestFalse(TEXT("Transient option does not mutate saved profile"),P->HasField(TEXT("navigation")));
    // Unsupported operations must invalidate the whole import, even in unused states.
    ACEVTProfile::Convert(MetaDocument({RuleMeta(1,2,NumMeta(0),StrMeta(TEXT("/mf login OtherCharacter")),TEXT("Unused"))}),Issues);
    TestTrue(TEXT("External login plugin rejected"),Issues.Num()>0);
    ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"setvar[x,broken[]]"})"))),TEXT("Default"))}),Issues);
    TestTrue(TEXT("Unknown expression function rejected"),Issues.Num()>0);
    // Incoming chat is still evaluated while the game action gate is busy.
    auto Chat=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(4,1,StrMeta(TEXT("^Quest complete$")),StrMeta(TEXT("Done")),TEXT("Default"))}),Issues));
    FACEPluginVM ChatVM;ChatVM.Load(Script,Error);ChatVM.Step(Snap,Chat,Intent,Error);Snap->SetBoolField(TEXT("busy"),true);
    Snap->SetArrayField(TEXT("chat_events"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"serial":1,"text":"Quest complete"})")))});
    ChatVM.Step(Snap,Chat,Intent,Error);TestEqual(TEXT("Chat transitions while action pending"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Done")));Snap->SetBoolField(TEXT("busy"),false);
    // Pause and recall waypoints are actions at the current position, not walks
    // to the old recording location. A final portal/recall completes on arrival.
    auto Nav=ACEVTProfile::Read(TEXT("uTank2 NAV 1.2\n4\n2\n3\n1\n1\n0\n0\n1500\n1\n0\n0\n0.05\n0\n1234\n"),TEXT("nav"),Error);
    auto Route=Base(ACEVTProfile::Convert(Nav,Issues));Route->SetBoolField(TEXT("navigation"),true);FACEPluginVM RouteVM;RouteVM.Load(Script,Error);
    RouteVM.Step(Snap,Route,Intent,Error);TestEqual(TEXT("Pause does not walk to recording location"),Intent->GetStringField(TEXT("status")),FString(TEXT("Pausing on route")));
    Snap->SetNumberField(TEXT("time"),104);RouteVM.Step(Snap,Route,Intent,Error);
    Snap->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":1234,"name":"Portal","distance":0})")))});
    RouteVM.Step(Snap,Route,Intent,Error);TestEqual(TEXT("Terminal portal can be used"),Intent->GetStringField(TEXT("action")),FString(TEXT("use_world")));
    Snap->SetNumberField(TEXT("teleport_sequence"),1);Snap->SetBoolField(TEXT("portal_space"),true);RouteVM.Step(Snap,Route,Intent,Error);TestEqual(TEXT("Portal waits for world ready"),Intent->GetNumberField(TEXT("route_point")),2.);
    Snap->SetBoolField(TEXT("portal_space"),false);RouteVM.Step(Snap,Route,Intent,Error);RouteVM.Step(Snap,Route,Intent,Error);TestEqual(TEXT("Once route stops at end"),Intent->GetStringField(TEXT("status")),FString(TEXT("Route complete")));
    // Circular wraps; linear retraces without repeating the end point.
    for(int Mode=1;Mode<=2;++Mode)
    {
        auto RP=Base(JSONMeta(TEXT(R"({"navigation":true,"route":[{"cell":2139029505,"x":84,"y":84,"z":12},{"cell":2139029505,"x":86,"y":84,"z":12},{"cell":2139029505,"x":88,"y":84,"z":12}]})")));
        RP->SetBoolField(TEXT("loop_route"),Mode==1);RP->SetBoolField(TEXT("reverse_route"),Mode==2);FACEPluginVM M;M.Load(Script,Error);
        for(int X=84;X<=88;X+=2){Snap->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),X);M.Step(Snap,RP,Intent,Error);}
        M.Step(Snap,RP,Intent,Error);TestEqual(TEXT("Route end target"),Intent->GetNumberField(TEXT("x")),Mode==1?84.:86.);
    }
    auto Jump=ACEVTProfile::Read(TEXT("uTank2 NAV 1.2\n4\n1\n9\n0\n0\n0\n0\n90\nTrue\n750.00004\n"),TEXT("nav"),Error);
    auto JP=ACEVTProfile::Convert(Jump,Issues);TestEqual(TEXT("Jump supported"),Issues.Num(),0);auto Pt=JP->GetArrayField(TEXT("route"))[0]->AsObject();TestEqual(TEXT("Jump strafe suffix"),Pt->GetNumberField(TEXT("strafe")),-1.);TestEqual(TEXT("Jump charge"),Pt->GetNumberField(TEXT("charge")),.75);TestEqual(TEXT("Heading to Unreal yaw"),Pt->GetNumberField(TEXT("heading")),180.);
    auto Blob=MakeShared<FJsonObject>();Blob->SetStringField(TEXT("blob"),TEXT("Test.nav\n1\nuTank2 NAV 1.2\n4\n1\n0\n0\n0\n0.05\n0\n"));
    auto Embedded=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,4,NumMeta(0),MakeShared<FJsonValueObject>(Blob),TEXT("Default")),RuleMeta(7,1,NumMeta(0),StrMeta(TEXT("Done")),TEXT("Default"))}),Issues));
    FACEPluginVM EVM;EVM.Load(Script,Error);TestTrue(TEXT("Embedded route executes"),EVM.Step(Snap,Embedded,Intent,Error));
    TestEqual(TEXT("Nav-empty observes newly loaded route"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Default")));
    const A* Overlay=nullptr;TestTrue(TEXT("Embedded route reaches native overlay output"),Intent->TryGetArrayField(TEXT("runtime_route"),Overlay)&&Overlay->Num()==1);
    auto Monsters=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(16,1,PropsMeta(JSONMeta(TEXT(R"({"r":5})"))),StrMeta(TEXT("Clear")),TEXT("Default"))}),Issues));
    Snap->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":5,"name":"Rat","distance":6})")))});
    FACEPluginVM MVM;MVM.Load(Script,Error);MVM.Step(Snap,Monsters,Intent,Error);TestEqual(TEXT("Monster ranges are meters, not map units"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Clear")));
    Snap->SetArrayField(TEXT("targets"),{});
    auto Uses=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,2,NumMeta(0),StrMeta(TEXT("/mt use Key on Door")),TEXT("Default")),RuleMeta(1,2,NumMeta(0),StrMeta(TEXT("/mt use Door")),TEXT("Default"))}),Issues));
    TestEqual(TEXT("Named use and apply compile"),Issues.Num(),0);
    Snap->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":9,"name":"Key","count":1})")))});
    Snap->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(JSONMeta(TEXT(R"({"id":10,"name":"Door","distance":1})")))});
    FACEPluginVM UVM;UVM.Load(Script,Error);UVM.Step(Snap,Uses,Intent,Error);TestEqual(TEXT("Key applies to chosen door"),Intent->GetStringField(TEXT("action")),FString(TEXT("apply_item")));TestEqual(TEXT("Correct target"),Intent->GetNumberField(TEXT("target")),10.);
    UVM.Step(Snap,Uses,Intent,Error);TestFalse(TEXT("Next use waits for server response"),Intent->HasField(TEXT("action")));
    auto Receipt=JSONMeta(TEXT(R"({"type":"use","source":9,"target":10,"action_serial":1,"error":0})"));
    Receipt->SetNumberField(TEXT("requested_at"),Snap->GetNumberField(TEXT("time")));Snap->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});
    Snap->SetNumberField(TEXT("action_serial"),1);UVM.Step(Snap,Uses,Intent,Error);TestEqual(TEXT("Next use follows completion"),Intent->GetStringField(TEXT("action")),FString(TEXT("use_world")));
    Receipt->SetNumberField(TEXT("source"),10);Receipt->SetNumberField(TEXT("action_serial"),2);Receipt->SetNumberField(TEXT("error"),1);
    Snap->SetNumberField(TEXT("action_serial"),2);Snap->SetNumberField(TEXT("action_error"),1);UVM.Step(Snap,Uses,Intent,Error);TestEqual(TEXT("Failed interaction pauses meta"),Intent->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    auto Edge=ACEVTProfile::Read(TEXT("uTank2 NAV 1.2\n4\n1\n0\n-101.950012143709\n18.0138335307439\n-0.474979146321615\n0\n"),TEXT("nav"),Error);
    auto EP=ACEVTProfile::Convert(Edge,Issues);TestEqual(TEXT("Dungeon point just beyond block zero imports"),Issues.Num(),0);TestTrue(TEXT("Negative local position is preserved"),EP->GetArrayField(TEXT("route"))[0]->AsObject()->GetNumberField(TEXT("x"))<0);
    {
        auto QuerySnap=MakeShared<FJsonObject>(*Snap);QuerySnap->SetObjectField(TEXT("skills"),JSONMeta(TEXT(R"({"21":{"base":100,"current":150,"training":3}})")));
        QuerySnap->SetObjectField(TEXT("char_ints"),JSONMeta(TEXT(R"({"25":275})")));QuerySnap->SetNumberField(TEXT("base_health"),80);
        auto Queries=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"touchvar[test]==0 && testvar[test] && touchvar[test]==1 && getcharintprop[25]==275 && getcharintprop[999]==0 && getcharskill_base[21]==100 && getcharskill_buffed[21]==150 && getcharskill_traininglevel[21]==3 && getcharvital_current[1]==100 && getcharvital_base[1]==80 && getcharvital_buffedmax[1]==100 && getobjectinternaltype[wobjectgetplayer[]]==7 && wobjectgetobjectclass[wobjectgetplayer[]]==24 && getplayerlandcell[]==2139029505"})"))),StrMeta(TEXT("Done")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Character expression adapters compile"),Issues.Num(),0);FACEPluginVM CharacterVM;CharacterVM.Load(Script,Error);TestTrue(TEXT("Character queries run"),CharacterVM.Step(QuerySnap,Queries,Intent,Error));if(Intent)TestEqual(TEXT("Character queries match snapshot"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Done")));
        auto Coordinates=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(26,1,PropsMeta(JSONMeta(TEXT(R"({"e":"coordinategetns[coordinateparse[`12.5S, 34.25E`]]==-12.5 && coordinategetwe[coordinateparse[`12.5S, 34.25E`]]==34.25 && coordinategetz[getplayercoordinates[]]==0.05 && coordinatedistanceflat[coordinateparse[`0N, 0E`],coordinateparse[`0N, 1E`]]==240 && coordinatedistancewithz[coordinateparse[`0N, 0E`],coordinateparse[`0N, 1E`]]==240 && coordinateparse[invalid]==0"})"))),StrMeta(TEXT("Done")),TEXT("Default"))}),Issues));
        TestEqual(TEXT("Coordinate expressions compile"),Issues.Num(),0);FACEPluginVM Geo;Geo.Load(Script,Error);TestTrue(TEXT("Coordinate queries run"),Geo.Step(QuerySnap,Coordinates,Intent,Error));if(Intent)TestEqual(TEXT("Distance expressions use meters and coordinate axes use map units"),Intent->GetStringField(TEXT("meta_state")),FString(TEXT("Done")));
        auto Select=Base(ACEVTProfile::Convert(MetaDocument({RuleMeta(1,7,NumMeta(0),PropsMeta(JSONMeta(TEXT(R"({"e":"actiontryselect[wobjectgetplayer[]]"})"))),TEXT("Default"))}),Issues));
        FACEPluginVM Self;Self.Load(Script,Error);TestTrue(TEXT("Self selection runs"),Self.Step(QuerySnap,Select,Intent,Error));if(Intent){TestEqual(TEXT("Self selection reaches host intent"),Intent->GetStringField(TEXT("action")),FString(TEXT("select")));TestEqual(TEXT("Self GUID retained"),Intent->GetNumberField(TEXT("target")),1.);}
    }
    {
        TArray<FString> CommandIssues;
        auto Tap=ACEVTProfile::CompileCommand(TEXT("/vt tapjump"),CommandIssues);TestEqual(TEXT("Tap jump has 100ms charge"),Tap->GetNumberField(TEXT("charge")),.1);
        auto Mark=ACEVTProfile::CompileCommand(TEXT("/vt setattackbar 0.75"),CommandIssues);TestEqual(TEXT("Attack mark command converts"),CommandIssues.Num(),0);
        FACEPluginVM MarkVM;MarkVM.Load(Script,Error);auto CommandProfile=Base(MakeShared<FJsonObject>());CommandProfile->SetArrayField(TEXT("ucm_commands"),{MakeShared<FJsonValueObject>(Mark)});TestTrue(*Error,MarkVM.Step(Snap,CommandProfile,Intent,Error));
        TestEqual(TEXT("Attack mark dispatches to native UI"),Intent->GetStringField(TEXT("action")),FString(TEXT("attack_bar")));TestEqual(TEXT("Attack mark preserves fraction"),Intent->GetNumberField(TEXT("power")),.75);
        ACEVTProfile::CompileCommand(TEXT("/vt setattackbar 1.1"),CommandIssues);TestTrue(TEXT("Out-of-range mark rejected"),CommandIssues.Num()>0);
        CommandIssues.Reset();auto Reverse=ACEVTProfile::CompileCommand(TEXT("/vt reverseroute"),CommandIssues);
        Snap->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),84);
        FACEPluginVM ReverseVM;ReverseVM.Load(Script,Error);CommandProfile=Base(JSONMeta(TEXT(R"({"navigation":true,"loop_route":true,"route":[{"cell":2139029505,"x":84,"y":84,"z":12},{"cell":2139029505,"x":85,"y":84,"z":12},{"cell":2139029505,"x":90,"y":84,"z":12}]})")));
        CommandProfile->SetArrayField(TEXT("ucm_commands"),{MakeShared<FJsonValueObject>(Reverse)});ReverseVM.Step(Snap,CommandProfile,Intent,Error);CommandProfile->RemoveField(TEXT("ucm_commands"));ReverseVM.Step(Snap,CommandProfile,Intent,Error);
        TestTrue(*Error,ReverseVM.Step(Snap,CommandProfile,Intent,Error));
        TestEqual(TEXT("Reversed circular route wraps from first to final point"),Intent->GetNumberField(TEXT("x")),90.);
    }
    return true;
}
#endif
