#pragma once

#include "CoreMinimal.h"

class UACEPluginSubsystem;
class SWidget;
class IHttpRequest;

namespace ACEPluginCatalog
{
    inline constexpr int32 MaxIndexBytes = 4 * 1024 * 1024;
    inline constexpr int32 MaxPackageBytes = 16 * 1024 * 1024;
    inline constexpr TCHAR Repository[] = TEXT("https://github.com/Thwargle/ACUnrealPlugins");
    inline constexpr TCHAR IndexURL[] = TEXT("https://raw.githubusercontent.com/Thwargle/ACUnrealPlugins/main/catalog.xml");

    struct FEntry
    {
        FString Id, Name, Developer, Description, Repo, Tag, Version, Download, Digest;
        TArray<uint8> Logo;
        int32 Bytes = 0;
    };
    bool Parse(const FString& Xml, TArray<FEntry>& Entries, FString& Error);
    FString ActiveDirectory(const FString& UserRoot, const FString& Id, FString* Digest=nullptr);
    enum class EInstallState { Available, Current, Update, Repair, ManualCurrent, ManualUpdate, ManualUnknown, Newer, Bundled, Delisted };
    struct FInstallInfo
    {
        EInstallState State = EInstallState::Available;
        FString InstalledVersion;
        bool HasReceipt = false;
    };
    FInstallInfo Inspect(const UACEPluginSubsystem& Host,const FEntry& Entry);
    bool IsOfficial(const FEntry& Entry);
    bool IsDataFile(const FString& Name);
    bool IsLinked(const FString& Path);
    void Fetch(TSharedPtr<IHttpRequest,ESPMode::ThreadSafe>& Request,const FString& URL,int32 Limit,TFunction<void(const TArray<uint8>*)> Complete);
    class FLegacyRestore : public TSharedFromThis<FLegacyRestore>
    {
    public:
        static TSharedPtr<FLegacyRestore> Prepare(UACEPluginSubsystem& Host);
        void Tick(UACEPluginSubsystem& Host);
        ~FLegacyRestore();
    private:
        TArray<FEntry> Pending;
        TSharedPtr<IHttpRequest,ESPMode::ThreadSafe> Request;
        double RetryAt=0;
    };
    bool Install(UACEPluginSubsystem& Host, const FEntry& Entry, const TArray<uint8>& Package, FString& Error, bool AdoptManual=false,bool RestoreLegacy=false);
    bool Remove(UACEPluginSubsystem& Host, const FString& Id, FString& Error);
    TSharedRef<SWidget> MakeBrowser(UACEPluginSubsystem* Host);
}
