#include "Mods/ACEActionDeckSubsystem.h"
#include "Mods/ACEActionDeckLayout.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginIcon.h"
#include "ACEClientSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Fonts/FontMeasure.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SInputKeySelector.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    TSharedRef<SWidget> DeckLabel(const FString& Text,int32 Size=12)
    {return SNew(SACEPluginText).Text(FText::FromString(Text)).Font(FACEPluginStyle::NativeFont("Regular",Size));}

    void PaintIcon(const FACEDeckSlot& Slot,FACEPluginStyle& Theme,const FGeometry& G,
        FSlateWindowElementList& Out,int32 Layer,bool Ready)
    {
        const auto Effect=Ready?ESlateDrawEffect::None:ESlateDrawEffect::DisabledEffect;
        const FLinearColor Tint=Ready?FLinearColor::White:FLinearColor(.36f,.36f,.36f,1);
        if(Slot.Action==EACEDeckAction::Item||Slot.Action==EACEDeckAction::Self||Slot.Action==EACEDeckAction::Macro)
        {
            FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(),Theme.ItemImage(Slot.Item,false),Effect,Tint);
            FSlateDrawElement::MakeBox(Out,Layer+1,G.ToPaintGeometry(),Theme.ItemImage(Slot.Item,true),Effect,Tint);
        }
        else if(Slot.Action==EACEDeckAction::Spell)
            // Use the same power background, effect mask and badge as inventory.
            FSlateDrawElement::MakeBox(Out,Layer+1,G.ToPaintGeometry(),Theme.SpellImage(Slot.Spell),Effect,Tint);
        else if(Slot.Action==EACEDeckAction::Melee||Slot.Action==EACEDeckAction::Missile)
        {
            const double Y=Slot.Height==3?6:Slot.Height==2?16:26;
            const TArray<FVector2D> Points={{5,Y},{26,Y},{21,Y-4},{26,Y},{21,Y+4}};
            FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Points,Effect,FLinearColor(.9f,.72f,.3f)*Tint,true,2);
            FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(),
                Slot.Action==EACEDeckAction::Melee?TEXT("M"):TEXT("R"),FACEPluginStyle::NativeFont("Bold",12),Effect,Tint);
        }
    }

    // Lives above the plugin window layer, so a shortcut remains visible while
    // being dragged over another slot, the window frame, or out into the world.
    class SDeckDragPreview final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SDeckDragPreview){} SLATE_ARGUMENT(TSharedPtr<SWidget>,Source)
            SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(TSharedPtr<FACEPluginStyle>,Theme)
            SLATE_ARGUMENT(FACEDeckSlot,Slot)
        SLATE_END_ARGS()
        void Construct(const FArguments& A)
        {
            Source=A._Source;Host=A._Host;Theme=A._Theme;Slot=A._Slot;
            SetVisibility(EVisibility::HitTestInvisible);SetCanTick(false);
        }
        void Move(FVector2D InPosition,FVector2D InSize)
        {Position=InPosition;Size=InSize;Invalidate(EInvalidateWidgetReason::Paint);}
        virtual FVector2D ComputeDesiredSize(float) const override {return FVector2D::ZeroVector;}
        virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,
            int32 Layer,const FWidgetStyle&,bool) const override
        {
            const auto Widget=Source.Pin();
            if(!Widget||!Widget->HasMouseCapture()||!Host.IsValid()||!Host->IsPluginWindowOpen(TEXT("actiondeck")))return Layer;
            const auto TopLeft=G.AbsoluteToLocal(Position-Size*.5);
            const auto BottomRight=G.AbsoluteToLocal(Position+Size*.5);
            PaintIcon(Slot,*Theme,G.MakeChild(BottomRight-TopLeft,FSlateLayoutTransform(TopLeft)),Out,Layer,true);
            return Layer+1;
        }
    private:
        TWeakPtr<SWidget> Source;
        TWeakObjectPtr<UACEPluginSubsystem> Host;
        TSharedPtr<FACEPluginStyle> Theme;
        FACEDeckSlot Slot;
        FVector2D Position=FVector2D::ZeroVector,Size=FVector2D::ZeroVector;
    };

    class SDeckGrid final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SDeckGrid){} SLATE_ARGUMENT(UACEActionDeckSubsystem*,Deck)
            SLATE_ARGUMENT(TFunction<void(int32)>,Configure) SLATE_ATTRIBUTE(int32,Selected)
        SLATE_END_ARGS()
        void Construct(const FArguments& A)
        {
            Deck=A._Deck;Configure=A._Configure;Selected=A._Selected;SetCanTick(false);
            Host=Deck->GetGameInstance()->GetSubsystem<UACEPluginSubsystem>();
            Theme=FACEPluginStyle::Use(Host.Get(),TEXT("actiondeck"));
            SetToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([this]
                {return FText::FromString(Deck.IsValid()?Deck->SlotHint(Hover):FString());})));
        }
        virtual ~SDeckGrid() override {EndDrag();}
        void RefreshLayout(){Hover=INDEX_NONE;EndDrag();Invalidate(EInvalidateWidgetReason::Layout);}
        virtual FVector2D ComputeDesiredSize(float) const override
        {return FVector2D(42*UACEActionDeckSubsystem::Columns,42*(Deck.IsValid()?Deck->VisibleRows():UACEActionDeckSubsystem::DefaultRows));}
        int32 SlotAt(FVector2D Absolute) const
        {
            const auto& G=GetCachedGeometry();const auto P=G.AbsoluteToLocal(Absolute),Size=G.GetLocalSize();
            if(!Deck.IsValid()||Size.X<=0||Size.Y<=0||P.X<0||P.Y<0||P.X>=Size.X||P.Y>=Size.Y)return INDEX_NONE;
            return Deck->RowAtDisplay(int32(P.Y/(Size.Y/Deck->VisibleRows())))*UACEActionDeckSubsystem::Columns
                +int32(P.X/(Size.X/UACEActionDeckSubsystem::Columns));
        }
        int32 DropSlotAt(FVector2D Absolute) const
        {
            auto& Slate=FSlateApplication::Get();
            if(Slate.AnyMenusVisible()||Slate.GetActiveModalWindow())return INDEX_NONE;
            const auto Window=Slate.FindWidgetWindow(SharedThis(this));
            if(!Window||!Slate.LocateWindowUnderMouse(Absolute,{Window.ToSharedRef()},false,0).ContainsWidget(this))
                return INDEX_NONE; // Do not rearrange through another window.
            return SlotAt(Absolute);
        }
        virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,
            int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
        {
            if(!Deck.IsValid())return Layer;
            const FVector2D Cell(G.GetLocalSize().X/UACEActionDeckSubsystem::Columns,G.GetLocalSize().Y/Deck->VisibleRows());
            static const FSlateRoundedBoxBrush Border(FLinearColor(.016f,.018f,.016f,.95f),3.f,FLinearColor(.48f,.38f,.18f),1.f);
            static const FSlateRoundedBoxBrush Highlight(FLinearColor(.08f,.12f,.11f,.98f),3.f,FLinearColor(1,.78f,.28f),2.f);
            static const FSlateColorBrush KeyBack(FLinearColor(0,0,0,.85f));
            for(int32 I=0;I<Deck->VisibleSlots();++I)
            {
                const auto& S=Deck->Slot(I);const auto& A=Deck->Availability(I);
                const auto CG=G.MakeChild(Cell-FVector2D(2,2),FSlateLayoutTransform(FVector2D(I%UACEActionDeckSubsystem::Columns*Cell.X+1,Deck->DisplayRow(I)*Cell.Y+1)));
                const bool Marked=Selected.Get(INDEX_NONE)==I||Hover==I;
                const FSlateBrush* CellBrush=Marked?&Highlight:&Border;
                FSlateDrawElement::MakeBox(Out,Layer,CG.ToPaintGeometry(),CellBrush,ESlateDrawEffect::None,CellBrush->GetTint(Style));
                const auto IconG=CG.MakeChild(CG.GetLocalSize()-FVector2D(6,6),FSlateLayoutTransform(FVector2D(3,3)));
                if(!Dragging||I!=From)PaintIcon(S,*Theme,IconG,Out,Layer+1,A.Ready);
                const auto Timer=Deck->EffectTimer(I);
                if(Timer.Remaining>0&&(!Dragging||I!=From))
                {
                    // Remaining effect time is information, not a casting lockout.
                    // Shade the full button, draining from top to bottom.
                    const float Height=CG.GetLocalSize().Y*Timer.Fraction();
                    const auto Progress=CG.MakeChild(FVector2D(CG.GetLocalSize().X,Height),
                        FSlateLayoutTransform(FVector2D(0,CG.GetLocalSize().Y-Height)));
                    FSlateDrawElement::MakeBox(Out,Layer+3,Progress.ToPaintGeometry(),
                        FCoreStyle::Get().GetBrush("WhiteBrush"),ESlateDrawEffect::None,FLinearColor(.12f,.2f,.3f,.55f));
                    const auto TimerFont=FACEPluginStyle::NativeFont("Bold",12);
                    const FString Label=Timer.Label();
                    const FVector2D Size=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Label,TimerFont);
                    const float Scale=FMath::Min(1.f,float((CG.GetLocalSize().X-4)/FMath::Max(1.,Size.X)));
                    const FVector2D At((CG.GetLocalSize().X-Size.X*Scale)*.5,FMath::Max(13.,(CG.GetLocalSize().Y-Size.Y*Scale)*.5));
                    FSlateDrawElement::MakeText(Out,Layer+4,CG.MakeChild(Size,FSlateLayoutTransform(Scale,At+FVector2D(1,1))).ToPaintGeometry(),
                        Label,TimerFont,ESlateDrawEffect::None,FLinearColor::Black);
                    FSlateDrawElement::MakeText(Out,Layer+5,CG.MakeChild(Size,FSlateLayoutTransform(Scale,At)).ToPaintGeometry(),
                        Label,TimerFont,ESlateDrawEffect::None,FLinearColor::White);
                }
                // High-contrast key labels are independent of the disabled item art.
                const FString Key=Deck->KeyLabel(I);
                const auto Font=FACEPluginStyle::NativeFont("Bold",9);
                const FVector2D TextSize=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Key,Font);
                const float Scale=FMath::Min(1.f,float((CG.GetLocalSize().X-4)/FMath::Max(1.,TextSize.X)));
                const float Width=float(TextSize.X*Scale+4);
                const auto KG=CG.MakeChild(FVector2D(Width,13),FSlateLayoutTransform(FVector2D(CG.GetLocalSize().X-Width,0)));
                FSlateDrawElement::MakeBox(Out,Layer+3,KG.ToPaintGeometry(),&KeyBack,ESlateDrawEffect::None,KeyBack.GetTint(Style));
                const auto TG=KG.MakeChild(TextSize,FSlateLayoutTransform(Scale,FVector2D(2,0)));
                FSlateDrawElement::MakeText(Out,Layer+4,TG.ToPaintGeometry(),Key,Font,
                    ESlateDrawEffect::None,FLinearColor(1,.94f,.72f));
                if(S.Action==EACEDeckAction::Item&&(!Dragging||I!=From))
                {
                    const auto CountG=CG.MakeChild(FVector2D(37,12),FSlateLayoutTransform(FVector2D(3,CG.GetLocalSize().Y-13)));
                    FSlateDrawElement::MakeText(Out,Layer+4,CountG.ToPaintGeometry(),FString::Printf(TEXT("%lld"),A.Count),
                        FACEPluginStyle::NativeFont("Bold",9),ESlateDrawEffect::None,A.Count?FLinearColor::White:FLinearColor(.65f,.65f,.65f));
                }
            }
            return Layer+5;
        }
        virtual FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent& E) override
        {
            if(!Deck.IsValid())return FReply::Unhandled();
            const int32 Index=SlotAt(E.GetScreenSpacePosition());if(Index==INDEX_NONE)return FReply::Unhandled();
            if(E.GetEffectingButton()==EKeys::RightMouseButton){Configure(Index);return FReply::Handled();}
            if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
            EndDrag();From=Index;Start=E.GetScreenSpacePosition();
            return FReply::Handled().CaptureMouse(SharedThis(this));
        }
        virtual FReply OnMouseMove(const FGeometry& G,const FPointerEvent& E) override
        {
            Hover=SlotAt(E.GetScreenSpacePosition());
            if(HasMouseCapture()&&From!=INDEX_NONE)
            {
                Moved|=FVector2D::Distance(Start,E.GetScreenSpacePosition())>8;
                if(!Deck.IsValid()||!Deck->IsEnabled()||!Host.IsValid()||!Host->IsPluginWindowOpen(TEXT("actiondeck")))
                {EndDrag();return FReply::Handled().ReleaseMouseCapture();}
                if(Moved&&!Dragging&&Deck->Slot(From).Action!=EACEDeckAction::Empty)
                {
                    if(auto* Viewport=Deck->GetGameInstance()->GetGameViewportClient())
                    {
                        DragViewport=Viewport;
                        DragPreview=SNew(SDeckDragPreview).Tag(TEXT("ActionDeck.DragPreview"))
                            .Source(SharedThis(this)).Host(Host.Get()).Theme(Theme).Slot(Deck->Slot(From));
                        Viewport->AddViewportWidgetContent(DragPreview.ToSharedRef(),1600);
                        Dragging=true;
                    }
                }
                if(DragPreview)
                {
                    const FVector2D IconSize(G.GetLocalSize().X/UACEActionDeckSubsystem::Columns-8,G.GetLocalSize().Y/Deck->VisibleRows()-8);
                    DragPreview->Move(E.GetScreenSpacePosition(),G.LocalToAbsolute(IconSize)-G.LocalToAbsolute(FVector2D::ZeroVector));
                }
            }
            Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();
        }
        virtual void OnMouseLeave(const FPointerEvent& E) override {Hover=INDEX_NONE;SLeafWidget::OnMouseLeave(E);}
        virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E) override
        {
            if(!HasMouseCapture()||E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
            const int32 To=DropSlotAt(E.GetScreenSpacePosition());
            if(Deck.IsValid()&&Deck->IsEnabled()&&Host.IsValid()&&Host->IsPluginWindowOpen(TEXT("actiondeck")))
            {
                if(Dragging)
                {
                    if(To!=INDEX_NONE)Deck->SwapSlots(From,To);
                    else if(!Host->IsPointInsidePluginWindow(TEXT("actiondeck"),E.GetScreenSpacePosition()))
                        Deck->ClearSlot(From);
                }
                else if(!Moved&&To==From&&To!=INDEX_NONE)
                {
                    if(Deck->IsConfigOpen())Configure(To);
                    else Deck->Activate(To);
                }
            }
            EndDrag();return FReply::Handled().ReleaseMouseCapture();
        }
        virtual void OnMouseCaptureLost(const FCaptureLostEvent& E) override
        {EndDrag();SLeafWidget::OnMouseCaptureLost(E);}
    private:
        TWeakObjectPtr<UACEActionDeckSubsystem> Deck;
        TWeakObjectPtr<UACEPluginSubsystem> Host;
        TSharedPtr<FACEPluginStyle> Theme;
        TSharedPtr<SDeckDragPreview> DragPreview;
        TWeakObjectPtr<UGameViewportClient> DragViewport;
        TFunction<void(int32)> Configure;
        TAttribute<int32> Selected;
        int32 From=INDEX_NONE,Hover=INDEX_NONE;
        FVector2D Start;
        bool Dragging=false,Moved=false;
        void EndDrag()
        {
            if(DragPreview&&DragViewport.IsValid())DragViewport->RemoveViewportWidgetContent(DragPreview.ToSharedRef());
            DragPreview.Reset();DragViewport.Reset();From=INDEX_NONE;Dragging=Moved=false;
            Invalidate(EInvalidateWidgetReason::Paint);
        }
    };
}

class SACEActionDeck final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SACEActionDeck){} SLATE_ARGUMENT(UACEActionDeckSubsystem*,Deck) SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {
        Deck=A._Deck;Host=Deck->GetGameInstance()->GetSubsystem<UACEPluginSubsystem>();
        Theme=FACEPluginStyle::Use(Host.Get(),TEXT("actiondeck"));
        Deck->Refresh(true);
        for(int32 Bits=0;Bits<16;++Bits)
        {
            FString Name;
            if(Bits&2)Name+=TEXT("Ctrl+");if(Bits&1)Name+=TEXT("Shift+");
            if(Bits&4)Name+=TEXT("Alt+");if(Bits&8)Name+=TEXT("Cmd+");
            if(Name.IsEmpty())Name=TEXT("None");else Name.LeftChopInline(1);
            Modifiers.Add(MakeShared<FString>(Name));
        }
        auto Toolbar=SNew(SHorizontalBox);
        Toolbar->AddSlot().AutoWidth()[SNew(SACEPluginTab).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).Tag(TEXT("ActionDeck.Override")).FontSize(12)
            .Selected_Lambda([this]{return Deck->Override;})
            .Text_Lambda([this]{return FText::FromString(Deck->Override?TEXT("Override: On"):TEXT("Override: Off"));})
            .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Assigned slots keep their hotkeys while the deck is open, even when depleted. Empty slots use native hotbars. Typing takes priority."))))
            .OnClicked_Lambda([this]{Deck->Override=!Deck->Override;Deck->Save();return FReply::Handled();})];
        Toolbar->AddSlot().AutoWidth().Padding(4,0)[SNew(SACEPluginTab).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).Tag(TEXT("ActionDeck.Lock")).FontSize(12)
            .Selected_Lambda([this]{return Host->IsPluginWindowPinned(TEXT("actiondeck"));})
            .Text_Lambda([this]{return FText::FromString(Host->IsPluginWindowPinned(TEXT("actiondeck"))?TEXT("Locked"):TEXT("Lock"));})
            .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Lock this window's position and hide its frame. Slots remain usable."))))
            .OnClicked_Lambda([this]{Host->SetPluginWindowPinned(TEXT("actiondeck"),!Host->IsPluginWindowPinned(TEXT("actiondeck")));return FReply::Handled();})];
        Toolbar->AddSlot().FillWidth(1);
        Toolbar->AddSlot().AutoWidth()[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::IconButtonPadding)).Tag(TEXT("ActionDeck.Settings")).ButtonStyle(&Theme->DockButton).IsFocusable(false)
            .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Configure hotbars, target timers, keys and attack shortcuts"))))
            .OnClicked_Lambda([this]
            {
                Deck->ConfigOpen=!Deck->ConfigOpen;Deck->CapturingKey=false;
                if(Deck->ConfigOpen)BuildSettings();
                else
                {
                    // The key selector retains focus after accepting a chord.
                    // Closing settings must immediately restore gameplay input.
                    auto& Slate=FSlateApplication::Get();
                    Slate.ClearKeyboardFocus();Slate.SetAllUserFocusToGameViewport();
                }
                return FReply::Handled();
            })
            [SNew(SBox).WidthOverride(22).HeightOverride(22)[SNew(SACEPluginIcon).Id(TEXT("plugin.options"))]]];
        ChildSlot[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Tag(TEXT("ActionDeck.Toolbar")).HeightOverride(ACEActionDeckLayout::ToolbarHeight)
                .Visibility_Lambda([this]{return Host->IsPluginWindowChromeHidden(TEXT("actiondeck"))?EVisibility::Collapsed:EVisibility::Visible;})[Toolbar]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,3)
                [SNew(SBox).HeightOverride_Lambda([this]{return FOptionalSize(FMath::Max(1.,GetCachedGeometry().GetLocalSize().X)*Deck->VisibleRows()/UACEActionDeckSubsystem::Columns);})
                [SNew(SScaleBox).Stretch(EStretch::ScaleToFit)
                [SAssignNew(Grid,SDeckGrid).Tag(TEXT("ActionDeck.Grid")).Deck(Deck.Get())
                    .Configure([this](int32 I)
                    {
                        Selected=I;
                        // Editing a pinned slot restores the frame and gear so
                        // settings can be hidden again without holding Quick-show.
                        if(Host->IsPluginWindowPinned(TEXT("actiondeck")))Host->SetPluginWindowPinned(TEXT("actiondeck"),false);
                        Deck->ConfigOpen=true;BuildSettings();
                    })
                    .Selected_Lambda([this]{return Deck->ConfigOpen?Selected:INDEX_NONE;})]]]
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Tag(TEXT("ActionDeck.Status"))
                .HeightOverride(ACEActionDeckLayout::StatusHeight)
                .Visibility_Lambda([this]{return Deck->IsMacroRunning()?EVisibility::Visible:EVisibility::Collapsed;})
                [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                [SNew(STextBlock).Font(FACEPluginStyle::NativeFont("Regular",10)).ColorAndOpacity(FLinearColor(.9f,.85f,.65f))
                    .Text_Lambda([this]{return FText::FromString(Deck->MacroStatus());})
                    .ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([this]{return FText::FromString(Deck->MacroStatus());})))
                    .Clipping(EWidgetClipping::ClipToBounds)]
                +SHorizontalBox::Slot().AutoWidth().Padding(4,0)[SNew(SACEPluginButton).Tag(TEXT("ActionDeck.StopMacro"))
                    .ButtonStyle(&Theme->DockButton).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding))
                    .OnClicked_Lambda([this]{Deck->StopMacro();Deck->Notice=TEXT("Macro stopped.");return FReply::Handled();})[DeckLabel(TEXT("Stop macro"))]]]]
            +SVerticalBox::Slot().FillHeight(1)
                [SAssignNew(Settings,SVerticalBox).Visibility_Lambda([this]{return Deck->ConfigOpen?EVisibility::Visible:EVisibility::Collapsed;})]];
        BuildSettings();SetClipping(EWidgetClipping::ClipToBounds);
        SetEnabled(TAttribute<bool>::CreateLambda([this]
            {return Deck.IsValid()&&Deck->Client&&Deck->Client->GetSessionState()==EACESessionState::InWorld;}));
    }
    virtual void Tick(const FGeometry& G,double T,float D) override
    {
        SCompoundWidget::Tick(G,T,D);
        if(Deck.IsValid())
        {
            Deck->Refresh();
            if(LastVisibleRows!=Deck->VisibleRows())
            {LastVisibleRows=Deck->VisibleRows();Grid->RefreshLayout();Invalidate(EInvalidateWidgetReason::Layout);}
            Grid->Invalidate(EInvalidateWidgetReason::Paint);
        }
    }
    int32 SlotAt(FVector2D Absolute) const
    {
        if(!Grid||!FSlateApplication::IsInitialized())return INDEX_NONE;
        return Grid->DropSlotAt(Absolute);
    }
private:
    TWeakObjectPtr<UACEActionDeckSubsystem> Deck;
    TWeakObjectPtr<UACEPluginSubsystem> Host;
    TSharedPtr<FACEPluginStyle> Theme;
    TSharedPtr<SDeckGrid> Grid;
    TSharedPtr<SVerticalBox> Settings;
    TSharedPtr<SInputKeySelector> KeySelector;
    TArray<TSharedPtr<FString>> Modifiers;
    int32 Selected=24;
    int32 LastVisibleRows=0;
    TSharedRef<SWidget> Button(const FString& Title,TFunction<void()> Action)
    {
        return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).ButtonStyle(&Theme->DockButton).IsFocusable(false)
            .OnClicked_Lambda([Action]{Action();return FReply::Handled();})[DeckLabel(Title)];
    }
    void BuildSettings()
    {
        if(!Settings)return;
        Deck->CapturingKey=false;
        Settings->ClearChildren();
        if(Deck->DisplayRow(Selected)==INDEX_NONE)Selected=24;
        auto Body=SNew(SVerticalBox);
        Body->AddSlot().AutoHeight().Padding(0,3)[SNew(SCheckBox).Tag(TEXT("ActionDeck.FourthRow")).Style(&Theme->Check)
            .IsChecked_Lambda([this]{return Deck->IsFourthRowEnabled()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([this](ECheckBoxState Value)
            {
                if(Deck->SetFourthRowEnabled(Value==ECheckBoxState::Checked))
                {Grid->RefreshLayout();BuildSettings();}
            })[DeckLabel(TEXT("Show fourth row (Alt by default)"))]];
        Body->AddSlot().AutoHeight()[SNew(SACEPluginText).Tag(TEXT("ActionDeck.SettingsNotice"))
            .Font(FACEPluginStyle::NativeFont("Regular",11)).AutoWrapText(true)
            .ColorAndOpacity(FLinearColor(1,.85f,.5f))
            .Text_Lambda([this]{return FText::FromString(Deck->Notice);})
            .Visibility_Lambda([this]{return Deck->Notice.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})];
        auto Option=[&](const TCHAR* WidgetTag,const TCHAR* Label,bool UACEActionDeckSubsystem::* Field)
        {
            Body->AddSlot().AutoHeight().Padding(0,3)[SNew(SCheckBox).Tag(FName(WidgetTag)).Style(&Theme->Check)
                .IsChecked_Lambda([this,Field]{return Deck.Get()->*Field?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this,Field](ECheckBoxState Value){Deck.Get()->*Field=Value==ECheckBoxState::Checked;Deck->Save();})
                [DeckLabel(Label)]];
        };
        Option(TEXT("ActionDeck.HideSpellBar"),TEXT("Hide native spell bar"),&UACEActionDeckSubsystem::HideSpellBar);
        Option(TEXT("ActionDeck.HideCombatBar"),TEXT("Hide native melee / missile bar"),&UACEActionDeckSubsystem::HideCombatBar);
        Body->AddSlot().AutoHeight()[DeckLabel(TEXT("Native bars return when Action Deck is closed or disabled."),11)];
        Option(TEXT("ActionDeck.TargetTimers"),TEXT("Show target debuff and damage-over-time timers"),&UACEActionDeckSubsystem::ShowTargetTimers);
        Body->AddSlot().AutoHeight()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",11)).AutoWrapText(true)
            .Text(FText::FromString(TEXT("Timers estimate your own confirmed casts. Early dispels can make them inaccurate.")))];
        Body->AddSlot().AutoHeight().Padding(0,3)[SNew(SBox)
            .Visibility_Lambda([this]{return Host->IsPluginWindowPinned(TEXT("actiondeck"))?EVisibility::Visible:EVisibility::Collapsed;})
            [Button(TEXT("Unlock window"),[this]{Host->SetPluginWindowPinned(TEXT("actiondeck"),false);})]];
        Body->AddSlot().AutoHeight().Padding(0,3)[DeckLabel(TEXT("Row modifiers · custom slot keys keep their own modifiers"))];
        auto Rows=SNew(SHorizontalBox);
        for(int32 Display=0;Display<UACEActionDeckSubsystem::Rows;++Display)
        {
            const int32 R=Display<Deck->VisibleRows()?Deck->RowAtDisplay(Display):UACEActionDeckSubsystem::DefaultRows;
            Rows->AddSlot().FillWidth(1).Padding(2)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[DeckLabel(Display>=Deck->VisibleRows()?TEXT("Extra (off)"):
                    Display==0?TEXT("Top"):Display==Deck->VisibleRows()-1?TEXT("Bottom"):*FString::Printf(TEXT("Row %d"),Display+1))]
                +SVerticalBox::Slot().AutoHeight()[SNew(SComboBox<TSharedPtr<FString>>).Tag(FName(*FString::Printf(TEXT("ActionDeck.RowModifier.%d"),R))).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding))
                    .ComboBoxStyle(&Theme->ComboBox).ItemStyle(&Theme->ComboRow)
                    .OptionsSource(&Modifiers).InitiallySelectedItem(Modifiers[Deck->RowModifiers[R]])
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> V){return DeckLabel(*V);})
                    .OnSelectionChanged_Lambda([this,R](TSharedPtr<FString> V,ESelectInfo::Type How)
                    {if(V&&How!=ESelectInfo::Direct)Deck->SetRowModifier(R,uint8(Modifiers.IndexOfByKey(V)));})
                    [SNew(SACEPluginText).Justification(ETextJustify::Center).Font(FACEPluginStyle::NativeFont("Regular",12))
                        .Text_Lambda([this,R]{return FText::FromString(*Modifiers[Deck->RowModifiers[R]]);})]]];
        }
        Body->AddSlot().AutoHeight()[Rows];
        Body->AddSlot().AutoHeight().Padding(0,6)[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",13))
            .Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("Row %d · Slot %d: %s"),Deck->DisplayRow(Selected)+1,Selected%UACEActionDeckSubsystem::Columns+1,*Deck->SlotName(Selected)));})
            .AutoWrapText(true)];
        auto Binding=SNew(SHorizontalBox);
        Body->AddSlot().AutoHeight()[DeckLabel(TEXT("Select a slot, then choose its key."),11)];
        Binding->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[DeckLabel(TEXT("Key"))];
        Binding->AddSlot().FillWidth(1)[SAssignNew(KeySelector,SInputKeySelector).Tag(TEXT("ActionDeck.Key")).AllowModifierKeys(true).AllowGamepadKeys(false)
            .SelectedKey_Lambda([this]{return Deck->EffectiveKey(Selected);})
            .OnIsSelectingKeyChanged_Lambda([this]{Deck->CapturingKey=KeySelector&&KeySelector->GetIsSelectingKey();})
            .OnKeySelected_Lambda([this](FInputChord K){Deck->SetKey(Selected,K);Deck->CapturingKey=false;})];
        Binding->AddSlot().AutoWidth().Padding(4,0)[Button(TEXT("Row default"),[this]
        {
            const auto Old=Deck->Slots[Selected];Deck->Slots[Selected].CustomKey=false;
            const auto K=Deck->EffectiveKey(Selected);Deck->Slots[Selected]=Old;
            if(Deck->SetKey(Selected,K)){Deck->Slots[Selected].CustomKey=false;Deck->Save();}
        })];
        Binding->AddSlot().AutoWidth()[Button(TEXT("Clear slot"),[this]{Deck->ClearSlot(Selected);})];
        Body->AddSlot().AutoHeight()[Binding];
        Body->AddSlot().AutoHeight().Padding(0,6)[SNew(SACEPluginButton).Tag(TEXT("ActionDeck.EditMacro"))
            .ButtonStyle(&Theme->DockButton).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding))
            .OnClicked_Lambda([this]{Deck->OpenMacroEditor(Selected);return FReply::Handled();})
            [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",12))
                .Text_Lambda([this]{return FText::FromString(Deck->Slot(Selected).Action==EACEDeckAction::Macro?TEXT("Edit macro…"):TEXT("Add macro…"));})]];
        Body->AddSlot().AutoHeight().Padding(0,6)[DeckLabel(TEXT("Assign an attack to the selected slot"))];
        for(bool Missile:{false,true})
        {
            auto Attacks=SNew(SHorizontalBox);
            for(int32 H=1;H<=3;++H)Attacks->AddSlot().FillWidth(1).Padding(2)
                [Button(FString(Missile?TEXT("Missile "):TEXT("Melee "))+ (H==1?TEXT("low"):H==2?TEXT("medium"):TEXT("high")),
                    [this,Missile,H]{Deck->BindAttack(Selected,Missile,H);})];
            Body->AddSlot().AutoHeight()[Attacks];
        }
        Body->AddSlot().AutoHeight().Padding(0,4)[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",12))
            .AutoWrapText(true).Text(FText::FromString(TEXT("Drag items or spells onto slots, or drag between slots to swap. Drag your main backpack to target yourself. Deck actions pause while editing.")))];
        Body->AddSlot().AutoHeight()[Button(TEXT("Restore default keys"),[this]
        {
            Deck->RowModifiers[0]=1;Deck->RowModifiers[1]=2;Deck->RowModifiers[2]=0;Deck->RowModifiers[3]=4;
            for(auto& S:Deck->Slots){S.CustomKey=false;S.Key=FInputChord();}
            Deck->Notice.Empty();Deck->Save();
        })];
        Settings->AddSlot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()[Body]];
    }
};

TSharedRef<SWidget> UACEActionDeckSubsystem::MakePanel()
{
    auto Widget=SNew(SACEActionDeck).Deck(this);Panel=Widget;return Widget;
}
bool UACEActionDeckSubsystem::DropAt(FVector2D Absolute,int32 ItemGuid,int32 SpellId)
{
    auto Widget=Panel.Pin();
    auto* Host=GetGameInstance()->GetSubsystem<UACEPluginSubsystem>();
    if(!Widget||!IsEnabled()||!Host->IsPluginWindowOpen(TEXT("actiondeck")))return false;
    const int32 Index=Widget->SlotAt(Absolute);if(Index==INDEX_NONE)return false;
    if(SpellId)BindSpell(Index,SpellId);else BindItem(Index,ItemGuid);
    return true;
}
