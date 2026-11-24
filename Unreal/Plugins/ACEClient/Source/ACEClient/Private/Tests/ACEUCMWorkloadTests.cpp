#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEDatSubsystem.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMIdleWorkloadTest,"ACE.Performance.UCMIdleWorkload",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMIdleWorkloadTest::RunTest(const FString&)
{
    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    auto* GI=NewObject<UGameInstance>(GEngine);GI->InitializeStandalone();
    GI->GetWorldContext()->SetCurrentWorld(World);World->SetGameInstance(GI);
    ON_SCOPE_EXIT {GI->Shutdown();World->DestroyWorld(false);};
    auto* C=GI->GetSubsystem<UACEClientSubsystem>();auto* H=GI->GetSubsystem<UACEPluginSubsystem>();auto* D=GI->GetSubsystem<UACEDatSubsystem>();
    H->StorageRoot=FPaths::ProjectSavedDir()/TEXT("Automation/UCMIdleWorkload");
    if(!TestTrue(TEXT("Retail DAT loads"),D->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call"))))return false;
    auto S=C->GetSession();S->State=EACESessionState::InWorld;S->PlayerGuid=1234;S->ServerName=TEXT("Idle workload");
    S->PlayerPosition.CellId=0x8A0E0016;S->PlayerPosition.Location=FVector(30,100,0);
    auto& V=S->PlayerVitals;V.bValid=true;V.Level=275;
    V.Health=V.MaxHealth=V.Stamina=V.MaxStamina=V.Mana=V.MaxMana=500;
    for(int32 Id=1;Id<7000&&S->KnownSpells.Num()<2048;++Id)
    {FString Name;uint32 Icon=0;if(D->TryGetSpellInfo(Id,Name,Icon))S->KnownSpells.Add(Id);}
    FACEWorldObject Player;Player.Guid=S->PlayerGuid;Player.Name=TEXT("Idle workload");Player.ItemsCapacity=102;
    Player.bIsPlayer=Player.bHasPosition=true;Player.Position=S->PlayerPosition;S->WorldObjects.Add(Player.Guid,Player);
    H->Plugins.Reset();auto P=MakeShared<FACEClientPlugin>();P->Id=TEXT("ucm");P->Enabled=P->Running=true;
    P->Profile=MakeShared<FJsonObject>();P->Profile->SetStringField(TEXT("combat"),TEXT("off"));
    for(const TCHAR* Key:{TEXT("buffing"),TEXT("recovery"),TEXT("looting"),TEXT("navigation"),TEXT("idle_peace")})P->Profile->SetBoolField(Key,false);
    H->Plugins.Add(P);H->RefreshAutomationData();
    for(int32 I=0;I<8;++I)
    {
        FACEWorldObject O;O.Guid=2000+I;O.Name=TEXT("Nearby monster");O.ItemType=ACEItemType::Creature;
        O.bHasPosition=true;O.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;O.Position=S->PlayerPosition;
        O.Position.Location.X+=I+2;S->WorldObjects.Add(O.Guid,O);
    }
    for(int32 I=0;I<512;++I)
    {
        FACEWorldObject O;O.Guid=3000+I;O.Name=TEXT("Stored armor");O.ItemType=ACEItemType::Armor;O.ContainerId=Player.Guid;
        O.StackSize=O.MaxStackSize=1;O.WeenieClassId=100+I;S->WorldObjects.Add(O.Guid,O);
        FACEAppraisalInfo A;A.ObjectGuid=O.Guid;A.bSuccess=true;
        for(int32 K=1;K<=40;++K)A.IntProperties.Add(K,K);
        for(int32 K=1;K<=8;++K)A.FloatProperties.Add(K,.5);
        H->ObserveAppraisal(A);
    }
    FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
    FACEPluginVM VM;if(!TestTrue(TEXT("Stock UCM loads"),VM.Load(Source,Error)))return false;
    TSharedPtr<FJsonObject> Output;
    const auto Initial=H->Snapshot();
    const auto FindRow=[](const TSharedPtr<FJsonObject>& Input,int32 Id)
    {
        for(const auto& Row:Input->GetArrayField(TEXT("inventory")))if(Row->AsObject()->GetNumberField(TEXT("id"))==Id)return Row;
        return TSharedPtr<FJsonValue>();
    };
    const auto Original=FindRow(Initial,3000),Unchanged=FindRow(Initial,3001);
    auto Appraisal=H->Appraisals.FindChecked(3000);Appraisal.IntProperties.Add(107,700);H->ObserveAppraisal(Appraisal);
    const auto Assessed=H->Snapshot();
    TestEqual(TEXT("One appraisal updates that item's mana immediately"),FindRow(Assessed,3000)->AsObject()->GetNumberField(TEXT("mana")),700.);
    TestTrue(TEXT("An unrelated inventory row is reused"),FindRow(Assessed,3001)==Unchanged);
    TestFalse(TEXT("The updated row is replaced rather than mutating an earlier snapshot"),FindRow(Assessed,3000)==Original);
    TestEqual(TEXT("Earlier snapshots keep their original values"),Original->AsObject()->GetNumberField(TEXT("mana")),0.);
    FACEPluginVM Probe;
    if(!TestTrue(TEXT("Snapshot probe loads"),Probe.Load(TEXT(R"(local book, pack, previousItem
        return function(s,p)
            local sameBook, samePack = book == s.spells, pack == s.inventory
            book, pack = s.spells, s.inventory
            local mana, unchanged
            for _, item in ipairs(pack) do
                if item.id == 3000 then mana=item.mana end
                if item.id == 3001 then unchanged=item end
            end
            local sameItem=previousItem==unchanged
            previousItem=unchanged
            return {same_book=sameBook, same_pack=samePack, same_item=sameItem, mana=mana, rules=#(p.loot_rules or {})}
        end)"),Error)))return false;
    for(const auto& Input:{Initial,Assessed,Assessed})
        if(!Probe.Step(Input,P->Profile,Output,Error,true)){AddError(Error);return false;}
    TestTrue(TEXT("An unchanged book and pack are reused in the stock VM"),Output->GetBoolField(TEXT("same_book")) && Output->GetBoolField(TEXT("same_pack")));
    TestEqual(TEXT("Updated item mana reaches the Lua policy"),Output->GetNumberField(TEXT("mana")),700.);
    if(!Probe.Step(Initial,P->Profile,Output,Error,true)){AddError(Error);return false;}
    TestTrue(TEXT("A changed inventory refreshes independently of spells"),Output->GetBoolField(TEXT("same_book")) && !Output->GetBoolField(TEXT("same_pack")));
    TestTrue(TEXT("One appraisal keeps unrelated Lua inventory rows"),Output->GetBoolField(TEXT("same_item")));
    TestEqual(TEXT("Inventory replacements reach Lua on that decision"),Output->GetNumberField(TEXT("mana")),0.);
    if(!Probe.Step(Initial,P->Profile,Output,Error)){AddError(Error);return false;}
    TestTrue(TEXT("Public plugin decisions still get fresh mutable tables"),!Output->GetBoolField(TEXT("same_book")) && !Output->GetBoolField(TEXT("same_pack")));
    auto ProbeProfile=MakeShared<FJsonObject>();
    for(int32 RuleCount:{2,1,0})
    {
        TArray<TSharedPtr<FJsonValue>> ChangedRules;
        for(int32 I=0;I<RuleCount;++I)ChangedRules.Add(MakeShared<FJsonValueNumber>(I));
        ProbeProfile->SetArrayField(TEXT("loot_rules"),MoveTemp(ChangedRules));
        if(!Probe.Step(Assessed,ProbeProfile,Output,Error,true)){AddError(Error);return false;}
        TestEqual(TEXT("Replacing or clearing rules takes effect on the next decision"),int32(Output->GetNumberField(TEXT("rules"))),RuleCount);
    }
    const auto Recharge=H->Snapshot(true);
    TestTrue(TEXT("Stopped equipment recharge sees current inventory, action receipts and cooldowns"),
        Recharge->HasField(TEXT("cooldowns")) && Recharge->HasField(TEXT("action_events")) && FindRow(Recharge,3000)==FindRow(Assessed,3000));
    TestFalse(TEXT("Stopped recharge does not build a combat snapshot"),Recharge->HasField(TEXT("targets")) || Recharge->HasField(TEXT("spells")));
    FACEAppraisalInfo Monster;Monster.ObjectGuid=2000;Monster.bSuccess=true;H->ObserveAppraisal(Monster);
    TestTrue(TEXT("Appraising a creature does not rebuild inventory"),FindRow(H->Snapshot(),3001)==Unchanged);
    S->WorldObjects[3001].WielderId=Player.Guid;S->WorldObjects[3001].CurrentWieldedLocation=4;S->WorldObjects[3001].ContainerId=0;++S->InventoryDataRevision;
    TestTrue(TEXT("Equipment confirmations still refresh ownership and equipped state"),FindRow(H->Snapshot(),3001)->AsObject()->GetBoolField(TEXT("equipped")));
    H->PendingResourceRefresh.Add(3000);H->CompletePendingAction(0);
    TestFalse(TEXT("Resource consumption invalidates its appraisal immediately"),FindRow(H->Snapshot(),3000)->AsObject()->GetBoolField(TEXT("identified")));
    H->ObserveAppraisal(Appraisal);
    V.Level=1;Appraisal.IntProperties.Add(158,7);Appraisal.IntProperties.Add(160,275);H->ObserveAppraisal(Appraisal);
    TestFalse(TEXT("An item below its wield requirement is ineligible"),FindRow(H->Snapshot(),3000)->AsObject()->GetBoolField(TEXT("can_wield")));
    V.Level=275;
    TestTrue(TEXT("Eligibility changes refresh cached rows without an appraisal"),FindRow(H->Snapshot(),3000)->AsObject()->GetBoolField(TEXT("can_wield")));
    S->WorldObjects.Remove(3001);++S->InventoryDataRevision;
    TestFalse(TEXT("Removed equipment disappears from the next decision"),FindRow(H->Snapshot(),3001).IsValid());
    FACEWorldObject Replacement=S->WorldObjects.FindChecked(3002);Replacement.Guid=3001;S->WorldObjects.Add(3001,Replacement);++S->InventoryDataRevision;
    TestTrue(TEXT("New inventory is included on the next decision"),FindRow(H->Snapshot(),3001).IsValid());
    for(bool Appraise:{false,true})
    {
        TArray<double> Samples;double SnapshotMs=0,LuaMs=0;
        for(int32 I=0;I<66;++I)
        {
            // The background scanner receives a reply about once per second.
            if(Appraise&&I%2==0)H->ObserveAppraisal(H->Appraisals.FindChecked(3000+I));
            const double Start=FPlatformTime::Seconds();const auto Input=H->Snapshot();const double Marshalled=FPlatformTime::Seconds();
            if(!VM.Step(Input,P->Profile,Output,Error,true)){AddError(Error);return false;}
            const double End=FPlatformTime::Seconds();
            if(I>=2){Samples.Add((End-Start)*1000);SnapshotMs+=(Marshalled-Start)*1000;LuaMs+=(End-Marshalled)*1000;}
            if(!TestTrue(TEXT("Combat-off idle UCM emits no action"),!Output||!Output->HasField(TEXT("action"))))return false;
        }
        Samples.Sort();
        AddInfo(FString::Printf(TEXT("Idle UCM %s (512 items / 2048 spells / 8 monsters): snapshot avg %.3f ms; Lua avg %.3f ms; total p95 %.3f ms; peak %.3f ms; heap peak %.2f MiB"),
            Appraise?TEXT("with appraisal replies"):TEXT("unchanged"),SnapshotMs/Samples.Num(),LuaMs/Samples.Num(),Samples[60],Samples.Last(),VM.GetPeakMemoryUsage()/1048576.));
    }
    // A full learned book, assessed packs and a Classic-sized loot profile must
    // fit together, not just pass separate spellbook/profile stress tests.
    for(int32 Id=1;Id<7000&&S->KnownSpells.Num()<4000;++Id)
    {FString Name;uint32 Icon=0;if(!S->KnownSpells.Contains(Id)&&D->TryGetSpellInfo(Id,Name,Icon))S->KnownSpells.Add(Id);}
    auto LargeProfile=MakeShared<FJsonObject>(*P->Profile);
    TArray<TSharedPtr<FJsonValue>> Rules;
    for(int32 I=0;I<1637;++I)
    {
        TSharedPtr<FJsonObject> Rule;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT(R"({"action":"keep","label":"Assessed armor rule","conditions":[{"field":"int","key":1,"op":"ge","value":0},{"field":"int","key":2,"op":"ge","value":0},{"field":"int","key":3,"op":"ge","value":0},{"field":"int","key":4,"op":"ge","value":0}]})")),Rule);
        const auto Four=Rule->GetArrayField(TEXT("conditions"));
        auto Eight=Four;Eight.Append(Four);Rule->SetArrayField(TEXT("conditions"),Eight);
        Rules.Add(MakeShared<FJsonValueObject>(Rule));
    }
    LargeProfile->SetArrayField(TEXT("loot_rules"),Rules);
    const auto LargeInput=H->Snapshot();FACEPluginVM LargeVM;
    if(!Probe.Step(LargeInput,P->Profile,Output,Error,true)){AddError(Error);return false;}
    TestFalse(TEXT("Learning spells replaces the VM's book on the next decision"),Output->GetBoolField(TEXT("same_book")));
    TestTrue(TEXT("Combined workload loads"),LargeVM.Load(Source,Error));
    double Work=0;
    for(int32 I=0;I<120;++I)
    {
        LargeInput->SetNumberField(TEXT("time"),100+I*.5);
        LargeInput->SetBoolField(TEXT("busy"),I%2!=0);
        const double Start=FPlatformTime::Seconds();
        if(!LargeVM.Step(LargeInput,LargeProfile,Output,Error,true))
        {AddError(FString::Printf(TEXT("Combined workload tick %d (%d spells, %d items, %d rules): %s"),I,S->KnownSpells.Num(),LargeInput->GetArrayField(TEXT("inventory")).Num(),Rules.Num(),*Error));return false;}
        Work+=FPlatformTime::Seconds()-Start;
    }
    AddInfo(FString::Printf(TEXT("Combined native inventory/book/profile (%d spells, %d items, %d rules): %.2f MiB peak; %.3f ms average"),LargeInput->GetArrayField(TEXT("spells")).Num(),LargeInput->GetArrayField(TEXT("inventory")).Num(),Rules.Num(),LargeVM.GetPeakMemoryUsage()/1048576.,Work*1000/120));
    FString ImportedText;TSharedPtr<FJsonObject> Imported;
    if(FFileHelper::LoadFileToString(ImportedText,*(FPaths::ProjectDir()/TEXT("Saved/ClientPlugins/Profiles/Loot/LootSnobV4.json")))
        &&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ImportedText),Imported))
    {
        auto ImportedProfile=MakeShared<FJsonObject>(*P->Profile);ImportedProfile->SetArrayField(TEXT("loot_rules"),Imported->GetArrayField(TEXT("loot_rules")));
        FACEPluginVM ImportedVM;TestTrue(TEXT("Imported combined workload loads"),ImportedVM.Load(Source,Error));
        for(int32 I=0;I<20;++I)if(!ImportedVM.Step(LargeInput,ImportedProfile,Output,Error,true))
        {AddError(TEXT("Native snapshot plus imported LootSnob: ")+Error);return false;}
        AddInfo(FString::Printf(TEXT("Native snapshot plus imported LootSnob: %.2f MiB peak"),ImportedVM.GetPeakMemoryUsage()/1048576.));
    }
    S->State=EACESessionState::Disconnected;
    H->RefreshAutomationData();
    TestTrue(TEXT("Disconnect releases cached inventory and dirty appraisal IDs"),H->CachedInventory.IsEmpty()&&H->CachedInventoryIndices.IsEmpty()&&H->DirtyInventoryAppraisals.IsEmpty());
    return !HasAnyErrors();
}
#endif
