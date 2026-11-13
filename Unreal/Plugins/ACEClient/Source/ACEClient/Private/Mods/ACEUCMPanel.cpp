#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEVTProfile.h"
#include "Mods/ACEUCMProfiles.h"
#include "ACEUCMLootPresentation.h"
#include "ACEPetDevice.h"
#include "ACEPluginEquipment.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACERetailObjectNames.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SUniformWrapPanel.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Application/SlateApplication.h"

namespace ACEUCMPanelPrivate
{
    const FLinearColor Bg(.025f,.016f,.008f),Card(.012f,.009f,.004f),Ink(1.f,.98f,.92f),Muted(.86f,.83f,.75f),Accent(1,.76f,.25f);
    const FSlateBrush* White(){return FCoreStyle::Get().GetBrush("WhiteBrush");}
    const FButtonStyle& Style() {return FACEPluginStyle::Current()->Button;}
    const FSliderStyle& SliderStyle() {return FACEPluginStyle::Current()->Slider;}
    FString SectionLabel(const FString& Section){return ACEUCMProfiles::DisplaySection(Section);}
    TSharedRef<STextBlock> Text(const FString& S,int Size=16,FLinearColor Color=Ink,bool Wrap=true)
    {return SNew(SACEPluginText).Text(FText::FromString(S)).Font(FACEPluginStyle::NativeFont("Regular",Size)).ColorAndOpacity(Color).AutoWrapText(Wrap);}
    TSharedRef<STextBlock> LiveText(TFunction<FString()> Value)
    {return SNew(SACEPluginText).Text_Lambda([Value](){return FText::FromString(Value());}).Font(FACEPluginStyle::NativeFont("Regular",16)).ColorAndOpacity(Ink).OverflowPolicy(ETextOverflowPolicy::Ellipsis);}
    TSharedRef<SWidget> ChoiceButton(const FString& Caption,TFunction<bool()> Selected,TFunction<void()> Action)
    {return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Tag(FName(*(TEXT("UCMChoice.")+Caption))).ButtonStyle(&Style()).IsFocusable(false).ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Caption))).OnClicked_Lambda([Action](){Action();return FReply::Handled();})
        [SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(14)
            [SNew(SACEPluginText).Text(FText::FromString(TEXT("•"))).ColorAndOpacity(Accent)
                .Visibility_Lambda([Selected](){return Selected()?EVisibility::HitTestInvisible:EVisibility::Hidden;})]]
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(SACEPluginText).Text(FText::FromString(Caption)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
            .Font(FACEPluginStyle::NativeFont("Regular",16)).ColorAndOpacity(Ink)]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(14)]];}
    TSharedRef<SWidget> Button(const FString& Caption,TFunction<void()> Action)
    {const bool Selected=Caption.EndsWith(TEXT(" •"))||Caption.EndsWith(TEXT(" *"));
     return ChoiceButton(Selected?Caption.LeftChop(2):Caption,[Selected](){return Selected;},MoveTemp(Action));}
    TSharedRef<SWidget> MenuButton(const FString& Caption,TFunction<void()> Action)
    {return SNew(SACEPluginButton).Tag(FName(*(TEXT("UCMMenu.")+Caption))).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&FACEPluginStyle::Current()->MenuItem).IsFocusable(false)

        .OnClicked_Lambda([Action](){Action();return FReply::Handled();})[Text(Caption,15)];}
    TSharedRef<SWidget> LiveButton(TFunction<FString()> Caption,TFunction<void()> Action)
    {return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&Style()).IsFocusable(false).OnClicked_Lambda([Action](){Action();return FReply::Handled();})
        [SNew(SACEPluginText).Text_Lambda([Caption](){return FText::FromString(Caption());}).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
            .Font(FACEPluginStyle::NativeFont("Regular",16)).ColorAndOpacity(Ink)];}
    double Num(const TSharedPtr<FJsonObject>& P,const TCHAR* K,double D=0){double V;return P->TryGetNumberField(K,V)?V:D;}
    FString Str(const TSharedPtr<FJsonObject>& P,const TCHAR* K,const FString& D=FString()){FString V;return P->TryGetStringField(K,V)?V:D;}
    bool Bool(const TSharedPtr<FJsonObject>& P,const TCHAR* K,bool D=false){bool V;return P->TryGetBoolField(K,V)?V:D;}

    class SUCMPanel : public SCompoundWidget
    {
        friend class FACEUCMLootEditorTest;
        friend class FACEUCMImportUITest;
    public:
        SLATE_BEGIN_ARGS(SUCMPanel):_ShowTitle(true){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(FString,InitialPage) SLATE_ARGUMENT(bool,ShowTitle) SLATE_END_ARGS()
        void Construct(const FArguments& A)
        {
            Host=A._Host;ShowTitle=A._ShowTitle;FACEPluginStyle::Use(A._Host,A._InitialPage==TEXT("Standalone loot")?TEXT("looteditor"):TEXT("ucm"));if(!A._InitialPage.IsEmpty())Page=A._InitialPage;for(auto P:Host->Plugins)if(P->Id==TEXT("ucm"))Plugin=P;
            if(!Plugin)return;
            if(Page==TEXT("Rules")||Page==TEXT("Mobs")||Page==TEXT("Mob"))Page=TEXT("Monsters");
            if(Page==TEXT("Profiles"))Page=TEXT("Import");
            LegacyFolder=Host->GetProfileFolder();RefreshLibrary();
            EditorOnly=Page==TEXT("Standalone loot");if(EditorOnly)Page=TEXT("Loot");
            if(Page==TEXT("Salvage groups")){Page=TEXT("Loot");EditingSalvage=true;}
            if(Page==TEXT("Loot editor")){Page=TEXT("Loot");ShowLootFilters=true;LootDraft=MakeShared<FJsonObject>();LootDraft->SetStringField(TEXT("action"),TEXT("keep"));LootDraft->SetStringField(TEXT("name_mode"),TEXT("prefix"));}
            ChildSlot[SNew(SBorder).BorderImage(&FACEPluginStyle::Current()->Background).Padding(14)
                [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(SBox).IsEnabled_Lambda([this](){return ImportQueue.IsEmpty();})[Header()]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,12)[SNew(SBox).IsEnabled_Lambda([this](){return ImportQueue.IsEmpty();})[Tabs()]]
                +SVerticalBox::Slot().FillHeight(1)[SAssignNew(BodyScroll,SScrollBox).IsEnabled_Lambda([this](){return ImportQueue.IsEmpty();})+SScrollBox::Slot()[SAssignNew(Body,SVerticalBox)]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(SBox).MaxDesiredHeight(100)
                    .Visibility_Lambda([this](){return !ImportLog.IsEmpty()||(Host.IsValid()&&!Host->Notice.IsEmpty())?EVisibility::Visible:EVisibility::Collapsed;})
                    [SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[SNew(SScrollBox)+SScrollBox::Slot()
                        [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Muted).AutoWrapText(true)
                            .Text_Lambda([this](){return FText::FromString(!ImportLog.IsEmpty()?ImportLog:Host.IsValid()?Host->Notice.Left(16000):FString());})]]
                    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(6,0)[SNew(SVerticalBox)
                        +SVerticalBox::Slot().AutoHeight()[Button(TEXT("Clear"),[this](){ImportLog.Empty();Host->Notice.Empty();})]
                        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility_Lambda([this](){return ImportQueue.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
                            [Button(TEXT("Cancel"),[this](){ImportQueue.Empty();ImportPreviousProfile.Reset();ImportPreviousLinks.Reset();LogImport(TEXT("Import cancelled. Completed imports are available."));Rebuild();})]]]]]]];
            Rebuild();
        }
        virtual void Tick(const FGeometry& Geometry,const double Time,const float Delta) override
        {
            SCompoundWidget::Tick(Geometry,Time,Delta);
            if(ImportQueue.Num())ProcessImport();
            const bool Narrow=Geometry.GetLocalSize().X<700;
            if(Narrow!=LootNarrow){LootNarrow=Narrow;if((Page==TEXT("Loot")&&!EditingSalvage)||Page==TEXT("Route")||Page==TEXT("Overview"))Rebuild();}
            if(RenderedProfile.Pin()!=P())Rebuild();
            if(Page==TEXT("Monsters")&&MonsterFontSize!=FACEPluginStyle::ScaleFont(FACEPluginStyle::NativeFont("Regular",16)).Size)Rebuild();
        }
    private:
        TWeakObjectPtr<UACEPluginSubsystem> Host;TSharedPtr<FACEClientPlugin> Plugin;
        TSharedPtr<SScrollBox> BodyScroll;
        TWeakPtr<FJsonObject> RenderedProfile;
        TSharedPtr<SVerticalBox> Body,SectionBody;TSharedPtr<SEditableTextBox> MonsterName,NewStateName;
        TMap<FString,bool> CollapsedSections;
        TArray<TSharedPtr<FSlateBrush>> Brushes;
        bool ShowTitle=true,EditorOnly=false;FString BuffSearch;
        FString Page=TEXT("Overview"),Search,TransitionWhen=TEXT("elapsed"),TransitionTarget;
        double PauseSeconds=5,TransitionValue=60;float JumpCharge=.5;int32 StateIndex=0,ExpandedMonster=INDEX_NONE;
        TSharedPtr<FJsonObject> P()const{return Plugin->Profile;}
        void Save(bool Stop=false)
        {
            const auto Edited=P();
            if(!Host->SaveCurrentProfile(Plugin->Id,Stop)){Rebuild();return;}
            bool ExternalChange=false;
            // Keep the objects bound to this panel alive after saving. Other
            // panels still see a new root and refresh their own controls.
            for(auto& Pair:P()->Values)
            {
                const auto* Old=Edited->Values.Find(Pair.Key);
                if(Old&&FJsonValue::CompareEqual(**Old,*Pair.Value))Pair.Value=*Old;
                else if(Pair.Key!=TEXT("loot_revision"))ExternalChange=true;
            }
            RenderedProfile=P();if(ExternalChange)Rebuild();
        }
        void Add(TSharedRef<SWidget> W){(SectionBody?SectionBody:Body)->AddSlot().AutoHeight().Padding(0,0,0,10)[W];}
        void Heading(const FString& Title,const FString& Description)
        {
            const FString Key=Page+TEXT("/")+Title;
            const bool DefaultCollapsed=(Page==TEXT("Buffs")&&Title!=TEXT("Automatic buffing"))
                ||(Page==TEXT("Route")&&Title!=TEXT("Route points"))
                ||(Page==TEXT("Buff others")&&Title!=TEXT("Buff requests by tell"))
                ||(Page==TEXT("Combat")&&Title!=TEXT("Automatic Combat"))
                ||(Page==TEXT("Recovery")&&Title!=TEXT("Maintain your vitals"))
                ||(Page==TEXT("Metas")&&Title!=TEXT("Activity states")&&Title!=TEXT("Imported Virindi meta"));
            const bool Collapsed=CollapsedSections.Contains(Key)?CollapsedSections[Key]:DefaultCollapsed;
            const auto Theme=FACEPluginStyle::Current();
            SectionBody=SNew(SVerticalBox);
            Body->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(SBorder).BorderImage(&Theme->SectionFrame).ForegroundColor(Ink).Padding(8)
                [SNew(SExpandableArea).Tag(FName(*Key)).InitiallyCollapsed(Collapsed)
                    .BorderImage(&Theme->SectionHeader).BodyBorderImage(&Theme->TitleBackground)
                    .HeaderPadding(FMargin(8,5))
                    .OnAreaExpansionChanged_Lambda([this,Key](bool Expanded){CollapsedSections.Add(Key,!Expanded);})
                    .HeaderContent()[Text(Title,20)]
                    .BodyContent()[SNew(SBox).Padding(4,12,4,0)[SectionBody.ToSharedRef()]]]];
            if(!Description.IsEmpty())Add(Text(Description,15,Muted));
        }
        TSharedRef<SWidget> Tile(TSharedRef<SWidget> W)
        {return SNew(SBorder).BorderImage(&FACEPluginStyle::Current()->SectionFrame).Padding(8)[W];}
        TMap<FString,FString> SectionNewNames;
        FString CreatingSection,CreationError,DeletingSection,DeletingName,MonsterSearch,MonsterFilter;
        int32 RoutePage=0;
        static constexpr int32 RoutePageSize=8;
        void ResetProfileEditors(){LootDraft.Reset();ConditionDraft.Reset();StateIndex=0;ExpandedMonster=INDEX_NONE;MonsterPage=0;RoutePage=0;}
        void CreateSection(const FString& Section)
        {
            if(!CanLeaveLootRule())return;
            CreationError.Empty();
            if(Host->NewUCMSection(Section,SectionNewNames.FindRef(Section).TrimStartAndEnd()))
            {ResetProfileEditors();CreatingSection.Empty();SectionNewNames.Remove(Section);BodyScroll->ScrollToStart();}
            else CreationError=Host->Notice;
            Rebuild();
        }
        TSharedRef<SWidget> ProfileChoice(const FString& Section)
        {
            return SNew(SACEPluginComboButton).Tag(FName(*(TEXT("UCMProfile.")+Section))).ComboButtonStyle(&FACEPluginStyle::Current()->Combo)
                .ButtonContent()[SNew(SACEPluginText).Tag(FName(*(TEXT("UCMProfileLabel.")+Section))).Font(FACEPluginStyle::NativeFont("Regular",16)).ColorAndOpacity(Accent)
                    .OverflowPolicy(ETextOverflowPolicy::Ellipsis).ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([this,Section](){return FText::FromString(Host->UCMSectionName(Section));})))
                    .Text_Lambda([this,Section](){const FString Name=Host->UCMSectionName(Section);
                        return FText::FromString((Section==TEXT("Monsters")?FString():SectionLabel(Section)+TEXT(": "))+(Name.IsEmpty()?TEXT("Unsaved"):Name));})]
                .OnGetMenuContent_Lambda([this,Section]()->TSharedRef<SWidget>{
                    auto List=SNew(SVerticalBox);
                    for(const auto& Name:Host->UCMSectionProfiles(Section))List->AddSlot().AutoHeight()[MenuButton(Name,[this,Section,Name](){
                        if(!CanLeaveLootRule())return;
                        if(Host->SelectUCMSection(Section,Name)){ResetProfileEditors();CreatingSection.Empty();DeletingSection.Empty();}
                        FSlateApplication::Get().DismissAllMenus();Rebuild();})];
                    if(List->NumSlots()==0)List->AddSlot().AutoHeight()[Text(TEXT("No saved profiles yet. Use New in this section's tab."),14,Muted)];
                    return SNew(SBox).MaxDesiredHeight(240).MinDesiredWidth(280)[SNew(SScrollBox)+SScrollBox::Slot()[List]];
                });
        }
        void SectionSelector(const FString& Section, bool Manage = true)
        {
            SectionBody.Reset();
            auto Row=SNew(SHorizontalBox);
            Row->AddSlot().FillWidth(1)[ProfileChoice(Section)];
            if(Manage)
            {
                Row->AddSlot().AutoWidth().Padding(6,0)[Button(Section==TEXT("Routes")?TEXT("+ New route"):TEXT("+ New"),[this,Section](){if(!CanLeaveLootRule())return;CreatingSection=Section;CreationError.Empty();DeletingSection.Empty();Rebuild();})];
                const FString Name=Host->UCMSectionName(Section);
                Row->AddSlot().AutoWidth()[SNew(SBox).IsEnabled(!Name.IsEmpty()&&!Name.Equals(TEXT("Default"),ESearchCase::IgnoreCase))
                    [Button(TEXT("Delete"),[this,Section,Name](){if(!CanLeaveLootRule())return;DeletingSection=Section;DeletingName=Name;CreatingSection.Empty();Rebuild();})]];
            }
            Add(Tile(Row));
            if(Manage&&CreatingSection==Section)
            {
                Add(SNew(SACEPluginEditableTextBox).Tag(TEXT("UCMNewProfileName")).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16))
                    .Text(FText::FromString(SectionNewNames.FindRef(Section))).HintText(FText::FromString(Section==TEXT("Routes")?TEXT("Route name, e.g. Academy Loop"):TEXT("New profile name")))
                    .OnTextChanged_Lambda([this,Section](const FText& Value){SectionNewNames.Add(Section,Value.ToString());})
                    .OnTextCommitted_Lambda([this,Section](const FText&,ETextCommit::Type How){if(How==ETextCommit::OnEnter)CreateSection(Section);}));
                Add(Text(Section==TEXT("Routes")?TEXT("Creates and selects an empty route file immediately. Use letters, numbers, spaces, _ or - (up to 64 characters). Your previous route is kept."):TEXT("Creates and selects a profile with default settings."),14,Muted));
                if(!CreationError.IsEmpty())Add(SNew(SACEPluginText).Tag(TEXT("UCMProfileCreateError"))
                    .Text(FText::FromString(CreationError)).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Accent).AutoWrapText(true));
                auto Actions=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6,6));
                Actions->AddSlot()[Button(Section==TEXT("Routes")?TEXT("Create and select route"):TEXT("Create profile"),[this,Section](){CreateSection(Section);})];
                Actions->AddSlot()[Button(TEXT("Cancel"),[this](){CreatingSection.Empty();Rebuild();})];Add(Actions);
            }
            if(Manage&&DeletingSection==Section)
            {
                Add(Text(TEXT("Delete ")+DeletingName+TEXT("? Any selection using this profile will switch to Default. A backup will be retained."),15,Muted));
                auto Actions=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6,6));
                Actions->AddSlot()[Button(TEXT("Delete profile"),[this,Section](){if(!CanLeaveLootRule())return;
                    if(Host->DeleteUCMSection(Section,DeletingName)){ResetProfileEditors();DeletingSection.Empty();}Rebuild();})];
                Actions->AddSlot()[Button(TEXT("Cancel"),[this](){DeletingSection.Empty();Rebuild();})];Add(Actions);
            }
            if(Manage&&Section==TEXT("Routes"))
                Add(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Accent).AutoWrapText(true)
                    .Text_Lambda([this](){const TArray<TSharedPtr<FJsonValue>>* Points=nullptr;const int32 Count=P()->TryGetArrayField(TEXT("route"),Points)?Points->Num():0;
                        return FText::FromString(FString::Printf(TEXT("Editing %s.json\n%d saved points | Auto-save on"),*Host->UCMSectionName(TEXT("Routes")),Count));}));
            else if(Manage)Add(Text(TEXT("Changes save to this shared ")+SectionLabel(Section)+TEXT(" profile."),13,Muted));
        }
        void Configuration()
        {
            Heading(TEXT("Configuration"),TEXT("Start or pause activities here or in UCM Micro. Configure each activity in its own tab; use the Mobs chart for attacks, debuffs and pet choices. Changes save automatically."));
            if(!Plugin->Enabled)Add(Button(TEXT("Enable UCM capabilities"),[this](){Host->SetEnabled(Plugin->Id,true);Rebuild();}));
            auto Activities=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12,4));
            Activities->AddSlot()[CombatToggle(TEXT("Enable Combat"))];
            Activities->AddSlot()[Toggle(TEXT("summon_pets"),TEXT("Enable Summoning"),TEXT("Summon eligible essences from the Equipment Pool for mobs allowed by the chart. Works independently of Combat; keeps an existing pet active."),false,false)];
            Activities->AddSlot()[Toggle(TEXT("buffing"),TEXT("Enable Buffs"),TEXT("Maintain buffs."),true,false)];
            Activities->AddSlot()[Toggle(TEXT("buff_others"),TEXT("Accept buff requests"),TEXT("Accept role-based buff requests by tell while UCM and Buffs are running."),false,false)];
            Activities->AddSlot()[Toggle(TEXT("recovery"),TEXT("Enable Recovery"),TEXT("Maintain vitals."),true,false)];Add(Activities);
            auto Grid=SNew(SUniformGridPanel).SlotPadding(FMargin(4,6));int32 Index=0;
            for(const auto& Section:TArray<FString>{TEXT("Character"),TEXT("Routes"),TEXT("Loot"),TEXT("Vendors"),TEXT("Metas"),TEXT("Monsters")})
            {
                auto CardBox=SNew(SVerticalBox);
                const TCHAR* Key=Section==TEXT("Routes")?TEXT("navigation"):Section==TEXT("Loot")?TEXT("looting"):
                    Section==TEXT("Vendors")?TEXT("vendor_restock"):Section==TEXT("Metas")?TEXT("meta_enabled"):nullptr;
                const FString Label=Section==TEXT("Routes")?TEXT("Enable Route Navigation"):Section==TEXT("Loot")?TEXT("Enable Looting"):
                    Section==TEXT("Vendors")?TEXT("Enable Vendor Restocking"):TEXT("Enable Metas");
                CardBox->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(SBox).MinDesiredHeight(30).VAlign(VAlign_Center)
                    [Key?Toggle(Key,Label,TEXT("Use the selected profile while UCM runs."),Section==TEXT("Metas")):Text(SectionLabel(Section))]];
                CardBox->AddSlot().AutoHeight()[ProfileChoice(Section)];
                Grid->AddSlot(LootNarrow?0:Index%2,LootNarrow?Index:Index/2)[Tile(CardBox)];++Index;
            }
            Add(Grid);
            Add(Toggle(TEXT("loot_priority"),TEXT("Prioritize loot"),TEXT("Finish nearby corpses before selecting another combat target.")));
            Add(Toggle(TEXT("nav_priority"),TEXT("Prioritize navigation"),TEXT("Prefer route navigation over combat. Recovery, buffs and prioritized looting still take precedence.")));
            Add(Toggle(TEXT("idle_peace"),TEXT("Peace mode when idle"),TEXT("Enter peace mode when no nearby mobs need attention.")));
            Add(Toggle(TEXT("suspend_world_rendering"),TEXT("Suspend 3D view while UCM runs"),TEXT("Desktop only. Keeps UCM, networking and the interface active.")));
        }
        TSharedRef<SWidget> Header()
        {
            if(EditorOnly)return SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility(ShowTitle?EVisibility::Visible:EVisibility::Collapsed)[Text(TEXT("Loot Profile Editor"),28)]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[Text(TEXT("Edits UCM's active loot profile. Opening or closing this window does not toggle looting."),14,Muted)];
            auto Row=SNew(SHorizontalBox);
            Row->AddSlot().FillWidth(1)[SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility(ShowTitle?EVisibility::Visible:EVisibility::Collapsed)[Text(TEXT("UCM"),22)]]
                +SVerticalBox::Slot().AutoHeight().Padding(0,6)[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Accent).AutoWrapText(true)
                    .Text_Lambda([this](){return FText::FromString(TEXT("Current status: ")+Host->UCMCurrentStatus());})]];
            Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&Style()).IsFocusable(false)
                .OnClicked_Lambda([this](){if(Plugin->Running)Host->Stop(Plugin->Id,TEXT("Stopped"),true);else Host->Start(Plugin->Id);return FReply::Handled();})
                [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Bold",18)).ColorAndOpacity(Ink)
                    .Text_Lambda([P=Plugin](){return FText::FromString(P->Running?TEXT("Stop"):P->CanResumeMeta?TEXT("Resume"):TEXT("Start"));})]];
            return Row;
        }
        TSharedRef<SWidget> Tabs()
        {
            auto Row=SNew(SUniformWrapPanel).SlotPadding(FMargin(2)).MinDesiredSlotWidth(92).MinDesiredSlotHeight(38)
                .MaxDesiredSlotHeight_Lambda([](){return 38.f*FMath::Max(1.f,FACEPluginStyle::ScaleFont(FACEPluginStyle::NativeFont("Regular",15)).Size/15.f);}).HAlign(HAlign_Fill).EvenRowDistribution(true);
            const TArray<FString> Names=EditorOnly?TArray<FString>{TEXT("Vendors"),TEXT("Loot rules"),TEXT("Import")}
                :TArray<FString>{TEXT("Overview"),TEXT("Buffs"),TEXT("Buff others"),TEXT("Combat"),TEXT("Recovery"),TEXT("Route"),TEXT("Loot"),TEXT("Vendors"),TEXT("Monsters"),TEXT("Metas"),TEXT("Import"),TEXT("Log")};
            if(EditorOnly)Row->SetNumColumnsOverride(3);
            for(const auto& Name:Names)
            {
                const FString TargetPage=EditorOnly&&Name==TEXT("Loot rules")?TEXT("Loot"):EditorOnly&&Name==TEXT("Import")?TEXT("Import tools"):Name;
                Row->AddSlot()[SNew(SACEPluginTab).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).Tag(TEXT("UCMTab")).Text(FText::FromString(SectionLabel(Name)))
                    .Selected_Lambda([this,TargetPage,Name](){return Page==TargetPage||(Name==TEXT("Import")&&(Page==TEXT("Files")||Page.StartsWith(TEXT("Import "))||Page==TEXT("Legacy loot")));})
                    .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Name==TEXT("Import")?TEXT("Import / compatibility"):SectionLabel(Name))))
                    .OnClicked_Lambda([this,TargetPage](){Page=TargetPage;BodyScroll->ScrollToStart();Rebuild();return FReply::Handled();})];
            }
            return Row;
        }
        TSharedRef<SWidget> CombatToggle(const FString& Caption=TEXT("Combat (automatic skill and equipment selection)"))
        {
            return SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsChecked_Lambda([this](){return Str(P(),TEXT("combat"),TEXT("off"))!=TEXT("off")?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Run automatic attacks allowed by the Mobs chart. Turning this off pauses all automatic attacks without changing mob rules or summoning."))))
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State){P()->SetStringField(TEXT("combat"),State==ECheckBoxState::Checked?TEXT("auto"):TEXT("off"));P()->SetBoolField(TEXT("manual_combat"),false);Save();})
                [Text(Caption,16)];
        }
        TSharedRef<SWidget> Toggle(const TCHAR* Key,const FString& Caption,const FString& Help,bool Default=false,bool WrapText=true)
        {
            return SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsChecked_Lambda([this,Key,Default](){return Bool(P(),Key,Default)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this,Key](ECheckBoxState State){P()->SetBoolField(Key,State==ECheckBoxState::Checked);Save();})
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Help)))
                [SNew(SBox).MinDesiredHeight(32).VAlign(VAlign_Center).Padding(8,0)[Text(Caption,16,Ink,WrapText)]];
        }

        void Slider(const TCHAR* Key,const FString& Caption,float Min,float Max,float Default,FLinearColor Color=Accent,const FString& Unit=TEXT("%"))
        {
            auto V=SNew(SVerticalBox);
            V->AddSlot().AutoHeight()[SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Text(Caption,18)]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Bold",18)).ColorAndOpacity(Color)
                    .Text_Lambda([this,Key,Default,Unit](){return FText::FromString(FString::Printf(TEXT("%.0f%s"),Num(P(),Key,Default),*Unit));})]];
            V->AddSlot().AutoHeight().Padding(0,5)[SNew(SSlider).Tag(FName(Key)).Style(&SliderStyle()).MinValue(Min).MaxValue(Max).StepSize(1).MouseUsesStep(true).SliderHandleColor(FLinearColor::White).SliderBarColor(FLinearColor::White)
                .Value_Lambda([this,Key,Default](){return float(Num(P(),Key,Default));})
                .OnValueChanged_Lambda([this,Key](float N){P()->SetNumberField(Key,FMath::RoundToInt(N));})
                .OnMouseCaptureEnd_Lambda([this](){Save();}).OnControllerCaptureEnd_Lambda([this](){Save();})];Add(Tile(V));
        }
        void Choices(const TCHAR* Key,const TArray<FString>& Values)
        {
            auto Row=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(5,5));
            for(const auto& V:Values)
            {
                Row->AddSlot()[SNew(SACEPluginTab).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Text(FText::FromString(V)).FontSize(16)
                .Selected_Lambda([this,Key,V](){return Str(P(),Key)==V;})
                .OnClicked_Lambda([this,Key,V](){P()->SetStringField(Key,V);Save();return FReply::Handled();})];
            }
            Add(Row);
        }
        void ToggleId(const TCHAR* Key,uint32 Id)
        {
            const TArray<TSharedPtr<FJsonValue>>* Old=nullptr;TArray<TSharedPtr<FJsonValue>> List;
            bool Removed=false;
            if(P()->TryGetArrayField(Key,Old))
            {
                for(auto V:*Old)
                {
                    if(uint32(V->AsNumber())==Id)Removed=true;
                    else List.Add(V);
                }
            }
            if(!Removed)List.Add(MakeShared<FJsonValueNumber>(Id));P()->SetArrayField(Key,List);Save();
        }
        bool Has(const TCHAR* Key,uint32 Id)const
        {const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;if(P()->TryGetArrayField(Key,Values))for(auto V:*Values)if(uint32(V->AsNumber())==Id)return true;return false;}
        TSharedRef<SWidget> Icon(uint32 Did)
        {
            auto* R=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->GetUIResourceResolver();
            TSharedPtr<FSlateBrush> B=MakeShared<FSlateBrush>();B->SetResourceObject(R?R->ResolveIconTexture(Did):nullptr);B->ImageSize=FVector2D(32,32);Brushes.Add(B);
            return SNew(SBox).WidthOverride(36).HeightOverride(36).HAlign(HAlign_Center).VAlign(VAlign_Center)[SNew(SImage).Image(B.Get())];
        }
        void AttackSpellControls()
        {
            auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            auto* D=Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
            Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[LiveText([this,D](){FString Name=TEXT("Automatic attack spell");uint32 Icon=0;if(int32 Id=int32(Num(P(),TEXT("attack_spell"))))D->TryGetSpellInfo(Id,Name,Icon);return Name;})]
                .OnGetMenuContent_Lambda([this,C,D]()
                {
                    auto List=SNew(SVerticalBox);
                    auto Choose=[this](int32 Id){P()->SetNumberField(TEXT("attack_spell"),Id);FSlateApplication::Get().DismissAllMenus();Save();};
                    List->AddSlot().AutoHeight()[MenuButton(TEXT("Automatic attack spell"),[Choose](){Choose(0);})];
                    auto Known=C->GetKnownSpells();Known.Sort();
                    for(int32 Id:Known)
                    {
                        uint32 School,Power,Category,Flags,IconId;double Duration;FString Name;
                        if(!D->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration)||(Flags&4))continue;
                        const bool Attack=(Category>=117&&Category<=130)||(Category>=222&&Category<=228)||(Category>=243&&Category<=249)||(Category>=639&&Category<=641);
                        if(!Attack||!D->TryGetSpellInfo(Id,Name,IconId))continue;
                        List->AddSlot().AutoHeight()[MenuButton(Name,[Choose,Id](){Choose(Id);})];
                    }
                    return SNew(SBox).MaxDesiredHeight(350)[SNew(SScrollBox)+SScrollBox::Slot()[List]];
                }));
        }
        void Inventory(bool Consumables)
        {
            auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();TSet<int32> Seen;
            const TCHAR* Key=Consumables?TEXT("consumables"):TEXT("weapon_items");
            Heading(Consumables?TEXT("Recovery items"):TEXT("Equipment pool"),Consumables
                ?TEXT("An empty list allows automatic selection. Add supply types to restrict the pool. Refresh updates this list after inventory changes.")
                :TEXT("Select weapons to restrict automatic equipment selection. Selecting only essences leaves weapon selection automatic. In a selected pool, only selected essences are summoned or refilled. An empty pool allows automatic selection of both. Refresh updates this list after inventory changes."));
            Add(Button(TEXT("Refresh inventory"),[this](){Rebuild();}));
            for(const auto& O:C->GetWorldObjects())
            {
                if(!Host->IsOwnedPluginItem(O)||(Consumables&&Seen.Contains(O.WeenieClassId)))continue;
                const auto* A=Consumables?Host->GetPluginAppraisals().Find(O.Guid):Host->GetEquipmentAppraisal(O);
                const bool Equipped=C->GetPlayerGuid()!=0&&(O.WielderId==C->GetPlayerGuid()||(O.CurrentWieldedLocation&&O.ParentGuid==C->GetPlayerGuid()));
                bool RequiresServerCheck=false;
                const FString WieldFailure=A&&A->bSuccess&&!Consumables&&!Equipped?ACEPluginEquipment::WieldFailure(*A,C->GetPlayerVitalsView(),&RequiresServerCheck):FString();
                const bool Pet=ACEPetDevice::IsSummoningDevice(A);
                const bool Weapon=Pet||((O.ItemType&(ACEItemType::MeleeWeapon|ACEItemType::MissileWeapon|ACEItemType::Caster))!=0 && !(O.ValidLocations&0x800000))||((O.ItemType&ACEItemType::Armor)&&(O.ValidLocations&0x200000));
                if(Consumables?Weapon||!ACEInventoryRulesUsable(O):!Weapon)continue;
                Seen.Add(O.WeenieClassId);
                auto Row=SNew(SHorizontalBox);Row->AddSlot().AutoWidth()[Icon(O.IconId)];
                auto Details=SNew(SVerticalBox);Details->AddSlot().AutoHeight()[Text(ACERetailObjectNames::Name(O))];
                if(Pet)Details->AddSlot().AutoHeight().Padding(0,3)[Text(FString::Printf(TEXT("Pet essence  •  %d charges  •  Level %d  •  Summoning %d"),O.Structure,A->IntProperties.FindRef(369),A->IntProperties.FindRef(367)),12,Muted)];
                else if(A&&A->bSuccess&&!Consumables)
                {
                    FString Summary;
                    if(O.ItemType&ACEItemType::Caster)
                    {
                        Summary=TEXT("Casting device");
                        const int32 Damage=A->IntProperties.FindRef(45);
                        for(const auto& Element:MonsterOptions(TEXT("damage_type")))if(FCString::Atoi(*Element.Key)==Damage)
                        {if(Damage)Summary+=TEXT("  •  ")+Element.Value;break;}
                        if(const double* Modifier=A->FloatProperties.Find(152))Summary+=FString::Printf(TEXT("  •  Elemental damage ×%.2f"),*Modifier);
                    }
                    else Summary=FString::Printf(TEXT("Damage %d  •  Modifier %.2f  •  Speed %d"),A->Damage,A->WeaponDamageMod,A->WeaponTime);
                    if(Equipped)Summary+=TEXT("  •  Equipped");
                    Details->AddSlot().AutoHeight().Padding(0,3)[Text(Summary,12,Muted)];
                    if(!WieldFailure.IsEmpty())Details->AddSlot().AutoHeight()[Text(WieldFailure,12,Muted)];
                    else if(RequiresServerCheck)Details->AddSlot().AutoHeight()[Text(TEXT("Custom requirements are checked by the server when equipping."),12,Muted)];
                }
                Row->AddSlot().FillWidth(1).Padding(10,0).VAlign(VAlign_Center)[Details];
                Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[Text(!A||!A->bSuccess?TEXT("Inspecting"):!WieldFailure.IsEmpty()?TEXT("Unavailable"):TEXT("Assessed"),12,Muted)];
                const uint32 Entry=Consumables?uint32(O.WeenieClassId):uint32(O.Guid);
                Row->AddSlot().AutoWidth().Padding(8,0)[ChoiceButton(TEXT("Use"),[this,Key,Entry](){return Has(Key,Entry);},[this,Key,Entry](){ToggleId(Key,Entry);})];Add(Tile(Row));
            }
        }
        bool ACEInventoryRulesUsable(const FACEWorldObject& O)const{return !(O.ItemUseable&1)&&!(O.ItemType&(ACEItemType::Container|ACEItemType::SpellComponents|ACEItemType::Money));}
        void MonsterEquipment(const TSharedPtr<FJsonObject>& Rule,bool Offhand,bool DefaultMelee=false)
        {
            const TCHAR* Key=DefaultMelee?(Offhand?TEXT("melee_secondary_equip"):TEXT("melee_weapon")):(Offhand?TEXT("secondary_equip"):TEXT("weapon"));
            const auto Display=DefaultMelee?Rule:EffectiveMonster(Rule);
            const double Selected=Num(Display,Key,Offhand?0:-1);
            const TArray<FString> Modes=Offhand?TArray<FString>{TEXT("Automatic"),TEXT("Shield"),TEXT("Dual wield"),TEXT("Empty hand")}:DefaultMelee?TArray<FString>{TEXT("Automatic")}:TArray<FString>{TEXT("Automatic"),TEXT("Casting device")};
            FString Caption=TEXT("Missing item — choose another");
            for(int32 I=0;I<Modes.Num();++I)if(Selected==(Offhand?I:I-1))Caption=Modes[I];
            if(Offhand&&!Display->HasField(Key))Caption=DefaultMelee?TEXT("Left-hand Tether / keep current"):TEXT("Use default hand settings");
            auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            TArray<FACEWorldObject> Items;
            for(const auto& O:C->GetWorldObjects())if(Host->IsOwnedPluginItem(O))
            {
                if(uint32(O.Guid)==Selected)Caption=ACERetailObjectNames::Name(O);
                const bool Melee=(O.ItemType&ACEItemType::MeleeWeapon)&&!(O.ValidLocations&0x2000000);
                const bool Shield=(O.ItemType&ACEItemType::Armor)&&(O.ValidLocations&0x200000);
                const bool Primary=(O.ItemType&(ACEItemType::MeleeWeapon|ACEItemType::MissileWeapon|ACEItemType::Caster))&&!(O.ValidLocations&0x800000);
                if(Offhand?(Melee||Shield):Primary&&(!DefaultMelee||(O.ItemType&ACEItemType::MeleeWeapon))&&!(O.ObjectDescriptionFlags&ACEObjectDescFlag::WieldLeft))Items.Add(O);
            }
            Items.Sort([](const auto& A,const auto& B){return ACERetailObjectNames::Name(A)<ACERetailObjectNames::Name(B);});
            Add(SNew(SACEPluginComboButton).Tag(FName(*(DefaultMelee?FString(TEXT("UCMEquipment."))+Key:FString(TEXT("UCMMobDetail."))+Str(Rule,TEXT("name"))+TEXT(".")+Key)))
                .ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[Text((Offhand?TEXT("Offhand: "):DefaultMelee?TEXT("Main hand: "):TEXT("Weapon: "))+Caption)]
                .OnGetMenuContent_Lambda([this,Rule,Key,Offhand,Modes,Items](){
                    auto List=SNew(SVerticalBox);
                    auto Select=[this,Rule,Key](double Value){Rule->SetNumberField(Key,Value);FSlateApplication::Get().DismissAllMenus();Save(true);Rebuild();};
                    if(Offhand||Bool(Rule,TEXT("inherit_default")))List->AddSlot().AutoHeight()[MenuButton(TEXT("Use default / Left-hand Tether"),[this,Rule,Key](){Rule->RemoveField(Key);FSlateApplication::Get().DismissAllMenus();Save(true);Rebuild();})];
                    for(int32 I=0;I<Modes.Num();++I)List->AddSlot().AutoHeight()[MenuButton(Modes[I],[Select,I,Offhand](){Select(Offhand?I:I-1);})];
                    for(const auto& O:Items)List->AddSlot().AutoHeight()[MenuButton(ACERetailObjectNames::Name(O)+((O.ObjectDescriptionFlags&ACEObjectDescFlag::WieldLeft)?TEXT(" (Left-hand Tether)"):TEXT("")),[Select,Id=uint32(O.Guid)](){Select(Id);})];
                    return SNew(SBox).MaxDesiredHeight(400)[SNew(SScrollBox)+SScrollBox::Slot()[List]];
                }));
        }
        void IntegerSetting(const TSharedPtr<FJsonObject>& Object,const TCHAR* Key,const FString& Caption,int32 Min,int32 Max,const FString& Unit,const FString& DefaultKey=FString())
        {
            auto Current=[this,Object,Root=Object==P()](){return Root?P():Object;};
            auto Fallback=[this,Object,Key,DefaultKey](){return Bool(Object,TEXT("inherit_default"))?Num(MonsterDefaults(),Key,Num(P(),*DefaultKey)):Num(P(),*DefaultKey);};
            const bool Override=Object->HasField(Key)&&(FString(Key)!=TEXT("range")||Num(Object,Key)>0);
            if(!DefaultKey.IsEmpty())Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check)
                .IsChecked(Override?ECheckBoxState::Unchecked:ECheckBoxState::Checked)
                .OnCheckStateChanged_Lambda([this,Object,Key,Fallback](ECheckBoxState State){
                    if(State==ECheckBoxState::Checked)Object->RemoveField(Key);
                    else Object->SetNumberField(Key,FString(Key)==TEXT("range")?FMath::Max(1,FMath::RoundToInt(Fallback())):FMath::RoundToInt(Fallback()));Save();Rebuild();})
                [Text(Caption+TEXT(": use default"),14,Muted)]);
            auto Value=[Current,Key,DefaultKey,Fallback](){const double N=Num(Current(),Key,DefaultKey.IsEmpty()?0:Fallback());
                return float(!DefaultKey.IsEmpty()&&FString(Key)==TEXT("range")&&N<=0?Fallback():N);};
            auto Box=SNew(SVerticalBox);
            Box->AddSlot().AutoHeight()[SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Text(Caption,16)]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SACEPluginText).ColorAndOpacity(Accent)
                    .Text_Lambda([Value,Unit](){return FText::FromString(FString::FromInt(FMath::RoundToInt(Value()))+Unit);})]];
            Box->AddSlot().AutoHeight().Padding(0,6)[SNew(SSlider).Tag(FName(Key)).Style(&SliderStyle()).MinValue(Min).MaxValue(Max).StepSize(1).MouseUsesStep(true)
                .IsEnabled(DefaultKey.IsEmpty()||Override).Value_Lambda(Value)
                .OnValueChanged_Lambda([Current,Key](float N){Current()->SetNumberField(Key,FMath::RoundToInt(N));})
                .OnMouseCaptureEnd_Lambda([this](){Save();}).OnControllerCaptureEnd_Lambda([this](){Save();})];Add(Tile(Box));
        }
        void MonsterDetails(const TSharedPtr<FJsonObject>& Rule)
        {
            if(!Bool(Rule,TEXT("default")))
            {
            Add(Text(TEXT("Mob name or expression"),14,Muted));
            Add(SNew(SACEPluginEditableTextBox).Tag(TEXT("UCMMobDetail.Name")).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(Str(Rule,TEXT("name"))))
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Plain text matches the beginning of a name. Examples: species==Tusker; name==Snow Tusker; name#`^Snow.*`; species==Tusker && maxhp>1000. Use || for OR."))))
                .OnTextCommitted_Lambda([this,Rule](const FText& Value,ETextCommit::Type){FString Error;if(ACEVTProfile::SetMonsterMatch(Rule,Value.ToString(),Error)){Save();Rebuild();}else Host->Notice=Error;}));
            if(!Rule->HasField(TEXT("expression")))Add(SNew(SCheckBox).Tag(TEXT("UCMMobDetail.Exact")).Style(&FACEPluginStyle::Current()->Check)
                .IsChecked_Lambda([Rule](){return Bool(Rule,TEXT("exact"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this,Rule](ECheckBoxState State){Rule->SetBoolField(TEXT("exact"),State==ECheckBoxState::Checked);SaveMonster(Rule);})
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Match the entire name when checked; otherwise match names beginning with this text."))))[Text(TEXT("Match exact name"),16)]);
            }
            Add(Text(TEXT("Attack profile"),18,Accent));
            for(const auto& Field:TArray<TPair<FString,FString>>{{TEXT("combat"),TEXT("Combat mode")},{TEXT("damage_type"),TEXT("Attack damage type")},{TEXT("pet_damage_type"),TEXT("Pet damage type")}})
            {
                Add(Text(Field.Value,14,Muted));
                Add(MonsterChoice(Rule,Field.Key,MonsterOptions(Field.Key),Field.Key!=TEXT("combat"),true));
            }
            MonsterDetailFlags(Rule);
            Add(Text(TEXT("Secondary vulnerability"),14,Muted));
            Add(MonsterChoice(Rule,TEXT("secondary_vuln"),MonsterOptions(TEXT("secondary_vuln")),true,true));
            Add(Text(TEXT("Equipment"),18,Accent));
            MonsterEquipment(Rule,false);MonsterEquipment(Rule,true);
            Add(Text(TEXT("Targeting"),18,Accent));
            IntegerSetting(Rule,TEXT("priority"),TEXT("Priority"),-1,10,TEXT(""));
            IntegerSetting(Rule,TEXT("range"),TEXT("Attack Range"),0,100,TEXT(" m"),TEXT("monster_default_range"));
            Add(Text(TEXT("Higher priority is attacked first; negative priority ignores the mob. Range 0 uses the shared attack distance. Power, spell selection and timing are shared settings in Combat."),14,Muted));
        }
        void AddMonster(const FString& Value)
        {
            const FString Name=Value.TrimStartAndEnd();auto Rule=MakeShared<FJsonObject>();FString Error;
            if(!ACEVTProfile::SetMonsterMatch(Rule,Name,Error)){Host->Notice=Error;return;}
            const TArray<TSharedPtr<FJsonValue>>* Old=nullptr;TArray<TSharedPtr<FJsonValue>> Rules;if(P()->TryGetArrayField(TEXT("monsters"),Old))Rules=*Old;
            MonsterSearch.Empty();MonsterFilter.Empty();CollapsedSections.Add(TEXT("Monsters/")+Name,false);
            int32 CustomCount=0;bool HasDefault=false;
            for(int32 I=0;I<Rules.Num();++I)
            {
                const auto Existing=Rules[I]->AsObject();const bool Default=Bool(Existing,TEXT("default"));HasDefault|=Default;
                if(Str(Existing,TEXT("name")).Equals(Name,ESearchCase::IgnoreCase))
                {ExpandedMonster=I;MonsterPage=CustomCount/25;Host->Notice=TEXT("This mob already has a rule.");Rebuild();return;}
                if(!Default)++CustomCount;
            }
            if(Rules.Num()>=(HasDefault?512:511)){Host->Notice=TEXT("This profile has reached its limit of 511 mob rules plus Default.");return;}
            CopyMonsterDefaults(Rule,false);Rule->SetNumberField(TEXT("priority"),0);
            Rules.Add(MakeShared<FJsonValueObject>(Rule));P()->SetArrayField(TEXT("monsters"),Rules);ExpandedMonster=INDEX_NONE;MonsterPage=CustomCount/25;Save();Rebuild();
        }
        void AddSelectedMonster()
        {
            auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Monster;
            if(!C||!C->GetWorldObject(C->GetSelectedObject().Guid,Monster)||!Monster.IsAttackable()||Monster.bIsPlayer||Monster.PetOwnerId||!(Monster.ItemType&ACEItemType::Creature))
            {Host->Notice=TEXT("Select a mob in the game first.");return;}
            AddMonster(Monster.Name);
        }
        #include "ACEUCMMonsterPanel.inl"
        void ProtectionControls()
        {
            Heading(TEXT("Protections and banes"),TEXT("Choose damage protections and equipment banes. Saved in the active Character profile."));
            for(bool Bane:{false,true})
            {
                const FString Key=Bane?TEXT("bane_profile"):TEXT("protection_profile"),Custom=Bane?TEXT("bane_custom"):TEXT("protection_custom");
                const TArray<FString> Labels=Bane?TArray<FString>{TEXT("Custom"),TEXT("All"),TEXT("None"),TEXT("B"),TEXT("BPS"),TEXT("BPSA"),TEXT("BPSAC"),TEXT("ALFC")}:TArray<FString>{TEXT("Custom"),TEXT("All"),TEXT("None"),TEXT("B"),TEXT("BPS"),TEXT("BPSA"),TEXT("ALFC"),TEXT("BPSAC")};
                Add(SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo).ButtonContent()[LiveText([this,Key,Bane,Labels](){return (Bane?TEXT("Banes: "):TEXT("Protections: "))+Labels[FMath::Clamp(int32(Num(P(),*Key,2))-1,0,7)];})]
                    .OnGetMenuContent_Lambda([this,Key,Labels]()->TSharedRef<SWidget>{auto List=SNew(SVerticalBox);
                        for(int32 I=0;I<Labels.Num();++I)List->AddSlot().AutoHeight()[MenuButton(Labels[I],[this,Key,I](){P()->SetNumberField(Key,I+1);Save();FSlateApplication::Get().DismissAllMenus();})];return List;}));
                Add(SNew(SACEPluginEditableTextBox).Visibility_Lambda([this,Key](){return Num(P(),*Key,2)==1?EVisibility::Visible:EVisibility::Collapsed;}).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(Str(P(),*Custom,TEXT("ALL")))).HintText(FText::FromString(TEXT("A acid, B bludgeon, C cold, L lightning, F fire, P pierce, S slash")))
                    .OnTextCommitted_Lambda([this,Custom](const FText& Value,ETextCommit::Type){FString Letters=Value.ToString().ToUpper();if(Letters.Len()<=7){P()->SetStringField(Custom,Letters);Save();}}));
            }
            Add(Text(TEXT("A = acid, B = bludgeoning, C = cold, L = lightning, F = fire, P = piercing, S = slashing."),14,Muted));
        }
        void ForceBuffControls()
        {
            auto Row=SNew(SHorizontalBox);
            Row->AddSlot().AutoWidth()[LiveButton([](){return FString(TEXT("Force Buff"));},[this](){Host->RequestForceBuff();})];
            Row->AddSlot().AutoWidth().Padding(8,0)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&Style()).IsFocusable(false)
                .IsEnabled_Lambda([this](){return Host->IsForceBuffRequested();})
                .OnClicked_Lambda([this](){Host->CancelForceBuff();return FReply::Handled();})[Text(TEXT("Cancel refresh"))]];
            Add(Row);Add(Text(TEXT("Refresh all enabled buff families now, regardless of remaining time. If UCM is stopped, runs only buffs and recovery, then stops."),14,Muted));
        }
        double BuffMargin()const{return Num(P(),TEXT("buff_skill_margin"),Num(P(),TEXT("skill_margin"),30));}
        int32 BuffSchoolSkill(uint32 School)const
        {
            const int32 SkillIds[]={0,34,33,32,31,43};
            if(School>=UE_ARRAY_COUNT(SkillIds))return 0;
            const auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            for(const auto& Skill:C->GetPlayerVitalsView().Skills)
                if(Skill.SkillId==SkillIds[School]&&Skill.AdvancementClass>=2)return Skill.Current;
            return 0;
        }
        void BuffDifficultyControls()
        {
            Add(Text(TEXT("Spell level by school"),18));
            Add(Text(TEXT("Automatic highest chooses the strongest learned, usable buff. A selected level casts that exact tier; unavailable spells are skipped. These choices affect self buffs and buff requests, not vital recovery."),14,Muted));
            for(const auto& School:TArray<TPair<FString,FString>>{{TEXT("creature_buff_level"),TEXT("Creature Magic")},{TEXT("life_buff_level"),TEXT("Life Magic")},{TEXT("item_buff_level"),TEXT("Item Magic")}})
            {
                const FString Key=School.Key;
                auto Menu=SNew(SVerticalBox);
                for(int32 Level=0;Level<=8;++Level)
                    Menu->AddSlot().AutoHeight()[MenuButton(Level?FString::Printf(TEXT("Level %d"),Level):TEXT("Automatic highest"),[this,Key,Level]()
                    {P()->SetNumberField(Key,Level);Save();FSlateApplication::Get().DismissAllMenus();})];
                Add(SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Text(School.Value)]
                    +SHorizontalBox::Slot().AutoWidth()[SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo)
                        .ButtonContent()[SNew(SBox).MinDesiredWidth(130)[LiveText([this,Key](){const int32 Level=FMath::Clamp(int32(Num(P(),*Key)),0,8);return Level?FString::Printf(TEXT("Level %d"),Level):TEXT("Automatic highest");})]]
                        .MenuContent()[Menu]]);
            }
            Slider(TEXT("buff_skill_margin"),TEXT("Extra skill required above spell difficulty"),0,150,
                float(Num(P(),TEXT("skill_margin"),30)),Accent,TEXT(" points"));
            Add(Text(TEXT("UCM chooses the strongest learned buff whose difficulty plus this buffer fits your current magic skill. Raising the buffer favors easier casts but can lower the spell level. Lower it to allow stronger buffs. This is a skill difference, not a success percentage."),14,Muted));
            auto Presets=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6,4));
            Presets->AddSlot()[Button(TEXT("Highest tiers (0 extra)"),[this](){P()->SetNumberField(TEXT("buff_skill_margin"),0);Save();})];
            Presets->AddSlot()[Button(TEXT("Default (30 extra)"),[this](){P()->SetNumberField(TEXT("buff_skill_margin"),30);Save();})];
            Add(Presets);
            auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            auto* D=Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
            // Cache DAT lookups on panel construction, not during Slate painting.
            TMap<uint32,TMap<uint32,uint32>> Levels;
            for(int32 Id:C->GetKnownSpells())
            {
                uint32 School=0,Power=0,Cat=0,Flags=0,Level=0;double Duration=0;
                if(D->TryGetPluginSpellInfo(Id,School,Power,Cat,Flags,Duration)&&(Flags&4)&&Duration>0
                    &&D->TryGetSpellSchoolAndLevel(Id,School,Level))
                    Levels.FindOrAdd(School).FindOrAdd(Power)=FMath::Max(Levels.FindOrAdd(School).FindRef(Power),Level);
            }
            for(uint32 School:{4u,2u,3u})
            {
                const FString Name=School==4?TEXT("Creature"):School==2?TEXT("Life"):TEXT("Item");
                const auto Tiers=Levels.FindRef(School);
                const FString LevelKey=School==4?TEXT("creature_buff_level"):School==2?TEXT("life_buff_level"):TEXT("item_buff_level");
                Add(Tile(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",15)).ColorAndOpacity(Ink).AutoWrapText(true)
                    .Text_Lambda([this,School,Name,Tiers,LevelKey]()
                    {
                        const int32 Skill=BuffSchoolSkill(School),Buffer=FMath::RoundToInt(BuffMargin());
                        const int32 Limit=Skill-Buffer;uint32 Level=0;
                        const uint32 Requested=uint32(Num(P(),*LevelKey));
                        for(const auto& Tier:Tiers)if(int64(Tier.Key)<=Limit&&(!Requested||Tier.Value==Requested))Level=FMath::Max(Level,Tier.Value);
                        const FString Result=Level?FString::Printf(TEXT("up to level %u learned buffs"),Level):TEXT("no learned buff fits");
                        return FText::FromString(FString::Printf(TEXT("%s: skill %d - buffer %d = difficulty %d max\n%s"),*Name,Skill,Buffer,Limit,*Result));
                    })));
            }
            Add(Text(TEXT("Live skill preview, including skill buffs. Individual buff families may have lower learned tiers; components, exclusions and temporary cast failures can also limit the final choice."),14,Muted));
        }
        void Rebuild()
        {
            if(!Host.IsValid())return;FACEPluginStyle::Use(Host.Get(),EditorOnly?TEXT("looteditor"):TEXT("ucm"));
            RenderedProfile=P();
            SectionBody.Reset();Body->ClearChildren();Brushes.Empty();
            if(Page==TEXT("Log"))
            {
                Add(Button(TEXT("Open floating UCM Log (desktop)"),[this](){Host->TogglePluginWindow(TEXT("ucm.log"));}));
                Add(SNew(SBox).HeightOverride(460)[Host->MakeUCMLogPanel()]);return;
            }
            if(Page==TEXT("Overview"))
            {
                Configuration();
            }
            else if(Page==TEXT("Buffs"))
            {
                SectionSelector(TEXT("Character"));
                Heading(TEXT("Automatic buffing"),TEXT("Uses your highest usable sustained buffs for attributes, trained skills, protections and equipment."));
                ForceBuffControls();
                Add(Toggle(TEXT("fast_cast_buffs"),TEXT("Fast buff movement"),TEXT("Shorten eligible buff recoil with a backward press-and-release in one update. The tap does not move your character or wait for a server reply to release."),true));
                Add(Toggle(TEXT("auto_buffs"),TEXT("Choose buffs automatically"),TEXT("Turn off only when using a manually authored buff list."),true));
                BuffDifficultyControls();
                Add(Toggle(TEXT("item_buffs_enabled"),TEXT("Item enchantment buffs"),TEXT("Maintain item-magic buffs, including armor banes and weapon auras. Turning this off also excludes manually selected item spells."),true));
                Slider(TEXT("refresh_seconds"),TEXT("Rebuff before expiry"),10,300,300,Accent,TEXT(" sec"));
                Add(Text(TEXT("Refreshes before timers expire; nearby expirations are grouped during a buff cycle. Saved refresh settings are preserved."),14,Muted));
                Add(Toggle(TEXT("idle_buff_topoff"),TEXT("Refresh buffs while idle"),TEXT("Tops up buffs after combat and looting when no target is nearby.")));
                Slider(TEXT("idle_buff_seconds"),TEXT("Idle buff refresh window"),60,3600,1200,Accent,TEXT(" sec"));
                ProtectionControls();
                Heading(TEXT("Buff exceptions"),TEXT("Override individual buff families when needed."));
                Add(SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(BuffSearch)).HintText(FText::FromString(TEXT("Find a buff family; press Enter")))
                    .OnTextCommitted_Lambda([this](const FText& Value,ETextCommit::Type){BuffSearch=Value.ToString();Rebuild();}));
                auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto* D=Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();TSet<uint32> Seen;
                auto Known=C->GetKnownSpells();
                // Family labels should represent the strongest learned tier, not
                // the first (usually level I) ID returned by the spellbook.
                TMap<int32,uint32> Powers;
                for(int32 Id:Known){uint32 S=0,Pow=0,Cat=0,F=0;double Dur=0;if(D->TryGetPluginSpellInfo(Id,S,Pow,Cat,F,Dur))Powers.Add(Id,Pow);}
                Known.Sort([&Powers](int32 A,int32 B){const uint32 PA=Powers.FindRef(A),PB=Powers.FindRef(B);return PA==PB?A<B:PA>PB;});
                Add(Text(TEXT("Families show their highest learned sustained buff. Short burst spells require an explicit profile entry. The skill buffer determines the usable tier."),14,Muted));
                for(int32 Id:Known)
                {
                    uint32 School,Power,Cat,Flags,Icon;double Duration;FString Name;
                    if(!D->TryGetPluginSpellInfo(Id,School,Power,Cat,Flags,Duration)||!(Flags&4)||Duration<=0||(Duration<300&&!Has(TEXT("buffs"),Id))||Seen.Contains(Cat))continue;
                    Seen.Add(Cat);D->TryGetSpellInfo(Id,Name,Icon);if(!BuffSearch.IsEmpty()&&!Name.Contains(BuffSearch))continue;
                    Add(Tile(SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth()[IconWidget(Icon)]
                        +SHorizontalBox::Slot().FillWidth(1).Padding(10,0).VAlign(VAlign_Center)[Text(Name)]
                        +SHorizontalBox::Slot().AutoWidth()[ChoiceButton(TEXT("Include"),[this,Id](){return Has(TEXT("buffs"),Id);},[this,Id,Cat](){if(Has(TEXT("excluded_buffs"),Cat))ToggleId(TEXT("excluded_buffs"),Cat);ToggleId(TEXT("buffs"),Id);})]
                        +SHorizontalBox::Slot().AutoWidth()[ChoiceButton(TEXT("Exclude"),[this,Cat](){return Has(TEXT("excluded_buffs"),Cat);},[this,Cat](){ToggleId(TEXT("excluded_buffs"),Cat);})]));
                }
            }
            else if(Page==TEXT("Vendors")){VendorPanel();}
            else if(Page==TEXT("Buff others"))
            {
                SectionSelector(TEXT("Character"));
                Heading(TEXT("Buff requests by tell"),TEXT("Players can request role-based buffs by tell while UCM and Buff are running."));
                Slider(TEXT("buff_other_range"),TEXT("Requester distance"),3,30,20,Accent,TEXT(" m"));
                Add(SNew(SACEPluginText).AutoWrapText(true).ColorAndOpacity(Accent).Text_Lambda([H=Host](){return FText::FromString(H.IsValid()?FString::Printf(TEXT("%d queued  •  %s"),H->QueuedBuffRequests(),*H->BuffRequestStatus):FString());}));
                Add(Button(TEXT("Clear request queue"),[this](){Host->ClearBuffRequests();}));
                Add(SNew(SACEPluginText).AutoWrapText(true).ColorAndOpacity(Ink).Text_Lambda([H=Host](){return FText::FromString(H.IsValid()?H->BuffQueueSummary():FString());}));
                Heading(TEXT("Idle helper buffs"),TEXT("Occasionally cast a random protection, armor, endurance, regeneration or rejuvenation buff on a visible player within 18 metres. Runs after recovery, combat, looting and navigation."));
                Add(Toggle(TEXT("random_helper_buffs"),TEXT("Random helper buffs"),TEXT("Uses eligible learned spells and components. Does not require fellowship membership or assume the recipient needs the buff.")));
                Slider(TEXT("random_helper_interval"),TEXT("Minimum interval"),1,300,5,Accent,TEXT(" s"));
                Heading(TEXT("Automatic fellowship management"),TEXT("Manage an existing fellowship while UCM runs. Tell xp to join, line/list/status for the queue, remove to leave the queue, leader for its leader, or location for members. Recruitment requires a nearby player and a server-confirmed invitation."));
                Add(Toggle(TEXT("auto_fellowship"),TEXT("Manage fellowship automatically"),TEXT("Enable tell recruitment and a FIFO waiting list. Leader changes clear reservations and votes.")));
                Add(Toggle(TEXT("fellowship_open"),TEXT("Keep fellowship open when no one is waiting"),TEXT("The leader closes recruitment while reserving free spots for the waiting list."),true));
                Add(Toggle(TEXT("fellowship_votes"),TEXT("Allow member votes"),TEXT("Members can propose kick, ban, giveleader or setopen. A majority of votes cast wins after two minutes; no quorum is required."),true));
                Slider(TEXT("fellowship_capacity"),TEXT("Server fellowship capacity"),2,100,9,Accent,TEXT(" members"));
                Add(SNew(SACEPluginText).AutoWrapText(true).ColorAndOpacity(Accent).Text_Lambda([H=Host](){return FText::FromString(H.IsValid()?H->FellowshipManagerStatus:FString());}));
                Add(Text(TEXT("A reserved spot lasts five minutes; return nearby and tell xp to claim it. Members: startvote kick/ban/giveleader NAME or startvote setopen true/false, then vote ID yes/no. Leader chat: /ucm fellow vote ID yes/no/abort, /ucm fellow clear, /ucm fellow unban NAME."),14,Muted));
                Heading(TEXT("Tell keywords"),TEXT("Edit a role's tell keyword and press Enter to save."));
                const auto Commands=Host->BuffCommands();
                for(int32 I=0;I<Commands.Num();++I)
                {
                    const auto Command=Commands[I]->AsObject();if(!Command)continue;
                    auto Row=SNew(SHorizontalBox);
                    Row->AddSlot().FillWidth(1).VAlign(VAlign_Center)[Text(Str(Command,TEXT("role")),17)];
                    Row->AddSlot().FillWidth(2).Padding(8,0)[SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Text(FText::FromString(Str(Command,TEXT("keyword"))))
                        .OnTextCommitted_Lambda([this,I](const FText& Text,ETextCommit::Type)
                        {auto V=Host->BuffCommands();const FString Key=Text.ToString().TrimStartAndEnd().ToLower();if(!V.IsValidIndex(I)||Key.IsEmpty()||Key.Len()>64)return;
                         for(int32 J=0;J<V.Num();++J)if(J!=I&&Str(V[J]->AsObject(),TEXT("keyword")).Equals(Key,ESearchCase::IgnoreCase)){Host->Notice=TEXT("That keyword already exists.");return;}
                         V[I]->AsObject()->SetStringField(TEXT("keyword"),Key);P()->SetArrayField(TEXT("buff_commands"),V);Save();})];
                    Add(Tile(Row));
                }
                Add(Text(TEXT("Melee roles include Dual Wield, Dirty Fighting, Recklessness, Sneak Attack and Shield. Unarmed uses Light Weapons in modern retail. Players receive tells with their queue position, turn and completion status."),15,Muted));
                Add(Text(TEXT("Every role includes all six attributes, melee/missile/magic defense, life protections and regeneration, run, jump, healing, lore and mana conversion. Mage adds magic schools; weapon roles add their attack skills. Uses the highest known Other spells within your skill margin, including item buffs on the requester's server-visible equipped gear."),15,Muted));
                Add(Text(TEXT("Keep the intended weapon equipped. Servers may not expose other players' armor as targetable items; UCM reports unavailable buffs instead of claiming those banes succeeded. Self-only spells cannot be cast on someone else."),15,Muted));

            }
            else if(Page==TEXT("Combat"))
            {
                SectionSelector(TEXT("Character"));
                Heading(TEXT("Automatic Combat"),TEXT("Automatically ranks usable equipment and known attack spells based on assessed resistances. Unknown resistances use neutral values; select an element override if needed. Make sure you have a weapon or wand configured in your Equipment Pool for automatic equipment selection to work."));
                Add(Text(TEXT("Choose default and per-mob attacks, debuffs, damage and pet types in the Mobs chart. Start or pause activities in Overview / Micro."),15,Muted));
                const int32 Selection=FMath::Clamp(int32(Num(P(),TEXT("target_select"),1)),1,3);
                const TCHAR* SelectionNames[]={TEXT(""),TEXT("Distance"),TEXT("Angle"),TEXT("Angle nearby, distance farther away")};
                Add(LiveButton([this,SelectionNames](){return FString(TEXT("Target preference: "))+SelectionNames[FMath::Clamp(int(Num(P(),TEXT("target_select"),1)),1,3)];},[this](){P()->SetNumberField(TEXT("target_select"),int(Num(P(),TEXT("target_select"),1))%3+1);Save();}));
                Slider(TEXT("target_angle_range"),TEXT("Nearby angle preference range"),1,30,5,Accent,TEXT(" m"));
                Slider(TEXT("minimum_range"),TEXT("Minimum target distance"),0,50,0,Accent,TEXT(" m"));
                Slider(TEXT("approach_range"),TEXT("Target acquisition range"),0,150,60,Accent,TEXT(" m"));
                Add(Toggle(TEXT("approach"),TEXT("Pursue targets outside attack range"),TEXT("Leave the route to approach visible mobs inside acquisition range, then return after combat. When off, UCM only attacks mobs already within attack range."),true));
                Slider(TEXT("radius"),TEXT("Missile / spell attack range"),5,100,30,Accent,TEXT(" m"));
                Slider(TEXT("melee_range"),TEXT("Melee approach distance"),1,5,2,Accent,TEXT(" m"));
                Add(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",15)).ColorAndOpacity(Accent).AutoWrapText(true)
                    .Text_Lambda([P=Plugin](){return FText::FromString(P->Running?P->TargetingStatus:FString());}));
                Slider(TEXT("monster_attempts"),TEXT("Environment misses before skipping target"),0,20,4);
                Slider(TEXT("monster_blacklist_seconds"),TEXT("Skip unreachable target for"),0,600,120,Accent,TEXT(" s"));
                Add(Text(TEXT("Acquisition is the maximum search distance; 0 uses attack range for imported stationary profiles. Enable pursuit to approach a visible mob before firing. Walls and closed doors exclude targets. Route priority and meta/state settings can override normal combat; the live search status above shows the settings in effect."),15,Muted));
                Slider(TEXT("power_percent"),TEXT("Attack power / accuracy"),0,100,50);
                Add(Toggle(TEXT("auto_attack_power"),TEXT("Automatic attack power"),TEXT("Chooses power for the weapon's attack pattern and damage element."),true));
                Add(Toggle(TEXT("use_recklessness"),TEXT("Use Recklessness"),TEXT("Keeps automatic power in the Recklessness range when the skill is usable.")));
                Heading(TEXT("Melee hands"),TEXT("Choose a default main hand and offhand. Mob rules can override these choices. Left-hand Tether reserves a weapon for the offhand; two-handed weapons cannot use a second weapon."));
                MonsterEquipment(P(),false,true);MonsterEquipment(P(),true,true);
                Heading(TEXT("Spellcasting"),TEXT("Tune the spells enabled in the Mobs chart: ring conditions, arc preference and debuff order."));
                Slider(TEXT("ring_range"),TEXT("Ring target distance"),1,15,5,Accent,TEXT(" m"));
                Slider(TEXT("ring_min_targets"),TEXT("Minimum ring targets"),1,10,4,Accent,TEXT(""));
                const int32 Arcs=FMath::Clamp(int32(Num(P(),TEXT("use_arcs"),1)),1,3);
                const TCHAR* ArcNames[]={TEXT(""),TEXT("Prefer bolts"),TEXT("Prefer arcs at range"),TEXT("Prefer arcs")};
                Add(LiveButton([this,ArcNames](){return FString(ArcNames[FMath::Clamp(int(Num(P(),TEXT("use_arcs"),1)),1,3)]);},[this](){P()->SetNumberField(TEXT("use_arcs"),int(Num(P(),TEXT("use_arcs"),1))%3+1);Save();}));
                Slider(TEXT("arc_range"),TEXT("Arc preference distance"),0,100,5,Accent,TEXT(" m"));
                Add(Toggle(TEXT("switch_debuff_wand"),TEXT("Use attack wand for debuffs"),TEXT("Use the chosen attack wand for magic loadouts. When off, keep the current usable wand.")));
                const int32 DebuffMode=FMath::Clamp(int32(Num(P(),TEXT("debuff_each_first"),1)),1,3);
                const TCHAR* DebuffNames[]={TEXT(""),TEXT("Debuff one target"),TEXT("Debuff same-priority targets first"),TEXT("Debuff all targets first")};
                Add(LiveButton([this,DebuffNames](){return FString(DebuffNames[FMath::Clamp(int(Num(P(),TEXT("debuff_each_first"),1)),1,3)]);},[this](){P()->SetNumberField(TEXT("debuff_each_first"),int(Num(P(),TEXT("debuff_each_first"),1))%3+1);Save();}));
                Add(Toggle(TEXT("debuff_fallback"),TEXT("Allow debuff fallback"),TEXT("Continue attacking if a required debuff is unavailable or fails three times.")));
                Add(LiveButton([this](){return Num(P(),TEXT("debuff_selection"),2)==1?TEXT("Debuff choice: spell strength first"):TEXT("Debuff choice: casting skill first");},[this](){P()->SetNumberField(TEXT("debuff_selection"),Num(P(),TEXT("debuff_selection"),2)==1?2:1);Save();}));
                Add(Text(TEXT("Compares learned spells, debuff wands, physical proc weapons and imported grenade supplies. Equal strength and skill prefer the learned spell. Grenades require their gameinfodb.ugd definition."),14,Muted));
                Add(Toggle(TEXT("cast_jiggle"),TEXT("Casting dodge movement"),TEXT("Optional VT-style backward movement with alternating strafes during casts. Uses normal collision; manual movement cancels it. Off preserves ordinary casting movement.")));
                Add(Toggle(TEXT("jump_out_wand"),TEXT("Jump after activating a wand spell"),TEXT("Optional short jump 0.2 seconds after an innate wand spell begins. Overrides dodge movement for item casts; server cast timers still apply.")));
                AttackSpellControls();
                Heading(TEXT("Summoned pets"),TEXT("Add essences to the equipment list below. Uses server requirements, remaining charges and cooldowns."));
                Add(Text(TEXT("For pets only, use Mode: No attacks and Pet: Auto in the Mobs chart. Enable the Summoning activity in Overview / Micro."),15,Accent));
                Add(Toggle(TEXT("refill_summons"),TEXT("Refill summon essences"),TEXT("Use Encapsulated Spirit from your inventory to refill low-charge essences in the equipment pool. Works independently of automatic summoning.")));
                Slider(TEXT("summon_refill_charges"),TEXT("Refill at or below remaining charges"),0,50,5,Accent,TEXT(" charges"));
                Add(LiveButton([this](){return Num(P(),TEXT("pet_range_mode"))==1?TEXT("Pet range: custom"):TEXT("Pet range: attack distance");},[this](){P()->SetNumberField(TEXT("pet_range_mode"),Num(P(),TEXT("pet_range_mode"))==1?0:1);Save();}));
                Slider(TEXT("pet_range"),TEXT("Custom pet distance"),1,240,5,Accent,TEXT(" m"));
                Slider(TEXT("pet_min_targets"),TEXT("Minimum nearby mobs"),1,20,1);
                Heading(TEXT("Mob defaults"),TEXT("Fallbacks for mobs without an explicit rule override. Higher priority is attacked first; negative priority ignores a mob. Leave range at 0 to use the melee or missile / spell attack distance."));
                IntegerSetting(P(),TEXT("monster_default_priority"),TEXT("Default mob priority"),-1,10,TEXT(""));
                IntegerSetting(P(),TEXT("monster_default_range"),TEXT("Default mob attack range"),0,100,TEXT(" m"));
                Add(Text(TEXT("Mob rules inherit these defaults unless overridden. Range 0 uses the melee or missile / spell distance above."),14,Muted));
                Inventory(false);
            }
            else if(Page==TEXT("Recovery"))
            {
                SectionSelector(TEXT("Character"));
                Heading(TEXT("Maintain your vitals"),TEXT("Recover when a vital falls below its percentage. Prefer the highest eligible Life Magic spell, with supplies as fallback; unknown recovery supplies are inspected automatically. Imported profiles retain their ordered recovery methods."));
                Add(Text(FString::Printf(TEXT("Recovery spells use your current magic skill minus the %.0f-point skill buffer. Components are required unless the server grants an exemption."),Num(P(),TEXT("skill_margin"),30)),14,Muted));
                if(P()->HasField(TEXT("recharge_handlers")))Add(Toggle(TEXT("use_imported_recovery_order"),TEXT("Use imported recovery order"),TEXT("On: preserve this profile's ordered recovery handlers. Turn off to use the spell/consumable preference below."),true));
                Add(SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check)
                    .IsChecked_Lambda([this](){return Bool(P(),TEXT("recovery_supplies_first"))?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState State)
                    {
                        P()->SetBoolField(TEXT("recovery_supplies_first"),State==ECheckBoxState::Checked);
                        // An explicit user choice must take effect even on an imported setup.
                        P()->SetBoolField(TEXT("use_imported_recovery_order"),false);Save();
                    })
                    [SNew(SBox).MinDesiredHeight(32).VAlign(VAlign_Center).Padding(8,0)[Text(TEXT("Prefer kits and consumables"))]]);
                Add(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Muted).AutoWrapText(true)
                    .Text_Lambda([this]()
                    {
                        if(P()->HasField(TEXT("recharge_handlers"))&&Bool(P(),TEXT("use_imported_recovery_order"),true))
                            return FText::FromString(TEXT("Using imported recovery order. Change the preference above to override it."));
                        return FText::FromString(Bool(P(),TEXT("recovery_supplies_first"))
                            ?TEXT("Kits/consumables first, then spells if unavailable or unsuccessful.")
                            :TEXT("Spells first, then kits/consumables if unavailable or unsuccessful."));
                    }));
                Slider(TEXT("health_threshold"),TEXT("Health"),1,95,65,FLinearColor(.95f,.25f,.3f));
                Slider(TEXT("emergency_health_threshold"),TEXT("Emergency stamina-to-health below"),0,75,35,FLinearColor(.95f,.25f,.3f));
                Add(Text(TEXT("Below this health percentage, prefer stamina-to-health independently of the Health slider. Uses the lowest sufficient tier. Zero disables; imported recovery order and supplies-first still take precedence."),12,Muted));
                Slider(TEXT("stamina_threshold"),TEXT("Stamina"),1,95,50,FLinearColor(.95f,.7f,.15f));
                Slider(TEXT("mana_threshold"),TEXT("Mana"),1,95,45,FLinearColor(.2f,.55f,1));
                Slider(TEXT("kit_min_success"),TEXT("Minimum kit success chance"),0,100,0,Accent);
                Add(Toggle(TEXT("kits_in_magic"),TEXT("Use kits in magic stance"),TEXT("Allow healing kits while wielding a casting device."),true));
                Add(Toggle(TEXT("kit_peace"),TEXT("Enter peace before using kits"),TEXT("Switch out of combat before using the selected kit.")));
                Heading(TEXT("Spell components"),TEXT("Imported pea recipes travel with the Character profile. Keep a Splitting Tool and the requested peas in inventory."));
                Add(Toggle(TEXT("split_peas"),TEXT("Split component peas"),TEXT("Uses imported All Peas or named component choices and waits for inventory confirmation."),true));
                Slider(TEXT("component_critical"),TEXT("Critical component minimum"),0,100,4);
                Slider(TEXT("component_normal"),TEXT("Normal component minimum"),0,100,20);
                Slider(TEXT("component_idle"),TEXT("Idle component minimum"),0,100,20);
                Heading(TEXT("Remove vulnerabilities"),TEXT("Remove temporary elemental vulnerabilities within the dispel's power limit. Waits for the server's enchantment update."));
                Add(Toggle(TEXT("dispel_self"),TEXT("Cast Life Magic dispel"),TEXT("Requires a learned self dispel, sufficient magic skill and Chorizite.")));
                Add(Toggle(TEXT("dispel_items"),TEXT("Use dispel supplies"),TEXT("Uses eligible runes, gems, chocolate or potions from inventory.")));
                Heading(TEXT("Fellowship recovery"),TEXT("Restore nearby members using fresh server vitals. Set a threshold to zero to disable that vital."));
                Slider(TEXT("helper_health_threshold"),TEXT("Fellow health"),0,100,0,FLinearColor(.95f,.25f,.3f));
                Slider(TEXT("helper_stamina_threshold"),TEXT("Fellow stamina"),0,100,0,FLinearColor(.95f,.7f,.15f));
                Slider(TEXT("helper_mana_threshold"),TEXT("Fellow mana"),0,100,0,FLinearColor(.2f,.55f,1));
                Heading(TEXT("Equipment mana"),TEXT("Keep equipped items charged using your configured mana supplies."));
                Add(Toggle(TEXT("mana_charges_when_off"),TEXT("Recharge equipment while UCM is stopped"),TEXT("Uses configured mana supplies only; does not start combat, buffs or navigation.")));
                Add(Toggle(TEXT("item_mana"),TEXT("Recharge equipment"),TEXT("Use filled mana stones / charges on the player when equipped item mana is low."),true));
                Slider(TEXT("item_mana_threshold"),TEXT("Equipment mana threshold"),0,99,20,FLinearColor(.2f,.55f,1),TEXT("%"));
                Add(Toggle(TEXT("fill_mana_stones"),TEXT("Automatically fill empty mana stones"),TEXT("While looting, pick up unwanted magical items to fill empty stones. Loot rules take precedence. Tinkered, inscribed, retained and equipped items are excluded; existing inventory is left alone.")));
                Slider(TEXT("mana_tank_minimum"),TEXT("Minimum mana in source loot"),0,10000,1000,Accent,TEXT(""));
                Heading(TEXT("Automatic crafting"),TEXT("Replenish supplies from the bundled recipe catalog using owned, unprotected ingredients. Imported assist and buff items are included. Recovery has priority between crafting actions; each craft waits for its inventory result."));
                Add(Toggle(TEXT("auto_craft"),TEXT("Craft supplies automatically"),TEXT("Make one batch when a configured supply runs out; top up to its minimum while idle.")));
                Add(Toggle(TEXT("craft_confirm"),TEXT("Accept crafting confirmations"),TEXT("Only accepts the crafting prompt for the active recipe. Turn off to answer prompts manually."),true));
                const TCHAR* CraftKeys[]={TEXT("craft_health_kits"),TEXT("craft_stamina_kits"),TEXT("craft_mana_kits"),TEXT("craft_health_food"),TEXT("craft_stamina_food"),TEXT("craft_mana_food")};
                const TCHAR* CraftLabels[]={TEXT("Idle health kits"),TEXT("Idle stamina kits"),TEXT("Idle mana kits"),TEXT("Idle health food"),TEXT("Idle stamina food"),TEXT("Idle mana food")};
                for(int32 I=0;I<6;++I)Slider(CraftKeys[I],CraftLabels[I],0,10000,0,Accent,TEXT(" items"));
                Add(Text(TEXT("Idle counts apply to imported assist item categories. Add other supplies below by selecting an inventory item. A zero idle count still replaces a missing supply. Unknown recipes or missing ingredients are skipped. After a failed or uncertain craft, that supply stays skipped until UCM restarts or automatic crafting is toggled."),14,Muted));
                Add(Button(TEXT("+ Maintain selected inventory item"),[this]()
                {
                    auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Item;
                    if(!C->GetWorldObject(C->GetSelectedObject().Guid,Item)||Item.Name.IsEmpty()||!C->IsOwnedInventoryItem(Item)){Host->Notice=TEXT("Select an item in your inventory first.");return;}
                    const TArray<TSharedPtr<FJsonValue>>* Existing=nullptr;TArray<TSharedPtr<FJsonValue>> Rows;
                    if(P()->TryGetArrayField(TEXT("craft_targets"),Existing))Rows=*Existing;
                    for(const auto& V:Rows)if(Str(V->AsObject(),TEXT("name")).Equals(Item.Name,ESearchCase::IgnoreCase)){Host->Notice=TEXT("That supply is already listed.");return;}
                    if(Rows.Num()>=64){Host->Notice=TEXT("Up to 64 crafting supplies are supported.");return;}
                    auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("name"),Item.Name);Row->SetNumberField(TEXT("count"),1);Rows.Add(MakeShared<FJsonValueObject>(Row));P()->SetArrayField(TEXT("craft_targets"),Rows);Save();Rebuild();
                }));
                const TArray<TSharedPtr<FJsonValue>>* CraftRows=nullptr;
                if(P()->TryGetArrayField(TEXT("craft_targets"),CraftRows))for(int32 I=0;I<CraftRows->Num();++I)
                {
                    const auto Row=(*CraftRows)[I]->AsObject();if(!Row)continue;
                    Add(SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Text(Str(Row,TEXT("name")))]
                        +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[SNew(SBox).WidthOverride(90)[SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit)
                            .ToolTipText(FText::FromString(TEXT("Minimum stock while idle (1-10000)"))).Text(FText::FromString(LexToString(int32(Num(Row,TEXT("count"),1)))))
                            .OnTextCommitted_Lambda([this,I](const FText& T,ETextCommit::Type){const FString V=T.ToString();if(!V.IsNumeric())return;const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;if(P()->TryGetArrayField(TEXT("craft_targets"),Rows)&&Rows->IsValidIndex(I)){(*Rows)[I]->AsObject()->SetNumberField(TEXT("count"),FMath::Clamp(FCString::Atoi(*V),1,10000));Save();}})]]
                        +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Remove"),[this,I](){const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;if(P()->TryGetArrayField(TEXT("craft_targets"),Rows)&&Rows->IsValidIndex(I)){auto Copy=*Rows;Copy.RemoveAt(I);P()->SetArrayField(TEXT("craft_targets"),Copy);Save();Rebuild();}})]);
                }
                Inventory(true);
            }
            else if(Page==TEXT("Route"))
            {
                SectionSelector(TEXT("Routes"));
                Heading(TEXT("Route points"),TEXT("Walk to your starting spot and add a waypoint. Move along the path and add another at each turn. Every point saves immediately to the file above."));
                Add(Button(TEXT("+ Add waypoint at my position"),[this](){AddRoutePoint(TEXT("walk"));}));
                Add(Toggle(TEXT("route_preview"),TEXT("Show route in world"),TEXT("Blue lines connect saved points; amber marks jumps."),true));
                const TArray<TSharedPtr<FJsonValue>>* Route=nullptr;
                const int32 Count=P()->TryGetArrayField(TEXT("route"),Route)?Route->Num():0;
                RoutePage=FMath::Clamp(RoutePage,0,FMath::Max(0,(Count-1)/RoutePageSize));
                if(!Count)Add(Text(TEXT("No points yet. Add your first waypoint above."),15,Accent));
                if(Count>RoutePageSize)
                {
                    auto Pages=SNew(SHorizontalBox);
                    Pages->AddSlot().AutoWidth()[SNew(SBox).IsEnabled(RoutePage>0)[Button(TEXT("Previous"),[this](){--RoutePage;Rebuild();})]];
                    Pages->AddSlot().FillWidth(1).VAlign(VAlign_Center).Padding(8,0)[Text(FString::Printf(TEXT("Points %d–%d of %d"),RoutePage*RoutePageSize+1,FMath::Min(Count,(RoutePage+1)*RoutePageSize),Count),14)];
                    Pages->AddSlot().AutoWidth()[SNew(SBox).IsEnabled((RoutePage+1)*RoutePageSize<Count)[Button(TEXT("Next"),[this](){++RoutePage;Rebuild();})]];Add(Pages);
                }
                for(int I=RoutePage*RoutePageSize;I<FMath::Min(Count,(RoutePage+1)*RoutePageSize);++I)
                {
                    auto O=(*Route)[I]->AsObject();FString Kind=Str(O,TEXT("kind"),TEXT("walk"));
                    if(!Kind.IsEmpty())Kind[0]=FChar::ToUpper(Kind[0]);
                    FString Label=FString::Printf(TEXT("%d. %s"),I+1,*Kind);
                    if(O->HasField(TEXT("object_name")))Label+=TEXT(" — ")+Str(O,TEXT("object_name"));
                    if(Kind==TEXT("Pause"))Label+=FString::Printf(TEXT(" — %.0f seconds"),Num(O,TEXT("seconds")));
                    auto Row=SNew(SHorizontalBox);Row->AddSlot().FillWidth(1).VAlign(VAlign_Center)
                        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Text(Label,15)]
                        +SVerticalBox::Slot().AutoHeight()[Text(FString::Printf(TEXT("%.1f, %.1f, %.1f"),Num(O,TEXT("x")),Num(O,TEXT("y")),Num(O,TEXT("z"))),12,Muted)]];
                    Row->AddSlot().AutoWidth()[SNew(SBox).IsEnabled(I>0)[Button(TEXT("↑"),[this,I](){EditRoute(I,-1);})]];
                    Row->AddSlot().AutoWidth().Padding(4,0)[SNew(SBox).IsEnabled(I+1<Count)[Button(TEXT("↓"),[this,I](){EditRoute(I,1);})]];
                    Row->AddSlot().AutoWidth()[Button(TEXT("Remove"),[this,I](){EditRoute(I,0);})];Add(Tile(Row));
                }
                Heading(TEXT("Special steps"),TEXT("Add an interaction, portal, jump, pause or recall at the end of the route. After a portal, add the next waypoint at your destination."));
                auto Buttons=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(5,5));
                Buttons->AddSlot()[Button(TEXT("+ Use selected"),[this](){AddRoutePoint(TEXT("use"));})];
                Buttons->AddSlot()[Button(TEXT("+ Portal selected"),[this](){AddRoutePoint(TEXT("portal"));})];
                Buttons->AddSlot()[Button(TEXT("+ Jump here"),[this](){AddRoutePoint(TEXT("jump"),JumpCharge);})];Add(Buttons);
                Add(Text(TEXT("Jump charge (0 = minimum, 1 = full). Face the jump direction before recording."),14,Muted));
                Add(SNew(SACEPluginSpinBox<float>).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(1).Delta(.1).Value(JumpCharge).OnValueChanged_Lambda([this](float V){JumpCharge=V;}));
                Add(Text(TEXT("Pause duration (seconds)"),14,Muted));Add(SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).MinValue(1).MaxValue(3600).Value(PauseSeconds).OnValueChanged_Lambda([this](double V){PauseSeconds=V;}));
                Add(Button(TEXT("+ Pause here"),[this](){RecordExtra(TEXT("pause"),TEXT("seconds"),PauseSeconds);}));
                auto* Client=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto* Dat=Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
                auto Recalls=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
                for(int32 Id:Client->GetKnownSpells())
                {
                    uint32 School,Power,Category,Flags,IconId;double Duration;FString Name;
                    if(Dat->TryGetPluginSpellInfo(Id,School,Power,Category,Flags,Duration)&&Category==201)
                    {Dat->TryGetSpellInfo(Id,Name,IconId);Recalls->AddSlot()[Button(TEXT("+ ")+Name,[this,Id](){RecordExtra(TEXT("recall"),TEXT("spell"),Id);})];}
                }
                Add(Recalls);
                Heading(TEXT("Route behavior"),TEXT("Start joins the nearest waypoint, then follows points in order. Route edits stop UCM; press Start when ready."));
                auto Options=SNew(SUniformGridPanel).SlotPadding(FMargin(0,0,8,6));int32 OptionIndex=0;
                auto Option=[&](const TCHAR* Key,const FString& Label,const FString& Help,bool Default=false){
                    Options->AddSlot(LootNarrow?0:OptionIndex%2,LootNarrow?OptionIndex:OptionIndex/2)[Toggle(Key,Label,Help,Default)];++OptionIndex;};
                Option(TEXT("open_doors"),TEXT("Open doors along route"),TEXT("Use a closed door only when it lies on the current walking segment."),true);
                Option(TEXT("follow_corners"),TEXT("Follow around corners"),TEXT("Follow the selected player's observed footsteps in order, retaining bends and elevation changes."));
                Option(TEXT("loop_route"),TEXT("Loop route"),TEXT("Repeat from the first point when the route finishes."));
                Option(TEXT("reverse_route"),TEXT("Reverse at route ends"),TEXT("VT Linear mode: walk back along the same points. Takes priority over Loop."));Add(Options);
                auto Follow=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(5,5));
                Follow->AddSlot()[Button(TEXT("Follow selected player"),[this](){Host->SetUCMFollowSelected(true);Rebuild();})];
                Follow->AddSlot()[Button(TEXT("Stop following"),[this](){Host->SetUCMFollowSelected(false);Rebuild();})];Add(Follow);
            }
            else if(Page==TEXT("Loot"))
            {
                SectionSelector(TEXT("Loot"));
                LootPanel();
            }
            else if(Page==TEXT("Monsters"))
            {
                MonsterMatrix();
            }
            else if(Page==TEXT("Metas"))
            {
                SectionSelector(TEXT("Metas"));
                Add(Button(TEXT("Import vTank Meta (*.met)"),[this](){ImportExtension=TEXT("met");RefreshLibrary();Page=TEXT("Import vTank");Rebuild();}));
                const TArray<TSharedPtr<FJsonValue>>* ImportedRules=nullptr;
                if(P()->TryGetArrayField(TEXT("vt_meta"),ImportedRules))
                {
                    Heading(TEXT("Imported Virindi meta"),TEXT("The meta loads its referenced character, mob, loot and route profiles when their actions run. Some metas wait for a chat command or required items before choosing a route. Start/Stop resumes the current run."));
                    Add(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Bold",18)).Text_Lambda([this](){return FText::FromString(TEXT("Current state: ")+(Plugin->MetaState.IsEmpty()?TEXT("Default"):Plugin->MetaState)+(Plugin->Running?TEXT(""):TEXT(" (stopped)")));}));
                    Add(SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",15)).ColorAndOpacity(Ink).AutoWrapText(true)
                        .Text_Lambda([this](){return FText::FromString(Plugin->MetaStatus);}));
                    TMap<FString,int32> Counts;for(const auto& R:*ImportedRules)++Counts.FindOrAdd(Str(R->AsObject(),TEXT("state")));
                    Counts.FindOrAdd(TEXT("Default"));
                    TArray<FString> Names;Counts.GenerateKeyArray(Names);Names.Sort();
                    auto Controls=SNew(SHorizontalBox);
                    Controls->AddSlot().FillWidth(1)[Button(TEXT("Restart from Default"),[this](){
                        const bool WasRunning=Plugin->Running;
                        if(Host->SelectUCMSection(TEXT("Metas"),Host->UCMSectionName(TEXT("Metas")))&&WasRunning)Host->Start(TEXT("ucm"));
                    })];
                    Controls->AddSlot().FillWidth(1).Padding(6,0,0,0)[SNew(SACEPluginComboButton).ComboButtonStyle(&FACEPluginStyle::Current()->Combo)
                        .IsEnabled_Lambda([this](){return Plugin->Running;}).ButtonContent()[Text(TEXT("Go to state..."))]
                        .OnGetMenuContent_Lambda([this,Names]()->TSharedRef<SWidget>{
                            auto List=SNew(SVerticalBox);
                            for(const auto& Name:Names)List->AddSlot().AutoHeight()[MenuButton(Name,[this,Name](){Host->RunUCMCommand(TEXT("setmetastate ")+Name);FSlateApplication::Get().DismissAllMenus();})];
                            return SNew(SBox).MaxDesiredHeight(300)[SNew(SScrollBox)+SScrollBox::Slot()[List]];
                        })];
                    Add(Controls);
                    Heading(TEXT("Referenced profiles"),TEXT("These shared files are used automatically by the meta. Edit them in their own tabs; their original vTank filenames remain linked."));
                    const TSharedPtr<FJsonObject>* Refs=nullptr;
                    if(P()->TryGetObjectField(TEXT("meta_profile_refs"),Refs))
                    {
                        TArray<FString> Files;for(const auto& Ref:(*Refs)->Values)Files.Add(FString(Ref.Key));Files.Sort();
                        for(const auto& File:Files)
                        {
                            TArray<FString> Parts;
                            for(const auto& Part:(*Refs)->GetObjectField(File)->Values)Parts.Add(FString(Part.Key)+TEXT(": ")+Part.Value->AsString());
                            Parts.Sort();Add(Text(File+TEXT(" → ")+FString::Join(Parts,TEXT(", ")),15));
                        }
                    }
                    Heading(TEXT("States"),TEXT("Rules execute once per state entry, in their original order. Empty routes deliberately stop navigation until another rule loads a route."));
                    for(const auto& Name:Names)Add(Text(FString::Printf(TEXT("%s — %d rules"),*Name,Counts[Name]),16));
                    Add(Button(TEXT("Remove loaded meta"),[this](){Host->ClearProfileFile(TEXT("met"));Rebuild();}));
                    return;
                }
                Heading(TEXT("Activity states"),TEXT("A state can override activities and switch to another state when a condition is met. Conditions run in order; the first match wins. Changes stop the current run."));
                const TArray<TSharedPtr<FJsonValue>>* StateList=nullptr;
                if(!P()->TryGetArrayField(TEXT("states"),StateList)||StateList->IsEmpty())
                {auto Default=MakeShared<FJsonObject>();Default->SetStringField(TEXT("name"),TEXT("Default"));P()->SetArrayField(TEXT("states"),{MakeShared<FJsonValueObject>(Default)});P()->SetStringField(TEXT("initial_state"),TEXT("Default"));P()->TryGetArrayField(TEXT("states"),StateList);}
                StateIndex=FMath::Clamp(StateIndex,0,StateList->Num()-1);
                auto StateTabs=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
                for(int Index=0;Index<StateList->Num();++Index){auto State=(*StateList)[Index]->AsObject();StateTabs->AddSlot()[Button(Str(State,TEXT("name"))+(StateIndex==Index?TEXT(" •"):TEXT("")),[this,Index](){StateIndex=Index;Rebuild();})];}Add(StateTabs);
                Add(SAssignNew(NewStateName,SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).HintText(FText::FromString(TEXT("New state name"))).Font(FACEPluginStyle::NativeFont("Regular",16)));
                Add(Button(TEXT("Create state"),[this](){const FString Name=NewStateName->GetText().ToString().TrimStartAndEnd();if(Name.IsEmpty())return;
                    auto States=P()->GetArrayField(TEXT("states"));for(auto V:States)if(Str(V->AsObject(),TEXT("name"))==Name){Host->Notice=TEXT("State name already exists");return;}
                    auto State=MakeShared<FJsonObject>();State->SetStringField(TEXT("name"),Name);States.Add(MakeShared<FJsonValueObject>(State));P()->SetArrayField(TEXT("states"),States);StateIndex=States.Num()-1;Save(true);Rebuild();}));
                const auto State=(*StateList)[StateIndex]->AsObject();const FString Name=Str(State,TEXT("name"));
                Heading(Name,Str(P(),TEXT("initial_state"))==Name?TEXT("This is the starting state."):TEXT("This state starts when a transition selects it."));
                Add(Button(TEXT("Use as starting state"),[this,Name](){P()->SetStringField(TEXT("initial_state"),Name);Save(true);Rebuild();}));
                for(const TCHAR* Key:{TEXT("buffing"),TEXT("recovery"),TEXT("navigation"),TEXT("looting")})
                {
                    const FString Label=FString(Key)==TEXT("buffing")?TEXT("Buffs"):FString(Key)==TEXT("recovery")?TEXT("Recovery"):FString(Key)==TEXT("navigation")?TEXT("Navigation"):TEXT("Loot");
                    Add(Text(Label,16));auto Row=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
                    for(const FString Value:{TEXT("inherit"),TEXT("on"),TEXT("off")})Row->AddSlot()[ChoiceButton(Value,[this,Key,Value](){auto Current=StateObject();return Current->HasField(Key)?Value!=TEXT("inherit")&&Bool(Current,Key)==(Value==TEXT("on")):Value==TEXT("inherit");},[this,Key,Value](){auto Current=StateObject();if(Value==TEXT("inherit"))Current->RemoveField(Key);else Current->SetBoolField(Key,Value==TEXT("on"));Save(true);})];Add(Row);
                }
                Add(Text(TEXT("Combat mode"),16));auto Modes=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
                for(const FString Mode:{TEXT("inherit"),TEXT("off"),TEXT("auto"),TEXT("melee"),TEXT("missile"),TEXT("magic")})Modes->AddSlot()[ChoiceButton(Mode,[this,Mode](){return Str(StateObject(),TEXT("combat"),TEXT("inherit"))==Mode;},[this,Mode](){if(Mode==TEXT("inherit"))StateObject()->RemoveField(TEXT("combat"));else StateObject()->SetStringField(TEXT("combat"),Mode);Save(true);})];Add(Modes);
                Heading(TEXT("Transitions"),TEXT("Health, stamina and mana thresholds are percentages. Elapsed time is seconds since entering this state."));
                auto Conditions=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
                for(const FString When:{TEXT("health_below"),TEXT("stamina_below"),TEXT("mana_below"),TEXT("elapsed"),TEXT("no_targets"),TEXT("target_available"),TEXT("route_complete")})
                {FString Label=When.Replace(TEXT("_"),TEXT(" "));Conditions->AddSlot()[ChoiceButton(Label,[this,When](){return TransitionWhen==When;},[this,When](){TransitionWhen=When;})];}Add(Conditions);
                Add(SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).MinValue(0).MaxValue(86400).Value(TransitionValue).OnValueChanged_Lambda([this](double V){TransitionValue=V;}));
                Add(Text(TEXT("Then switch to")));auto Destinations=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4,4));
                for(auto V:*StateList){const FString To=Str(V->AsObject(),TEXT("name"));Destinations->AddSlot()[ChoiceButton(To,[this,To](){return To==TransitionTarget;},[this,To](){TransitionTarget=To;})];}Add(Destinations);
                Add(Button(TEXT("Add transition"),[this](){if(TransitionTarget.IsEmpty()){Host->Notice=TEXT("Choose a destination state");return;}auto Rule=MakeShared<FJsonObject>();Rule->SetStringField(TEXT("when"),TransitionWhen);Rule->SetNumberField(TEXT("value"),TransitionValue);Rule->SetStringField(TEXT("next"),TransitionTarget);
                    const TArray<TSharedPtr<FJsonValue>>* Old=nullptr;TArray<TSharedPtr<FJsonValue>> Rules;if(StateObject()->TryGetArrayField(TEXT("transitions"),Old))Rules=*Old;Rules.Add(MakeShared<FJsonValueObject>(Rule));StateObject()->SetArrayField(TEXT("transitions"),Rules);Save(true);Rebuild();}));
                const TArray<TSharedPtr<FJsonValue>>* Rules=nullptr;if(State->TryGetArrayField(TEXT("transitions"),Rules))for(int Index=0;Index<Rules->Num();++Index)
                {auto Rule=(*Rules)[Index]->AsObject();Add(Tile(SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Text(FString::Printf(TEXT("%s %.0f → %s"),*Str(Rule,TEXT("when")).Replace(TEXT("_"),TEXT(" ")),Num(Rule,TEXT("value")),*Str(Rule,TEXT("next"))))]
                    +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Remove"),[this,Index](){auto Copy=StateObject()->GetArrayField(TEXT("transitions"));Copy.RemoveAt(Index);StateObject()->SetArrayField(TEXT("transitions"),Copy);Save(true);Rebuild();})]));}
            }
            else if(Page==TEXT("Import")||Page==TEXT("Files")){ImportHome();}
            else if(Page==TEXT("Import tools"))ImportPanel();
            else if(Page==TEXT("Import vTank"))SectionImportPanel();
            else if(Page==TEXT("Legacy loot"))LegacyLootPanel();
            else
            {
                ImportHome();
            }
        }
        TSharedRef<SWidget> IconWidget(uint32 Id){return Icon(Id);}
        TSharedPtr<FJsonObject> StateObject()const{return P()->GetArrayField(TEXT("states"))[StateIndex]->AsObject();}
        void ShowLastRoutePoint()
        {const TArray<TSharedPtr<FJsonValue>>* Points=nullptr;if(P()->TryGetArrayField(TEXT("route"),Points)&&Points->Num())RoutePage=(Points->Num()-1)/RoutePageSize;}
        void AddRoutePoint(const FString& Kind,float Charge=0)
        {Host->RecordRouteAction(Kind,Charge);ShowLastRoutePoint();Rebuild();}
        void RecordExtra(const FString& Kind,const TCHAR* Key,double Value)
        {const TArray<TSharedPtr<FJsonValue>>* Before=nullptr;const int32 Count=P()->TryGetArrayField(TEXT("route"),Before)?Before->Num():0;
         Host->RecordRouteAction(Kind);const TArray<TSharedPtr<FJsonValue>>* After=nullptr;
         if(P()->TryGetArrayField(TEXT("route"),After)&&After->Num()==Count+1){After->Last()->AsObject()->SetNumberField(Key,Value);Save(true);ShowLastRoutePoint();}Rebuild();}
        #include "ACEUCMVendorPanel.inl"
        #include "ACEUCMLootPanel.inl"
        #include "ACEVTImportPanel.inl"
        #include "ACEVTLegacyLootPanel.inl"
        void EditRoute(int I,int Direction)
        {auto Route=P()->GetArrayField(TEXT("route"));if(!Route.IsValidIndex(I))return;if(!Direction)Route.RemoveAt(I);else if(Route.IsValidIndex(I+Direction)){Route.Swap(I,I+Direction);RoutePage=(I+Direction)/RoutePageSize;}P()->SetArrayField(TEXT("route"),Route);Save(true);Rebuild();}
    };
}
TSharedRef<SWidget> UACEPluginSubsystem::MakeUCMPanel(const FString& InitialPage,bool bShowTitle){return SNew(ACEUCMPanelPrivate::SUCMPanel).Host(this).InitialPage(InitialPage).ShowTitle(bShowTitle);}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Slate/WidgetRenderer.h"
#include "Widgets/SVirtualWindow.h"
#include "Layout/WidgetPath.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "ACERuntimeOptions.h"
namespace ACEUCMPanelPrivate
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMLootEditorTest,"ACE.Plugins.LootEditor",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMLootEditorTest::RunTest(const FString&)
{
    auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT{GI->Shutdown();};
    TestTrue(TEXT("Retail fonts load for the actual loot editor"),GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call")));
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();TSharedPtr<FACEClientPlugin> Plugin;
    for(auto P:Host->Plugins)if(P->Id==TEXT("ucm"))Plugin=P;
    if(!Plugin)return false;
    const auto Original=Plugin->Profile;ON_SCOPE_EXIT{Plugin->Profile=Original;};
    auto Parse=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
    Plugin->Profile=Parse(TEXT(R"({"loot_profile":"Imported sample","loot_rules":[{"label":"Legendary armor","action":"keep","keep_up_to":4,"count_by_name":true,"conditions":[{"field":"int","key":265,"op":"ge","value":1,"missing_zero":true},{"field":"float","key":167772169,"op":"ge","value":7.25},{"field":"spells","pattern":"Legendary|Epic","exclude":"Bane","value":2}]}]})"));
    auto Panel=SNew(SUCMPanel).Host(Host).InitialPage(TEXT("Standalone loot"));
    const auto Rule=Plugin->Profile->GetArrayField(TEXT("loot_rules"))[0]->AsObject();
    const auto Conditions=Rule->GetArrayField(TEXT("conditions"));
    TestEqual(TEXT("Opening editor selects first rule"),Panel->LootEditIndex,0);
    TestTrue(TEXT("Unchanged rule can be switched"),Panel->CanLeaveLootRule());
    Panel->SelectLootRequirement(0);
    TestTrue(TEXT("Opening a requirement alone does not dirty it"),Panel->CanLeaveLootRequirement());
    Panel->SelectLootRequirement(0);
    Panel->ConditionDraft->SetNumberField(TEXT("value"),42);
    TestFalse(TEXT("Unsaved requirement prevents changing rule"),Panel->CanLeaveLootRule());
    Panel->DiscardLootRule();
    TestEqual(TEXT("Discard restores requirement value"),Panel->LootDraft->GetArrayField(TEXT("conditions"))[0]->AsObject()->GetNumberField(TEXT("value")),1.);
    Panel->LootDraft->SetStringField(TEXT("label"),TEXT("Unsaved"));
    Panel->NewLootRule();
    TestEqual(TEXT("New cannot silently replace an edited rule"),Panel->LootEditIndex,0);
    Panel->DiscardLootRule();
    Panel->SelectLootRule(0,true);
    TestEqual(TEXT("Clone is a new draft"),Panel->LootEditIndex,INDEX_NONE);
    TestEqual(TEXT("Clone has distinct name"),Panel->LootDraft->GetStringField(TEXT("label")),FString(TEXT("Legendary armor copy")));
    Panel->LootDraft->GetArrayField(TEXT("conditions"))[0]->AsObject()->SetNumberField(TEXT("value"),77);
    TestEqual(TEXT("Cloned requirements do not modify original"),Conditions[0]->AsObject()->GetNumberField(TEXT("value")),1.);
    Panel->DiscardLootRule();
    Panel->NewLootRule();
    Panel->LootDraft->SetStringField(TEXT("action"),TEXT("invalid-test-action"));
    Panel->ApplyLootRule();
    TestEqual(TEXT("Rejected new rule remains a new draft"),Panel->LootEditIndex,INDEX_NONE);
    TestTrue(TEXT("Rejected save retains editable draft"),Panel->LootDraft.IsValid());
    TestEqual(TEXT("Rejected save leaves active rules unchanged"),Panel->CurrentLootRules().Num(),1);
    Panel->DiscardLootRule();
    Host->Notice.Reset();
    TestTrue(TEXT("Classic property names include armor set ID"),ACEUCMLootPresentation::ConditionSummary(Conditions[0]->AsObject()).Contains(TEXT("ArmorSetID (265) >= 1")));
    TestTrue(TEXT("Fractional workmanship appears in summary"),ACEUCMLootPresentation::ConditionSummary(Conditions[1]->AsObject()).Contains(TEXT("7.25")));
    const FString SpellSummary=ACEUCMLootPresentation::ConditionSummary(Conditions[2]->AsObject());
    TestTrue(TEXT("Spell summary includes count and both regexes"),SpellSummary.Contains(TEXT(">= 2"))&&SpellSummary.Contains(TEXT("Legendary|Epic"))&&SpellSummary.Contains(TEXT("Exclude regex: Bane")));
    TestEqual(TEXT("Public workmanship tagged correctly"),ACEUCMLootPresentation::DataRequirement(Conditions[1]->AsObject()),FString(TEXT("Public item data")));
    TestEqual(TEXT("Appraisal property tagged correctly"),ACEUCMLootPresentation::DataRequirement(Conditions[0]->AsObject()),FString(TEXT("Appraisal may be needed")));
    TestTrue(TEXT("Unknown property ID remains visible"),ACEUCMLootPresentation::PropertyLabel(TEXT("int"),999999).Contains(TEXT("999999")));
    for(int Key:{6,7,10,17,1000,1001,1002,1003,1004,2000,2001,2003,2005,2006,2007,2008})
        TestTrue(*FString::Printf(TEXT("Every executable calculated condition has editor fields: %d"),Key),ACEUCMLootPresentation::Calculated().Contains(Key));
    Panel->LootDraft=ACEUCMLootPresentation::Clone(Rule);
    Panel->LootDraft->GetArrayField(TEXT("conditions"))[0]->AsObject()->SetNumberField(TEXT("value"),10);
    TestEqual(TEXT("Cancelling nested draft does not alter active rule"),Conditions[0]->AsObject()->GetNumberField(TEXT("value")),1.);
    Panel->LootDraft.Reset();

    auto Document=Parse(TEXT(R"({"format":"utl","rules":[],"extras":[{"name":"CustomBlock","body":"unchanged\r\n"}]})"));
    TArray<TSharedPtr<FJsonValue>> Rules;
    for(const auto& Schema:Panel->LegacySchemas())
    {
        TArray<FString> Fields;Schema.Value.ParseIntoArray(Fields,TEXT("|"));
        auto Req=MakeShared<FJsonObject>();Req->SetNumberField(TEXT("type"),Schema.Key);FString Body;
        for(int I=1;I<Fields.Num();++I)Body+=(Schema.Key==9999?FString(TEXT("true")):Fields[I].Contains(TEXT("attern"))?FString(TEXT("^Legendary|Epic")):(Schema.Key==15&&I==6)?FString(TEXT("Amuli Coat (Chest)")):FString(TEXT("1")))+TEXT("\r\n");
        Req->SetStringField(TEXT("body"),Body);
        auto R=Parse(TEXT(R"({"label":"Classic requirement","action_id":10,"amount":0,"priority":2147483647,"expression":"custom expression","requirements":[]})"));
        R->SetArrayField(TEXT("requirements"),{MakeShared<FJsonValueObject>(Req)});Rules.Add(MakeShared<FJsonValueObject>(R));
        TestTrue(TEXT("Every Classic field is visible in its summary"),Panel->LegacyRequirementSummary(Req).Contains(Fields[1]));
    }
    TestEqual(TEXT("All original Classic requirement types covered"),Rules.Num(),31);
    Document->SetArrayField(TEXT("rules"),Rules);FString Error;
    const FString Encoded=ACEVTProfile::WriteLoot(Document,Error);TestTrue(TEXT("All Classic requirement layouts save"),!Encoded.IsEmpty()&&Error.IsEmpty());
    const auto Reload=ACEVTProfile::Read(Encoded,TEXT("utl"),Error);if(!TestTrue(TEXT("Original-format document reloads"),Reload.IsValid()))return false;
    TestEqual(TEXT("Expressions remain intact"),Reload->GetArrayField(TEXT("rules"))[0]->AsObject()->GetStringField(TEXT("expression")),FString(TEXT("custom expression")));
    TestEqual(TEXT("Extra blocks remain intact"),Reload->GetArrayField(TEXT("extras"))[0]->AsObject()->GetStringField(TEXT("body")),FString(TEXT("unchanged\r\n")));
    for(int I=0;I<Rules.Num();++I)TestEqual(TEXT("Every requirement body round trips without narrowing"),Reload->GetArrayField(TEXT("rules"))[I]->AsObject()->GetArrayField(TEXT("requirements"))[0]->AsObject()->GetStringField(TEXT("body")),Rules[I]->AsObject()->GetArrayField(TEXT("requirements"))[0]->AsObject()->GetStringField(TEXT("body")));
    auto Req=Parse(TEXT(R"({"type":9,"body":"Epic\r\nBane\r\n3\r\n"})"));
    Panel->SetLegacyValue(Req,1,TEXT("Bane|Ward"));TestEqual(TEXT("Editing one field retains other fields"),Req->GetStringField(TEXT("body")),FString(TEXT("Epic\r\nBane|Ward\r\n3\r\n")));
    int ImportedFiles=0;TSharedPtr<FJsonObject> ImportedProfile;
    for(const FString& Folder:{FPaths::ProjectSavedDir()/TEXT("ClientPlugins/ImportInbox"),FString(TEXT("C:/Games/VirindiPlugins/VirindiTank"))})
    {
        TArray<FString> Files;IFileManager::Get().FindFiles(Files,*(Folder/TEXT("*.utl")),true,false);
        for(const auto& File:Files)
        {
            FString Source;FFileHelper::LoadFileToString(Source,*(Folder/File));const auto OriginalFile=ACEVTProfile::Read(Source,TEXT("utl"),Error);
            if(!TestTrue(*(TEXT("Existing UTL parses: ")+File),OriginalFile.IsValid()))continue;
            const FString SavedCopy=ACEVTProfile::WriteLoot(ACEUCMLootPresentation::Clone(OriginalFile),Error);
            const auto Reopened=ACEVTProfile::Read(SavedCopy,TEXT("utl"),Error);
            TestTrue(*(TEXT("Existing UTL survives draft / save / reload: ")+File),Reopened.IsValid()&&ACEVTProfile::WriteLoot(Reopened,Error)==SavedCopy);
            TArray<FString> Issues;auto Converted=ACEVTProfile::Convert(OriginalFile,Issues);
            if(Converted&&Issues.IsEmpty())for(const auto& Entry:Converted->GetArrayField(TEXT("loot_rules")))
                for(const auto& Condition:Entry->AsObject()->GetArrayField(TEXT("conditions")))TestFalse(TEXT("Imported condition has readable presentation"),ACEUCMLootPresentation::ConditionSummary(Condition->AsObject()).IsEmpty());
            if(Converted&&Issues.IsEmpty()&&File==TEXT("Gardener_LootSnobV4.utl")){ImportedProfile=Converted;ImportedProfile->SetStringField(TEXT("loot_profile"),File);}
            ++ImportedFiles;
        }
    }
    AddInfo(FString::Printf(TEXT("Existing local UTL files checked without modifying originals: %d"),ImportedFiles));
    if(FApp::CanEverRender())
    {
        FWidgetRenderer Renderer(false,true);
        const auto SampleProfile=Plugin->Profile;
        // A real, scrolled, wrapping list must not jump or remeasure when selecting a row.
        TArray<TSharedPtr<FJsonValue>> ManyRules;
        for(int32 I=0;I<100;++I)
        {
            auto R=ACEUCMLootPresentation::Clone(Rule);
            R->SetStringField(TEXT("label"),FString::Printf(TEXT("Imported armor rule %d with a long descriptive name"),I+1));
            ManyRules.Add(MakeShared<FJsonValueObject>(R));
        }
        Plugin->Profile=MakeShared<FJsonObject>();Plugin->Profile->SetArrayField(TEXT("loot_rules"),ManyRules);
        Panel->Page=TEXT("Loot");Panel->Rebuild();
        for(int32 Width:{780,450})
        {
            const FVector2D Size(Width,1100);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            auto Draw=[&]{for(int32 Pass=0;Pass<6;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}};
            Draw();Panel->LootRuleScroll->SetScrollOffset(600);Draw();
            const auto Scroll=Panel->LootRuleScroll;const float Offset=Scroll->GetScrollOffset();
            const auto Content=Scroll->GetChildren()->GetChildAt(0);const auto Desired=Content->GetDesiredSize();
            Panel->SelectLootRule(12);Draw();
            TestTrue(TEXT("Rule selection retains the measured list widgets"),Panel->LootRuleScroll==Scroll);
            TestEqual(TEXT("Rule selection keeps the list scroll offset"),Panel->LootRuleScroll->GetScrollOffset(),Offset);
            TestTrue(TEXT("Selecting a rule does not reflow list content"),Content->GetDesiredSize().Equals(Desired,.1f));
            TestEqual(TEXT("Selected rule still opens its editable detail"),Panel->LootEditIndex,12);
            Panel->SelectLootRequirement(0);Draw();
            TestTrue(TEXT("Opening requirement retains list and offset"),Panel->LootRuleScroll==Scroll&&Panel->LootRuleScroll->GetScrollOffset()==Offset);
            Panel->ConditionDraft.Reset();Panel->LootPage=1;Panel->Rebuild();Draw();Panel->LootRuleScroll->SetScrollOffset(500);Draw();
            const float SecondPageOffset=Panel->LootRuleScroll->GetScrollOffset();Panel->SelectLootRule(62);Draw();
            TestEqual(TEXT("Selecting on a later page keeps the page"),Panel->LootPage,1);
            TestEqual(TEXT("Selecting on a later page keeps its scroll"),Panel->LootRuleScroll->GetScrollOffset(),SecondPageOffset);
            Panel->LootSearch=TEXT("rule 6");Panel->LootPage=0;Panel->Rebuild();Draw();
            TestEqual(TEXT("Changing filter resets to its first result"),Panel->LootRuleScroll->GetScrollOffset(),0.f);
            Panel->LootSearch.Reset();Panel->LootPage=0;Panel->Rebuild();Target->ReleaseResource();
        }
        Plugin->Profile=SampleProfile;Panel->Rebuild();
        for(int Width:{780,450})for(const FString View:{TEXT("Rules"),TEXT("Condition"),TEXT("Classic"),TEXT("Imported")})
        {
            Plugin->Profile=View==TEXT("Imported")&&ImportedProfile?ImportedProfile:SampleProfile;
            Panel->Page=TEXT("Loot");Panel->LootDraft.Reset();Panel->ConditionDraft.Reset();
            if(View==TEXT("Condition")){Panel->LootDraft=ACEUCMLootPresentation::Clone(Rule);Panel->ConditionDraft=Parse(TEXT(R"({"field":"legacy","key":2008,"args":[70,1.15,1.12]})"));}
            if(View==TEXT("Classic")){Panel->Page=TEXT("Legacy loot");Panel->LegacyDocument=Document;Panel->LegacyRule=Parse(TEXT(R"({"label":"Amuli chest color","action_id":10,"amount":4,"priority":0,"expression":"","requirements":[{"type":15,"body":"255\r\n0\r\n0\r\n10\r\n0.1\r\nAmuli Coat (Chest)\r\n"}]})"));}
            Panel->Rebuild();const FVector2D Size(Width,1100);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            for(int Pass=0;Pass<6;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Panel,Size,0);FlushRenderingCommands();}
            TArray<TSharedRef<SWidget>> Tabs;int Lists=0,Details=0;TSet<const SWidget*> Seen;
            TFunction<void(TSharedRef<SWidget>)> Inspect=[&](TSharedRef<SWidget> W)
            {
                if(Seen.Contains(&W.Get()))return;Seen.Add(&W.Get());
                if(!W->GetVisibility().IsVisible())return;
                if(W->GetTag()==TEXT("UCMTab"))Tabs.Add(W);
                if(W->GetTag()==TEXT("LootRuleList"))
                {++Lists;TestTrue(TEXT("Rule list is attached with usable geometry"),W->GetCachedGeometry().GetLocalSize().X>100&&W->GetCachedGeometry().GetLocalSize().Y>150);}
                if(W->GetTag()==TEXT("LootRuleDetail"))
                {++Details;TestTrue(TEXT("Rule detail is attached with usable geometry"),W->GetCachedGeometry().GetLocalSize().X>100&&W->GetCachedGeometry().GetLocalSize().Y>150);}
                auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)Inspect(Children->GetChildAt(I));
            };Inspect(Panel);
            TestEqual(TEXT("Standalone editor has three tabs"),Tabs.Num(),3);
            if(Tabs.Num()==3)for(int32 I=1;I<3;++I)
            {
                TestTrue(TEXT("Editor tabs have uniform sizes"),Tabs[0]->GetCachedGeometry().GetLocalSize().Equals(Tabs[I]->GetCachedGeometry().GetLocalSize(),.1f));
                TestTrue(TEXT("Editor tabs stay in one row"),FMath::IsNearlyEqual(Tabs[0]->GetCachedGeometry().GetAbsolutePosition().Y,Tabs[I]->GetCachedGeometry().GetAbsolutePosition().Y,.1));
            }
            if(View!=TEXT("Classic")){TestEqual(TEXT("Loot rules are in the visible widget tree"),Lists,1);TestEqual(TEXT("Loot details are in the visible widget tree"),Details,1);}
            TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            TestEqual(TEXT("Loot editor renders at desktop and narrow widths"),Pixels.Num(),Width*1100);
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Width,1100,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("LootEditor-%s-%d.png"),*View,Width)));Target->ReleaseResource();
        }
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMImportUITest,"ACE.Plugins.ImportAndSettingsUI",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMImportUITest::RunTest(const FString&)
{
    auto* GI=NewObject<UGameInstance>();GI->Init();auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation/ImportUI")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    ON_SCOPE_EXIT{GI->Shutdown();IFileManager::Get().DeleteDirectory(*Root,false,true);};
    Host->StorageRoot=Root;Host->Settings=MakeShared<FJsonObject>();
    if(!TestTrue(TEXT("Isolated selection initialized"),Host->InitializeUCMSelection()))return false;
    auto Plugin=Host->Find(TEXT("ucm"));auto Panel=SNew(SUCMPanel).Host(Host).InitialPage(TEXT("Import"));
    const FString Inbox=Host->GetProfileFolder();IFileManager::Get().MakeDirectory(*Inbox,true);
    FFileHelper::SaveStringToFile(TEXT("uTank2 NAV 1.2\n2\n1\n0\n0\n0\n0.05\n0\n"),*(Inbox/TEXT("Walking.NAV")));
    FFileHelper::SaveStringToFile(TEXT("UTL\r\n1\r\n1\r\nCopper\r\n\r\n0;1;12\r\n8\r\n6\r\n131\r\n"),*(Inbox/TEXT("Copper.utl")));
    FFileHelper::SaveStringToFile(TEXT("broken nav"),*(Inbox/TEXT("Broken.nav")));
    for(int Run=0;Run<2;++Run)
    {
        Panel->BeginImport();TestEqual(TEXT("Import all enumerates supported types including mixed case"),Panel->ImportQueue.Num(),3);
        while(Panel->ImportQueue.Num())Panel->ProcessImport();
        TestEqual(TEXT("One invalid file does not stop remaining imports"),Panel->ImportSucceeded,2);
        TestEqual(TEXT("Bulk import preserves current route selection"),Host->UCMSectionName(TEXT("Routes")),FString(TEXT("Default")));
        TestEqual(TEXT("Repeat bulk import creates one route"),Host->UCMSectionProfiles(TEXT("Routes")).Num(),2);
        TestEqual(TEXT("Repeat bulk import creates one loot profile"),Host->UCMSectionProfiles(TEXT("Loot")).Num(),2);
    }
    for(int I=0;I<200;++I)Panel->LogImport(FString::ChrN(200,'x'));
    TestTrue(TEXT("Import log is bounded"),Panel->ImportLog.Len()<=16000);
    auto FindButton=[](const TSharedRef<SWidget>& Root,const FString& Caption)
    {
        TSharedPtr<SButton> Result;TFunction<void(const TSharedRef<SWidget>&)> Visit;
        Visit=[&](const TSharedRef<SWidget>& W){if(W->GetType()==FName(TEXT("SACEPluginButton"))&&W->GetTag()==FName(*(TEXT("UCMChoice.")+Caption)))Result=StaticCastSharedRef<SButton>(W);
            const auto Children=W->GetChildren();for(int I=0;I<Children->Num();++I)Visit(Children->GetChildAt(I));};Visit(Root);return Result;
    };
    auto Clear=FindButton(Panel,TEXT("Clear"));if(TestTrue(TEXT("Clear button available"),Clear.IsValid()))Clear->SimulateClick();
    TestTrue(TEXT("Clear removes both import output and notice"),Panel->ImportLog.IsEmpty()&&Host->Notice.IsEmpty());
    auto Rule=MakeShared<FJsonObject>();Rule->SetStringField(TEXT("state"),TEXT("Default"));
    auto Condition=MakeShared<FJsonObject>();Condition->SetNumberField(TEXT("type"),1);
    auto Action=MakeShared<FJsonObject>();Action->SetNumberField(TEXT("type"),0);
    Rule->SetObjectField(TEXT("condition"),Condition);Rule->SetObjectField(TEXT("action"),Action);
    Plugin->Profile->SetArrayField(TEXT("vt_meta"),{MakeShared<FJsonValueObject>(Rule)});
    if(!TestTrue(TEXT("Save test meta"),Host->SaveUCMSectionAs(TEXT("Metas"),TEXT("UITest"))))return false;
    Panel->Page=TEXT("Metas");Panel->Rebuild();
    auto Restart=FindButton(Panel,TEXT("Restart from Default"));
    if(TestTrue(TEXT("Imported meta has restart control"),Restart.IsValid()))Restart->SimulateClick();
    TestFalse(TEXT("Restarting a stopped meta does not unexpectedly start UCM"),Plugin->Running);
    TestTrue(TEXT("Restart keeps meta execution enabled for the next Start"),Plugin->Profile->GetBoolField(TEXT("meta_enabled")));
    TestTrue(TEXT("Restart clears prior state diagnostics"),Plugin->MetaState.IsEmpty()&&Plugin->MetaStatus.IsEmpty());
    #include "ACEUCMMonsterPanelTests.inl"
    auto FindTabs=[](const TSharedRef<SWidget>& Root,FName Tag)
    {
        TArray<TSharedRef<SACEPluginTab>> Result;TFunction<void(TSharedRef<SWidget>)> Visit;
        Visit=[&](TSharedRef<SWidget> W){if(W->GetType()==TEXT("SACEPluginTab")&&W->GetTag()==Tag)Result.Add(StaticCastSharedRef<SACEPluginTab>(W));
            const auto Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)Visit(Children->GetChildAt(I));};Visit(Root);return Result;
    };
    auto CheckTabs=[&](const TSharedRef<SWidget>& Root,FName Tag,int32 Expected)
    {
        auto Tabs=FindTabs(Root,Tag);if(!TestEqual(TEXT("Expected navigation tabs are present"),Tabs.Num(),Expected))return;
        for(int32 I=0;I<Expected;++I)
        {
            Tabs=FindTabs(Root,Tag);Tabs[I]->SimulateClick();Root->SlatePrepass();Tabs=FindTabs(Root,Tag);
            int32 Selected=0;for(const auto& Tab:Tabs)Selected+=Tab->IsSelected()?1:0;
            TestTrue(TEXT("Clicking a navigation tab selects only that tab"),Selected==1&&Tabs.IsValidIndex(I)&&Tabs[I]->IsSelected());
        }
    };
    CheckTabs(Panel,TEXT("UCMTab"),12);
    // Subpages retain their parent indicator instead of leaving every tab idle.
    Panel->Page=TEXT("Import tools");Panel->Rebuild();
    TestTrue(TEXT("Import subpage keeps Import selected"),FindTabs(Panel,TEXT("UCMTab"))[10]->IsSelected());
    Panel->Page=TEXT("Loot");Panel->Rebuild();CheckTabs(Panel,TEXT("UCMLootTab"),4);
    auto Editor=SNew(SUCMPanel).Host(Host).InitialPage(TEXT("Standalone loot"));
    CheckTabs(Editor,TEXT("UCMTab"),3);
    auto Items=Host->MakeItemToolsPanel(false);CheckTabs(Items,TEXT("ItemToolsTab"),6);
    if(FApp::CanEverRender())
    {
        TestTrue(TEXT("Retail tab artwork loads"),GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call")));
        Panel->Page=TEXT("Loot");Panel->Rebuild();Editor->Page=TEXT("Loot");Editor->Rebuild();
        FWidgetRenderer Renderer(false,true);
        for(int32 Width:{450,780})for(int32 View=0;View<3;++View)
        {
            const FVector2D Size(Width,760);TSharedRef<SWidget> Widget=View==0?StaticCastSharedRef<SWidget>(Panel):View==1?StaticCastSharedRef<SWidget>(Editor):Items;
            auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Widget,Size,.016f);FlushRenderingCommands();}
            const auto Tabs=FindTabs(Widget,View==2?TEXT("ItemToolsTab"):TEXT("UCMTab"));
            for(const auto& Tab:Tabs)
            {
                const auto& G=Tab->GetCachedGeometry();const auto At=Widget->GetCachedGeometry().AbsoluteToLocal(G.GetAbsolutePosition());
                TestTrue(TEXT("Navigation tabs fit narrow and wide plugin windows"),At.X>=0&&At.X+G.GetLocalSize().X<=Width+1&&G.GetLocalSize().Y>=24);
                TestTrue(TEXT("Native tab labels stay on a single line"),G.GetLocalSize().Y<=40);
            }
            TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> PNG;
            FImageUtils::PNGCompressImageArray(Width,760,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("PluginTabs-%s-%d.png"),View==0?TEXT("UCM"):View==1?TEXT("LootEditor"):TEXT("ItemTools"),Width)));
            Target->ReleaseResource();
        }
    }
    return true;
}
}
#endif
