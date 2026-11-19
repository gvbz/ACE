#include "Mods/ACEPluginSubsystem.h"
#include "ACERuntimeOptions.h"
#include "Async/Async.h"
#include "Audio.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <shobjidl.h>
#elif PLATFORM_LINUX
#include <SDL3/SDL.h>
#endif

namespace ACEPluginSoundPrivate
{
    constexpr int64 MaxBytes=1024*1024;
    bool Filename(const FString& Name)
    {
        if(Name.IsEmpty()||Name.Len()>128||Name!=FPaths::GetCleanFilename(Name)
            ||Name.Contains(TEXT(":"))||Name.Contains(TEXT("/"))||Name.Contains(TEXT("\\"))
            ||!Name.EndsWith(TEXT(".wav"),ESearchCase::IgnoreCase))return false;
        for(TCHAR C:Name)if(FChar::IsControl(C))return false;
        return true;
    }
    bool CustomToken(const FString& Name)
    {
        if(Name.Len()!=50||!Name.StartsWith(TEXT("sound-"))||!Name.EndsWith(TEXT(".wav")))return false;
        for(int32 I=6;I<46;++I)if(!FChar::IsHexDigit(Name[I]))return false;
        return true;
    }
    bool CompletePCMFile(const TArray<uint8>& Data)
    {
        if(Data.Num()<44||FMemory::Memcmp(Data.GetData(),"RIFF",4)||FMemory::Memcmp(Data.GetData()+8,"WAVE",4))return false;
        const auto U16=[&](uint64 At){return uint32(Data[int32(At)])|(uint32(Data[int32(At+1)])<<8);};
        const auto U32=[&](uint64 At){return U16(At)|(U16(At+2)<<16);};
        const uint64 End=uint64(U32(4))+8;
        if(End!=uint64(Data.Num()))return false;
        bool HaveFormat=false,HaveData=false;uint32 Channels=0,SampleBytes=0;
        uint64 At=12;
        while(At<End)
        {
            if(End-At<8)return false;
            const uint32 Size=U32(At+4);const uint64 Body=At+8;
            if(uint64(Size)>End-Body)return false;
            if(!FMemory::Memcmp(Data.GetData()+At,"fmt ",4))
            {
                if(HaveFormat||Size<16)return false;
                const uint32 Format=U16(Body),Rate=U32(Body+4);Channels=U16(Body+2);
                if((Format!=1&&Format!=0xfffe)||(Format==0xfffe&&Size<40)||Channels<1||Channels>2
                    ||Rate<8000||Rate>48000||U16(Body+14)!=16||U16(Body+12)!=Channels*2||U32(Body+8)!=Rate*Channels*2)return false;
                HaveFormat=true;
            }
            else if(!FMemory::Memcmp(Data.GetData()+At,"data",4))
            {
                if(HaveData)return false;
                SampleBytes=Size;HaveData=true;
            }
            At=Body+uint64(Size)+(Size&1u);
            if(At>End)return false;
        }
        return HaveFormat&&HaveData&&SampleBytes&&SampleBytes%(Channels*2u)==0;
    }
    bool LoadWave(const FString& Path,TArray<uint8>& Data,FWaveModInfo& Wave,FString& Error)
    {
        const int64 Size=IFileManager::Get().FileSize(*Path);
        if(Size<0){Error=TEXT("Sound file was not found. Choose it again or use the default chime.");return false;}
        if(Size<44||Size>MaxBytes||!FFileHelper::LoadFileToArray(Data,*Path)
            ||Data.Num()<44||Data.Num()>MaxBytes||!CompletePCMFile(Data)||!Wave.ReadWaveInfo(Data.GetData(),Data.Num(),nullptr,true)
            ||!Wave.pFormatTag||*Wave.pFormatTag!=1||!Wave.pChannels||*Wave.pChannels<1||*Wave.pChannels>2
            ||!Wave.pBitsPerSample||*Wave.pBitsPerSample!=16||!Wave.pSamplesPerSec||*Wave.pSamplesPerSec<8000||*Wave.pSamplesPerSec>48000
            ||!Wave.SampleDataStart||!Wave.SampleDataSize||Wave.SampleDataSize%(*Wave.pChannels*2u)!=0
            ||Wave.SampleDataSize>10u*(*Wave.pSamplesPerSec)*(*Wave.pChannels)*2u)
        {Error=TEXT("Choose a PCM16 WAV: mono or stereo, 8–48 kHz, up to 10 seconds and 1 MiB.");return false;}
        // A declared data chunk may extend beyond a truncated RIFF file. Never
        // pass those bytes to the audio thread, including when replaying imports.
        const UPTRINT Start=reinterpret_cast<UPTRINT>(Wave.SampleDataStart);
        const UPTRINT Begin=reinterpret_cast<UPTRINT>(Data.GetData());
        if(Start<Begin||Start-Begin>uint64(Data.Num())||uint64(Wave.SampleDataSize)>uint64(Data.Num())-(Start-Begin))
        {Error=TEXT("The WAV is truncated or has an invalid sample-data chunk.");return false;}
        return true;
    }
}

bool UACEPluginSubsystem::ImportPluginSound(FACEClientPlugin& P,const FString& SourcePath,const TSharedPtr<FJsonObject>& Reply,FString& Error)
{
    if(!P.Permissions.Contains(TEXT("sound"))){Error=TEXT("Missing permission: sound");return false;}
    if(!FPaths::GetExtension(SourcePath).Equals(TEXT("wav"),ESearchCase::IgnoreCase))
    {Error=TEXT("Choose a WAV sound file.");return false;}
    TArray<uint8> Data;FWaveModInfo Wave;
    if(!ACEPluginSoundPrivate::LoadWave(SourcePath,Data,Wave,Error))return false;
    const FString Asset=TEXT("sound-")+FSHA1::HashBuffer(Data.GetData(),Data.Num()).ToString()+TEXT(".wav");
    const FString Directory=UserDirectory()/TEXT("Sounds")/P.Id,Path=Directory/Asset;
    IFileManager::Get().MakeDirectory(*Directory,true);
    const FString Temporary=Path+TEXT(".tmp");
    if(!FFileHelper::SaveArrayToFile(Data,*Temporary)||!IFileManager::Get().Move(*Path,*Temporary,true,true))
    {
        IFileManager::Get().Delete(*Temporary,false,true);
        Error=TEXT("Could not save the chosen sound. Your previous sound is unchanged.");return false;
    }
    Reply->SetStringField(TEXT("asset"),Asset);
    FString Label;
    for(TCHAR Ch:FPaths::GetCleanFilename(SourcePath))if(!FChar::IsControl(Ch))Label.AppendChar(Ch);
    Reply->SetStringField(TEXT("name"),Label.Left(256));
    Reply->SetStringField(TEXT("source"),TEXT("custom"));
    return true;
}

bool UACEPluginSubsystem::SoundService(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& R,const TSharedPtr<FJsonObject>& Reply,FString& Error)
{
    if(!P.Permissions.Contains(TEXT("sound"))){Error=TEXT("Missing permission: sound");return false;}
    FString Op,Source,Asset;R->TryGetStringField(TEXT("operation"),Op);R->TryGetStringField(TEXT("source"),Source);R->TryGetStringField(TEXT("asset"),Asset);
    if(Op!=TEXT("play")){Error=TEXT("Unknown sound operation");return false;}
    if(!ACEPluginSoundPrivate::Filename(Asset)||(Source!=TEXT("")&&Source!=TEXT("bundled")&&Source!=TEXT("custom"))
        ||(Source==TEXT("custom")&&!ACEPluginSoundPrivate::CustomToken(Asset)))
    {Error=TEXT("Choose a bundled sound or an imported custom WAV; paths and URLs are not accepted.");return false;}
    const FString Path=Source==TEXT("custom") ? UserDirectory()/TEXT("Sounds")/P.Id/Asset : P.Directory/TEXT("sounds")/Asset;
    TArray<uint8> Data;FWaveModInfo Wave;
    if(!ACEPluginSoundPrivate::LoadWave(Path,Data,Wave,Error))return false;
    const float Duration=float(Wave.SampleDataSize)/(*Wave.pSamplesPerSec*(*Wave.pChannels)*2);
    Reply->SetNumberField(TEXT("duration"),Duration);
    double Volume=1;R->TryGetNumberField(TEXT("volume"),Volume);if(!FMath::IsFinite(Volume))Volume=1;
    Volume=FMath::Clamp(Volume,0.,1.)*ACERuntimeOptions::SoundGain(false);
    // A muted window or a headless host still accepts the notification; it must
    // not retry later and play a burst of old alerts after sound is unmuted.
    if(Volume<=0||!GetWorld())return true;
    auto* Sound=NewObject<USoundWaveProcedural>(this);Sound->SetSampleRate(*Wave.pSamplesPerSec);Sound->NumChannels=*Wave.pChannels;
    Sound->Duration=Duration;Sound->bLooping=false;
    Sound->QueueAudio(Wave.SampleDataStart,Wave.SampleDataSize);
    if(auto* Audio=UGameplayStatics::SpawnSound2D(GetWorld(),Sound,Volume))
    {
        // Procedural sources otherwise remain alive generating silence.
        FTimerHandle Stop;GetWorld()->GetTimerManager().SetTimer(Stop,FTimerDelegate::CreateWeakLambda(Audio,[Audio](){Audio->Stop();}),Sound->Duration+.1f,false);
    }
    return true;
}

bool UACEPluginSubsystem::ChoosePluginSound(FACEClientPlugin& P,const FString& ControlId,FString& Error)
{
    auto Failure=[&](const FString& Message)
    {
        Error=Message;auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("type"),TEXT("sound_file"));E->SetStringField(TEXT("id"),ControlId);
        E->SetBoolField(TEXT("ok"),false);E->SetBoolField(TEXT("cancelled"),false);E->SetStringField(TEXT("error"),Message);QueuePluginEvent(P,E);return false;
    };
    if(!P.Permissions.Contains(TEXT("sound")))return Failure(TEXT("Missing permission: sound"));
    if(PluginSoundPickerOpen)return Failure(TEXT("A sound file picker is already open."));
#if PLATFORM_WINDOWS || PLATFORM_LINUX
    PluginSoundPickerOpen=true;
    using FComplete=TFunction<void(const FString&,const FString&,bool)>;
    FComplete Complete=[Weak=TWeakObjectPtr<UACEPluginSubsystem>(this),Plugin=TWeakPtr<FACEClientPlugin>(Find(P.Id)),
        StatePath=P.StatePath,Player=P.Player,Session=P.VMSession,ControlId](const FString& Path,const FString& FailureText,bool Cancelled)
    {
        auto* Host=Weak.Get();if(!Host)return;Host->PluginSoundPickerOpen=false;
        auto Current=Plugin.Pin();
        // An asynchronous desktop picker must not change another character's
        // preferences after logout, reload, or disabling the plugin.
        if(!Current||Host->Find(Current->Id)!=Current||!Current->Running||!Current->Enabled
            ||Current->StatePath!=StatePath||Current->Player!=Player||Current->VMSession.Pin()!=Session.Pin())return;
        auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("type"),TEXT("sound_file"));E->SetStringField(TEXT("id"),ControlId);
        FString ImportError=FailureText;
        const bool Ok=!Cancelled&&ImportError.IsEmpty()&&Host->ImportPluginSound(*Current,Path,E,ImportError);
        E->SetBoolField(TEXT("ok"),Ok);E->SetBoolField(TEXT("cancelled"),Cancelled);E->SetStringField(TEXT("error"),ImportError);Host->QueuePluginEvent(*Current,E);
    };
#if PLATFORM_WINDOWS
    const HRESULT Init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IFileOpenDialog* Dialog=nullptr;
    HRESULT Result=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&Dialog));
    FString Selected;
    if(SUCCEEDED(Result))
    {
        DWORD Options=0;Dialog->GetOptions(&Options);Dialog->SetOptions(Options|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_NOCHANGEDIR);
        const COMDLG_FILTERSPEC Filter{L"WAV sounds",L"*.wav"};Dialog->SetFileTypes(1,&Filter);Dialog->SetTitle(L"Choose an alert sound");
        const void* Parent=FSlateApplication::IsInitialized()?FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr):nullptr;
        Result=Dialog->Show(static_cast<HWND>(const_cast<void*>(Parent)));
        IShellItem* Item=nullptr;if(SUCCEEDED(Result))Result=Dialog->GetResult(&Item);
        if(Item)
        {
            PWSTR Path=nullptr;Result=Item->GetDisplayName(SIGDN_FILESYSPATH,&Path);
            if(SUCCEEDED(Result))Selected=Path;
            CoTaskMemFree(Path);Item->Release();
        }
        Dialog->Release();
    }
    if(SUCCEEDED(Init))CoUninitialize();
    const bool Cancelled=Result==HRESULT_FROM_WIN32(ERROR_CANCELLED);
    Complete(Selected,FAILED(Result)&&!Cancelled?TEXT("Could not open the sound file picker."):TEXT(""),Cancelled);
#else
    static const SDL_DialogFileFilter Filter{"WAV sounds","wav"};
    const void* Parent=FSlateApplication::IsInitialized()?FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr):nullptr;
    SDL_ShowOpenFileDialog([](void* Context,const char* const* Files,int)
    {
        TUniquePtr<FComplete> Done(static_cast<FComplete*>(Context));
        const FString Path=Files&&Files[0]?UTF8_TO_TCHAR(Files[0]):TEXT("");
        const bool Cancelled=Files&&!Files[0];
        const FString FailureText=Files?TEXT(""):TEXT("Could not open the sound file picker.");
        AsyncTask(ENamedThreads::GameThread,[Done=MoveTemp(*Done),Path,FailureText,Cancelled]() {Done(Path,FailureText,Cancelled);});
    },new FComplete(MoveTemp(Complete)),static_cast<SDL_Window*>(const_cast<void*>(Parent)),&Filter,1,nullptr,false);
#endif
    return true;
#else
    return Failure(TEXT("Choosing a custom WAV is available in the Windows and Linux desktop clients. You can use the default chime here."));
#endif
}
