#include "ACEProjectMigration.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEProjectMigrationTest, "ACE.Packaging.ProfileMigration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEProjectMigrationTest::RunTest(const FString& Parameters)
{
    const FString Root = FPaths::ProjectSavedDir() / TEXT("Automation/ProfileMigration") / FGuid::NewGuid().ToString();
    const FString Old = Root / TEXT("Old"), New = Root / TEXT("New");
    auto Write = [](const FString& Path, const FString& Text) {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        return FFileHelper::SaveStringToFile(Text, *Path);
    };
    Write(Old / TEXT("Login/LastLogin.dat"), TEXT("encrypted fixture bytes"));
    Write(Old / TEXT("Journal/test.json"), TEXT("journal fixture"));
    const FString Atlas = TEXT("ClientPlugins/Waypoint/locations.json");
    const FString LegacyAtlas = TEXT(R"({"locations":[{"name":"Legacy portal","type":"Portal","east":3,"north":4,"exitEast":5,"exitNorth":6}],"includesRouting":true})");
    Write(Old / Atlas, LegacyAtlas);
    Write(Old / (Atlas + TEXT(".abandoned.tmp")), TEXT("incomplete import"));
    Write(Old / TEXT("Config/Windows/ACEVR.ini"), TEXT("[VR]\nValue=old"));
    Write(Old / TEXT("Config/Windows/GameUserSettings.ini"), TEXT("[Settings]\nValue=old"));
    Write(Old / TEXT("Config/Windows/Engine.ini"), TEXT("stale project paths"));
    Write(Old / TEXT("Logs/test.log"), TEXT("ignored log"));
    Write(New / TEXT("Config/Windows/ACEVR.ini"), TEXT("[VR]\nValue=new"));
    TestEqual(TEXT("Missing login, journal, graphics settings and atlas copied"), ACEProjectMigration::CopyMissingProfileFiles(Old, New), 4);
    FString Text;
    FFileHelper::LoadFileToString(Text, *(New / TEXT("Config/Windows/ACEVR.ini")));
    TestTrue(TEXT("New profile wins"), Text.Contains(TEXT("Value=new")));
    FFileHelper::LoadFileToString(Text, *(New / TEXT("Login/LastLogin.dat")));
    TestEqual(TEXT("Credentials preserved byte content"), Text, FString(TEXT("encrypted fixture bytes")));
    TestFalse(TEXT("No stale engine config"), IFileManager::Get().FileExists(*(New / TEXT("Config/Windows/Engine.ini"))));
    TestFalse(TEXT("No logs"), IFileManager::Get().FileExists(*(New / TEXT("Logs/test.log"))));
    TestFalse(TEXT("Unfinished imports are ignored"), IFileManager::Get().FileExists(*(New / (Atlas + TEXT(".abandoned.tmp")))));
    TestTrue(TEXT("Migrated atlas is readable"), FFileHelper::LoadFileToString(Text, *(New / Atlas)));
    TestEqual(TEXT("All legacy atlas records and routing metadata survive"), Text, LegacyAtlas);
    const FString CurrentAtlas = TEXT(R"({"locations":[{"name":"Current town","type":"Town","east":7,"north":8}]})");
    Write(New / Atlas, CurrentAtlas);
    TestEqual(TEXT("Repeat import is idempotent"), ACEProjectMigration::CopyMissingProfileFiles(Old, New), 0);
    TestTrue(TEXT("Current atlas is readable"), FFileHelper::LoadFileToString(Text, *(New / Atlas)));
    TestEqual(TEXT("Legacy migration keeps the current import"), Text, CurrentAtlas);
    TestTrue(TEXT("Old profile preserved"), IFileManager::Get().FileExists(*(Old / TEXT("Login/LastLogin.dat"))));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif
