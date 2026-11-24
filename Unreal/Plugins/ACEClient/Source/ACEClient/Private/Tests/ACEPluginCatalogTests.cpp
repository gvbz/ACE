#if WITH_DEV_AUTOMATION_TESTS
#include "Mods/ACEPluginCatalog.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "ImageUtils.h"
#include "Slate/WidgetRenderer.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Interfaces/IPluginManager.h"
#include "Mods/ACEPluginVM.h"
#include "Widgets/Text/STextBlock.h"
#include "RenderingThread.h"
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#undef UI

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginCatalogTest,"ACE.Plugins.Catalog",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginCatalogTest::RunTest(const FString&)
{
    using namespace ACEPluginCatalog;
    auto* GI=NewObject<UGameInstance>();GI->Init();
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Host->StorageRoot=Root;Host->Settings=MakeShared<FJsonObject>();Host->bCatalogOnlyTest=true;Host->Discover();
    TestTrue(TEXT("New installs have no active plugin manifests"),Host->Plugins.IsEmpty());
    TestFalse(TEXT("Fresh origin does not restore previously bundled plugins"),FLegacyRestore::Prepare(*Host).IsValid());
    ON_SCOPE_EXIT{GI->Shutdown();IFileManager::Get().DeleteDirectory(*Root,false,true);};

    FEntry E;E.Id=TEXT("catalog_test");E.Name=TEXT("Catalog test");E.Download=TEXT("fixture");
    auto Package=[&](const FString& Version,const FString& Permissions,const FString& Extra=FString())
    {
        const FString Manifest=TEXT("{\"id\":\"")+E.Id+TEXT("\",\"api\":2,\"mode\":\"companion\",\"name\":\"Test\",\"version\":\"")+Version+TEXT("\",\"permissions\":")+Permissions+TEXT("}");
        const FString Bundle=TEXT("{\"format\":1,\"files\":{\"plugin.json\":\"")+FBase64::Encode(Manifest)+TEXT("\",\"main.lua\":\"")+FBase64::Encode(FString(TEXT("return function() return {} end")))+TEXT("\"")+Extra+TEXT("}}");
        FTCHARToUTF8 Utf8(*Bundle);
        TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
        uint8 Hash[SHA256_DIGEST_LENGTH];SHA256(Bytes.GetData(),Bytes.Num(),Hash);
        E.Digest=BytesToHex(Hash,sizeof(Hash)).ToLower();E.Bytes=Bytes.Num();
        return Bytes;
    };
    FString Error;
    auto Bytes=Package(TEXT("1"),TEXT("[\"ui\"]"));
    const FString Repo=TEXT("https://github.com/example/catalog_test");
    const FString Xml=FString::Printf(TEXT("<catalog schema=\"1\"><plugin id=\"catalog_test\" repository=\"%s\"><name>Test &amp; Demo</name><description>Example</description><release tag=\"v1\" bytes=\"%d\" sha256=\"%s\" download=\"%s/releases/download/v1/catalog_test.acplugin\"/></plugin></catalog>"),*Repo,E.Bytes,*E.Digest,*Repo);
    TArray<FEntry> Entries;
    if(!TestTrue(TEXT("Valid release index"),Parse(Xml,Entries,Error)))return false;
    TestEqual(TEXT("Display XML entities correctly"),Entries[0].Name,FString(TEXT("Test & Demo")));
    TestEqual(TEXT("Old feed falls back to repository owner"),Entries[0].Developer,FString(TEXT("example")));
    const FString Credited=Xml.Replace(TEXT("<description>"),TEXT("<developer>Camp &amp; Co</developer><description>"));
    TestTrue(TEXT("Read credited developer"),Parse(Credited,Entries,Error)&&Entries[0].Developer==TEXT("Camp & Co"));
    TestTrue(TEXT("New listing is available"),Inspect(*Host,E).State==EInstallState::Available);
    TestFalse(TEXT("Reject a release URL outside the registered repository"),Parse(Xml.Replace(TEXT("/releases/download/"),TEXT("/other/download/")),Entries,Error));
    TestFalse(TEXT("Reject traversal in registered IDs"),Parse(Xml.Replace(TEXT("id=\"catalog_test\""),TEXT("id=\"../other\"")),Entries,Error));
    TestFalse(TEXT("IDs are portable across case-sensitive filesystems"),Parse(Xml.Replace(TEXT("catalog_test"),TEXT("catalog_Test")),Entries,Error));
    TestEqual(TEXT("Last good catalog survives malformed refresh"),Entries.Num(),1);
    Host->Settings->SetStringField(E.Id,TEXT("enabled:ui")); // Former manual install.
    if(!TestTrue(TEXT("Install package"),Install(*Host,E,Bytes,Error))){AddError(Error);return false;}
    auto P=Host->Find(E.Id);
    if(!TestTrue(TEXT("Installed plugin discovered with fresh consent"),P&&!P->Enabled&&P->Version==TEXT("1")))return false;
    TestTrue(TEXT("Matching local package is current"),Inspect(*Host,E).State==EInstallState::Current);
    const FString Script=P->Directory/TEXT("main.lua");
    TestTrue(TEXT("Remove fixture script"),IFileManager::Get().Delete(*Script,true));
    TestTrue(TEXT("Missing script offers repair even when receipt matches"),Inspect(*Host,E).State==EInstallState::Repair);
    if(!TestTrue(TEXT("Repair from verified package"),Install(*Host,E,Bytes,Error)))return false;
    Host->SetEnabled(E.Id,true);
    const FString Old=ActiveDirectory(Root,E.Id);
    Bytes.Last()^=1;
    TestFalse(TEXT("Reject altered download"),Install(*Host,E,Bytes,Error));
    Bytes=Package(TEXT("2"),TEXT("[\"ui\"]"),TEXT(",\"../escaped.lua\":\"MQ==\""));
    TestFalse(TEXT("Reject an out-of-package path even with a valid hash"),Install(*Host,E,Bytes,Error));
    Bytes=Package(TEXT("2"),TEXT("[\"ui\"]"),TEXT(",\"DEFAULT.JSON\":\"e30=\""));
    TestFalse(TEXT("Require canonical names for package metadata"),Install(*Host,E,Bytes,Error));
    TestEqual(TEXT("Failed updates preserve the working revision"),ActiveDirectory(Root,E.Id),Old);
    Bytes=Package(TEXT("2"),TEXT("[\"ui\"]"));
    TestTrue(TEXT("New package digest offers an individual update"),Inspect(*Host,E).State==EInstallState::Update);
    TestTrue(TEXT("Install compatible update"),Install(*Host,E,Bytes,Error));
    P=Host->Find(E.Id);
    TestTrue(TEXT("Existing consent survives same-permissions update"),P&&P->Enabled&&P->Version==TEXT("2"));
    Bytes=Package(TEXT("3"),TEXT("[\"ui\",\"chat\"]"));
    TestTrue(TEXT("Install changed-capability update"),Install(*Host,E,Bytes,Error));
    Host->Discover(); P=Host->Find(E.Id);
    TestTrue(TEXT("Updated capabilities require renewed consent on discovery"),P&&!P->Enabled&&P->Version==TEXT("3"));
    const FString Profile=Root/TEXT("Profiles/catalog_test/Mine.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Profile),true); FFileHelper::SaveStringToFile(TEXT("{}"),*Profile);
    TestTrue(TEXT("Remove managed package"),Remove(*Host,E.Id,Error));
    TestTrue(TEXT("Removal preserves profiles and survives refresh"),!Host->Find(E.Id)&&ActiveDirectory(Root,E.Id).IsEmpty()&&IFileManager::Get().FileExists(*Profile));
    TestFalse(TEXT("Removal cleans retired package files"),IFileManager::Get().DirectoryExists(*Old));
    const FString Manual=Root/TEXT("Installed/another-folder/plugin.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Manual),true);
    FFileHelper::SaveStringToFile(TEXT("{\"id\":\"catalog_test\",\"version\":\"2.9\",\"api\":2,\"permissions\":[]}"),*Manual);
    Host->Discover();
    E.Version=TEXT("2.9.0");
    TestTrue(TEXT("Recognize matching manual manifest even under another folder name"),Inspect(*Host,E).State==EInstallState::ManualCurrent);
    E.Version=TEXT("2.8");
    TestTrue(TEXT("Newer manual version is not offered a downgrade"),Inspect(*Host,E).State==EInstallState::Newer);
    E.Version=TEXT("2.10");
    TestTrue(TEXT("Compare numerical version components"),Inspect(*Host,E).State==EInstallState::ManualUpdate);
    E.Version=TEXT("spring");
    TestTrue(TEXT("Do not invent an ordering for publisher labels"),Inspect(*Host,E).State==EInstallState::ManualUnknown);
    E.Version=TEXT("3");
    TestFalse(TEXT("A manual install with the same ID is never overwritten"),Install(*Host,E,Bytes,Error));
    if(!TestTrue(TEXT("Explicitly choose the catalog copy"),Install(*Host,E,Bytes,Error,true)))return false;
    TestTrue(TEXT("Single active copy after adoption"),Inspect(*Host,E).State==EInstallState::Current&&Host->Plugins.FilterByPredicate([&](const auto& Plugin){return Plugin->Id==E.Id;}).Num()==1);
    Remove(*Host,E.Id,Error);
    TestTrue(TEXT("Uninstall deletes managed and renamed manual folders"),!Host->Find(E.Id)&&!IFileManager::Get().DirectoryExists(*FPaths::GetPath(Manual))&&!IFileManager::Get().DirectoryExists(*(Root/TEXT("Catalog/Packages")/E.Id)));
    TestTrue(TEXT("Profiles created on a fresh install never trigger a later migration"),!FLegacyRestore::Prepare(*Host));
    FString Fixtures;
    if(FParse::Value(FCommandLine::Get(),TEXT("OfficialPluginFixtureDir="),Fixtures))
    {
        FString LegacyXml;
        const auto Descriptor=IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
        TArray<FEntry> OfficialEntries;
        if(!Descriptor||!FFileHelper::LoadFileToString(LegacyXml,*(Descriptor->GetBaseDir()/TEXT("ClientMods/legacy-plugins.xml")))||!Parse(LegacyXml,OfficialEntries,Error))return false;
        TestEqual(TEXT("Every original plugin has an upgrade descriptor"),OfficialEntries.Num(),12);
        for(const auto& Entry:OfficialEntries)
        {
            TArray<uint8> Asset;
            if(!TestTrue(Entry.Id+TEXT(" package loaded"),FFileHelper::LoadFileToArray(Asset,*(Fixtures/(Entry.Id+TEXT(".acplugin"))))))return false;
            if(!TestTrue(Entry.Id+TEXT(" installs alone"),Install(*Host,Entry,Asset,Error))){AddError(Error);return false;}
            auto Only=Host->Find(Entry.Id);
            if(!TestTrue(Entry.Id+TEXT(" is the only installed plugin"),Only.IsValid()&&Host->Plugins.Num()==1&&Inspect(*Host,Entry).State==EInstallState::Current))return false;
            const auto Panel=Host->MakePanel(Entry.Id);
            TestTrue(Entry.Id+TEXT(" builds its panel"),Panel->GetChildren()->Num()>0);
            FString Source,Policy;
            if(FFileHelper::LoadFileToString(Source,*(Only->Directory/TEXT("main.lua"))))
            {
                Only->Manifest->TryGetStringField(TEXT("policy"),Policy);FACEPluginVM VM;
                if(!TestTrue(Entry.Id+TEXT(" loads its Lua entry point"),VM.Load(Source,Error,Policy))){AddError(Error);return false;}
            }
            if(Entry.Id==TEXT("ucmmicro"))
            {
                TestTrue(TEXT("Micro drives its own profile"),Host->CompactPolicy(Entry.Id)==Only&&Only->IsPolicy()&&Only->UsesCompactControls());
                TestTrue(TEXT("Micro requires its own explicit permission grant"),!Only->Enabled);
            }
            if(Entry.Id==TEXT("looteditor"))
            {
                TestTrue(TEXT("Standalone editor creates and selects a shared loot profile"),Host->NewUCMSection(TEXT("Loot"),TEXT("Standalone loot")));
                TestEqual(TEXT("New loot profile selected"),Host->UCMSectionName(TEXT("Loot")),FString(TEXT("Standalone loot")));
            }
            if(Entry.Id==TEXT("thirdeye"))
            {
                Host->SetWaypoint(FVector2D(1,2),0x014F0000,true);FVector2D Point;uint32 Dungeon=0;
                TestTrue(TEXT("Object arrow without Waypoint"),Host->GetWaypoint(Point,Dungeon)&&Point==FVector2D(1,2)&&Dungeon==0x014F0000&&Host->ShouldShowWaypointArrow());
                Host->ClearWaypoint();TestFalse(TEXT("Clearing standalone arrow hides it"),Host->ShouldShowWaypointArrow());
            }
            if(!TestTrue(Entry.Id+TEXT(" uninstalls"),Remove(*Host,Entry.Id,Error)))return false;
        }

    }

    if(FParse::Param(FCommandLine::Get(),TEXT("PluginCatalogLive")))
    {
        const auto Browser=MakeBrowser(Host);
        FWidgetRenderer Renderer(false,true);const FVector2D Size(960,620);
        auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        ON_SCOPE_EXIT{Target->ReleaseResource();};
        auto Draw=[&]{for(int32 I=0;I<3;++I){++GFrameCounter;Renderer.DrawWidget(Target,Browser,Size,.016f);FlushRenderingCommands();}};
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName)> Find=[&](TSharedRef<SWidget> W,FName Name)->TSharedPtr<SWidget>
        {
            if(W->GetTag()==Name)return W;
            auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=Find(C->GetChildAt(I),Name))return Found;
            return {};
        };
        Draw();FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::Default);Draw();
        TArray<FColor> Pixels;
        if(Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels))
        {
            TArray64<uint8> Png;
            FImageUtils::PNGCompressImageArray(int32(Size.X),int32(Size.Y),Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/CommunityPluginCatalog.png")));
        }
        auto Click=[&](const TCHAR* Name)
        {
            auto Control=Find(Browser,Name);
            if(!Control){auto Msg=Find(Browser,TEXT("Catalog.Status"));AddError(FString(Name)+TEXT(": ")+(Msg?StaticCastSharedPtr<STextBlock>(Msg)->GetText().ToString():TEXT("no status")));return false;}
            StaticCastSharedPtr<SButton>(Control)->SimulateClick();return true;
        };
        if(auto Search=Find(Browser,TEXT("Catalog.Search")))StaticCastSharedPtr<SEditableTextBox>(Search)->SetText(FText::FromString(TEXT("graphics_gallery")));
        Draw();
        if(!Click(TEXT("Catalog.Install.graphics_gallery")))return false;
        FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::Default);Draw();
        if(!TestTrue(TEXT("Installed real GitHub release through account manager UI"),Host->Find(TEXT("graphics_gallery")).IsValid()))
        {AddError(StaticCastSharedPtr<STextBlock>(Find(Browser,TEXT("Catalog.Status")))->GetText().ToString());return false;}
        TestFalse(TEXT("Installed release does not offer another install or update"),Find(Browser,TEXT("Catalog.Install.graphics_gallery")).IsValid()||Find(Browser,TEXT("Catalog.Update.graphics_gallery")).IsValid());
        E.Id=TEXT("graphics_gallery");E.Version=TEXT("0.1");
        const auto Older=Package(E.Version,TEXT("[\"ui\"]"));
        for(const TCHAR* Action:{TEXT("Catalog.Update.graphics_gallery"),TEXT("Catalog.UpdateAll")})
        {
            if(!TestTrue(TEXT("Seed a prior release in the isolated profile"),Install(*Host,E,Older,Error)))return false;
            if(!Click(TEXT("Catalog.Refresh")))return false;
            FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::Default);Draw();
            if(!Click(Action))return false;
            FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::Default);
            FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
            Draw();
            const auto Gallery=Host->Find(E.Id);
            TestTrue(TEXT("Update installs the published release"),Gallery&&Gallery->Version!=TEXT("0.1"));
            TestFalse(TEXT("Updated row no longer offers an update"),Find(Browser,TEXT("Catalog.Update.graphics_gallery")).IsValid());
            TestFalse(TEXT("A removed plugin stayed removed"),Host->Find(TEXT("catalog_test")).IsValid());
        }
        if(!Click(TEXT("Catalog.Uninstall.graphics_gallery")))return false;
        Draw();if(!Click(TEXT("Catalog.Confirm uninstall.graphics_gallery")))return false;
        TestFalse(TEXT("Confirmed removal through UI"),Host->Find(TEXT("graphics_gallery")).IsValid());

        Host->StorageRoot=Root/TEXT("PreviousInstallation");
        Host->Settings=MakeShared<FJsonObject>();
        Host->Settings->SetStringField(TEXT("waypoint"),TEXT("disabled"));
        Host->SaveSettings();Host->Discover();
        const auto Migration=FLegacyRestore::Prepare(*Host);
        if(!TestTrue(TEXT("Existing user data triggers the one-time package restore"),Migration.IsValid()))return false;
        for(int32 Index=0;Index<12;++Index)
        {
            Migration->Tick(*Host);
            FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::Default);
        }
        TestEqual(TEXT("An upgraded installation recovers all original packages from GitHub"),Host->Plugins.Num(),12);
        const auto Restored=Host->Find(TEXT("waypoint"));
        TestTrue(TEXT("Restoring a formerly shipped plugin retains a disabled preference"),Restored&&!Restored->Enabled);
        TestTrue(TEXT("Remove a restored package"),Remove(*Host,TEXT("waypoint"),Error));
        TestFalse(TEXT("A subsequent login never restores an uninstalled plugin"),FLegacyRestore::Prepare(*Host).IsValid());
    }
    return true;
}
#endif
