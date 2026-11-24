#include "Mods/ACEActionDeckSubsystem.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginStyle.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEDatSubsystem.h"
#include "Dat/ACEDatDatabase.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerInput.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SInputKeySelector.h"
#include "Widgets/Input/SSpinBox.h"

class SACEDeckMacroEditor final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SACEDeckMacroEditor){} SLATE_ARGUMENT(UACEActionDeckSubsystem*,Deck) SLATE_ARGUMENT(int32,Index) SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {
        Deck=A._Deck;Index=A._Index;
        auto* Host=Deck->GetGameInstance()->GetSubsystem<UACEPluginSubsystem>();
        Theme=FACEPluginStyle::Use(Host); // Opaque modal, independent of the deck's pinned opacity.
        const auto& Slot=Deck->Slot(Index);const bool Editing=Slot.Action==EACEDeckAction::Macro;
        Icon=Editing?uint32(Slot.Item.Icon):0x06004CF7;
        Key=Deck->EffectiveKey(Index);Spacing=Editing?Slot.MacroDelayMs:0;
        Dat=Deck->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
        for(const auto& Item:Deck->Client->GetWorldObjects())
            if(Item.IconId&&Deck->Client->IsOwnedInventoryItem(Item))AddKnown(uint32(Item.IconId),Item.Name);
        for(int32 Spell:Deck->Client->GetKnownSpells())
        {FString SpellName;uint32 Id=0;if(Dat->TryGetSpellInfo(Spell,SpellName,Id))AddKnown(Id,SpellName);}
        Known.Sort();
        if(const auto* Portal=Dat->GetPortalDat())Candidates=Portal->GetFileIds(6,16384);

        auto Body=SNew(SVerticalBox);
        Body->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(Editing?TEXT("Edit macro"):TEXT("New macro"),18)];
        auto Identity=SNew(SHorizontalBox);
        Identity->AddSlot().FillWidth(1).Padding(0,0,12,0)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[Label(TEXT("Macro name"))]
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(Name,SACEPluginEditableTextBox).Tag(TEXT("ActionDeck.Macro.Name")).Style(&Theme->Edit)
                .Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(Editing?Slot.Item.Name:TEXT("")))
                .OnTextChanged_Lambda([this](const FText&){Error.Empty();})]];
        Identity->AddSlot().AutoWidth()[SNew(SBox).WidthOverride(180)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[Label(TEXT("Hotkey"))]
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(KeySelector,SInputKeySelector).Tag(TEXT("ActionDeck.Macro.Key"))
                .ButtonStyle(&Theme->DockButton).AllowModifierKeys(true).AllowGamepadKeys(false)
                .SelectedKey(Key).OnKeySelected_Lambda([this](FInputChord K){Key=K;Error.Empty();})]]];
        Body->AddSlot().AutoHeight()[Identity];

        auto IconHeading=SNew(SHorizontalBox);
        IconHeading->AddSlot().AutoWidth().VAlign(VAlign_Center)[Label(TEXT("Icon"))];
        IconHeading->AddSlot().AutoWidth().Padding(8,0)[SNew(SBox).WidthOverride(32).HeightOverride(32)
            [SNew(SImage).Image_Lambda([this]{FACESupplyEntry Item;Item.Icon=int32(Icon);return Theme->ItemImage(Item,true);})]];
        IconHeading->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SACEPluginTab).Text(FText::FromString(TEXT("Your items & spells")))
            .Selected_Lambda([this]{return !All;}).OnClicked_Lambda([this]{All=false;Page=0;RebuildIcons();return FReply::Handled();})];
        IconHeading->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6,0)[SNew(SACEPluginTab).Tag(TEXT("ActionDeck.Macro.AllIcons"))
            .Text(FText::FromString(TEXT("All icons"))).Selected_Lambda([this]{return All;})
            .OnClicked_Lambda([this]{All=true;Page=0;RebuildIcons();return FReply::Handled();})];
        Body->AddSlot().AutoHeight().Padding(0,8,0,4)[IconHeading];
        Body->AddSlot().AutoHeight()[SNew(SACEPluginEditableTextBox).Tag(TEXT("ActionDeck.Macro.IconSearch")).Style(&Theme->Edit)
            .Font(FACEPluginStyle::NativeFont("Regular",15)).HintText(FText::FromString(TEXT("Find inventory / known spell name or hexadecimal icon ID")))
            .OnTextChanged_Lambda([this](const FText& T){Search=T.ToString().TrimStartAndEnd();Page=0;RebuildIcons();})];
        Body->AddSlot().AutoHeight().Padding(0,4)[SNew(SBox).HeightOverride(136)[SAssignNew(Icons,SVerticalBox).Tag(TEXT("ActionDeck.Macro.IconGrid"))]];
        auto Pager=SNew(SHorizontalBox);
        Pager->AddSlot().AutoWidth()[Button(TEXT("<"),TEXT("ActionDeck.Macro.Previous"),[this]{Page=FMath::Max(0,Page-1);RebuildIcons();},[this]{return Page>0;})];
        Pager->AddSlot().FillWidth(1).VAlign(VAlign_Center)[SNew(SACEPluginText).Justification(ETextJustify::Center)
            .Font(FACEPluginStyle::NativeFont("Regular",12)).Text_Lambda([this]
            {return FText::FromString(FString::Printf(TEXT("%d / %d · %d icons%s"),Page+1,Pages(),Filtered.Num(),
                All&&CatalogAt<Candidates.Num()?TEXT(" · loading"):TEXT("")));})];
        Pager->AddSlot().AutoWidth()[Button(TEXT(">"),TEXT("ActionDeck.Macro.Next"),[this]{Page=FMath::Min(Pages()-1,Page+1);RebuildIcons();},[this]{return Page+1<Pages();})];
        Body->AddSlot().AutoHeight()[Pager];
        Body->AddSlot().AutoHeight().Padding(0,8,0,3)[Label(TEXT("Commands · one per line"))];
        Body->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(116)
            [SAssignNew(Commands,SACEPluginMultiLineEditableTextBox).Tag(TEXT("ActionDeck.Macro.Commands")).Style(&Theme->Edit)
                .Font(FACEPluginStyle::NativeFont("Regular",15)).AutoWrapText(true)
                .Text(FText::FromString(Editing?Slot.MacroText:TEXT("")))
                .OnTextChanged_Lambda([this](const FText&){Error.Empty();})
                .HintText(FText::FromString(TEXT("/hom\n/wait 30000\n/f I'm home.")))]];
        Body->AddSlot().AutoHeight().Padding(0,4)[Label(TEXT("/wait 1000 pauses for one second."),12)];
        auto Delay=SNew(SHorizontalBox);
        Delay->AddSlot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Extra delay between commands (ms)"),12)];
        Delay->AddSlot().AutoWidth()[SNew(SBox).WidthOverride(120)[SNew(SACEPluginSpinBox<int32>).Tag(TEXT("ActionDeck.Macro.Delay"))
            .Style(&Theme->Spin).MinValue(0).MaxValue(3600000).MinSliderValue(0).MaxSliderValue(30000).Delta(100)
            .Value_Lambda([this]{return Spacing;}).OnValueChanged_Lambda([this](int32 V){Spacing=V;Error.Empty();})]];
        Body->AddSlot().AutoHeight()[Delay];
        Body->AddSlot().AutoHeight().Padding(0,6)[SNew(SBox).HeightOverride(38)
            [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",12)).AutoWrapText(true)
                .ColorAndOpacity(FLinearColor(1,.8f,.45f)).Text_Lambda([this]{return FText::FromString(Error.IsEmpty()
                    ?TEXT("Run the saved macro from its slot or override hotkey. One macro can run at a time."):Error);})]];
        auto Footer=SNew(SHorizontalBox);
        Footer->AddSlot().FillWidth(1);
        Footer->AddSlot().AutoWidth().Padding(0,0,8,0)[Button(TEXT("Cancel"),TEXT("ActionDeck.Macro.Cancel"),[this]{Deck->CloseMacroEditor();})];
        Footer->AddSlot().AutoWidth()[Button(TEXT("Save macro"),TEXT("ActionDeck.Macro.Save"),[this]
        {
            if(Deck->BindMacro(Index,Name->GetText().ToString(),Icon,Commands->GetText().ToString(),Spacing,Key))Deck->CloseMacroEditor();
            else Error=Deck->Notice;
        })];
        Body->AddSlot().AutoHeight()[Footer];
        ChildSlot[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0,0,0,.65f))
            .Padding(12).HAlign(HAlign_Fill).VAlign(VAlign_Fill)
            [SNew(SScaleBox).Stretch(EStretch::ScaleToFit).StretchDirection(EStretchDirection::DownOnly)
                [SNew(SBox).WidthOverride(660)
                    [SNew(SBorder).BorderImage(&Theme->Frame).Padding(1)
                        [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                            .BorderBackgroundColor(FLinearColor(.025f,.02f,.012f,1)).Padding(14)[Body]]]]]];
        RebuildIcons();
    }
    virtual bool SupportsKeyboardFocus() const override {return true;}
    virtual FNavigationReply OnNavigation(const FGeometry&,const FNavigationEvent&) override {return FNavigationReply::Wrap();}
    virtual FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&) override {return FReply::Handled();}
    virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent&) override {return FReply::Handled();}
    virtual FReply OnMouseWheel(const FGeometry&,const FPointerEvent&) override {return FReply::Handled();}
    virtual FReply OnKeyDown(const FGeometry&,const FKeyEvent&) override {return FReply::Handled();}
    virtual FReply OnPreviewKeyDown(const FGeometry&,const FKeyEvent& E) override
    {
        if(E.GetKey()==EKeys::Escape&&!KeySelector->GetIsSelectingKey()){Deck->CloseMacroEditor();return FReply::Handled();}
        return FReply::Unhandled();
    }
    virtual void Tick(const FGeometry& G,double T,float D) override
    {
        SCompoundWidget::Tick(G,T,D);
        // Read only tiny image records, with a small per-frame budget. Opening
        // the editor never decodes thousands of textures on the game thread.
        if(CatalogAt>=Candidates.Num()||!Dat.IsValid())return;
        const auto* Portal=Dat->GetPortalDat();if(!Portal)return;
        const double End=FPlatformTime::Seconds()+.001;
        const int32 PreviousCount=Filtered.Num();
        int32 Read=0;
        do
        {
            const uint32 Id=Candidates[CatalogAt++];TArray<uint8> Bytes;
            if(Portal->ReadFile(Id,Bytes))
            {
                FACEDatCursor Cursor(Bytes);bool Ok=true;
                const uint32 FileId=Cursor.ReadU32(Ok);Cursor.ReadU32(Ok);
                const int32 W=Cursor.ReadI32(Ok),H=Cursor.ReadI32(Ok);
                if(Ok&&FileId==Id&&W==32&&H==32)
                {Catalog.Add(Id);if(All&&MatchesSearch(Id))Filtered.Add(Id);}
            }
        }while(CatalogAt<Candidates.Num()&&++Read<64&&FPlatformTime::Seconds()<End);
        // Existing pages stay intact as the catalog grows; do not recreate
        // focused buttons or re-filter the entire catalog every frame.
        if(All&&FMath::Clamp(PreviousCount-Page*PageSize,0,PageSize)!=FMath::Clamp(Filtered.Num()-Page*PageSize,0,PageSize))
        {
            BuildIconPage();
            // Tick runs after the normal prepass. Size newly appended buttons
            // before painting this frame, including on slow DAT storage.
            Icons->SlatePrepass(G.GetAccumulatedLayoutTransform().GetScale());
        }
    }
private:
    TWeakObjectPtr<UACEActionDeckSubsystem> Deck;
    TWeakObjectPtr<UACEDatSubsystem> Dat;
    TSharedPtr<FACEPluginStyle> Theme;
    TSharedPtr<SEditableTextBox> Name;
    TSharedPtr<SMultiLineEditableTextBox> Commands;
    TSharedPtr<SInputKeySelector> KeySelector;
    TSharedPtr<SVerticalBox> Icons;
    TArray<TSharedPtr<SHorizontalBox>> IconRows;
    int32 DrawnIcons=0;
    TArray<uint32> Known,Candidates,Catalog,Filtered;
    TMap<uint32,FString> Names;
    int32 Index=0,Spacing=0,Page=0,CatalogAt=0;
    uint32 Icon=0;
    FInputChord Key;
    FString Search,Error;
    bool All=false;
    static constexpr int32 PageSize=42;
    bool MatchesSearch(uint32 Id) const
    {return Search.IsEmpty()||FString::Printf(TEXT("%08X"),Id).Contains(Search)||Names.FindRef(Id).Contains(Search);}
    int32 Pages() const {return FMath::Max(1,FMath::DivideAndRoundUp(Filtered.Num(),PageSize));}
    void AddKnown(uint32 Id,const FString& Title)
    {if((Id>>24)!=6)return;Known.AddUnique(Id);auto& Text=Names.FindOrAdd(Id);if(!Text.IsEmpty())Text+=TEXT(", ");Text+=Title;}
    TSharedRef<SWidget> Label(const FString& Text,int32 Size=15)
    {return SNew(SACEPluginText).Text(FText::FromString(Text)).Font(FACEPluginStyle::NativeFont("Regular",Size)).AutoWrapText(true);}
    TSharedRef<SWidget> Button(const TCHAR* Title,const TCHAR* WidgetTag,TFunction<void()> Action,TFunction<bool()> Enabled=[] {return true;})
    {
        return SNew(SACEPluginButton).Tag(FName(WidgetTag)).ButtonStyle(&Theme->DockButton).ContentPadding(FMargin(8,5))
            .IsEnabled_Lambda([Enabled]{return Enabled();})
            .OnClicked_Lambda([Action]{Action();return FReply::Handled();})[Label(Title)];
    }
    void RebuildIcons()
    {
        if(!Icons)return;
        Filtered.Reset();
        for(uint32 Id:All?Catalog:Known)
            if(MatchesSearch(Id))Filtered.Add(Id);
        Icons->ClearChildren();IconRows.Reset();DrawnIcons=0;
        BuildIconPage();
    }
    void BuildIconPage()
    {
        Page=FMath::Clamp(Page,0,Pages()-1);
        if(Filtered.IsEmpty())
        {Icons->AddSlot().FillHeight(1).VAlign(VAlign_Center)[Label(All&&CatalogAt<Candidates.Num()?TEXT("Loading icons…"):TEXT("No matching icons. Try All icons or clear the search."),12)];return;}
        if(IconRows.IsEmpty())
        {
            Icons->ClearChildren();
            for(int32 Row=0;Row<3;++Row)
            {
                auto Line=SNew(SHorizontalBox);IconRows.Add(Line);
                for(int32 Column=0;Column<14;++Column)Line->AddSlot().FillWidth(1).Padding(1);
                Icons->AddSlot().AutoHeight()[Line];
            }
        }
        const int32 Count=FMath::Clamp(Filtered.Num()-Page*PageSize,0,PageSize);
        for(;DrawnIcons<Count;++DrawnIcons)
        {
            const uint32 Id=Filtered[Page*PageSize+DrawnIcons];FACESupplyEntry Item;Item.Icon=int32(Id);
            IconRows[DrawnIcons/14]->GetSlot(DrawnIcons%14).AttachWidget(
                SNew(SACEPluginTab).Tag(FName(*FString::Printf(TEXT("ActionDeck.Macro.Icon.%08X"),Id)))
                .ContentPadding(FMargin(3)).Selected_Lambda([this,Id]{return Icon==Id;})
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Names.FindRef(Id)+FString::Printf(TEXT("  %08X"),Id))))
                .OnClicked_Lambda([this,Id]{Icon=Id;return FReply::Handled();})
                [SNew(SBox).WidthOverride(32).HeightOverride(32)[SNew(SImage).Image(Theme->ItemImage(Item,true))]]);
        }
    }
};

void UACEActionDeckSubsystem::OpenMacroEditor(int32 Index)
{
    EnsureCharacter();
    if(IsMacroEditorOpen()||!Slots.IsValidIndex(Index)||!Client||Client->GetSessionState()!=EACESessionState::InWorld||!IsEnabled())return;
    auto* Viewport=GetGameInstance()->GetGameViewportClient();if(!Viewport)return;
    CapturingKey=false;
    MacroEditor=SNew(SACEDeckMacroEditor).Tag(TEXT("ActionDeck.Macro.Editor")).Deck(this).Index(Index);
    MacroEditorViewport=Viewport;Viewport->AddViewportWidgetContent(MacroEditor.ToSharedRef(),2000);
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
    {
        if(PC->PlayerInput)PC->PlayerInput->FlushPressedKeys();
        FInputModeUIOnly Mode;Mode.SetWidgetToFocus(MacroEditor);PC->SetInputMode(Mode);
    }
    FSlateApplication::Get().ReleaseAllPointerCapture();
    FSlateApplication::Get().SetKeyboardFocus(MacroEditor);
}

void UACEActionDeckSubsystem::CloseMacroEditor()
{
    if(!MacroEditor)return;
    if(auto* Viewport=MacroEditorViewport.Get())Viewport->RemoveViewportWidgetContent(MacroEditor.ToSharedRef());
    MacroEditor.Reset();MacroEditorViewport.Reset();CapturingKey=false;
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))PC->RefreshDesktopDisplayInputMode();
    if(Client&&Client->GetSessionState()==EACESessionState::InWorld&&FSlateApplication::IsInitialized())
        FSlateApplication::Get().SetAllUserFocusToGameViewport();
}
