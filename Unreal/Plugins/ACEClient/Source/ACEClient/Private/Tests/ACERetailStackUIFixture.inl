// Included inside the live DAT-screen / isolated loopback fixture. Keep this
// separately invoked so the large screen suite does not exceed MSVC's optimizer limit.
TFunction<void()> CheckStackTransfers = [&]()
{
    auto StackReply=[&](int32 Guid,int32 Count)
    {
        FACEBinaryWriter W;W.WriteUInt8(1);W.WriteInt32(Guid);W.WriteInt32(Count);W.WriteInt32(Count);
        FACEBinaryReader R(W.GetData());Session.HandleSetStackSize(R);
    };
    auto LastWire=[&](uint32 Expected, TArray<uint32> Values)
    {
        uint32 Last=0;
        for(const auto& Pair:Session.CachedC2SPackets)
        {
            FACEBinaryReader Packet(Pair.Value.Payload); Packet.Skip(24);
            if (!Packet.CanRead(4)) continue;
            const uint32 Action=Packet.ReadUInt32();
            if(Action!=ACEGameAction::QueryHealth && Action!=ACEGameAction::IdentifyObject) Last=FMath::Max(Last,Pair.Key);
        }
        if (!TestTrue(TEXT("Stack operation sends a reliable action"),Last!=0)) return;
        FACEBinaryReader Wire(Session.CachedC2SPackets[Last].Payload); Wire.Skip(16);
        TestEqual(TEXT("Stack operation uses GameAction envelope"),Wire.ReadUInt32(),ACEOpcode::GameAction); Wire.ReadUInt32();
        const uint32 Actual=Wire.ReadUInt32();
        if (!TestEqual(FString::Printf(TEXT("Stack operation action: expected %04X, received %04X"),Expected,Actual),Actual,Expected)) return;
        if (!TestTrue(TEXT("Stack action contains all expected fields"),Wire.CanRead(Values.Num()*4))) return;
        for(int32 I=0;I<Values.Num();++I)
        {
            const uint32 Value=Wire.ReadUInt32();
            TestEqual(FString::Printf(TEXT("Stack action %04X field %d: expected %u, received %u"),Expected,I,Values[I],Value),Value,Values[I]);
        }
    };
    const auto SavedObjects=Session.WorldObjects;
    const auto SavedContents=Session.ContainerContents;
    const auto SavedSelection=Session.SelectedObject;
    // This suite constructs a bare subsystem; connect its normally-initialized
    // session selection event while exercising real mouse input and quantity edits.
    const FDelegateHandle SelectionHandler=Session.OnSelectionChanged.AddLambda(
        [&](const FACESelectedObject& Selection){Gameplay->HandleSelectionChanged(Selection);});
    const auto SavedOptions=Session.CharacterOptions1;
    const auto SavedOptions2=Session.CharacterOptions2;
    const int32 SavedPartner=Session.TradePartnerGuid;
    Session.TradeSelfItems.Reset(); Session.TradePartnerItems.Reset();
    Gameplay->HideTradePanel(false); Gameplay->HideVendorPanel(); Gameplay->HideExternalContainer(false);
    Gameplay->CancelPendingUseWith(); Gameplay->ShowExamination(false);
    Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
    Gameplay->SelectedPackGuid=Player.Guid; Gameplay->InventoryScrollOffset=0;
    Session.ContainerContents.Remove(Player.Guid);
    for (auto It=Session.WorldObjects.CreateIterator();It;++It)
        if (It.Value().ContainerId==Player.Guid) It.RemoveCurrent();
    FACEWorldObject Notes; Notes.Guid=99410; Notes.WeenieClassId=20630; Notes.Name=TEXT("Trade Note (250,000)");
    Notes.ItemType=ACEItemType::PromissoryNote; Notes.IconId=0x06001355;
    Notes.StackSize=250; Notes.MaxStackSize=250; Notes.ContainerId=Player.Guid; Notes.PlacementPosition=0;
    auto Match=Notes; Match.Guid=99411; Match.StackSize=240; Match.PlacementPosition=1;
    auto Other=Notes; Other.Guid=99412; Other.WeenieClassId=20631; Other.PlacementPosition=2;
    FACEWorldObject Bag; Bag.Guid=99413; Bag.Name=TEXT("Stack fixture pack"); Bag.ItemType=ACEItemType::Container;
    Bag.ItemsCapacity=24; Bag.ContainerId=Player.Guid; Bag.IconId=0x06004CF7;
    for (const auto& Object:{Notes,Match,Other,Bag}) Session.WorldObjects.Add(Object.Guid,Object);
    Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayStackTransfers"));
    auto Center=[&](UWidget* Widget)
    {
        const auto Geo=Widget->GetCachedGeometry();
        return Canvas->GetCachedGeometry().AbsoluteToLocal(Geo.LocalToAbsolute(Geo.GetLocalSize()*.5));
    };
    auto Slot=[&](int32 Guid)
    {
        const int32 Index=Gameplay->InventorySlotGuids.IndexOfByKey(Guid);
        return Index==INDEX_NONE ? FVector2D(-1000,-1000) : Center(Gameplay->InventorySlotBgs[Index]);
    };
    auto SelectAmount=[&](int32 Quantity)
    {
        Gameplay->CancelInventoryDrag(); Gameplay->SelectInventoryGuid(Notes.Guid);
        Gameplay->StackAmountEntryGuid=Notes.Guid;
        Gameplay->HandleStackAmountCommitted(FText::AsNumber(Quantity),ETextCommit::OnEnter);
    };
    auto Drag=[&](int32 Quantity,FVector2D Destination,bool ChangeSelection=false)
    {
        SelectAmount(Quantity);
        TestTrue(TEXT("Real inventory mouse-down begins stack drag"),Gameplay->TryBeginInventoryDrag(Slot(Notes.Guid)));
        TestEqual(TEXT("Mouse-down keeps the dragged source GUID"),Gameplay->InvDragGuid,Notes.Guid);
        TestEqual(TEXT("Drag snapshots the selected quantity"),Gameplay->InvDragAmount,Quantity);
        Gameplay->UpdateInventoryDrag(Destination);
        if(ChangeSelection) Gameplay->SelectInventoryGuid(Other.Guid);
        Session.CachedC2SPackets.Reset();
        TestTrue(TEXT("Stack drop is handled"),Gameplay->TryFinishInventoryDrag(Destination));
    };
    const int32 EmptyIndex=Gameplay->InventorySlotGuids.IndexOfByKey(0);
    if (TestTrue(TEXT("Inventory exposes empty drop cells"),EmptyIndex!=INDEX_NONE))
    {
        const FVector2D Empty=Center(Gameplay->InventorySlotBgs[EmptyIndex]);
        Drag(7,Empty,true);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Player.Guid),3,7});
        TestEqual(TEXT("Client never predicts the source count before server acknowledgment"),Session.WorldObjects[Notes.Guid].StackSize,250);
        Drag(7,Slot(Other.Guid));
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Player.Guid),2,7});
        Drag(250,Slot(Other.Guid));
        LastWire(ACEGameAction::PutItemInContainer,{uint32(Notes.Guid),uint32(Player.Guid),1});
        Drag(7,Slot(Match.Guid));
        LastWire(ACEGameAction::StackableMerge,{uint32(Notes.Guid),uint32(Match.Guid),7});
        TestEqual(TEXT("Retail merge selects the receiving stack"),Client->GetSelectedObject().Guid,Match.Guid);
        Drag(25,Slot(Match.Guid));
        LastWire(ACEGameAction::StackableMerge,{uint32(Notes.Guid),uint32(Match.Guid),3});
        StackReply(Match.Guid,250);
        Drag(7,Slot(Match.Guid));
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Player.Guid),1,7});
        StackReply(Match.Guid,240);
        SelectAmount(7); Gameplay->TryBeginInventoryDrag(Slot(Notes.Guid));
        Gameplay->UpdateInventoryDrag(Empty); Session.CachedC2SPackets.Reset();
        Gameplay->TryFinishInventoryDrag(Slot(Notes.Guid));
        TestTrue(TEXT("Returning a partial drag to its own slot cancels without sending"),Session.CachedC2SPackets.IsEmpty());

        // Typing then immediately dragging must not require Enter first.
        auto InputWindow=SNew(SVirtualWindow).Size(FVector2D(ScreenSize));
        InputWindow->SetIsFocusable(true); InputWindow->SetContent(Slate);
        FSlateApplication::Get().RegisterVirtualWindow(InputWindow);
        SelectAmount(250); Gameplay->RefreshSelectionOverlay();
        FSlateApplication::Get().SetKeyboardFocus(Gameplay->StackAmountEntry->TakeWidget());
        Gameplay->StackAmountEntry->SetText(FText::FromString(TEXT("9")));
        TestTrue(TEXT("Quantity entry owns focus before immediate drag"),Gameplay->StackAmountEntry->HasKeyboardFocus());
        Gameplay->TryBeginInventoryDrag(Slot(Notes.Guid));
        TestEqual(TEXT("Uncommitted typed number is captured before selection refresh"),Gameplay->InvDragAmount,9);
        Gameplay->UpdateInventoryDrag(Empty); Session.CachedC2SPackets.Reset(); Gameplay->TryFinishInventoryDrag(Empty);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Player.Guid),3,9});
        FSlateApplication::Get().ClearKeyboardFocus(); FSlateApplication::Get().UnregisterVirtualWindow(InputWindow);
    }
    const int32 BagIndex=Gameplay->PackSlotGuids.IndexOfByKey(Bag.Guid);
    if (TestTrue(TEXT("Side backpack has a drop target"),BagIndex!=INDEX_NONE))
    {
        Drag(7,Center(Gameplay->PackSlots[BagIndex]));
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Bag.Guid),0,7});
        Drag(250,Center(Gameplay->PackSlots[BagIndex]));
        LastWire(ACEGameAction::PutItemInContainer,{uint32(Notes.Guid),uint32(Bag.Guid),0});
        Drag(7,Center(Gameplay->PackSlots[0]));
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Player.Guid),0,7});
    }
    FACEWorldObject Chest=Bag; Chest.Guid=99414; Chest.ContainerId=0;
    Session.WorldObjects.Add(Chest.Guid,Chest); Gameplay->ShowExternalContainer(Chest.Guid);
    CaptureScreen(TEXT("GameplayStackExternalContainer"));
    const auto ExtList=Manager->FindElementUnder(TEXT("ExternalContainer"),TEXT("Ext_Container_ItemList"));
    if(TestTrue(TEXT("Open external container has authored item list"),ExtList.IsValid()))
    {
        Drag(7,Canvas->LayoutToViewport(FVector2D(ExtList->GetScreenOrigin())+FVector2D(15,15)));
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Chest.Guid),0,7});
    }
    Gameplay->HideExternalContainer(false);
    // Select the newly created stack only after both halves of the split arrive.
    {
        auto BagStack=Match;BagStack.Guid=99419;BagStack.ContainerId=Bag.Guid;BagStack.StackSize=240;
        Session.WorldObjects.Add(BagStack.Guid,BagStack);Session.WorldObjects[Bag.Guid].ItemsCapacity=1;
        TestTrue(TEXT("A full bag accepts a selected amount that fits its matching stack"),Gameplay->CanMoveInventoryAmountToContainer(Notes.Guid,Bag.Guid,7));
        TestFalse(TEXT("A full bag rejects a quantity larger than remaining stack room"),Gameplay->CanMoveInventoryAmountToContainer(Notes.Guid,Bag.Guid,11));
        Session.CachedC2SPackets.Reset();Gameplay->MoveInventoryAmount(Notes.Guid,Bag.Guid,0,7);
        LastWire(ACEGameAction::StackableMerge,{uint32(Notes.Guid),uint32(BagStack.Guid),7});
        TestEqual(TEXT("Bag auto-merge selects the surviving stack"),Client->GetSelectedObject().Guid,BagStack.Guid);
        TestEqual(TEXT("Bag auto-merge waits for the server quantity"),Session.WorldObjects[BagStack.Guid].StackSize,240);
        Session.TradeSelfItems.Add(BagStack.Guid);
        TestFalse(TEXT("An offered stack cannot make a full bag accept a merge"),Gameplay->CanMoveInventoryAmountToContainer(Notes.Guid,Bag.Guid,7));
        Session.TradeSelfItems.Reset();Session.WorldObjects.Remove(BagStack.Guid);Session.WorldObjects[Bag.Guid].ItemsCapacity=24;
    }
    SelectAmount(7); Gameplay->MoveInventoryAmount(Notes.Guid,Bag.Guid,0,7);
    auto SplitPart=Notes; SplitPart.Guid=99420; SplitPart.ContainerId=Bag.Guid; SplitPart.StackSize=7;
    Session.WorldObjects.Add(SplitPart.Guid,SplitPart); Gameplay->UpdateInventorySplitSelection();
    TestEqual(TEXT("Split creation alone does not change selection"),Client->GetSelectedObject().Guid,Notes.Guid);
    Session.WorldObjects[Notes.Guid].StackSize=243; Gameplay->UpdateInventorySplitSelection();
    TestEqual(TEXT("Confirmed backpack split selects the newly created stack"),Client->GetSelectedObject().Guid,SplitPart.Guid);
    Session.WorldObjects[Notes.Guid]=Notes;
    auto LootStack=Notes; LootStack.Guid=99421; LootStack.ContainerId=Chest.Guid;
    Session.WorldObjects.Add(LootStack.Guid,LootStack); Session.CachedC2SPackets.Reset();
    Gameplay->MoveInventoryAmount(LootStack.Guid,Player.Guid,0,7);
    LastWire(ACEGameAction::StackableMerge,{uint32(LootStack.Guid),uint32(Match.Guid),7});
    const auto SavedControllerBinder=Controller->DatGameplayBinder;
    Controller->DatGameplayBinder=Gameplay;
    Gameplay->SelectInventoryGuid(LootStack.Guid); Gameplay->SelectedStackAmount=7;
    Session.CachedC2SPackets.Reset(); Controller->InteractWithObject(LootStack.Guid);
    LastWire(ACEGameAction::StackableMerge,{uint32(LootStack.Guid),uint32(SplitPart.Guid),7});
    // Selecting another item while walking to a world stack cannot change its amount.
    Gameplay->SelectInventoryGuid(LootStack.Guid); Gameplay->SelectedStackAmount=7;
    Session.WorldObjects[LootStack.Guid].ContainerId=0;
    Controller->BeginUseApproach(LootStack.Guid,.6f,true);
    TestEqual(TEXT("Pickup approach captures the requested quantity"),Controller->ApproachPickupAmount,7);
    Gameplay->SelectInventoryGuid(Other.Guid);
    Session.CachedC2SPackets.Reset();
    Controller->SendInventoryPickup(LootStack.Guid,Controller->ApproachPickupAmount);
    LastWire(ACEGameAction::StackableMerge,{uint32(LootStack.Guid),uint32(SplitPart.Guid),7});
    Controller->ClearServerMoveTo();
    // Retail PlaceInBackpack: option 0x29 chooses main vs the open owned bag.
    // Exercise reliable action payloads, including quantity, rather than just the resolver.
    Session.CharacterOptions2&=~0x00004000u;
    StackReply(Match.Guid,250);StackReply(SplitPart.Guid,250);
    Session.WorldObjects[LootStack.Guid]=LootStack;
    Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayPickupBag"));
    if (TestTrue(TEXT("Pickup fixture can select its side bag"),BagIndex!=INDEX_NONE))
    {
        TestTrue(TEXT("Clicking side bag selects the pickup destination"),Gameplay->TryBeginInventoryDrag(Center(Gameplay->PackSlots[BagIndex])));
        Gameplay->CancelInventoryDrag();
        TestEqual(TEXT("Own bag remains open after releasing its icon"),Gameplay->SelectedPackGuid,Bag.Guid);
    }
    Gameplay->SelectInventoryGuid(LootStack.Guid); Gameplay->SelectedStackAmount=7;
    Session.CachedC2SPackets.Reset(); Controller->InteractWithObject(LootStack.Guid);
    LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Bag.Guid),0,7});
    Gameplay->SelectedStackAmount=250; Session.CachedC2SPackets.Reset(); Controller->InteractWithObject(LootStack.Guid);
    LastWire(ACEGameAction::PutItemInContainer,{uint32(LootStack.Guid),uint32(Bag.Guid),0});
    Gameplay->SelectedStackAmount=7; Session.WorldObjects[LootStack.Guid].ContainerId=0;
    Controller->BeginUseApproach(LootStack.Guid,.6f,true); Gameplay->SelectInventoryGuid(Other.Guid);
    Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,Controller->ApproachPickupAmount);
    LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Bag.Guid),0,7});
    Controller->ClearServerMoveTo(); Session.WorldObjects[LootStack.Guid]=LootStack;
    Gameplay->ShowExternalContainer(Chest.Guid); CaptureScreen(TEXT("GameplayPickupLootBag"));
    const int32 LootIndex=Gameplay->ExtItemGuids.IndexOfByKey(LootStack.Guid);
    if (TestTrue(TEXT("Loot is present for double-click pickup"),LootIndex!=INDEX_NONE))
    {
        const FVector2D Point=Center(Gameplay->ExtItemSlots[LootIndex]);
        Gameplay->SelectInventoryGuid(LootStack.Guid); Gameplay->SelectedStackAmount=7;
        Gameplay->LastInvClickGuid=LootStack.Guid; Gameplay->LastInvClickTime=FPlatformTime::Seconds();
        TestTrue(TEXT("Second loot press enters the double-click path"),Gameplay->TryBeginInventoryDrag(Point));
        Session.CachedC2SPackets.Reset(); Gameplay->TryFinishInventoryDrag(Point);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Bag.Guid),0,7});
        Gameplay->LastInvClickGuid=LootStack.Guid; Gameplay->LastInvClickTime=FPlatformTime::Seconds();
        Session.CachedC2SPackets.Reset(); Gameplay->TryHandleOverlayClick(Point,false);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Bag.Guid),0,7});
    }
    Gameplay->HideExternalContainer(false);
    Gameplay->ShowPanelPage(TEXT("OptionsPanel_Field")); Gameplay->SyncOptionsPanelTab(TEXT("CharacterSettingsPage"));
    Gameplay->OptionsScrollOffset=0; Gameplay->RefreshOptionsOverlays(); CaptureScreen(TEXT("GameplayPickupOption"));
    const int32 PickupOptionRow=Gameplay->OptionRowOptions.IndexOfByKey(0x29);
    const auto OptionList=Manager->FindElementUnder(TEXT("CharacterSettingsPage"),TEXT("CharacterOptionsListBox"));
    if (TestTrue(TEXT("Retail pickup checkbox exists"),PickupOptionRow!=INDEX_NONE && OptionList.IsValid()))
    {
        const FVector2D Checkbox=FVector2D(OptionList->GetScreenOrigin())+FVector2D(8,PickupOptionRow*16+8);
        TestTrue(TEXT("Retail pickup checkbox accepts a click"),Gameplay->TryHandleOptionsListClick(Checkbox));
        TestFalse(TEXT("Checkbox draft does not change pickup before Apply"),Client->IsCharacterOptionSet(0x29));
        Session.CachedC2SPackets.Reset(); Gameplay->HandleOptionsNamedClick(TEXT("ApplyButton"));
        TestTrue(TEXT("Apply enables the persisted main-pack option"),Client->IsCharacterOptionSet(0x29));
        Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,7);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Player.Guid),0,7});
        Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,250);
        LastWire(ACEGameAction::PutItemInContainer,{uint32(LootStack.Guid),uint32(Player.Guid),0});
        // Explicit drag destinations still override the pickup preference.
        Session.CachedC2SPackets.Reset(); Gameplay->MoveInventoryAmount(LootStack.Guid,Bag.Guid,0,7);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Bag.Guid),0,7});
        Gameplay->TryHandleOptionsListClick(Checkbox); Gameplay->HandleOptionsNamedClick(TEXT("ApplyButton"));
        TestFalse(TEXT("Applying the unchecked option restores selected-bag pickup"),Client->IsCharacterOptionSet(0x29));
        Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,7);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Bag.Guid),0,7});
    }
    // Invalid/full preferences fall back in retail order; ordinary bags cannot nest.
    for (int32 InvalidPack : {0,Chest.Guid,Notes.Guid,99499})
        TestEqual(TEXT("Unavailable or non-owned pickup destination falls back to main"),Client->ResolvePickupContainer(LootStack.Guid,InvalidPack,7),Player.Guid);
    Session.TradeSelfItems.Add(Bag.Guid);
    TestEqual(TEXT("A bag offered in trade cannot receive pickups"),Client->ResolvePickupContainer(LootStack.Guid,Bag.Guid,7),Player.Guid);
    Session.TradeSelfItems.Remove(Bag.Guid);
    Session.WorldObjects[Bag.Guid].ItemsCapacity=1; // Confirmed split already occupies its only slot.
    Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,7);
    LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Player.Guid),0,7});
    auto Spare=Bag; Spare.Guid=99422; Session.WorldObjects.Add(Spare.Guid,Spare);
    const auto PlayerRecord=Session.WorldObjects[Player.Guid];
    Session.WorldObjects[Player.Guid].ItemsCapacity=3;
    Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,7);
    LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Spare.Guid),0,7});
    auto GroundBag=Bag; GroundBag.Guid=99423; GroundBag.ContainerId=0;
    Session.WorldObjects.Add(GroundBag.Guid,GroundBag); Session.WorldObjects[Player.Guid].ContainersCapacity=7;
    Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(GroundBag.Guid);
    LastWire(ACEGameAction::PutItemInContainer,{uint32(GroundBag.Guid),uint32(Player.Guid),0});
    // Full selected bag can still receive a compatible stack, even when main has room.
    Session.WorldObjects[Player.Guid]=PlayerRecord;
    StackReply(SplitPart.Guid,7);
    Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,7);
    LastWire(ACEGameAction::StackableMerge,{uint32(LootStack.Guid),uint32(SplitPart.Guid),7});
    // Ordinary cell drops insert at the requested position instead of searching
    // another bag for an unrelated matching stack.
    Session.CachedC2SPackets.Reset(); Gameplay->MoveInventoryAmount(LootStack.Guid,Player.Guid,0,7,false);
    LastWire(ACEGameAction::StackableSplitToContainer,{uint32(LootStack.Guid),uint32(Player.Guid),0,7});
    auto& MovingStack=Session.WorldObjects[LootStack.Guid];
    const int32 BeforeContainer=MovingStack.ContainerId;
    MovingStack.ContainerId=Bag.Guid;
    Session.CachedC2SPackets.Reset(); Controller->SendInventoryPickup(LootStack.Guid,7);
    LastWire(ACEGameAction::StackableMerge,{uint32(LootStack.Guid),uint32(SplitPart.Guid),7});
    MovingStack.ContainerId=BeforeContainer;
    const auto SplitSpace=Client->PlanInventoryPickup(LootStack.Guid,Bag.Guid,7,false);
    TestEqual(TEXT("A split that needs a new stack uses a free slot"),SplitSpace.Container,Player.Guid);
    TestEqual(TEXT("Split preparation never substitutes an existing stack"),SplitSpace.MergeTarget,0);
    Session.WorldObjects[Bag.Guid]=Bag;
    Session.WorldObjects.Remove(Spare.Guid); Session.WorldObjects.Remove(GroundBag.Guid);
    Session.CharacterOptions2=SavedOptions2;
    Gameplay->SelectedPackGuid=Player.Guid; Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));
    Gameplay->RefreshInventoryOverlays(); CaptureScreen(TEXT("GameplayStackTransfersAfterPickup"));
    Controller->DatGameplayBinder=SavedControllerBinder;
    Session.WorldObjects.Remove(LootStack.Guid); Session.WorldObjects.Remove(SplitPart.Guid);
    SelectAmount(7); Session.WorldObjects[Notes.Guid].StackSize=3; Gameplay->RefreshSelectionOverlay();
    TestEqual(TEXT("Server count changes clamp the selected amount"),Gameplay->SelectedStackAmount,3);
    TestEqual(TEXT("Server count changes update the slider maximum"),Gameplay->SelectedStackMax,3);
    Session.WorldObjects[Notes.Guid]=Notes;

    FACEWorldObject Friend; Friend.Guid=99415; Friend.Name=TEXT("Stack trade partner"); Friend.bIsPlayer=true;
    Session.WorldObjects.Add(Friend.Guid,Friend); Session.TradePartnerGuid=Friend.Guid;
    Gameplay->ShowTradePanel(Friend.Guid); CaptureScreen(TEXT("GameplayStackSecureTrade"));
    SelectAmount(7); Gameplay->TryBeginInventoryDrag(Slot(Notes.Guid));
    const auto TradeList=Manager->FindElementByName(TEXT("TradeSelfItemsList"));
    // The named list varies between DATs, so the authored whole trade root also accepts drops.
    const auto TradeRoot=TradeList ? TradeList : Manager->FindElementByName(TEXT("SecureTrade"));
    if(TestTrue(TEXT("Secure trade drop area exists"),TradeRoot.IsValid()))
    {
        const FVector2D Point=Canvas->LayoutToViewport(FVector2D(TradeRoot->GetScreenOrigin())+FVector2D(20,20));
        Gameplay->UpdateInventoryDrag(Point); Session.CachedC2SPackets.Reset(); Gameplay->TryFinishInventoryDrag(Point);
        LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Player.Guid),0,7});
        TestFalse(TEXT("Original stack is not offered before split confirmation"),HasAction(ACEGameAction::AddToTrade));
        auto Created=Notes; Created.Guid=99416; Created.StackSize=7;
        Session.WorldObjects.Add(Created.Guid,Created); Gameplay->UpdateTradeStackSplit();
        TestFalse(TEXT("Created-only split update cannot offer items"),HasAction(ACEGameAction::AddToTrade));
        Session.WorldObjects[Notes.Guid].StackSize=243; Gameplay->UpdateTradeStackSplit();
        LastWire(ACEGameAction::AddToTrade,{uint32(Created.Guid),0});
        const int32 SentCount=Session.CachedC2SPackets.Num(); Gameplay->UpdateTradeStackSplit();
        TestEqual(TEXT("Confirmed split is offered exactly once"),Session.CachedC2SPackets.Num(),SentCount);

        Session.WorldObjects[Notes.Guid]=Notes; Session.CachedC2SPackets.Reset();
        Gameplay->AddInventoryGuidToTrade(Notes.Guid,250);
        LastWire(ACEGameAction::AddToTrade,{uint32(Notes.Guid),0});
        Session.CachedC2SPackets.Reset(); Gameplay->AddInventoryGuidToTrade(Notes.Guid,7);
        Gameplay->HandleNamedClick(TEXT("Trade_ClearAllButon"));
        TestEqual(TEXT("Clear All cancels pending split-to-trade"),Gameplay->TradeStackSplit.SourceGuid,0);
        TestFalse(TEXT("Clear All cannot offer the original stack"),HasAction(ACEGameAction::AddToTrade));
        Gameplay->AddInventoryGuidToTrade(Notes.Guid,7);
        Gameplay->TradeStackSplit.Deadline=0; Gameplay->UpdateTradeStackSplit();
        TestEqual(TEXT("Unconfirmed trade split expires without offering"),Gameplay->TradeStackSplit.SourceGuid,0);
        Gameplay->AddInventoryGuidToTrade(Notes.Guid,7); Gameplay->HideTradePanel(false);
        TestEqual(TEXT("Closing trade cancels split staging"),Gameplay->TradeStackSplit.SourceGuid,0);
        Gameplay->ShowTradePanel(Friend.Guid); Gameplay->AddInventoryGuidToTrade(Notes.Guid,7);
        Session.WorldObjects[Notes.Guid].StackSize=243; Session.CachedC2SPackets.Reset();
        Created.Guid=99417; Created.ContainerId=0; Session.WorldObjects.Add(Created.Guid,Created);
        Gameplay->UpdateTradeStackSplit(); TestFalse(TEXT("Uncontained split cannot be offered"),HasAction(ACEGameAction::AddToTrade));
        Session.WorldObjects[Created.Guid].ContainerId=Player.Guid; Gameplay->UpdateTradeStackSplit();
        LastWire(ACEGameAction::AddToTrade,{uint32(Created.Guid),0});
        Session.WorldObjects[Notes.Guid]=Notes; Gameplay->AddInventoryGuidToTrade(Notes.Guid,7);
        Session.WorldObjects[Notes.Guid].StackSize=243;
        Created.Guid=99418; Created.ContainerId=Player.Guid; Session.WorldObjects.Add(Created.Guid,Created);
        Created.Guid=99419; Session.WorldObjects.Add(Created.Guid,Created);
        Session.CachedC2SPackets.Reset(); Gameplay->UpdateTradeStackSplit();
        TestFalse(TEXT("Ambiguous new stacks are never guessed or offered"),HasAction(ACEGameAction::AddToTrade));
        TestEqual(TEXT("Ambiguous split ends automated staging"),Gameplay->TradeStackSplit.SourceGuid,0);
    }
    Gameplay->HideTradePanel(false); Session.TradePartnerGuid=0; Session.WorldObjects[Notes.Guid]=Notes;
    Session.CharacterOptions1|=0x04000000u; Session.CachedC2SPackets.Reset();
    TestTrue(TEXT("Retail drag-to-trade option opens negotiation"),Gameplay->TryOpenTradeForDraggedItem(Friend,Notes.Guid,7));
    LastWire(ACEGameAction::OpenTradeNegotiations,{uint32(Friend.Guid)});
    TestFalse(TEXT("Opening negotiation alone does not offer or split items"),HasAction(ACEGameAction::StackableSplitToContainer));
    Session.TradePartnerGuid=Friend.Guid; Gameplay->HandleTradeStateChanged(ACEGameEvent::RegisterTrade);
    LastWire(ACEGameAction::StackableSplitToContainer,{uint32(Notes.Guid),uint32(Player.Guid),0,7});
    Gameplay->HideTradePanel(false); Session.CharacterOptions1&=~0x04000000u;
    TestFalse(TEXT("Option off retains ordinary giving"),Gameplay->TryOpenTradeForDraggedItem(Friend,Notes.Guid,7));
    Session.WorldObjects=SavedObjects; Session.ContainerContents=SavedContents;
    Session.CharacterOptions1=SavedOptions; Session.CharacterOptions2=SavedOptions2; Session.TradePartnerGuid=SavedPartner;
    Gameplay->CancelInventoryDrag(); Gameplay->InventorySelectionSplit={};
    Session.SelectedObject=SavedSelection; Gameplay->HandleSelectionChanged(SavedSelection);
    Gameplay->SelectedPackGuid=Player.Guid;
    Gameplay->RefreshInventoryOverlays();
    Session.OnSelectionChanged.Remove(SelectionHandler);
};
CheckStackTransfers();
