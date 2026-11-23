#include "Mods/ACEPluginPanel.h"
#include "Mods/ACEPluginPresentation.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginDesktop.h"
#include "Mods/ACEPluginOrder.h"
#include "Mods/ACEPluginIcon.h"
#include "Mods/ACEActionDeckSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "Mobile/ACEMobileWidget.h"
#include "ACEDatSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "VR/ACEVRComponent.h"
#include "VR/ACEVRMenu.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"

namespace
{
namespace ACEPluginPanelPrivate
{
    FString Text(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
    { FString V; if (O) O->TryGetStringField(Key, V); return V; }
    TSharedRef<SWidget> Label(const FString& S, int Size = 20)
    { return SNew(SACEPluginText).Text(FText::FromString(S)).Font(FACEPluginStyle::NativeFont("Regular", Size)).AutoWrapText(true); }
    const FButtonStyle& PluginButtonStyle() {return FACEPluginStyle::Current()->Button;}
    TSharedRef<SWidget> Button(const FString& S, TFunction<void()> Fn)
    { return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&PluginButtonStyle()).IsFocusable(false).OnClicked_Lambda([Fn](){ Fn(); return FReply::Handled(); })[Label(S)]; }

    class SACEPlugins : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SACEPlugins) {} SLATE_ARGUMENT(UACEPluginSubsystem*, Host) SLATE_ARGUMENT(FString, PluginId) SLATE_END_ARGS()
        void Construct(const FArguments& Args)
        {
            Host = Args._Host; Id = Args._PluginId; FACEPluginStyle::Use(Args._Host,Id.IsEmpty()?TEXT("__manager"):Id); PluginOnly = !Id.IsEmpty();
            if(Host.IsValid())ManagerNotice=Host->Notice;
            Body=SNew(SVerticalBox);
            Rebuild();
        }
        virtual void Tick(const FGeometry& G,double T,float D) override
        {
            SCompoundWidget::Tick(G,T,D);
            if(Retail&&!ArtworkReady)
            {
                Retail->Refresh();ArtworkReady=Retail->HasDatArtwork();
                if(ArtworkReady)
                {
                    // Repaint the existing controls once when DAT loading ends.
                    // Their brush addresses stay the same, so Slate cannot detect
                    // this resource change from the checkbox's image attribute.
                    TFunction<void(SWidget&)> Refresh=[&](SWidget& W){W.Invalidate(EInvalidateWidgetReason::Layout|EInvalidateWidgetReason::Paint);
                        if(auto* C=W.GetChildren())for(int32 I=0;I<C->Num();++I)Refresh(C->GetChildAt(I).Get());};
                    Refresh(*this);
                }
            }
            if(Host.IsValid()&&Id.IsEmpty()&&OrderRevision!=Host->GetPluginOrderRevision())OrderRows();
            if(auto P=DisplayedPlugin();P&&P->ScrollPanel()!=Scrollable)SetScrolling(P->ScrollPanel());
        }
    private:
        TWeakObjectPtr<UACEPluginSubsystem> Host;
        TSharedPtr<FACEPluginStyle> Retail;
        bool ArtworkReady=false;
        TSharedPtr<SVerticalBox> Body;
        TSharedPtr<SVerticalBox> PluginRows;
        TMap<FString,TSharedRef<SWidget>> RowsById;
        uint32 OrderRevision=0;
        TSharedPtr<SVerticalBox> SettingsBody;
        TSharedPtr<SEditableTextBox> Name;
        TSharedPtr<SMultiLineEditableTextBox> Json;
        FString Id, Search, ManagerNotice;
        bool PluginOnly = false;
        bool Scrollable=true;
        TSharedPtr<SScrollBox> Scroll;
        SVerticalBox::FSlot* CompanionSlot=nullptr;
        TSharedPtr<FACEClientPlugin> DisplayedPlugin() const
        {
            if(Host.IsValid())for(const auto& P:Host->Plugins)if(P->Id==Id)return P;
            return nullptr;
        }
        void SetScrolling(bool Value)
        {
            Scrollable=Value;
            ChildSlot[SNullWidget::NullWidget];if(Scroll)Scroll->ClearChildren();Scroll.Reset();
            if(Scrollable)ChildSlot[SAssignNew(Scroll,SScrollBox).Tag(TEXT("PluginScroll"))+SScrollBox::Slot()[Body.ToSharedRef()]];
            else ChildSlot[Body.ToSharedRef()];
            if(CompanionSlot){if(Scrollable)CompanionSlot->SetAutoHeight();else CompanionSlot->SetFillHeight(1);}
            SetClipping(EWidgetClipping::ClipToBounds);
        }
        void OrderRows()
        {
            OrderRevision=Host->GetPluginOrderRevision();if(!PluginRows)return;
            // Reattach existing cards; preserve their controls and the manager's scroll position.
            PluginRows->ClearChildren();
            for(const auto& PluginId:Host->PluginDisplayOrder())if(auto* Row=RowsById.Find(PluginId))
                PluginRows->AddSlot().AutoHeight().Padding(5)[*Row];
        }
        void Add(TSharedRef<SWidget> W) { (SettingsBody?SettingsBody:Body)->AddSlot().AutoHeight().Padding(5)[W]; }
        void Save(const TSharedPtr<FACEClientPlugin>& P)
        {
            Host->SaveCurrentProfile(P->Id, !P->HasPresentation());
            if(Json)Json->SetText(FText::FromString(Host->ProfileJson(P->Id)));
        }
        void Open(const FString& PluginId)
        {
            if(!Host.IsValid())return;
            auto* GI=Host->GetGameInstance();
            auto* Client=GI->GetSubsystem<UACEClientSubsystem>();
            auto* PC=Cast<AACEPlayerController>(GI->GetFirstLocalPlayerController());
            if(Client&&Client->GetSessionState()==EACESessionState::InWorld&&PC&&!PC->IsVRActive()&&!PC->IsMobileActive())
            {
                // The manager is a launcher. Each desktop tool has its own
                // window; opening settings must not embed a second loot editor.
                auto H=Host;
                if(H->OpenPluginWindow(PluginId))H->CloseDesktopPluginManager();
                return;
            }
            // Login, VR and touch use the same manager in a single panel.
            Id=PluginId;Rebuild();
        }
        void Rebuild()
        {
            CompanionSlot=nullptr;SettingsBody.Reset();PluginRows.Reset();RowsById.Reset();Body->ClearChildren(); if (!Host.IsValid()) return;
            // Paginated plugins receive the available height, with no wheel
            // scrolling or scrollbar. Keep the manager's normal scrolling.
            auto Display=DisplayedPlugin();SetScrolling(!Display||Display->ScrollPanel());
            Retail=FACEPluginStyle::Use(Host.Get(),Id.IsEmpty()?TEXT("__manager"):Id);ArtworkReady=Retail->HasDatArtwork();
            if (!PluginOnly && Id.IsEmpty())
            {
                auto Header=SNew(SHorizontalBox);
                Header->AddSlot().FillWidth(1).HAlign(HAlign_Right)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&PluginButtonStyle()).IsFocusable(false)
                    .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Reload installed plugins. Running plugins will stop."))))
                    .OnClicked_Lambda([this](){Host->Discover();ManagerNotice=Host->Notice;Rebuild();return FReply::Handled();})[Label(TEXT("Refresh"),16)]];
                Add(Header);
                Add(SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Enabled plugin limit (0 = unlimited)"),16)]
                    +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[SNew(SBox).WidthOverride(110)
                        [SNew(SACEPluginSpinBox<int32>).Tag(TEXT("PluginEnabledLimit")).Style(&FACEPluginStyle::Current()->Spin)
                            .Font(FACEPluginStyle::NativeFont("Regular",16)).MinValue(0).MaxValue(MAX_int32).EnableSlider(false)
                            .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Maximum enabled plugins. Disabled plugins do not count. Saved across logins; 0 removes the limit."))))
                            .Value_Lambda([this](){return Host->GetEnabledPluginLimit();})
                            .OnValueCommitted_Lambda([this](int32 Limit,ETextCommit::Type){Host->SetEnabledPluginLimit(Limit);ManagerNotice=Host->Notice;})]]);
                Add(SNew(SACEPluginText).Tag(TEXT("PluginEnabledCount")).Font(FACEPluginStyle::NativeFont("Regular",14))
                    .Text_Lambda([this](){const int32 Limit=Host->GetEnabledPluginLimit();return FText::FromString(Limit==0
                        ? FString::Printf(TEXT("%d enabled / unlimited. Disabled plugins do not count."),Host->GetEnabledPluginCount())
                        : FString::Printf(TEXT("%d / %d enabled. Disabled plugins do not count."),Host->GetEnabledPluginCount(),Limit));}).AutoWrapText(true));
                Add(SNew(SACEPluginText).Tag(TEXT("PluginManagerNotice")).Font(FACEPluginStyle::NativeFont("Regular",16)).AutoWrapText(true)
                    .Visibility_Lambda([this](){return ManagerNotice.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
                    .Text_Lambda([this](){return FText::FromString(ManagerNotice);}));
                auto BarControls=SNew(SHorizontalBox);
                BarControls->AddSlot().FillWidth(1).VAlign(VAlign_Center)[SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).Padding(FMargin(4,8))
                    .IsChecked_Lambda([this](){return Host->IsPluginBarVisible()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState S){Host->SetPluginBarVisible(S==ECheckBoxState::Checked);})[Label(TEXT("Show plugin bar"),16)]];
                BarControls->AddSlot().AutoWidth()[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&PluginButtonStyle()).IsFocusable(false)
                    .Visibility_Lambda([this](){return Host->IsPluginBarMinimized()?EVisibility::Visible:EVisibility::Collapsed;})
                    .OnClicked_Lambda([this](){Host->SetPluginBarVisible(true);Host->SetPluginBarMinimized(false);return FReply::Handled();})[Label(TEXT("Expand bar"),16)]];
                Add(BarControls);
                Add(Host->MakePluginWindowControls(FString()));
                Add(SNew(SCheckBox).Tag(TEXT("PluginBarHorizontal")).Style(&FACEPluginStyle::Current()->Check).Padding(FMargin(4,8))
                    .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Arrange plugin icons in a horizontal row. Uncheck for a vertical column. Saved across logins."))))
                    .IsChecked_Lambda([this](){return Host->IsPluginBarHorizontal()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState S){Host->SetPluginBarHorizontal(S==ECheckBoxState::Checked);})[Label(TEXT("Horizontal plugin bar"),16)]);
                Add(Label(TEXT("Drop plugin folders into: ")+FPaths::ConvertRelativePathToFull(Host->UserDirectory()/TEXT("Installed")),14));
                Add(Button(TEXT("Open plugin folder"),[this](){const FString Folder=FPaths::ConvertRelativePathToFull(Host->UserDirectory()/TEXT("Installed"));IFileManager::Get().MakeDirectory(*Folder,true);FPlatformProcess::ExploreFolder(*Folder);}));
                Body->AddSlot().AutoHeight()[SAssignNew(PluginRows,SVerticalBox)];
                for (auto P : Host->Plugins)
                {
                    auto Row = SNew(SHorizontalBox);
                    Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,6,0)
                        [SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::IconButtonPadding)).Tag(FName(*(TEXT("PluginOrderGrip.")+P->Id))).ButtonStyle(&FACEPluginStyle::Current()->DockButton)
                            .IsFocusable(false).AllowDragDrop(true).Cursor(EMouseCursor::GrabHand)
                            .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Drag to reorder plugins in the manager and icon bar. Unlock or hold Quick-show first."))))
                            .OnSlateButtonDragDetected_Lambda([H=Host,PluginId=P->Id](const FGeometry&,const FPointerEvent&){return ACEPluginOrder::BeginDrag(H.Get(),PluginId,TEXT("__manager"));})
                            [SNew(SBox).WidthOverride(14).HeightOverride(24)[SNew(SACEPluginIcon).Id(TEXT("plugin.grip"))]]];
                    Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[SNew(SACEPluginBadge).Plugin(P)];
                    FString Description=P->Description;
                    if(P->Id==TEXT("ucm"))Description=TEXT("Combat, buffs, recovery, routes and looting.");
                    else if(P->Id==TEXT("ucmmicro"))Description=TEXT("Quick toggles for UCM activities.");
                    else if(P->Id==TEXT("looteditor"))Description=TEXT("Create and edit loot profiles.");
                    else if(P->Id==TEXT("waypoint"))Description=TEXT("Direction arrow, world map and dungeon map.");
                    Row->AddSlot().FillWidth(1).VAlign(VAlign_Center).Padding(0,0,12,0)
                        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",18)).Text_Lambda([P]{return FText::FromString(P->DisplayTitle());})]
                        +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[SNew(SACEPluginText).Text(FText::FromString(Description))
                            .Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(FLinearColor(.86f,.83f,.75f)).AutoWrapText(true)]];
                    const FString Capabilities=P->LocalService?(P->Permissions.Contains(TEXT("local_tools"))?TEXT("Local appearance and bundled native converter. Native tools run outside the Lua sandbox."):TEXT("Local cosmetic appearance only")):P->Permissions.IsEmpty()?TEXT("No game actions"):TEXT("Game actions: ")+FString::Join(P->Permissions,TEXT(", "));
                    Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,12,0)[SNew(SCheckBox).Tag(FName(*(TEXT("PluginEnabled.")+P->Id))).Style(&FACEPluginStyle::Current()->Check).Padding(4)
                        .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Capabilities)))
                        .IsChecked_Lambda([P](){return P->Enabled?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                        .OnCheckStateChanged_Lambda([this,P](ECheckBoxState S){Host->SetEnabled(P->Id,S==ECheckBoxState::Checked);ManagerNotice=Host->Notice;})[Label(TEXT("Enabled"),14)]];
                    Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Tag(FName(*(TEXT("PluginOpen.")+P->Id))).ButtonStyle(&PluginButtonStyle()).IsFocusable(false)
                        .IsEnabled_Lambda([P](){return P->Enabled;})
                        .OnClicked_Lambda([this,PluginId=P->Id](){Open(PluginId);return FReply::Handled();})[Label(TEXT("Open"),16)]];
                    RowsById.Add(P->Id,SNew(SACEPluginOrderTarget).Host(Host.Get()).Id(P->Id).LockId(TEXT("__manager")).Tag(FName(*(TEXT("PluginManagerOrder.")+P->Id)))
                        [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.06f,.035f,.014f))
                        .Padding(12).ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([P]{return FText::FromString(P->DisplayTitle()+TEXT(" ")+P->Version);})))
                        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Row]
                            // Align settings with the title, after the grip and icon columns.
                            +SVerticalBox::Slot().AutoHeight().Padding(58,8,0,0)[Host->MakePluginWindowControls(P->Id)]]]);
                }
                OrderRows();
                if(Host->Plugins.IsEmpty())Add(Label(TEXT("No plugins installed."),16));
                return;
            }
            if(!PluginOnly)Add(Button(TEXT("Back to plugins"),[this](){Id.Empty();Rebuild();}));
            TSharedPtr<FACEClientPlugin> P;
            for (auto Candidate:Host->Plugins) if (Candidate->Id==Id) P=Candidate;
            if (!P) return;
            if(P->LocalService)
            {
                if(!PluginOnly)Add(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",26)).Text_Lambda([P]{return FText::FromString(P->DisplayTitle());}));
                Add(SNew(SACEPluginText).AutoWrapText(true).Visibility_Lambda([P](){return P->Running?EVisibility::Collapsed:EVisibility::Visible;}).Text_Lambda([P](){return FText::FromString(P->Status);}));
                Add(Host->MakeLocalServicePanel(P->Id));return;
            }
            // Notice is shared action feedback, not this plugin's title or status.
            // Each plugin supplies its own feedback inside its panel.
            if(UACEPluginSubsystem::IsLedger(P->Id)){Add(Host->MakeLedgerPanel(P->Id));return;}
            if(P->Id==TEXT("waypoint")){Add(SNew(SBox).HeightOverride(650)[Host->MakeWaypointPanel()]);return;}
            if(P->Id==TEXT("looteditor")){Add(SNew(SBox).HeightOverride(650)[Host->MakeUCMPanel(TEXT("Standalone loot"))]);return;}
            if(P->Id==TEXT("ucmmicro")){Add(Host->MakeUCMMicroPanel());return;}
            if(P->Id==TEXT("actiondeck")){Add(Host->GetGameInstance()->GetSubsystem<UACEActionDeckSubsystem>()->MakePanel());return;}
            if(P->Id==TEXT("ucm")){Add(SNew(SBox).HeightOverride(650)[Host->MakeUCMPanel()]);return;}
            if(P->Id==TEXT("itemtools")){Add(SNew(SBox).HeightOverride(650)[Host->MakeItemToolsPanel()]);return;}
            if(P->Id==TEXT("autocraft")){Add(SNew(SBox).HeightOverride(650)[Host->MakeAutoCraftPanel()]);return;}
            if(!PluginOnly)Add(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",26)).Text_Lambda([P]{return FText::FromString(P->DisplayTitle());}));
            Add(SNew(SACEPluginText).Visibility_Lambda([P]{return P->Status.IsEmpty()||!P->ShowStatus()?EVisibility::Collapsed:EVisibility::Visible;}).Font(FACEPluginStyle::NativeFont("Regular",18)).Text_Lambda([P](){return FText::FromString(P->Status);}).AutoWrapText(true));
            auto Actions=SNew(SHorizontalBox).Visibility_Lambda([P]{return P->DisplayControls()==TEXT("none")?EVisibility::Collapsed:EVisibility::Visible;});
            Actions->AddSlot().FillWidth(1)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Tag(TEXT("PluginStartToggle")).ButtonStyle(&PluginButtonStyle()).IsFocusable(false)
                .OnClicked_Lambda([this,P](){if(P->DisplayControls()==TEXT("toggle")&&P->Running)Host->Stop(P->Id,TEXT("Stopped"),true);else Host->Start(P->Id);return FReply::Handled();})
                [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",20)).Text_Lambda([P]{return FText::FromString(P->DisplayControls()==TEXT("toggle")&&P->Running?P->DisplayStopLabel():P->DisplayStartLabel());})]];
            Actions->AddSlot().FillWidth(1).Padding(8,0)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Tag(TEXT("PluginStop")).ButtonStyle(&PluginButtonStyle()).IsFocusable(false)
                .Visibility_Lambda([P]{return P->DisplayControls()==TEXT("start_stop")?EVisibility::Visible:EVisibility::Collapsed;})
                .OnClicked_Lambda([this,P](){Host->Stop(P->Id,TEXT("Stopped"),true);return FReply::Handled();})
                [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",20)).Text_Lambda([P]{return FText::FromString(P->DisplayStopLabel());})]]; Add(Actions);
            if(P->HasPresentation())
            {
                Body->AddSlot().Expose(CompanionSlot).Padding(5)[Host->MakeCompanionPanel(P->Id)];
                if(Scrollable)CompanionSlot->SetAutoHeight();else CompanionSlot->SetFillHeight(1);
                SettingsBody=SNew(SVerticalBox);
                Body->AddSlot().AutoHeight().Padding(5)[SNew(SExpandableArea).Tag(TEXT("PluginSettings")).InitiallyCollapsed(true)
                    .Visibility_Lambda([P]{return P->ShowSettings()?EVisibility::Visible:EVisibility::Collapsed;})
                    .HeaderContent()[Label(TEXT("Settings and profiles"),16)].BodyContent()[SettingsBody.ToSharedRef()]];
            }
            Add(Label(TEXT("Profile name")));
            Add(SAssignNew(Name,SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",20)).Text(FText::FromString(P->ProfileName)));
            TArray<FString> SavedProfiles; IFileManager::Get().FindFiles(SavedProfiles, *(Host->UserDirectory()/TEXT("Profiles")/P->Id/TEXT("*.json")),true,false);
            SavedProfiles.Sort();
            for(const FString& File:SavedProfiles)
            {
                const FString Profile=FPaths::GetBaseFilename(File);
                Add(Button(TEXT("Load: ")+Profile,[this,P,Profile](){Host->LoadProfile(P->Id,Profile);Rebuild();}));
            }
            auto Profiles=SNew(SHorizontalBox);
            Profiles->AddSlot().FillWidth(1)[Button(TEXT("Save as"),[this,P](){Host->SaveProfile(P->Id,Name->GetText().ToString(),Host->ProfileJson(P->Id));Rebuild();})];
            Profiles->AddSlot().FillWidth(1).Padding(8,0)[Button(TEXT("Load profile"),[this,P](){if(!Host->LoadProfile(P->Id,Name->GetText().ToString()))Host->Notice=TEXT("Profile not found");Rebuild();})]; Add(Profiles);
            const TArray<TSharedPtr<FJsonValue>>* Fields=nullptr;
            if (P->Manifest->TryGetArrayField(TEXT("settings"),Fields)) for (const auto& Field:*Fields)
            {
                const auto O=Field->AsObject(); if(!O)continue;
                const FString Key=Text(O,TEXT("key")), Kind=Text(O,TEXT("type")), Caption=Text(O,TEXT("label"));
                if(Kind==TEXT("bool"))
                {
                    Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).Padding(FMargin(6,10)).IsChecked_Lambda([P,Key](){bool V=false;P->Profile->TryGetBoolField(Key,V);return V?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                        .OnCheckStateChanged_Lambda([this,P,Key](ECheckBoxState S){P->Profile->SetBoolField(Key,S==ECheckBoxState::Checked);Save(P);})[Label(Caption)]);
                }
                else if(Kind==TEXT("text"))
                {
                    Add(Label(Caption));Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",18)).Text(FText::FromString(Text(P->Profile,*Key)))
                        .OnTextCommitted_Lambda([this,P,Key](const FText& V,ETextCommit::Type){P->Profile->SetStringField(Key,V.ToString().Left(512));Save(P);}));
                }
                else if(Kind==TEXT("number"))
                {
                    double V=0,Min=0,Max=1000;P->Profile->TryGetNumberField(Key,V);O->TryGetNumberField(TEXT("min"),Min);O->TryGetNumberField(TEXT("max"),Max);
                    Add(Label(Caption));Add(SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).Font(FACEPluginStyle::NativeFont("Regular",20)).MinValue(Min).MaxValue(Max).Value(V)
                        .OnValueCommitted_Lambda([this,P,Key](double N,ETextCommit::Type){P->Profile->SetNumberField(Key,N);Save(P);}));
                }
                else if(Kind==TEXT("choice"))
                {
                    Add(Label(Caption));auto Row=SNew(SHorizontalBox);const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
                    if(O->TryGetArrayField(TEXT("values"),Values))
                    {
                    for(const auto& Value:*Values)
                    {
                        const FString Choice=Value->AsString();
                        Row->AddSlot().FillWidth(1).Padding(2)[Button((Text(P->Profile,*Key)==Choice?TEXT("[ "):TEXT(""))+Choice+(Text(P->Profile,*Key)==Choice?TEXT(" ]"):TEXT("")),[this,P,Key,Choice](){P->Profile->SetStringField(Key,Choice);Save(P);Rebuild();})];
                    }
                    }
                    Add(Row);
                }
            }
            const TArray<TSharedPtr<FJsonValue>>* Tools=nullptr;
            if(P->Manifest->TryGetArrayField(TEXT("tools"),Tools))for(const auto& Tool:*Tools)
            {
                if(Tool->AsString()==TEXT("route"))
                {
                    const TArray<TSharedPtr<FJsonValue>>* Route=nullptr;P->Profile->TryGetArrayField(TEXT("route"),Route);
                    Add(Label(FString::Printf(TEXT("Route: %d waypoints. Walk the path and record points at corners. Same landblock only; no automatic jumps or portals."),Route?Route->Num():0),18));
                    Add(Button(TEXT("Record current position"),[this,P](){Host->RecordWaypoint(P->Id);Rebuild();}));
                    Add(Button(TEXT("Remove last waypoint"),[this,P](){const TArray<TSharedPtr<FJsonValue>>* R=nullptr;if(P->Profile->TryGetArrayField(TEXT("route"),R)&&R->Num()){auto Copy=*R;Copy.Pop();P->Profile->SetArrayField(TEXT("route"),Copy);Save(P);Rebuild();}}));
                }
                else if(Tool->AsString()==TEXT("spells"))
                {
                    Add(Label(TEXT("Buff families and attack spell"),24));
                    Add(Label(TEXT("Toggle a Self buff to maintain its family. UCM chooses the strongest known version within your magic skill margin. Choose an attack spell for magic combat."),18));
                    Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",20)).HintText(FText::FromString(TEXT("Filter known spells; press Enter"))).Text(FText::FromString(Search))
                        .OnTextCommitted_Lambda([this](const FText& V,ETextCommit::Type){Search=V.ToString();Rebuild();}));
                    auto List=SNew(SVerticalBox);auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto* D=Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
                    const TArray<TSharedPtr<FJsonValue>>* Buffs=nullptr;P->Profile->TryGetArrayField(TEXT("buffs"),Buffs);
                    TSet<uint32> SelectedCategories; if(Buffs)for(auto V:*Buffs){uint32 S,W,Cat,F;double Dur;if(D->TryGetPluginSpellInfo(V->AsNumber(),S,W,Cat,F,Dur))SelectedCategories.Add(Cat);}
                    TArray<int32> Known=C->GetKnownSpells();Known.Sort();TSet<uint32> Seen;
                    for(int32 Spell:Known)
                    {
                        FString SpellName;uint32 Icon=0,S=0,W=0,Cat=0,F=0;double Dur=0;
                        if(!D->TryGetSpellInfo(Spell,SpellName,Icon)||!D->TryGetPluginSpellInfo(Spell,S,W,Cat,F,Dur)||(!Search.IsEmpty()&&!SpellName.Contains(Search)))continue;
                        const bool Buff=(F&8)&&(F&4)&&Dur>0;
                        if(Buff&&Seen.Contains(Cat))continue;
                        if(Buff)Seen.Add(Cat);else if(F&4)continue;
                        const FString Caption=(Buff?(SelectedCategories.Contains(Cat)?TEXT("[Selected buff] "):TEXT("[Buff] ")):TEXT("[Attack] "))+SpellName;
                        List->AddSlot().AutoHeight().Padding(2)[Button(Caption,[this,P,Spell,Buff,Cat,D](){
                            if(Buff){const TArray<TSharedPtr<FJsonValue>>* B=nullptr;TArray<TSharedPtr<FJsonValue>> Keep;bool Removed=false;
                                if(P->Profile->TryGetArrayField(TEXT("buffs"),B))for(auto V:*B){uint32 S,W,C,F;double Dur;if(D->TryGetPluginSpellInfo(V->AsNumber(),S,W,C,F,Dur)&&C==Cat)Removed=true;else Keep.Add(V);}
                                if(!Removed)Keep.Add(MakeShared<FJsonValueNumber>(Spell));P->Profile->SetArrayField(TEXT("buffs"),Keep);
                            }else P->Profile->SetNumberField(TEXT("attack_spell"),Spell);Save(P);Rebuild();})];
                    }
                    Add(SNew(SBox).HeightOverride(280)[SNew(SScrollBox)+SScrollBox::Slot()[List]]);
                }
            }
            Add(Label(P->HasPresentation()?TEXT("Advanced settings (JSON)"):TEXT("Advanced profile and state rules (JSON)"),24));
            Add(Label(P->HasPresentation()?TEXT("Configuration is separate from this character's saved plugin state. Apply changes here, then Start to resume."):TEXT("Edit buffs, waypoints, and states here, then Apply profile. Invalid scripts stop with an error above. Profile changes stop automation."),18));
            Add(SNew(SBox).HeightOverride(240)[SAssignNew(Json,SACEPluginMultiLineEditableTextBox).Font(FACEPluginStyle::NativeFont("Regular",18)).Text(FText::FromString(Host->ProfileJson(P->Id)))]);
            Add(Button(TEXT("Apply profile"),[this,P](){if(Host->SaveProfile(P->Id,Name->GetText().ToString(),Json->GetText().ToString()))Rebuild();}));
        }
    };
}
} // anonymous namespace

TSharedRef<SWidget> UACEPluginSubsystem::MakePanel(const FString& PluginId)
{
    FACEPluginStyle::Use(this,PluginId);
    // Named panels live inside the desktop frame, which already supplies their title.
    if(IsLedger(PluginId))return MakeLedgerPanel(PluginId);
    if(PluginId==TEXT("itemtools"))return MakeItemToolsPanel(false);
    if(PluginId==TEXT("autocraft"))return MakeAutoCraftPanel(false);
    if(PluginId==TEXT("ucm.log"))return MakeUCMLogPanel();
    if(PluginId==TEXT("ucmmicro"))return MakeUCMMicroPanel();
    if(PluginId==TEXT("actiondeck"))return GetGameInstance()->GetSubsystem<UACEActionDeckSubsystem>()->MakePanel();
    if(PluginId==TEXT("waypoint")||PluginId==TEXT("waypoint.map"))return MakeWaypointPanel(PluginId.EndsWith(TEXT(".map")),false,false);
    if(PluginId==TEXT("waypoint.route"))return MakeWaypointRoutePanel();
    if(PluginId==TEXT("looteditor"))return MakeUCMPanel(TEXT("Standalone loot"),false);
    if(PluginId==TEXT("ucm"))return MakeUCMPanel(FString(),false);
    return SNew(ACEPluginPanelPrivate::SACEPlugins).Host(this).PluginId(PluginId);
}
void UACEPluginSubsystem::TogglePanel()
{
    using namespace ACEPluginPanelPrivate;FACEPluginStyle::Use(this);
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());PC && PC->IsMobileActive() && PC->MobileWidget)
    {
        if(PC->MobileWidget->Panel=="Plugins")PC->MobileWidget->ClosePanel();else PC->MobileWidget->OpenPanel("Plugins");
        return;
    }
    if(auto* PC=GetGameInstance()->GetFirstLocalPlayerController())if(APawn* Pawn=PC->GetPawn())
        if(auto* VR=Pawn->FindComponentByClass<UACEVRComponent>();VR && VR->IsActive())
        {
            VR->OpenPluginManager(); return;
        }
    auto* Viewport=GetGameInstance()->GetGameViewportClient(); if(!Viewport)return;
    if(DesktopPanel){CloseDesktopPluginManager();return;}
    DesktopPanel=SNew(SACEPluginDesktop).Host(this).ManagerOnly(true);
    Viewport->AddViewportWidgetContent(DesktopPanel.ToSharedRef(),1600);
}
void UACEPluginSubsystem::CloseDesktopPluginManager(bool Remember)
{
    if(!DesktopPanel)return;
    DesktopPanel->SaveOverlayLayout();if(Remember)SavePluginWindowVisibility(TEXT("__manager"),false);
    if(auto* Viewport=GetGameInstance()->GetGameViewportClient())Viewport->RemoveViewportWidgetContent(DesktopPanel.ToSharedRef());
    DesktopPanel.Reset();
}
TSharedRef<SWidget> UACEPluginPanel::RebuildWidget()
{
    if(auto* GI=GetWorld()?GetWorld()->GetGameInstance():nullptr) if(auto* Host=GI->GetSubsystem<UACEPluginSubsystem>())return Host->MakePanel();
    return SNew(SACEPluginText).Text(FText::FromString(TEXT("Plugin manager unavailable")));
}
