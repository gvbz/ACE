#include "VR/ACEVRMenu.h"
#include "VR/ACEVRComponent.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEPlayerController.h"
#include "ACECharacterOptions.h"
#include "ACEVendorPricing.h"
#include "Components/EditableTextBox.h"
#include "../UI/ACEAppraisalFormatting.h"
#include "ACESession.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACERetailObjectNames.h"
#include "Engine/GameInstance.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"

namespace
{
    TSharedRef<SWidget> PageText(const FString& Value,int Size=24)
    {return SNew(STextBlock).Text(FText::FromString(Value)).Font(FCoreStyle::GetDefaultFontStyle("Regular",Size)).ColorAndOpacity(FLinearColor::White).AutoWrapText(true);}
}
void UACEVRMenu::BuildFellowship()
{
    const auto Fellow=Client->GetFellowship();
    const int32 Self=Client->GetPlayerGuid();
    const bool Leader=Fellow.bValid && Fellow.LeaderGuid==Self;
    Body->AddSlot().AutoHeight()[PageText(Fellow.bValid?Fellow.Name:TEXT("Create a fellowship"),30)];
    auto Preferences=[this]()
    {
        for(int32 Id:{0x02,0x12,0x0F,0x11})if(const auto* Option=ACECharacterOptions::Find(Id))
            Body->AddSlot().AutoHeight().Padding(4)[SNew(SCheckBox)
                .IsChecked_Lambda([this,Id](){return Client->IsCharacterOptionSet(Id)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this,Id](ECheckBoxState State){Client->SendSetSingleCharacterOption(Id,State==ECheckBoxState::Checked);bDirty=true;})
                [PageText(Option->Label)]];
    };
    if(!Fellow.bValid)
    {
        Body->AddSlot().AutoHeight().Padding(4)[Entry(TEXT("Fellowship name"),FellowName)];
        Body->AddSlot().AutoHeight().Padding(4)[Button(bShareFellowXP?TEXT("Share XP: On"):TEXT("Share XP: Off"),[this](){bShareFellowXP=!bShareFellowXP;bDirty=true;})];
        Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Create fellowship"),[this]()
        {
            const FString Name=TextEntry?TextEntry->GetText().ToString().TrimStartAndEnd():FellowName.TrimStartAndEnd();
            if(Name.IsEmpty()){Confirmation=TEXT("Enter a fellowship name first.");bDirty=true;return;}
            FellowName=Name;if(Rig)Rig->DismissTextEntry();Client->SendFellowshipCreate(Name,bShareFellowXP);
            Confirmation=TEXT("Creating fellowship...");bDirty=true;
        })];
        Preferences();return;
    }
    Body->AddSlot().AutoHeight().Padding(4)[PageText(Fellow.bOpen?TEXT("Open — members can recruit"):TEXT("Closed — only the leader can recruit"))];
    Body->AddSlot().AutoHeight().Padding(4)[PageText(TEXT("Members — select one to manage"),28)];
    for(const auto& Member:Fellow.Members)
        Body->AddSlot().AutoHeight().Padding(3)[Tab(FString::Printf(TEXT("%s%s   Level %d\nHealth %d/%d   Stamina %d/%d   Mana %d/%d"),
            *Member.Name,Member.Guid==Fellow.LeaderGuid?TEXT(" (Leader)"):TEXT(""),Member.Level,Member.HealthCur,Member.HealthMax,Member.StaminaCur,Member.StaminaMax,Member.ManaCur,Member.ManaMax),
            FellowSelection==Member.Guid,[this,Id=Member.Guid](){FellowSelection=Id;Client->SelectObject(Id);Confirmation.Reset();bDirty=true;})];
    const bool OtherMember=FellowSelection!=Self && Fellow.Members.ContainsByPredicate([this](const auto& M){return M.Guid==FellowSelection;});
    auto Manage=SNew(SWrapBox).UseAllottedSize(true);
    Manage->AddSlot().Padding(2)[Button(TEXT("Dismiss member"),[this](){Client->SendFellowshipDismiss(FellowSelection);},Leader && OtherMember)];
    Manage->AddSlot().Padding(2)[Button(TEXT("Make leader"),[this](){Client->SendFellowshipAssignNewLeader(FellowSelection);},Leader && OtherMember)];
    Manage->AddSlot().Padding(2)[Button(Fellow.bOpen?TEXT("Close fellowship"):TEXT("Open fellowship"),[this,Fellow](){Client->SendFellowshipChangeOpenness(!Fellow.bOpen);},Leader)];
    Body->AddSlot().AutoHeight()[Manage];
    Body->AddSlot().AutoHeight().Padding(4)[PageText(TEXT("Recruit a nearby player"),28)];
    bool Any=false;
    for(const auto& Target:Client->GetWorldObjects())
    {
        if(!Target.bIsPlayer || Target.Guid==Self || !Target.IsSelectableWorldObject()
            || Fellow.Members.ContainsByPredicate([&](const auto& M){return M.Guid==Target.Guid;}))continue;
        Any=true;
        Body->AddSlot().AutoHeight().Padding(3)[Button(TEXT("Recruit ")+Target.Name,[this,Id=Target.Guid](){Client->SendFellowshipRecruit(Id);},(Leader || Fellow.bOpen) && Fellow.Members.Num()<9)];
    }
    if(!Any)Body->AddSlot().AutoHeight()[PageText(TEXT("No eligible players nearby."))];
    Body->AddSlot().AutoHeight().Padding(6)[Button(TEXT("Leave fellowship"),[this](){Client->SendFellowshipQuit(false);})];
    Body->AddSlot().AutoHeight().Padding(6)[Button(TEXT("Disband fellowship"),[this]()
    {if(Confirmation==TEXT("Press Disband again to confirm.")){Client->SendFellowshipQuit(true);Confirmation.Reset();}else Confirmation=TEXT("Press Disband again to confirm.");bDirty=true;},Leader)];
    Preferences();
}
void UACEVRMenu::BuildCharacter()
{
    const auto V=Client->GetPlayerVitals();
    auto* Dat=Client->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();if(!Dat)return;
    Body->AddSlot().AutoHeight().Padding(4)[PageText(FString::Printf(TEXT("Level %d    Available XP: %lld    Skill credits: %d"),V.Level,V.AvailableExperience,V.AvailableSkillCredits),28)];
    Body->AddSlot().AutoHeight().Padding(4)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[Tab(TEXT("Attributes"),!bSkillsPage,[this](){bSkillsPage=false;bDirty=true;})]
        +SHorizontalBox::Slot().FillWidth(1)[Tab(TEXT("Skills"),bSkillsPage,[this](){bSkillsPage=true;bDirty=true;})]];
    auto Row=[&](const FString& Name,uint32 Did,int32 Value,int64 One,int64 Ten,TFunction<void(int32)> Raise)
    {
        Body->AddSlot().AutoHeight().Padding(4,7)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(4).VAlign(VAlign_Center)[Icon(Did)]
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[PageText(Name)]
            +SHorizontalBox::Slot().AutoWidth().Padding(12,0).VAlign(VAlign_Center)[PageText(FString::FromInt(Value))]
            +SHorizontalBox::Slot().AutoWidth().Padding(3)[Button(FString::Printf(TEXT("+1\n%lld XP"),One),[Raise](){Raise(1);},One>0 && V.AvailableExperience>=One)]
            +SHorizontalBox::Slot().AutoWidth().Padding(3)[Button(FString::Printf(TEXT("+10\n%lld XP"),Ten),[Raise](){Raise(10);},Ten>0 && V.AvailableExperience>=Ten)]];
    };
    if(!bSkillsPage)
    {
        const TCHAR* Names[]={TEXT("Strength"),TEXT("Endurance"),TEXT("Coordination"),TEXT("Quickness"),TEXT("Focus"),TEXT("Self"),TEXT("Health"),TEXT("Stamina"),TEXT("Mana")};
        const int32 Ids[]={1,2,4,3,5,6};
        const uint32 Icons[]={0x060002C8,0x060002C4,0x060002C9,0x060002C6,0x060002C5,0x060002C7,0x06004C3B,0x06004C3C,0x06004C3D};
        for(int32 I=0;I<9;++I)
        {
            int64 One=0,Ten=0;
            if(I<6){Dat->TryGetAttributeXpToNextRank(V.GetAttributeXpSpent(Ids[I]),One);Dat->TryGetAttributeXpToNextRank(V.GetAttributeXpSpent(Ids[I]),Ten,nullptr,10);}
            else{const int32 Spent=I==6?V.HealthXpSpent:I==7?V.StaminaXpSpent:V.ManaXpSpent;Dat->TryGetVitalXpToNextRank(Spent,One);Dat->TryGetVitalXpToNextRank(Spent,Ten,nullptr,10);}
            Row(Names[I],Icons[I],I<6?V.GetAttributeCurrent(Ids[I]):I==6?V.MaxHealth:I==7?V.MaxStamina:V.MaxMana,
                One,Ten,[this,I](int32 Count){Binder->SelectedAttributeRow=I;Binder->ActiveSkillTab="AttributePage";Binder->RaiseSelectedStat(Count);});
        }
    }
    else
    {
        // gmSkillUI::AddSortedSkill sorts names *within* advancement sections.
        // DAT min_level distinguishes untrained-but-usable from unusable.
        auto Skills=V.Skills;Skills.Sort([](const auto& A,const auto& B){return A.Name<B.Name;});
        const TCHAR* Groups[]={TEXT("Specialized"),TEXT("Trained"),TEXT("Untrained"),TEXT("Unusable")};
        auto GroupFor=[Dat](const FACESkillInfo& S) -> int32
        {
            if(S.SkillId<=0 || S.AdvancementClass==0)return INDEX_NONE;
            if(S.AdvancementClass>=3)return 0;
            if(S.AdvancementClass==2)return 1;
            uint32 Minimum=0;Dat->TryGetSkillMinLevel(S.SkillId,Minimum);return Minimum<=1?2:3;
        };
        for(int32 Group=0;Group<4;++Group)
        {
          if(!Skills.ContainsByPredicate([&](const auto& S){return GroupFor(S)==Group;}))continue;
          auto Heading=SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
              .BorderBackgroundColor(FLinearColor(.12f,.085f,.025f)).Padding(8)[PageText(Groups[Group],28)];
          Heading->SetTag(FName(Groups[Group]));Body->AddSlot().AutoHeight().Padding(4,8)[Heading];
          for(const auto& Skill:Skills)
          {
            if(GroupFor(Skill)!=Group)continue;
            FString Name;uint32 Did=0;Dat->TryGetSkillInfo(Skill.SkillId,Name,Did);
            if(Skill.AdvancementClass>=2)
            {
                int64 One=0,Ten=0;Dat->TryGetSkillXpToNextRank(Skill.AdvancementClass,Skill.XpSpent,One);Dat->TryGetSkillXpToNextRank(Skill.AdvancementClass,Skill.XpSpent,Ten,nullptr,10);
                Row(Skill.Name,Did,Skill.Current,One,Ten,
                    [this,Id=Skill.SkillId](int32 Count){Binder->SelectedSkillId=Id;Binder->ActiveSkillTab="SkillPage";Binder->RaiseSelectedStat(Count);});
            }
            else
            {
                int32 Credits=0;const bool CanTrain=Skill.AdvancementClass==1 && Dat->TryGetSkillTrainedCost(Skill.SkillId,Credits);
                Body->AddSlot().AutoHeight().Padding(4,7)[SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().AutoWidth().Padding(4).VAlign(VAlign_Center)[Icon(Did)]
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[PageText(Skill.Name)]
                    +SHorizontalBox::Slot().AutoWidth()[Button(FString::Printf(TEXT("Train\n%d credits"),Credits),[this,Id=Skill.SkillId](){Binder->SelectedSkillId=Id;Binder->ActiveSkillTab="SkillPage";Binder->RaiseSelectedStat(1);},CanTrain && V.AvailableSkillCredits>=Credits)]];
            }
          }
        }
    }
}
void UACEVRMenu::BuildLoot()
{
    const int32 Root=Client->GetOpenExternalContainerGuid();
    if(!Root){Body->AddSlot().AutoHeight()[PageText(TEXT("Open a corpse or container to see its contents."))];return;}
    TArray<int32> Packs;
    if(const auto Session=Client->GetSession())Session->GetContainerPackGuids(Root,Packs);
    const int32 Container=Packs.Contains(Binder->OpenLootSelectedPackGuid)?Binder->OpenLootSelectedPackGuid:Root;
    Binder->OpenLootSelectedPackGuid=Container;
    if(Container!=Root)Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Back to container"),[this,Root]()
        {Binder->OpenLootSelectedPackGuid=Root;ClearItemSelection();bDirty=true;})];
    auto Items=Client->GetPackItems(Container);
    // Side-slot packs are deliberately omitted from the ordinary item grid.
    // Expose them here too, so touch/VR can open or take an installed chest pack.
    if(Container==Root)for(int32 Guid:Packs)
    {FACEWorldObject PackItem;if(Client->GetWorldObject(Guid,PackItem))Items.Add(PackItem);}
    auto Grid=SNew(SUniformGridPanel).SlotPadding(FMargin(4));int32 I=0;
    for(const auto& Item:Items){Grid->AddSlot(I%6,I/6)[ItemButton(Item)];++I;}
    Body->AddSlot().AutoHeight()[Grid];
    FACEWorldObject Item;const bool Valid=Client->GetWorldObject(Selected,Item)
        && Items.ContainsByPredicate([this](const auto& Entry){return Entry.Guid==Selected;});
    if(Valid){Body->AddSlot().AutoHeight()[PageText(ACERetailObjectNames::Name(Item),28)];AddQuantity(Item.StackSize);}
    if(Valid && Packs.Contains(Selected))Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Open selected pack"),[this]()
        {Binder->OpenLootSelectedPackGuid=Selected;ClearItemSelection();bDirty=true;})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Take selected"),[this](){Binder->PickupInventoryAmount(Selected,Binder->GetSelectedItemAmount(Selected));bDirty=true;},Valid)];
}
void UACEVRMenu::BuildVendor()
{
    if(!Client->GetOpenVendorGuid()){Body->AddSlot().AutoHeight()[PageText(TEXT("Speak to a vendor to buy or sell."))];return;}
    Body->AddSlot().AutoHeight().Padding(4)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[Tab(TEXT("Buying"),!bVendorSelling,[this](){ClearItemSelection();bVendorSelling=false;PageIndex=0;bDirty=true;})]
        +SHorizontalBox::Slot().FillWidth(1)[Tab(TEXT("Selling"),bVendorSelling,[this](){ClearItemSelection();bVendorSelling=true;PageIndex=0;bDirty=true;})]];
    const auto Session=Client->GetSession();if(!Session)return;
    const FString Currency=Session->VendorUsesPyreals()?TEXT("pyreals"):
        (Session->GetVendorCurrencyName().IsEmpty()?TEXT("currency"):Session->GetVendorCurrencyName());
    const int32 Purse=Session->GetVendorPurchaseFunds();
    Body->AddSlot().AutoHeight().Padding(4)[PageText(FString::Printf(TEXT("You have %d %s"),Purse,*Currency),28)];
    auto& Cart=bVendorSelling?Binder->VendorSellCart:Binder->VendorBuyCart;
    int64 Total=0;
    auto CartPanel=SNew(SVerticalBox);
    for(const auto& Offer:Cart)
    {
        FACEWorldObject Item;if(!Client->GetWorldObject(Offer.Value,Item))continue;
        const uint32 Price=bVendorSelling?ACEVendorPricing::VendorBuyPayout(Item,Offer.Key,Client->GetVendorBuyRate()):ACEVendorPricing::VendorSellCost(Item,Offer.Key,Client->GetVendorSellRate());
        Total+=Price;
        Item.StackSize=Offer.Key;
        CartPanel->AddSlot().AutoHeight().Padding(3)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ItemButton(Item)]
            +SHorizontalBox::Slot().FillWidth(1).Padding(8).VAlign(VAlign_Center)[PageText(FString::Printf(TEXT("%d × %s\n%u %s"),Offer.Key,*ACERetailObjectNames::Name(Item),Price,bVendorSelling?TEXT("pyreals"):*Currency))]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Button(TEXT("Remove"),[this,Guid=Offer.Value](){auto& List=bVendorSelling?Binder->VendorSellCart:Binder->VendorBuyCart;List.RemoveAll([Guid](const auto& O){return O.Value==Guid;});bDirty=true;})]];
    }
    CartPanel->AddSlot().AutoHeight().Padding(4)[PageText(FString::Printf(TEXT("%s list: %lld %s"),bVendorSelling?TEXT("Sell"):TEXT("Buy"),Total,bVendorSelling?TEXT("pyreals"):*Currency),28)];
    if(Cart.IsEmpty())CartPanel->AddSlot().AutoHeight().Padding(4)[PageText(bVendorSelling?TEXT("Select an inventory item below, or drag it into this sell list."):TEXT("Select vendor stock below to add it to your buy list."))];
    auto CartSurface=SNew(SBorder).Padding(8).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.03f,.05f,.07f))[CartPanel];
    Body->AddSlot().AutoHeight().Padding(4)[CartSurface];if(bVendorSelling)ItemDestinations.Add({CartSurface,0,0,0,0,true});
    Body->AddSlot().AutoHeight().Padding(4)[Button(bVendorSelling?TEXT("Sell listed items"):TEXT("Buy listed items"),[this]()
    {if(bVendorSelling)Binder->SellVendorCart(false);else Binder->BuyVendorCart();bDirty=true;},!Cart.IsEmpty())];
    FACEWorldObject Selection;const bool SelectedExists=Client->GetWorldObject(Selected,Selection);
    const auto Stock=Client->GetVendorMerchandise();
    const bool Valid=bVendorSelling?SelectedExists && Client->IsOwnedInventoryItem(Selection):Stock.ContainsByPredicate([this](const auto& I){return I.Guid==Selected;});
    if(Valid)
    {
        Body->AddSlot().AutoHeight().Padding(4)[PageText(ACERetailObjectNames::Name(Selection),28)];
        AddQuantity(bVendorSelling?Selection.StackSize:Binder->GetVendorPurchaseLimit(Selected));
        Body->AddSlot().AutoHeight().Padding(4)[Button(bVendorSelling?TEXT("Add selected quantity to sell list"):TEXT("Add selected quantity to buy list"),[this]()
        {if(bVendorSelling)Binder->AddInventoryGuidToVendorSellCart(Selected,Binder->GetSelectedItemAmount(Selected));else{Binder->VendorSelectedGuid=Selected;Binder->AddSelectedVendorItemToBuyCart();}bDirty=true;})];
    }
    if(bVendorSelling)
    {
        Body->AddSlot().AutoHeight().Padding(4)[PageText(TEXT("Your inventory"),28)];
        Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[PackGrid(Pack?Pack:Client->GetPlayerGuid())]
            +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[PackIcons()]];
    }
    else
    {
        const int32 Pages=FMath::Max(1,FMath::DivideAndRoundUp(Stock.Num(),24));PageIndex=FMath::Clamp(PageIndex,0,Pages-1);
        Body->AddSlot().AutoHeight().Padding(4)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Previous"),[this](){ClearItemSelection();--PageIndex;bDirty=true;},PageIndex>0)]
            +SHorizontalBox::Slot().FillWidth(1).Padding(12)[PageText(FString::Printf(TEXT("Vendor stock: %d / %d"),PageIndex+1,Pages))]
            +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Next"),[this](){ClearItemSelection();++PageIndex;bDirty=true;},PageIndex+1<Pages)]];
        auto Grid=SNew(SUniformGridPanel).SlotPadding(FMargin(4));
        for(int32 I=PageIndex*24;I<FMath::Min(Stock.Num(),(PageIndex+1)*24);++I)Grid->AddSlot((I%24)%8,(I%24)/8)[ItemButton(Stock[I])];
        Body->AddSlot().AutoHeight()[Grid];
    }
}
void UACEVRMenu::BuildAllegiance()
{
    const auto A=Client->GetAllegiance();
    Body->AddSlot().AutoHeight().Padding(4)[PageText(A.AllegianceName,30)];
    Body->AddSlot().AutoHeight().Padding(4)[PageText(FString::Printf(TEXT("Rank %d   Members %d\nMonarch: %s\nPatron: %s"),A.Rank,A.TotalMembers,*A.MonarchName,*A.PatronName))];
    for(const auto& Member:A.Vassals)Body->AddSlot().AutoHeight().Padding(4)[Button(FString::Printf(TEXT("%s   Level %d   Rank %d"),*Member.Name,Member.Level,Member.Rank),[this,Id=Member.Guid](){Client->SelectObject(Id);})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Swear to selected player"),[this](){if(Client->GetSelectedObject().Guid)Client->SendSwearAllegiance(Client->GetSelectedObject().Guid);})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Break allegiance to patron"),[this,A](){if(Confirmation==TEXT("Press again to break allegiance.")){Client->SendBreakAllegiance(A.PatronGuid);Confirmation.Reset();}else Confirmation=TEXT("Press again to break allegiance.");bDirty=true;},A.PatronGuid!=0)];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Dismiss selected vassal"),[this,A](){const int32 Id=Client->GetSelectedObject().Guid;if(A.Vassals.ContainsByPredicate([Id](const auto& Member){return Member.Guid==Id && Member.Role==3;}))Client->SendBreakAllegiance(Id);})];
}
void UACEVRMenu::BuildOptions()
{
    for(const auto& Option:ACECharacterOptions::GetTable())
        Body->AddSlot().AutoHeight().Padding(3)[Button(FString::Printf(TEXT("%s: %s"),Option.Label,Client->IsCharacterOptionSet(Option.Option)?TEXT("On"):TEXT("Off")),[this,Id=Option.Option](){Client->SendSetSingleCharacterOption(Id,!Client->IsCharacterOptionSet(Id));bDirty=true;})];
}
void UACEVRMenu::BuildTrade()
{
    if(!Client->GetTradePartnerGuid())
    {
        Body->AddSlot().AutoHeight()[Button(TEXT("Trade with selected player"),[this](){Client->SendOpenTrade(Client->GetSelectedObject().Guid);})];return;
    }
    for(bool Mine:{true,false})
    {
        Body->AddSlot().AutoHeight().Padding(4)[PageText(Mine?TEXT("Your offer"):TEXT("Their offer"),30)];
        auto Grid=SNew(SUniformGridPanel).SlotPadding(FMargin(4));int32 I=0;
        for(int32 Guid:Mine?Client->GetTradeSelfItems():Client->GetTradePartnerItems())
        {FACEWorldObject Item;if(Client->GetWorldObject(Guid,Item)){Grid->AddSlot(I%6,I/6)[ItemButton(Item)];++I;}}
        Body->AddSlot().AutoHeight()[Grid];
    }
    if(Client->HasAcceptedTrade())Body->AddSlot().AutoHeight().Padding(4)[PageText(TEXT("You have accepted this offer."))];
    if(Client->HasPartnerAcceptedTrade())Body->AddSlot().AutoHeight().Padding(4)[PageText(TEXT("Your partner has accepted this offer."))];
    Body->AddSlot().AutoHeight().Padding(4)[Button(Client->HasAcceptedTrade()?TEXT("Withdraw acceptance"):TEXT("Accept trade"),[this](){if(Client->HasAcceptedTrade())Client->SendDeclineTrade();else Client->SendAcceptTrade();})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Clear your offer"),[this](){Client->SendResetTrade();})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Cancel trade"),[this](){Client->SendCloseTrade();bDirty=true;})];
}
void UACEVRMenu::BuildMore()
{
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Manage Plugins"),[this](){OpenPage("Plugins");})];
    if(Rig)Body->AddSlot().AutoHeight()[Button(TEXT("VR settings"),[this](){Rig->ToggleSettings();})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Allegiance"),[this](){OpenPage("Allegiance");})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Character and chat options"),[this](){OpenPage("Options");})];
    Body->AddSlot().AutoHeight().Padding(4)[Button(TEXT("Item hotbars"),[this](){OpenPage("Hotbars");})];
    // Keep every existing interaction reachable while the native views share its
    // controller. The desktop surface is rendered only when explicitly opened.
    const TPair<const TCHAR*,const TCHAR*> Pages[]={{TEXT("Chess / Game Center"),TEXT("MiniGamePanel_Field")},{TEXT("Journal"),TEXT("QuestManagementPanel_Field")},
        {TEXT("Map"),TEXT("WorldPanel_Field")},{TEXT("Options"),TEXT("OptionsPanel_Field")},{TEXT("All classic panels"),TEXT("InventoryPanel_Field")}};
    if(Rig)for(const auto& P:Pages)Body->AddSlot().AutoHeight().Padding(4)[Button(P.Key,[this,Panel=FName(P.Value)](){Rig->bUseDesktopMenu=true;Rig->OpenRetailPanel(Panel);})];
}
void UACEVRMenu::BuildShortcuts()
{
    Body->AddSlot().AutoHeight().Padding(4)[PageText(TEXT("Choose an inventory item, then assign it to a shortcut below."))];
    if(bTouchMode)Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1)[PackGrid(Pack?Pack:Client->GetPlayerGuid())]
        +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[SNew(SBox).WidthOverride(100).HeightOverride(210)[SNew(SScrollBox)+SScrollBox::Slot()[PackIcons()]]]];
    else Body->AddSlot().AutoHeight()[PackGrid(Pack?Pack:Client->GetPlayerGuid())];
    FACEWorldObject Item;const bool Owned=Client->GetWorldObject(Selected,Item) && Client->IsOwnedInventoryItem(Item);
    Body->AddSlot().AutoHeight().Padding(4)[Button(bAssignShortcut?TEXT("Cancel assignment"):TEXT("Assign selected inventory item"),[this](){bAssignShortcut=!bAssignShortcut;bDirty=true;},Owned)];
    if(bAssignShortcut)Body->AddSlot().AutoHeight()[PageText(TEXT("Choose a slot for ")+ACERetailObjectNames::Name(Item),28)];
    for(int32 Index=0;Index<18;++Index)
    {
        FACEWorldObject Target;const int32 Guid=Client->GetShortcutObject(Index);Client->GetWorldObject(Guid,Target);
        auto Row=SNew(SHorizontalBox);
        Row->AddSlot().FillWidth(1).Padding(2)[Button(FString::Printf(TEXT("%d.%d   %s"),Index/9+1,Index%9+1,Guid?*ACERetailObjectNames::Name(Target):TEXT("Empty")),[this,Index]()
        {if(bAssignShortcut){Binder->AssignInventoryShortcut(Selected,Index);bAssignShortcut=false;bDirty=true;}else Binder->UseShortcutSlot(Index+1);},bAssignShortcut || Guid!=0)];
        Row->AddSlot().AutoWidth().Padding(2)[Button(TEXT("Clear"),[this,Index](){Client->SendRemoveShortcut(Index);bDirty=true;},Guid!=0)];
        Body->AddSlot().AutoHeight()[Row];
    }
}
