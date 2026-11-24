#include "Mods/ACEPluginCatalog.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginIcon.h"
#include "ACEUpdateSubsystem.h"
#include "Engine/GameInstance.h"
#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Views/SListView.h"

namespace ACEPluginCatalog
{
namespace
{
TSharedRef<SWidget> Label(const FString& Text,int32 Size)
{
    return SNew(SACEPluginText).Text(FText::FromString(Text)).Font(FACEPluginStyle::NativeFont("Regular",Size)).AutoWrapText(true);
}
TSharedRef<SWidget> Button(const FString& Text,TFunction<void()> Action)
{
    return SNew(SACEPluginButton).ButtonStyle(&FACEPluginStyle::Current()->Button).ContentPadding(FMargin(10,4))
        .OnClicked_Lambda([Action=MoveTemp(Action)]{Action();return FReply::Handled();})[Label(Text,15)];
}
class SCatalogLogo : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCatalogLogo){} SLATE_ARGUMENT(TSharedPtr<FEntry>,Entry) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        const auto& Bytes=Args._Entry->Logo;
        if(!Bytes.IsEmpty())
        {
            const auto Decoder=FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CreateImageWrapper(EImageFormat::PNG);
            if(Decoder && Decoder->SetCompressed(Bytes.GetData(),Bytes.Num()) && Decoder->GetWidth()>0 && Decoder->GetWidth()<=256 && Decoder->GetHeight()>0 && Decoder->GetHeight()<=256)
                Image.Reset(FImageUtils::ImportBufferAsTexture2D(Bytes));
        }
        TSharedRef<SWidget> Art=SNew(SACEPluginIcon).Id(TEXT("plugin.options"));
        if(Image.IsValid())
        {
            Brush.SetResourceObject(Image.Get()); Brush.ImageSize=FVector2D(Image->GetSizeX(),Image->GetSizeY());
            Art=SNew(SImage).Image(&Brush);
        }
        ChildSlot[SNew(SBox).WidthOverride(52).HeightOverride(52)[SNew(SScaleBox).Stretch(EStretch::ScaleToFit)[Art]]];
    }
private:
    TStrongObjectPtr<UTexture2D> Image;
    FSlateBrush Brush;
};

class SCatalog : public SCompoundWidget
{
    using FItem=TSharedPtr<FEntry>;
public:
    SLATE_BEGIN_ARGS(SCatalog){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Host=Args._Host; Theme=FACEPluginStyle::Use(Args._Host,TEXT("__manager"));
        auto Tabs=SNew(SWrapBox).UseAllottedSize(true);
        Tabs->AddSlot().Padding(0,0,6,4)[Button(TEXT("Browse plugins"),[this]{ShowCatalog();})];
        Tabs->AddSlot().Padding(0,0,6,4)[Button(TEXT("Installed"),[this]{if(Host.IsValid()){Browsing=false;Body->SetContent(Host->MakePanel());}})];
        Tabs->AddSlot().Padding(0,0,6,4)[Button(TEXT("Register a plugin"),[]{FPlatformProcess::LaunchURL(Repository,nullptr,nullptr);})];
        ChildSlot[SNew(SBorder).BorderImage(&Theme->Background).Padding(14)
            [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Plugin Manager"),24)]
            +SVerticalBox::Slot().AutoHeight()[Tabs]
            +SVerticalBox::Slot().FillHeight(1)[SAssignNew(Body,SBox)]]];
        ShowCatalog();
    }
    virtual void Tick(const FGeometry& G,double Time,float Delta) override
    {
        SCompoundWidget::Tick(G,Time,Delta);
        // The lobby constructs all switcher pages on startup. Fetch only on first display.
        if(!Started&&Host.IsValid()){Started=true;LoadCache();Refresh();}
        if(Host.IsValid()&&Revision!=Host->GetPluginOrderRevision()){Revision=Host->GetPluginOrderRevision();if(Browsing)ReadInstallations();else Body->SetContent(Host->MakePanel());}
    }
    ~SCatalog(){if(Request)Request->CancelRequest();}
private:
    TWeakObjectPtr<UACEPluginSubsystem> Host;
    TSharedPtr<FACEPluginStyle> Theme;
    TSharedPtr<SBox> Body;
    TSharedPtr<SListView<FItem>> List;
    TArray<FEntry> Index;
    TArray<FItem> Items;
    TMap<FString,FInstallInfo> Installations;
    TArray<FItem> PendingUpdates;
    TArray<FString> FailedUpdates;
    int32 UpdatesAvailable=0, Updated=0, Revision=INDEX_NONE;
    FString Query,Status,ConfirmId,AdoptId;
    FHttpRequestPtr Request;
    bool Started=false, Browsing=true, UpdatingAll=false;

    FString Cache() const {return Host->UserDirectory()/TEXT("Catalog/index.xml");}
    void ShowCatalog()
    {
        Browsing=true;
        auto Search=SNew(SEditableTextBox).Style(&Theme->Edit).HintText(FText::FromString(TEXT("Search plugins"))).Text(FText::FromString(Query))
            .OnTextChanged_Lambda([this](const FText& T){Query=T.ToString();Filter();});
        Search->SetTag(TEXT("Catalog.Search"));
        auto Batch=Button(TEXT("Update all"),[this]{StartUpdates();});
        auto RefreshButton=Button(TEXT("Refresh"),[this]{Refresh();});
        RefreshButton->SetTag(TEXT("Catalog.Refresh"));
        Batch->SetTag(TEXT("Catalog.UpdateAll"));
        Batch->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]{return UpdatesAvailable>0?EVisibility::Visible:EVisibility::Collapsed;}));
        Batch->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !Request&&!UpdatingAll;}));
        Body->SetContent(SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,6)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[Search]
                +SHorizontalBox::Slot().AutoWidth().Padding(6,0,0,0)[Batch]
                +SHorizontalBox::Slot().AutoWidth().Padding(6,0,0,0)[RefreshButton]]
            +SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginText).Tag(TEXT("Catalog.Status")).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Status);})]
            +SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginText).Text(FText::FromString(TEXT("No matching plugins.")))
                .Visibility_Lambda([this]{return !Query.IsEmpty()&&Items.IsEmpty()?EVisibility::Visible:EVisibility::Collapsed;})]
            +SVerticalBox::Slot().FillHeight(1)[SAssignNew(List,SListView<FItem>).ListItemsSource(&Items).SelectionMode(ESelectionMode::None)
                .OnGenerateRow(this,&SCatalog::Row)]);
        ReadInstallations();
    }
    void ReadInstallations()
    {
        Installations.Reset();UpdatesAvailable=0;
        if(Host.IsValid())for(const FEntry& E:Index)
        {
            const auto Info=Inspect(*Host.Get(),E);
            if(Info.State==EInstallState::Update)++UpdatesAvailable;
            Installations.Add(E.Id,Info);
        }
        Filter();
    }
    void Filter()
    {
        Items.Reset(); TSet<FString> Listed;
        auto Add=[&](const FEntry& E)
        {
            if(Query.IsEmpty()||E.Name.Contains(Query)||E.Developer.Contains(Query)||E.Id.Contains(Query)||E.Description.Contains(Query)||E.Repo.Contains(Query))Items.Add(MakeShared<FEntry>(E));
        };
        for(const auto& E:Index){Listed.Add(E.Id);Add(E);}
        if(Host.IsValid())for(const auto& P:Host->Plugins)
            if(!Listed.Contains(P->Id)&&(!ActiveDirectory(Host->UserDirectory(),P->Id).IsEmpty()||FPaths::IsUnderDirectory(P->Directory,Host->UserDirectory()/TEXT("Installed"))))
            {
                FEntry E; E.Id=P->Id;E.Name=P->Name;E.Description=P->Description;Add(E);
            }
        if(List)List->RequestListRefresh();
    }
    TSharedRef<ITableRow> Row(FItem E,const TSharedRef<STableViewBase>& Owner)
    {
        const auto* Cached=Installations.Find(E->Id);
        const FInstallInfo Info=Cached?*Cached:Inspect(*Host.Get(),*E);
        const FString Latest=E->Version.IsEmpty()?E->Tag:E->Version;
        FString Version;
        switch(Info.State)
        {
        case EInstallState::Available: Version=TEXT("Available ")+Latest; break;
        case EInstallState::Current: Version=TEXT("Up to date"); break;
        case EInstallState::ManualCurrent: Version=TEXT("Up to date · Installed manually"); break;
        case EInstallState::Update: case EInstallState::ManualUpdate: Version=TEXT("Update available · ")+Latest; break;
        case EInstallState::ManualUnknown: Version=TEXT("Installed manually · Catalog version ")+Latest; break;
        case EInstallState::Newer: Version=TEXT("Installed version is newer · Catalog version ")+Latest; break;
        case EInstallState::Repair: Version=TEXT("Installation incomplete"); break;
        case EInstallState::Bundled: Version=TEXT("Included with the client"); break;
        case EInstallState::Delisted: Version=TEXT("Installed locally"); break;
        }
        if(!Info.InstalledVersion.IsEmpty())Version+=TEXT(" · Installed ")+Info.InstalledVersion;
        if(!E->Developer.IsEmpty())Version=TEXT("By ")+E->Developer+TEXT("  ·  ")+Version;
        auto Actions=SNew(SWrapBox).UseAllottedSize(true);
        auto Add=[&](const FString& Title,TFunction<void()> Fn)
        {
            auto Control=Button(Title,MoveTemp(Fn));
            Control->SetTag(FName(*(TEXT("Catalog.")+Title+TEXT(".")+E->Id)));
            if(Title!=TEXT("Source") && Title!=TEXT("Release notes"))
                Control->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !Request&&!UpdatingAll;}));
            Actions->AddSlot().Padding(0,2,6,2)[Control];
        };
        if(Info.State==EInstallState::Available)Add(TEXT("Install"),[this,E]{Download(E);});
        if(Info.State==EInstallState::Update)Add(TEXT("Update"),[this,E]{Download(E);});
        if(Info.State==EInstallState::Repair)Add(TEXT("Repair"),[this,E]{Download(E);});
        if(Info.State==EInstallState::ManualUpdate||Info.State==EInstallState::ManualUnknown)
        {
            if(AdoptId==E->Id)
            {
                Add(TEXT("Use catalog copy"),[this,E]{AdoptId.Reset();Download(E,true);});
                Add(TEXT("Cancel"),[this]{AdoptId.Reset();Status.Reset();Filter();});
            }
            else Add(Info.State==EInstallState::ManualUpdate?TEXT("Update"):TEXT("Choose catalog version"),[this,E]
            {
                AdoptId=E->Id;
                Status=TEXT("Use the catalog copy of ")+E->Name+TEXT(" for future updates?");
                Filter();
            });
        }
        if(!E->Repo.IsEmpty())
        {
            Add(TEXT("Source"),[E]{const FString URL=IsOfficial(*E)?E->Repo/TEXT("tree")/E->Tag/TEXT("Unreal/Plugins/ACEClient/ClientMods")/E->Id:E->Repo;FPlatformProcess::LaunchURL(*URL,nullptr,nullptr);});
            Add(TEXT("Release notes"),[E]{FPlatformProcess::LaunchURL(*(E->Repo/TEXT("releases/tag")/E->Tag),nullptr,nullptr);});
        }
        if(Info.HasReceipt||(Info.State!=EInstallState::Available&&Info.State!=EInstallState::Bundled))
        {
            const bool Confirm=ConfirmId==E->Id;
            Add(Confirm?TEXT("Confirm uninstall"):TEXT("Uninstall"),[this,E,Confirm]
            {
                if(Request||!Host.IsValid())return;
                if(Confirm){if(Remove(*Host.Get(),E->Id,Status))Status=E->Name+TEXT(" uninstalled. Your profiles were kept.");ConfirmId.Reset();}
                else {ConfirmId=E->Id;Status=TEXT("Remove all installed copies of ")+E->Name+TEXT(" from your plugin folder?");}
                ReadInstallations();
            });
            if(Confirm)Add(TEXT("Cancel"),[this]{ConfirmId.Reset();Filter();});
        }
        return SNew(STableRow<FItem>,Owner).Padding(6)
            [SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0,0,12,0)[SNew(SCatalogLogo).Entry(E)]
            +SHorizontalBox::Slot().FillWidth(1)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[Label(E->Name,19)]
                +SVerticalBox::Slot().AutoHeight()[Label(Version,14)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,3)[Label(E->Description,15)]
                +SVerticalBox::Slot().AutoHeight()[Actions]]];
    }

    void LoadCache()
    {
        FString Text,Ignored;
        if(IFileManager::Get().FileSize(*Cache())<=MaxIndexBytes&&FFileHelper::LoadFileToString(Text,*Cache())&&Parse(Text,Index,Ignored))ReadInstallations();
    }
    void Fetch(const FString& Url,int32 Limit,TFunction<void(const TArray<uint8>*)> Done)
    {
        TWeakPtr<SCatalog> Weak=SharedThis(this);
        ACEPluginCatalog::Fetch(Request,Url,Limit,[Weak,Url,Done=MoveTemp(Done)](const TArray<uint8>* Bytes)
        {
            auto Self=Weak.Pin();if(!Self)return;
            Self->Request.Reset();if(!Self->Host.IsValid())return;
            if(!Bytes)Self->Status=Url==IndexURL?TEXT("Catalog unavailable. Showing saved listings. Try Refresh."):TEXT("Download failed. Retry the installation.");
            Done(Bytes);
        });
    }
    void Refresh()
    {
        if(Request||UpdatingAll||!Host.IsValid())return;
        if(Host->GetGameInstance()->GetSubsystem<UACEUpdateSubsystem>()->CanUseLobby())Host->Discover();
        ReadInstallations();
        Status=TEXT("Checking the plugin catalog…");
        Fetch(IndexURL,MaxIndexBytes,[this](const TArray<uint8>* Result)
        {
            if(!Result)return;
            const auto& Bytes=*Result;
            if(Bytes.IsEmpty()){Status=TEXT("The catalog response was empty.");return;}
            FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),Bytes.Num());
            if(!Parse(FString(Text.Length(),Text.Get()),Index,Status))return;
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Cache()),true);
            FFileHelper::SaveArrayToFile(Bytes,*Cache());
            ReadInstallations();
            Status=FString::Printf(TEXT("%d registered plugin%s"),Index.Num(),Index.Num()==1?TEXT(""):TEXT("s"));
            if(UpdatesAvailable)Status+=FString::Printf(TEXT(" · %d update%s available"),UpdatesAvailable,UpdatesAvailable==1?TEXT(""):TEXT("s"));
        });
    }
    void StartUpdates()
    {
        if(Request||UpdatingAll||!Host.IsValid())return;
        PendingUpdates.Reset(); FailedUpdates.Reset(); Updated=0;
        for(const auto& E:Index)
            if(Inspect(*Host.Get(),E).State==EInstallState::Update)PendingUpdates.Add(MakeShared<FEntry>(E));
        UpdatingAll=true;NextUpdate();
    }
    void NextUpdate()
    {
        if(!Host.IsValid())return;
        while(!PendingUpdates.IsEmpty()&&Host->GetGameInstance()->GetSubsystem<UACEUpdateSubsystem>()->CanUseLobby())
        {
            const auto E=PendingUpdates.Pop();
            if(Inspect(*Host.Get(),*E).State==EInstallState::Update){Download(E);return;}
        }
        UpdatingAll=false;
        Status=FString::Printf(TEXT("%d plugin%s updated."),Updated,Updated==1?TEXT(""):TEXT("s"));
        if(!FailedUpdates.IsEmpty())Status+=TEXT(" Failed: ")+FString::Join(FailedUpdates,TEXT(", "))+TEXT(". Retry individually.");
        if(!PendingUpdates.IsEmpty())Status+=TEXT(" Return to Accounts to finish remaining updates.");
        PendingUpdates.Reset();
        ReadInstallations();
    }
    void Download(FItem E,bool AdoptManual=false)
    {
        if(Request||!Host.IsValid())return;
        const FString Before=ActiveDirectory(Host->UserDirectory(),E->Id);
        Status=TEXT("Downloading ")+E->Name+TEXT("…");
        Fetch(E->Download,E->Bytes,[this,E,AdoptManual,Before](const TArray<uint8>* Bytes)
        {
            bool Success=false;
            if(Bytes&&Before!=ActiveDirectory(Host->UserDirectory(),E->Id))
            {
                Status=E->Name+TEXT(" changed in another client. Refresh to check its version.");
            }
            else if(Bytes)Success=Install(*Host.Get(),*E,*Bytes,Status,AdoptManual);
            if(Success)
            {
                Status=E->Name+(Before.IsEmpty()&&!AdoptManual?TEXT(" installed."):TEXT(" updated."))+TEXT(" Open Installed to review its permissions.");
                if(!Browsing)Body->SetContent(Host->MakePanel());
            }
            ReadInstallations();
            if(UpdatingAll)
            {
                if(Success)++Updated;else FailedUpdates.Add(E->Name);
                TWeakPtr<SCatalog> Weak=SharedThis(this);
                AsyncTask(ENamedThreads::GameThread,[Weak]{if(auto Self=Weak.Pin())Self->NextUpdate();});
            }
        });
    }
};
}
TSharedRef<SWidget> MakeBrowser(UACEPluginSubsystem* Host){return SNew(SCatalog).Host(Host);}
}
