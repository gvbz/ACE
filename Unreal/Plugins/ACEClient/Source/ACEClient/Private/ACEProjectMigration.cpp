#include "ACEProjectMigration.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

int32 ACEProjectMigration::CopyMissingProfileFiles(const FString& OldSaved, const FString& NewSaved)
{
    IFileManager& Files = IFileManager::Get();
    int32 Copied = 0;
    for (const TCHAR* Folder : {TEXT("Config"), TEXT("Login"), TEXT("Journal"), TEXT("SaveGames")})
    {
        const FString SourceRoot = FPaths::ConvertRelativePathToFull(OldSaved / Folder) + TEXT("/");
        TArray<FString> Sources;
        Files.FindFilesRecursive(Sources, *SourceRoot, TEXT("*"), true, false);
        for (const FString& Source : Sources)
        {
            FString Relative = Source;
            FPaths::MakePathRelativeTo(Relative, *SourceRoot);
            // Do not import stale engine/project paths or crash reporter configuration.
            if (FCString::Strcmp(Folder, TEXT("Config")) == 0 &&
                FPaths::GetCleanFilename(Source) != TEXT("GameUserSettings.ini") &&
                FPaths::GetCleanFilename(Source) != TEXT("ACEVR.ini") &&
                FPaths::GetCleanFilename(Source) != TEXT("Input.ini")) continue;
            const FString Destination = NewSaved / Folder / Relative;
            if (Files.FileExists(*Destination)) continue;
            Files.MakeDirectory(*FPaths::GetPath(Destination), true);
            if (Files.Copy(*Destination, *Source, false) != COPY_OK) continue;
            ++Copied;
            // Config defaults may already be cached when the plugin starts.
            if (GConfig)
                if (FConfigFile* Config = GConfig->FindConfigFile(Destination)) Config->Combine(Destination);
        }
    }
    const FString AtlasFile = TEXT("ClientPlugins/Waypoint/locations.json");
    const FString SourceAtlas = OldSaved / AtlasFile;
    const FString DestinationAtlas = NewSaved / AtlasFile;
    if (!Files.FileExists(*DestinationAtlas) && Files.FileExists(*SourceAtlas))
    {
        // Stage beside the destination. Windows MoveFile publishes only if the
        // name is still free, including when another client imports meanwhile.
        const FString Temp = DestinationAtlas + TEXT(".") + FGuid::NewGuid().ToString() + TEXT(".tmp");
        if (Files.Copy(*Temp, *SourceAtlas) == COPY_OK &&
            FPlatformFileManager::Get().GetPlatformPhysical().MoveFile(*DestinationAtlas, *Temp))
        {
            ++Copied;
        }
        else
        {
            Files.Delete(*Temp);
        }
    }
    return Copied;
}

void ACEProjectMigration::ImportWindowsProfile()
{
#if PLATFORM_WINDOWS
    const FString Destination = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
    // Compatibility identifiers only: old releases remain readable after the rename.
    const FString OldProject = TEXT("ACEViewer");
    const FString Adjacent = FPaths::ConvertRelativePathToFull(Destination / TEXT("../..") / OldProject / TEXT("Saved"));
    int32 Copied = CopyMissingProfileFiles(Adjacent, Destination);
    Copied += CopyMissingProfileFiles(FString(FPlatformProcess::UserSettingsDir()) / OldProject / TEXT("Saved"), Destination);
    if (Copied) UE_LOG(LogTemp, Log, TEXT("AC:Unreal imported %d existing profile files without replacing current settings."), Copied);
#endif
}
