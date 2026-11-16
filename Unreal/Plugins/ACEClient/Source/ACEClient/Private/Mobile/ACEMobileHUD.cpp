#include "Mobile/ACEMobileWidget.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "UI/ACEUIGameplayBinder.h"
#include "Framework/Application/SlateApplication.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Engine/GameInstance.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

namespace
{
constexpr float PanelTop=100;
const FLinearColor MobileHUDInk(.018f,.029f,.043f,.9f),MobileHUDEdge(.45f,.56f,.60f,.65f);
TSharedRef<SWidget> HUDButton(const FString& Label,FName Tag,TFunction<void()> Action)
{
    static const FButtonStyle Style=FButtonStyle().SetNormal(FSlateRoundedBoxBrush(MobileHUDInk,8,MobileHUDEdge,1))
        .SetHovered(FSlateRoundedBoxBrush(FLinearColor(.04f,.17f,.20f),8,MobileHUDEdge,1))
        .SetPressed(FSlateRoundedBoxBrush(FLinearColor(.04f,.24f,.25f),8,MobileHUDEdge,1));
    return SNew(SBox).HeightOverride(36)[SNew(SButton).Tag(Tag).ButtonStyle(&Style).IsFocusable(false)
        .TouchMethod(EButtonTouchMethod::PreciseTap).ContentPadding(6).OnClicked_Lambda([Action](){Action();return FReply::Handled();})
        [SNew(STextBlock).Text(FText::FromString(Label)).ColorAndOpacity(FLinearColor::White).Font(FCoreStyle::GetDefaultFontStyle("Bold",14)).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]];
}
// Empty panel space must consume touches too, instead of selecting objects or
// starting camera movement behind a nonmodal readout.
class SHUDPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SHUDPanel){} SLATE_DEFAULT_SLOT(FArguments,Content) SLATE_END_ARGS()
    void Construct(const FArguments& Args){ChildSlot[Args._Content.Widget];SetClipping(EWidgetClipping::ClipToBounds);}
    FReply OnTouchStarted(const FGeometry&,const FPointerEvent&)override{return FReply::Handled();}
    FReply OnTouchMoved(const FGeometry&,const FPointerEvent&)override{return FReply::Handled();}
    FReply OnTouchEnded(const FGeometry&,const FPointerEvent&)override{return FReply::Handled();}
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&)override{return FReply::Handled();}
};
}

void UACEMobileWidget::FitControls()
{
    Layout=Settings?Settings->Controls:Layout;
    ACEMobile::FitLayout(Layout,ViewSize,ControlsTop);
    FittedLayout=Layout;
}
bool UACEMobileWidget::WantsHUDPlugin(const FString& Id)const
{
    if(!Settings || Settings->HiddenHUDPlugins.Contains(Id))return false;
    if(Settings->ExtraHUDPlugins.Contains(Id))return true;
    if(Id==TEXT("waypoint") || Id==TEXT("ucmmicro") || Id==TEXT("actiondeck") || UACEPluginSubsystem::IsLedger(Id))return true;
    if(auto* Host=GetGameInstance()->GetSubsystem<UACEPluginSubsystem>())
        for(const auto& P:Host->Plugins)if(P->Id==Id)return P->Companion && !P->PanelRows.IsEmpty();
    return false;
}
bool UACEMobileWidget::IsHUDPluginVisible(const FString& Id)const
{
    if(!Client || Client->GetSessionState()!=EACESessionState::InWorld || !Panel.IsNone() || bWheel || bEditing)return false;
    return HUDPluginIds.Contains(Id) || (Id==TEXT("waypoint") && (HUDPluginIds.Contains(TEXT("waypoint.arrow")) || HUDPluginIds.Contains(TEXT("waypoint.dungeon"))));
}
void UACEMobileWidget::ToggleHUDPlugin(const FString& Id)
{
    if(WantsHUDPlugin(Id)){Settings->ExtraHUDPlugins.Remove(Id);Settings->HiddenHUDPlugins.AddUnique(Id);}
    else {Settings->HiddenHUDPlugins.Remove(Id);Settings->ExtraHUDPlugins.AddUnique(Id);}
    Settings->SaveConfig();HUDSignature.Reset();RefreshHUDPanels();
}
void UACEMobileWidget::SetPinnedChat(bool Pinned)
{
    if(!Pinned && PinnedChatEntry && PinnedChatEntry->HasKeyboardFocus())FSlateApplication::Get().ClearKeyboardFocus();
    Settings->bPinnedChat=Pinned;Settings->SaveConfig();HUDSignature.Reset();RefreshHUDPanels();
}
void UACEMobileWidget::SendPinnedChat()
{
    if(auto* Binder=PC?PC->GetActionDeckBinder():nullptr)
        if(!PinnedChatDraft.IsEmpty() && Binder->TrySendChatFromEntry(&PinnedChatDraft))
        {PinnedChatDraft.Reset();if(PinnedChatEntry)PinnedChatEntry->SetText(FText::GetEmpty());}
    FSlateApplication::Get().ClearKeyboardFocus();CancelTouches();
}
TSharedRef<SWidget> UACEMobileWidget::MakePinnedChat()
{
    auto Body=SNew(SVerticalBox);
    Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[HUDButton(TEXT("Chat"),"MobilePinnedChatOpen",[this](){OpenPanel("Chat");})]
        +SHorizontalBox::Slot().AutoWidth()[HUDButton(TEXT("Unpin"),"MobilePinnedChatUnpin",[this](){SetPinnedChat(false);})]];
    Body->AddSlot().FillHeight(1).Padding(4)[SAssignNew(PinnedChatScroll,SScrollBox).ScrollBarThickness(FVector2D(8,8))
        +SScrollBox::Slot()[SAssignNew(PinnedChatLines,SVerticalBox)]];
    Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,4,0)[SNew(SBox).HeightOverride(36)[SAssignNew(PinnedChatEntry,SEditableTextBox)
            .Tag(TEXT("MobilePinnedChatEntry")).ForegroundColor(FLinearColor::White).Font(FCoreStyle::GetDefaultFontStyle("Regular",16)).Text(FText::FromString(PinnedChatDraft))
            .HintText(FText::FromString(TEXT("Message / command"))).OnBeginTextEdit_Lambda([this](const FText&){CancelTouches();})
            .OnTextChanged_Lambda([this](const FText& T){PinnedChatDraft=T.ToString();})
            .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter)SendPinnedChat();})]]
        +SHorizontalBox::Slot().AutoWidth()[HUDButton(TEXT("Send"),"MobilePinnedChatSend",[this](){SendPinnedChat();})]];
    bChatDirty=true;return Body;
}
void UACEMobileWidget::RefreshHUDPanels()
{
    if(!HUDHost || !Settings || !Client || Client->GetSessionState()!=EACESessionState::InWorld)return;
    auto* Host=GetGameInstance()->GetSubsystem<UACEPluginSubsystem>();if(!Host)return;
    TArray<FString> Ids;
    float LargestControl=168;
    for(const auto& C:Settings->Controls)if(C.bVisible)LargestControl=FMath::Max(LargestControl,float(ACEMobile::ControlRect(C,ViewSize).GetSize().Y));
    const float PanelHeight=FMath::Min(200.f,float(ViewSize.Y)-48-LargestControl-PanelTop-12);
    FString Signature=FString::Printf(TEXT("%.0f,%.0f:%.0f:%d"),ViewSize.X,ViewSize.Y,PanelHeight,Settings->bPinnedChat);
    for(const auto& Id:Host->PluginDisplayOrder())for(const auto& P:Host->Plugins)
    {
        if(P->Id!=Id || !P->Enabled || !WantsHUDPlugin(Id))continue;
        if(Id==TEXT("waypoint"))
        {
            if(Host->ShouldShowWaypointArrow())Ids.Add(TEXT("waypoint.arrow"));
            if(Host->WaypointOption(TEXT("dungeon_overlay")))Ids.Add(TEXT("waypoint.dungeon"));
        }
        else Ids.Add(Id);
        // Reloading a plugin must replace widgets holding the old plugin object.
        Signature+=FString::Printf(TEXT("|%s:%p"),*Id,P.Get());break;
    }
    Signature+=FString::Join(Ids,TEXT("|"));
    if(Signature==HUDSignature)return;
    const bool HasPanels=Settings->bPinnedChat || !Ids.IsEmpty();
    const float NewTop=HasPanels?PanelTop+PanelHeight+12:100;
    if(NewTop!=ControlsTop){CancelTouches();ControlsTop=NewTop;FitControls();}
    HUDSignature=Signature;HUDPluginIds=Ids;PinnedChatLines.Reset();PinnedChatScroll.Reset();PinnedChatEntry.Reset();
    auto Panels=SNew(SScrollBox).Orientation(Orient_Horizontal).ScrollBarThickness(FVector2D(8,8)).ScrollBarAlwaysVisible(true);
    auto Add=[&](float Width,TSharedRef<SWidget> Content)
    {
        Panels->AddSlot().Padding(0,0,8,0)[SNew(SBox).WidthOverride(Width).HeightOverride(PanelHeight-12)
            [SNew(SHUDPanel)[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(MobileHUDInk).Padding(4)[Content]]]];
    };
    if(Settings->bPinnedChat)Add(320,MakePinnedChat());
    for(const FString& Id:Ids)
    {
        const bool Arrow=Id==TEXT("waypoint.arrow"),Map=Id==TEXT("waypoint.dungeon");
        const FString PluginId=Arrow||Map?TEXT("waypoint"):Id;
        FString Title=Id;
        for(const auto& P:Host->Plugins)if(P->Id==PluginId){Title=P->DisplayTitle();break;}
        if(Map)Title+=TEXT(" map");
        auto Body=SNew(SVerticalBox);
        Body->AddSlot().AutoHeight()[HUDButton(Title,FName(*(TEXT("MobileHUD:")+Id)),[this,PluginId](){OpenPanel(FName(*(TEXT("Plugin:")+PluginId)));})];
        if(Arrow)
            Body->AddSlot().FillHeight(1)[SNew(SScaleBox).Stretch(EStretch::ScaleToFit)[SNew(SBox).WidthOverride(260).HeightOverride(190)[Host->MakeWaypointPanel(false,true)]]];
        else if(Map)Body->AddSlot().FillHeight(1)[Host->MakeWaypointDungeonOverlay()];
        else
        {
            // Preserve the plugin's own widgets and live updates. A bounded
            // viewport scrolls larger panels rather than shrinking their text.
            const float Height=UACEPluginSubsystem::IsLedger(Id)?Host->LedgerSize(Id).Y:300;
            Body->AddSlot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()
                [SNew(SBox).MinDesiredHeight(Height)[Host->MakePanel(Id)]]];
        }
        Add(Arrow?210:Id==TEXT("supplyledger")?200:Id==TEXT("actiondeck")?420:310,Body);
    }
    HUDHost->SetContent(SNew(SScaleBox).Stretch(EStretch::ScaleToFit).Visibility(EVisibility::SelfHitTestInvisible)
        [SNew(SBox).WidthOverride(ViewSize.X).HeightOverride(ViewSize.Y).Visibility(EVisibility::SelfHitTestInvisible)
            [SNew(SOverlay).Visibility(EVisibility::SelfHitTestInvisible)
                +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(80,PanelTop,0,0)
                    [SNew(SBox).WidthOverride(ViewSize.X-88).HeightOverride(HasPanels?PanelHeight:0).Visibility(HasPanels?EVisibility::Visible:EVisibility::Collapsed)[Panels]]]]);
    RefreshChat();
}
