#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootPipelineTest,"ACE.Plugins.LootAppraisalPipeline",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootPipelineTest::RunTest(const FString&)
{
    auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    FString Source,Error;
    FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    auto Snapshot=[&](int32 Count)
    {
        auto S=Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":700,"container_is_corpse":true,"contents_ready":true,"position":{"cell":30998795,"x":30,"y":-34,"z":0},"spells":[],"targets":[],"inventory":[],"corpses":[]})"));
        TArray<TSharedPtr<FJsonValue>> Items;
        for(int32 Index=0;Index<Count;++Index)
        {
            auto Item=Json(TEXT(R"({"wcid":200,"name":"Copper Ring","container":700,"count":1,"identified":false,"resource_available":true})"));
            Item->SetNumberField(TEXT("id"),800+Index);Items.Add(MakeShared<FJsonValueObject>(Item));
        }
        S->SetArrayField(TEXT("contents"),Items);return S;
    };
    auto Profile=[&](){return Json(TEXT(R"({"combat":"off","buffing":false,"recovery":false,"looting":true,"idle_peace":false,"loot_rules":[{"action":"keep","min_workmanship":7},{"action":"skip"}],"navigation":false})"));};
    auto Step=[&](FACEPluginVM& VM,auto S,auto P,double At)
    {
        S->SetNumberField(TEXT("time"),At);TSharedPtr<FJsonObject> Intent;
        if(!VM.Step(S,P,Intent,Error))AddError(Error);
        return Intent?Intent:MakeShared<FJsonObject>();
    };
    auto Action=[](auto I){FString A;I->TryGetStringField(TEXT("action"),A);return A;};
    auto Flag=[](auto I,const TCHAR* Name){bool B=false;I->TryGetBoolField(Name,B);return B;};
    auto Answer=[](auto S,int32 Index,double Workmanship)
    {
        auto Item=S->GetArrayField(TEXT("contents"))[Index]->AsObject();
        Item->SetBoolField(TEXT("identified"),true);Item->SetNumberField(TEXT("workmanship"),Workmanship);
    };
    {
        FACEPluginVM VM;if(!TestTrue(TEXT("Pipeline policy loads"),VM.Load(Source,Error)))return false;
        auto S=Snapshot(12),P=Profile();TSet<int32> Requested;
        for(int32 Index=0;Index<8;++Index)
        {
            auto I=Step(VM,S,P,100+Index*.05);
            TestEqual(TEXT("Queue advances while prior appraisals remain unanswered"),Action(I),FString(TEXT("identify")));
            TestTrue(TEXT("Corpse appraisal opts into pipelining and prompt work"),Flag(I,TEXT("pipeline"))&&Flag(I,TEXT("continue_work")));
            double Id=0;I->TryGetNumberField(TEXT("item"),Id);Requested.Add(int32(Id));
        }
        TestEqual(TEXT("Eight different items fill the pending window"),Requested.Num(),8);
        for(int32 Index=0;Index<6;++Index)
            TestTrue(TEXT("Full queue neither floods nor runs later rules on unknown data"),Action(Step(VM,S,P,100.4+Index*.05)).IsEmpty());
        Answer(S,7,0); // Out-of-order success that does not match the keep rule.
        auto I=Step(VM,S,P,100.75);
        TestEqual(TEXT("One answered appraisal makes room for the next item"),Action(I),FString(TEXT("identify")));
        TestEqual(TEXT("Pending items are not requested again"),I->GetNumberField(TEXT("item")),808.);
        S->SetNumberField(TEXT("container"),701);S->SetArrayField(TEXT("contents"),{});
        TestEqual(TEXT("Changing corpses discards the old scan without looting stale items"),Action(Step(VM,S,P,100.8)),FString(TEXT("close_corpse")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(1),P=Profile();
        TestEqual(TEXT("Initial missing appraisal is requested"),Action(Step(VM,S,P,100)),FString(TEXT("identify")));
        TestTrue(TEXT("Lost/failed reply is not immediately retried"),Action(Step(VM,S,P,104.99)).IsEmpty());
        TestEqual(TEXT("Unanswered appraisal retries after five seconds"),Action(Step(VM,S,P,105.01)),FString(TEXT("identify")));
        TestEqual(TEXT("Third attempt is available after another timeout"),Action(Step(VM,S,P,110.02)),FString(TEXT("identify")));
        auto Failed=Step(VM,S,P,115.03);
        TestTrue(TEXT("Exhausted appraisals never loot unknown qualities"),Action(Failed)!=TEXT("loot")&&Action(Failed)!=TEXT("identify"));
        TestTrue(TEXT("Appraisal exhaustion is visible to the user"),Failed->GetStringField(TEXT("status")).Contains(TEXT("apprais")));
        P->SetBoolField(TEXT("ucm_resume"),true);
        TestEqual(TEXT("Restart clears abandoned retries"),Action(Step(VM,S,P,115.1)),FString(TEXT("identify")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(2),P=Profile();
        P->SetArrayField(TEXT("loot_rules"),Json(TEXT(R"({"rules":[{"action":"skip","min_workmanship":7},{"action":"keep"}]})"))->GetArrayField(TEXT("rules")));
        Step(VM,S,P,100);Step(VM,S,P,100.05);
        TestTrue(TEXT("Unknown earlier skip rule blocks fallback keep"),Action(Step(VM,S,P,100.1)).IsEmpty());
        Answer(S,1,8);TestTrue(TEXT("Out-of-order high-workmanship reply is skipped"),Action(Step(VM,S,P,100.15)).IsEmpty());
        Answer(S,0,2);auto Pickup=Step(VM,S,P,100.2);
        TestEqual(TEXT("Only the item whose ordered rules now allow it is picked up"),Pickup->GetNumberField(TEXT("item")),800.);
        TestEqual(TEXT("Matching item issues a pickup"),Action(Pickup),FString(TEXT("loot")));
        TestTrue(TEXT("Pickup schedules a prompt continuation"),Flag(Pickup,TEXT("continue_work")));
        TestTrue(TEXT("Next pickup waits for ownership confirmation"),Action(Step(VM,S,P,100.25)).IsEmpty());
        auto Item=S->GetArrayField(TEXT("contents"))[0]->AsObject();Item->SetNumberField(TEXT("container"),1);
        S->SetArrayField(TEXT("inventory"),{MakeShared<FJsonValueObject>(Item)});
        S->SetArrayField(TEXT("contents"),{S->GetArrayField(TEXT("contents"))[1]});
        TestEqual(TEXT("Confirmed pickup can finish the corpse on the next work decision"),Action(Step(VM,S,P,100.3)),FString(TEXT("close_corpse")));
    }
    {
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(2),P=Profile();
        auto Items=S->GetArrayField(TEXT("contents"));Items[0]->AsObject()->SetNumberField(TEXT("material"),60);Items[1]->AsObject()->SetNumberField(TEXT("material"),59);
        P->SetArrayField(TEXT("loot_rules"),Json(TEXT(R"({"rules":[{"action":"keep","material":59,"min_workmanship":7}]})"))->GetArrayField(TEXT("rules")));
        auto I=Step(VM,S,P,100);
        TestEqual(TEXT("Public filters avoid needless appraisal of irrelevant items"),I->GetNumberField(TEXT("item")),801.);
        S->SetBoolField(TEXT("contents_ready"),false);
        TestTrue(TEXT("Incomplete contents wait without closing or appraising"),Action(Step(VM,S,P,100.05)).IsEmpty());
        S->SetBoolField(TEXT("contents_ready"),true);S->SetBoolField(TEXT("container_is_corpse"),false);
        TestTrue(TEXT("UCM never treats a player-opened chest as a corpse"),Action(Step(VM,S,P,100.1)).IsEmpty());
    }
    {
        // Deterministic reply timing, using the real Lua queue and its requested
        // decision cadence. Native dispatch gating is covered by RouteAndTellRequests.
        FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(12),P=Profile();
        TMap<int32,double> Pending;int32 Requests=0,PeakPending=0;double Finished=0;
        for(double At=100;At<102;)
        {
            for(auto It=Pending.CreateIterator();It;++It)if(It.Value()<=At)
            {Answer(S,It.Key()-800,0);It.RemoveCurrent();}
            auto I=Step(VM,S,P,At);const FString A=Action(I);
            if(A==TEXT("identify"))
            {
                const int32 Id=int32(I->GetNumberField(TEXT("item")));
                TestFalse(TEXT("Simulated requests never duplicate a pending item"),Pending.Contains(Id));
                Pending.Add(Id,At+.2);++Requests;PeakPending=FMath::Max(PeakPending,Pending.Num());
            }
            if(A==TEXT("close_corpse")){Finished=At-100;break;}
            TestTrue(TEXT("No unknown/low-quality item is accidentally kept"),A!=TEXT("loot"));
            At+=Flag(I,TEXT("continue_work"))?.05:.25;
        }
        TestEqual(TEXT("Twelve required appraisals are each requested once"),Requests,12);
        TestTrue(TEXT("Pending work stays bounded"),PeakPending<=8);
        TestTrue(TEXT("Twelve-item evaluation finishes within one second with 200ms replies"),Finished>0&&Finished<1.);
        AddInfo(FString::Printf(TEXT("12-item simulated corpse, 200ms appraisal replies: %.2fs through close; peak %d pending. Excludes movement and real server transfer time."),Finished,PeakPending));
    }
    return !HasAnyErrors();
}
#endif
