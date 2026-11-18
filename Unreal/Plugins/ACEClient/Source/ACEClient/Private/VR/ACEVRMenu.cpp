#include "VR/ACEVRMenu.h"
#include "Mods/ACEPluginSubsystem.h"
#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "VR/ACEVRUIStyle.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEInventoryRules.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACERetailObjectNames.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Blueprint/WidgetTree.h"
#include "Components/EditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

namespace
{
    TSharedRef<STextBlock> Label(const FString& Value, int32 Size=24)
    {
        return SNew(STextBlock).Text(FText::FromString(Value)).Font(FCoreStyle::GetDefaultFontStyle("Regular",Size))
            .ColorAndOpacity(FLinearColor::White).AutoWrapText(true);
    }
    const FButtonStyle& MenuButton()
    {
        static const FButtonStyle Style=FButtonStyle()
            .SetNormal(FSlateRoundedBoxBrush(FLinearColor(.018f,.025f,.04f),6,ACEVRUIStyle::Gold,1.f))
            .SetHovered(FSlateRoundedBoxBrush(FLinearColor(.13f,.085f,.025f),6,ACEVRUIStyle::TextColor,3.f))
            .SetPressed(FSlateRoundedBoxBrush(FLinearColor(.035f,.10f,.13f),6,FLinearColor::White,3.f))
            .SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.014f,.017f,.02f),6,FLinearColor(.13f,.13f,.13f),1.f));
        return Style;
    }
}

void UACEVRMenu::InitializeMenu(UACEVRComponent* InRig,UACEClientSubsystem* InClient,UACEUIGameplayBinder* InBinder)
{
    Rig=InRig; Client=InClient; Binder=InBinder;
    InspectionSelectionGuid=Client->GetSelectedObject().Guid;
    Client->OnObjectCreated.AddUniqueDynamic(this,&UACEVRMenu::ObjectChanged);
    Client->OnObjectDeleted.AddUniqueDynamic(this,&UACEVRMenu::ObjectDeleted);
    Client->OnSelectionChanged.AddUniqueDynamic(this,&UACEVRMenu::SelectionChanged);
    Client->OnVitalsUpdated.AddUniqueDynamic(this,&UACEVRMenu::VitalsChanged);
    Client->OnAppraisal.AddUniqueDynamic(this,&UACEVRMenu::Appraised);
    Client->OnFellowshipChanged.AddUniqueDynamic(this,&UACEVRMenu::Changed);
    Client->OnAllegianceChanged.AddUniqueDynamic(this,&UACEVRMenu::Changed);
    Client->OnContractsChanged.AddUniqueDynamic(this,&UACEVRMenu::Changed);
    Client->OnTradeStateChanged.AddUniqueDynamic(this,&UACEVRMenu::ContextChanged);
    Client->OnExternalContainerClosed.AddUniqueDynamic(this,&UACEVRMenu::ContextChanged);
    MenuOwnerGuid=Pack=Client->GetPlayerGuid(); bDirty=true;
}
void UACEVRMenu::NativeDestruct()
{
    if(Client)
    {
        Client->OnObjectCreated.RemoveDynamic(this,&UACEVRMenu::ObjectChanged);
        Client->OnObjectDeleted.RemoveDynamic(this,&UACEVRMenu::ObjectDeleted);
        Client->OnSelectionChanged.RemoveDynamic(this,&UACEVRMenu::SelectionChanged);
        Client->OnVitalsUpdated.RemoveDynamic(this,&UACEVRMenu::VitalsChanged);
        Client->OnAppraisal.RemoveDynamic(this,&UACEVRMenu::Appraised);
        Client->OnFellowshipChanged.RemoveDynamic(this,&UACEVRMenu::Changed);
        Client->OnAllegianceChanged.RemoveDynamic(this,&UACEVRMenu::Changed);
        Client->OnContractsChanged.RemoveDynamic(this,&UACEVRMenu::Changed);
        Client->OnTradeStateChanged.RemoveDynamic(this,&UACEVRMenu::ContextChanged);
        Client->OnExternalContainerClosed.RemoveDynamic(this,&UACEVRMenu::ContextChanged);
    }
    Super::NativeDestruct();
}
void UACEVRMenu::ObjectChanged(const FACEWorldObject& O)
{ if(Client && (Client->IsOwnedInventoryItem(O) || O.WielderId==Client->GetPlayerGuid() || O.Guid==Selected || (Page=="Fellowship" && O.bIsPlayer)
    || (Client->GetOpenExternalContainerGuid() && O.ContainerId==Client->GetOpenExternalContainerGuid())))bDirty=true; }
void UACEVRMenu::ObjectDeleted(int32 Guid){bDirty=true;if(Guid==DragItem)CancelItemPointer();if(Guid==Selected)Selected=0;if(Guid==UseSource)UseSource=0;if(Guid==Pack)Pack=Client->GetPlayerGuid();}
void UACEVRMenu::SelectionChanged(const FACESelectedObject& S)
{
    const bool TargetChanged=InspectionSelectionGuid!=S.Guid;
    InspectionSelectionGuid=S.Guid;
    FACEWorldObject Item;
    const bool Found=Client && Client->GetWorldObject(S.Guid,Item);
    Selected=Found && ((Page=="Inventory" && Item.ContainerId==Pack) || (Page=="Equipment" && Item.WielderId==Client->GetPlayerGuid())
        || Page=="Salvage" || Page=="Loot" || Page=="Vendor" || Page=="Trade" || (Page=="Hotbars" && Client->IsOwnedInventoryItem(Item)))?S.Guid:0;
    if(bInspectionOpen && (!InspectSpell || (TargetChanged && S.bValid && S.Guid)))
    {
        if(!S.bValid || !S.Guid) {bInspectionOpen=false;InspectItem=0;InspectionAppraisal={};}
        else if(InspectItem!=S.Guid || InspectSpell)
        {
            InspectItem=S.Guid;InspectSpell=0;InspectionAppraisal={};
            // Desktop's selection listener may already have requested this
            // appraisal. Share it when both presentations are open.
            const auto Exam=Binder && Binder->Manager?Binder->Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field")):nullptr;
            if(!Exam || !Exam->bVisible || Client->GetIdentifyRequestGuid()!=S.Guid)
                Client->SendIdentifyObject(S.Guid);
        }
    }
    Confirmation.Reset();bDirty=true;
}
void UACEVRMenu::VitalsChanged(const FACEPlayerVitals&){if(Page=="Character")bDirty=true;}
void UACEVRMenu::Appraised(const FACEAppraisalInfo& A){if(A.ObjectGuid==InspectItem)InspectionAppraisal=A;if(A.ObjectGuid==Selected || A.ObjectGuid==InspectItem)bDirty=true;}
void UACEVRMenu::Changed(){Confirmation.Reset();bDirty=true;}
void UACEVRMenu::ContextChanged(int32){bDirty=true;}
void UACEVRMenu::OpenPage(FName InPage)
{
    if(Rig)Rig->DismissTextEntry();
    if(TextEntry)TextEntry->SetVisibility(ESlateVisibility::Collapsed);
    TextEntry=nullptr;
    const bool KeepUseTarget=UseSource && (InPage=="Inventory" || InPage=="Equipment");
    if(Page=="Fellowship" && InPage!=Page && Client)Client->SendFellowshipUpdateRequest(false);
    if(InPage=="Inspect"){CancelItemPointer();Selected=0;}
    else ClearItemSelection();
    if(!KeepUseTarget)UseSource=0;
    Spell=0;PendingLearnedSpellId=0;FellowSelection=0;bAssignShortcut=false;
    Page=InPage;PageIndex=0;Confirmation.Reset();bDirty=true;
    if(Client && Page=="Inspect")
    {
        Selected=Client->GetSelectedObject().Guid;
        if(Selected)InspectSelection();
        else {bInspectionOpen=false;Confirmation=TEXT("Tap an object in the world, then Inspect.");}
    }
    if(ContentScroll)ContentScroll->ScrollToStart();
    if(Client && Page=="Fellowship")Client->SendFellowshipUpdateRequest(true);
    if(Client && Page=="Allegiance")Client->SendAllegianceUpdateRequest(true);
}
void UACEVRMenu::EntryChanged(const FText& Value)
{
    if(Page=="Spellbook" && Search!=Value.ToString()){Search=Value.ToString();PageIndex=0;bDirty=true;}
    else if(Page=="Fellowship")FellowName=Value.ToString();
}
TSharedRef<SWidget> UACEVRMenu::Entry(const FString& Hint,const FString& Value)
{
    if(!TextEntry)
    {
        TextEntry=NewObject<UEditableTextBox>(this);
        auto Style=TextEntry->GetWidgetStyle();Style.SetFont(FCoreStyle::GetDefaultFontStyle("Regular",28));TextEntry->SetWidgetStyle(Style);TextEntry->SetForegroundColor(FLinearColor::Black);
        TextEntry->OnTextChanged.AddDynamic(this,&UACEVRMenu::EntryChanged);
    }
    TextEntry->SetVisibility(ESlateVisibility::Visible);
    TextEntry->SetHintText(FText::FromString(Hint));TextEntry->SetText(FText::FromString(Value));
    return SNew(SBox).MinDesiredHeight(60)[TextEntry->TakeWidget()];
}
TSharedRef<SWidget> UACEVRMenu::Button(const FString& Text,TFunction<void()> Click,bool Enabled)
{
    auto Caption=Label(Text);Caption->SetAutoWrapText(false);Caption->SetJustification(ETextJustify::Center);
    return SNew(SButton).Tag(FName(*Text)).ButtonStyle(&MenuButton()).IsFocusable(false).IsEnabled(Enabled).ContentPadding(FMargin(12,10))
        .TouchMethod(EButtonTouchMethod::PreciseTap).HAlign(HAlign_Center).VAlign(VAlign_Center)
        .OnClicked_Lambda([Click=MoveTemp(Click)](){Click();return FReply::Handled();})[Caption];
}
TSharedRef<SWidget> UACEVRMenu::RebuildWidget()
{
    if(bTouchMode)
    {
        bDirty=true;
        return ACEVRUIStyle::Frame(SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(HoverLabel,STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold",24)).ColorAndOpacity(ACEVRUIStyle::TextColor)]
            +SVerticalBox::Slot().FillHeight(1)[SAssignNew(ContentScroll,SScrollBox)+SScrollBox::Slot()[SAssignNew(Body,SVerticalBox)]],8.f,true);
    }
    auto Nav=SNew(SVerticalBox);
    const TCHAR* Names[]={TEXT("Inventory"),TEXT("Spellbook"),TEXT("Fellowship"),TEXT("Character"),TEXT("Vendor"),TEXT("Loot"),TEXT("Trade"),TEXT("Salvage"),TEXT("More")};
    for(const auto* Name:Names)Nav->AddSlot().AutoHeight().Padding(2)[Button(Name,[this,Name](){OpenPage(Name);})];
    Nav->AddSlot().AutoHeight().Padding(2)[Button(TEXT("Close"),[this](){if(Rig)Rig->ToggleInventory();})];
    auto Content=SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(8)[SNew(SBox).HeightOverride(64)[SAssignNew(HoverLabel,STextBlock).Text(FText::FromString(TEXT("Backpack")))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold",28)).ColorAndOpacity(ACEVRUIStyle::TextColor).AutoWrapText(true)]]
        +SVerticalBox::Slot().FillHeight(1)[SAssignNew(ContentScroll,SScrollBox)+SScrollBox::Slot()[SAssignNew(Body,SVerticalBox)]];
    bDirty=true;
    return ACEVRUIStyle::Frame(SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(180)[Nav]]
        +SHorizontalBox::Slot().FillWidth(1).Padding(12,0)[Content],12.f,true);
}
void UACEVRMenu::ReleaseSlateResources(bool Children)
{ Super::ReleaseSlateResources(Children);Body.Reset();HoverLabel.Reset();ContentScroll.Reset();InspectionWidget.Reset();InspectionBody.Reset();IconBrushes.Reset();IconTextures.Reset();PendingIcons.Reset(); }
void UACEVRMenu::RefreshIfDirty()
{
    RefreshPendingIcons();
    if(Client && MenuOwnerGuid!=Client->GetPlayerGuid())
    {
        MenuOwnerGuid=Pack=Client->GetPlayerGuid();Selected=Spell=UseSource=PageIndex=0;
        Confirmation.Reset();Search.Reset();PendingLearnedSpellId=0;bAssignShortcut=false;bInspectionOpen=false;InspectItem=InspectSpell=0;bDirty=true;
    }
	if(Client && Client->GetSession() && Page=="Spellbook" && SpellRevision!=Client->GetSession()->GetSpellDataRevision())
	{SpellRevision=Client->GetSession()->GetSpellDataRevision();bDirty=true;}
	if(Client && Client->GetSession() && InventoryRevision!=Client->GetSession()->GetInventoryDataRevision())
	{InventoryRevision=Client->GetSession()->GetInventoryDataRevision();if(Page!="Plugins")bDirty=true;}
    if(!bDirty || !Body || !Client || !Binder || DragItem || DragSpell)return;
    // Inventory/server updates can arrive between trigger down and release.
    // Keep action buttons and sliders alive until the owning pointer finishes.
    if(Rig && (Rig->bLeftPointerPressed || Rig->bRightPointerPressed))return;
    bDirty=false;++RefreshCount;
    Body->ClearChildren();ItemDestinations.Reset();MenuTargets.Reset();SpellDestinations.Reset();IconBrushes.Reset();IconTextures.Reset();PendingIcons.Reset();
    if(HoverLabel)HoverLabel->SetText(FText::FromString(Page.ToString()));
    if(Page=="Inventory" || Page=="Equipment")BuildInventory();
    else if(Page=="Salvage")BuildSalvage();
    else if(Page=="Spellbook")BuildSpells();
    else if(Page=="Fellowship")BuildFellowship();
    else if(Page=="Character")BuildCharacter();
    else if(Page=="Vendor")BuildVendor();
    else if(Page=="Loot")BuildLoot();
    else if(Page=="Trade")BuildTrade();
    else if(Page=="Give")
    {
        Body->AddSlot().AutoHeight()[Label(TEXT("Choose a recipient"),28)];
        for(const auto& Target:Client->GetWorldObjects())if(Target.IsGiveOrCreatureTarget() && Target.Guid!=Client->GetPlayerGuid() && Target.IsSelectableWorldObject())
            Body->AddSlot().AutoHeight().Padding(4)[Button(ACERetailObjectNames::Name(Target),[this,Id=Target.Guid]()
            {Client->SendGiveObjectRequest(Id,Selected,Binder->GetSelectedItemAmount(Selected));OpenPage("Inventory");})];
    }
    else if(Page=="Allegiance")BuildAllegiance();
    else if(Page=="Plugins")
    {
        Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Manage installed plugins"),[this](){OpenPage("PluginManager");})];
        Body->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(610)[GetGameInstance()->GetSubsystem<UACEPluginSubsystem>()->MakePanel(TEXT("ucm"))]];
    }
    else if(Page=="PluginManager")Body->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(610)[GetGameInstance()->GetSubsystem<UACEPluginSubsystem>()->MakePanel()]];
    else if(Page=="Options")BuildOptions();
    else if(Page=="Hotbars")BuildShortcuts();
    else if(Page!="Inspect")BuildMore();
    if(!Confirmation.IsEmpty())Body->AddSlot().AutoHeight().Padding(4)[Label(Confirmation)];
    if(bInspectionOpen)BuildInspection();
    if(bTouchMode && bInspectionOpen)
    {
        Body->AddSlot().AutoHeight().Padding(8)[SNew(SBox).HeightOverride(360)[GetInspectionWidget()]];
        Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Close inspection"),[this](){bInspectionOpen=false;bDirty=true;})];
    }
}
void UACEVRMenu::SelectItem(int32 Guid)
{
    if(UseSource)
    {
        FACEWorldObject Source,Target;
        if(Client->GetWorldObject(UseSource,Source) && Client->GetWorldObject(Guid,Target)
            && ACEInventoryRules::IsTargetCompatible(Source,Target,Client->GetPlayerGuid(),Client->IsOwnedInventoryItem(Source),Client->IsOwnedInventoryItem(Target),Client->GetTradeSelfItems().Contains(Guid)))
        {Client->SendUseWithTarget(UseSource,Guid);UseSource=0;Confirmation.Reset();}
        else Confirmation=TEXT("Choose a compatible item. Use on item is still active.");
        bDirty=true;return;
    }
    Selected=Guid;Binder->SelectInventoryGuid(Guid);
    if(!bInspectionOpen || InspectItem!=Guid)Client->SendIdentifyObject(Guid);
    bDirty=true;
}
void UACEVRMenu::AddQuantity(int32 Maximum)
{
    if(Maximum<=1)return;
    Binder->SelectedStackAmount=FMath::Clamp(Binder->SelectedStackAmount,1,Maximum);
    auto Quantity=SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("Quantity: %d / %d"),Binder->SelectedStackAmount,Maximum)))
        .Font(FCoreStyle::GetDefaultFontStyle("Regular",24)).ColorAndOpacity(FLinearColor::White);
    Body->AddSlot().AutoHeight()[Quantity];
    Body->AddSlot().AutoHeight().Padding(4,12)[SNew(SBox).MinDesiredHeight(bTouchMode?56:18)[SNew(SSlider).MinValue(1).MaxValue(Maximum).StepSize(1)
        .Value(Binder->SelectedStackAmount).OnValueChanged_Lambda([this,Quantity,Maximum](float Value)
        {Binder->SelectedStackAmount=FMath::Clamp(FMath::RoundToInt(Value),1,Maximum);Quantity->SetText(FText::FromString(FString::Printf(TEXT("Quantity: %d / %d"),Binder->SelectedStackAmount,Maximum)));})]];
}
void UACEVRMenu::Execute(FName Action)
{
    if(!Client || !Binder)return;
    FACEWorldObject Item;const bool Valid=Client->GetWorldObject(Selected,Item);
    const int32 Amount=Valid?Binder->GetSelectedItemAmount(Selected):0;
    if(Action=="Use / Equip" && Valid)Binder->UseInventoryItem(Selected);
    else if(Action=="Inspect")InspectSelection();
    else if(Action=="Move here" && Valid)Binder->MoveInventoryAmount(Selected,Pack,Client->GetPackItems(Pack).Num(),Amount);
    else if(Action=="Sell" && Valid)Binder->AddInventoryGuidToVendorSellCart(Selected,Amount);
    else if(Action=="Trade" && Valid)Binder->AddInventoryGuidToTrade(Selected,Amount);
    else if(Action=="Drop" && Valid)
    {
        const FString Prompt=FString::Printf(TEXT("Drop %d %s? Press Drop again to confirm."),Amount,*ACERetailObjectNames::Name(Item));
        if(Confirmation==Prompt)
        {
            if(!Item.CanDropToWorld())Confirmation=TEXT("You cannot drop that item.");
            else {DropItem(Selected,Amount);Confirmation.Reset();}
        }
        else Confirmation=Prompt;
    }
    else if(Action=="Give" && Valid)
    {
        Page="Give";
    }
    bDirty=true;
}

void UACEVRComponent::OpenPluginManager()
{
    bUseDesktopMenu=false;
    if(!bInventoryOpen)ToggleInventory();
    if(GameplayMenu)GameplayMenu->OpenPage("Plugins");
}
