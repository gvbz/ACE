#include "Mods/ACEPluginCatalog.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEUpdateSubsystem.h"
#include "Engine/GameInstance.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

namespace ACEPluginCatalog
{
void Fetch(FHttpRequestPtr& Request,const FString& URL,int32 Limit,TFunction<void(const TArray<uint8>*)> Complete)
{
    const auto Bytes=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();
    const auto Overflow=MakeShared<TAtomic<bool>,ESPMode::ThreadSafe>(false);
    const auto R=FHttpModule::Get().CreateRequest();Request=R;
    R->SetURL(URL);R->SetVerb(TEXT("GET"));R->SetTimeout(45);
    R->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    R->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([Bytes,Overflow,Limit](void* Data,int64& N)
    {
        if(N<0||N>Limit-Bytes->Num()){Overflow->Store(true);N=0;return;}
        if(N)Bytes->Append(static_cast<const uint8*>(Data),int32(N));
    }));
    R->OnProcessRequestComplete().BindLambda([Bytes,Overflow,URL,Done=MoveTemp(Complete)](FHttpRequestPtr,FHttpResponsePtr Response,bool OK) mutable
    {
        if(!Done)return;
        auto Finish=MoveTemp(Done);
        const FString Final=Response?Response->GetEffectiveURL():FString();
        const bool Allowed=Final==URL||(URL!=IndexURL&&Final.StartsWith(TEXT("https://release-assets.githubusercontent.com/")));
        Finish(OK&&Response&&Response->GetResponseCode()==200&&!Overflow->Load()&&Allowed?&*Bytes:nullptr);
    });
    if(!R->ProcessRequest()&&Request==R)R->OnProcessRequestComplete().ExecuteIfBound(R,nullptr,false);
}

TSharedPtr<FLegacyRestore> FLegacyRestore::Prepare(UACEPluginSubsystem& Host)
{
    const FString Root=Host.UserDirectory(),Dir=Root/TEXT("Catalog/Legacy"),Marker=Dir/TEXT("origin.txt");
    auto& FS=IFileManager::Get();
    FString Origin;
    if(!FFileHelper::LoadFileToString(Origin,*Marker))
    {
        const bool Existing=FS.FileExists(*(Root/TEXT("settings.json")))||FS.DirectoryExists(*(Root/TEXT("Profiles")))||
            FS.FileExists(*(Root/TEXT("ucm-log.json")))||FS.FileExists(*(FPaths::GetPath(Root)/TEXT("Login/LastLogin.dat")))||
            FS.FileExists(*(FPaths::ProjectDir()/TEXT("legacy-client-install.txt")));
        FS.MakeDirectory(*Dir,true);
        FFileHelper::SaveStringToFile(Existing?TEXT("upgrade"):TEXT("fresh"),*Marker,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&FS,FILEWRITE_NoReplaceExisting);
        if(!FFileHelper::LoadFileToString(Origin,*Marker))return nullptr;
    }
    if(Origin!=TEXT("upgrade"))return nullptr;
    FString XML,Error;
    const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
    auto Restore=MakeShared<FLegacyRestore>();
    if(!Plugin||!FFileHelper::LoadFileToString(XML,*(Plugin->GetBaseDir()/TEXT("ClientMods/legacy-plugins.xml")))||!Parse(XML,Restore->Pending,Error))return nullptr;
    Restore->Pending.RemoveAll([&](const FEntry& E){return !IsOfficial(E)||FS.FileExists(*(Dir/E.Id));});
    if(Restore->Pending.IsEmpty())return nullptr;
    return Restore;
}

FLegacyRestore::~FLegacyRestore(){if(Request)Request->CancelRequest();}

void FLegacyRestore::Tick(UACEPluginSubsystem& Host)
{
    if(Request||Pending.IsEmpty()||FPlatformTime::Seconds()<RetryAt||
       !Host.GetGameInstance()->GetSubsystem<UACEUpdateSubsystem>()->CanUseLobby())return;
    const FEntry E=Pending.Pop();
    const FString Stamp=Host.UserDirectory()/TEXT("Catalog/Legacy")/E.Id;
    if(IFileManager::Get().FileExists(*Stamp))return;
    if(Host.Plugins.ContainsByPredicate([&](const auto& P){return P->Id==E.Id;}))
    {
        FFileHelper::SaveStringToFile(TEXT("installed"),*Stamp);
        return;
    }
    const TWeakPtr<FLegacyRestore> Weak=AsShared();
    const TWeakObjectPtr<UACEPluginSubsystem> Owner=&Host;
    Fetch(Request,E.Download,E.Bytes,[Weak,Owner,E,Stamp](const TArray<uint8>* Bytes)
    {
        const auto Self=Weak.Pin();if(!Self||!Owner.IsValid())return;
        Self->Request.Reset();
        if(IFileManager::Get().FileExists(*Stamp))return;
        FString Error;
        if(Bytes&&Install(*Owner.Get(),E,*Bytes,Error,false,true))FFileHelper::SaveStringToFile(TEXT("installed"),*Stamp);
        else
        {
            Self->Pending.Insert(E,0);
            Self->RetryAt=FPlatformTime::Seconds()+30;
        }
    });
}
}
