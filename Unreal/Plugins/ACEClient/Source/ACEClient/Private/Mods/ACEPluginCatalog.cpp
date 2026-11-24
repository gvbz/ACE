#include "Mods/ACEPluginCatalog.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEUpdateSubsystem.h"
#include "Engine/GameInstance.h"
#include "XmlFile.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#elif PLATFORM_ANDROID
#include <sys/stat.h>
#endif
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace ACEPluginCatalog
{
namespace
{
bool Token(const FString& S, int32 Limit, bool Dots=false)
{
    if (S.IsEmpty() || S.Len()>Limit || S==TEXT(".") || S==TEXT("..")) return false;
    for (TCHAR C:S)
        if (!(C<128 && (FChar::IsAlnum(C) || C=='_' || C=='-' || (Dots && C=='.')))) return false;
    return true;
}
bool DeviceName(const FString& S)
{
    FString Stem=S.ToUpper(),Tail;
    S.ToUpper().Split(TEXT("."),&Stem,&Tail);
    return Stem==TEXT("CON") || Stem==TEXT("PRN") || Stem==TEXT("AUX") || Stem==TEXT("NUL")
        || (Stem.Len()==4 && (Stem.StartsWith(TEXT("COM")) || Stem.StartsWith(TEXT("LPT"))) && FChar::IsDigit(Stem[3]));
}
bool ValidId(const FString& Id)
{
    return Token(Id,64) && Id[0]>='a' && Id[0]<='z' && Id.Equals(Id.ToLower(),ESearchCase::CaseSensitive) && !DeviceName(Id);
}
bool Hex(const FString& S, int32 Length)
{
    if (S.Len()!=Length) return false;
    for (TCHAR C:S) if (!FChar::IsHexDigit(C)) return false;
    return true;
}
FString Content(const FXmlNode* Node,const TCHAR* Name)
{
    const auto* Child=Node->FindChildNode(Name);
    FString S=Child?Child->GetContent().TrimStartAndEnd():FString();
    return S.Replace(TEXT("&lt;"),TEXT("<")).Replace(TEXT("&gt;"),TEXT(">")).Replace(TEXT("&quot;"),TEXT("\""))
        .Replace(TEXT("&apos;"),TEXT("'")).Replace(TEXT("&amp;"),TEXT("&"));
}
FString Base(const FString& Root,const FString& Id) { return Root/TEXT("Catalog/Packages")/Id; }
TSharedPtr<FJsonObject> Json(const TArray<uint8>& Bytes)
{
    if(Bytes.IsEmpty())return nullptr;
    FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
    TSharedPtr<FJsonObject> Obj;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(),Text.Get())),Obj);
    return Obj;
}
bool NativePanel(const FString& Id)
{
    static const TSet<FString> Panels={TEXT("ucmmicro"),TEXT("looteditor"),TEXT("waypoint"),TEXT("actiondeck"),TEXT("autocraft"),TEXT("huntledger"),TEXT("supplyledger")};
    return Panels.Contains(Id);
}
int32 FileLimit(const FString& Path,const FString& OfficialId)
{
    if(OfficialId==TEXT("autocraft")&&Path==TEXT("catalog.json"))return 8*1024*1024;
    if((OfficialId==TEXT("autocraft")&&(Path==TEXT("NOTICE.txt")||Path==TEXT("LICENSE")))||
        (OfficialId==TEXT("ucm")&&Path==TEXT("monster-elements.json"))||
        (OfficialId==TEXT("thirdeye")&&Path==TEXT("sounds/README.txt"))||
        (OfficialId==TEXT("waypoint")&&(Path==TEXT("MapRegions.xml")||Path==TEXT("locations.xml")||Path==TEXT("LICENSE_GOARROW.txt"))))return 2*1024*1024;
    if(Path.Equals(TEXT("plugin.json"),ESearchCase::CaseSensitive) || Path.Equals(TEXT("default.json"),ESearchCase::CaseSensitive))return 2*1024*1024;
    if(Path.Equals(TEXT("main.lua"),ESearchCase::CaseSensitive))return 256*1024;
    FString Dir,Name;
    if(!Path.Split(TEXT("/"),&Dir,&Name) || Name.IsEmpty() || Name.Len()>96 || DeviceName(Name))return 0;
    for(TCHAR C:Name)if(!(C<128&&(FChar::IsAlnum(C)||C=='_'||C=='-'||C=='.'||C==' ')))return 0;
    if(!FChar::IsAlnum(Name[0])&&Name[0]!='_'&&Name[0]!='-')return 0;
    if(Dir.Equals(TEXT("data"),ESearchCase::CaseSensitive)&&IsDataFile(Name))return 8*1024*1024;
    if(Dir.Equals(TEXT("images"),ESearchCase::CaseSensitive) && Name.EndsWith(TEXT(".png")))return 2*1024*1024;
    if(Dir.Equals(TEXT("sounds"),ESearchCase::CaseSensitive) && Name.EndsWith(TEXT(".wav")))return 1024*1024;
    return 0;
}
bool PublishReceipt(const FString& Dir,const FString& Revision,const FString& Digest)
{
    const FString Target=Dir/TEXT("active.txt"), Temp=Dir/(Revision+TEXT(".tmp"));
    ON_SCOPE_EXIT{IFileManager::Get().Delete(*Temp);};
    if(!FFileHelper::SaveStringToFile(Revision+TEXT(" ")+Digest,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return false;
#if PLATFORM_WINDOWS
    auto Physical=[](const FString& Path)
    {
        FString Full=FPaths::ConvertRelativePathToFull(Path);
        FPaths::MakePlatformFilename(Full);
        if(Full.StartsWith(TEXT("\\\\?\\")))return Full;
        return Full.StartsWith(TEXT("\\\\"))?TEXT("\\\\?\\UNC\\")+Full.Mid(2):TEXT("\\\\?\\")+Full;
    };
    return !!::MoveFileExW(*Physical(Temp),*Physical(Target),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
#else
    return FPlatformFileManager::Get().GetPlatformPhysical().MoveFile(*Target,*Temp);
#endif
}
}

bool IsOfficial(const FEntry& E)
{
    return E.Repo==TEXT("https://github.com/Thwargle/ACE")&&E.Tag.StartsWith(TEXT("plugins-"))&&Hex(E.Tag.Mid(8),40);
}
bool Parse(const FString& Xml,TArray<FEntry>& Entries,FString& Error)
{
    Error=TEXT("Invalid plugin catalog. Your installed plugins are unchanged.");
    if(Xml.IsEmpty()||Xml.Len()>MaxIndexBytes||Xml.Contains(TEXT("<!DOCTYPE"))||Xml.Contains(TEXT("<!ENTITY")))return false;
    FXmlFile File(Xml,EConstructMethod::ConstructFromBuffer);
    const auto* Root=File.GetRootNode();
    if(!File.IsValid()||!Root||Root->GetTag()!=TEXT("catalog")||Root->GetAttribute(TEXT("schema"))!=TEXT("1")||Root->GetChildrenNodes().Num()>512)return false;
    TSet<FString> Ids;
    TArray<FEntry> Parsed;
    for(const FXmlNode* Node:Root->GetChildrenNodes())
    {
        FEntry E;
        E.Id=Node->GetAttribute(TEXT("id")); E.Repo=Node->GetAttribute(TEXT("repository"));
        E.Name=Content(Node,TEXT("name")); E.Description=Content(Node,TEXT("description"));
        FString Slug=E.Repo,Owner,Repo;
        if(Node->GetTag()!=TEXT("plugin")||!ValidId(E.Id)||Ids.Contains(E.Id)||E.Name.IsEmpty()||E.Name.Len()>80||E.Description.IsEmpty()||E.Description.Len()>600||
            !Slug.RemoveFromStart(TEXT("https://github.com/"),ESearchCase::CaseSensitive)||!Slug.Split(TEXT("/"),&Owner,&Repo)||!Token(Owner,100,true)||!Token(Repo,100,true))return false;
        E.Developer=Content(Node,TEXT("developer"));
        if(E.Developer.Len()>80)return false;
        if(E.Developer.IsEmpty())E.Developer=Owner; // Cached schema-1 listings predate developer attribution.
        const auto* Release=Node->FindChildNode(TEXT("release"));
        if(!Release)return false;
        E.Tag=Release->GetAttribute(TEXT("tag")); E.Digest=Release->GetAttribute(TEXT("sha256")); E.Download=Release->GetAttribute(TEXT("download"));
        E.Version=Release->GetAttribute(TEXT("version")).TrimStartAndEnd();
        if(E.Version.Len()>96)return false;
        const FString Size=Release->GetAttribute(TEXT("bytes"));
        if(!Token(E.Tag,96,true)||!Hex(E.Digest,64)||Size.Len()>8||!LexTryParseString(E.Bytes,*Size)||E.Bytes<=0||E.Bytes>MaxPackageBytes||
            !E.Download.Equals(E.Repo/TEXT("releases/download")/E.Tag/(E.Id+TEXT(".acplugin")),ESearchCase::CaseSensitive))return false;
        const FString Logo=Content(Node,TEXT("logo"));
        if(!Logo.IsEmpty() && (Logo.Len()>87384 || !FBase64::Decode(Logo,E.Logo) || E.Logo.Num()>65536))return false;
        Ids.Add(E.Id); Parsed.Add(MoveTemp(E));
    }
    Entries=MoveTemp(Parsed); Error.Reset(); return true;
}

bool IsLinked(const FString& Path)
{
    auto& Physical=IPlatformFile::GetPlatformPhysical();
#if PLATFORM_ANDROID
    const FString Local=Physical.ConvertToAbsolutePathForExternalAppForWrite(*Path);
    struct stat Info;
    return lstat(TCHAR_TO_UTF8(*Local),&Info)==0&&S_ISLNK(Info.st_mode);
#else
    return Physical.IsSymlink(*Path)==ESymlinkResult::Symlink;
#endif
}

bool IsDataFile(const FString& Name)
{
    if(Name.IsEmpty()||Name.Len()>95||DeviceName(Name)||!(FChar::IsAlnum(Name[0])||Name[0]=='_'||Name[0]=='-'))return false;
    for(TCHAR C:Name)if(!(C<128&&(FChar::IsAlnum(C)||C=='_'||C=='-'||C==' '||C=='.')))return false;
    return Name.EndsWith(TEXT(".json"))||Name.EndsWith(TEXT(".xml"))||Name.EndsWith(TEXT(".txt"));
}

FString ActiveDirectory(const FString& Root,const FString& Id,FString* Digest)
{
    if(!ValidId(Id))return {};
    const FString Dir=Base(Root,Id), File=Dir/TEXT("active.txt");
    FString Data,Revision,Sha;
    if(IFileManager::Get().FileSize(*File)!=97 || !FFileHelper::LoadFileToString(Data,*File) || !Data.Split(TEXT(" "),&Revision,&Sha)||!Hex(Revision,32)||!Hex(Sha,64))return {};
    if(Digest)*Digest=Sha;
    return Dir/Revision;
}

// Publishers can use arbitrary version labels. Only infer an ordering for dotted
// numeric versions. Exact labels and managed package digests work with any scheme.
static TOptional<int32> CompareVersions(FString Installed,FString Latest)
{
    Installed.TrimStartAndEndInline();
    Latest.TrimStartAndEndInline();
    if(Installed.IsEmpty()||Latest.IsEmpty())return {};
    if(Installed.Equals(Latest,ESearchCase::CaseSensitive))return 0;
    auto Components=[](FString Version,TArray<FString>& Parts)
    {
        Version.RemoveFromStart(TEXT("v"),ESearchCase::IgnoreCase);
        Version.ParseIntoArray(Parts,TEXT("."),false);
        if(Parts.IsEmpty())return false;
        for(FString& Part:Parts)
        {
            if(Part.IsEmpty())return false;
            for(TCHAR Digit:Part)if(Digit<'0'||Digit>'9')return false;
            int32 Start=0;
            while(Start+1<Part.Len()&&Part[Start]=='0')++Start;
            Part.RightChopInline(Start);
        }
        return true;
    };
    TArray<FString> Left,Right;
    if(!Components(Installed,Left)||!Components(Latest,Right))return {};
    for(int32 I=0;I<FMath::Max(Left.Num(),Right.Num());++I)
    {
        const FString A=Left.IsValidIndex(I)?Left[I]:TEXT("0"), B=Right.IsValidIndex(I)?Right[I]:TEXT("0");
        const int32 Difference=A.Len()==B.Len()?A.Compare(B):A.Len()-B.Len();
        if(Difference)return Difference<0?-1:1;
    }
    return 0;
}

FInstallInfo Inspect(const UACEPluginSubsystem& Host,const FEntry& E)
{
    FInstallInfo Info;
    FString Digest;
    const FString Root=Host.UserDirectory(), Active=ActiveDirectory(Root,E.Id,&Digest);
    Info.HasReceipt=!Active.IsEmpty();
    const auto* Match=Host.Plugins.FindByPredicate([&](const auto& P){return P->Id.Equals(E.Id,ESearchCase::IgnoreCase);});
    if(Match)Info.InstalledVersion=(*Match)->Version;
    const bool Manual=Match&&FPaths::IsUnderDirectory((*Match)->Directory,Root/TEXT("Installed"));
    const auto Order=CompareVersions(Info.InstalledVersion,E.Version.IsEmpty()?E.Tag:E.Version);
    if(E.Download.IsEmpty())Info.State=EInstallState::Delisted;
    else if(Match&&!Manual&&!FPaths::IsUnderDirectory((*Match)->Directory,Base(Root,E.Id)))Info.State=EInstallState::Bundled;
    else if(Manual)
    {
        if(!Order.IsSet())Info.State=EInstallState::ManualUnknown;
        else if(Order.GetValue()>0)Info.State=EInstallState::Newer;
        else Info.State=Order.GetValue()==0?EInstallState::ManualCurrent:EInstallState::ManualUpdate;
    }
    else if(Info.HasReceipt)
    {
        if(!Match||(*Match)->Directory!=Active||(((*Match)->Api>=2||!IsOfficial(E)||!NativePanel(E.Id))&&!IFileManager::Get().FileExists(*(Active/TEXT("main.lua"))))
            ||!IFileManager::Get().FileExists(*(Active/TEXT("plugin.json"))))Info.State=EInstallState::Repair;
        else if(Digest.Equals(E.Digest,ESearchCase::IgnoreCase))Info.State=EInstallState::Current;
        else if(Order.IsSet()&&Order.GetValue()>0)Info.State=EInstallState::Newer;
        else Info.State=EInstallState::Update;
    }
    return Info;
}

bool Install(UACEPluginSubsystem& Host,const FEntry& E,const TArray<uint8>& Package,FString& Error,bool AdoptManual,bool RestoreLegacy)
{
    Error=TEXT("The release failed size or SHA-256 verification.");
    uint8 Hash[EVP_MAX_MD_SIZE]; unsigned HashSize=0;
    if(!ValidId(E.Id)||Package.IsEmpty()||Package.Num()!=E.Bytes||Package.Num()>MaxPackageBytes||
        EVP_Digest(Package.GetData(),Package.Num(),Hash,&HashSize,EVP_sha256(),nullptr)!=1||HashSize!=32||!E.Digest.Equals(BytesToHex(Hash,HashSize),ESearchCase::IgnoreCase))return false;
    if(!Host.GetGameInstance()->GetSubsystem<UACEUpdateSubsystem>()->CanUseLobby()){Error=TEXT("Return to the account screen to install plugins.");return false;}
    static const TSet<FString> OfficialIds={TEXT("ucm"),TEXT("ucmmicro"),TEXT("looteditor"),TEXT("autocraft"),TEXT("actiondeck"),TEXT("waypoint"),TEXT("supplyledger"),TEXT("huntledger"),TEXT("derethlens"),TEXT("foundry"),TEXT("thirdeye"),TEXT("itemtools")};
    if(OfficialIds.Contains(E.Id)&&!IsOfficial(E)){Error=TEXT("This plugin ID belongs to AC:Unreal.");return false;}
    const FString Root=Host.UserDirectory(), Old=ActiveDirectory(Root,E.Id);
    for(const auto& Existing:Host.Plugins)
        if(Existing->Id.Equals(E.Id,ESearchCase::IgnoreCase)&&Existing->Directory!=Old&&
            !(AdoptManual&&FPaths::IsUnderDirectory(Existing->Directory,Root/TEXT("Installed"))))
        {Error=TEXT("This ID belongs to a bundled or separately installed plugin.");return false;}
    const auto O=Json(Package);
    const TSharedPtr<FJsonObject>* Files=nullptr;
    double Format=0;
    Error=TEXT("Unsupported plugin package.");
    if(!O||!O->TryGetNumberField(TEXT("format"),Format)||Format!=1||!O->TryGetObjectField(TEXT("files"),Files)||(*Files)->Values.Num()>128)return false;
    TMap<FString,TArray<uint8>> Data; TSet<FString> Names;
    for(const auto& Pair:(*Files)->Values)
    {
        const FString Path(Pair.Key);
        const int32 Limit=FileLimit(Path,IsOfficial(E)?E.Id:FString());
        FString Encoded; TArray<uint8> Bytes;
        if(!Limit||Names.Contains(Path.ToLower())||!Pair.Value||Pair.Value->Type!=EJson::String||!Pair.Value->TryGetString(Encoded)||
            Encoded.Len()>((Limit+2)/3)*4||!FBase64::Decode(Encoded,Bytes)||Bytes.IsEmpty()||Bytes.Num()>Limit)return false;
        Names.Add(Path.ToLower()); Data.Add(Path,MoveTemp(Bytes));
    }
    if(!Data.Contains(TEXT("plugin.json"))||(!(IsOfficial(E)&&NativePanel(E.Id))&&!Data.Contains(TEXT("main.lua")))||(Data.Contains(TEXT("default.json"))&&!Json(Data[TEXT("default.json")])))return false;
    const auto Plugin=Host.ParsePluginManifest(Json(Data[TEXT("plugin.json")]),Error);
    if(!Plugin||!Plugin->Id.Equals(E.Id,ESearchCase::CaseSensitive)||Plugin->LocalService){if(Error.IsEmpty())Error=TEXT("The manifest ID, mode or API is incompatible.");return false;}
    if(Plugin->Api>=2&&!Data.Contains(TEXT("main.lua"))){Error=TEXT("API 2 plugins require a Lua entry point.");return false;}
    if(!E.Version.IsEmpty()&&!Plugin->Version.TrimStartAndEnd().Equals(E.Version,ESearchCase::CaseSensitive))
    {Error=TEXT("The package version differs from the catalog listing.");return false;}
    const FString Dir=Base(Root,E.Id),Revision=FGuid::NewGuid().ToString(EGuidFormats::Digits),Stage=Dir/Revision;
    auto& FS=IFileManager::Get();
    for(const FString& Parent:{Root,Root/TEXT("Catalog"),Root/TEXT("Catalog/Packages"),Dir,Stage})
        if(IsLinked(Parent)){Error=TEXT("Cannot install through a linked plugin directory.");return false;}
    bool Published=false;
    ON_SCOPE_EXIT{if(!Published)FS.DeleteDirectory(*Stage,false,true);};
    Error=TEXT("Could not save the plugin. Check available disk space and retry.");
    for(const auto& Pair:Data)
    {
        const FString Path=Stage/Pair.Key;
        if(!FS.MakeDirectory(*FPaths::GetPath(Path),true)||!FFileHelper::SaveArrayToFile(Pair.Value,*Path))return false;
    }
    if(IsOfficial(E)&&!FFileHelper::SaveStringToFile(E.Tag,*(Stage/TEXT("official.txt"))))return false;
    if(!PublishReceipt(Dir,Revision,E.Digest))return false;
    Published=true; Host.Discover();
    // A stale grant from a former manual installation must not enable a new publisher.
    if(Old.IsEmpty()&&!(RestoreLegacy&&IsOfficial(E)))Host.SetEnabled(E.Id,false);
    Error.Reset();
    // Another running client may still be reading the previous revision. Only
    // collect explicitly retired revisions so concurrent downloads stay intact.
    if(!Old.IsEmpty())FFileHelper::SaveStringToFile(TEXT("retired"),*(Old/TEXT("retired.txt")));
    TArray<FString> Versions; FS.FindFiles(Versions,*(Dir/TEXT("*")),false,true);
    for(const FString& V:Versions)
    {
        const FString Retired=Dir/V/TEXT("retired.txt");
        if(Hex(V,32)&&!IsLinked(Dir/V)&&FS.FileExists(*Retired)&&FS.GetTimeStamp(*Retired)<FDateTime::UtcNow()-FTimespan::FromDays(7)&&Dir/V!=ActiveDirectory(Root,E.Id)
            &&IPlatformFile::GetPlatformPhysical().IterateDirectoryRecursively(*(Dir/V),[](const TCHAR* Child,bool){return !IsLinked(Child);}))
            FS.DeleteDirectory(*(Dir/V),false,true);
    }
    return true;
}

bool Remove(UACEPluginSubsystem& Host,const FString& Id,FString& Error)
{
    if(!Token(Id,64)||!Host.GetGameInstance()->GetSubsystem<UACEUpdateSubsystem>()->CanUseLobby())
    {Error=TEXT("Return to the account screen to uninstall this plugin.");return false;}
    auto& FS=IFileManager::Get();
    auto& Physical=IPlatformFile::GetPlatformPhysical();
    const FString Root=FPaths::ConvertRelativePathToFull(Host.UserDirectory());
    const FString Manual=Root/TEXT("Installed"),Catalog=Root/TEXT("Catalog"),Packages=Catalog/TEXT("Packages");
    if(IsLinked(Root)||IsLinked(Manual)||IsLinked(Catalog)||IsLinked(Packages))
    {Error=TEXT("Cannot uninstall through a linked plugin directory.");return false;}
    TArray<FString> Dirs,Targets;
    FS.FindFiles(Dirs,*(Manual/TEXT("*")),false,true);
    for(const auto& Dir:Dirs)
    {
        const FString Path=Manual/Dir,Manifest=Path/TEXT("plugin.json");
        if(IsLinked(Path)||IsLinked(Manifest)||FS.FileSize(*Manifest)>2*1024*1024)continue;
        TArray<uint8> Bytes;
        if(!FFileHelper::LoadFileToArray(Bytes,*Manifest))continue;
        const auto M=Json(Bytes);FString ManifestId;
        if(M&&M->TryGetStringField(TEXT("id"),ManifestId)&&ManifestId==Id)Targets.Add(Path);
    }
    if(ValidId(Id)&&FS.DirectoryExists(*Base(Root,Id)))Targets.Add(Base(Root,Id));
    if(Targets.IsEmpty()){Error=TEXT("No installed plugin files were found.");return false;}
    for(const auto& Path:Targets)
    {
        bool Safe=!IsLinked(Path);
        if(Safe)Safe=Physical.IterateDirectoryRecursively(*Path,[](const TCHAR* Child,bool){return !IsLinked(Child);});
        if(!Safe){Error=TEXT("The plugin folder contains a link or unreadable entry. Check the folder and retry.");return false;}
    }
    const FString Legacy=Catalog/TEXT("Legacy");
    if(FS.DirectoryExists(*Legacy)&&(IsLinked(Legacy)||IsLinked(Legacy/Id)||!FFileHelper::SaveStringToFile(TEXT("removed"),*(Legacy/Id))))
    {Error=TEXT("Could not save the uninstall choice.");return false;}
    Host.SetEnabled(Id,false);
    bool Removed=true;
    for(const auto& Path:Targets)Removed=FS.DeleteDirectory(*Path,false,true)&&Removed;
    Host.Discover();
    Error=Removed?FString():TEXT("Some plugin files could not be removed. Close other clients and retry.");
    return Removed;
}
}
