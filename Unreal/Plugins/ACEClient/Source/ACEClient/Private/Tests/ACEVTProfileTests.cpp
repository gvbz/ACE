#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEVTProfile.h"
#include "Mods/ACEPluginVM.h"
namespace
{
TSharedPtr<FJsonObject> ParseVTTest(const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVTProfileTest,"ACE.Plugins.VirindiProfiles",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEVTProfileTest::RunTest(const FString&)
{
    FString Error;TArray<FString> Issues;
    auto D=ACEVTProfile::Read(TEXT("UTL\r\n1\r\n1\r\nCopper\r\n\r\n0;1;12\r\n9\r\n6\r\n131\r\n"),TEXT("utl"),Error);
    // Length is .NET UTF-16 character count, including CRLF (8 here).
    TestFalse(TEXT("Incorrect block length rejected"),D.IsValid());
    D=ACEVTProfile::Read(TEXT("UTL\r\n1\r\n1\r\nCopper\r\n\r\n0;1;12\r\n8\r\n6\r\n131\r\n"),TEXT("utl"),Error);
    TestTrue(TEXT("Length-prefixed CRLF UTL parses"),D.IsValid());if(!D){AddError(Error);return false;}
    const FString Encoded=ACEVTProfile::WriteLoot(D,Error);TestTrue(TEXT("UTL writer succeeds"),!Encoded.IsEmpty()&&Error.IsEmpty());
    const auto RoundTrip=ACEVTProfile::Read(Encoded,TEXT("utl"),Error);TestEqual(TEXT("UTL requirement body round trips"),RoundTrip->GetArrayField(TEXT("rules"))[0]->AsObject()->GetArrayField(TEXT("requirements"))[0]->AsObject()->GetStringField(TEXT("body")),FString(TEXT("6\r\n131\r\n")));
    auto Converted=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Material condition supported"),Issues.Num(),0);
    TestEqual(TEXT("Property key preserved"),Converted->GetArrayField(TEXT("loot_rules"))[0]->AsObject()->GetArrayField(TEXT("conditions"))[0]->AsObject()->GetNumberField(TEXT("key")),131.);
    D->GetArrayField(TEXT("rules"))[0]->AsObject()->SetStringField(TEXT("expression"),TEXT("false [persisted Classic metadata]"));
    Converted=ACEVTProfile::Convert(D,Issues);
    TestEqual(TEXT("Classic expression metadata does not reject ordinary requirements"),Issues.Num(),0);
    TestEqual(TEXT("Classic expression metadata preserves the rule conditions"),Converted->GetArrayField(TEXT("loot_rules"))[0]->AsObject()->GetArrayField(TEXT("conditions")).Num(),1);
    auto MetadataRoundTrip=ACEVTProfile::Read(ACEVTProfile::WriteLoot(D,Error),TEXT("utl"),Error);
    TestEqual(TEXT("Unused Classic expression still round trips"),MetadataRoundTrip->GetArrayField(TEXT("rules"))[0]->AsObject()->GetStringField(TEXT("expression")),FString(TEXT("false [persisted Classic metadata]")));
    D->GetArrayField(TEXT("rules"))[0]->AsObject()->SetNumberField(TEXT("action_id"),10);
    D->GetArrayField(TEXT("rules"))[0]->AsObject()->SetNumberField(TEXT("amount"),4);
    Converted=ACEVTProfile::Convert(D,Issues);
    TestTrue(TEXT("VT KeepUpTo retains name-based inventory counting"),Converted->GetArrayField(TEXT("loot_rules"))[0]->AsObject()->GetBoolField(TEXT("count_by_name")));
    auto Bad=ACEVTProfile::Read(TEXT("UTL\n1\n1\nSkip\n\n0;0;999\n4\nabc\n"),TEXT("utl"),Error);
    TestTrue(TEXT("Unknown requirement is retained"),Bad.IsValid());ACEVTProfile::Convert(Bad,Issues);TestTrue(TEXT("Unknown Skip blocks whole profile activation"),Issues.Num()>0);
    auto Disabled=ParseVTTest(TEXT(R"({"format":"utl","extras":[],"rules":[{"label":"Disabled unknown","action_id":1,"requirements":[{"type":123456,"body":"custom"},{"type":9999,"body":"True\n"}]}]})"));
    auto DisabledProfile=ACEVTProfile::Convert(Disabled,Issues);
    TestEqual(TEXT("Disabled unsupported requirement does not block other rules"),Issues.Num(),0);
    const auto DisabledRule=DisabledProfile->GetArrayField(TEXT("loot_rules"))[0]->AsObject();
    TestFalse(TEXT("Disabled imported rule stays disabled"),DisabledRule->GetBoolField(TEXT("enabled")));
    TestTrue(TEXT("Unsupported requirements remain flagged against partial reactivation"),DisabledRule->GetArrayField(TEXT("compatibility_issues")).Num()>0);
    TestTrue(TEXT("Regex repetition accepted with runtime work limits"),ACEVTProfile::IsSupportedPattern(TEXT("(a+)+$")));
    TestTrue(TEXT("Literal alternatives and anchors supported"),ACEVTProfile::IsSupportedPattern(TEXT("^Copper|Legendary Strength$")));
    auto Nav=ACEVTProfile::Read(TEXT("uTank2 NAV 1.2\n2\n2\n0\n0\n0\n0.05\n0\n3\n0.01\n0\n0.05\n0\n1500\n"),TEXT("nav"),Error);
    TestTrue(TEXT("Linear route parses"),Nav.IsValid());auto Route=ACEVTProfile::Convert(Nav,Issues);TestEqual(TEXT("Walk and pause conversion"),Issues.Num(),0);
    TestTrue(TEXT("Linear reverses direction"),Route->GetBoolField(TEXT("reverse_route")));TestFalse(TEXT("Linear does not wrap"),Route->GetBoolField(TEXT("loop_route")));
    const auto Point=Route->GetArrayField(TEXT("route"))[0]->AsObject();TestEqual(TEXT("Height conversion"),Point->GetNumberField(TEXT("z")),12.);TestEqual(TEXT("Origin local coordinate"),Point->GetNumberField(TEXT("x")),84.);
    TestEqual(TEXT("Pause milliseconds converted"),Route->GetArrayField(TEXT("route"))[1]->AsObject()->GetNumberField(TEXT("seconds")),1.5);
    auto Meta=ACEVTProfile::Read(TEXT("1\nCondAct\n1\nState\nn\n1\ns\nDefault\n"),TEXT("met"),Error);TestTrue(TEXT("Typed table parses"),Meta.IsValid());ACEVTProfile::Convert(Meta,Issues);TestTrue(TEXT("Meta activation requires runtime adapter"),Issues.Num()>0);
    TestFalse(TEXT("Negative table count rejected"),ACEVTProfile::Read(TEXT("-1\n"),TEXT("met"),Error).IsValid());
    // End-to-end rule decision: ID first, matching spell + property, preserve first-match Skip.
    FString Script;FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto P=ParseVTTest(TEXT(R"({"looting":true,"combat":"off","loot_rules":[{"action":"keep","conditions":[{"field":"int","key":131,"op":"eq","value":6},{"field":"spells","pattern":"Legendary|Epic","exclude":"Bane","value":1}]}]})"));
    auto Snapshot=ParseVTTest(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":0,"position":{"cell":2139029532,"x":84,"y":84,"z":12},"container":9,"container_is_corpse":true,"contents_ready":true,"contents":[{"id":10,"name":"Copper Ring","wcid":12,"count":1,"type":8,"max_stack":1,"int_properties":{"131":6}}],"inventory":[]})"));
    FACEPluginVM VM;TestTrue(TEXT("UCM loads"),VM.Load(Script,Error));TSharedPtr<FJsonObject> Intent;
    TestTrue(TEXT("Unknown spell properties wait for ID"),VM.Step(Snapshot,P,Intent,Error));TestEqual(TEXT("Requests appraisal"),Intent->GetStringField(TEXT("action")),FString(TEXT("identify")));
    auto Item=Snapshot->GetArrayField(TEXT("contents"))[0]->AsObject();Item->SetBoolField(TEXT("identified"),true);Item->SetArrayField(TEXT("spell_names"),{MakeShared<FJsonValueString>(TEXT("Legendary Strength"))});Snapshot->SetNumberField(TEXT("time"),101);
    TestTrue(TEXT("Appraised conditions execute"),VM.Step(Snapshot,P,Intent,Error));TestEqual(TEXT("Matching item is looted"),Intent->GetStringField(TEXT("action")),FString(TEXT("loot")));
    FACEPluginVM NavVM;NavVM.Load(Script,Error);auto NP=ParseVTTest(TEXT(R"({"navigation":true,"combat":"off","reverse_route":true,"route":[{"cell":2139029532,"x":84,"y":84,"z":12},{"cell":2139029532,"x":86,"y":84,"z":12}]})"));
    auto NS=ParseVTTest(TEXT(R"({"time":1,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":0,"position":{"cell":2139029532,"x":84,"y":84,"z":12},"inventory":[]})"));
    NavVM.Step(NS,NP,Intent,Error);NS->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),86);NavVM.Step(NS,NP,Intent,Error);NavVM.Step(NS,NP,Intent,Error);
    TestEqual(TEXT("Linear route returns to previous point"),Intent->GetStringField(TEXT("action")),FString(TEXT("move")));TestEqual(TEXT("Reverse target x"),Intent->GetNumberField(TEXT("x")),84.);
    // Large rulesets remain bounded by the same sandbox and scan budget.
    auto Large=ParseVTTest(TEXT(R"({"looting":true,"combat":"off"})"));TArray<TSharedPtr<FJsonValue>> Many;
    for(int I=0;I<2048;++I)Many.Add(MakeShared<FJsonValueObject>(ParseVTTest(TEXT(R"({"label":"Rule","action":"keep","name":"Does not match","conditions":[{"field":"int","key":131,"op":"eq","value":6}]})"))));
    Large->SetArrayField(TEXT("loot_rules"),Many);FACEPluginVM LargeVM;LargeVM.Load(Script,Error);const double Start=FPlatformTime::Seconds();
    TestTrue(TEXT("2048-rule profile fits sandbox"),LargeVM.Step(Snapshot,Large,Intent,Error));AddInfo(FString::Printf(TEXT("2048-rule decision: %.2f ms"),(FPlatformTime::Seconds()-Start)*1000));
    // Optional local corpus audit: never required on another developer's machine.
    FString Folder;if(FParse::Value(FCommandLine::Get(),TEXT("VTProfileCorpus="),Folder))
    {
        TArray<TSharedPtr<FJsonValue>> Reports;int32 Count=0,Failed=0,Ready=0;
        for(const TCHAR* Ext:{TEXT("utl"),TEXT("nav"),TEXT("met"),TEXT("usd")})
        {
            TArray<FString> Files;IFileManager::Get().FindFilesRecursive(Files,*Folder,*(FString(TEXT("*."))+Ext),true,false);Files.Sort();
            for(const FString& File:Files)
            {
                ++Count;FString Source;FFileHelper::LoadFileToString(Source,*File);auto Doc=ACEVTProfile::Read(Source,Ext,Error);auto Entry=MakeShared<FJsonObject>();Entry->SetStringField(TEXT("file"),File);
                if(!Doc){++Failed;Entry->SetStringField(TEXT("error"),Error);AddError(File+TEXT(": ")+Error);}
                else
                {
                    if(FString(Ext)==TEXT("utl"))
                    {
                        const FString Saved=ACEVTProfile::WriteLoot(Doc,Error);auto Again=ACEVTProfile::Read(Saved,TEXT("utl"),Error);
                        TestTrue(*(File+TEXT(" re-exports")),Again.IsValid());
                        if(Again){FString Before,After;FJsonSerializer::Serialize(Doc,TJsonWriterFactory<>::Create(&Before));FJsonSerializer::Serialize(Again,TJsonWriterFactory<>::Create(&After));TestEqual(*(File+TEXT(" all rules, requirements and extras round trip")),After,Before);}
                    }
                    auto Profile=ACEVTProfile::ConvertFile(File,Issues);Profile->RemoveField(TEXT("vt_library"));if(Issues.IsEmpty())
                    {
                        ++Ready;
                        if(FPaths::GetCleanFilename(File)==TEXT("PhaelaeCustom_v6.utl"))
                        {
                            TestEqual(TEXT("Phaelae preserves all original rules"),Profile->GetArrayField(TEXT("loot_rules")).Num(),1434);
                            Profile->SetBoolField(TEXT("looting"),true);Profile->SetBoolField(TEXT("buffing"),false);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("off"));
                            for(const TCHAR* ItemName:{TEXT("Aetheria"),TEXT("Unmatched regression object")})
                            {
                                auto State=ParseVTTest(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"main_pack_slots":20,"inventory":[],"container":9,"container_is_corpse":true,"contents_ready":true,"contents":[{"id":10,"wcid":12,"count":1,"identified":true,"int_properties":{"218103849":27704},"spell_names":[]}]})"));
                                auto CorpusItem=State->GetArrayField(TEXT("contents"))[0]->AsObject();CorpusItem->SetStringField(TEXT("name"),ItemName);auto Strings=MakeShared<FJsonObject>();Strings->SetStringField(TEXT("1"),ItemName);CorpusItem->SetObjectField(TEXT("string_properties"),Strings);
                                FACEPluginVM LootVM;LootVM.Load(Script,Error);TSharedPtr<FJsonObject> CorpusIntent;bool Looted=false,Ran=true;
                                const double Begin=FPlatformTime::Seconds();
                                for(int Tick=0;Tick<128&&Ran;++Tick)
                                {
                                    State->SetNumberField(TEXT("time"),100+Tick*.2);Ran=LootVM.Step(State,Profile,CorpusIntent,Error);
                                    if(Ran&&CorpusIntent&&CorpusIntent->GetStringField(TEXT("action"))==TEXT("loot")){Looted=true;break;}
                                }
                                TestTrue(*(FString(TEXT("Full Phaelae rules fit sandbox: "))+Error),Ran);
                                TestEqual(TEXT("Full profile accepts Aetheria and rejects unmatched loot"),Looted,FString(ItemName)==TEXT("Aetheria"));
                                AddInfo(FString::Printf(TEXT("Phaelae %s: %.2f ms total"),ItemName,(FPlatformTime::Seconds()-Begin)*1000));
                            }
                        }
                        const TArray<TSharedPtr<FJsonValue>>* MetaRules=nullptr;
                        if(Profile->TryGetArrayField(TEXT("vt_meta"),MetaRules)&&MetaRules->IsEmpty())Entry->SetBoolField(TEXT("empty_meta"),true);
                        if(FPaths::GetCleanFilename(File)==TEXT("VirindiSpells.utl"))
                        {
                            Profile->SetBoolField(TEXT("looting"),true);Profile->SetBoolField(TEXT("buffing"),false);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("off"));
                            bool Passed=true;
                            for(const FString Name:{TEXT("Nelamar's War Magic Scroll"),TEXT("Esard's Life Magic Scroll"),TEXT("Geraux's Life Magic Scroll"),TEXT("Copper Ring")})
                            {
                                auto State=ParseVTTest(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"inventory":[],"container":9,"container_is_corpse":true,"contents_ready":true,"contents":[{"id":10,"wcid":12,"count":1,"identified":true}]})"));
                                auto ItemRecord=State->GetArrayField(TEXT("contents"))[0]->AsObject();ItemRecord->SetStringField(TEXT("name"),Name);auto Strings=MakeShared<FJsonObject>();Strings->SetStringField(TEXT("1"),Name);ItemRecord->SetObjectField(TEXT("string_properties"),Strings);
                                FACEPluginVM LootVM;LootVM.Load(Script,Error);TSharedPtr<FJsonObject> Result;FString Action;
                                const bool Ran=LootVM.Step(State,Profile,Result,Error)&&Result.IsValid()&&Result->TryGetStringField(TEXT("action"),Action);
                                Passed&=Ran&&Action==(Name==TEXT("Copper Ring")?TEXT("close_corpse"):TEXT("loot"));
                            }
                            TestTrue(TEXT("Installed VirindiSpells profile accepts its three scrolls and rejects unrelated loot"),Passed);Entry->SetBoolField(TEXT("known_loot_decisions_pass"),Passed);
                        }
                        if(FString(Ext)==TEXT("nav"))
                        {
                            const auto& Points=Profile->GetArrayField(TEXT("route"));
                            bool WalkOnly=Points.Num()>0;
                            for(const auto& V:Points){FString Kind;V->AsObject()->TryGetStringField(TEXT("kind"),Kind);WalkOnly&=Kind.IsEmpty()||Kind==TEXT("walk");}
                            if(WalkOnly)
                            {
                                // Execute whole known routes and their endpoint behavior, not just
                                // an import/idle tick. Geometry and host steering have separate tests.
                                FACEPluginVM RouteVM;RouteVM.Load(Script,Error);
                                Profile->SetBoolField(TEXT("navigation"),true);Profile->SetBoolField(TEXT("buffing"),false);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("off"));
                                auto State=ParseVTTest(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":0,"inventory":[]})"));
                                // This fixture measures an ordered full cycle from
                                // point one. The +5m approach below can be closer
                                // to a different point, so explicitly expose only
                                // the intended entry for the initial join. Nearest
                                // visible joining is tested by RouteJoin separately.
                                auto Visible=MakeShared<FJsonObject>();
                                for(int PointIndex=0;PointIndex<Points.Num();++PointIndex)Visible->SetBoolField(FString::FromInt(PointIndex+1),PointIndex==0);
                                State->SetObjectField(TEXT("route_visible"),Visible);
                                bool Reverse=false,Loop=false;Profile->TryGetBoolField(TEXT("reverse_route"),Reverse);Profile->TryGetBoolField(TEXT("loop_route"),Loop);
                                int Index=0,Direction=1,Visited=0;bool Passed=true;
                                const int Steps=(Loop||Reverse)?Points.Num()*2+2:Points.Num();
                                for(int Step=0;Step<Steps&&Passed;++Step)
                                {
                                    const auto Goal=Points[Index]->AsObject();auto Position=MakeShared<FJsonObject>();Position->Values=Goal->Values;
                                    Position->SetNumberField(TEXT("x"),Goal->GetNumberField(TEXT("x"))+5);State->SetObjectField(TEXT("position"),Position);State->SetNumberField(TEXT("time"),100+Step);
                                    TSharedPtr<FJsonObject> Move;Passed=RouteVM.Step(State,Profile,Move,Error)&&Move.IsValid();
                                    FString Action;double X=0,Y=0,Z=0,Cell=0;
                                    Passed=Passed&&Move->TryGetStringField(TEXT("action"),Action)&&Action==TEXT("move")&&Move->TryGetNumberField(TEXT("x"),X)&&Move->TryGetNumberField(TEXT("y"),Y)&&Move->TryGetNumberField(TEXT("z"),Z)&&Move->TryGetNumberField(TEXT("cell"),Cell);
                                    Passed=Passed&&X==Goal->GetNumberField(TEXT("x"))&&Y==Goal->GetNumberField(TEXT("y"))&&Z==Goal->GetNumberField(TEXT("z"))&&Cell==Goal->GetNumberField(TEXT("cell"));
                                    State->SetObjectField(TEXT("position"),Goal);Passed=Passed&&RouteVM.Step(State,Profile,Move,Error);
                                    if(Passed)++Visited;
                                    Index+=Direction;
                                    if(Index>=Points.Num()||Index<0){if(Reverse&&Points.Num()>1){Direction=-Direction;Index=Index<0?1:Points.Num()-2;}else Index=0;}
                                }
                                TestTrue(*(File+TEXT(" complete route and endpoint traversal: ")+Error),Passed);
                                Entry->SetNumberField(TEXT("walk_points_executed"),Visited);Entry->SetBoolField(TEXT("route_cycle_pass"),Passed);
                            }
                        }
                        if(FString(Ext)==TEXT("met")||FString(Ext)==TEXT("usd"))
                        {
                            FString EncodedProfile;FJsonSerializer::Serialize(Profile,TJsonWriterFactory<>::Create(&EncodedProfile));Entry->SetNumberField(TEXT("compiled_bytes"),FTCHARToUTF8(*EncodedProfile).Length());
                            FACEPluginVM CorpusVM;CorpusVM.Load(Script,Error);TSharedPtr<FJsonObject> Result;const double Begin=FPlatformTime::Seconds();
                            const bool Ran=CorpusVM.Step(NS,Profile,Result,Error);TestTrue(*(File+TEXT(" initial tick fits sandbox: ")+Error),Ran);
                            Entry->SetNumberField(TEXT("first_tick_ms"),(FPlatformTime::Seconds()-Begin)*1000);Entry->SetBoolField(TEXT("sandbox_pass"),Ran);
                            bool Repeated=Ran;for(int Tick=0;Repeated&&Tick<32;++Tick)Repeated=CorpusVM.Step(NS,Profile,Result,Error);
                            TestTrue(*(File+TEXT(" repeated ticks fit sandbox: ")+Error),Repeated);Entry->SetBoolField(TEXT("repeated_ticks_pass"),Repeated);
                        }
                    }
                    Entry->SetNumberField(TEXT("issues"),Issues.Num());TArray<TSharedPtr<FJsonValue>> Detail;for(const auto& I:Issues)Detail.Add(MakeShared<FJsonValueString>(I));Entry->SetArrayField(TEXT("details"),Detail);
                }
                Reports.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        auto Report=MakeShared<FJsonObject>();Report->SetArrayField(TEXT("files"),Reports);Report->SetNumberField(TEXT("parsed"),Count-Failed);Report->SetNumberField(TEXT("ready"),Ready);FString Json;FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Json));
        FFileHelper::SaveStringToFile(Json,*(FPaths::ProjectSavedDir()/TEXT("Logs/vt-native-compatibility.json")));
        AddInfo(FString::Printf(TEXT("VT corpus: %d parsed, %d errors, %d supported conversions"),Count-Failed,Failed,Ready));
    }
    return true;
}
#endif
