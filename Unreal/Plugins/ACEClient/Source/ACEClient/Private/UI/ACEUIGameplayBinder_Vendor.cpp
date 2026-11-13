#include "UI/ACEUIGameplayBinder.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEInventoryRules.h"
#include "ACEVendorPricing.h"

bool UACEUIGameplayBinder::SubmitVendorPurchase(const TArray<TPair<int32,int32>>& Items)
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	if (!Session || Session->GetState() != EACESessionState::InWorld || !OpenVendorGuid
		|| OpenVendorGuid != Session->GetOpenVendorGuid() || Items.IsEmpty()) return false;

	// Retail gmVendorUI::BuySingleItem and Buy All both gate the request before
	// CM_Vendor::Event_Buy. Keep this UI policy out of the transport: Item Tools
	// has a separate, explicit custom-server bank opt-in.
	TArray<TPair<int32,int32>> Purchase;
	for (const auto& Row : Items)
	{
		const auto* Stock = Session->GetVendorMerchandise().FindByPredicate([&](const auto& Item){return Item.Guid == Row.Value;});
		if (!Stock || Row.Key <= 0 || Row.Key > ACEInventoryRules::VendorPurchaseLimit(*Stock))
		{
			PostInventorySystemMessage(TEXT("The vendor's stock changed. Review your buy list and try again."));
			return false;
		}
		if (auto* Existing = Purchase.FindByPredicate([&](const auto& Item){return Item.Value == Row.Value;}))
		{
			const int64 Amount = int64(Existing->Key) + Row.Key;
			if (Amount > ACEInventoryRules::VendorPurchaseLimit(*Stock))
			{
				PostInventorySystemMessage(TEXT("The vendor's stock changed. Review your buy list and try again."));
				return false;
			}
			Existing->Key = int32(Amount);
		}
		else Purchase.Add(Row);
	}
	int64 Total = 0;
	for (const auto& Row : Purchase)
	{
		const auto& Stock = *Session->GetVendorMerchandise().FindByPredicate([&](const auto& Item){return Item.Guid == Row.Value;});
		Total += ACEVendorPricing::VendorSellCost(Stock, Row.Key, Session->GetVendorSellRate());
	}
	if (Total > Session->GetVendorPurchaseFunds())
	{
		PostInventorySystemMessage(TEXT("You don't have enough money"));
		return false;
	}
	Client->SendBuyItems(OpenVendorGuid, Purchase);
	return true;
}

void UACEUIGameplayBinder::BuyVendorCart()
{
	if (!SubmitVendorPurchase(VendorBuyCart)) return;
	VendorBuyCart.Reset();
	RefreshVendorOverlays();
}

bool UACEUIGameplayBinder::CanStageVendorSellItem(int32 Guid) const
{
	FACEWorldObject Item;
	if (!Client || !OpenVendorGuid || !Client->GetSession()
		|| VendorSellSplit.SourceGuid || TradeStackSplit.SourceGuid
		|| !Client->GetWorldObject(Guid, Item) || Client->GetTradeSelfItems().Contains(Guid)) return false;
	const bool bMainPack = Guid == Client->GetPlayerGuid();
	if (!bMainPack && !Client->IsOwnedInventoryItem(Item)) return false;
	// VendorSellUI::DragItemAcceptable accepts a carried pack as a bulk offer.
	// Its contents are filtered individually; the pack itself is never sold.
	if (bMainPack || (Item.ItemType & ACEItemType::Container) || Item.ItemsCapacity > 0)
		return !Client->GetPackItems(Guid).IsEmpty();
	return Item.Attuned == 0 && Client->GetSession()->CanVendorBuyItem(Item);
}

void UACEUIGameplayBinder::UpdateVendorSellSplit()
{
	if (!VendorSellSplit.SourceGuid) return;
	if (!Client || OpenVendorGuid != VendorSellSplit.VendorGuid
		|| Client->GetOpenVendorGuid() != VendorSellSplit.VendorGuid
		|| !Client->GetSession() || Client->GetSession()->GetState() != EACESessionState::InWorld)
	{
		VendorSellSplit = {};
		return;
	}
	if (FPlatformTime::Seconds() >= VendorSellSplit.Deadline)
	{
		VendorSellSplit = {};
		PostInventorySystemMessage(TEXT("The stack split was not confirmed. Nothing was sold; select the item and try again."));
		return;
	}

	const int32 NewGuid = FindCompletedInventoryStackSplit(VendorSellSplit);
	if (NewGuid == INDEX_NONE)
	{
		VendorSellSplit = {};
		PostInventorySystemMessage(TEXT("The stack split completed. Select the new stack and add it to the sell list."));
		return;
	}
	if (!NewGuid) return;
	const int32 Amount = VendorSellSplit.Amount;
	const bool bSellWhenReady = VendorSellSplit.bSellWhenReady;
	VendorSellSplit = {};
	SelectInventoryGuid(NewGuid);
	AddInventoryGuidToVendorSellCart(NewGuid, Amount);
	if (bSellWhenReady) SellVendorCart(true);
}
