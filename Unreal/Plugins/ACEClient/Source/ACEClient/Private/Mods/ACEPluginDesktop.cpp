#include "Mods/ACEPluginDesktop.h"
#include "Mods/ACEPluginPresentation.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginIcon.h"
#include "Mods/ACEActionDeckSubsystem.h"
#include "Mods/ACEActionDeckLayout.h"
#include "Mods/ACEPluginOrder.h"
#include "Widgets/Images/SImage.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "Mobile/ACEMobileWidget.h"
#include "ACERuntimeOptions.h"
#include "UI/ACEUIElementManager.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

namespace
{
    const FString BarId = TEXT("__bar");
    // Hunt Ledger was fixed-size; its old .size entry stored a snapshot with
    // editor chrome. Only new user resizes should override its automatic height.
    FString SizeKey(const FString& Id){return Id+(Id==TEXT("huntledger")?TEXT(".body_size"):TEXT(".size"));}
    const FButtonStyle& DockButtonStyle(){return FACEPluginStyle::Current()->DockButton;}
    TSharedRef<SWidget> DockIcon(const FString& Id)
    {return SNew(SBox).WidthOverride(26).HeightOverride(26)[SNew(SACEPluginIcon).Id(Id)];}
    TSharedRef<STextBlock> Caption(const FString& Text, int32 Size = 12)
    { return SNew(SACEPluginText).Text(FText::FromString(Text)).Font(FCoreStyle::GetDefaultFontStyle("Regular",Size)); }

    class SPluginFrame : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SPluginFrame) : _Host(nullptr), _Frameless(false), _CompactWhenLocked(false), _Minimized(false) {}
            SLATE_ATTRIBUTE(FString, Title)
            SLATE_ARGUMENT(UACEPluginSubsystem*, Host)
            SLATE_ARGUMENT(FString, WindowId)
            SLATE_ARGUMENT(bool, Frameless)
            SLATE_ARGUMENT(TFunction<void(FVector2D,bool)>, Move)
            SLATE_ARGUMENT(TFunction<void(FVector2D,bool)>, Resize)
            SLATE_ARGUMENT(TFunction<void()>, Raise)
            SLATE_ARGUMENT(TFunction<void()>, Close)
            SLATE_ARGUMENT(bool, CompactWhenLocked)
            SLATE_ATTRIBUTE(bool, Locked)
            SLATE_ARGUMENT(TFunction<bool()>, CanManipulate)
            SLATE_ATTRIBUTE(bool, Minimized)
            SLATE_ARGUMENT(TFunction<void()>, ToggleMinimized)
            SLATE_DEFAULT_SLOT(FArguments, Content)
        SLATE_END_ARGS()
        void Construct(const FArguments& Args)
        {
            Host=Args._Host;Id=Args._WindowId;
            Retail=Host.IsValid()?FACEPluginStyle::Use(Host.Get(),Id):FACEPluginStyle::Current();
            ArtworkReady=Retail->HasDatArtwork();
            SetClipping(EWidgetClipping::ClipToBounds);
            Move = Args._Move; Resize = Args._Resize; Raise = Args._Raise;
            Locked=Args._Locked;
            Frameless=Args._Frameless;
            CanManipulate=Args._CanManipulate;
            if(Frameless){ChildSlot[Args._Content.Widget];return;}
            auto Title = SNew(SHorizontalBox);
            const bool IsBar=bool(Args._ToggleMinimized);
            if(IsBar)
            {
                Title->AddSlot().FillWidth(1).HAlign(HAlign_Center).VAlign(VAlign_Center)
                    [SNew(SBox).WidthOverride(22).HeightOverride(2)
                    [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.6f,.65f,.7f))]];
                Title->AddSlot().AutoWidth()[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ChromeButtonPadding)).Tag(TEXT("PluginBarMinimize")).ButtonStyle(&DockButtonStyle()).IsFocusable(false)
                    .ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([Mini=Args._Minimized](){return FText::FromString(Mini.Get()?TEXT("Expand plugin bar"):TEXT("Minimize plugin bar"));})))
                    .OnClicked_Lambda([Toggle=Args._ToggleMinimized](){Toggle();return FReply::Handled();})
                    [SNew(SACEPluginText).Font(FCoreStyle::GetDefaultFontStyle("Bold",12))
                        .Text_Lambda([Mini=Args._Minimized](){return FText::FromString(Mini.Get()?TEXT("+"):TEXT("-"));})]];
            }
            else Title->AddSlot().FillWidth(1).VAlign(VAlign_Center)[SNew(SACEPluginText).Text_Lambda([Title=Args._Title]{return FText::FromString(Title.Get());}).Font(FCoreStyle::GetDefaultFontStyle("Regular",12))];
            if(!IsBar&&Host.IsValid())Title->AddSlot().AutoWidth()[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ChromeButtonPadding)).Tag(TEXT("PluginFramePin"))
                .IsFocusable(false).ButtonStyle(&Retail->DockButton)
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Pin and hide frame. Hold Quick-show pinned windows to move or unpin."))))
                .OnClicked_Lambda([H=Host,Window=Id]{if(H.IsValid())H->SetPluginWindowPinned(Window,!H->IsPluginWindowPinned(Window));return FReply::Handled();})
                [SNew(SACEPluginText).Font(FCoreStyle::GetDefaultFontStyle("Regular",12))
                    .Text_Lambda([H=Host,Window=Id]{return FText::FromString(H.IsValid()&&H->IsPluginWindowPinned(Window)?TEXT("Unpin"):TEXT("Pin"));})]];
            if (Args._Close) Title->AddSlot().AutoWidth()[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ChromeButtonPadding)).IsFocusable(false).ButtonStyle(&DockButtonStyle())
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Hide window (plugin keeps running)"))))
                .OnClicked_Lambda([Close=Args._Close](){Close();return FReply::Handled();})[Caption(TEXT("X"))]];
            ChildSlot[SNew(SBorder).BorderImage(&Retail->Frame).Padding(IsBar?0:3)
                [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(IsBar?18:ACELedger::HeaderHeight)
                    .Tag(Args._CompactWhenLocked?FName(TEXT("LedgerHeader")):FName(NAME_None))
                    .Visibility_Lambda([Lock=Locked]{return Lock.Get(false)?EVisibility::Collapsed:EVisibility::Visible;})
                    [SNew(SBorder).Tag(IsBar?TEXT("PluginBarHandle"):TEXT("PluginWindowHandle")).BorderImage(&Retail->TitleBackground)
                    .Padding(IsBar?0:3)
                    .ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([this](){return FText::FromString(CanManipulate&&!CanManipulate()?TEXT("Unlock the UI or hold Quick-show pinned windows to move"):TEXT("Drag to move"));})))
                    .Cursor_Lambda([this](){return CanManipulate&&!CanManipulate()?EMouseCursor::Default:EMouseCursor::CardinalCross;})
                    .OnMouseButtonDown_Lambda([this](const FGeometry&, const FPointerEvent& E)
                    {
                        if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
                        if(!Move||(CanManipulate&&!CanManipulate()))return FReply::Handled();
                        Dragging=true;return FReply::Handled().CaptureMouse(SharedThis(this));
                    })[Title]]]
                +SVerticalBox::Slot().FillHeight(1)[SNew(SBox).Padding(0,IsBar?0:3)
                    .Tag(Args._CompactWhenLocked?FName(TEXT("LedgerContent")):FName(NAME_None))
                    .Visibility_Lambda([Mini=Args._Minimized](){return Mini.Get(false)?EVisibility::Collapsed:EVisibility::Visible;})[Args._Content.Widget]]
                +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)[SNew(SBorder).Padding(FMargin(12,2)).Visibility_Lambda([this]{return Resize&&(!CanManipulate||CanManipulate())?EVisibility::Visible:EVisibility::Collapsed;})
                    .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Drag to resize")))).Cursor(EMouseCursor::ResizeSouthEast)
                    .OnMouseButtonDown_Lambda([this](const FGeometry&,const FPointerEvent& E){if(E.GetEffectingButton()!=EKeys::LeftMouseButton||(CanManipulate&&!CanManipulate()))return FReply::Unhandled();Resizing=true;return FReply::Handled().CaptureMouse(SharedThis(this));})[Caption(TEXT("◢"))]]]];
        }
        virtual void Tick(const FGeometry& G,double Time,float Delta) override
        {
            SCompoundWidget::Tick(G,Time,Delta);
            if(!Host.IsValid())return;
            bool ArtworkChanged=false;
            if(!ArtworkReady){Retail->Refresh();ArtworkChanged=ArtworkReady=Retail->HasDatArtwork();}
            const float Opacity=Host->GetPluginWindowOpacity(Id);
            const bool Hide=Host->IsPluginWindowChromeHidden(Id);
            if(!ArtworkChanged&&LastOpacity==Opacity&&LastHidden==Hide)return;
            LastOpacity=Opacity;LastHidden=Hide;Retail->SetWindowAppearance(Opacity,Hide);
            // Brush updates repaint the existing tree, preserving menus, focus and scroll.
            TFunction<void(SWidget&)> Repaint=[&](SWidget& W){W.Invalidate(ArtworkChanged?EInvalidateWidgetReason::Layout|EInvalidateWidgetReason::Paint:EInvalidateWidgetReason::Paint);
                if(auto* Children=W.GetChildren())for(int32 I=0;I<Children->Num();++I)Repaint(Children->GetChildAt(I).Get());};
            Repaint(*this);
        }
        virtual FReply OnPreviewMouseButtonDown(const FGeometry&,const FPointerEvent& E) override
        {
            if(Raise)Raise();
            return FReply::Unhandled();
        }
        virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E) override
        {
            if(!Frameless||E.GetEffectingButton()!=EKeys::LeftMouseButton||(CanManipulate&&!CanManipulate()))return FReply::Unhandled();
            const auto At=G.AbsoluteToLocal(E.GetScreenSpacePosition());
            Resizing=Resize&&At.X>=G.GetLocalSize().X-24&&At.Y>=G.GetLocalSize().Y-24;
            Dragging=!Resizing;return FReply::Handled().CaptureMouse(SharedThis(this));
        }
        virtual FReply OnMouseMove(const FGeometry& G,const FPointerEvent& E) override
        {
            if((!Dragging&&!Resizing)||!HasMouseCapture())return FReply::Unhandled();
            auto& Action=Resizing?Resize:Move;
            if(CanManipulate&&!CanManipulate())
            {Action(FVector2D::ZeroVector,true);Dragging=Resizing=false;return FReply::Handled().ReleaseMouseCapture();}
            Action(G.AbsoluteToLocal(E.GetScreenSpacePosition())-G.AbsoluteToLocal(E.GetLastScreenSpacePosition()),false);
            return FReply::Handled();
        }
        virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E) override
        {
            if((!Dragging&&!Resizing)||E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
            auto& Action=Resizing?Resize:Move;Action(FVector2D::ZeroVector,true);Dragging=Resizing=false;return FReply::Handled().ReleaseMouseCapture();
        }
        virtual void OnMouseCaptureLost(const FCaptureLostEvent& E) override
        { if(Dragging)Move(FVector2D::ZeroVector,true);if(Resizing&&Resize)Resize(FVector2D::ZeroVector,true);Dragging=Resizing=false;SCompoundWidget::OnMouseCaptureLost(E); }
    private:
        TSharedPtr<FACEPluginStyle> Retail;
        TWeakObjectPtr<UACEPluginSubsystem> Host;FString Id;
        float LastOpacity=-1;bool LastHidden=false;
        bool ArtworkReady=false;
        bool Dragging=false,Resizing=false,Frameless=false;
        TFunction<void(FVector2D,bool)> Move,Resize;
        TFunction<void()> Raise;
        TAttribute<bool> Locked;
        TFunction<bool()> CanManipulate;
    };
}

void SACEPluginDesktop::Construct(const FArguments& Args)
{
    Host=Args._Host;ManagerOnly=Args._ManagerOnly;FACEPluginStyle::Use(Args._Host);
    ConstrainWindows=ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"))>.5f;
    SetVisibility(EVisibility::SelfHitTestInvisible);
    SetClipping(EWidgetClipping::ClipToBounds);
    ChildSlot[SAssignNew(Canvas,SConstraintCanvas)];
    if(ManagerOnly){Toggle(TEXT("__manager"));return;}
    FWindow Bar;
    Bar.Position=Host->GetPluginWindowPosition(BarId,FVector2D(8,80));Bar.Size=FVector2D(38,120);Bar.PluginBar=true;
    Bar.Widget=SNew(SPluginFrame)
        .Minimized_Lambda([H=Host](){return H.IsValid()&&H->IsPluginBarMinimized();})
        .ToggleMinimized([H=Host](){if(H.IsValid())H->SetPluginBarMinimized(!H->IsPluginBarMinimized());})
        .CanManipulate([H=Host](){return H.IsValid()&&H->IsPluginBarUnlocked();})
        .Move([this](FVector2D D,bool Save){Move(BarId,D,Save);})
        .Raise([this](){Raise(BarId);})
        [SAssignNew(BarScroll,SScrollBox).ScrollBarVisibility(EVisibility::Collapsed)
            +SScrollBox::Slot()[SAssignNew(Buttons,SStackBox).Orientation(Orient_Vertical)]];
    Canvas->AddSlot().Expose(Bar.Slot).Alignment(FVector2D::ZeroVector).ZOrder(10000)[Bar.Widget.ToSharedRef()];
    Windows.Add(BarId,Bar);
    Refresh();
}
FVector2D SACEPluginDesktop::DisplaySize(const FWindow& W) const
{
    if(Host.IsValid())
    {
        if(!W.LedgerId.IsEmpty())
        {
            auto Size=Host->LedgerSize(W.LedgerId);
            Size.X=W.Size.X;
            if(W.Size.Y>0)
            {
                const double SavedHeight=W.Size.Y+Host->LedgerChromeHeight(W.LedgerId,Host->IsPluginWindowChromeHidden(W.LedgerId));
                // Hunt rows must fit even when restoring a height saved before a new metric was added.
                Size.Y=W.LedgerId==TEXT("huntledger")?FMath::Max(Size.Y,SavedHeight):SavedHeight;
            }
            return Size;
        }
        if(W.PluginBar&&Host->IsPluginBarMinimized())return FVector2D(Host->IsPluginBarHorizontal()?70:38,18);
        if(W.MapOverlay&&Host->WaypointOption(TEXT("map_minimized"),false))return FVector2D(70,26);
    }
    auto Size=W.Size;
    if(W.Id==TEXT("actiondeck")&&Host.IsValid())
    {
        const auto* Deck=Host->GetGameInstance()->GetSubsystem<UACEActionDeckSubsystem>();
        const bool Hidden=Host->IsPluginWindowChromeHidden(W.Id);
        // Square rows, toolbar, frame padding and the resize grip.
        // Only an active macro adds the status footer.
        // The grip is outside the content area and must not cover the last row.
        Size.Y=(Size.X-6)*Deck->VisibleRows()/UACEActionDeckSubsystem::Columns+68+(Host->CanManipulatePluginWindow(W.Id)?24:0)
            +(Deck->IsConfigOpen()?300:0)-(Hidden?ACEActionDeckLayout::ToolbarHeight:0)
            +(Deck->IsMacroRunning()?ACEActionDeckLayout::StatusHeight:0);
    }
    if(!W.PluginBar&&!W.MapOverlay&&W.Id!=TEXT("waypoint.arrow")&&Host.IsValid()&&Host->IsPluginWindowChromeHidden(W.Id))
        Size.Y=FMath::Max(1.,Size.Y-ACELedger::HeaderHeight);
    return Size;
}
void SACEPluginDesktop::Layout(FWindow& W)
{
    const FVector2D Display=DisplaySize(W);
    const bool HiddenHeader=!W.PluginBar&&!W.MapOverlay&&W.Id!=TEXT("waypoint.arrow")&&Host.IsValid()&&Host->IsPluginWindowChromeHidden(W.Id);
    const bool LockedLedger=HiddenHeader&&!W.LedgerId.IsEmpty();
    const double HeaderOffset=HiddenHeader?ACELedger::HeaderHeight
        +(W.Id==TEXT("actiondeck")?ACEActionDeckLayout::ToolbarHeight:0):0.;
    // Saved placement describes the expanded window. Remove the header above
    // the content, and clamp against that same footprint in both lock states.
    // Otherwise both collapsing the header and bottom-edge clamping move rows.
    FVector2D Expanded=Display;
    if(LockedLedger)Expanded.Y+=Host->LedgerChromeHeight(W.LedgerId,false)-Host->LedgerChromeHeight(W.LedgerId,true);
    else if(HiddenHeader)Expanded.Y+=HeaderOffset;
    if(!ConstrainWindows||ViewSize.X<=0||ViewSize.Y<=0)
    {W.Slot->SetOffset(FMargin(W.Position.X,W.Position.Y+HeaderOffset,Display.X,Display.Y));return;}
    const FVector2D Size(FMath::Min(Display.X,ViewSize.X),FMath::Min(Display.Y,FMath::Max(0.,ViewSize.Y-HeaderOffset)));
    // Fit the rendered window without overwriting the user's saved placement
    // during login, minimization or a temporary viewport size change.
    const FVector2D At(FMath::Clamp(W.Position.X,0.,FMath::Max(0.,ViewSize.X-Size.X)),
        FMath::Clamp(W.Position.Y,0.,FMath::Max(0.,ViewSize.Y-Expanded.Y))+HeaderOffset);
    W.Slot->SetOffset(FMargin(At.X,At.Y,Size.X,Size.Y));
}
void SACEPluginDesktop::Tick(const FGeometry& G,double Time,float Delta)
{
    SCompoundWidget::Tick(G,Time,Delta);
    const bool Constrain=ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"))>.5f;
    const bool BoundsChanged=Constrain!=ConstrainWindows||!G.GetLocalSize().Equals(ViewSize);
    ConstrainWindows=Constrain;ViewSize=G.GetLocalSize();
    if(auto* Map=Windows.Find(TEXT("waypoint.dungeon"));Map&&Map->Open&&Host.IsValid())
    {
        const auto P=Host->WaypointPlayerPosition();
        Map->Widget->SetVisibility(!P.IsValid()?EVisibility::Collapsed:EVisibility::Visible);
        Layout(*Map); // Minimize without replacing the saved expanded size.
    }
    for(auto& Pair:Windows)
    {
        const bool Hidden=Host.IsValid()&&Host->IsPluginWindowChromeHidden(Pair.Key);
        if(BoundsChanged||Hidden!=Pair.Value.HiddenChrome||!Pair.Value.LedgerId.IsEmpty()||Pair.Key==TEXT("actiondeck"))Layout(Pair.Value);
        Pair.Value.HiddenChrome=Hidden;
    }
}
void SACEPluginDesktop::Move(const FString& Id,FVector2D Delta,bool Save)
{
    if(auto* W=Windows.Find(Id))
    {
        if(!Delta.IsNearlyZero()&&(!Host.IsValid()||!Host->CanManipulatePluginWindow(Id)))return;
        if(!Delta.IsNearlyZero())
        {
            const auto Offset=W->Slot->GetOffset();W->Position=FVector2D(Offset.Left,Offset.Top)+Delta;
            if(ConstrainWindows&&ViewSize.X>0&&ViewSize.Y>0)
            {const FVector2D Size=DisplaySize(*W);W->Position.X=FMath::Clamp(W->Position.X,0.,FMath::Max(0.,ViewSize.X-Size.X));W->Position.Y=FMath::Clamp(W->Position.Y,0.,FMath::Max(0.,ViewSize.Y-Size.Y));}
        }
        Layout(*W);if(Save&&Host.IsValid())Host->SavePluginWindowPosition(Id,W->Position);
    }
}
void SACEPluginDesktop::SaveOverlayLayout()
{
    if(!Host.IsValid())return;
    // A logout during a drag must save every plugin, including third-party UI.
    for(const auto& Pair:Windows)
    {
        Host->SavePluginWindowPosition(Pair.Key,Pair.Value.Position);
        if(!Pair.Value.PluginBar)Host->SavePluginWindowPosition(SizeKey(Pair.Key),Pair.Value.Size);
    }
}
void SACEPluginDesktop::Resize(const FString& Id,FVector2D Delta,bool Save)
{
    if(!Delta.IsNearlyZero()&&(!Host.IsValid()||!Host->CanManipulatePluginWindow(Id)))return;
    if(auto* W=Windows.Find(Id))
    {
     if(UACEPluginSubsystem::IsLedger(Id))
     {
         // Persist the list area; pinning only hides the editor chrome.
         const double Chrome=Host->LedgerChromeHeight(Id,Host->IsPluginWindowChromeHidden(Id));
         const FVector2D Size=DisplaySize(*W)+Delta;
         const double MinWidth=Id==TEXT("supplyledger")?72.:160.;
         W->Size=FVector2D(FMath::Clamp(Size.X,MinWidth,FMath::Max(MinWidth,ViewSize.X)),
             FMath::Clamp(Size.Y-Chrome,40.,FMath::Max(40.,ViewSize.Y-Chrome)));
         Layout(*W);if(Save)Host->SavePluginWindowPosition(SizeKey(Id),W->Size);return;
     }
     W->Size+=Delta;const double MinWidth=Id==TEXT("actiondeck")?360.:Id==TEXT("waypoint.route")?280.:96.,MinHeight=Id==TEXT("waypoint.route")?180.:64.;W->Size.X=FMath::Clamp(W->Size.X,MinWidth,FMath::Max(MinWidth,ViewSize.X));W->Size.Y=FMath::Clamp(W->Size.Y,MinHeight,FMath::Max(MinHeight,ViewSize.Y));Layout(*W);
     if(Save&&Host.IsValid())Host->SavePluginWindowPosition(Id+TEXT(".size"),W->Size);}
}
void SACEPluginDesktop::Raise(const FString& Id)
{if(Id!=BarId)if(auto* W=Windows.Find(Id))W->Slot->SetZOrder(++TopOrder);}
FString SACEPluginDesktop::ResolveWindowId(const FString& Id) const
{
    // NAV and Map are entry views of the same interactive Waypoint interface.
    // Keep the first frame and its current page, input and layout. The pinned
    // minimap and direction arrow remain separate HUD readouts.
    if(Id==TEXT("waypoint")||Id==TEXT("waypoint.map"))
        for(const TCHAR* Existing:{TEXT("waypoint"),TEXT("waypoint.map")})
            if(Windows.Contains(Existing))return Existing;
    return Id;
}
bool SACEPluginDesktop::IsOpen(const FString& Id) const
{const auto* W=Windows.Find(ResolveWindowId(Id));return W&&W->Open;}
bool SACEPluginDesktop::ContainsPoint(const FString& Id,FVector2D Absolute) const
{
    const auto* W=Windows.Find(ResolveWindowId(Id));
    return W&&W->Open&&W->Widget&&W->Widget->GetCachedGeometry().IsUnderLocation(Absolute);
}
void SACEPluginDesktop::Hide(const FString& Id,bool Remember)
{
    if(auto* W=Windows.Find(Id))
    {
        const bool Focused=W->Widget->HasKeyboardFocus()||W->Widget->HasFocusedDescendants();
        if(Host.IsValid()&&Remember)
        {
            if(Id==TEXT("waypoint.arrow"))Host->SetWaypointOption(TEXT("arrow"),false);
            else Host->SavePluginWindowVisibility(Id,false);
        }
        W->Open=false;W->Widget->SetVisibility(EVisibility::Collapsed);
        if(Focused)FSlateApplication::Get().SetAllUserFocusToGameViewport();
    }
}
void SACEPluginDesktop::Toggle(const FString& RequestedId)
{
    const FString Id=ResolveWindowId(RequestedId);
    if(IsOpen(Id))Hide(Id);
    else Open(Id);
}
bool SACEPluginDesktop::Open(const FString& RequestedId)
{
    if(!Host.IsValid())return false;
    const FString Id=ResolveWindowId(RequestedId);
    const bool Manager=Id==TEXT("__manager");
    TSharedPtr<FACEClientPlugin> Plugin;
    for(auto P:Host->Plugins)if((P->Id==Id || (Id.StartsWith(TEXT("waypoint."))&&P->Id==TEXT("waypoint")) || (Id==TEXT("ucm.log")&&P->Id==TEXT("ucm")))&&(P->Enabled||Id==TEXT("ucm.log")))Plugin=P;
    if(!Plugin&&!Manager)return false;
    if(Id==TEXT("waypoint.route")&&Host->WaypointItinerary.IsEmpty())return false;
    FACEPluginStyle::Use(Host.Get());
    const bool Arrow=Id==TEXT("waypoint.arrow");
    if(Arrow&&!Host->ShouldShowWaypointArrow())return false;
    TAttribute<EVisibility> OpenVisibility(EVisibility::Visible);
    if(Arrow)OpenVisibility.BindLambda([H=Host]()
    {
        if(!H.IsValid()||!H->ShouldShowWaypointArrow())return EVisibility::Collapsed;
        return H->IsWaypointUnlocked()?EVisibility::Visible:EVisibility::HitTestInvisible;
    });
    if(auto* W=Windows.Find(Id))
    {
        Host->SavePluginWindowVisibility(Id,true);W->Open=true;W->Widget->SetVisibility(OpenVisibility);
        Layout(*W);Raise(Id);return true;
    }
    FWindow W;W.Id=Id;W.Position=Host->GetPluginWindowPosition(Id,FVector2D(84+24*(Windows.Num()-1),80+24*(Windows.Num()-1)));W.Size=Host->GetPluginWindowPosition(Id+TEXT(".size"),FVector2D(780,700));
    const bool Dungeon=Id==TEXT("waypoint.dungeon");
    W.MapOverlay=Dungeon;
    const bool Ledger=UACEPluginSubsystem::IsLedger(Id);
    if(Ledger){W.LedgerId=Id;W.Size=Host->GetPluginWindowPosition(SizeKey(Id),FVector2D(Id==TEXT("supplyledger")?200:290,0));}
    if(Id==TEXT("ucmmicro"))W.Size=Host->GetPluginWindowPosition(Id+TEXT(".size"),FVector2D(270,360));
    if(Id==TEXT("actiondeck"))W.Size=Host->GetPluginWindowPosition(Id+TEXT(".size"),FVector2D(530,203));
    if(Id==TEXT("ucm.log"))W.Size=Host->GetPluginWindowPosition(Id+TEXT(".size"),FVector2D(700,440));
    if(Id==TEXT("waypoint.route"))W.Size=Host->GetPluginWindowPosition(Id+TEXT(".size"),FVector2D(440,360));
    if(Manager)W.Size=Host->GetPluginWindowPosition(Id+TEXT(".size"),FVector2D(740,600));
    if(Arrow)W.Size=FVector2D(290,225);
    if(Dungeon)W.Size=Host->GetPluginWindowPosition(Id+TEXT(".size"),FVector2D(420,420));
    W.Widget=SNew(SPluginFrame).Host(Host.Get()).WindowId(Id).Title_Lambda([Id,Manager,Arrow,Plugin](){return Manager?TEXT("Plugin Manager"):Id==TEXT("ucm.log")?TEXT("UCM Log"):Arrow?TEXT("Waypoint · drag when unlocked"):Id==TEXT("waypoint.map")?TEXT("Waypoint Map"):Id==TEXT("waypoint.route")?TEXT("Waypoint Route"):Plugin->DisplayTitle();})
        .Frameless(Arrow||Dungeon)
        .CompactWhenLocked(Ledger)
        .Locked_Lambda([H=Host,Id]{return H.IsValid()&&H->IsPluginWindowChromeHidden(Id);})
        .CanManipulate([H=Host,Id]{return H.IsValid()&&H->CanManipulatePluginWindow(Id);})
        .Move([this,Id](FVector2D D,bool Save){Move(Id,D,Save);})
        .Resize(Arrow?TFunction<void(FVector2D,bool)>():TFunction<void(FVector2D,bool)>([this,Id](FVector2D D,bool Save){Resize(Id,D,Save);}))
        .Raise([this,Id](){Raise(Id);}).Close([this,Id,Manager](){if(Manager&&ManagerOnly){Host->CloseDesktopPluginManager();return;}Hide(Id);})[Manager?Host->MakePanel():Dungeon?Host->MakeWaypointDungeonOverlay():Arrow?Host->MakeWaypointPanel(false,true):Host->MakePanel(Id)];
    if(Dungeon)W.Widget->SetVisibility(EVisibility::Collapsed); // Tick validates the current cell before showing it.
    if(Arrow)W.Widget->SetVisibility(OpenVisibility);
    Canvas->AddSlot().Expose(W.Slot).Alignment(FVector2D::ZeroVector).ZOrder(++TopOrder)[W.Widget.ToSharedRef()];Layout(W);Windows.Add(Id,W);
    Host->SavePluginWindowVisibility(Id,true);
    return true;
}
void SACEPluginDesktop::Refresh()
{
    if(!Host.IsValid()||ManagerOnly)return;
    FACEPluginStyle::Use(Host.Get());
    for(const auto& P:Host->Plugins)if(P->Enabled&&Host->WasPluginWindowOpen(P->Id)&&!Windows.Contains(ResolveWindowId(P->Id)))Open(P->Id);
    Windows.FindChecked(BarId).Widget->SetVisibility(Host->IsPluginBarVisible()?EVisibility::Visible:EVisibility::Collapsed);
    Layout(Windows.FindChecked(BarId));
    const bool Horizontal=Host->IsPluginBarHorizontal();
    FString Next=(Horizontal?FString(TEXT("horizontal|")):FString(TEXT("vertical|")))+FString::FromInt(Host->GetPluginOrderRevision())+TEXT("|");TSet<FString> Enabled;
    for(auto P:Host->Plugins)if(P->Enabled){Next+=P->Id+TEXT("|");Enabled.Add(P->Id);}
    if(Host->Plugins.ContainsByPredicate([](const auto& P){return P->Id==TEXT("ucm");}))Enabled.Add(TEXT("ucm.log"));
    for(const TCHAR* Id:{TEXT("ucm.log"),TEXT("waypoint.map")})
        if((Enabled.Contains(Id)||(FString(Id)==TEXT("waypoint.map")&&Enabled.Contains(TEXT("waypoint"))))
            &&Host->WasPluginWindowOpen(Id)&&!Windows.Contains(ResolveWindowId(Id)))Open(Id);
    if(Enabled.Contains(TEXT("waypoint")))
    {
        Enabled.Add(TEXT("waypoint.map"));Enabled.Add(TEXT("waypoint.arrow"));Enabled.Add(TEXT("waypoint.dungeon"));Enabled.Add(TEXT("waypoint.route"));
        if(!Host->WaypointItinerary.IsEmpty()&&Host->WasPluginWindowOpen(TEXT("waypoint.route"))&&!Windows.Contains(TEXT("waypoint.route")))Open(TEXT("waypoint.route"));
        const bool Show=Host->ShouldShowWaypointArrow();
        if(Show && !IsOpen(TEXT("waypoint.arrow")))Toggle(TEXT("waypoint.arrow"));
        else if(!Show && IsOpen(TEXT("waypoint.arrow")))Hide(TEXT("waypoint.arrow"),false);
        const bool Pin=Host->WaypointOption(TEXT("dungeon_overlay"));
        if(Pin&&!IsOpen(TEXT("waypoint.dungeon")))Toggle(TEXT("waypoint.dungeon"));
        else if(!Pin&&IsOpen(TEXT("waypoint.dungeon")))Hide(TEXT("waypoint.dungeon"));
    }
    if(Next==Signature&&Buttons->NumSlots())return;
    Signature=Next;
    for(auto It=Windows.CreateIterator();It;++It)if(It.Key()!=BarId&&!Enabled.Contains(It.Key()))
    {Hide(It.Key(),false);Canvas->RemoveSlot(It.Value().Widget.ToSharedRef());It.RemoveCurrent();}
    Buttons->ClearChildren();
    Buttons->SetOrientation(Horizontal?Orient_Horizontal:Orient_Vertical);
    BarScroll->SetOrientation(Horizontal?Orient_Horizontal:Orient_Vertical);
    BarScroll->ScrollToStart();
    for(const FString& Id:Host->PluginDisplayOrder())
    {
        TSharedPtr<FACEClientPlugin> P;
        for(const auto& Candidate:Host->Plugins)if(Candidate->Id==Id)P=Candidate;
        const bool Manager=Id==TEXT("__manager");
        if(!Manager&&!(P&&P->Enabled)&&!(Id==TEXT("ucm.log")&&Enabled.Contains(TEXT("ucm")))
            &&!(Id==TEXT("waypoint.map")&&Enabled.Contains(TEXT("waypoint"))))continue;
        auto Icon=P?StaticCastSharedRef<SWidget>(SNew(SACEPluginBadge).Plugin(P)):DockIcon(Manager?TEXT("plugin.options"):Id);
        Buttons->AddSlot().AutoSize().Padding(1)[SNew(SACEPluginOrderTarget).Host(Host.Get()).Id(Id).LockId(BarId).Horizontal(Horizontal)
            .Tag(FName(*(TEXT("PluginBarOrder.")+Id)))
            [SNew(SBox).WidthOverride(32).HeightOverride(32)
            [SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::IconButtonPadding)).Tag(Manager?FName(TEXT("PluginOptions")):FName(*(TEXT("PluginBarButton.")+Id)))
                .ButtonStyle(&DockButtonStyle()).IsFocusable(false).AllowDragDrop(true)
                .OnSlateButtonDragDetected_Lambda([H=Host,Id](const FGeometry&,const FPointerEvent&){return ACEPluginOrder::BeginDrag(H.Get(),Id,BarId);})
                .ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([H=Host,P,Id,Manager]()
                {
                    FString Tip=Manager?TEXT("Plugin Manager"):P?P->DisplayTitle():Id==TEXT("ucm.log")?TEXT("UCM activity and problem log"):TEXT("Waypoint world / dungeon map");
                    if(P&&P->Id==TEXT("looteditor"))
                    {
                        bool Looting=false;for(const auto& Other:H->Plugins)if(Other->Id==TEXT("ucm"))Other->Profile->TryGetBoolField(TEXT("looting"),Looting);
                        Tip+=TEXT("\nClick to open/close the editor. This does not toggle looting.\nUCM looting: ")+FString(Looting?TEXT("enabled"):TEXT("disabled"));
                    }
                    else if(P)Tip+=TEXT("\nClick to show/hide\n")+P->Status;
                    return FText::FromString(Tip+TEXT("\nDrag to reorder while unlocked. Hold Quick-show if pinned."));
                })))
                .OnClicked_Lambda([this,Id,Manager](){if(Manager)Host->TogglePanel();else Toggle(Id);return FReply::Handled();})[Icon]]]];
    }
    auto& Bar=Windows.FindChecked(BarId);
    Bar.Size=Horizontal?FVector2D(4+34*Buttons->NumSlots(),52):FVector2D(38,18+34*Buttons->NumSlots());
    Layout(Bar);
}

void UACEPluginSubsystem::UpdateDesktopDock()
{
    auto* GI=GetGameInstance();auto* V=GI->GetGameViewportClient();
    auto* PC=Cast<AACEPlayerController>(GI->GetFirstLocalPlayerController());
    auto* C=GI->GetSubsystem<UACEClientSubsystem>();
    if(!V||!PC||!C||C->GetSessionState()!=EACESessionState::InWorld||PC->IsVRActive()||PC->IsMobileActive())
    {
        const bool LeavingDesktop=DesktopDock.IsValid()||(PC&&(PC->IsVRActive()||PC->IsMobileActive()));
        RemoveDesktopDock();
        if(LeavingDesktop)CloseDesktopPluginManager(false);
        return;
    }
    if(!DesktopDock)
    {
        // A login manager may contain an embedded plugin. Desktop gameplay
        // instead opens that plugin in its one dock-owned frame.
        CloseDesktopPluginManager(false);
        DesktopDock=SNew(SACEPluginDesktop).Host(this);
        DesktopDock->SetVisibility(TAttribute<EVisibility>::CreateLambda([Weak=TWeakObjectPtr<AACEPlayerController>(PC)]()
            {return Weak.IsValid()&&!Weak->IsDesktopInterfaceHidden()?EVisibility::SelfHitTestInvisible:EVisibility::Collapsed;}));
        V->AddViewportWidgetContent(DesktopDock.ToSharedRef(),1500);
    }
    DesktopDock->Refresh();
    if(!DesktopPanel&&WasPluginWindowOpen(TEXT("__manager")))TogglePanel();
}
void UACEPluginSubsystem::RemoveDesktopDock()
{
    if(DesktopDock)DesktopDock->SaveOverlayLayout();
    if(DesktopDock)if(auto* V=GetGameInstance()->GetGameViewportClient())V->RemoveViewportWidgetContent(DesktopDock.ToSharedRef());
    DesktopDock.Reset();
}
bool UACEPluginSubsystem::OpenPluginWindow(const FString& Id)
{
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());PC&&PC->IsMobileActive()&&PC->MobileWidget)
    {
        const FName Page(*(TEXT("Plugin:")+Id));
        if(PC->MobileWidget->Panel!=Page&&!PC->MobileWidget->IsHUDPluginVisible(Id))PC->MobileWidget->OpenPanel(Page);
        return IsPluginWindowOpen(Id);
    }
    UpdateDesktopDock();return DesktopDock&&DesktopDock->Open(Id);
}
void UACEPluginSubsystem::TogglePluginWindow(const FString& Id)
{
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());PC && PC->IsMobileActive() && PC->MobileWidget)
    {
        if(Id==TEXT("waypoint.arrow"))
        {
            if(PC->MobileWidget->IsHUDPluginVisible(Id))SetWaypointOption(TEXT("arrow"),false);
            return;
        }
        const FName Page(*(TEXT("Plugin:")+Id));
        if(PC->MobileWidget->Panel==Page)PC->MobileWidget->ClosePanel();
        else if(PC->MobileWidget->IsHUDPluginVisible(Id))PC->MobileWidget->ToggleHUDPlugin(Id);
        else PC->MobileWidget->OpenPanel(Page);
        return;
    }
    UpdateDesktopDock();if(DesktopDock)DesktopDock->Toggle(Id);
}
bool UACEPluginSubsystem::IsPluginWindowOpen(const FString& Id) const
{
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());PC && PC->IsMobileActive() && PC->MobileWidget)
        return PC->MobileWidget->Panel==FName(*(TEXT("Plugin:")+Id)) || PC->MobileWidget->IsHUDPluginVisible(Id);
    return DesktopDock&&DesktopDock->IsOpen(Id);
}
bool UACEPluginSubsystem::IsPointInsidePluginWindow(const FString& Id,FVector2D Absolute) const
{return DesktopDock&&DesktopDock->ContainsPoint(Id,Absolute);}
bool UACEPluginSubsystem::WasPluginWindowOpen(const FString& Id) const
{
    // Preserve ledger preferences from versions before ordinary windows saved
    // visibility. Arrow/pinned-map visibility remains owned by waypoint options.
    if(IsLedger(Id))return LedgerOption(Id,TEXT("visible"));
    if(Id==TEXT("waypoint.arrow"))return WaypointOption(TEXT("arrow"));
    const TSharedPtr<FJsonObject>* Visible=nullptr;bool Open=false;
    if(Settings&&Settings->TryGetObjectField(TEXT("_window_visibility"),Visible))(*Visible)->TryGetBoolField(Id,Open);
    return Open;
}
void UACEPluginSubsystem::SavePluginWindowVisibility(const FString& Id,bool Open)
{
    if(!Settings||Id==TEXT("waypoint.arrow"))return;
    if(IsLedger(Id)){SetLedgerOption(Id,TEXT("visible"),Open);return;}
    const TSharedPtr<FJsonObject>* Existing=nullptr;
    auto Visible=Settings->TryGetObjectField(TEXT("_window_visibility"),Existing)?*Existing:MakeShared<FJsonObject>();
    const FString Other=Id==TEXT("waypoint")?TEXT("waypoint.map"):Id==TEXT("waypoint.map")?TEXT("waypoint"):TEXT("");
    bool Previous=false,OtherOpen=false;
    const bool Same=Visible->TryGetBoolField(Id,Previous)&&Previous==Open;
    if(Same&&(Other.IsEmpty()||(Visible->TryGetBoolField(Other,OtherOpen)&&!OtherOpen)))return;
    // Restore only the entry view that owns the frame. A stale alias must not
    // resurrect another interface after the user closes this one.
    if(!Other.IsEmpty())Visible->SetBoolField(Other,false);
    Visible->SetBoolField(Id,Open);Settings->SetObjectField(TEXT("_window_visibility"),Visible);SaveSettings();
}
bool UACEPluginSubsystem::IsPluginBarVisible() const
{
    bool Visible=true;if(Settings)Settings->TryGetBoolField(TEXT("_show_plugin_bar"),Visible);return Visible;
}
void UACEPluginSubsystem::SetPluginBarVisible(bool Visible)
{
    if(!Settings)return;
    Settings->SetBoolField(TEXT("_show_plugin_bar"),Visible);SaveSettings();
    if(DesktopDock)DesktopDock->Refresh();
}
bool UACEPluginSubsystem::IsPluginBarMinimized() const
{
    bool Minimized=false;if(Settings)Settings->TryGetBoolField(TEXT("_plugin_bar_minimized"),Minimized);return Minimized;
}
void UACEPluginSubsystem::SetPluginBarMinimized(bool Minimized)
{
    if(!Settings)return;
    Settings->SetBoolField(TEXT("_plugin_bar_minimized"),Minimized);SaveSettings();
    if(DesktopDock)DesktopDock->Refresh();
}
bool UACEPluginSubsystem::IsPluginBarHorizontal() const
{
    bool Horizontal=false;if(Settings)Settings->TryGetBoolField(TEXT("_plugin_bar_horizontal"),Horizontal);return Horizontal;
}
void UACEPluginSubsystem::SetPluginBarHorizontal(bool Horizontal)
{
    if(!Settings)return;
    Settings->SetBoolField(TEXT("_plugin_bar_horizontal"),Horizontal);SaveSettings();
    if(DesktopDock)DesktopDock->Refresh();
}
bool UACEPluginSubsystem::IsPluginBarUnlocked() const
{
    const auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const auto* Manager=Client?Client->GetUIElementManager():nullptr;
    return !Manager||!Manager->IsUiLocked();
}
FVector2D UACEPluginSubsystem::GetPluginWindowPosition(const FString& Id,const FVector2D& Default) const
{
    const TSharedPtr<FJsonObject>* Positions=nullptr;
    const TSharedPtr<FJsonObject>* Point=nullptr;double X,Y;
    if(Settings&&Settings->TryGetObjectField(TEXT("_window_positions"),Positions)&&(*Positions)->TryGetObjectField(Id,Point)
        &&(*Point)->TryGetNumberField(TEXT("x"),X)&&(*Point)->TryGetNumberField(TEXT("y"),Y)&&FMath::IsFinite(X)&&FMath::IsFinite(Y))return FVector2D(X,Y);
    return Default;
}
void UACEPluginSubsystem::SavePluginWindowPosition(const FString& Id,const FVector2D& Position)
{
    if(!Settings||!FMath::IsFinite(Position.X)||!FMath::IsFinite(Position.Y))return;
    const TSharedPtr<FJsonObject>* Existing=nullptr;
    auto Positions=Settings->TryGetObjectField(TEXT("_window_positions"),Existing)?*Existing:MakeShared<FJsonObject>();
    auto Point=MakeShared<FJsonObject>();Point->SetNumberField(TEXT("x"),Position.X);Point->SetNumberField(TEXT("y"),Position.Y);
    Positions->SetObjectField(Id,Point);Settings->SetObjectField(TEXT("_window_positions"),Positions);SaveSettings();
}
