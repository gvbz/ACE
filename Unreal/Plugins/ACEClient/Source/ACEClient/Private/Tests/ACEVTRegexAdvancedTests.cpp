#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEVTRegex.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVTRegexAdvancedTest,"ACE.Plugins.AdvancedRegex",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEVTRegexAdvancedTest::RunTest(const FString&)
{
    FString Json,Error;TSharedPtr<FJsonObject> Oracle;
    const FString Path=IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("Source/ACEClient/Private/Tests/Fixtures/VTRegexDotNet.json");
    if(!TestTrue(TEXT("Load .NET regex oracle"),FFileHelper::LoadFileToString(Json,*Path)))return false;
    if(!TestTrue(TEXT("Parse .NET oracle"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Oracle)))return false;
    int Failures=0;
    for(const auto& Value:Oracle->GetArrayField(TEXT("cases")))
    {
        auto Case=Value->AsObject();const FString Pattern=Case->GetStringField(TEXT("pattern")),Text=Case->GetStringField(TEXT("text"));
        const FString Label=Pattern+TEXT(" on [")+Text+TEXT("]");
        ACEVTRegex::FCaptures Captures;bool Found=ACEVTRegex::Match(Pattern,Text,Captures,Error);
        bool OK=TestTrue(*(Label+TEXT(" error: ")+Error),Error.IsEmpty());
        OK=TestEqual(*Label,Found,Case->GetBoolField(TEXT("matches")))&&OK;
        const auto& Expected=Case->GetArrayField(TEXT("captures"));
        OK=TestEqual(*(Label+TEXT(" capture count")),Captures.Num(),Expected.Num())&&OK;
        for(const auto& Pair:Expected)
        {
            const auto Entry=Pair->AsObject();const FString Name=Entry->GetStringField(TEXT("name"));
            OK=TestEqual(*(Label+TEXT(" group ")+Name),Captures.FindRef(Name),Entry->GetStringField(TEXT("value")))&&OK;
        }
        // Both API entry points use the same semantics; cached results must
        // never turn an execution-limit error into a successful no-match.
        if(OK)OK=TestEqual(*(Label+TEXT(" predicate")),ACEVTRegex::Test(Pattern,Text,Error),Found);
        if(!OK&&++Failures>=20)break;
    }
    for(const FString Pattern:{TEXT("(?<-missing>a)"),TEXT("(?(4)a|b)"),TEXT("(?(1)a|b|c)(a)"),TEXT("(?<open>a)(?<-open>b"),TEXT("(?<o>a)(?<-o>b){4,2}")})
    {TestFalse(TEXT("Invalid history expression rejected"),ACEVTRegex::Validate(Pattern,Error));TestFalse(TEXT("Import error explains invalid expression"),Error.IsEmpty());}
    ACEVTRegex::FCaptures Captures;
    TestTrue(TEXT("Case-sensitive predicate warmup"),ACEVTRegex::Test(TEXT("^Case$"),TEXT("Case"),Error));
    TestFalse(TEXT("Result cache distinguishes case"),ACEVTRegex::Test(TEXT("^Case$"),TEXT("case"),Error));
    TestTrue(TEXT("Compiled cache distinguishes case"),ACEVTRegex::Test(TEXT("^case$"),TEXT("case"),Error));
    TestFalse(TEXT("Lowercase compiled pattern stays distinct"),ACEVTRegex::Test(TEXT("^case$"),TEXT("Case"),Error));
    TestTrue(TEXT("Validation cache case warmup"),ACEVTRegex::Validate(TEXT("(?<X>a)\\k<X>"),Error));
    TestFalse(TEXT("Validation cache preserves undefined case-sensitive names"),ACEVTRegex::Validate(TEXT("(?<X>a)\\k<x>"),Error));
    TestTrue(TEXT("Explicit capture condition survives normalization"),ACEVTRegex::Match(TEXT("(?n)^(?<x>a)?(?(x)b|c)$"),TEXT("ab"),Captures,Error));
    TestFalse(TEXT("Undefined balancing capture rejected on repeated validation"),ACEVTRegex::Validate(TEXT("(?<-missing>a)"),Error));
    const FString Hard=TEXT("^(?<o>z)?(a+)+$(?(o)b|c)");
    const FString Long=FString::ChrN(400,'a')+TEXT("!");
    for(int I=0;I<2;++I)
    {
        TestFalse(TEXT("Pathological history expression is bounded"),ACEVTRegex::Match(Hard,Long,Captures,Error));
        TestFalse(TEXT("Bounded failure retains a diagnostic"),Error.IsEmpty());
        TestEqual(TEXT("Bounded failure exposes no stale captures"),Captures.Num(),0);
    }
    TestTrue(TEXT("A fresh match recovers after work-limit failure"),ACEVTRegex::Match(TEXT("^(?<o>a)(?<-o>b)(?(o)(?!))$"),TEXT("ab"),Captures,Error));
    FACEPluginVM VM;TSharedPtr<FJsonObject> Intent;
    TestTrue(TEXT("Load Lua capture consumer"),VM.Load(TEXT("return function(s,p) local c=regexmatch('abce','^(?<x>a)(?<X>b)(?(x)c|d)(?(X)e|f)$'); return {action='chat',text=c.x..c.X..c['1']..c['2']} end"),Error));
    TestTrue(TEXT("Lua receives distinct case-sensitive capture names"),VM.Step(MakeShared<FJsonObject>(),MakeShared<FJsonObject>(),Intent,Error));
    if(TestTrue(TEXT("Capture consumer returns intent"),Intent.IsValid()))TestEqual(TEXT("Capture values reach the plugin unchanged"),Intent->GetStringField(TEXT("text")),FString(TEXT("abab")));
    return true;
}
#endif
