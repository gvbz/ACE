#include "Mods/ACEWaypointFilePicker.h"
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <shobjidl.h>
#elif PLATFORM_LINUX
#include <SDL3/SDL.h>
#endif

namespace ACEWaypointFilePicker
{
    namespace
    {
        bool PickerOpen=false;
#if WITH_DEV_AUTOMATION_TESTS
        FPicker TestPicker;
#endif
    }

    bool IsOpen(){return PickerOpen;}
#if WITH_DEV_AUTOMATION_TESTS
    void SetTestPicker(FPicker Picker){TestPicker=MoveTemp(Picker);}
#endif

    bool Open(const FString& InitialPath,FComplete Complete)
    {
        if(PickerOpen)return false;
        PickerOpen=true;
        FComplete Done=[Complete=MoveTemp(Complete)](const FString& Path,const FString& Error,bool Cancelled)
        {
            PickerOpen=false;
            // Some desktops allow typing a filename that bypasses the filter.
            if(!Cancelled&&Error.IsEmpty()&&(Path.IsEmpty()||!FPaths::GetExtension(Path).Equals(TEXT("xml"),ESearchCase::IgnoreCase)))
                Complete(TEXT(""),TEXT("Choose a GoArrow or CoD XML file."),false);
            else Complete(Path,Error,Cancelled);
        };
        const FString Initial=InitialPath.TrimStartAndEnd().TrimQuotes();
#if WITH_DEV_AUTOMATION_TESTS
        if(TestPicker){TestPicker(Initial,MoveTemp(Done));return true;}
#endif
#if PLATFORM_WINDOWS
        const HRESULT Init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        IFileOpenDialog* Dialog=nullptr;
        HRESULT Result=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&Dialog));
        FString Selected;
        if(SUCCEEDED(Result))
        {
            DWORD Options=0;Dialog->GetOptions(&Options);
            Dialog->SetOptions(Options|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
            const COMDLG_FILTERSPEC Filter{L"GoArrow / CoD XML files",L"*.xml"};
            Dialog->SetFileTypes(1,&Filter);Dialog->SetDefaultExtension(L"xml");Dialog->SetTitle(L"Choose a GoArrow or CoD atlas XML file");
            if(!Initial.IsEmpty())
            {
                const bool Directory=IFileManager::Get().DirectoryExists(*Initial);
                FString Folder=Directory?Initial:FPaths::GetPath(Initial);
                FPaths::MakePlatformFilename(Folder);
                IShellItem* Item=nullptr;
                if(!Folder.IsEmpty()&&SUCCEEDED(SHCreateItemFromParsingName(*Folder,nullptr,IID_PPV_ARGS(&Item))))
                {Dialog->SetFolder(Item);Item->Release();}
                if(!Directory)Dialog->SetFileName(*FPaths::GetCleanFilename(Initial));
            }
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
        Done(Selected,FAILED(Result)&&!Cancelled?TEXT("Could not open the XML file picker. Paste the full file path above instead."):TEXT(""),Cancelled);
#elif PLATFORM_LINUX
        static const SDL_DialogFileFilter Filter{"GoArrow / CoD XML files","xml"};
        struct FPending
        {
            FComplete Complete;
            TArray<ANSICHAR> InitialLocation;
        };
        auto* Pending=new FPending{MoveTemp(Done),{}};
        if(!Initial.IsEmpty())
        {
            FTCHARToUTF8 Utf8(*Initial);
            Pending->InitialLocation.Append(Utf8.Get(),Utf8.Length()+1);
        }
        const void* Parent=FSlateApplication::IsInitialized()?FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr):nullptr;
        SDL_ShowOpenFileDialog([](void* Context,const char* const* Files,int)
        {
            TUniquePtr<FPending> Pending(static_cast<FPending*>(Context));
            const FString Path=Files&&Files[0]?UTF8_TO_TCHAR(Files[0]):TEXT("");
            const bool Cancelled=Files&&!Files[0];
            const FString Error=Files?TEXT(""):TEXT("Could not open the XML file picker. Paste the full file path above instead.");
            AsyncTask(ENamedThreads::GameThread,[Done=MoveTemp(Pending->Complete),Path,Error,Cancelled](){Done(Path,Error,Cancelled);});
        },Pending,static_cast<SDL_Window*>(const_cast<void*>(Parent)),&Filter,1,
            Pending->InitialLocation.IsEmpty()?nullptr:Pending->InitialLocation.GetData(),false);
#else
        Done(TEXT(""),TEXT("File browsing is available in the Windows and Linux desktop clients. Paste the full XML file path above, then choose Import locations."),false);
#endif
        return true;
    }
}
