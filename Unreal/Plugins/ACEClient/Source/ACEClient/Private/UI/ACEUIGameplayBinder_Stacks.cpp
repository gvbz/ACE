#include "UI/ACEUIGameplayBinder.h"
#include "ACEClientSubsystem.h"
#include "ACEInventoryRules.h"
#include "ACESession.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIElementManager.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Blueprint/WidgetTree.h"

bool UACEUIGameplayBinder::CanMoveInventoryAmountToContainer(int32 Guid, int32 Container, int32 Amount, int32 MergeTarget, bool bAutoMerge) const
{
	FACEWorldObject Item, Destination;
	if (!Client || !Guid || !Container || Guid == Client->GetPlayerGuid() || Guid == Container
		|| !Client->GetWorldObject(Guid, Item) || !Client->GetWorldObject(Container, Destination)
		|| Client->GetTradeSelfItems().Contains(Guid) || Client->GetTradeSelfItems().Contains(Container)) return false;
	if (!ACEInventoryRules::CheckHookStatus(Item, Destination)) return false;
	Amount = Amount > 0 ? FMath::Min(Amount, FMath::Max(1, Item.StackSize)) : FMath::Max(1, Item.StackSize);
	const bool bPack = ACEInventoryRules::IsContainer(Item) || (Item.ItemType & ACEItemType::Container)
		|| (Item.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot);
	// Retail permits packs in any container with container slots (including
	// chests). Ordinary backpacks have none. Never allow a containment cycle.
	TSet<int32> Parents;
	for (int32 Parent = Container; Parent;)
	{
		if (Parent == Guid || Parents.Contains(Parent)) return false;
		Parents.Add(Parent);
		FACEWorldObject Ancestor;
		if (!Client->GetWorldObject(Parent, Ancestor)) break;
		Parent = Ancestor.ContainerId;
	}
	if (MergeTarget)
	{
		FACEWorldObject Target;
		if (Client->GetWorldObject(MergeTarget, Target) && Target.ContainerId == Container
			&& !Client->GetTradeSelfItems().Contains(MergeTarget)
			&& ACEInventoryRules::MergeAmount(Item, Target) > 0) return true;
	}
	// A full move within the same pack simply reorders its existing slot.
	if (Item.ContainerId == Container && Amount == FMath::Max(1, Item.StackSize)) return true;
	const auto Session=Client->GetSession();
	if (bAutoMerge && Session && Session->FindContainerMergeTarget(Guid,Container,Amount)) return true;
	// Match the inventory's seven default side slots when a player descriptor
	// omits capacity. Zero on an ordinary bag still means no nested packs.
	const int32 Capacity = bPack ? (Container == Client->GetPlayerGuid() && Destination.ContainersCapacity == 0
		? 7 : Destination.ContainersCapacity) : Destination.ItemsCapacity;
	if (Capacity == -1) return true;
	return Session && Capacity > Session->CountContainerSlots(Container,bPack);
}

void UACEUIGameplayBinder::UpdateInventoryContainerDropFeedback(FVector2D CanvasLocalPos)
{
	if (!Client || !Canvas || !Manager || !Canvas->WidgetTree) return;
	const FVector2D Absolute = Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos);
	const int32 Amount = InvDragAmount > 0 ? InvDragAmount : GetSelectedItemAmount(InvDragGuid);
	const bool OrdinaryDrag = InvDragShortcutSlot == INDEX_NONE && !bInvDragFromVendorSell
		&& !bInvDragFromSalvage && !bInvDragFromHouse;
	if (const auto Overlay = Manager->FindElementByName(TEXT("BigBackpack_Icon_DragAccept")))
	{
		Overlay->bActivatable = false;
		Overlay->bVisible = OrdinaryDrag && Canvas->IsElementExposedAt(
			Manager->FindElementByName(TEXT("InventoryButton")), CanvasLocalPos);
		Overlay->bUseExplicitState = true;
		Overlay->DefaultState = 0x10000046; // gmToolbarUI::HandleInventoryButtonDragOver: DropIn.
	}
	auto Mark = [&](UBorder* Cell, bool Accept, bool DropIn, const TCHAR* Owner, UCanvasPanel* ClipPanel = nullptr)
	{
		const auto* CellSlot = Cell ? Cast<UCanvasPanelSlot>(Cell->Slot) : nullptr;
		if (!CellSlot) return;
		if (!PaperDollDragTargetIcon)
		{
			PaperDollDragTargetIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			PaperDollDragTargetIcon->SetPadding(FMargin(0));
		}
		// ItemSlot_DragOver_DropIn / Accept / Reject, from retail layout 0x21000037.
		SetIconDid(PaperDollDragTargetIcon, Accept ? (DropIn ? 0x060011F7 : 0x060011F9) : 0x060011F8);
		PaperDollDragTargetIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
		UCanvasPanel* Layer = ClipPanel ? ClipPanel : Canvas->GetElementLayer();
		if (PaperDollDragTargetIcon->GetParent() != Layer)
			Layer->AddChild(PaperDollDragTargetIcon);
		if (auto* Slot = Cast<UCanvasPanelSlot>(PaperDollDragTargetIcon->Slot))
		{
			Slot->SetAnchors(FAnchors(0,0)); Slot->SetAutoSize(false);
			Slot->SetPosition(CellSlot->GetPosition()); Slot->SetSize(CellSlot->GetSize());
			if (ClipPanel) Slot->SetZOrder(5); // Above the shortcut icon, selection and cooldown.
			else Canvas->SetOverlayOrder(PaperDollDragTargetIcon, Manager->FindElementByName(Owner), 120004);
		}
	};
	if (OpenHouseLord && HitHouseItemList(CanvasLocalPos))
	{
		for (UBorder* Cell : HouseOfferBackgrounds)
		{
			if (!Cell || !Cell->IsVisible() || !Canvas->IsWidgetExposedAt(Cell, Absolute)) continue;
			const auto Session=Client->GetSession();
			Mark(Cell, !HousePaymentSplit.SourceGuid && Session && Session->CanStageHousePayment(InvDragGuid,bHouseRentTab),
				false, TEXT("Slumlord"));
			return;
		}
	}
	if (const int32 Index = HitTestShortcutSlot(CanvasLocalPos);
		Index != INDEX_NONE && ShortcutClipPanel && ShortcutSlotBgs.IsValidIndex(Index))
	{
		// gmToolbarUI::OnItemListDragOver sets ItemSlot_DragOver_Accept
		// (0x10000040) for items and existing shortcuts. Keep its green ring
		// inside the toolbar's clipping panel, including a partially revealed row.
		if (!bInvDragFromVendorSell && !bInvDragFromSalvage && !bInvDragFromHouse)
			Mark(ShortcutSlotBgs[Index], true, false, TEXT("ToolbarField"), ShortcutClipPanel);
		return;
	}
	if (OpenSalvageToolGuid)
	{
		for (UBorder* Cell : SalvageItemBackgrounds)
		{
			if (!Cell || !Cell->IsVisible() || !Canvas->IsWidgetExposedAt(Cell, Absolute)) continue;
			// Match the drop's ownership, material and retained-item rules. Empty
			// squares have backgrounds too, even though they have no item icon yet.
			Mark(Cell, CanAddItemToSalvageQueue(InvDragGuid), false, TEXT("SalvagePanel"));
			return;
		}
	}
	if (bTradeOpen && OrdinaryDrag)
	{
		// gmSecureTradeUI::OnItemListDragOver uses the same accept/reject art on
		// either offer list. The item is always offered on the player's own side.
		for (const auto* Cells : { &TradeSelfBackgrounds, &TradeOtherBackgrounds })
			for (UBorder* Cell : *Cells)
			{
				if (!Cell || !Cell->IsVisible() || !Canvas->IsWidgetExposedAt(Cell, Absolute)) continue;
				FACEWorldObject Item;
				const bool Accept = TradePartnerGuid && Client->GetTradePartnerGuid() == TradePartnerGuid
					&& !TradeStackSplit.SourceGuid && !VendorSellSplit.SourceGuid
					&& Client->GetWorldObject(InvDragGuid, Item) && Client->IsOwnedInventoryItem(Item)
					&& !Item.Attuned && !Client->GetTradeSelfItems().Contains(InvDragGuid);
				Mark(Cell, Accept, false, TEXT("SecureTrade"));
				return;
			}
	}
	FACEWorldObject DragItem;const bool DragIsPack=Client->GetWorldObject(InvDragGuid,DragItem)
        && (ACEInventoryRules::IsContainer(DragItem) || (DragItem.ItemType & ACEItemType::Container)
			|| (DragItem.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot));
	if (OpenLootContainerGuid)
	{
		// Icons include the root at index zero; background slots do not.
		for (int32 I=0; I<ExtPackSlots.Num() && I<ExtPackGuids.Num(); ++I)
		{
			auto* Cell=ExtPackSlots[I].Get();
			if (!Cell || !Cell->IsVisible() || !Canvas->IsWidgetExposedAt(Cell,Absolute)) continue;
			FACEWorldObject Target;
			const bool DropIn = ExtPackGuids[I] && Client->GetWorldObject(ExtPackGuids[I],Target)
				&& (!DragIsPack || Target.ContainersCapacity != 0);
			const int32 Container = DropIn ? ExtPackGuids[I] : DragIsPack && I > 0 ? OpenLootContainerGuid : 0;
			Mark(Cell, InvDragGuid != ExtPackGuids[I] && CanMoveInventoryAmountToContainer(InvDragGuid,Container,Amount),
				DropIn, TEXT("RootGameplay_FloatyEnvPanel_Field"));
			return;
		}
		const int32 Container=OpenLootSelectedPackGuid ? OpenLootSelectedPackGuid : OpenLootContainerGuid;
		// Item backgrounds reserve index zero for the root container icon.
		for (int32 I=0; I<ExtItemSlots.Num(); ++I)
		{
			auto* Cell=ExtItemSlots[I].Get();
			if (!Cell || !Cell->IsVisible() || !Canvas->IsWidgetExposedAt(Cell,Absolute)) continue;
			const int32 Target=ExtItemGuids.IsValidIndex(I) ? ExtItemGuids[I] : 0;
			Mark(Cell, !DragIsPack && CanMoveInventoryAmountToContainer(InvDragGuid,Container,Amount,Target,false),
				false, TEXT("RootGameplay_FloatyEnvPanel_Field"));
			return;
		}
	}
	for (int32 I=0; I<PackSlots.Num() && I<PackSlotGuids.Num(); ++I)
	{
		auto* Cell=PackSlotBgs.IsValidIndex(I) && PackSlotBgs[I] && PackSlotBgs[I]->IsVisible() ? PackSlotBgs[I].Get() : PackSlots[I].Get();
		if (!Cell || !Cell->IsVisible() || !Canvas->IsWidgetExposedAt(Cell,Absolute)) continue;
		FACEWorldObject TargetPack;
		const bool Nest = PackSlotGuids[I]!=Client->GetPlayerGuid()
			&& Client->GetWorldObject(PackSlotGuids[I],TargetPack) && TargetPack.ContainersCapacity!=0;
		const bool Reorder = (InvDragPackSlotIndex != INDEX_NONE || DragIsPack)
			&& InvDragGuid != Client->GetPlayerGuid() && !Nest;
		const int32 Container = Reorder ? Client->GetPlayerGuid() : PackSlotGuids[I];
		Mark(Cell, CanMoveInventoryAmountToContainer(InvDragGuid,Container,Amount), !Reorder, TEXT("InventoryPanel_Field"));
		return;
	}
	int32 Target=0, Index=INDEX_NONE;
	if (HitTestInventorySlot(Absolute,Target,Index))
	{
		const int32 Visible=Index-InventoryScrollOffset;
		FACEWorldObject Item;
		const bool Ordinary=Client->GetWorldObject(InvDragGuid,Item) && !ACEInventoryRules::IsContainer(Item)
			&& !(Item.ItemType & ACEItemType::Container) && !(Item.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot);
		if (InventorySlotBgs.IsValidIndex(Visible))
			Mark(InventorySlotBgs[Visible], Ordinary && CanMoveInventoryAmountToContainer(InvDragGuid,
				SelectedPackGuid ? SelectedPackGuid : Client->GetPlayerGuid(),Amount,Target,false), false, TEXT("InventoryPanel_Field"));
	}
}

int32 UACEUIGameplayBinder::ResolveInventoryDropPlacement(int32 Guid, int32 Amount, int32 Container, int32 TargetGuid) const
{
	FACEWorldObject Source;
	if (!Client || !Client->GetWorldObject(Guid, Source)) return INDEX_NONE;
	const auto Session = Client->GetSession();
	if (!Session) return INDEX_NONE;
	TArray<int32> Items;
	Session->GetPackItemGuids(Container, Items);
	return ACEInventoryRules::DropInsertionIndex(Items, Guid, TargetGuid, Amount >= FMath::Max(1, Source.StackSize));
}

bool UACEUIGameplayBinder::TryDropInExternalContainer(int32 Guid, int32 Amount, FVector2D Absolute)
{
	if (!Client || !Canvas || !Manager || !OpenLootContainerGuid) return false;
	FACEWorldObject SourceObject, ContainerObject;
	const int32 Destination=OpenLootSelectedPackGuid ? OpenLootSelectedPackGuid : OpenLootContainerGuid;
	if (!Client->GetWorldObject(Guid,SourceObject) || !Client->GetWorldObject(Destination,ContainerObject)) return false;
	const bool IsPack = ACEInventoryRules::IsContainer(SourceObject) || (SourceObject.ItemType & ACEItemType::Container)
		|| (SourceObject.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot);
	// The same hook rule drives both drag feedback and the eventual drop.
	if (!ACEInventoryRules::CheckHookStatus(SourceObject,ContainerObject))
	{
		const auto List=Manager->FindElementUnder(TEXT("ExternalContainer"),TEXT("Ext_Container_ItemList"));
		return List && Canvas->IsElementExposedAt(List,Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute));
	}
	for (int32 I = 0; I < ExtPackSlots.Num() && I < ExtPackGuids.Num(); ++I)
		if (ExtPackSlots[I] && ExtPackSlots[I]->IsVisible() && Canvas->IsWidgetExposedAt(ExtPackSlots[I], Absolute))
		{
			if (ExtPackGuids[I] == Guid) return true;
			FACEWorldObject Target;
			if (ExtPackGuids[I] && Client->GetWorldObject(ExtPackGuids[I],Target)
				&& (!IsPack || Target.ContainersCapacity != 0))
				MoveInventoryAmount(Guid, ExtPackGuids[I], 0, Amount);
			else if (IsPack && I > 0)
			{
				TArray<int32> Packs;
				if (const auto Session = Client->GetSession()) Session->GetContainerPackGuids(OpenLootContainerGuid, Packs);
				const int32 Place = ACEInventoryRules::DropInsertionIndex(Packs, Guid, ExtPackGuids[I], true);
				if (Place != INDEX_NONE) MoveInventoryAmount(Guid, OpenLootContainerGuid, Place, Amount);
			}
			return true;
		}
	if (IsPack)
	{
		const auto List = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Ext_Container_ItemList"));
		if (!List || !Canvas->IsElementExposedAt(List,Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute))) return false;
		PostInventorySystemMessage(TEXT("Cannot place container in item list."));
		return true;
	}
	const int32 Container = OpenLootSelectedPackGuid ? OpenLootSelectedPackGuid : OpenLootContainerGuid;
	for (int32 I = 0; I < ExtItemSlots.Num() && I < ExtItemGuids.Num(); ++I)
		if (ExtItemSlots[I] && ExtItemSlots[I]->IsVisible() && Canvas->IsWidgetExposedAt(ExtItemSlots[I], Absolute))
		{
			if (ExtItemGuids[I] == Guid) return true;
			if (!MergeInventoryAmount(Guid, ExtItemGuids[I], Amount))
			{
				const int32 Place = ResolveInventoryDropPlacement(Guid, Amount, Container, ExtItemGuids[I]);
				if (Place != INDEX_NONE) MoveInventoryAmount(Guid, Container, Place, Amount, false);
			}
			return true;
		}
	const auto List = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Ext_Container_ItemList"));
	if (!List || !Canvas->IsElementExposedAt(List, Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute))) return false;
	MoveInventoryAmount(Guid, Container, Client->GetPackItems(Container).Num(), Amount, false);
	return true;
}

int32 UACEUIGameplayBinder::GetSelectedItemAmount(int32 Guid) const
{
	FACEWorldObject Item;
	if (!Client || !Client->GetWorldObject(Guid, Item)) return 0;
	const int32 Size = FMath::Max(1, Item.StackSize);
	// ItemHolder::GetObjectSplitSize: only the selected object uses the slider.
	return LastSelection.Guid == Guid ? FMath::Clamp(SelectedStackAmount, 1, Size) : Size;
}

void UACEUIGameplayBinder::PickupInventoryAmount(int32 Guid, int32 Amount)
{
	if (!Client) return;
	if (Amount <= 0) Amount = GetSelectedItemAmount(Guid);
	if(Amount<=0)return;
	const auto Pickup=Client->PlanInventoryPickup(Guid,SelectedPackGuid,Amount);
	if(Pickup.MergeTarget)MergeInventoryAmount(Guid,Pickup.MergeTarget,Amount);
	else if(Pickup.Container)MoveInventoryAmount(Guid,Pickup.Container,0,Amount,false);
	else PostInventorySystemMessage(TEXT("No pack has room for that item."));
}

void UACEUIGameplayBinder::MoveInventoryAmount(int32 Guid, int32 Container, int32 Placement, int32 Amount, bool bAutoMerge)
{
	FACEWorldObject Item;
	if (!Client || !Container || Amount <= 0 || !Client->GetWorldObject(Guid, Item)) return;
	if ((ACEInventoryRules::IsContainer(Item) || (Item.ItemType & ACEItemType::Container)
		|| (Item.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot))
		&& !CanMoveInventoryAmountToContainer(Guid, Container, Amount))
	{
		PostInventorySystemMessage(TEXT("That container cannot accept this pack."));
		return;
	}
	if (Client->GetTradeSelfItems().Contains(Guid))
	{
		PostInventorySystemMessage(TEXT("Remove that item from the trade before moving it."));
		return;
	}
	Amount = FMath::Min(Amount, FMath::Max(1, Item.StackSize));
	if(const auto Session=Client->GetSession();bAutoMerge && Session)
		if(const int32 Target=Session->FindContainerMergeTarget(Guid,Container,Amount))
			if(MergeInventoryAmount(Guid,Target,Amount))return;
	if(!bAutoMerge && !CanMoveInventoryAmountToContainer(Guid,Container,Amount,0,false))
	{
		PostInventorySystemMessage(TEXT("That pack cannot accept this item."));
		return;
	}
	// UIElement_ItemList only removes the source's old position for a full move.
	// A split inserts a NEW object; the remainder keeps its original slot.
	if (Amount < Item.StackSize)
	{
		TrackInventoryStackSplit(InventorySelectionSplit, Item, Amount);
		InventorySelectionSplit.ContainerGuid = Container;
		Client->SendStackableSplitToContainer(Guid, Container, Placement, Amount);
	}
	else Client->SendPutItemInContainer(Guid, Container, Placement,false);
}

bool UACEUIGameplayBinder::MergeInventoryAmount(int32 Guid, int32 TargetGuid, int32 Amount)
{
	FACEWorldObject Source, Target;
	if (!Client || Amount <= 0 || !Client->GetWorldObject(Guid, Source) || !Client->GetWorldObject(TargetGuid, Target)) return false;
	const auto Offered = Client->GetTradeSelfItems();
	const auto OtherOffers = Client->GetTradePartnerItems();
	if (Offered.Contains(Guid) || Offered.Contains(TargetGuid) || OtherOffers.Contains(Guid) || OtherOffers.Contains(TargetGuid)) return false;
	Amount = FMath::Min(Amount, ACEInventoryRules::MergeAmount(Source, Target));
	if (Amount <= 0) return false;
	Client->SendStackableMerge(Guid, TargetGuid, Amount);
	SelectInventoryGuid(TargetGuid); // ItemHolder::AttemptMerge selects the surviving stack.
	return true;
}

void UACEUIGameplayBinder::TrackInventoryStackSplit(FInventoryStackSplit& Split, const FACEWorldObject& Source, int32 Amount)
{
	Split = {};
	Split.SourceGuid = Source.Guid;
	Split.SourceContainerGuid = Source.ContainerId;
	Split.ContainerGuid = Source.ContainerId;
	Split.Wcid = Source.WeenieClassId;
	Split.Amount = Amount;
	Split.OriginalSize = Source.StackSize;
	Split.Deadline = FPlatformTime::Seconds() + 15.0;
	Split.ExistingGuids.Reset();
	for (const auto& Existing : Client->GetWorldObjects()) Split.ExistingGuids.Add(Existing.Guid);
}

int32 UACEUIGameplayBinder::FindCompletedInventoryStackSplit(const FInventoryStackSplit& Split, bool bRequireOwned) const
{
	// Object creation, containment and source count updates may arrive in any order.
	FACEWorldObject Source;
	if (!Client || !Client->GetWorldObject(Split.SourceGuid, Source)
		|| Source.ContainerId != Split.SourceContainerGuid || Source.StackSize != Split.OriginalSize - Split.Amount) return 0;
	int32 NewGuid = 0;
	const auto Candidates = Split.bToWorld || Split.WieldLocation ? Client->GetWorldObjects() : Client->GetPackItems(Split.ContainerGuid);
	for (const auto& Item : Candidates)
	{
		if (Split.ExistingGuids.Contains(Item.Guid) || Item.WeenieClassId != Split.Wcid
			|| FMath::Max(1, Item.StackSize) != Split.Amount) continue;
		if (Split.bToWorld)
		{
			if (!Item.bHasPosition || Item.ContainerId || Item.WielderId || Item.ParentGuid) continue;
		}
		else if (Split.WieldLocation)
		{
			if (Item.WielderId != Client->GetPlayerGuid() || Item.CurrentWieldedLocation != Split.WieldLocation) continue;
		}
		else if (Item.ContainerId != Split.ContainerGuid || Item.WielderId || Item.CurrentWieldedLocation
			|| (bRequireOwned && !Client->IsOwnedInventoryItem(Item))) continue;
		// No transaction ID exists on the wire. Never guess between matching new objects.
		if (NewGuid) return INDEX_NONE;
		NewGuid = Item.Guid;
	}
	return NewGuid;
}

void UACEUIGameplayBinder::UpdateInventorySplitSelection()
{
	if (!InventorySelectionSplit.SourceGuid) return;
	if (!Client || !Client->GetSession() || Client->GetSession()->GetState() != EACESessionState::InWorld
		|| FPlatformTime::Seconds() >= InventorySelectionSplit.Deadline)
	{
		InventorySelectionSplit = {};
		return;
	}
	const int32 NewGuid = FindCompletedInventoryStackSplit(InventorySelectionSplit, false);
	if (!NewGuid) return;
	InventorySelectionSplit = {};
	if (NewGuid != INDEX_NONE) SelectInventoryGuid(NewGuid);
}

void UACEUIGameplayBinder::AddInventoryGuidToTrade(int32 Guid, int32 Amount)
{
	if (!Client || !bTradeOpen || !TradePartnerGuid || Client->GetTradePartnerGuid() != TradePartnerGuid) return;
	if (TradeStackSplit.SourceGuid || VendorSellSplit.SourceGuid)
	{
		PostInventorySystemMessage(TEXT("Please wait for the stack to finish splitting."));
		return;
	}
	FACEWorldObject Item;
	if (Amount <= 0 || !Client->GetWorldObject(Guid, Item) || !Client->IsOwnedInventoryItem(Item)
		|| Client->GetTradeSelfItems().Contains(Guid)) return;
	if (Item.Attuned != 0)
	{
		PostInventorySystemMessage(TEXT("That item cannot be traded."));
		return;
	}
	Amount = FMath::Min(Amount, FMath::Max(1, Item.StackSize));
	if (Amount < Item.StackSize)
	{
		// gmSecureTradeUI::AcceptDragObject splits first, then offers the created GUID.
		TradeStackSplit.PartnerGuid = TradePartnerGuid;
		TrackInventoryStackSplit(TradeStackSplit, Item, Amount);
		Client->SendStackableSplitToContainer(Guid, Item.ContainerId, 0, Amount);
		PostInventorySystemMessage(TEXT("Splitting the selected amount before trading it."));
	}
	else Client->SendAddToTrade(Guid, 0);
}

void UACEUIGameplayBinder::UpdateTradeStackSplit()
{
	if (PendingTradeItemGuid && (!Client || !Client->GetSession()
		|| Client->GetSession()->GetState() != EACESessionState::InWorld
		|| FPlatformTime::Seconds() >= PendingTradeItemDeadline))
		PendingTradeItemGuid = PendingTradeItemAmount = PendingTradeItemPartner = 0;
	if (!TradeStackSplit.SourceGuid) return;
	if (!Client || !bTradeOpen || TradePartnerGuid != TradeStackSplit.PartnerGuid
		|| Client->GetTradePartnerGuid() != TradeStackSplit.PartnerGuid
		|| !Client->GetSession() || Client->GetSession()->GetState() != EACESessionState::InWorld)
	{
		TradeStackSplit = {};
		return;
	}
	if (FPlatformTime::Seconds() >= TradeStackSplit.Deadline)
	{
		TradeStackSplit = {};
		PostInventorySystemMessage(TEXT("The stack split was not confirmed. Nothing was offered; select the item and try again."));
		return;
	}
	const int32 NewGuid = FindCompletedInventoryStackSplit(TradeStackSplit);
	if (!NewGuid) return;
	const int32 Amount = TradeStackSplit.Amount;
	TradeStackSplit = {};
	if (NewGuid == INDEX_NONE)
	{
		PostInventorySystemMessage(TEXT("The stack split completed. Select the new stack and add it to the trade."));
		return;
	}
	SelectInventoryGuid(NewGuid);
	AddInventoryGuidToTrade(NewGuid, Amount);
}

bool UACEUIGameplayBinder::TryOpenTradeForDraggedItem(const FACEWorldObject& Target, int32 Guid, int32 Amount)
{
	if (!Client || !Target.bIsPlayer || Target.Guid == Client->GetPlayerGuid()
		|| !(Client->GetCharacterOptions1() & 0x04000000u)) return false;
	if (bTradeOpen && TradePartnerGuid == Target.Guid)
		AddInventoryGuidToTrade(Guid, Amount);
	else
	{
		PendingTradeItemGuid = Guid;
		PendingTradeItemAmount = Amount;
		PendingTradeItemPartner = Target.Guid;
		PendingTradeItemDeadline = FPlatformTime::Seconds() + 15.0;
		Client->SendOpenTrade(Target.Guid);
	}
	return true;
}
