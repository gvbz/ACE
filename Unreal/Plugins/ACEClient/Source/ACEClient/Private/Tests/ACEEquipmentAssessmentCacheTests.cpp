#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEEquipmentAssessmentCacheTest,"ACE.Plugins.EquipmentAssessmentCache",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEEquipmentAssessmentCacheTest::RunTest(const FString&)
{
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
    auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);
    Context.OwningGameInstance=GI;Context.SetCurrentWorld(World);GI->OnWorldChanged(nullptr,World);GI->Init();
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);};
    auto* C=GI->GetSubsystem<UACEClientSubsystem>();auto* H=GI->GetSubsystem<UACEPluginSubsystem>();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/EquipmentAssessments")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    H->StorageRoot=Root;
    ON_SCOPE_EXIT {H->EquipmentAssessmentsDirty=false;IFileManager::Get().DeleteDirectory(*Root,false,true);};
    C->Session=MakeShared<FACESession>();auto& Session=*C->Session;
    Session.State=EACESessionState::InWorld;Session.PlayerGuid=0x50000001;Session.ServerName=TEXT("Assessment test server");
    H->RefreshAutomationData();
    FACEWorldObject W;W.Guid=int32(0xF06A016Au);W.Name=TEXT("Custom piercing sceptre");W.WeenieClassId=29260;
    W.ItemType=ACEItemType::Caster;W.ValidLocations=0x1000000;W.ContainerId=Session.PlayerGuid;W.SetupId=0x020012BF;W.IconId=0x06001234;
    Session.WorldObjects.Add(W.Guid,W);
    FACEAppraisalInfo A;A.ObjectGuid=W.Guid;A.bSuccess=true;A.IntProperties={{45,2},{179,16},{107,500},{108,1000},{273,13},{274,9007},{275,5500}};
    A.FloatProperties={{152,2.5}};A.StringProperties.Add(16,TEXT("Loot-only information"));A.SpellIds.Add(6046);
    H->ObserveAppraisal(A);
    TestTrue(TEXT("Fresh equipment is identified"),H->PluginItemSnapshot(W)->AsObject()->GetBoolField(TEXT("identified")));
    H->ObserveTargetCreated(W);
    auto Snapshot=H->PluginItemSnapshot(W)->AsObject();
    TestFalse(TEXT("Full object refresh still invalidates general appraisal"),Snapshot->GetBoolField(TEXT("identified")));
    TestTrue(TEXT("Full object refresh retains combat assessment"),Snapshot->GetBoolField(TEXT("combat_identified")));
    TestEqual(TEXT("Caster element retained"),Snapshot->GetNumberField(TEXT("damage_type")),2.);
    TestFalse(TEXT("Cached combat data cannot restore mana"),Snapshot->HasField(TEXT("mana")));
    TestFalse(TEXT("Cached combat data cannot authorize salvage quality rules"),Snapshot->GetObjectField(TEXT("int_properties"))->HasField(TEXT("179")));
    TestFalse(TEXT("Cached combat data does not restore item spell lists"),Snapshot->HasField(TEXT("spell_ids")));
    FACEAppraisalInfo Failed;Failed.ObjectGuid=W.Guid;H->ObserveAppraisal(Failed);
    TestTrue(TEXT("Failed refresh does not erase a successful combat assessment"),H->PluginItemSnapshot(W)->AsObject()->GetBoolField(TEXT("combat_identified")));
    W.ContainerId=0;W.WielderId=Session.PlayerGuid;W.CurrentWieldedLocation=0x1000000;Session.WorldObjects[W.Guid]=W;
    H->ObserveTargetDeleted(W.Guid);
    TestNotNull(TEXT("Visual detachment while owned keeps assessment"),H->GetEquipmentAppraisal(W));
    W.ContainerId=Session.PlayerGuid;W.WielderId=0;W.CurrentWieldedLocation=0;Session.WorldObjects[W.Guid]=W;
    H->ObserveTargetCreated(W);
    TestTrue(TEXT("Unequipped weapon remains available to combat"),H->PluginItemSnapshot(W)->AsObject()->GetBoolField(TEXT("can_wield")));
    // Exercise the same save/reset/load path as an actual logout and login.
    Session.State=EACESessionState::Disconnected;H->RefreshAutomationData();
    TestEqual(TEXT("Logout clears in-memory combat records"),H->EquipmentAssessments.Num(),0);
    Session.State=EACESessionState::InWorld;H->RefreshAutomationData();
    TestTrue(TEXT("Relog restores owned equipment without ID wait"),H->PluginItemSnapshot(W)->AsObject()->GetBoolField(TEXT("combat_appraisal_cached")));
    Session.ServerName=TEXT("Other server");H->RefreshAutomationData();
    TestNull(TEXT("Server scope prevents ID collisions"),H->GetEquipmentAppraisal(W));
    Session.ServerName=TEXT("Assessment test server");Session.PlayerGuid++;H->RefreshAutomationData();
    TestNull(TEXT("Character scope prevents reuse"),H->GetEquipmentAppraisal(W));
    Session.PlayerGuid--;H->RefreshAutomationData();
    W.WeenieClassId++;TestNull(TEXT("Reused GUID with another WCID cannot reuse cache"),H->GetEquipmentAppraisal(W));W.WeenieClassId--;
    W.ContainerId=77;TestNull(TEXT("Unowned equipment cannot use cache"),H->GetEquipmentAppraisal(W));W.ContainerId=Session.PlayerGuid;
    // New success replaces old stats; current requirements are recalculated.
    A.IntProperties[45]=4;H->ObserveAppraisal(A);H->Appraisals.Empty();
    TestEqual(TEXT("New appraisal replaces cached element"),H->PluginItemSnapshot(W)->AsObject()->GetNumberField(TEXT("damage_type")),4.);
    A.IntProperties.Add(158,10);A.IntProperties.Add(159,999);A.IntProperties.Add(160,1);
    H->ObserveAppraisal(A);H->Appraisals.Empty();
    TestFalse(TEXT("Cached appraisal does not bypass current wield requirements"),H->PluginItemSnapshot(W)->AsObject()->GetBoolField(TEXT("can_wield")));
    A.IntProperties.Remove(158);A.IntProperties.Remove(159);A.IntProperties.Remove(160);
    H->ObserveAppraisal(A);H->Appraisals.Empty();
    auto InventoryRow=[&](){auto Out=MakeShared<FJsonObject>();H->ExtendInventorySnapshot(Out);return Out->GetArrayField(TEXT("inventory"))[0]->AsObject();};
    TestTrue(TEXT("Missing custom count defers to server"),InventoryRow()->GetBoolField(TEXT("can_wield")));
    Session.PlayerVitals.QualityInt64s.Add(9007,0);
    TestFalse(TEXT("Receiving zero invalidates cached row eligibility"),InventoryRow()->GetBoolField(TEXT("can_wield")));
    Session.PlayerVitals.QualityInt64s[9007]=5500;
    TestTrue(TEXT("Meeting requirement updates cached row without another appraisal"),InventoryRow()->GetBoolField(TEXT("can_wield")));
    FACESkillInfo Missile;Missile.SkillId=47;Missile.Base=288;Missile.Current=338;Missile.AdvancementClass=2;
    Session.PlayerVitals.Skills.Add(Missile);
    A.IntProperties={{158,2},{159,47},{160,290}};
    H->ObserveAppraisal(A);H->Appraisals.Empty();
    TestFalse(TEXT("A temporary bonus cannot qualify cached equipment"),InventoryRow()->GetBoolField(TEXT("can_wield")));
    // Keep Current constant to isolate the cache dependency on the permanent baseline.
    Session.PlayerVitals.Skills.Last().UnscaledAugmentationBonus=5;
    TestTrue(TEXT("A changed permanent base unlocks cached equipment without reappraisal"),InventoryRow()->GetBoolField(TEXT("can_wield")));
    Session.WorldObjects.Remove(W.Guid);H->ObserveTargetDeleted(W.Guid);
    TestEqual(TEXT("Actual deletion removes combat record"),H->EquipmentAssessments.Num(),0);
    return true;
}
#endif
