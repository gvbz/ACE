#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"
#include "Mods/ACEUCMLootPresentation.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWeaponSkillLootTest,"ACE.Plugins.WeaponSkillLoot",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEWeaponSkillLootTest::RunTest(const FString&)
{
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
    auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);
    Context.SetCurrentWorld(World);Context.OwningGameInstance=GI;GI->OnWorldChanged(nullptr,World);GI->Init();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/WeaponSkillLoot")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);IFileManager::Get().DeleteDirectory(*Root,false,true);};
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();Host->StorageRoot=Root;Host->Settings=MakeShared<FJsonObject>();
    if(!TestTrue(TEXT("Isolated UCM selection initializes"),Host->InitializeUCMSelection()))return false;
    if(!TestTrue(TEXT("Create an empty loot profile"),Host->NewUCMSection(TEXT("Loot"),TEXT("Weapon skills"))))return false;
    auto Plugin=Host->Find(TEXT("ucm"));

    TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName,bool)> Find;
    Find=[&](TSharedRef<SWidget> W,FName Name,bool Tag)->TSharedPtr<SWidget>
    {
        if(Tag?W->GetTag()==Name:W->GetType()==Name)return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto R=Find(Children->GetChildAt(I),Name,Tag))return R;
        return nullptr;
    };
    auto Panel=Host->MakeUCMPanel(TEXT("Standalone loot"));
    auto Click=[&](const TCHAR* Tag)
    {
        auto W=Find(Panel,Tag,true);if(!TestTrue(Tag,W.IsValid()))return false;
        StaticCastSharedPtr<SButton>(W)->SimulateClick();return true;
    };
    // Exercise the actual controls and file persistence, not a parallel rule builder.
    if(FApp::CanEverRender()&&FSlateApplication::IsInitialized())
    {
        FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");
        FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
        TestTrue(TEXT("Retail UI resources load"),GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(DatDirectory));
        const FVector2D Size(780,1100);auto Window=SNew(SWindow).ClientSize(Size)[Panel];
        auto& Slate=FSlateApplication::Get();const auto OldFocus=Slate.GetUserFocusedWidget(0);Slate.AddWindow(Window,false);
        FWidgetRenderer Renderer(false,true);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        ON_SCOPE_EXIT {Slate.DismissAllMenus();if(OldFocus)Slate.SetUserFocus(0,OldFocus);Window->RequestDestroyWindow();Target->ReleaseResource();};
        auto Frame=[&](){++GFrameCounter;Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();};
        auto Choose=[&](const FString& Caption)
        {
            Frame();Frame();auto W=Find(Panel,TEXT("UCMLootPicker.weapon_skill"),true);
            if(!TestTrue(TEXT("Named skill picker is present"),W.IsValid()))return false;
            auto Combo=StaticCastSharedPtr<SComboButton>(W);Combo->SetIsOpen(true);Frame();
            auto Menu=Combo->GetMenuWindow();if(!TestTrue(TEXT("Named skill menu opens"),Menu.IsValid()))return false;
            for(const auto& Skill:ACEUCMLootPresentation::Properties(TEXT("weapon_skill")))
                TestTrue(TEXT("Each reference skill is a selectable menu entry"),Find(Menu.ToSharedRef(),FName(*(TEXT("UCMMenu.")+ACEUCMLootPresentation::PropertyLabel(TEXT("weapon_skill"),Skill.Id))),true).IsValid());
            auto Entry=Find(Menu.ToSharedRef(),FName(*(TEXT("UCMMenu.")+Caption)),true);
            if(!TestTrue(TEXT("Requested skill entry exists"),Entry.IsValid()))return false;
            StaticCastSharedPtr<SButton>(Entry)->SimulateClick();return true;
        };
        if(!Click(TEXT("UCMLootTool.New rule"))||!Click(TEXT("UCMLootTool.Weapon skill"))||!Choose(TEXT("Missile Weapons (47)")))return false;
        if(!Click(TEXT("UCMChoice.Apply requirement"))||!Click(TEXT("UCMLootTool.Apply rule")))return false;
        if(!Click(TEXT("UCMLootTool.New rule"))||!Click(TEXT("UCMLootTool.Wield requirement skill"))||!Choose(TEXT("Mana Conversion (16)")))return false;
        if(!Click(TEXT("UCMChoice.Apply requirement"))||!Click(TEXT("UCMLootTool.Apply rule")))return false;
        if(!Click(TEXT("UCMLootTool.New rule"))||!Click(TEXT("UCMLootTool.Weapon skill")))return false;
        auto Custom=StaticCastSharedPtr<SSpinBox<int32>>(Find(Panel,TEXT("UCMLootID.weapon_skill"),true));
        if(!TestTrue(TEXT("Custom skill IDs remain editable"),Custom.IsValid()))return false;
        Custom->SetValue(600,true);
        for(int32 Width:{780,450})
        {
            const FVector2D CaptureSize(Width,1100);auto* Capture=FWidgetRenderer::CreateTargetFor(CaptureSize,TF_Bilinear,true);
            for(int32 Pass=0;Pass<5;++Pass){++GFrameCounter;Renderer.DrawWidget(Capture,Panel,CaptureSize,0);FlushRenderingCommands();}
            // Narrow windows stack list/detail vertically. Scroll the page as
            // a user would before asserting geometry of the nested controls.
            auto PageScroll=StaticCastSharedPtr<SScrollBox>(Find(Panel,TEXT("SScrollBox"),false));
            if(PageScroll)PageScroll->SetScrollOffset(Width<500?650:80);
            auto Detail=Find(Panel,TEXT("LootRuleDetail"),true);
            auto Scroll=Detail?StaticCastSharedPtr<SScrollBox>(Find(Detail.ToSharedRef(),TEXT("SScrollBox"),false)):nullptr;
            if(Scroll)Scroll->SetScrollOffset(500);
            for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Capture,Panel,CaptureSize,0);FlushRenderingCommands();}
            auto Picker=Find(Panel,TEXT("UCMLootPicker.weapon_skill"),true);
            TestTrue(*FString::Printf(TEXT("Skill picker has usable width at %d pixels"),Width),Picker&&Picker->GetCachedGeometry().GetLocalSize().X>=200);
            TArray<FColor> Pixels;Capture->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> PNG;
            FImageUtils::PNGCompressImageArray(Width,1100,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("WeaponSkillLoot-%d.png"),Width)));Capture->ReleaseResource();
        }
        if(!Click(TEXT("UCMChoice.Apply requirement"))||!Click(TEXT("UCMLootTool.Apply rule")))return false;
        TestTrue(TEXT("Saved profile reloads from disk"),Host->InitializeUCMSelection());
        TestEqual(TEXT("Saved loot selection survives reload"),Host->UCMSectionName(TEXT("Loot")),FString(TEXT("Weapon skills")));
        const auto& Rules=Plugin->Profile->GetArrayField(TEXT("loot_rules"));
        if(!TestEqual(TEXT("All three UI-created rules persisted"),Rules.Num(),3))return false;
        for(int32 I=0;I<3;++I)
        {
            const auto C=Rules[I]->AsObject()->GetArrayField(TEXT("conditions"))[0]->AsObject();
            TestEqual(TEXT("UI saves original VT property key"),C->GetNumberField(TEXT("key")),double(I==1?159:218103840));
            TestEqual(TEXT("UI saves original skill ID including custom values"),C->GetNumberField(TEXT("value")),double(I==0?47:I==1?16:600));
            TestEqual(TEXT("Quick requirement uses equality"),C->GetStringField(TEXT("op")),FString(TEXT("eq")));
            TestFalse(TEXT("Native missing-value policy remains no match"),C->HasField(TEXT("missing_zero"))&&C->GetBoolField(TEXT("missing_zero")));
        }
    }

    auto Session=GI->GetSubsystem<UACEClientSubsystem>()->GetSession();
    Session->State=EACESessionState::InWorld;Session->PlayerGuid=0x50000001;
    Session->PlayerVitals.bValid=true;Session->PlayerVitals.Health=Session->PlayerVitals.MaxHealth=100;
    Session->PlayerPosition.CellId=0x7d640001;Session->PlayerPosition.Location=FVector(84,84,12);
    FACEWorldObject Self;Self.Guid=Session->PlayerGuid;Self.ItemsCapacity=100;Session->WorldObjects.Add(Self.Guid,Self);
    FACEWorldObject Corpse;Corpse.Guid=9;Corpse.ItemType=ACEItemType::Container;Corpse.ObjectDescriptionFlags=ACEObjectDescFlag::Corpse;
    Session->WorldObjects.Add(Corpse.Guid,Corpse);Session->OpenExternalContainerGuid=Corpse.Guid;Session->ContainerContents.Add(Corpse.Guid,{});
    FACEWorldObject Item;Item.Guid=10;Item.Name=TEXT("Test weapon");Item.ItemType=ACEItemType::MeleeWeapon;Item.ContainerId=Corpse.Guid;
    Session->WorldObjects.Add(Item.Guid,Item);
    FString Script,Error;
    if(!TestTrue(TEXT("Load shipped UCM"),FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")))))return false;
    auto Decision=[&](const TSharedPtr<FJsonObject>& Profile,const TCHAR* Expected)
    {
        Profile->SetBoolField(TEXT("looting"),true);Profile->SetBoolField(TEXT("buffing"),false);
        Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("off"));
        FACEPluginVM VM;if(!TestTrue(TEXT("UCM loads"),VM.Load(Script,Error)))return;
        auto State=Host->Snapshot();State->SetNumberField(TEXT("time"),100);
        TSharedPtr<FJsonObject> Intent;if(!TestTrue(TEXT("UCM evaluates native snapshot"),VM.Step(State,Profile,Intent,Error)&&Intent.IsValid()))return;
        TestEqual(TEXT("Loot decision uses the chosen skill property"),Intent->GetStringField(TEXT("action")),FString(Expected));
    };
    auto Appraise=[&](int32 AttackSkill,int32 RequiredSkill,bool HasWeapon=true)
    {
        FACEAppraisalInfo A;A.bSuccess=true;A.bHasWeaponProfile=HasWeapon;A.WeaponSkill=AttackSkill;
        A.IntProperties.Add(159,RequiredSkill);A.IntProperties.Add(158,2);Host->Appraisals.Add(Item.Guid,A);
    };
    // Rules actually created by the UI must drive the same shipped UCM matcher.
    for(const auto& R:Plugin->Profile->GetArrayField(TEXT("loot_rules")))
    {
        auto Profile=MakeShared<FJsonObject>();Profile->SetArrayField(TEXT("loot_rules"),{R});
        const auto C=R->AsObject()->GetArrayField(TEXT("conditions"))[0]->AsObject();const int32 Skill=C->GetNumberField(TEXT("value"));
        const bool Wield=C->GetNumberField(TEXT("key"))==159;
        Appraise(Wield?Skill+1:Skill,Wield?Skill:Skill+1);Decision(Profile,TEXT("loot"));
        Appraise(Skill+1,Skill+1);Decision(Profile,TEXT("close_corpse"));
    }
    // Reference IDs plus a custom server value, through UTL import, native
    // appraisal snapshots and UCM. EquipSkill and WieldReqAttribute must differ.
    const TArray<int32> Skills={31,46,44,32,33,45,16,47,54,41,43,34,600};
    for(int32 Key:{218103840,159})for(int32 Skill:Skills)
    {
        auto Req=MakeShared<FJsonObject>();Req->SetNumberField(TEXT("type"),12);
        Req->SetStringField(TEXT("body"),FString::Printf(TEXT("%d\n%d\n"),Skill,Key));
        auto Rule=MakeShared<FJsonObject>();Rule->SetStringField(TEXT("label"),TEXT("Weapon skill"));Rule->SetNumberField(TEXT("action_id"),1);
        Rule->SetStringField(TEXT("expression"),TEXT(""));Rule->SetArrayField(TEXT("requirements"),{MakeShared<FJsonValueObject>(Req)});
        auto Document=MakeShared<FJsonObject>();Document->SetStringField(TEXT("format"),TEXT("utl"));Document->SetArrayField(TEXT("extras"),{});
        Document->SetArrayField(TEXT("rules"),{MakeShared<FJsonValueObject>(Rule)});TArray<FString> Issues;
        auto Read=ACEVTProfile::Read(ACEVTProfile::WriteLoot(Document,Error),TEXT("utl"),Error);
        if(!TestTrue(TEXT("Classic skill rule round trips"),Read.IsValid()))return false;
        auto Profile=ACEVTProfile::Convert(Read,Issues);if(!TestTrue(TEXT("Classic skill rule converts without unsupported requirements"),Profile.IsValid()&&Issues.IsEmpty()))return false;
        const auto C=Profile->GetArrayField(TEXT("loot_rules"))[0]->AsObject()->GetArrayField(TEXT("conditions"))[0]->AsObject();
        TestEqual(TEXT("Import preserves property key"),C->GetNumberField(TEXT("key")),double(Key));
        TestEqual(TEXT("Import preserves skill value"),C->GetNumberField(TEXT("value")),double(Skill));
        TestTrue(TEXT("Import preserves Classic's missing-zero behavior"),C->GetBoolField(TEXT("missing_zero")));
        TestTrue(TEXT("Summary includes skill ID"),ACEUCMLootPresentation::ConditionSummary(C).Contains(FString::FromInt(Skill)));
        Host->Appraisals.Remove(Item.Guid);Decision(Profile,TEXT("identify"));
        Appraise(Key==159?Skill+1:Skill,Key==159?Skill:Skill+1);Decision(Profile,TEXT("loot"));
        Appraise(Skill+1,Skill+1);Decision(Profile,TEXT("close_corpse"));
        Appraise(Skill,Skill,false);Decision(Profile,Key==159?TEXT("loot"):TEXT("close_corpse"));
    }
    return true;
}
#endif
