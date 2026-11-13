#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACESession.h"
#include "ACEClientSubsystem.h"
#include "UI/ACEUIGameplayBinder.h"
#include "Engine/GameInstance.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVendorTransactionsTest, "ACE.Network.VendorTransactions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEVendorTransactionsTest::RunTest(const FString&)
{
    auto* Sockets = ISocketSubsystem::Get();
    auto* Receiver = FUdpSocketBuilder(TEXT("Vendor transaction fixture")).AsNonBlocking().BoundToPort(0).Build();
    ON_SCOPE_EXIT { if (Receiver) { Receiver->Close(); Sockets->DestroySocket(Receiver); } };
    if (!TestNotNull(TEXT("Loopback receiver"), Receiver)) return false;
    auto Destination = Sockets->CreateInternetAddr();
    Destination->SetIp(0x7f000001); Destination->SetPort(Receiver->GetPortNo());
    auto* Game = NewObject<UGameInstance>();
    auto* Client = NewObject<UACEClientSubsystem>(Game);
    Client->Session = MakeShared<FACESession>();
    auto& Session = *Client->Session;
    auto* UI = NewObject<UACEUIGameplayBinder>(Game);
    UI->Client = Client;
    Session.SocketC2S = FUdpSocketBuilder(TEXT("Vendor transaction client")).AsNonBlocking().BoundToPort(0).Build();
    if (!TestNotNull(TEXT("Loopback sender"), Session.SocketC2S)) return false;
    Session.ServerC2SAddr = Destination; Session.IssacClient = MakeUnique<FACEIsaac>(123u);
    Session.State = EACESessionState::InWorld; Session.PlayerGuid = 1;
    ON_SCOPE_EXIT { Session.State = EACESessionState::Disconnected; };

    constexpr int32 Vendor = 0x7B4703F0;
    FACEWorldObject Book; Book.Guid = int32(0xF1234567u); Book.MaxStackSize = 1; Book.VendorQuantityAvailable = -1;
    FACEWorldObject Drink; Drink.Guid = int32(0xF1234568u); Drink.MaxStackSize = 250; Drink.VendorQuantityAvailable = -1;
    auto Complete = [&](uint32 Error)
    {
        FACEBinaryWriter W; W.WriteUInt32(Error); FACEBinaryReader R(W.GetData()); Session.HandleUseDone(R);
        TestFalse(TEXT("Vendor completion releases the hourglass"), Session.IsCursorBusy());
    };
    auto CheckWire = [&](uint32 Action, const TArray<TPair<int32, int32>>& Expected, uint32 Currency)
    {
        if (!TestEqual(TEXT("One transaction sends one reliable packet"), Session.CachedC2SPackets.Num(), 1)) return;
        const auto& Packet = Session.CachedC2SPackets.CreateConstIterator().Value();
        FACEBinaryReader R(Packet.Payload); R.Skip(14);
        TestEqual(TEXT("Vendor uses the retail weenie queue"), R.ReadUInt16(), ACEQueue::WeenieQueue);
        TestEqual(TEXT("Vendor action envelope"), R.ReadUInt32(), ACEOpcode::GameAction);
        R.ReadUInt32();
        TestEqual(TEXT("Transaction opcode"), R.ReadUInt32(), Action);
        TestEqual(TEXT("Transaction addresses the open vendor"), R.ReadUInt32(), uint32(Vendor));
        TestEqual(TEXT("Transaction item count"), R.ReadUInt32(), uint32(Expected.Num()));
        for (const auto& Item : Expected)
        {
            TestEqual(TEXT("Retail ItemProfile amount"), R.ReadInt32(), Item.Key);
            TestEqual(TEXT("Full unsigned stock GUID survives serialization"), R.ReadUInt32(), uint32(Item.Value));
        }
        // CM_Vendor::Event_Buy (006AA0F0) always appends the uint32 currency,
        // including zero for pyreals. Event_Sell (006AA000) has no such trailer.
        // A strict server must be able to read the complete request before it
        // can transact or send UseDone; stock ACE currently ignores this field.
        if (Action == ACEGameAction::Buy)
        {
            if (TestEqual(TEXT("Retail Buy includes the required four-byte currency trailer"), R.Remaining(), 4))
                TestEqual(TEXT("Buy echoes the vendor's advertised currency"), R.ReadUInt32(), Currency);
        }
        TestEqual(TEXT("No missing or extra transaction bytes"), R.Remaining(), 0);
        TestTrue(TEXT("Submitted transaction waits for server completion"), Session.IsCursorBusy());
        Session.CachedC2SPackets.Reset();
    };
    // Switch back to an ordinary vendor after an alternate-currency vendor;
    // the previous shop's currency must never leak into the next purchase.
    for (uint32 Currency : {0u, 0x12345678u, 0u})
    {
        FACEBinaryWriter Offer;
        Offer.WriteUInt32(Vendor); Offer.WriteUInt32(MAX_uint32);
        Offer.WriteInt32(-1); Offer.WriteInt32(-1); Offer.WriteUInt32(0);
        Offer.WriteFloat(.9f); Offer.WriteFloat(1.1f); Offer.WriteUInt32(Currency);
        Offer.WriteUInt32(10000); Offer.WriteString16L(Currency ? TEXT("Tokens") : TEXT("")); Offer.WriteUInt32(0);
        FACEBinaryReader OfferReader(Offer.GetData()); Session.HandleApproachVendor(OfferReader);
        Session.VendorMerchandise = {Book, Drink};

        Session.SendBuyItems(Vendor, {{1, Book.Guid}});
        CheckWire(ACEGameAction::Buy, {{1, Book.Guid}}, Currency); Complete(0);
        Session.SendBuyItems(Vendor, {{2, Drink.Guid}, {3, Drink.Guid}, {1, Book.Guid}});
        CheckWire(ACEGameAction::Buy, {{5, Drink.Guid}, {1, Book.Guid}}, Currency); Complete(1);
    }
    FACEWorldObject Owned = Drink; Owned.ContainerId = Session.PlayerGuid; Owned.StackSize = 5;
    Session.WorldObjects.Add(Owned.Guid, Owned);
    Session.SendSellItems(Vendor, {{5, Owned.Guid}});
    CheckWire(ACEGameAction::Sell, {{5, Owned.Guid}}, 0); Complete(0);

    Session.SendBuyItems(Vendor + 1, {{1, Book.Guid}});
    Session.SendBuyItems(Vendor, {{0, Book.Guid}, {1, 999}});
    TestTrue(TEXT("Invalid or stale purchases send no request"), Session.CachedC2SPackets.IsEmpty());
    TestFalse(TEXT("Rejected purchase never starts an hourglass"), Session.IsCursorBusy());

    // Normal retail vendor UI must stop an unaffordable request locally, even
    // if a custom server would accept it against a separate bank balance.
    UI->OpenVendorGuid = Vendor; UI->VendorSelectedGuid = Drink.Guid;
    Session.VendorSellRate = 1.1f;
    Book.Value = 10; Drink.Value = 10;
    Session.VendorMerchandise = {Book, Drink};
    Session.VendorCurrencyCount = MAX_int32;
    Session.PlayerVitals.StatQualityInts.Add(9000, MAX_int32); // unrelated custom bank quality
    FACEWorldObject Coins; Coins.Guid = 300; Coins.ContainerId = Session.PlayerGuid;
    Coins.WeenieClassId = 273; Coins.ItemType = ACEItemType::Money;
    Coins.Name = TEXT("Pyreal Bank Token"); Coins.StackSize = 10000; Coins.MaxStackSize = 10000;
    Session.UpsertWorldObject(Coins);
    auto NoPurchase = [&]()
    {
        TestTrue(TEXT("Unaffordable UI purchase sends no Buy packet"), Session.CachedC2SPackets.IsEmpty());
        TestFalse(TEXT("Unaffordable UI purchase does not start an hourglass"), Session.IsCursorBusy());
    };
    for (int32 Balance : {0, -1, 10})
    {
        Session.ApplyPlayerIntQuality(20, Balance);
        UI->BuySelectedVendorItem(); NoPurchase();
    }
    Session.PlayerVitals.StatQualityInts.Remove(20);
    UI->BuySelectedVendorItem(); NoPurchase();
    TestEqual(TEXT("Missing CoinValue does not fall back to inferred stacks or bank"), UI->CountPlayerPyreals(), 0);
    Session.ApplyPlayerIntQuality(20, 11);
    TestEqual(TEXT("Retail purse reads CoinValue rather than name-matched items"), UI->CountPlayerPyreals(), 11);
    UI->BuySelectedVendorItem();
    CheckWire(ACEGameAction::Buy, {{1, Drink.Guid}}, 0); Complete(0);

    // Validate the entire cart, and keep it intact when only individual rows fit.
    UI->VendorBuyCart = {{1, Book.Guid}, {1, Drink.Guid}};
    UI->BuyVendorCart(); NoPurchase();
    TestEqual(TEXT("Rejected Buy All retains the whole cart"), UI->VendorBuyCart.Num(), 2);
    Session.ApplyPlayerIntQuality(20, 22);
    UI->BuyVendorCart();
    CheckWire(ACEGameAction::Buy, {{1, Book.Guid}, {1, Drink.Guid}}, 0); Complete(0);
    TestTrue(TEXT("Affordable Buy All clears its submitted cart"), UI->VendorBuyCart.IsEmpty());

    // A balance update after staging must apply immediately to single/cart buys.
    UI->VendorBuyCart = {{2, Drink.Guid}};
    Session.ApplyPlayerIntQuality(20, 21);
    UI->BuyVendorCartItem(); NoPurchase();
    TestEqual(TEXT("Rejected cart-row purchase retains its quantity"), UI->VendorBuyCart[0].Key, 2);
    Session.ApplyPlayerIntQuality(20, 22);
    UI->BuyVendorCartItem();
    CheckWire(ACEGameAction::Buy, {{2, Drink.Guid}}, 0); Complete(0);
    TestTrue(TEXT("Successful cart-row purchase removes only bought quantities"), UI->VendorBuyCart.IsEmpty());

    // Price duplicate rows as one request, with the same rounding as the wire.
    Session.VendorSellRate = 1.05f;
    Session.ApplyPlayerIntQuality(20, 20);
    TestFalse(TEXT("Combined quantity cannot bypass funds"), UI->SubmitVendorPurchase({{1, Drink.Guid}, {1, Drink.Guid}}));
    NoPurchase();
    Session.ApplyPlayerIntQuality(20, 21);
    TestTrue(TEXT("Duplicate lines use the combined retail stack price"), UI->SubmitVendorPurchase({{1, Drink.Guid}, {1, Drink.Guid}}));
    CheckWire(ACEGameAction::Buy, {{2, Drink.Guid}}, 0); Complete(0);

    Session.VendorSellRate = 1.0f;
    Session.VendorMerchandise[0].Value = MAX_int32;
    Session.VendorMerchandise[1].Value = MAX_int32;
    Session.ApplyPlayerIntQuality(20, MAX_int32);
    UI->VendorBuyCart = {{1, Book.Guid}, {1, Drink.Guid}};
    UI->BuyVendorCart(); NoPurchase();
    TestEqual(TEXT("Large total cannot wrap into an affordable cart"), UI->VendorBuyCart.Num(), 2);

    // Trade notes retain their fixed premium, independent of vendor markup.
    Session.VendorMerchandise = {Book};
    Session.VendorMerchandise[0].ItemType = ACEItemType::PromissoryNote;
    Session.VendorMerchandise[0].Value = 100;
    Session.VendorSellRate = 1.55f;
    Session.ApplyPlayerIntQuality(20, 114);
    TestFalse(TEXT("Note premium is included in affordability"), UI->SubmitVendorPurchase({{1, Book.Guid}}));
    NoPurchase();
    Session.ApplyPlayerIntQuality(20, 115);
    TestTrue(TEXT("Exact note price is sufficient"), UI->SubmitVendorPurchase({{1, Book.Guid}}));
    CheckWire(ACEGameAction::Buy, {{1, Book.Guid}}, 0); Complete(0);

    // Alternate currency keeps its existing live inventory count and cannot
    // spend pyreals, a stale opening balance or an unrelated bank property.
    Session.VendorMerchandise = {Book}; Session.VendorSellRate = 1;
    Session.VendorCurrencyWeenie = 500; Session.VendorCurrencyName.Empty();
    Session.ApplyPlayerIntQuality(20, MAX_int32);
    TestFalse(TEXT("Currency identity comes from WCID even without a display name"), Session.VendorUsesPyreals());
    TestFalse(TEXT("Pyreals cannot fund a token purchase"), UI->SubmitVendorPurchase({{1, Book.Guid}})); NoPurchase();
    Coins.WeenieClassId = 500; Coins.StackSize = 10; Session.UpsertWorldObject(Coins);
    TestTrue(TEXT("Exact live alternate currency balance can buy"), UI->SubmitVendorPurchase({{1, Book.Guid}}));
    CheckWire(ACEGameAction::Buy, {{1, Book.Guid}}, 500); Complete(0);
    Coins.StackSize = 9; Session.UpsertWorldObject(Coins);
    TestFalse(TEXT("Spent currency is rechecked on the next click"), UI->SubmitVendorPurchase({{1, Book.Guid}})); NoPurchase();
    Session.VendorCurrencyWeenie = 0; Session.VendorCurrencyName = TEXT("Pyreals");
    TestTrue(TEXT("Ordinary vendor remains pyreal-based with a nonempty label"), Session.VendorUsesPyreals());
    TestFalse(TEXT("Stale stock is not partially purchased"), UI->SubmitVendorPurchase({{1, Book.Guid}, {1, 999}})); NoPurchase();
    return true;
}
#endif
