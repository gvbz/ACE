#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
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
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "ACEDatSubsystem.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"
#include "Mods/ACEUCMLootPresentation.h"
#include "Mods/ACEVTLootPalette.h"
#include "UI/ACEAppraisalFormatting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootBuilderTest,"ACE.Plugins.LootBuilderParity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootBuilderTest::RunTest(const FString&)
{
    auto Parse=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
    auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);Context.SetCurrentWorld(World);Context.OwningGameInstance=GI;GI->OnWorldChanged(nullptr,World);GI->Init();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/LootBuilder")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);IFileManager::Get().DeleteDirectory(*Root,false,true);};
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();Host->StorageRoot=Root;Host->Settings=MakeShared<FJsonObject>();
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    if(!TestTrue(TEXT("Retail DAT loads"),Dat->LoadDatDirectory(ACETestDatDirectory())))return false;
    if(!TestTrue(TEXT("Isolated loot profile creates"),Host->InitializeUCMSelection()&&Host->NewUCMSection(TEXT("Loot"),TEXT("String and weapon choices"))))return false;
    auto Plugin=Host->Find(TEXT("ucm"));
    // Reference keys from the supplied uTank2.LootPlugins enums: no named VT parameter is omitted.
    for(int Id:{2,5,7,10,17,18,19,20,24,25,26,28,30,33,35,36,38,43,45,86,87,88,89,90,91,92,95,98,105,106,107,108,109,110,111,113,114,115,117,125,129,131,133,158,159,160,166,167,170,171,172,174,175,176,177,178,179,181,188,192,193,204,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,240,241,242,243,244,245,246,263,265,270,271,272,280,292,300,302,310,353,362,366,367,369,370,371,374,375,376,379,218103808,218103809,218103810,218103811,218103812,218103813,218103814,218103815,218103816,218103817,218103818,218103819,218103820,218103821,218103822,218103823,218103824,218103825,218103826,218103827,218103828,218103829,218103830,218103831,218103832,218103833,218103834,218103835,218103836,218103837,218103838,218103839,218103840,218103841,218103842,218103843,218103846,218103847,218103848,218103849,218103850,218103851,218103852,218103853,218103854,226492416,226492417,226492418,226492419,226492420,226492421,226492422,226492423,226492424,226492425,226492426,226492427,226492428,226492429,226492430})
        TestTrue(TEXT("Every VT Int property has a named picker entry"),ACEUCMLootPresentation::Properties(TEXT("int")).ContainsByPredicate([Id](const auto& P){return P.Id==Id;}));
    for(int Id:{5,29,87,100,137,144,149,150,152,280,167772160,167772161,167772162,167772163,167772164,167772165,167772166,167772167,167772168,167772169,167772170,167772171,167772172,167772173,167772174})
        TestTrue(TEXT("Every VT Double property has a named picker entry"),ACEUCMLootPresentation::Properties(TEXT("float")).ContainsByPredicate([Id](const auto& P){return P.Id==Id;}));
    for(int Id:{1,5,7,8,10,14,15,16,21,25,35,38,39,40,43,47,184549376})
        TestTrue(TEXT("Every VT String property has a named picker entry"),ACEUCMLootPresentation::Properties(TEXT("string")).ContainsByPredicate([Id](const auto& P){return P.Id==Id;}));
    FACEWorldObject Item;Item.Guid=10;Item.Name=TEXT("Ivory Sword");Item.PluralName=TEXT("Ivory Swords");Item.ItemType=ACEItemType::MeleeWeapon;Item.StackSize=5;
    Item.Structure=3;Item.MaxStructure=10;Item.UseRadius=1.5f;Item.CooldownDuration=30;
    FACEAppraisalInfo Appraisal;Appraisal.bSuccess=Appraisal.bHasWeaponProfile=true;Appraisal.WeaponSkill=45;
    Appraisal.IntProperties.Add(353,1);Appraisal.StringProperties.Add(16,TEXT("A Sword for an Unarmed master [rare]."));
    Appraisal.ArmorResistances={1,2,3,4,5,6,7,8};Host->Appraisals.Add(Item.Guid,Appraisal);
    TestTrue(TEXT("Retail displays attack skill and subtype independently"),ACEAppraisalFormatting::WeaponDetails(Appraisal,Dat).Contains(TEXT("Skill: Light Weapons (Unarmed Weapon)")));
    auto NativeItem=Host->PluginItemSnapshot(Item)->AsObject();
    TestEqual(TEXT("String Name retains singular wire name"),NativeItem->GetObjectField(TEXT("string_properties"))->GetStringField(TEXT("1")),Item.Name);
    TestEqual(TEXT("SecondaryName exports the server plural"),NativeItem->GetObjectField(TEXT("string_properties"))->GetStringField(TEXT("184549376")),Item.PluralName);
    const double ExpectedArmor[]={1,2,3,6,8,5,4};
    for(int I=0;I<7;++I)TestEqual(TEXT("VT armor aliases follow Decal order"),NativeItem->GetObjectField(TEXT("float_properties"))->GetNumberField(FString::FromInt(167772160+I)),ExpectedArmor[I]);
    TestEqual(TEXT("Cooldown is exposed under VT's decimal key"),NativeItem->GetObjectField(TEXT("float_properties"))->GetNumberField(TEXT("280")),30.);
    TestEqual(TEXT("Public structure available without an ID property"),NativeItem->GetObjectField(TEXT("int_properties"))->GetNumberField(TEXT("92")),3.);

    // Deterministically shrink the cooperative quota to exercise resumptions;
    // keep the real sandbox's instruction/time limits in force as well.
    FString Error;const FString Matcher=TEXT("local real=workavailable;local checks=0;workavailable=function() checks=checks+1;return checks%8~=0 and real() end;local scan={} return function(s,p) local m=lootmatch(p.rule,s.item,s,scan);return {matched=m==true,unknown=m==nil,pending=m=='pending'} end");
    auto Match=[&](const TSharedPtr<FJsonObject>& Rule,const TSharedPtr<FJsonObject>& TestedItem,bool Expected,const TCHAR* Label,bool Unknown=false)
    {
        FACEPluginVM VM;if(!TestTrue(TEXT("Shared evaluator loads"),VM.Load(Matcher,Error)))return;
        auto S=MakeShared<FJsonObject>(),P=MakeShared<FJsonObject>();S->SetObjectField(TEXT("item"),TestedItem);P->SetObjectField(TEXT("rule"),Rule);
        TSharedPtr<FJsonObject> Result;
        for(int Step=0;Step<100;++Step){if(!TestTrue(*Error,VM.Step(S,P,Result,Error)))return;if(!Result->GetBoolField(TEXT("pending")))break;}
        TestFalse(TEXT("Bounded scan finishes"),Result->GetBoolField(TEXT("pending")));
        TestEqual(Label,Result->GetBoolField(TEXT("matched")),Expected);TestEqual(TEXT("Unknown appraisal state is preserved"),Result->GetBoolField(TEXT("unknown")),Unknown);
    };
    auto Check=[&](const TCHAR* Rule,bool Expected,const TCHAR* Label){Match(Parse(Rule),NativeItem,Expected,Label);};
    Check(TEXT(R"({"conditions":[{"field":"string","key":16,"pattern":"sword|unarmed","match_mode":"any","ignore_case":true}]})"),true,TEXT("OR text is literal and ignores case when requested"));
    Check(TEXT(R"({"conditions":[{"field":"string","key":16,"pattern":"sword|unarmed","match_mode":"all","ignore_case":true}]})"),true,TEXT("AND text requires both terms"));
    Check(TEXT(R"({"conditions":[{"field":"string","key":16,"pattern":"sword|axe","match_mode":"all","ignore_case":true}]})"),false,TEXT("AND text rejects missing term"));
    Check(TEXT(R"({"conditions":[{"field":"string","key":16,"pattern":"[rare]","match_mode":"contains"}]})"),true,TEXT("Literal mode does not interpret regex punctuation"));
    Check(TEXT(R"({"conditions":[{"field":"string","key":16,"pattern":"sword|unarmed"}]})"),false,TEXT("Existing Classic regex retains case-sensitive semantics"));
    Check(TEXT(R"({"conditions":[{"field":"string","key":16,"pattern":"sword|unarmed","ignore_case":true}]})"),true,TEXT("Regex explicitly supports ignore case"));
    Check(TEXT(R"({"conditions":[{"field":"string","key":1,"pattern":"ivory sword","match_mode":"exact"}]})"),true,TEXT("Exact string match"));
    Check(TEXT(R"({"name":"sword|axe","name_mode":"any"})"),true,TEXT("Basic name filter also accepts OR alternatives"));
    Check(TEXT(R"({"conditions":[{"field":"int","key":353,"op":"in","values":[2,1]}]})"),true,TEXT("Sword OR Unarmed matches subtype independently of skill"));
    auto Or=Parse(TEXT(R"({"conditions_mode":"any","conditions":[{"field":"int","key":353,"op":"eq","value":2},{"field":"int","key":353,"op":"eq","value":1}]})"));
    Match(Or,NativeItem,true,TEXT("OR requirements choose either subtype"));Or->SetStringField(TEXT("conditions_mode"),TEXT("all"));Match(Or,NativeItem,false,TEXT("AND does not accept only one subtype"));
    auto UnknownItem=Parse(TEXT(R"({"name":"Sword","identified":false,"int_properties":{"353":1}})"));
    Or=Parse(TEXT(R"({"conditions_mode":"any","conditions":[{"field":"int","key":9999,"op":"eq","value":2},{"field":"int","key":353,"op":"eq","value":1}]})"));
    Match(Or,UnknownItem,true,TEXT("Known OR success avoids unnecessary appraisal"));
    Or->GetArrayField(TEXT("conditions"))[1]->AsObject()->SetNumberField(TEXT("value"),2);Match(Or,UnknownItem,false,TEXT("Unknown OR alternative waits for appraisal"),true);
    Or->SetStringField(TEXT("conditions_mode"),TEXT("all"));Match(Or,UnknownItem,false,TEXT("Known AND failure avoids unnecessary appraisal"));
    // Force cooperative yields before a late OR match; restart state must not lose unknowns.
    Or=Parse(TEXT(R"({"conditions_mode":"any","conditions":[]})"));TArray<TSharedPtr<FJsonValue>> Many;
    for(int I=0;I<ACEVTProfile::MaxLootConditions-1;++I)Many.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"field":"string","key":1,"pattern":"absent"})"))));
    Many.Add(MakeShared<FJsonValueObject>(Parse(TEXT(R"({"field":"string","key":1,"pattern":"Sword"})"))));Or->SetArrayField(TEXT("conditions"),Many);Match(Or,NativeItem,true,TEXT("OR match survives scan yields"));

    // All Classic color requirement kinds, including original-format import.
    NativeItem->SetArrayField(TEXT("palette_colors"),{MakeShared<FJsonValueNumber>(0xff0000),MakeShared<FJsonValueNumber>(0x0000ff)});
    for(int Type:{14,15,16})
    {
        auto Req=MakeShared<FJsonObject>();Req->SetNumberField(TEXT("type"),Type);
        Req->SetStringField(TEXT("body"),TEXT("255\n0\n0\n10\n0.1\n")+FString(Type==15?TEXT("Amuli Coat (Chest)\n"):Type==16?TEXT("0\n"):TEXT("")));
        auto R=Parse(TEXT(R"({"label":"Color","action_id":1,"expression":"","requirements":[]})"));R->SetArrayField(TEXT("requirements"),{MakeShared<FJsonValueObject>(Req)});
        auto D=Parse(TEXT(R"({"format":"utl","rules":[],"extras":[]})"));D->SetArrayField(TEXT("rules"),{MakeShared<FJsonValueObject>(R)});
        auto Reopened=ACEVTProfile::Read(ACEVTProfile::WriteLoot(D,Error),TEXT("utl"),Error);TArray<FString> Issues;auto Converted=ACEVTProfile::Convert(Reopened,Issues);
        TestTrue(TEXT("Classic color rule imports and validates"),Issues.IsEmpty()&&Host->ValidatePublicProfile(Converted));
        auto Rule=Converted->GetArrayField(TEXT("loot_rules"))[0]->AsObject();Match(Rule,NativeItem,true,TEXT("Color requirement matches correct slot"));
        auto Args=Rule->GetArrayField(TEXT("conditions"))[0]->AsObject()->GetArrayField(TEXT("args"));Args[0]=MakeShared<FJsonValueNumber>(0);Args[1]=MakeShared<FJsonValueNumber>(255);
        Rule->GetArrayField(TEXT("conditions"))[0]->AsObject()->SetArrayField(TEXT("args"),Args);Match(Rule,NativeItem,false,TEXT("Color requirement rejects wrong hue"));
    }
    const auto PaletteIds=Dat->GetPortalDat()->GetFileIds(4);
    if(TestTrue(TEXT("Retail palettes exist"),PaletteIds.Num()>0))
    {
        FACEObjDescSubPalette Palette;Palette.SubPaletteId=PaletteIds[0];Palette.NumColors=8;
        TArray<uint8> Bytes;Dat->GetPortalDat()->ReadFile(PaletteIds[0],Bytes);uint32 Expected=0;
        if(Bytes.Num()>=28)FMemory::Memcpy(&Expected,Bytes.GetData()+24,4);
        TestEqual(TEXT("Native color samples the same DAT byte as VT bb.a"),ACEVTLootPalette::ExampleColor(Dat,Palette),Expected&0xffffffu);
    }
    // Known public alias was previously rejected during conversion.
    auto Secondary=ACEVTProfile::Read(TEXT("UTL\n1\n1\nPlural\n\n0;1;1\n17\nSwords\n184549376\n"),TEXT("utl"),Error);
    if(TestTrue(TEXT("SecondaryName UTL parses"),Secondary.IsValid())){TArray<FString> Issues;auto P=ACEVTProfile::Convert(Secondary,Issues);TestEqual(TEXT("SecondaryName imports"),Issues.Num(),0);Match(P->GetArrayField(TEXT("loot_rules"))[0]->AsObject(),NativeItem,true,TEXT("Imported plural rule actually matches"));}
    auto Invalid=Parse(TEXT(R"({"loot_rules":[{"conditions_mode":"xor"}]})"));TestFalse(TEXT("Invalid logical mode is rejected"),Host->ValidatePublicProfile(Invalid));
    Invalid=Parse(TEXT(R"({"loot_rules":[{"conditions":[{"field":"int","key":353,"op":"in","values":[]}]}]})"));TestFalse(TEXT("Empty numeric OR set is rejected"),Host->ValidatePublicProfile(Invalid));
    auto LongText=FString::ChrN(1024,TEXT('x'))+TEXT("| sword | | axe |");
    auto LongRule=Parse(TEXT(R"({"conditions":[{"field":"string","key":1,"match_mode":"any"}]})"));
    LongRule->GetArrayField(TEXT("conditions"))[0]->AsObject()->SetStringField(TEXT("pattern"),LongText);
    Match(LongRule,NativeItem,true,TEXT("Long literal alternatives, whitespace and empty terms stay bounded"));
    FACEPluginVM BoundsVM;TSharedPtr<FJsonObject> BoundsResult;
    TestTrue(TEXT("Literal search is bounded, one-based and not a Lua pattern"),BoundsVM.Load(TEXT("return function() return {ok=string.findplain('a.[b]','.[b]')==2 and string.findplain('aba','a',2)==3 and string.findplain('abc','x')==nil and string.find==nil} end"),Error)&&BoundsVM.Step(MakeShared<FJsonObject>(),MakeShared<FJsonObject>(),BoundsResult,Error)&&BoundsResult->GetBoolField(TEXT("ok")));
    FACEPluginVM TooLongVM;
    TestTrue(TEXT("Oversize search fixture loads"),TooLongVM.Load(TEXT("return function() return {ok=string.findplain(string.rep('x',16385),'x')} end"),Error));
    TestFalse(TEXT("Literal search rejects oversize input"),TooLongVM.Step(MakeShared<FJsonObject>(),MakeShared<FJsonObject>(),BoundsResult,Error));
    Error.Reset();
    auto SpellDocument=Parse(TEXT(R"({"format":"utl","rules":[{"label":"Spells","action_id":1,"expression":"","requirements":[{"type":9,"body":"Major\n \n0\n"}]}],"extras":[]})"));
    TArray<FString> SpellIssues;auto SpellProfile=ACEVTProfile::Convert(SpellDocument,SpellIssues);
    TestTrue(TEXT("Classic zero-count SpellMatch imports"),SpellIssues.IsEmpty()&&Host->ValidatePublicProfile(SpellProfile));
    auto SpellRule=SpellProfile->GetArrayField(TEXT("loot_rules"))[0]->AsObject();
    Match(SpellRule,NativeItem,false,TEXT("Classic SpellMatch still needs one match when Count is zero"));
    NativeItem->SetArrayField(TEXT("spell_names"),{MakeShared<FJsonValueString>(TEXT("Major Strength"))});
    Match(SpellRule,NativeItem,true,TEXT("Classic ignores whitespace-only spell exclusions"));

    if(FApp::CanEverRender()&&FSlateApplication::IsInitialized())
    {
        auto Panel=Host->MakeUCMPanel(TEXT("Standalone loot"));TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName)> Find;
        Find=[&](TSharedRef<SWidget> W,FName Tag)->TSharedPtr<SWidget>{if(W->GetTag()==Tag)return W;auto* Children=W->GetChildren();for(int I=0;I<Children->Num();++I)if(auto R=Find(Children->GetChildAt(I),Tag))return R;return nullptr;};
        auto Click=[&](const TCHAR* Tag){auto W=Find(Panel,Tag);if(!TestTrue(Tag,W.IsValid()))return false;StaticCastSharedPtr<SButton>(W)->SimulateClick();return true;};
        auto& Slate=FSlateApplication::Get();const auto Focus=Slate.GetUserFocusedWidget(0);const FVector2D Size(780,1100);
        auto Window=SNew(SWindow).ClientSize(Size)[Panel];Slate.AddWindow(Window,false);FWidgetRenderer Renderer(false,true);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        ON_SCOPE_EXIT {Slate.DismissAllMenus();if(Focus)Slate.SetUserFocus(0,Focus);Window->RequestDestroyWindow();Target->ReleaseResource();};
        auto Frame=[&](){++GFrameCounter;Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();};
        auto Choose=[&](const TCHAR* Tag,const TCHAR* Caption)
        {
            Frame();Frame();auto W=Find(Panel,Tag);if(!TestTrue(Tag,W.IsValid()))return false;auto Combo=StaticCastSharedPtr<SComboButton>(W);Combo->SetIsOpen(true);Frame();auto Menu=Combo->GetMenuWindow();
            if(!TestTrue(TEXT("Menu opens"),Menu.IsValid()))return false;auto Entry=Find(Menu.ToSharedRef(),FName(*(FString(TEXT("UCMMenu."))+Caption)));if(!TestTrue(Caption,Entry.IsValid()))return false;StaticCastSharedPtr<SButton>(Entry)->SimulateClick();return true;
        };
        if(!Click(TEXT("UCMLootTool.New rule"))||!Click(TEXT("UCMLootTool.Weapon type")))return false;
        auto Unarmed=StaticCastSharedPtr<SCheckBox>(Find(Panel,TEXT("UCMLootValue.1")));if(!TestTrue(TEXT("Unarmed checkbox exists"),Unarmed.IsValid()))return false;Unarmed->ToggleCheckedState();
        if(!Click(TEXT("UCMChoice.Apply requirement"))||!Click(TEXT("UCMLootTool.New requirement")))return false;
        if(!Choose(TEXT("UCMLootType"),TEXT("Text / name"))||!Choose(TEXT("UCMLootTextMode"),TEXT("OR - any text separated by |")))return false;
        auto Pattern=StaticCastSharedPtr<SEditableTextBox>(Find(Panel,TEXT("UCMLootPattern")));
        if(!TestTrue(TEXT("String input exists"),Pattern.IsValid()))return false;Pattern->SetText(FText::FromString(TEXT("sword|unarmed")));
        TFunction<TSharedPtr<SScrollBox>(TSharedRef<SWidget>)> FindScroll;
        FindScroll=[&](TSharedRef<SWidget> W)->TSharedPtr<SScrollBox>{if(W->GetType()==TEXT("SScrollBox"))return StaticCastSharedRef<SScrollBox>(W);auto* C=W->GetChildren();for(int I=0;I<C->Num();++I)if(auto R=FindScroll(C->GetChildAt(I)))return R;return nullptr;};
        for(int Width:{780,450})
        {
            const FVector2D CaptureSize(Width,1100);auto* Capture=FWidgetRenderer::CreateTargetFor(CaptureSize,TF_Bilinear,true);
            auto Draw=[&](){++GFrameCounter;Renderer.DrawWidget(Capture,Panel,CaptureSize,0);FlushRenderingCommands();};
            for(int Pass=0;Pass<5;++Pass)Draw();
            if(auto Page=FindScroll(Panel))Page->SetScrollOffset(Width<500?650:80);
            // Responsive layout rebuilds the controls when switching widths.
            Pattern=StaticCastSharedPtr<SEditableTextBox>(Find(Panel,TEXT("UCMLootPattern")));
            if(!TestTrue(TEXT("Responsive string input exists"),Pattern.IsValid()))return false;
            if(auto Detail=Find(Panel,TEXT("LootRuleDetail")))if(auto Scroll=FindScroll(Detail.ToSharedRef()))Scroll->ScrollDescendantIntoView(Pattern,false,EDescendantScrollDestination::Center);
            for(int Pass=0;Pass<4;++Pass)Draw();
            TestTrue(TEXT("String input remains usable at narrow width"),Pattern->GetCachedGeometry().GetLocalSize().X>=200);
            TArray<FColor> Pixels;Capture->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> PNG;
            FImageUtils::PNGCompressImageArray(Width,1100,Pixels,PNG);FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("LootBuilder-String-%d.png"),Width)));Capture->ReleaseResource();
        }
        if(!Click(TEXT("UCMChoice.Apply requirement"))||!Choose(TEXT("UCMLootChoice.conditions_mode"),TEXT("AND - all requirements"))||!Click(TEXT("UCMLootTool.Apply rule")))return false;
        if(!TestTrue(TEXT("Profile reloads from the saved file"),Host->InitializeUCMSelection()))return false;
        TestEqual(TEXT("Reload selects saved profile"),Host->UCMSectionName(TEXT("Loot")),FString(TEXT("String and weapon choices")));
        const auto& Saved=Plugin->Profile->GetArrayField(TEXT("loot_rules"));if(!TestEqual(TEXT("UI created one rule"),Saved.Num(),1))return false;
        const auto SavedRule=Saved[0]->AsObject();const auto& Conditions=SavedRule->GetArrayField(TEXT("conditions"));
        if(!TestEqual(TEXT("Both UI requirements persist"),Conditions.Num(),2))return false;
        TestEqual(TEXT("Weapon values retain both Sword and Unarmed"),Conditions[0]->AsObject()->GetArrayField(TEXT("values")).Num(),2);
        TestEqual(TEXT("String alternatives retain matching mode"),Conditions[1]->AsObject()->GetStringField(TEXT("match_mode")),FString(TEXT("any")));
        Match(SavedRule,NativeItem,true,TEXT("Saved AND rule executes with nested Sword OR Unarmed choice"));
        auto Mace=ACEUCMLootPresentation::Clone(NativeItem);Mace->GetObjectField(TEXT("int_properties"))->SetNumberField(TEXT("353"),4);
        Match(SavedRule,Mace,false,TEXT("Saved rule rejects another weapon subtype even with matching name"));
        // The actual shipped UCM policy consumes the UI-created rule and native item snapshot.
        FString UCM;FFileHelper::LoadFileToString(UCM,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
        auto State=Parse(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"mana":100,"max_mana":100,"inventory":[],"container":9,"container_is_corpse":true,"contents_ready":true})"));
        State->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(NativeItem)});
        auto Profile=Parse(TEXT(R"({"looting":true,"buffing":false,"recovery":false,"combat":"off"})"));Profile->SetArrayField(TEXT("loot_rules"),Saved);
        FACEPluginVM VM;TSharedPtr<FJsonObject> Intent;
        if(TestTrue(TEXT("Shipped UCM executes saved profile"),VM.Load(UCM,Error)&&VM.Step(State,Profile,Intent,Error)&&Intent.IsValid()))
            TestEqual(TEXT("UI rule results in an actual loot intent"),Intent->GetStringField(TEXT("action")),FString(TEXT("loot")));
    }
    return true;
}
#endif
