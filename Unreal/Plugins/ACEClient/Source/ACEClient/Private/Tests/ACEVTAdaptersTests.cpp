#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEVTProfile.h"
#include "Mods/ACEVTRegex.h"
#include "Mods/ACEVTObjectClass.h"
#include "Mods/ACEPluginVM.h"
namespace
{
TSharedPtr<FJsonObject> AdapterJSON(const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
FString CommandMetaFile(const FString& Command)
{return TEXT("1\nCondAct\n5\nCType\nAType\nCData\nAData\nState\nn\nn\nn\nn\nn\n1\ni\n1\ni\n2\ni\n0\ns\n")+Command+TEXT("\ns\nDefault\n");}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVTAdaptersTest,"ACE.Plugins.LegacyAdapters",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEVTAdaptersTest::RunTest(const FString&)
{
    FString Error,Script;TArray<FString> Issues;TSharedPtr<FJsonObject> Intent;
    FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    {
        TArray<FString> Problems;auto Command=ACEVTProfile::CompileCommand(TEXT("/vt opt set CombineSalvage true"),Problems);
        TestEqual(TEXT("VT's actual CombineSalvage option has an adapter"),Problems.Num(),0);
        TestEqual(TEXT("CombineSalvage controls bag combining"),Command->GetStringField(TEXT("key")),FString(TEXT("salvage_combine")));
        TestTrue(TEXT("CombineSalvage retains the requested state"),Command->GetBoolField(TEXT("value")));
    }
    {
        auto S=AdapterJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"inventory":[],"char_strings":{"1":"Alice"},"fellowship":{"members":[{"id":2,"name":"Bob","share_loot":true}]},"corpses":[{"id":10,"name":"Corpse","distance":2,"identified":true,"ownership_available":true,"observed_age":10,"killer":"Alice","rare":false}]})"));
        auto P=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[{"action":"keep"}]})"));
        auto Corpse=S->GetArrayField(TEXT("corpses"))[0]->AsObject();
        auto Opens=[&](){FACEPluginVM V;V.Load(Script,Error);TestTrue(TEXT("Corpse policy executes"),V.Step(S,P,Intent,Error));FString Action;return Intent&&Intent->TryGetStringField(TEXT("action"),Action)&&Action==TEXT("open_corpse");};
        TestTrue(TEXT("Own corpse opens"),Opens());
        S->GetObjectField(TEXT("char_strings"))->SetStringField(TEXT("1"),TEXT("+Alice"));
        Corpse->SetStringField(TEXT("killer"),TEXT("alice"));TestTrue(TEXT("Administrator prefix and name case do not exclude own kills"),Opens());
        Corpse->SetStringField(TEXT("killer"),TEXT("Alicia"));TestFalse(TEXT("Similar names do not grant ownership"),Opens());
        Corpse->SetStringField(TEXT("killer"),TEXT("Bob"));TestFalse(TEXT("Fellow loot requires enabled setting"),Opens());
        P->SetBoolField(TEXT("loot_fellow_corpses"),true);TestTrue(TEXT("Sharing fellow corpse opens immediately"),Opens());
        S->GetObjectField(TEXT("fellowship"))->GetArrayField(TEXT("members"))[0]->AsObject()->SetStringField(TEXT("name"),TEXT("+BOB"));
        TestTrue(TEXT("Sharing administrator fellow uses normalized ownership"),Opens());
        S->GetObjectField(TEXT("fellowship"))->GetArrayField(TEXT("members"))[0]->AsObject()->SetBoolField(TEXT("share_loot"),false);TestFalse(TEXT("Nonsharing fellow remains reserved"),Opens());
        Corpse->SetNumberField(TEXT("observed_age"),100);TestTrue(TEXT("Fellow reservation expires at 100 seconds"),Opens());
        Corpse->SetStringField(TEXT("killer"),TEXT("Stranger"));TestFalse(TEXT("Other corpses require all-corpses option"),Opens());P->SetBoolField(TEXT("loot_all_corpses"),true);TestTrue(TEXT("Unreserved stranger corpse opens"),Opens());
        Corpse->SetNumberField(TEXT("observed_age"),99);TestFalse(TEXT("All-corpses does not ignore reservation"),Opens());
        Corpse->SetNumberField(TEXT("observed_age"),200);Corpse->SetBoolField(TEXT("rare"),true);TestFalse(TEXT("Other player's rare stays excluded"),Opens());
    }
    for(const FString Args:{TEXT("start"),TEXT("stop"),TEXT("jump 90 true 600 strafeleft"),TEXT("tapjump"),TEXT("forcebuff"),TEXT("cancelforcebuff"),TEXT("echo Some message"),TEXT("setmetastate Test state"),TEXT("mexec 1+2"),TEXT("opt set AttackDistance 0.2"),TEXT("enablecombat true"),TEXT("enablebuffing false"),TEXT("enablenav true"),TEXT("enablelooting false"),TEXT("nav load Walk home"),TEXT("meta load Example"),TEXT("loot load Rings"),TEXT("looting load Rings"),TEXT("settings load Mage")})
    {
        TArray<FString> OldIssues,NewIssues;auto Old=ACEVTProfile::CompileCommand(TEXT("/vt ")+Args,OldIssues);auto New=ACEVTProfile::CompileCommand(TEXT("/acueucm ")+Args,NewIssues);
        TestEqual(*(Args+TEXT(" legacy accepted")),OldIssues.Num(),0);TestEqual(*(Args+TEXT(" native accepted")),NewIssues.Num(),0);
        FString OldJson,NewJson;FJsonSerializer::Serialize(Old.ToSharedRef(),TJsonWriterFactory<>::Create(&OldJson));FJsonSerializer::Serialize(New.ToSharedRef(),TJsonWriterFactory<>::Create(&NewJson));
        TestEqual(*(Args+TEXT(" identical native instruction")),OldJson,NewJson);
        TestTrue(TEXT("Converted command uses client namespace"),Old->GetStringField(TEXT("source_command")).StartsWith(TEXT("/acueucm ")));
        TArray<FString> AliasIssues;auto Alias=ACEVTProfile::CompileCommand(TEXT("/ucm ")+Args,AliasIssues);FString AliasJson;
        FJsonSerializer::Serialize(Alias.ToSharedRef(),TJsonWriterFactory<>::Create(&AliasJson));
        TestEqual(*(Args+TEXT(" old UCM alias accepted")),AliasIssues.Num(),0);
        TestEqual(*(Args+TEXT(" old UCM alias keeps behavior")),AliasJson,NewJson);
    }
    TestEqual(TEXT("Prefix lookalike is not rewritten"),ACEVTProfile::NativeCommand(TEXT("/vtank jump")),FString(TEXT("/vtank jump")));
    TestEqual(TEXT("Case-insensitive command conversion"),ACEVTProfile::NativeCommand(TEXT(" /VT jump 90 false 1000 ")),FString(TEXT("/acueucm jump 90 false 1000")));
    TestEqual(TEXT("Native prefix lookalike is not rewritten"),ACEVTProfile::NativeCommand(TEXT("/acueucmfoo jump")),FString(TEXT("/acueucmfoo jump")));
    ACEVTRegex::FCaptures Captures;
    TestTrue(TEXT("Empty regex is a valid match-all filter"),ACEVTRegex::Match(TEXT(""),TEXT("A spell"),Captures,Error));
    TestTrue(TEXT("Named chat capture with escaped punctuation"),ACEVTRegex::Match(TEXT("^\\[Fellowship\\] (?<speaker>.+) says, \"!start\"$"),TEXT("[Fellowship] Test Player says, \"!start\""),Captures,Error));
    TestEqual(TEXT("Captured full sender"),Captures.FindRef(TEXT("speaker")),FString(TEXT("Test Player")));
    TestTrue(TEXT("Mixed named and unnamed captures"),ACEVTRegex::Match(TEXT("(?<name>A)(B)"),TEXT("AB"),Captures,Error));
    TestEqual(TEXT("Dotnet unnamed group numbered first"),Captures.FindRef(TEXT("1")),FString(TEXT("B")));
    TestEqual(TEXT("Dotnet named group numbered after unnamed"),Captures.FindRef(TEXT("2")),FString(TEXT("A")));
    TestTrue(TEXT("Dotnet apostrophe named captures translate"),ACEVTRegex::Match(TEXT("(?'speaker'A)(B)"),TEXT("AB"),Captures,Error));
    TestEqual(TEXT("Translated named capture preserves name"),Captures.FindRef(TEXT("speaker")),FString(TEXT("A")));
    TestTrue(TEXT("Fixed positive lookbehind"),ACEVTRegex::Match(TEXT("(?<=Prefix )(word)\\1"),TEXT("Prefix wordword"),Captures,Error));
    TestEqual(TEXT("Lookbehind does not add a capture"),Captures.FindRef(TEXT("1")),FString(TEXT("word")));
    TestTrue(TEXT("Negative lookbehind"),ACEVTRegex::Test(TEXT("(?<!No )Loot"),TEXT("Yes Loot"),Error));
    TestFalse(TEXT("Negative lookbehind rejects prohibited prefix"),ACEVTRegex::Test(TEXT("(?<!No )Loot"),TEXT("No Loot"),Error));
    TestTrue(TEXT("Comments cannot change named capture numbering"),ACEVTRegex::Match(TEXT("(?# ignored (?<fake)(?'word'A)(B)\\k'word'"),TEXT("ABA"),Captures,Error));
    TestEqual(TEXT("Named backreference retains .NET output numbering"),Captures.FindRef(TEXT("1")),FString(TEXT("B")));
    TestEqual(TEXT("Comment excluded from named captures"),Captures.FindRef(TEXT("word")),FString(TEXT("A")));
    TestFalse(TEXT("Unterminated comment rejected"),ACEVTRegex::Validate(TEXT("(?# broken"),Error));
    TestFalse(TEXT("Unbounded lookbehind remains explicit error"),ACEVTRegex::Validate(TEXT("(?<=a*)b"),Error));
    for(int I=0;I<600;++I)
    {
        const FString Pattern=FString::Printf(TEXT("^(?<value>Item%d)$"),I);
        TestTrue(TEXT("Bounded predicate cache matches changing text"),ACEVTRegex::Test(Pattern,FString::Printf(TEXT("Item%d"),I),Error));
    }
    TestTrue(TEXT("Evicted pattern recompiles with capture mapping intact"),ACEVTRegex::Match(TEXT("(?'speaker'A)(B)"),TEXT("AB"),Captures,Error));
    TestEqual(TEXT("Cached captures retain .NET numbering"),Captures.FindRef(TEXT("1")),FString(TEXT("B")));
    for(int I=0;I<2;++I)
    {
        TestFalse(TEXT("Predicate negative results stay negative"),ACEVTRegex::Test(TEXT("^Fire$"),TEXT("Cold"),Error));
        TestTrue(TEXT("No-match is not an error"),Error.IsEmpty());
        TestTrue(TEXT("Changing text cannot reuse another input's result"),ACEVTRegex::Test(TEXT("^Fire$"),TEXT("Fire"),Error));
        TestFalse(TEXT("Invalid predicates never become cached no-match results"),ACEVTRegex::Test(TEXT("["),TEXT("Fire"),Error));
        TestFalse(TEXT("Invalid pattern keeps its diagnostic"),Error.IsEmpty());
    }
    TestTrue(TEXT("Extended-mode comments excluded from parsing"),ACEVTRegex::Validate(TEXT("(?x)a # comment ("),Error));
    TestTrue(TEXT("Extended mode preserves escaped and class whitespace"),ACEVTRegex::Match(TEXT("(?x) ^ (?<word>A) # fake (?<ignore>\n (B) [ #] \\# $"),TEXT("AB #"),Captures,Error));
    TestEqual(TEXT("Extended mode keeps unnamed-first numbering"),Captures.FindRef(TEXT("1")),FString(TEXT("B")));
    TestEqual(TEXT("Extended mode keeps named capture"),Captures.FindRef(TEXT("word")),FString(TEXT("A")));
    TestTrue(TEXT("Scoped extended mode restores outer whitespace rules"),ACEVTRegex::Test(TEXT("^(?x:A B)(?-x: C )D$"),TEXT("AB C D"),Error));
    TestFalse(TEXT("Disabled extended mode whitespace is mandatory"),ACEVTRegex::Test(TEXT("^(?x:A B)(?-x: C )D$"),TEXT("ABCD"),Error));
    TestFalse(TEXT("Extended mode cannot repair invalid group names"),ACEVTRegex::Validate(TEXT("(?x)(?<na me>A)"),Error));
    TestFalse(TEXT("Extended mode cannot repair invalid group prefixes"),ACEVTRegex::Validate(TEXT("(?x)(? <name>A)"),Error));
    TestFalse(TEXT("Spaced braces are not converted into a quantifier"),ACEVTRegex::Test(TEXT("(?x)^a{1, 2}$"),TEXT("aa"),Error));
    TestTrue(TEXT("Spaced braces retain dotnet literal semantics"),ACEVTRegex::Test(TEXT("(?x)^a{1, 2}$"),TEXT("a{1,2}"),Error));
    TestTrue(TEXT("Unicode property escapes retain their braces"),ACEVTRegex::Test(TEXT("(?x)^\\p{Lu}\\P{Lu}$"),TEXT("Aa"),Error));
    TestTrue(TEXT("Dotnet four digit hex escape stays one character"),ACEVTRegex::Test(TEXT("^\\x1234\\u0041$"),FString::Chr(TCHAR(0x1234))+TEXT("A"),Error));
    TestFalse(TEXT("Extended whitespace cannot repair a malformed escape"),ACEVTRegex::Validate(TEXT("(?x)\\u00 41"),Error));
    TestTrue(TEXT("Mixed numeric and named backreferences remap"),ACEVTRegex::Match(TEXT("^(?<name>A)(B)\\1\\2$"),TEXT("ABBA"),Captures,Error));
    TestFalse(TEXT("ICU source-order references cannot leak through"),ACEVTRegex::Test(TEXT("^(?<name>A)(B)\\1\\2$"),TEXT("ABAB"),Error));
    TestTrue(TEXT("Delimited numeric reference remains separate from next digit"),ACEVTRegex::Test(TEXT("^(?<name>A)(B)\\k<1>2$"),TEXT("ABB2"),Error));
    TestTrue(TEXT("Explicit capture mode excludes ordinary groups"),ACEVTRegex::Match(TEXT("(?n)^(A)(?<word>B)(?-n:(C))\\1\\2$"),TEXT("ABCCB"),Captures,Error));
    TestEqual(TEXT("Explicit capture mode result numbering"),Captures.FindRef(TEXT("1")),FString(TEXT("C")));
    TestTrue(TEXT("Missing multidigit reference uses dotnet octal fallback"),ACEVTRegex::Test(TEXT("^(A)\\10$"),FString(TEXT("A"))+TCHAR(8),Error));
    TestFalse(TEXT("Undefined single-digit reference is rejected"),ACEVTRegex::Validate(TEXT("(?<name>A)\\2"),Error));
    TestTrue(TEXT("Balancing groups import with capture history"),ACEVTRegex::Validate(TEXT("(?<open>a)(?<-open>b)"),Error));
    TestTrue(TEXT("Conditional groups import with capture participation"),ACEVTRegex::Validate(TEXT("(a)?(?(1)b|c)"),Error));
    TestFalse(TEXT("Capture limit enforced on import"),ACEVTRegex::Validate(FString::ChrN(33,'(')+FString::ChrN(33,')'),Error));
    const double Start=FPlatformTime::Seconds();
    TestFalse(TEXT("Pathological regex cannot hang the game thread"),ACEVTRegex::Match(TEXT("(a+)+$"),FString::ChrN(4000,'a')+TEXT("!"),Captures,Error));
    TestTrue(TEXT("Match failure explains work limit"),!Error.IsEmpty());TestTrue(TEXT("Regex terminates within bounded wall time"),FPlatformTime::Seconds()-Start<.5);
    FACEWorldObject Object;Object.ItemType=ACEItemType::Creature;Object.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
    TestEqual(TEXT("Monster Decal class is five, not mana-stone class"),ACEVTObjectClass::Classify(Object),5);
    Object.ObjectDescriptionFlags=0;TestEqual(TEXT("Non-attackable creature is NPC"),ACEVTObjectClass::Classify(Object),37);
    Object.ItemType=0;Object.ObjectDescriptionFlags=ACEObjectDescFlag::Corpse;TestEqual(TEXT("Corpse class"),ACEVTObjectClass::Classify(Object),27);
    auto S=AdapterJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":0,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"inventory":[{"id":9,"name":"Quest Token","count":2}],"route_objects":[{"id":10,"name":"Collector","object_class":37,"distance":1}],"action_serial":0,"teleport_sequence":0})"));
    auto Compile=[&](const FString& Command){auto D=ACEVTProfile::Read(CommandMetaFile(Command),TEXT("met"),Error);auto P=ACEVTProfile::Convert(D,Issues);P->SetBoolField(TEXT("buffing"),false);P->SetBoolField(TEXT("recovery"),false);P->SetStringField(TEXT("combat"),TEXT("off"));return P;};
    auto Give=Compile(TEXT("/mt give Quest Token to Collector"));TestEqual(TEXT("Give compiles"),Issues.Num(),0);FACEPluginVM VM;VM.Load(Script,Error);
    {
        // Manual commands run through the same executor without starting the
        // saved hunt, and their queue is consumed exactly once by the host.
        auto Chat=AdapterJSON(TEXT(R"({"combat":"auto","buffing":true,"navigation":true,"ucm_command_only":true})"));
        Chat->SetArrayField(TEXT("ucm_commands"),{MakeShared<FJsonValueObject>(ACEVTProfile::CompileCommand(TEXT("/ucm jump 90 true 600 strafeleft"),Issues))});
        FACEPluginVM Manual;Manual.Load(Script,Error);TestTrue(TEXT("Manual UCM jump executes"),Manual.Step(S,Chat,Intent,Error));
        if(Intent){TestEqual(TEXT("Manual jump action"),Intent->GetStringField(TEXT("action")),FString(TEXT("jump")));TestEqual(TEXT("Manual heading matches imported jump"),Intent->GetNumberField(TEXT("heading")),180.);}
        Chat->RemoveField(TEXT("ucm_commands"));S->SetBoolField(TEXT("jumping"),true);
        Manual.Step(S,Chat,Intent,Error);TestFalse(TEXT("One-shot does not cancel active jump"),Intent->HasField(TEXT("action")));
        S->SetBoolField(TEXT("jumping"),false);Manual.Step(S,Chat,Intent,Error);
        TestEqual(TEXT("One-shot stops without starting saved hunt"),Intent->GetStringField(TEXT("action")),FString(TEXT("stop")));
    }
    {
        auto Jump=Compile(TEXT("/vt jump 90 true 600 strafeleft"));TestEqual(TEXT("VT heading jump compiles"),Issues.Num(),0);
        FACEPluginVM J;J.Load(Script,Error);TSharedPtr<FJsonObject> I;TestTrue(TEXT("VT jump executes"),J.Step(S,Jump,I,Error));
        if(I){TestEqual(TEXT("Jump retains explicit converted heading"),I->GetNumberField(TEXT("heading")),180.);TestFalse(TEXT("Explicit heading does not become current heading"),I->GetBoolField(TEXT("current_heading")));TestEqual(TEXT("Jump milliseconds become charge fraction"),I->GetNumberField(TEXT("charge")),.6);TestEqual(TEXT("Left strafe retained"),I->GetNumberField(TEXT("strafe")),-1.);TestTrue(TEXT("Shift jump retained"),I->GetBoolField(TEXT("walk_jump")));}
        auto Current=Compile(TEXT("/vt jump 700 false 1000"));TestEqual(TEXT("Common current-heading jump compiles"),Issues.Num(),0);
        FACEPluginVM K;K.Load(Script,Error);K.Step(S,Current,I,Error);if(I)TestTrue(TEXT("700 sentinel uses current heading"),I->GetBoolField(TEXT("current_heading")));
    }
    TestTrue(TEXT("Give executes"),VM.Step(S,Give,Intent,Error));if(!Intent){AddError(Error);return false;}
    TestEqual(TEXT("Normal give intent"),Intent->GetStringField(TEXT("action")),FString(TEXT("give")));TestEqual(TEXT("Give one item"),Intent->GetNumberField(TEXT("amount")),1.);
    VM.Step(S,Give,Intent,Error);TestFalse(TEXT("Give not repeated while waiting"),Intent->HasField(TEXT("action")));
    S->SetNumberField(TEXT("inventory_failure_serial"),1);S->SetNumberField(TEXT("inventory_failure_item"),9);S->SetNumberField(TEXT("inventory_failure_error"),3);VM.Step(S,Give,Intent,Error);TestEqual(TEXT("Rejected give pauses the meta"),Intent->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    S->SetNumberField(TEXT("action_error"),0);
    auto Use=Compile(TEXT("/vt mexec actiontryuseitem[wobjectfindnearestbyobjectclass[37]]"));TestEqual(TEXT("World expression compiles"),Issues.Num(),0);
    FACEPluginVM U;U.Load(Script,Error);U.Step(S,Use,Intent,Error);TestEqual(TEXT("Expression resolves NPC and uses it"),Intent->GetNumberField(TEXT("item")),10.);
    auto Nav=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","navigation":true,"open_doors":true,"route":[{"cell":2139029505,"x":90,"y":84,"z":12}]})"));
    S->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(AdapterJSON(TEXT(R"({"id":21,"name":"Door","object_class":26,"cell":2139029505,"x":85,"y":84,"z":12,"distance":1,"door_open":false})")))});
    FACEPluginVM N;N.Load(Script,Error);N.Step(S,Nav,Intent,Error);TestEqual(TEXT("Closed door on route is used before walking"),Intent->GetStringField(TEXT("action")),FString(TEXT("use_world")));
    S->GetArrayField(TEXT("route_objects"))[0]->AsObject()->SetBoolField(TEXT("door_open"),true);S->SetNumberField(TEXT("time"),102);N.Step(S,Nav,Intent,Error);TestEqual(TEXT("Walk resumes after door opens"),Intent->GetStringField(TEXT("action")),FString(TEXT("move")));
    // A and B can refer to each other without recursive JSON or unbounded import.
    const FString Folder=FPaths::ProjectSavedDir()/TEXT("Tests/VTDependencies")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Folder,true);
    FFileHelper::SaveStringToFile(CommandMetaFile(TEXT("/vt meta load B")),*(Folder/TEXT("A.met")));
    FFileHelper::SaveStringToFile(CommandMetaFile(TEXT("/vt meta load A")),*(Folder/TEXT("B.met")));
    auto Graph=ACEVTProfile::ConvertFile(Folder/TEXT("A.met"),Issues);TestEqual(TEXT("Cyclic dependencies compile once"),Issues.Num(),0);
    TestEqual(TEXT("Two named profiles archived"),Graph->GetObjectField(TEXT("vt_library"))->Values.Num(),2);
    TestEqual(TEXT("Linked profile command migrated"),Graph->GetObjectField(TEXT("vt_library"))->GetObjectField(TEXT("b.met"))->GetArrayField(TEXT("vt_meta"))[0]->AsObject()->GetObjectField(TEXT("action"))->GetStringField(TEXT("value")),FString(TEXT("/acueucm meta load A")));
    FFileHelper::SaveStringToFile(CommandMetaFile(TEXT("/vt meta load Missing")),*(Folder/TEXT("B.met")));
    ACEVTProfile::ConvertFile(Folder/TEXT("A.met"),Issues);TestTrue(TEXT("Missing transitive dependency blocks activation"),Issues.Num()>0);
    TArray<FString> Bad;ACEVTProfile::CompileCommand(TEXT("/vt meta load ../outside"),Bad);TestTrue(TEXT("Profile reference cannot escape its import folder"),Bad.Num()>0);
    // Reloading a NAV must reset an old embedded route and its pending action.
    {
        FACEPluginVM Reload;Reload.Load(Script,Error);auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"position":{"cell":2139029505,"x":84,"y":84,"z":12}})"));
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","navigation":true,"route":[],"vt_meta":[{"state":"Default","condition":{"type":1},"action":{"type":4,"route":{"route":[{"kind":"pause","legacy":true,"seconds":10,"cell":2139029505,"x":84,"y":84,"z":12}]}}}]})"));
        TestTrue(TEXT("Embedded pause begins"),Reload.Step(Snap,Profile,Intent,Error));
        Profile->SetNumberField(TEXT("vt_revision"),1);Profile->SetStringField(TEXT("vt_loaded_kind"),TEXT("nav"));
        Profile->SetArrayField(TEXT("route"),{MakeShared<FJsonValueObject>(AdapterJSON(TEXT(R"({"cell":2139029505,"x":90,"y":84,"z":12})")))});
        TestTrue(TEXT("Loaded route executes"),Reload.Step(Snap,Profile,Intent,Error));
        if(Intent)TestEqual(TEXT("New NAV clears pending pause and old overlay route"),Intent->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    {
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"action_serial":0,"container":100,"container_is_corpse":true,"inventory":[],"known_spells":[1],"contents":[{"id":99,"wcid":500,"name":"Unknown Scroll","type":8192,"object_class":42,"spell":6150,"scroll_can_learn":true,"identified":true,"count":1}]})"));
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"read_unknown_scrolls":true,"loot_rules":[{"action":"skip"}]})"));
        FACEPluginVM Scrolls;Scrolls.Load(Script,Error);TestTrue(TEXT("Automatic scroll pickup executes"),Scrolls.Step(Snap,Profile,Intent,Error));
        TestEqual(TEXT("Learnable unknown scroll precedes skip rule"),Intent->GetStringField(TEXT("action")),FString(TEXT("loot")));
        Snap->SetArrayField(TEXT("inventory"),Snap->GetArrayField(TEXT("contents")));Snap->SetArrayField(TEXT("contents"),{});
        Scrolls.Step(Snap,Profile,Intent,Error);TestEqual(TEXT("Confirmed transfer closes corpse before reading"),Intent->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
        Snap->SetNumberField(TEXT("container"),0);Scrolls.Step(Snap,Profile,Intent,Error);TestEqual(TEXT("Reads only the newly received scroll"),Intent->GetStringField(TEXT("action")),FString(TEXT("read")));
        Scrolls.Step(Snap,Profile,Intent,Error);TestFalse(TEXT("Reading waits for completion"),Intent->HasField(TEXT("action")));
        Snap->SetArrayField(TEXT("inventory"),{});Snap->SetArrayField(TEXT("known_spells"),{MakeShared<FJsonValueNumber>(6150)});Scrolls.Step(Snap,Profile,Intent,Error);TestFalse(TEXT("Successful learning finishes without retry"),Intent->HasField(TEXT("action")));
        Snap->SetNumberField(TEXT("container"),100);Snap->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(AdapterJSON(TEXT(R"({"id":101,"wcid":500,"name":"Another Copy","object_class":42,"spell":6150,"scroll_can_learn":true,"count":1})")))});
        for(int Case=0;Case<4;++Case)
        {
            auto Scenario=MakeShared<FJsonObject>(*Snap);auto Item=MakeShared<FJsonObject>(*Snap->GetArrayField(TEXT("contents"))[0]->AsObject());Scenario->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Item)});
            if(Case>0)Scenario->SetArrayField(TEXT("known_spells"),{});
            if(Case==1)Item->SetBoolField(TEXT("scroll_can_learn"),false);
            if(Case==2)Scenario->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Item)});
            if(Case==3)Profile->SetBoolField(TEXT("read_unknown_scrolls"),false);
            FACEPluginVM Excluded;Excluded.Load(Script,Error);Excluded.Step(Scenario,Profile,Intent,Error);
            TestEqual(*FString::Printf(TEXT("Known, too difficult, duplicate, or disabled scroll case %d uses skip"),Case),Intent->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
        }
    }
    // An owned unknown scroll can survive a busy response or a previous run.
    // UseDone alone is not evidence that its spell was learned.
    for(bool BusyError:{false,true})
    {
        FACEPluginVM Reads;Reads.Load(Script,Error);
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"action_serial":0,"action_error":0,"container":0,"inventory":[{"id":99,"wcid":500,"name":"Scroll","object_class":42,"spell":6150,"scroll_can_learn":true,"resource_available":true,"count":1}],"known_spells":[]})"));
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"read_unknown_scrolls":true})"));
        auto Step=[&](){TestTrue(TEXT("Scroll retry policy executes"),Reads.Step(Snap,Profile,Intent,Error));return Intent&&Intent->HasField(TEXT("action"))?Intent->GetStringField(TEXT("action")):FString();};
        TestEqual(TEXT("Restart recovers an unread carried scroll"),Step(),FString(TEXT("read")));
        Snap->SetNumberField(TEXT("action_serial"),1);Snap->SetNumberField(TEXT("action_error"),BusyError?0x3c:0);
        auto Receipt=AdapterJSON(TEXT(R"({"type":"use","source":98,"action_serial":1,"requested_at":100,"error":0})"));Receipt->SetNumberField(TEXT("error"),BusyError?0x3c:0);
        Snap->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});Step();
        Snap->SetNumberField(TEXT("time"),101);TestTrue(TEXT("Unrelated completion cannot finish scroll reading"),Step().IsEmpty());
        Snap->SetNumberField(TEXT("time"),100);Receipt->SetNumberField(TEXT("source"),99);
        TestTrue(TEXT("A use completion cannot discard an unlearned scroll"),Step().IsEmpty());
        Snap->SetNumberField(TEXT("time"),101);Step();
        Snap->SetNumberField(TEXT("time"),104);
        TestEqual(TEXT("Rejected or unconfirmed read is retried"),Step(),FString(TEXT("read")));
        Snap->SetArrayField(TEXT("known_spells"),{MakeShared<FJsonValueNumber>(6150)});
        TestTrue(TEXT("Learning confirmation prevents another read even before removal arrives"),Step().IsEmpty());
        Snap->SetNumberField(TEXT("time"),200);TestTrue(TEXT("Known carried copy is never consumed again"),Step().IsEmpty());

        FACEPluginVM Bounded;Bounded.Load(Script,Error);Snap->SetArrayField(TEXT("known_spells"),{});Snap->SetNumberField(TEXT("action_serial"),0);
        for(int Attempt=1;Attempt<=3;++Attempt)
        {
            Snap->SetNumberField(TEXT("time"),200+Attempt*10);Bounded.Step(Snap,Profile,Intent,Error);
            TestEqual(TEXT("Bounded retry sends a read"),Intent->GetStringField(TEXT("action")),FString(TEXT("read")));
            Snap->SetNumberField(TEXT("action_serial"),Attempt);Snap->SetNumberField(TEXT("action_error"),0x3c);
            Receipt->SetNumberField(TEXT("action_serial"),Attempt);Receipt->SetNumberField(TEXT("requested_at"),200+Attempt*10);Receipt->SetNumberField(TEXT("error"),0x3c);
            Bounded.Step(Snap,Profile,Intent,Error);
        }
        TestEqual(TEXT("Third failure is reported without stopping UCM"),Intent->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
        Snap->SetNumberField(TEXT("time"),300);Bounded.Step(Snap,Profile,Intent,Error);
        TestFalse(TEXT("Repeatedly rejected scroll cannot create an endless loop"),Intent->HasField(TEXT("action")));
        for(const FString Exclusion:{TEXT("retained"),TEXT("equipped"),TEXT("unavailable"),TEXT("unlearnable"),TEXT("disabled")})
        {
            FACEPluginVM Excluded;Excluded.Load(Script,Error);auto S2=MakeShared<FJsonObject>(*Snap);auto P2=MakeShared<FJsonObject>(*Profile);
            auto Item=MakeShared<FJsonObject>(*Snap->GetArrayField(TEXT("inventory"))[0]->AsObject());S2->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Item)});
            if(Exclusion==TEXT("disabled"))P2->SetBoolField(TEXT("read_unknown_scrolls"),false);
            else if(Exclusion==TEXT("unavailable"))Item->SetBoolField(TEXT("resource_available"),false);
            else if(Exclusion==TEXT("unlearnable"))Item->SetBoolField(TEXT("scroll_can_learn"),false);
            else Item->SetBoolField(Exclusion,true);
            Excluded.Step(S2,P2,Intent,Error);TestFalse(TEXT("Protected or unauthorized inventory scroll stays untouched"),Intent->HasField(TEXT("action")));
        }
    }
    for(const FString Action:{TEXT("salvage"),TEXT("sell"),TEXT("read")})
    {
        FACEPluginVM Loot;Loot.Load(Script,Error);
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"position":{"cell":2139029505,"x":84,"y":84,"z":12},"action_serial":0,"container":100,"container_is_corpse":true,"inventory":[{"id":42,"wcid":42,"name":"Ust","object_class":40,"count":1}],"contents":[{"id":99,"wcid":500,"name":"Loot","count":1,"identified":true}],"corpses":[{"id":101,"name":"Next corpse","distance":1}]})"));
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[{"action":"keep"}]})"));
        Profile->GetArrayField(TEXT("loot_rules"))[0]->AsObject()->SetStringField(TEXT("action"),Action);
        Snap->GetArrayField(TEXT("contents"))[0]->AsObject()->SetNumberField(TEXT("material"),59);
        Snap->GetArrayField(TEXT("contents"))[0]->AsObject()->SetNumberField(TEXT("workmanship"),5);
        TestTrue(TEXT("Loot action transfers first"),Loot.Step(Snap,Profile,Intent,Error));if(!Intent){AddError(Error);continue;}
        TestEqual(TEXT("Loot uses normal transfer"),Intent->GetStringField(TEXT("action")),FString(TEXT("loot")));
        auto Owned=Snap->GetArrayField(TEXT("inventory"));Owned.Add(Snap->GetArrayField(TEXT("contents"))[0]);Snap->SetArrayField(TEXT("inventory"),Owned);Snap->SetArrayField(TEXT("contents"),{});
        TestTrue(TEXT("Confirmed transfer closes corpse"),Loot.Step(Snap,Profile,Intent,Error));
        if(Intent)TestEqual(TEXT("Close before post-loot action"),Intent->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
        Snap->SetNumberField(TEXT("container"),0);if(Action==TEXT("sell"))Snap->SetNumberField(TEXT("vendor"),50);
        TestTrue(TEXT("Post-loot action executes"),Loot.Step(Snap,Profile,Intent,Error));
        if(Intent)TestEqual(TEXT("Pending item processed before next corpse"),Intent->GetStringField(TEXT("action")),Action);
        TestTrue(TEXT("Wait for server completion"),Loot.Step(Snap,Profile,Intent,Error));if(Intent)TestFalse(TEXT("No duplicate destructive request"),Intent->HasField(TEXT("action")));
        Snap->SetNumberField(TEXT("time"),120);TestTrue(TEXT("Unchanged item timeout handled"),Loot.Step(Snap,Profile,Intent,Error));
        if(Intent)TestEqual(TEXT("Timeout pauses its activity instead of selling again"),Intent->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
    }
    for(int Type:{2000,2001,2003,2005,2006,2008,17})
    {
        FACEPluginVM Rules;Rules.Load(Script,Error);
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"container":100,"inventory":[],"contents":[{"id":99,"wcid":500,"name":"Weapon","count":1,"identified":true,"object_class":1,"spell_ids":[6089,6091],"palettes":[67108865],"int_properties":{"218103842":40,"171":9,"179":1,"131":1},"float_properties":{"167772171":0.2,"167772174":2,"29":1.2}}]})"));
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[{"action":"keep","conditions":[{"field":"legacy","key":0,"args":[]}]}]})"));
        auto Rule=Profile->GetArrayField(TEXT("loot_rules"))[0]->AsObject()->GetArrayField(TEXT("conditions"))[0]->AsObject();Rule->SetNumberField(TEXT("key"),Type);
        TArray<TSharedPtr<FJsonValue>> Args;auto Add=[&](double V){Args.Add(MakeShared<FJsonValueNumber>(V));};
        if(Type==2003){Add(50);Add(218103842);}else if(Type==2005){Add(1.28);Add(29);}else if(Type==17){Add(0);Add(1);}else {Add(Type==2001?80:Type==2000?45:70);if(Type==2008){Add(1.28);Add(0);}}
        if(Type==2001)Snap->GetArrayField(TEXT("contents"))[0]->AsObject()->SetNumberField(TEXT("object_class"),9);
        Rule->SetArrayField(TEXT("args"),Args);TestTrue(TEXT("Computed loot rule executes"),Rules.Step(Snap,Profile,Intent,Error));
        if(Intent)TestEqual(*FString::Printf(TEXT("Classic computed rule %d chooses matching item"),Type),Intent->GetStringField(TEXT("action")),FString(TEXT("loot")));
    }
    {
        auto Doc=ACEVTProfile::Read(TEXT("UTL\n1\n0\n"),TEXT("utl"),Error);auto Block=MakeShared<FJsonObject>();Block->SetStringField(TEXT("name"),TEXT("SalvageCombine"));Block->SetStringField(TEXT("body"),TEXT("1\n1-6,7-8,9,10\n1\n6\n1-10\n1\n6\n10000\n"));Doc->SetArrayField(TEXT("extras"),{MakeShared<FJsonValueObject>(Block)});
        const auto Converted=ACEVTProfile::Convert(Doc,Issues);TestEqual(TEXT("Salvage group and value policy imports"),Issues.Num(),0);
        TestEqual(TEXT("Material threshold retained"),Converted->GetObjectField(TEXT("salvage_policy"))->GetObjectField(TEXT("values"))->GetNumberField(TEXT("6")),10000.);
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","salvage_combine":true})"));Profile->SetObjectField(TEXT("salvage_policy"),Converted->GetObjectField(TEXT("salvage_policy")));
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"inventory":[{"id":1,"object_class":40},{"id":2,"object_class":39,"material":6,"structure":40,"workmanship":3,"value":100},{"id":3,"object_class":39,"material":6,"structure":40,"workmanship":7,"value":100},{"id":4,"object_class":39,"material":6,"structure":100,"workmanship":7,"value":100000}]})"));
        FACEPluginVM Combine;Combine.Load(Script,Error);TestTrue(TEXT("Salvage plan runs"),Combine.Step(Snap,Profile,Intent,Error));if(Intent){TestEqual(TEXT("Combines partial bags below value target"),Intent->GetStringField(TEXT("action")),FString(TEXT("combine_salvage")));TestEqual(TEXT("Full bag excluded"),Intent->GetStringField(TEXT("items")),FString(TEXT("2,3")));}
        Combine.Step(Snap,Profile,Intent,Error);if(Intent)TestFalse(TEXT("Combine waits for removal"),Intent->HasField(TEXT("action")));
    }
    {
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","radius":48,"vt_meta":[{"state":"Default","condition":{"type":1},"action":{"type":11,"key":"radius","boolean":false,"scale":240,"properties":{"v":"radius"}}},{"state":"Default","condition":{"type":26,"expression":{"op":"==","left":{"op":"call","name":"getvar","args":[{"op":"literal","value":"radius"}]},"right":{"op":"literal","value":0.2}}},"action":{"type":2,"command":{"op":"say","text":"units verified"}}}]})"));
        FACEPluginVM Options;Options.Load(Script,Error);TestTrue(TEXT("VT option get executes"),Options.Step(S,Profile,Intent,Error));if(Intent)TestEqual(TEXT("Range option converts meters to VT coordinates"),Intent->GetStringField(TEXT("text")),FString(TEXT("units verified")));
        TArray<FString> Problems;auto Face=ACEVTProfile::CompileCommand(TEXT("/mt face 270"),Problems);TestEqual(TEXT("Face heading uses native axis conversion"),Face->GetNumberField(TEXT("heading")),0.);
        auto Jump=ACEVTProfile::CompileCommand(TEXT("/mt sjumpw 750"),Problems);TestEqual(TEXT("Jump hold milliseconds becomes charge"),Jump->GetNumberField(TEXT("charge")),.75);TestTrue(TEXT("Shift jump walks"),Jump->GetBoolField(TEXT("walk_jump")));
        auto UseWorld=ACEVTProfile::CompileCommand(TEXT("/mt uselp Chest"),Problems);TestEqual(TEXT("Landscape use cannot resolve an inventory item"),UseWorld->GetStringField(TEXT("scope")),FString(TEXT("world")));TestEqual(TEXT("Native action commands compile"),Problems.Num(),0);
    }
    // Assert decisions as well as compilation: operator semantics control whole routes.
    const TArray<TPair<FString,FString>> Expressions={
        {TEXT("setvar[flag,0];0&&setvar[flag,1]"),TEXT("getvar[flag]==1")},
        {TEXT("setvar[result,`ABC`==abc]"),TEXT("getvar[result]==1")},
        {TEXT("setvar[result,1||0&&0]"),TEXT("getvar[result]==0")},
        {TEXT("setvar[result,`ABC`#`^ab`]"),TEXT("getvar[result]==1")},
        {TEXT("setvar[result,cstr[7] + ` points`]"),TEXT("getvar[result]==`7 points`")}
    };
    for(const auto& Expression:Expressions)
    {
        TArray<FString> Problems;auto Command=ACEVTProfile::CompileCommand(TEXT("/vt mexec ")+Expression.Key,Problems);
        TestEqual(TEXT("Legacy operators and quoted strings compile"),Problems.Num(),0);
        auto Check=ACEVTProfile::CompileCommand(TEXT("/vt mexec ")+Expression.Value,Problems);
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","vt_meta":[{"state":"Default","condition":{"type":1},"action":{"type":2}},{"state":"Default","condition":{"type":26},"action":{"type":2,"command":{"op":"say","text":"matched"}}}]})"));
        Profile->GetArrayField(TEXT("vt_meta"))[0]->AsObject()->GetObjectField(TEXT("action"))->SetObjectField(TEXT("command"),Command);
        Profile->GetArrayField(TEXT("vt_meta"))[1]->AsObject()->GetObjectField(TEXT("condition"))->SetObjectField(TEXT("expression"),Check->GetObjectField(TEXT("expression")));
        FACEPluginVM VMExpression;VMExpression.Load(Script,Error);TestTrue(TEXT("Expression policy executes"),VMExpression.Step(S,Profile,Intent,Error));
        if(Intent)TestEqual(*Expression.Key,Intent->GetStringField(TEXT("text")),FString(TEXT("matched")));
    }
    {
        FACEPluginVM Rare;Rare.Load(Script,Error);
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":20,"distance":1,"targets":[{"id":20,"distance":1,"name":"Monster"}],"corpses":[{"id":10,"distance":1,"name":"Ordinary corpse","identified":true,"rare":false},{"id":11,"distance":2,"name":"Rare corpse","identified":true,"rare":true}]})"));
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","looting":true,"loot_only_rare":true,"loot_priority":true,"loot_rules":[{"action":"keep"}]})"));
        TestTrue(TEXT("Rare corpse priority policy executes"),Rare.Step(Snap,Profile,Intent,Error));
        if(Intent){TestEqual(TEXT("Loot priority precedes combat"),Intent->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));TestEqual(TEXT("Ordinary corpse skipped"),Intent->GetNumberField(TEXT("item")),11.);}
    }
    {
        FACEPluginVM Dialog;Dialog.Load(Script,Error);
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","vt_meta":[{"state":"Default","condition":{"type":1},"action":{"type":3,"children":[{"type":2,"command":{"op":"use","item":"Collector"}},{"type":2,"command":{"op":"confirm","accept":true}}]}}]})"));
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"action_serial":0,"route_objects":[{"id":10,"name":"Collector"}]})"));
        Dialog.Step(Snap,Profile,Intent,Error);if(Intent)TestEqual(TEXT("Interaction precedes confirmation"),Intent->GetStringField(TEXT("action")),FString(TEXT("use_world")));
        Snap->SetBoolField(TEXT("busy"),true);Snap->SetBoolField(TEXT("ready"),false);Snap->SetArrayField(TEXT("confirmations"),{MakeShared<FJsonValueObject>(AdapterJSON(TEXT(R"({"type":5,"context":123})")))});
        Dialog.Step(Snap,Profile,Intent,Error);if(Intent)TestFalse(TEXT("Confirmation isn't discarded during host cooldown"),Intent->HasField(TEXT("action")));
        Snap->SetBoolField(TEXT("ready"),true);Dialog.Step(Snap,Profile,Intent,Error);if(Intent){TestEqual(TEXT("Confirmation can complete pending use"),Intent->GetStringField(TEXT("action")),FString(TEXT("confirm")));TestEqual(TEXT("Exact dialog context returned"),Intent->GetNumberField(TEXT("context")),123.);}
    }
    {
        FACEPluginVM Partial;Partial.Load(Script,Error);
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[{"action":"sell","keep_up_to":1}]})"));
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"container":10,"container_is_corpse":true,"inventory":[],"contents":[{"id":99,"wcid":500,"name":"Stack","count":5,"identified":true}]})"));
        Partial.Step(Snap,Profile,Intent,Error);if(Intent)TestEqual(TEXT("Partial transfer requests selected count"),Intent->GetNumberField(TEXT("amount")),1.);
        Snap->GetArrayField(TEXT("contents"))[0]->AsObject()->SetNumberField(TEXT("count"),4);
        Snap->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(AdapterJSON(TEXT(R"({"id":100,"wcid":500,"name":"Split stack","count":1})")))});
        Partial.Step(Snap,Profile,Intent,Error);Snap->SetNumberField(TEXT("container"),0);Snap->SetNumberField(TEXT("vendor"),50);Partial.Step(Snap,Profile,Intent,Error);
        if(Intent){TestEqual(TEXT("Split GUID is used for sale"),Intent->GetStringField(TEXT("action")),FString(TEXT("sell")));TestEqual(TEXT("Original corpse stack never sold"),Intent->GetNumberField(TEXT("item")),100.);}
    }
    {
        FACEPluginVM Buff;Buff.Load(Script,Error);
        auto Profile=AdapterJSON(TEXT(R"({"buffing":true,"recovery":false,"combat":"off"})"));
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"action_serial":0,"spells":[{"id":20,"name":"Strength Self","category":1,"power":50,"skill":300,"known":true,"self_buff":true}]})"));
        Buff.Step(Snap,Profile,Intent,Error);if(Intent)TestEqual(TEXT("Initial buff attempt"),Intent->GetStringField(TEXT("action")),FString(TEXT("cast")));
        Snap->SetNumberField(TEXT("action_serial"),1);Snap->SetNumberField(TEXT("last_spell"),20);Snap->SetNumberField(TEXT("action_error"),1024);Snap->SetNumberField(TEXT("time"),105);
        Buff.Step(Snap,Profile,Intent,Error);if(Intent)TestFalse(TEXT("Unavailable buff tier does not stop the scheduler"),Intent->HasField(TEXT("action")));
    }
    {
        FACEPluginVM Follow;Follow.Load(Script,Error);
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","navigation":true,"follow_corners":true,"follow_id":8})"));
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"position":{"cell":2139029505,"x":0,"y":0,"z":12},"route_objects":[{"id":8,"name":"Friend","cell":2139029505,"x":10,"y":0,"z":12,"distance":10}]})"));
        Follow.Step(Snap,Profile,Intent,Error);
        // JSON values are copied into Lua each tick; previous observations stay immutable.
        Snap->GetArrayField(TEXT("route_objects"))[0]->AsObject()->SetNumberField(TEXT("y"),10);Follow.Step(Snap,Profile,Intent,Error);
        if(Intent){TestEqual(TEXT("Follow goes to observed corner before cutting diagonally"),Intent->GetNumberField(TEXT("x")),10.);TestEqual(TEXT("Old corner remains queued"),Intent->GetNumberField(TEXT("y")),0.);}
        auto Leader=Snap->GetArrayField(TEXT("route_objects"))[0]->AsObject();
        Leader->SetNumberField(TEXT("x"),0);Leader->SetNumberField(TEXT("y"),1);Leader->SetNumberField(TEXT("distance"),1);
        TestTrue(*Error,Follow.Step(Snap,Profile,Intent,Error));
        TestEqual(TEXT("Nearby leader across a wall cannot skip the first corner"),Intent->GetNumberField(TEXT("x")),10.);
        TestEqual(TEXT("Nearby returning segment cannot replace the oldest footstep"),Intent->GetNumberField(TEXT("y")),0.);
        Snap->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),10);
        Follow.Step(Snap,Profile,Intent,Error);
        TestEqual(TEXT("Reaching one corner advances only to the next corner"),Intent->GetNumberField(TEXT("y")),10.);
        Snap->GetObjectField(TEXT("position"))->SetNumberField(TEXT("y"),10);Leader->SetNumberField(TEXT("distance"),14);
        Follow.Step(Snap,Profile,Intent,Error);
        TestEqual(TEXT("After traversing all corners follow can approach the leader"),Intent->GetNumberField(TEXT("x")),0.);
        TestEqual(TEXT("Corner movement uses a tight arrival radius"),Intent->GetNumberField(TEXT("arrival_radius")),.15);
        // While a cast is pending the host collects target position packets,
        // without copying the entire inventory/spellbook through Lua per packet.
        FACEPluginVM BusyFollow;BusyFollow.Load(Script,Error);
        Snap->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),0);Snap->GetObjectField(TEXT("position"))->SetNumberField(TEXT("y"),0);
        Snap->SetNumberField(TEXT("follow_motion_target"),8);
        Snap->SetArrayField(TEXT("follow_motion"),AdapterJSON(TEXT(R"({"v":[{"cell":2139029505,"x":10,"y":0,"z":12},{"cell":2139029505,"x":10,"y":10,"z":12}]})"))->GetArrayField(TEXT("v")));
        BusyFollow.Step(Snap,Profile,Intent,Error);
        TestEqual(TEXT("Buffered observations preserve corners crossed during buffing"),Intent->GetNumberField(TEXT("x")),10.);
        TestEqual(TEXT("Buffered path starts with its oldest observation"),Intent->GetNumberField(TEXT("y")),0.);
        Snap->SetArrayField(TEXT("follow_motion"),{});
        FACEPluginVM Straight;Straight.Load(Script,Error);Leader->SetNumberField(TEXT("y"),0);
        for(int32 Sample=0;Sample<2000;++Sample)
        {Leader->SetNumberField(TEXT("x"),10+Sample*.1);TestTrue(*Error,Straight.Step(Snap,Profile,Intent,Error));}
        TestEqual(TEXT("Straight paths coalesce without exhausting the bounded trail"),Intent->GetStringField(TEXT("action")),FString(TEXT("move")));
        Profile->SetBoolField(TEXT("follow_corners"),false);
        Straight.Step(Snap,Profile,Intent,Error);
        TestEqual(TEXT("Disabling Corners resumes direct following"),Intent->GetNumberField(TEXT("x")),Leader->GetNumberField(TEXT("x")));
    }
    {
        FACEPluginVM RouteDialog;RouteDialog.Load(Script,Error);
        auto Profile=AdapterJSON(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","navigation":true,"route":[{"kind":"use","legacy":true,"object_id":10,"cell":2139029505,"x":0,"y":0,"z":12},{"kind":"command","legacy":true,"cell":2139029505,"x":0,"y":0,"z":12,"command":{"op":"confirm","accept":true}},{"kind":"walk","cell":2139029505,"x":20,"y":0,"z":12}]})"));
        auto Snap=AdapterJSON(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"action_serial":0,"position":{"cell":2139029505,"x":0,"y":0,"z":12},"route_objects":[{"id":10,"name":"Gate","distance":1}]})"));
        TestTrue(*Error,RouteDialog.Step(Snap,Profile,Intent,Error));if(Intent)TestEqual(TEXT("Route first uses object"),Intent->GetStringField(TEXT("action")),FString(TEXT("use_world")));
        Snap->SetBoolField(TEXT("busy"),true);Snap->SetArrayField(TEXT("confirmations"),{MakeShared<FJsonValueObject>(AdapterJSON(TEXT(R"({"type":5,"context":123})")))});
        TestTrue(*Error,RouteDialog.Step(Snap,Profile,Intent,Error));if(Intent)TestEqual(TEXT("Next NAV confirmation can unblock pending use"),Intent->GetStringField(TEXT("action")),FString(TEXT("confirm")));
        Snap->SetBoolField(TEXT("busy"),false);Snap->SetArrayField(TEXT("confirmations"),{});Snap->SetNumberField(TEXT("action_serial"),1);
        TestTrue(*Error,RouteDialog.Step(Snap,Profile,Intent,Error));TestTrue(*Error,RouteDialog.Step(Snap,Profile,Intent,Error));
        if(Intent)TestEqual(TEXT("Completed confirmation is not issued twice"),Intent->GetStringField(TEXT("action")),FString(TEXT("move")));
    }
    return true;
}
#endif
