#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Mods/ACEUCMProfiles.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Misc/App.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SWindow.h"
#include "Widgets/SVirtualWindow.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMProfileStorageTest, "ACE.Plugins.LinkedProfiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEUCMProfileStorageTest::RunTest(const FString&)
{
    using namespace ACEUCMProfiles;
    TestTrue(TEXT("Profile names support spaces"),SafeName(TEXT("Academy Loop")));
    for(const FString Name:{TEXT("../Escape"),TEXT("sub/route"),TEXT("sub\\route"),TEXT(""),TEXT(" Leading"),TEXT("Trailing "),TEXT("CON"),TEXT("nul"),TEXT("COM1"),TEXT("LPT9")})
        TestFalse(*FString::Printf(TEXT("Unsafe profile filename rejected: %s"),*Name),SafeName(Name));
    const FString Root = FPaths::ProjectSavedDir()/TEXT("Automation/UCMProfiles")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*(Root/TEXT("Profiles/ucm")), true);
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    const FString Legacy = TEXT(R"({"buffing":true,"combat":"auto","skill_margin":30,"summon_pets":true,"route":[{"cell":100,"x":1,"y":2,"z":3}],"navigation":true,"loot_rules":[{"label":"Copper","material":59}],"salvage_policy":{"default":[{"min":7,"max":10}]},"monsters":[{"name":"Rat","damage_type":1}],"states":[{"name":"Default"}],"vendor_rules":[{"item_name":"Scarab","quantity":100}],"future_character_option":42})");
    const FString Setup = Root/TEXT("Profiles/ucm/First.json");
    FFileHelper::SaveStringToFile(Legacy,*Setup);
    TSharedPtr<FJsonObject> Profile,Links; FString Error;
    TestTrue(TEXT("Existing monolithic setup loads"),Load(Root,TEXT("First"),Profile,Links,Error));
    auto Original=Copy(Profile);
    TestTrue(TEXT("Migration writes linked setup"),Save(Root,TEXT("First"),Profile,Links,nullptr,Error));
    FString Backup;FFileHelper::LoadFileToString(Backup,*(Root/TEXT("Profiles/Legacy/ucm/First.json")));
    TSharedPtr<FJsonObject> BackupObject;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Backup),BackupObject);
    TSharedPtr<FJsonObject> OriginalOnDisk;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Legacy),OriginalOnDisk);
    TestTrue(TEXT("Migration preserves all original data in backup"),BackupObject && FJsonValue::CompareEqual(FJsonValueObject(OriginalOnDisk),FJsonValueObject(BackupObject)));
    TestEqual(TEXT("All six sections have references"),Links->Values.Num(),6);
    TSharedPtr<FJsonObject> Reload,ReloadLinks;
    TestTrue(TEXT("Linked setup resolves"),Load(Root,TEXT("First"),Reload,ReloadLinks,Error));
    TestTrue(TEXT("Every setting survives migration including unknown future options"),FJsonValue::CompareEqual(FJsonValueObject(Original),FJsonValueObject(Reload)));
    TestFalse(TEXT("Character file does not contain route data"),Extract(Reload,TEXT("Character"))->HasField(TEXT("route")));
    TestTrue(TEXT("Run activity flags remain independent"),Extract(Reload,TEXT("ucm"))->HasField(TEXT("navigation")));
    auto Baseline=Copy(Profile);
    Profile->SetBoolField(TEXT("navigation"),false);
    TestTrue(TEXT("Second setup shares existing profiles"),Save(Root,TEXT("Second"),Profile,Links,Baseline,Error));
    Baseline=Copy(Profile);
    Profile->SetNumberField(TEXT("skill_margin"),12);
    Profile->GetArrayField(TEXT("vendor_rules"))[0]->AsObject()->SetNumberField(TEXT("quantity"),200);
    TestTrue(TEXT("Character and vendor edits save through shared references"),Save(Root,TEXT("Second"),Profile,Links,Baseline,Error));
    TestTrue(TEXT("First setup picks up shared changes"),Load(Root,TEXT("First"),Reload,ReloadLinks,Error));
    TestEqual(TEXT("Character changes propagate"),Reload->GetNumberField(TEXT("skill_margin")),12.);
    TestEqual(TEXT("Vendor changes propagate"),Reload->GetArrayField(TEXT("vendor_rules"))[0]->AsObject()->GetNumberField(TEXT("quantity")),200.);
    TestTrue(TEXT("Shared edits do not turn off another setup's navigation"),Reload->GetBoolField(TEXT("navigation")));
    TestTrue(TEXT("Create a separate route variation"),SaveSectionAs(Root,TEXT("Routes"),TEXT("Cave"),Profile,Error));
    TestFalse(TEXT("Save copy cannot overwrite a profile"),SaveSectionAs(Root,TEXT("Routes"),TEXT("Cave"),Profile,Error));
    TestTrue(TEXT("Swap only route reference"),Select(Root,TEXT("Routes"),TEXT("Cave"),Profile,Links,Error));
    Baseline=Copy(Profile);
    Profile->GetArrayField(TEXT("route"))[0]->AsObject()->SetNumberField(TEXT("x"),9);
    TestTrue(TEXT("Route variation saves independently"),Save(Root,TEXT("Second"),Profile,Links,Baseline,Error));
    Load(Root,TEXT("First"),Reload,ReloadLinks,Error);
    TestEqual(TEXT("Original route is unchanged"),Reload->GetArrayField(TEXT("route"))[0]->AsObject()->GetNumberField(TEXT("x")),1.);
    // Simulate an editor holding an old snapshot while another setup changes shared settings.
    auto Stale=Copy(Reload),StaleLinks=Copy(ReloadLinks),StaleBase=Copy(Reload);
    auto FreshBase=Copy(Reload);Reload->SetNumberField(TEXT("skill_margin"),5);
    Save(Root,TEXT("First"),Reload,ReloadLinks,FreshBase,Error);
    Stale->SetBoolField(TEXT("navigation"),false);
    TestTrue(TEXT("Editing a run flag does not overwrite a newer shared file"),Save(Root,TEXT("First"),Stale,StaleLinks,StaleBase,Error));
    TestEqual(TEXT("Unchanged shared section refreshes from disk"),Stale->GetNumberField(TEXT("skill_margin")),5.);
    Stale=Copy(StaleBase);Stale->SetNumberField(TEXT("skill_margin"),99);
    TestFalse(TEXT("Conflicting stale edits require reload"),Save(Root,TEXT("First"),Stale,StaleLinks,StaleBase,Error));
    Load(Root,TEXT("First"),Reload,ReloadLinks,Error);
    TestEqual(TEXT("Rejected stale edit leaves shared data intact"),Reload->GetNumberField(TEXT("skill_margin")),5.);
    const auto Unchanged=Copy(Reload);
    TestFalse(TEXT("Missing selection does not silently fall back"),Select(Root,TEXT("Loot"),TEXT("missing"),Reload,ReloadLinks,Error));
    TestTrue(TEXT("Failed selection preserves current data"),FJsonValue::CompareEqual(FJsonValueObject(Reload),FJsonValueObject(Unchanged)));
    TestFalse(TEXT("Path traversal rejected"),Select(Root,TEXT("Loot"),TEXT("../Character/First"),Reload,ReloadLinks,Error));
    FFileHelper::SaveStringToFile(TEXT("{\"skill_margin\":1}"),*(Root/TEXT("Profiles/Loot/Wrong.json")));
    TestFalse(TEXT("Wrong section contents cannot overwrite character settings"),Select(Root,TEXT("Loot"),TEXT("Wrong"),Reload,ReloadLinks,Error));
    FFileHelper::SaveStringToFile(TEXT("not json"),*(Root/TEXT("Profiles/Routes/Cave.json")));
    TestFalse(TEXT("Corrupt linked file prevents partial setup activation"),Load(Root,TEXT("Second"),Reload,ReloadLinks,Error));
    TestTrue(TEXT("Failed load keeps previously resolved profile"),FJsonValue::CompareEqual(FJsonValueObject(Reload),FJsonValueObject(Unchanged)));
    TestEqual(TEXT("Protections belong to Character"),SectionFor(TEXT("protection_profile")),FString(TEXT("Character")));
    TestEqual(TEXT("Custom banes belong to Character"),SectionFor(TEXT("bane_custom")),FString(TEXT("Character")));
    {
        TSharedPtr<FJsonObject> Native=MakeShared<FJsonObject>();Native->SetNumberField(TEXT("damage_type"),8);
        auto Mob=MakeShared<FJsonObject>();Mob->SetStringField(TEXT("name"),TEXT("Rat"));Mob->SetNumberField(TEXT("damage_type"),16);
        Native->SetArrayField(TEXT("monsters"),{MakeShared<FJsonValueObject>(Mob)});
        TSharedPtr<FJsonObject> NativeLinks,Loaded,LoadedLinks;
        TestTrue(TEXT("Native profile without format markers saves"),Save(Root,TEXT("Native"),Native,NativeLinks,nullptr,Error));
        auto NativeBase=Copy(Native);Native->SetNumberField(TEXT("skill_margin"),45);
        TestTrue(TEXT("Next save does not report a migration as a disk conflict"),Save(Root,TEXT("Native"),Native,NativeLinks,NativeBase,Error));
        TestTrue(TEXT("Native profile reloads"),Load(Root,TEXT("Native"),Loaded,LoadedLinks,Error));
        TestEqual(TEXT("Native cold damage stays cold after reload"),Loaded->GetNumberField(TEXT("damage_type")),8.);
        TestEqual(TEXT("Native mob fire damage stays fire after reload"),Loaded->GetArrayField(TEXT("monsters"))[0]->AsObject()->GetNumberField(TEXT("damage_type")),16.);
    }
    {
        auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT{GI->Shutdown();};
        auto* H=GI->GetSubsystem<UACEPluginSubsystem>();H->StorageRoot=Root/TEXT("Host");H->Settings=MakeShared<FJsonObject>();
        TestTrue(TEXT("Initial selection creates Default references"),H->InitializeUCMSelection());
        auto P=H->Find(TEXT("ucm"));if(!P)return false;
        for(const auto& Section:Sections())TestEqual(TEXT("New setup selects Default in every section"),H->UCMSectionName(Section),FString(TEXT("Default")));
        TestTrue(TEXT("New Character profile"),H->NewUCMSection(TEXT("Character"),TEXT("Hero")));
        P->Profile->SetNumberField(TEXT("protection_profile"),4);P->Profile->SetStringField(TEXT("bane_custom"),TEXT("BPS"));
        TestTrue(TEXT("Save current setup"),H->SaveProfile(P->Id,P->ProfileName,H->ProfileJson(P->Id),false));
        TestTrue(TEXT("New route creates empty file"),H->NewUCMSection(TEXT("Routes"),TEXT("OldPath")));
        auto Point=MakeShared<FJsonObject>();Point->SetNumberField(TEXT("cell"),0x7d640001);Point->SetNumberField(TEXT("x"),1);Point->SetNumberField(TEXT("y"),2);Point->SetNumberField(TEXT("z"),3);
        P->Profile->SetArrayField(TEXT("route"),{MakeShared<FJsonValueObject>(Point)});
        TestTrue(TEXT("Save populated route"),H->SaveProfile(P->Id,P->ProfileName,H->ProfileJson(P->Id),false));
        TestTrue(TEXT("Another new route starts blank"),H->NewUCMSection(TEXT("Routes"),TEXT("Blank")));
        TestEqual(TEXT("New route never copies previous waypoints"),P->Profile->GetArrayField(TEXT("route")).Num(),0);
        TestEqual(TEXT("Route creation preserves Character reference"),H->UCMSectionName(TEXT("Character")),FString(TEXT("Hero")));
        TestFalse(TEXT("New cannot overwrite an existing profile"),H->NewUCMSection(TEXT("Routes"),TEXT("OldPath")));
        // Exercise the same controls a user clicks, including the immediate
        // on-disk file and current selection, then actual position recording.
        auto Panel=H->MakeUCMPanel(TEXT("Route"));
        TFunction<TSharedPtr<SWidget>(const TSharedRef<SWidget>&,FName)> FindTag;
        FindTag=[&](const TSharedRef<SWidget>& W,FName Tag)->TSharedPtr<SWidget>{
            if(W->GetTag()==Tag)return W;
            auto Children=W->GetChildren();for(int I=0;I<Children->Num();++I)if(auto Found=FindTag(Children->GetChildAt(I),Tag))return Found;
            return nullptr;};
        auto Click=[&](const TCHAR* Caption){auto W=FindTag(Panel,FName(*(FString(TEXT("UCMChoice."))+Caption)));
            if(!TestTrue(Caption,W.IsValid()))return false;StaticCastSharedPtr<SButton>(W)->SimulateClick();return true;};
        if(!Click(TEXT("+ New route")))return false;
        auto NameInput=StaticCastSharedPtr<SEditableTextBox>(FindTag(Panel,TEXT("UCMNewProfileName")));
        if(!TestTrue(TEXT("Route name field is shown"),NameInput.IsValid()))return false;
        NameInput->SetText(FText::FromString(TEXT("Academy_Loop")));Click(TEXT("Create and select route"));
        TestEqual(TEXT("Create selects the new route immediately"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Academy_Loop")));
        TestTrue(TEXT("Empty route file exists before any point is added"),IFileManager::Get().FileExists(*(H->UserDirectory()/TEXT("Profiles/Routes/Academy_Loop.json"))));
        TestTrue(TEXT("Creation confirmation names the saved file"),H->Notice.Contains(TEXT("Created and selected Routes: Academy_Loop.json")));
        TestTrue(TEXT("New route is the persisted selection"),H->InitializeUCMSelection());
        TestEqual(TEXT("Restart keeps new empty route selected"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Academy_Loop")));
        auto Session=GI->GetSubsystem<UACEClientSubsystem>()->GetSession();Session->State=EACESessionState::InWorld;
        for(int I=0;I<10;++I)
        {
            FACEPosition Pos;Pos.CellId=0x7D640001;Pos.Location={double(I),2,3};Session->SetLocalPosition(Pos);
            if(!Click(TEXT("+ Add waypoint at my position")))return false;
        }
        TestEqual(TEXT("One click adds one waypoint"),P->Profile->GetArrayField(TEXT("route")).Num(),10);
        TestTrue(TEXT("Last point is visible after pagination"),FindTag(Panel,TEXT("UCMChoice.Previous")).IsValid());
        TestTrue(TEXT("Recorded route reloads without a Save button"),H->InitializeUCMSelection());
        TestEqual(TEXT("All waypoints persisted"),P->Profile->GetArrayField(TEXT("route")).Num(),10);
        TestEqual(TEXT("Recording uses current player position"),P->Profile->GetArrayField(TEXT("route"))[9]->AsObject()->GetNumberField(TEXT("x")),9.);
        Session->State=EACESessionState::Disconnected;
        H->SelectUCMSection(TEXT("Routes"),TEXT("OldPath"));TestEqual(TEXT("Original route remains intact"),P->Profile->GetArrayField(TEXT("route")).Num(),1);
        const FString Folder=H->UserDirectory()/TEXT("ImportInbox");IFileManager::Get().MakeDirectory(*Folder,true);
        const FString Nav=TEXT("uTank2 NAV 1.2\n2\n2\n0\n0\n0\n0.05\n0\n3\n0.01\n0\n0.05\n0\n1500\n");
        FFileHelper::SaveStringToFile(Nav,*(Folder/TEXT("Loop.nav")));
        TestTrue(TEXT("Folder import creates route section"),H->ImportUCMSection(TEXT("Loop.nav")));
        TestEqual(TEXT("Import keeps a single current selection"),P->ProfileName,ActiveName());
        TestEqual(TEXT("Import uses source file stem"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Loop")));
        TestEqual(TEXT("Imported points loaded for world preview"),P->Profile->GetArrayField(TEXT("route")).Num(),2);
        TestTrue(TEXT("Imported route has world preview"),P->Profile->GetBoolField(TEXT("route_preview")));
        P->Profile->GetArrayField(TEXT("route"))[0]->AsObject()->SetNumberField(TEXT("x"),42);
        TestTrue(TEXT("Save native route edits"),H->SaveProfile(P->Id,P->ProfileName,H->ProfileJson(P->Id),false));
        H->SelectUCMSection(TEXT("Routes"),TEXT("Default"));
        TestTrue(TEXT("Repeated import selects existing file"),H->ImportUCMSection(TEXT("Loop.nav")));
        TestEqual(TEXT("Repeated import retains name"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Loop")));
        TestEqual(TEXT("Reimport preserves native edits"),P->Profile->GetArrayField(TEXT("route"))[0]->AsObject()->GetNumberField(TEXT("x")),42.);
        TestFalse(TEXT("No numbered duplicate"),H->UCMSectionProfiles(TEXT("Routes")).Contains(TEXT("Loop_2")));
        FString AfterImport;FFileHelper::LoadFileToString(AfterImport,*(Folder/TEXT("Loop.nav")));TestEqual(TEXT("Import leaves original nav unchanged"),AfterImport,Nav);
        FFileHelper::SaveStringToFile(TEXT("UTL\r\n1\r\n1\r\nCopper\r\n\r\n0;1;12\r\n8\r\n6\r\n131\r\n"),*(Folder/TEXT("Copper.utl")));
        TestTrue(TEXT("Folder import creates Loot section"),H->ImportUCMSection(TEXT("Copper.utl")));
        TestEqual(TEXT("Loot import keeps selected route"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Loop")));
        TestEqual(TEXT("Loot import names active profile"),H->UCMSectionName(TEXT("Loot")),FString(TEXT("Copper")));
        TestEqual(TEXT("Loot rules loaded"),P->Profile->GetArrayField(TEXT("loot_rules")).Num(),1);
        FFileHelper::SaveStringToFile(TEXT("bad nav"),*(Folder/TEXT("Broken.nav")));
        TestFalse(TEXT("Broken import rejected without changing selected profile"),H->ImportUCMSection(TEXT("Broken.nav")));
        TestEqual(TEXT("Failed import preserves active route"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Loop")));
        TestTrue(TEXT("Selection reloads across restart"),H->InitializeUCMSelection());
        TestEqual(TEXT("Restart retains chosen route"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Loop")));
        TestEqual(TEXT("No new named setups are created"),List(H->UserDirectory(),TEXT("ucm")).Num(),0);
        TestTrue(TEXT("Delete referenced route"),H->DeleteUCMSection(TEXT("Routes"),TEXT("Loop")));
        TestEqual(TEXT("Active setup switches deleted route to Default"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Default")));
        TestFalse(TEXT("Deleted profile leaves chooser"),H->UCMSectionProfiles(TEXT("Routes")).Contains(TEXT("Loop")));
        TestTrue(TEXT("Current selection remains loadable"),H->LoadProfile(TEXT("ucm"),ActiveName()));
        TestEqual(TEXT("Current selection references Default after delete"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("Default")));
        TestEqual(TEXT("Delete leaves unrelated Character intact"),H->UCMSectionName(TEXT("Character")),FString(TEXT("Hero")));
        TestEqual(TEXT("Protections survive setup switches"),P->Profile->GetNumberField(TEXT("protection_profile")),4.);
        TestEqual(TEXT("Custom banes survive setup switches"),P->Profile->GetStringField(TEXT("bane_custom")),FString(TEXT("BPS")));
        TArray<FString> Backups;IFileManager::Get().FindFiles(Backups,*(H->UserDirectory()/TEXT("Profiles/Deleted/Routes/*.json")),true,false);
        TestEqual(TEXT("Deleted file is recoverable"),Backups.Num(),1);
        TestFalse(TEXT("Default cannot be deleted"),H->DeleteUCMSection(TEXT("Routes"),TEXT("Default")));
        TestTrue(TEXT("Active selection is saved outside legacy setup folder"),IFileManager::Get().FileExists(*(H->UserDirectory()/TEXT("ucm-selection.json"))));
        auto Anonymous=Copy(P->Profile);Anonymous->SetStringField(TEXT("nav_source"),Folder/TEXT("SourcePath.nav"));
        TestTrue(TEXT("Create old anonymous import fixture"),SaveSectionAs(H->UserDirectory(),TEXT("Routes"),TEXT("Imported"),Anonymous,Error));
        H->SelectUCMSection(TEXT("Routes"),TEXT("Imported"));
        TestTrue(TEXT("Anonymous import migrates on login"),H->InitializeUCMSelection());
        TestEqual(TEXT("Anonymous route adopts its actual filename"),H->UCMSectionName(TEXT("Routes")),FString(TEXT("SourcePath")));
        TestFalse(TEXT("Anonymous placeholder leaves dropdown"),H->UCMSectionProfiles(TEXT("Routes")).Contains(TEXT("Imported")));
        H->StorageRoot=Root/TEXT("Migration");H->Settings=MakeShared<FJsonObject>();
        IFileManager::Get().MakeDirectory(*(H->StorageRoot/TEXT("Profiles/ucm")),true);
        auto LegacyProfile=Copy(P->Profile);LegacyProfile->SetNumberField(TEXT("skill_margin"),27);FString LegacyText;
        FJsonSerializer::Serialize(LegacyProfile.ToSharedRef(),TJsonWriterFactory<>::Create(&LegacyText));
        FFileHelper::SaveStringToFile(LegacyText,*(H->StorageRoot/TEXT("Profiles/ucm/LastHunt.json")));
        H->Settings->SetStringField(TEXT("ucm.profile"),TEXT("LastHunt"));
        TestTrue(TEXT("Last selected legacy setup migrates automatically"),H->InitializeUCMSelection());
        TestEqual(TEXT("Migration retains character settings"),P->Profile->GetNumberField(TEXT("skill_margin")),27.);
        TestEqual(TEXT("Migrated Character is independently selectable"),H->UCMSectionName(TEXT("Character")),FString(TEXT("LastHunt")));
        TestTrue(TEXT("Legacy original is backed up"),IFileManager::Get().FileExists(*(H->StorageRoot/TEXT("Profiles/Legacy/ucm/LastHunt.json"))));
        TestEqual(TEXT("Migration saves active selection key"),P->ProfileName,ActiveName());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMProfilePanelTest, "ACE.Plugins.LinkedProfilePanel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEUCMProfilePanelTest::RunTest(const FString&)
{
    auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT { GI->Shutdown(); };
    if(FApp::CanEverRender())
    {
        FString Directory=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
        if(!TestTrue(TEXT("Load retail artwork for profile panel captures"),GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(Directory)))return false;
    }
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    auto CollectText=[](const TSharedRef<SWidget>& Root)
    {
        FString Result;TFunction<void(const TSharedRef<SWidget>&)> Visit;
        Visit=[&](const TSharedRef<SWidget>& Widget){
            if((Widget->GetType()==FName(TEXT("STextBlock"))||Widget->GetType()==FName(TEXT("SACEPluginText"))))Result+=StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString()+TEXT("\n");
            auto Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)Visit(Children->GetChildAt(I));
        };Visit(Root);return Result;
    };
    for(const auto& Plugin:Host->Plugins)if(Plugin->Id==TEXT("ucm"))
    {
        Plugin->ProfileName=TEXT("Olthoi_Hunt");Plugin->ProfileLinks=MakeShared<FJsonObject>();
        for(const auto& Section:ACEUCMProfiles::Sections())Plugin->ProfileLinks->SetStringField(Section,
            Section==TEXT("Character")?TEXT("PlayerNameVOID"):Section==TEXT("Routes")?TEXT("Olthoi120"):TEXT("Shared"));
    }
    for(const TCHAR* Page:{TEXT("Buffs"),TEXT("Buff others"),TEXT("Combat"),TEXT("Recovery"),TEXT("Route"),TEXT("Loot"),TEXT("Vendors"),TEXT("Monsters"),TEXT("Metas")})
    {
        const FString Text=CollectText(Host->MakeUCMPanel(Page));
        TestTrue(*(FString(Page)+TEXT(" has New profile")),Text.Contains(FString(Page)==TEXT("Route")?TEXT("+ New route\n"):TEXT("+ New\n")));
        TestTrue(*(FString(Page)+TEXT(" has Delete profile")),Text.Contains(TEXT("Delete\n")));
        if(FString(Page)==TEXT("Route"))TestFalse(TEXT("Route has no duplicate import selector"),Text.Contains(TEXT("Navigation route\n")));
        if(FString(Page)==TEXT("Monsters"))TestTrue(TEXT("Mobs can add current target"),Text.Contains(TEXT("Add selected mob")));
        if(FString(Page)==TEXT("Buffs"))TestTrue(TEXT("Buffs includes protection choices"),Text.Contains(TEXT("Protections:"))&&Text.Contains(TEXT("Banes:")));
        for(const TCHAR* Caption:{TEXT("Enable Combat\n"),TEXT("Enable Summoning\n"),TEXT("Enable Buffs\n"),TEXT("Enable Recovery\n"),TEXT("Accept buff requests\n"),TEXT("Enable Route Navigation\n"),TEXT("Enable Vendor Restocking\n"),TEXT("Run imported meta\n"),TEXT("Loot corpses\n")})
            TestFalse(*(FString(Page)+TEXT(" has no duplicate activity control: ")+Caption),Text.Contains(Caption));
    }
    const FString Overview=CollectText(Host->MakeUCMPanel(TEXT("Overview")));
    TestFalse(TEXT("Overview no longer includes Open Log"),Overview.Contains(TEXT("Open UCM Log")));
    TestFalse(TEXT("Overview no longer includes protection selectors"),Overview.Contains(TEXT("Protections:"))||Overview.Contains(TEXT("Banes:")));
    TestTrue(TEXT("Overview calls section Configuration"),Overview.Contains(TEXT("Configuration")));
    TestTrue(TEXT("Overview keeps the master Combat and Summoning controls"),Overview.Contains(TEXT("Enable Combat\n"))&&Overview.Contains(TEXT("Enable Summoning\n")));
    TestTrue(TEXT("Overview keeps buff request control"),Overview.Contains(TEXT("Accept buff requests\n")));
    TestFalse(TEXT("Loot notification settings have one home in Loot"),Overview.Contains(TEXT("Announce item ID matches\n")));
    TestFalse(TEXT("Overview removes setup header and plugin bar preference"),Overview.Contains(TEXT("Setup:"))||Overview.Contains(TEXT("Show plugin bar")));
    for(const auto& Section:ACEUCMProfiles::Sections())TestTrue(TEXT("Overview contains all six profile selectors"),
        Overview.Contains(Section==TEXT("Monsters")?TEXT("Mobs\nShared\n"):Section+TEXT(": ")));
    const FString ImportText=CollectText(Host->MakeUCMPanel(TEXT("Import")));
    TestFalse(TEXT("Import removes setups and JSON editor"),ImportText.Contains(TEXT("New setup"))||ImportText.Contains(TEXT("Save current setup"))||ImportText.Contains(TEXT("Edit advanced JSON")));
    TestTrue(TEXT("Distinct loot and route import actions"),ImportText.Contains(TEXT("Import vTank Loot Profile (*.utl)"))&&ImportText.Contains(TEXT("Import vTank Route (*.nav)")));
    TestTrue(TEXT("Import all and clear notices available"),ImportText.Contains(TEXT("Import all files"))&&ImportText.Contains(TEXT("Clear")));
    auto UCM=Host->Find(TEXT("ucm"));auto Micro=Host->MakeUCMMicroPanel();
    TestTrue(TEXT("Micro starts with full width Start UCM action"),CollectText(Micro).Contains(TEXT("Start UCM")));
    UCM->Running=true;TestTrue(TEXT("Micro action updates to Stop UCM"),CollectText(Micro).Contains(TEXT("Stop UCM")));UCM->Running=false;
    TestFalse(TEXT("Micro removes running checkbox"),CollectText(Micro).Contains(TEXT("UCM running")));
    auto Rat=MakeShared<FJsonObject>();Rat->SetStringField(TEXT("name"),TEXT("Brown Rat"));UCM->Profile->SetArrayField(TEXT("monsters"),{MakeShared<FJsonValueObject>(Rat)});
    auto MonstersPanel=Host->MakeUCMPanel(TEXT("Monsters"));
    TFunction<void(const TSharedRef<SWidget>&)> OpenDetails=[&](const TSharedRef<SWidget>& Widget)
    {
        if(Widget->GetType()==TEXT("SACEPluginButton")&&Widget->GetTag()==TEXT("UCMMonster.Brown Rat.details"))
        {StaticCastSharedRef<SButton>(Widget)->SimulateClick();return;}
        // Snapshot children: clicking Details rebuilds the panel.
        TArray<TSharedRef<SWidget>> Children;auto* C=Widget->GetChildren();for(int32 I=0;I<C->Num();++I)Children.Add(C->GetChildAt(I));
        for(const auto& Child:Children)OpenDetails(Child);
    };
    OpenDetails(MonstersPanel);const FString Monsters=CollectText(MonstersPanel);
    TestTrue(TEXT("Mob details expose priority and retain default range"),Monsters.Contains(TEXT("Priority\n"))&&Monsters.Contains(TEXT("Attack Range: use default")));
    TestFalse(TEXT("Priority has no redundant default checkbox"),Monsters.Contains(TEXT("Priority: use default")));
    TestTrue(TEXT("Monster search and name action exist"),Monsters.Contains(TEXT("Search"))&&Monsters.Contains(TEXT("Add\n")));
    TestTrue(TEXT("Details exposes the complete attack profile"),Monsters.Contains(TEXT("Combat mode\n"))&&Monsters.Contains(TEXT("Imperil\n"))&&Monsters.Contains(TEXT("Ring spells\n"))&&Monsters.Contains(TEXT("Pet damage type\n"))&&Monsters.Contains(TEXT("Secondary vulnerability\n")));
    TestTrue(TEXT("Tab uses Mobs while existing initial page names remain compatible"),Monsters.Contains(TEXT("Mobs\n"))&&!Monsters.Contains(TEXT("Monsters\n")));
    TestTrue(TEXT("New initial page name opens the mob editor"),CollectText(Host->MakeUCMPanel(TEXT("Mobs"))).Contains(TEXT("Add selected mob")));
    UCM->Profile->SetNumberField(TEXT("ring_min_targets"),4);
    auto CombatPanel=Host->MakeUCMPanel(TEXT("Combat"));
    const FString Combat=CollectText(CombatPanel);
    TestTrue(TEXT("Combat exposes both profile defaults"),Combat.Contains(TEXT("Default mob priority"))&&Combat.Contains(TEXT("Default mob attack range")));
    TestFalse(TEXT("Combat does not duplicate chart spell or damage switches"),Combat.Contains(TEXT("Use ring spells\n"))||Combat.Contains(TEXT("Magic Yield\n"))||Combat.Contains(TEXT("Void curses\n"))||Combat.Contains(TEXT("Fire\n")));
    TestTrue(TEXT("Ring count displays a whole number without percent"),Combat.Contains(TEXT("Minimum ring targets\n4\n")));
    TMap<FName,TSharedPtr<SSlider>> Sliders;
    TFunction<void(const TSharedRef<SWidget>&)> CollectSliders=[&](const TSharedRef<SWidget>& Widget)
    {
        if(Widget->GetType()==FName(TEXT("SSlider")))Sliders.Add(Widget->GetTag(),StaticCastSharedRef<SSlider>(Widget));
        auto Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)CollectSliders(Children->GetChildAt(I));
    };
    CollectSliders(CombatPanel);CollectSliders(MonstersPanel);
    for(const TCHAR* Key:{TEXT("ring_min_targets"),TEXT("monster_default_priority"),TEXT("priority")})
    {
        const auto* Slider=Sliders.Find(FName(Key));
        if(!TestTrue(TEXT("Count/priority control is present"),Slider!=nullptr))continue;
        TestEqual(TEXT("Count and priority sliders stop at ten"),(*Slider)->GetMaxValue(),10.f);
        TestEqual(TEXT("Priority retains ignore while ring count starts at one"),(*Slider)->GetMinValue(),FString(Key)==TEXT("ring_min_targets")?1.f:-1.f);
        TestEqual(TEXT("Count and priority controls use integer increments"),(*Slider)->GetStepSize(),1.f);
    }
    if(FApp::CanEverRender())
    {
        FWidgetRenderer Renderer(false,true);
        const FString Folder=FPaths::ProjectSavedDir()/TEXT("UCMProfileReview/UI");IFileManager::Get().MakeDirectory(*Folder,true);
        for(const TCHAR* Page:{TEXT("Overview"),TEXT("Import"),TEXT("Micro"),TEXT("Route"),TEXT("Buffs"),TEXT("Buff others"),TEXT("Combat"),TEXT("Recovery"),TEXT("Loot"),TEXT("Vendors"),TEXT("Monsters"),TEXT("Import vTank")})for(int Width:{780,460})
        {
            auto Panel=FString(Page)==TEXT("Micro")?Host->MakeUCMMicroPanel():Host->MakeUCMPanel(Page);const FVector2D Size(Width,900);
            auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}
            TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            TestEqual(TEXT("Shared profile controls render at both window widths"),Pixels.Num(),Width*900);
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Width,900,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(Folder/FString::Printf(TEXT("%s-%d.png"),Page,Width)));Target->ReleaseResource();
        }
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMRouteSaveEndToEndTest, "ACE.Plugins.RouteSaveEndToEnd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEUCMRouteSaveEndToEndTest::RunTest(const FString&)
{
    if(!FApp::CanEverRender()){AddError(TEXT("Route UI verification requires rendering"));return false;}
    auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT{GI->Shutdown();};
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/RouteSave")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Host->StorageRoot=Root;Host->Settings=MakeShared<FJsonObject>();
    if(!TestTrue(TEXT("Initialize clean route storage"),Host->InitializeUCMSelection()))return false;
    auto Plugin=Host->Find(TEXT("ucm"));auto Session=GI->GetSubsystem<UACEClientSubsystem>()->GetSession();
    Session->State=EACESessionState::InWorld;
    auto& Slate=FSlateApplication::Get();const auto OldFocus=Slate.GetUserFocusedWidget(0);
    auto Window=SNew(SVirtualWindow).Size(FVector2D(900,1000));Window->SetIsFocusable(true);Slate.RegisterVirtualWindow(Window);
    ON_SCOPE_EXIT{Slate.DismissAllMenus();Slate.UnregisterVirtualWindow(Window);if(OldFocus)Slate.SetUserFocus(0,OldFocus);};
    FWidgetRenderer Renderer;TSharedPtr<SWidget> Panel;
    TFunction<TSharedPtr<SWidget>(const TSharedRef<SWidget>&,FName,bool)> Find;
    Find=[&](const TSharedRef<SWidget>& W,FName Name,bool Type)->TSharedPtr<SWidget>
    {
        if((Type?W->GetType():W->GetTag())==Name)return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Found=Find(Children->GetChildAt(I),Name,Type))return Found;
        return nullptr;
    };
    auto PaintWindow=[&](const TSharedRef<SWindow>& W,const FVector2D& Size)
    {
        auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,false);
        for(int32 I=0;I<3;++I){++GFrameCounter;Renderer.DrawWindow(Target,W->GetHittestGrid(),W,1.f,Size,.016f);FlushRenderingCommands();}
        Target->ReleaseResource();
    };
    auto Paint=[&](){PaintWindow(Window,FVector2D(900,1000));};
    auto Page=[&](const TCHAR* Name){Slate.DismissAllMenus();Panel=Host->MakeUCMPanel(Name);Window->SetContent(Panel.ToSharedRef());Paint();};
    auto Click=[&](const TSharedPtr<SWidget>& Widget)
    {
        if(!TestTrue(TEXT("Route control exists and is enabled"),Widget && Widget->IsEnabled()))return false;
        const auto& G=Widget->GetCachedGeometry();const auto P=G.LocalToAbsolute(G.GetLocalSize()*.5);
        const auto Owner=Slate.FindWidgetWindow(Widget.ToSharedRef());
        if(!TestTrue(TEXT("Route control belongs to a window"),Owner.IsValid()))return false;
        const auto Path=Slate.LocateWindowUnderMouse(P,{Owner.ToSharedRef()},false,0);
        if(!TestTrue(*FString::Printf(TEXT("Rendered route control %s has a hit-test path at %s"),*Widget->GetTag().ToString(),*P.ToString()),Path.IsValid()))return false;
        Slate.RoutePointerMoveEvent(Path,FPointerEvent(0,P,P,{},EKeys::Invalid,0,FModifierKeysState()),false);
        Slate.RoutePointerDownEvent(Path,FPointerEvent(0,P,P,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
        Slate.RoutePointerUpEvent(Path,FPointerEvent(0,P,P,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
        Paint();return true;
    };
    auto Button=[&](const TCHAR* Caption){return Click(Find(Panel.ToSharedRef(),FName(*(FString(TEXT("UCMChoice."))+Caption)),false));};
    auto Key=[&](FKey K){Slate.ProcessKeyDownEvent(FKeyEvent(K,FModifierKeysState(),0,false,0,0));Slate.ProcessKeyUpEvent(FKeyEvent(K,FModifierKeysState(),0,false,0,0));};
    auto Create=[&](const FString& Name,bool Enter,bool Success=true)
    {
        const FString Previous=Host->UCMSectionName(TEXT("Routes"));
        if(!Button(TEXT("+ New route")))return false;
        auto Input=Find(Panel.ToSharedRef(),TEXT("UCMNewProfileName"),false);
        if(!TestTrue(TEXT("New route name can receive typing"),Input.IsValid()))return false;
        Slate.SetUserFocus(0,Input);
        // A failed attempt keeps the draft, just as the live editor does.
        Slate.ProcessKeyDownEvent(FKeyEvent(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false),0,false,0,0));
        Slate.ProcessKeyUpEvent(FKeyEvent(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false),0,false,0,0));
        for(TCHAR C:Name)Slate.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
        TestEqual(TEXT("Typed name reaches the route field"),StaticCastSharedPtr<SEditableTextBox>(Input)->GetText().ToString(),Name);
        if(Enter){Key(EKeys::Enter);Paint();}else if(!Button(TEXT("Create and select route")))return false;
        if(!Success)
        {
            TestEqual(TEXT("Failed creation preserves the active route"),Host->UCMSectionName(TEXT("Routes")),Previous);
            const auto Error=Find(Panel.ToSharedRef(),TEXT("UCMProfileCreateError"),false);
            TestTrue(TEXT("Creation error is shown beside the name field"),Error.IsValid());
            if(Error)TestEqual(TEXT("Inline message explains creation failure"),StaticCastSharedPtr<STextBlock>(Error)->GetText().ToString(),Host->Notice);
            return Error.IsValid();
        }
        TestEqual(TEXT("Create selects the saved route"),Host->UCMSectionName(TEXT("Routes")),Name);
        TestTrue(TEXT("Create reports saved file"),Host->Notice.Contains(TEXT("Created and selected Routes: ")+Name));
        return Host->UCMSectionName(TEXT("Routes"))==Name;
    };
    auto ReadRoute=[&](const FString& Name)
    {
        FString Text;TSharedPtr<FJsonObject> Data;
        FFileHelper::LoadFileToString(Text,*(Root/TEXT("Profiles/Routes")/(Name+TEXT(".json"))));
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Data);return Data;
    };
    auto Choose=[&](const FString& Name)
    {
        auto Choice=Find(Panel.ToSharedRef(),TEXT("UCMProfile.Routes"),false);
        if(!TestTrue(TEXT("Route dropdown is present"),Choice.IsValid()))return false;
        auto Combo=StaticCastSharedPtr<SComboButton>(Choice);
        auto Open=Find(Choice.ToSharedRef(),TEXT("SButton"),true);
        if(!Click(Open))return false;
        if(!TestTrue(TEXT("Route dropdown opens"),Combo->IsOpen()))return false;
        auto MenuWindow=Combo->GetMenuWindow();
        if(!TestTrue(TEXT("Route dropdown has a visible popup"),MenuWindow.IsValid()))return false;
        PaintWindow(MenuWindow.ToSharedRef(),MenuWindow->GetClientSizeInScreen());
        auto Entry=Find(MenuWindow.ToSharedRef(),FName(*(TEXT("UCMMenu.")+Name)),false);
        if(!TestTrue(TEXT("Saved route appears in the actual dropdown"),Entry.IsValid()))return false;
        if(!Click(Entry))return false;
        TestEqual(TEXT("Dropdown activates saved route"),Host->UCMSectionName(TEXT("Routes")),Name);return true;
    };
    Page(TEXT("Route"));
    // Exercise ordinary UI typing, not SetText/SaveProfile shortcuts. Both click
    // and Enter must save names users naturally enter and populate every chooser.
    for(const FString Name:{TEXT("Academy_Loop"),TEXT("Academy Loop")})
    {
        if(!Create(Name,Name.Contains(TEXT(" "))))return false;
        auto Disk=ReadRoute(Name);if(!TestTrue(TEXT("Empty route exists before recording"),Disk.IsValid()))return false;
        TestEqual(TEXT("New route starts empty"),Disk->GetArrayField(TEXT("route")).Num(),0);
        for(int32 I=0;I<3;++I)
        {
            FACEPosition Pos;Pos.CellId=0x7D640001;Pos.Location=FVector(10+I,20+I,3);Session->SetLocalPosition(Pos);
            if(!Button(TEXT("+ Add waypoint at my position")))return false;
        }
        Disk=ReadRoute(Name);if(!TestTrue(TEXT("Recorded route is valid on disk"),Disk.IsValid()))return false;
        TestEqual(TEXT("Every recorded point saved"),Disk->GetArrayField(TEXT("route")).Num(),3);
        TestEqual(TEXT("Recorded coordinates saved"),Disk->GetArrayField(TEXT("route"))[2]->AsObject()->GetNumberField(TEXT("x")),12.);
        for(const TCHAR* Surface:{TEXT("Route"),TEXT("Overview")})
        {Page(Surface);if(!Choose(TEXT("Default"))||!Choose(Name))return false;TestEqual(TEXT("Dropdown reloads all saved points"),Plugin->Profile->GetArrayField(TEXT("route")).Num(),3);}
        // Discard runtime profile data and reload the persisted selection, then
        // rebuild the panels: this catches memory-only entries and stale menus.
        Plugin->Profile.Reset();Plugin->ProfileLinks.Reset();Plugin->ProfileBaseline.Reset();
        if(!TestTrue(TEXT("Saved selection reloads from disk"),Host->InitializeUCMSelection()))return false;
        TestEqual(TEXT("Reload retains selected route"),Host->UCMSectionName(TEXT("Routes")),Name);
        for(const TCHAR* Surface:{TEXT("Overview"),TEXT("Route")}){Page(Surface);if(!Choose(Name))return false;}
    }
    Page(TEXT("Route"));
    if(!Create(TEXT("Academy Loop"),false,false))return false;
    TestTrue(TEXT("Duplicate explains how to use the existing route"),Host->Notice.Contains(TEXT("already exists")));
    TestEqual(TEXT("Duplicate leaves recorded route intact"),ReadRoute(TEXT("Academy Loop"))->GetArrayField(TEXT("route")).Num(),3);
    if(!Create(TEXT("../Escape"),true,false))return false;
    TestFalse(TEXT("Invalid filename never enters the chooser"),Host->UCMSectionProfiles(TEXT("Routes")).Contains(TEXT("../Escape")));
    // A directory occupying the requested file makes an actual disk write fail,
    // without altering permissions or touching the player's saved profiles.
    IFileManager::Get().MakeDirectory(*(Root/TEXT("Profiles/Routes/Blocked.json")),true);
    if(!Create(TEXT("Blocked"),false,false))return false;
    TestTrue(TEXT("Write failure identifies the destination"),Host->Notice.Contains(TEXT("Blocked.json")));
    TestFalse(TEXT("Failed write is absent from dropdowns"),Host->UCMSectionProfiles(TEXT("Routes")).Contains(TEXT("Blocked")));
    if(!Button(TEXT("Cancel")))return false;
    if(!Choose(TEXT("Academy_Loop"))||!Choose(TEXT("Academy Loop")))return false;
    return true;
}
#endif
