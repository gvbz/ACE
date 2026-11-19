#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEInventoryRules.h"
#include "UI/ACEUIGameplayBinder.h"
#include "Engine/GameInstance.h"
#include "Protocol/ACEBinaryReader.h"
#include "Protocol/ACEBinaryWriter.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailInventoryOrderTest, "ACE.RetailParity.InventoryOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACERetailInventoryOrderTest::RunTest(const FString& Parameters)
{
    FACESession Session;
    auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("Inventory persistence fixture"),false);
    auto Address=Sockets->CreateInternetAddr(); bool Valid=false;
    Address->SetIp(TEXT("127.0.0.1"),Valid); Address->SetPort(0);
    if(!Receiver || !Receiver->Bind(*Address))
    {
        if(Receiver) Sockets->DestroySocket(Receiver);
        AddError(TEXT("Could not create isolated loopback inventory fixture")); return false;
    }
    Receiver->GetAddress(*Address);
    Session.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("Inventory persistence sender"),false);
    Session.ServerC2SAddr=Address; Session.IssacClient=MakeUnique<FACEIsaac>(123u);
    Session.PlayerGuid=100; Session.State=EACESessionState::InWorld;
    auto Item=[](int32 Id,int32 Container,int32 Place,bool Pack=false)
    {
        FACEWorldObject O; O.Guid=Id; O.ContainerId=Container; O.PlacementPosition=Place;
        O.ItemType=Pack ? ACEItemType::Container : ACEItemType::Misc;
        O.ItemsCapacity=Pack ? 24 : 0; return O;
    };
    for (const auto& O : {Item(1,100,0),Item(2,100,1),Item(3,100,2),Item(10,100,0,true),Item(20,100,1,true)})
        Session.UpsertWorldObject(O);
    auto Contains=[&](int32 Id,int32 Container,int32 Place,bool Pack=false)
    {
        FACEBinaryWriter Prop; Prop.WriteUInt8(1); Prop.WriteUInt32(Id); Prop.WriteUInt32(2); Prop.WriteUInt32(Container);
        FACEBinaryReader PR(Prop.GetData()); Session.HandlePublicUpdateInstanceId(PR);
        FACEBinaryWriter W; W.WriteUInt32(Id); W.WriteUInt32(Container); W.WriteInt32(Place); W.WriteInt32(Pack ? 1 : 0);
        FACEBinaryReader R(W.GetData()); Session.HandleInventoryPutObjInContainer(R);
    };
    auto Send=[&](int32 Id,int32 Container,int32 Position)
    {
        Session.CachedC2SPackets.Reset();
        Session.SendPutItemInContainer(Id,Container,Position);
        uint32 Sequence=0; for(const auto& Pair:Session.CachedC2SPackets) Sequence=FMath::Max(Sequence,Pair.Key);
        if(!TestTrue(TEXT("Inventory drag sends reliable server action"),Sequence!=0))return;
        FACEBinaryReader Wire(Session.CachedC2SPackets[Sequence].Payload); Wire.Skip(16);
        TestEqual(TEXT("Inventory move GameAction envelope"),Wire.ReadUInt32(),ACEOpcode::GameAction); Wire.ReadUInt32();
        TestEqual(TEXT("Inventory move action"),Wire.ReadUInt32(),ACEGameAction::PutItemInContainer);
        TestEqual(TEXT("Inventory move item"),Wire.ReadUInt32(),uint32(Id));
        TestEqual(TEXT("Inventory move container"),Wire.ReadUInt32(),uint32(Container));
        TestEqual(TEXT("Inventory move position"),Wire.ReadInt32(),Position);
    };
    auto Check=[&](int32 Container,TArray<int32> Expected,bool Packs=false)
    {
        TArray<FACEWorldObject> Actual;
        if (Packs) Session.GetPlayerPacks(Actual); else Session.GetPackItems(Container,Actual);
        TestEqual(TEXT("Server inventory count"),Actual.Num(),Expected.Num());
        for(int32 I=0;I<FMath::Min(Actual.Num(),Expected.Num());++I)
            TestEqual(FString::Printf(TEXT("Server inventory position %d"),I),Actual[I].Guid,Expected[I]);
    };
    Session.SendPutItemInContainer(1,10,0);
    TestTrue(TEXT("Inventory move remains pending until its own server receipt"),Session.IsInventoryTransferPending());
    Contains(2,100,1);
    TestTrue(TEXT("Unrelated inventory receipt does not release a manual move"),Session.IsInventoryTransferPending());
    Contains(1,10,0);
    TestFalse(TEXT("Confirmed containment releases inventory arbitration"),Session.IsInventoryTransferPending());
    Contains(1,100,0);
    // Two drags outstanding at once; ACK order is authoritative, without duplicate shifts.
    {
        auto Pickup=Item(500,900,0); Pickup.WeenieClassId=42; Pickup.MaxStackSize=100; Pickup.StackSize=12;
        auto Match=Item(501,10,0); Match.WeenieClassId=42; Match.MaxStackSize=100; Match.StackSize=50;
        auto Wrong=Match; Wrong.Guid=499; Wrong.WeenieClassId=43;
        Session.UpsertWorldObject(Pickup); Session.UpsertWorldObject(Match); Session.UpsertWorldObject(Wrong);
        Session.CachedC2SPackets.Reset(); Session.SendPutItemInContainer(500,100,0);
        uint32 Last=0; for(const auto& P:Session.CachedC2SPackets) Last=FMath::Max(Last,P.Key);
        if(TestTrue(TEXT("Pickup sends an inventory action"),Last!=0))
        {
            FACEBinaryReader R(Session.CachedC2SPackets[Last].Payload); R.Skip(24);
            TestEqual(TEXT("Pickup automatically merges into a nested pack"),R.ReadUInt32(),ACEGameAction::StackableMerge);
            TestEqual(TEXT("Merge source"),R.ReadUInt32(),500u);
            TestEqual(TEXT("Merge matches WCID"),R.ReadUInt32(),501u);
            TestEqual(TEXT("Merge full pickup quantity"),R.ReadUInt32(),12u);
        }
        TestEqual(TEXT("Merge waits for authoritative stack update"),Session.WorldObjects[501].StackSize,50);
        Session.WorldObjects[500].StackSize=0;
        Session.CachedC2SPackets.Reset(); Session.SendPutItemInContainer(500,100,0);
        Last=0; for(const auto& P:Session.CachedC2SPackets) Last=FMath::Max(Last,P.Key);
        if(TestTrue(TEXT("Single pickup with omitted count emits an action"),Last!=0))
        {
            FACEBinaryReader R(Session.CachedC2SPackets[Last].Payload);R.Skip(24);
            TestEqual(TEXT("Retail implicit single item still auto-merges"),R.ReadUInt32(),ACEGameAction::StackableMerge);
            R.Skip(8); TestEqual(TEXT("Omitted stack count means one"),R.ReadInt32(),1);
        }
        Session.WorldObjects[500].StackSize=2; Session.WorldObjects[501].StackSize=0; Session.WorldObjects[501].MaxStackSize=2;
        Send(500,100,3); // An implicit single item already occupies one of the two slots.
        Session.WorldObjects[500].StackSize=12; Session.WorldObjects[501].StackSize=50; Session.WorldObjects[501].MaxStackSize=100;
        Session.TradeSelfItems.Add(501); Send(500,100,3); Session.TradeSelfItems.Reset();
        Session.WorldObjects[501].StackSize=95; Send(500,100,3);
        Session.WorldObjects[501].StackSize=50; Session.WorldObjects[500].ContainerId=100;
        auto Merge=[&](int32 Container)
        {
            Session.CachedC2SPackets.Reset();Session.SendPutItemInContainer(500,Container,0);
            uint32 Sequence=0;for(const auto& P:Session.CachedC2SPackets)Sequence=FMath::Max(Sequence,P.Key);
            if(!TestTrue(TEXT("Bag transfer sends an action"),Sequence!=0))return;
            FACEBinaryReader R(Session.CachedC2SPackets[Sequence].Payload);R.Skip(24);
            TestEqual(TEXT("Moving an owned stack to another bag auto-merges"),R.ReadUInt32(),ACEGameAction::StackableMerge);
            TestEqual(TEXT("Owned merge source"),R.ReadUInt32(),500u);
            TestEqual(TEXT("Owned merge destination"),R.ReadUInt32(),501u);
            TestEqual(TEXT("Owned merge amount"),R.ReadUInt32(),12u);
        };
        Merge(10);
        TestEqual(TEXT("Auto-merge does not predict source containment"),Session.WorldObjects[500].ContainerId,100);
        Session.WorldObjects[500].ContainerId=10;Send(500,10,1); // Reordering within a bag stays a move.
        TestEqual(TEXT("A same-bag partial split is not auto-merged"),Session.FindContainerMergeTarget(500,10,7),0);
        Send(500,20,0); // Matching items in another bag are outside this destination.
        Session.WorldObjects[500].ContainerId=100;
        Session.TradeSelfItems.Add(10);Send(500,10,0);Session.TradeSelfItems.Reset();
        Session.WorldObjects[501].StackSize=95;Send(500,10,0);
        TestEqual(TEXT("A selected portion may fit when the whole stack does not"),Session.FindContainerMergeTarget(500,10,5),501);
        TestEqual(TEXT("Auto-merge requires room for all selected units"),Session.FindContainerMergeTarget(500,10,6),0);
        Session.WorldObjects[501].StackSize=50;
        Session.TradePartnerItems.Add(501);Send(500,10,0);Session.TradePartnerItems.Reset();
        for(int Id:{499,500,501}) Session.WorldObjects.Remove(Id);
    }
    Send(3,100,0); Send(2,100,0);
    Check(100,{1,2,3}); Contains(3,100,0); Contains(2,100,0); Check(100,{2,3,1});
    Send(3,20,0); Contains(3,20,0); Check(100,{2,1}); Check(20,{3});
    Send(20,100,0); Contains(20,100,0,true); Check(100,{20,10},true);
    // Failed put must not drop an owned item or undo another acknowledged move.
    Send(1,20,0);
    FACEBinaryWriter Failed; Failed.WriteUInt32(1); FACEBinaryReader FR(Failed.GetData()); Session.HandleInventoryServerSaveFailed(FR);
    Check(100,{2,1}); Check(20,{3});
    // Replay the ACE login footer from the saved inventory, with objects arriving later.
    Session.ClearWorldState(); Session.PlayerGuid=0;
    FACEBinaryWriter Login;
    Login.WriteUInt32(1); Login.WriteUInt32(0); // quality flags and weenie type
    Login.WriteUInt16(1); Login.WriteUInt16(1); Login.WriteUInt32(322); Login.WriteInt32(3);
    Login.WriteUInt32(0); Login.WriteUInt32(0); // vectors
    Login.WriteUInt32(0x20); // filter field is flagged, as in retail PlayerModule
    for(int I=0;I<3;++I) Login.WriteUInt32(0); // options, first bar, filters
    Login.WriteUInt32(4);
    for(const auto& Ref : {TPair<int32,int32>(2,0),{20,1},{1,0},{10,1}})
    { Login.WriteUInt32(Ref.Key); Login.WriteUInt32(Ref.Value); }
    Login.WriteUInt32(2); // equipped profile is independent of ObjectCreate order
    Login.WriteUInt32(30); Login.WriteUInt32(uint32(ACEEquipMask::Held)); Login.WriteUInt32(0);
    Login.WriteUInt32(40); Login.WriteUInt32(uint32(ACEEquipMask::Cloak)); Login.WriteUInt32(9);
    auto Caster=Item(30,0,-1); Caster.SpellDID=1;
    Session.UpsertWorldObject(Caster);
    Session.UpsertWorldObject(Item(1,100,0)); // one create precedes the footer
    FACEBinaryReader LR(Login.GetData()); Session.HandlePlayerDescription(LR);
    TestTrue(TEXT("Description before PlayerCreate retains the saved inventory"),Session.ContainerContents.Contains(0));
    TestEqual(TEXT("Login decodes aetheria unlocks from property 322"),Session.PlayerVitals.AetheriaUnlocked,3);
    FACEBinaryWriter Create; Create.WriteUInt32(100);
    Session.bLoginCompleteSent=true;
    FACEBinaryReader PlayerCreate(Create.GetData()); Session.HandlePlayerCreate(PlayerCreate);
    TestFalse(TEXT("PlayerCreate transfers the temporary inventory profile"),Session.ContainerContents.Contains(0));
    TestEqual(TEXT("Earlier caster create inherits equipped owner"),Session.WorldObjects[30].WielderId,100);
    TestEqual(TEXT("Earlier caster retains its innate spell"),Session.WorldObjects[30].SpellDID,1);
    Session.UpsertWorldObject(Item(40,0,-1));
    TestEqual(TEXT("Later cloak create retains equipped owner"),Session.WorldObjects[40].WielderId,100);
    TestEqual(TEXT("Later cloak create retains its location"),Session.WorldObjects[40].CurrentWieldedLocation,int64(ACEEquipMask::Cloak));
    TestEqual(TEXT("Later cloak create retains layer priority"),Session.WorldObjects[40].ClothingPriority,9u);
    TestEqual(TEXT("Missing earlier creates still reserve their saved position"),Session.WorldObjects[1].PlacementPosition,1);
    for(const auto& O : {Item(10,100,-1,true),Item(1,100,-1),Item(20,100,-1,true),Item(2,100,-1)}) Session.UpsertWorldObject(O);
    Check(100,{2,1}); Check(100,{20,10},true);
    FACEBinaryWriter Contents; Contents.WriteUInt32(20); Contents.WriteUInt32(2);
    Contents.WriteUInt32(9); Contents.WriteUInt32(0); Contents.WriteUInt32(3); Contents.WriteUInt32(0);
    FACEBinaryReader CR(Contents.GetData()); Session.HandleViewContents(CR);
    // Reverse ObjectCreate order used to destroy the saved side-bag order.
    Session.UpsertWorldObject(Item(3,20,-1)); Session.UpsertWorldObject(Item(9,20,-1));
    Check(20,{9,3});
    FACEBinaryWriter RootContents; RootContents.WriteUInt32(100); RootContents.WriteUInt32(4);
    for(const auto& Ref : {TPair<int32,int32>(10,1),{1,0},{20,1},{2,0}})
    { RootContents.WriteUInt32(Ref.Key); RootContents.WriteUInt32(Ref.Value); }
    FACEBinaryReader RR(RootContents.GetData()); Session.HandleViewContents(RR);
    Check(100,{1,2}); Check(100,{10,20},true);

    // A bag's ViewContents can arrive before its ownership or the login root
    // list. It must stop being an external container once either establishes
    // ownership, without deleting the inventory list through the UI close.
    for(bool OwnerObjectFirst:{false,true})
    {
        Session.ClearWorldState();Session.PlayerGuid=100;
        auto View=[&](int32 Guid,TArray<FACEContainerItemRef> Refs)
        {
            FACEBinaryWriter W;W.WriteUInt32(Guid);W.WriteUInt32(Refs.Num());
            for(const auto& Ref:Refs){W.WriteUInt32(Ref.ItemGuid);W.WriteUInt32(Ref.ContainerType);}
            FACEBinaryReader R(W.GetData());Session.HandleViewContents(R);
        };
        View(20,{{21,0}});
        const auto Closed=Session.OnCloseGroundContainer.AddLambda([&](int32 Guid){Session.ClearContainerContents(Guid);});
        if(OwnerObjectFirst)Session.UpsertWorldObject(Item(20,100,0,true));
        else View(100,{{20,1}});
        TestEqual(TEXT("Late bag ownership clears phantom external container"),Session.OpenExternalContainerGuid,0);
        TestTrue(TEXT("Closing phantom bag preserves its contents"),Session.ContainerContents.Contains(20));
        Session.UpsertWorldObject(Item(21,20,0));Check(20,{21});
        auto OpenableBag=Item(20,100,0,true);OpenableBag.ObjectDescriptionFlags|=ACEObjectDescFlag::Openable;
        Session.UpsertWorldObject(OpenableBag);View(20,{{21,0}});
        TestEqual(TEXT("Owned openable bag is never external loot"),Session.OpenExternalContainerGuid,0);
        View(700,{});Session.ClearContainerContents(700);
        TestEqual(TEXT("Local panel closure clears session access without server close"),Session.OpenExternalContainerGuid,0);
        Session.OnCloseGroundContainer.Remove(Closed);
    }
    // ACE sends corpse contents (including bags) before CreateObject; GDLE can
    // describe objects first. Both orders must expose the same root and ownership.
    for (bool ObjectsFirst : {false,true})
    {
        Session.ClearWorldState(); Session.PlayerGuid=100;
        auto CreateLoot=[&]()
        {
            Session.UpsertWorldObject(Item(702,0,-1));
            Session.UpsertWorldObject(Item(701,0,-1,true));
            Session.UpsertWorldObject(Item(703,0,-1));
        };
        auto View=[&](int32 Container,TArray<FACEContainerItemRef> Refs)
        {
            if (Container != 701) Session.BeginPendingUse(Container,0);
            FACEBinaryWriter W; W.WriteUInt32(Container); W.WriteUInt32(Refs.Num());
            for (const auto& Ref:Refs) {W.WriteUInt32(Ref.ItemGuid); W.WriteUInt32(Ref.ContainerType);}
            FACEBinaryReader R(W.GetData()); Session.HandleViewContents(R);
        };
        if (ObjectsFirst) CreateLoot();
        View(700,{{701,1},{703,0}});
        View(701,{{702,0}});
        if (!ObjectsFirst) CreateLoot();
        TestEqual(TEXT("Nested corpse bag retains the root as open container"),Session.OpenExternalContainerGuid,700);
        TestEqual(TEXT("Early/late bag create receives corpse membership"),Session.WorldObjects[701].ContainerId,700);
        TestEqual(TEXT("Early/late item create receives nested bag membership"),Session.WorldObjects[702].ContainerId,701);
        Check(701,{702}); Check(700,{703});
        TArray<int32> Packs; Session.GetContainerPackGuids(700,Packs);
        TestTrue(TEXT("External pack strip includes installed bags in both packet orders"),Packs==TArray<int32>{701});
        View(800,{});
        FACEBinaryWriter Close; Close.WriteUInt32(700); FACEBinaryReader C(Close.GetData()); Session.HandleCloseGroundContainer(C);
        TestEqual(TEXT("Old corpse close does not revoke the new corpse"),Session.OpenExternalContainerGuid,800);
        TestFalse(TEXT("Closed corpse contents are retired"),Session.ContainerContents.Contains(700));
        TestFalse(TEXT("Closed nested bag contents are retired"),Session.ContainerContents.Contains(701));
        Close=FACEBinaryWriter(); Close.WriteUInt32(800); FACEBinaryReader C2(Close.GetData()); Session.HandleCloseGroundContainer(C2);
        TestEqual(TEXT("Matching server close ends access immediately"),Session.OpenExternalContainerGuid,0);
        View(700,{{701,1},{703,0}});View(701,{{702,0}});
        Session.WorldObjects.Add(704,Item(704,100,0)); // already-picked-up inventory survives recall
        bool ClosedBeforePortal=false;
        const auto CloseHandle=Session.OnCloseGroundContainer.AddLambda([&](int32 Guid){if(Guid==700)ClosedBeforePortal=true;});
        const auto PortalHandle=Session.OnPlayerTeleportStarted.AddLambda([&]()
        {
            TestTrue(TEXT("Loot closes before portal presentation starts"),ClosedBeforePortal);
            TestEqual(TEXT("Recall revokes container access immediately"),Session.OpenExternalContainerGuid,0);
        });
        FACEBinaryWriter Teleport;Teleport.WriteUInt16(1);Teleport.Align();
        FACEBinaryReader T(Teleport.GetData());Session.HandlePlayerTeleport(T);
        TestFalse(TEXT("Recall clears nested loot lists"),Session.ContainerContents.Contains(700)||Session.ContainerContents.Contains(701));
        TestEqual(TEXT("Recall preserves items already picked up"),Session.WorldObjects[704].ContainerId,100);
        Session.OnCloseGroundContainer.Remove(CloseHandle);Session.OnPlayerTeleportStarted.Remove(PortalHandle);
    }
    // Stance changes delay dequipping on both ACE and GDLE. Only ContainID is the
    // completion event; the earlier public property update must not release Wield.
    for (int32 Conflicts : {1,2})
    {
        Session.ClearWorldState(); Session.PlayerGuid=100; Session.State=EACESessionState::InWorld;
        auto New=Item(900,100,0); New.ItemType=ACEItemType::MeleeWeapon;
        Session.UpsertWorldObject(New);
        auto Old=Item(901,0,-1); Old.WielderId=100; Old.CurrentWieldedLocation=ACEEquipMask::MeleeWeapon;
        Session.UpsertWorldObject(Old);
        if (Conflicts==2)
        {
            auto Shield=Old; Shield.Guid=902; Shield.CurrentWieldedLocation=ACEEquipMask::Shield;
            Session.UpsertWorldObject(Shield);
        }
        Session.CachedC2SPackets.Reset();
        auto LastAction=[&](uint32 Action,int32 Guid)
        {
            uint32 Seq=0; for(const auto& P:Session.CachedC2SPackets)Seq=FMath::Max(Seq,P.Key);
            if(!TestTrue(TEXT("Swap emitted a reliable action"),Seq!=0))return;
            FACEBinaryReader R(Session.CachedC2SPackets[Seq].Payload);R.Skip(24);
            TestEqual(TEXT("Swap action order"),R.ReadUInt32(),Action);
            TestEqual(TEXT("Swap action item"),R.ReadUInt32(),uint32(Guid));
        };
        const int64 Location=Conflicts==2?ACEEquipMask::TwoHanded:ACEEquipMask::MeleeWeapon;
        Session.SendGetAndWieldItem(900,Location);
        TestEqual(TEXT("Swap removes all conflicting hands"),Session.PendingEquipmentRemovals.Num(),Conflicts);
        TestTrue(TEXT("Swap keeps interaction busy while waiting for server"),Session.IsUseBusy());
        for(int32 I=0;I<Conflicts;++I)
        {
            const int32 Removing=Session.PendingEquipmentRemovals[0];
            LastAction(ACEGameAction::PutItemInContainer,Removing);
            const int32 Sent=Session.CachedC2SPackets.Num();
            FACEBinaryWriter Prop;Prop.WriteUInt8(1);Prop.WriteUInt32(Removing);Prop.WriteUInt32(2);Prop.WriteUInt32(100);
            FACEBinaryReader PR(Prop.GetData());Session.HandlePublicUpdateInstanceId(PR);
            TestEqual(TEXT("Early container property does not advance swap"),Session.CachedC2SPackets.Num(),Sent);
            Session.SendGetAndWieldItem(900,Location);
            TestEqual(TEXT("Repeated click does not duplicate pending swap"),Session.CachedC2SPackets.Num(),Sent);
            Contains(Removing,100,0);
        }
        LastAction(ACEGameAction::GetAndWieldItem,900);
        TestTrue(TEXT("All dequips complete before final equip"),Session.PendingEquipmentRemovals.IsEmpty());
        FACEBinaryWriter Wield;Wield.WriteUInt32(900);Wield.WriteInt32(int32(Location));
        FACEBinaryReader WR(Wield.GetData());Session.HandleWieldItem(WR);
        TestEqual(TEXT("Wield acknowledgement completes the swap"),Session.PendingEquipmentGuid,0);
        Session.WorldObjects[900].WielderId=0;Session.WorldObjects[900].CurrentWieldedLocation=0;Session.WorldObjects[900].ContainerId=100;
        Old.CurrentWieldedLocation=ACEEquipMask::MeleeWeapon;Session.UpsertWorldObject(Old);
        Session.SendGetAndWieldItem(900,Location);
        const int32 Removing=Session.PendingEquipmentRemovals[0];
        const int32 Sent=Session.CachedC2SPackets.Num();
        FACEBinaryWriter Failure;Failure.WriteUInt32(Removing);FACEBinaryReader R(Failure.GetData());Session.HandleInventoryServerSaveFailed(R);
        Contains(Removing,100,0);
        TestEqual(TEXT("Failed removal cannot later equip the requested item"),Session.CachedC2SPackets.Num(),Sent);
        TestEqual(TEXT("Failure cancels pending swap"),Session.PendingEquipmentGuid,0);
    }
    // Every launcher pair, including a matching-ammo swap, uses the same
    // acknowledged transaction for manual equips and UCM equip intents.
    for(int32 OldAmmo : {1,2,4})for(int32 NewAmmo : {1,2,4})
    {
        Session.ClearWorldState();Session.PlayerGuid=100;Session.State=EACESessionState::InWorld;
        auto New=Item(900,100,0);New.ItemType=ACEItemType::MissileWeapon;New.AmmoType=NewAmmo;Session.UpsertWorldObject(New);
        auto Old=Item(901,0,-1);Old.WielderId=100;Old.CurrentWieldedLocation=ACEEquipMask::MissileWeapon;
        Old.ItemType=ACEItemType::MissileWeapon;Old.AmmoType=OldAmmo;Session.UpsertWorldObject(Old);
        auto Ammo=Old;Ammo.Guid=902;Ammo.ItemType=0x20000;Ammo.CurrentWieldedLocation=ACEEquipMask::MissileAmmo;Ammo.StackSize=73;Session.UpsertWorldObject(Ammo);
        Session.CachedC2SPackets.Reset();Session.SendGetAndWieldItem(900,ACEEquipMask::MissileWeapon);
        TestEqual(TEXT("Only incompatible ammo is scheduled for removal"),Session.PendingEquipmentRemovals.Contains(902),OldAmmo!=NewAmmo);
        TestEqual(TEXT("Launcher and incompatible ammunition are both conflicts"),Session.PendingEquipmentRemovals.Num(),OldAmmo==NewAmmo?1:2);
        while(!Session.PendingEquipmentRemovals.IsEmpty())
        {
            const int32 Removing=Session.PendingEquipmentRemovals[0];
            TestFalse(TEXT("Wield waits for every removal acknowledgement"),Session.bPendingEquipmentWieldSent);
            Contains(Removing,100,0);
        }
        TestTrue(TEXT("Replacement is wielded after ammunition is stowed"),Session.bPendingEquipmentWieldSent);
        TestEqual(TEXT("Ammunition count is preserved"),Session.WorldObjects[902].StackSize,73);
        TestEqual(TEXT("Matching ammo stays equipped"),Session.WorldObjects[902].CurrentWieldedLocation,OldAmmo==NewAmmo?ACEEquipMask::MissileAmmo:int64(0));
        FACEBinaryWriter Wield;Wield.WriteUInt32(900);Wield.WriteInt32(int32(ACEEquipMask::MissileWeapon));
        FACEBinaryReader R(Wield.GetData());Session.HandleWieldItem(R);
        TestEqual(TEXT("Ranged swap completes"),Session.PendingEquipmentGuid,0);
    }
    // Full main pack: retail chooses the first side pack that can hold each
    // dequip, before sending it. Reconstruct normal ACE property + ContainID
    // replies and verify the old weapon is immediately usable without relog.
    for(bool HaveContents:{false,true})for(int32 Conflicts:{1,2})
    {
        Session.ClearWorldState();Session.PlayerGuid=100;Session.State=EACESessionState::InWorld;
        auto Self=Item(100,0,0);Self.ItemsCapacity=2;Session.UpsertWorldObject(Self);
        auto New=Item(900,100,0);New.ItemType=ACEItemType::MeleeWeapon;Session.UpsertWorldObject(New);
        Session.UpsertWorldObject(Item(950,100,1));
        auto Old=Item(901,0,-1);Old.ItemType=ACEItemType::MeleeWeapon;
        Old.WielderId=100;Old.CurrentWieldedLocation=ACEEquipMask::MeleeWeapon;Session.UpsertWorldObject(Old);
        if(Conflicts==2){auto Shield=Old;Shield.Guid=902;Shield.CurrentWieldedLocation=ACEEquipMask::Shield;Session.UpsertWorldObject(Shield);}
        // GUID order deliberately differs from pack placement order.
        auto Full=Item(10,100,0,true);Full.ItemsCapacity=1;Session.UpsertWorldObject(Full);
        Session.UpsertWorldObject(Item(11,10,0));
        auto Focus=Item(20,100,1,true);Focus.ItemsCapacity=0;Session.UpsertWorldObject(Focus);
        auto Traded=Item(30,100,2,true);Session.UpsertWorldObject(Traded);Session.TradeSelfItems.Add(30);
        auto First=Item(3000,100,3,true);First.ItemsCapacity=1;Session.UpsertWorldObject(First);
        auto Second=Item(1000,100,4,true);Second.ItemsCapacity=1;Session.UpsertWorldObject(Second);
        if(HaveContents){Session.ContainerContents.Add(3000,{});Session.ContainerContents.Add(1000,{});}
        const int64 Location=Conflicts==2?ACEEquipMask::TwoHanded:ACEEquipMask::MeleeWeapon;
        auto Last=[&]()
        {
            uint32 Seq=0;for(const auto& Packet:Session.CachedC2SPackets)Seq=FMath::Max(Seq,Packet.Key);
            TArray<uint32> Words;if(!Seq)return Words;
            FACEBinaryReader R(Session.CachedC2SPackets[Seq].Payload);R.Skip(24);
            while(R.CanRead(4))Words.Add(R.ReadUInt32());return Words;
        };
        Session.CachedC2SPackets.Reset();Session.SendGetAndWieldItem(900,Location);
        TMap<int32,int32> Stowed;
        for(int32 I=0;I<Conflicts;++I)
        {
            if(!TestTrue(TEXT("Full-pack swap retains its pending removal"),!Session.PendingEquipmentRemovals.IsEmpty()))break;
            const int32 Removing=Session.PendingEquipmentRemovals[0],Destination=I==0?3000:1000;
            const auto Wire=Last();if(!TestEqual(TEXT("Dequip request has item, container and position"),Wire.Num(),4))break;
            TestEqual(TEXT("Full-pack swap uses standard PutItemInContainer"),Wire[0],ACEGameAction::PutItemInContainer);
            TestEqual(TEXT("Dequip goes to first available non-traded pack in display order"),Wire[2],uint32(Destination));
            TestEqual(TEXT("Request does not predict item out of equipment"),Session.WorldObjects[Removing].WielderId,100);
            const int32 Before=Session.CachedC2SPackets.Num();
            FACEBinaryWriter Prop;Prop.WriteUInt8(1);Prop.WriteUInt32(Removing);Prop.WriteUInt32(2);Prop.WriteUInt32(Destination);
            FACEBinaryReader PR(Prop.GetData());Session.HandlePublicUpdateInstanceId(PR);
            TestEqual(TEXT("Side-pack property still waits for ContainID"),Session.CachedC2SPackets.Num(),Before);
            Contains(Removing,Destination,0);Stowed.Add(Removing,Destination);
            TArray<FACEWorldObject> StowedContents;Session.GetPackItems(Destination,StowedContents);
            TestTrue(TEXT("Removed equipment is immediately visible in the destination pack"),StowedContents.ContainsByPredicate([&](const auto& O){return O.Guid==Removing;}));
            TestEqual(TEXT("Removed equipment is no longer wielded"),Session.WorldObjects[Removing].CurrentWieldedLocation,int64(0));
        }
        const auto WieldWire=Last();
        if(TestTrue(TEXT("Side-pack acknowledgements release the final wield"),WieldWire.Num()>=3))
            TestEqual(TEXT("Swap sends GetAndWieldItem after all removals"),WieldWire[0],ACEGameAction::GetAndWieldItem);
        FACEBinaryWriter W;W.WriteUInt32(900);W.WriteInt32(int32(Location));FACEBinaryReader WR(W.GetData());Session.HandleWieldItem(WR);
        TestFalse(TEXT("Successful full-pack swap releases busy state"),Session.IsUseBusy());
        for(const auto& Pair:Stowed)TestEqual(TEXT("New wield cannot put stowed equipment back into main pack"),Session.WorldObjects[Pair.Key].ContainerId,Pair.Value);
        // Hotkey the old weapon immediately: the new weapon now fits the freed
        // main-pack slot, and the old GUID remains a normal equip target.
        Session.SendGetAndWieldItem(901,ACEEquipMask::MeleeWeapon);
        const auto Back=Last();if(TestEqual(TEXT("Switch back starts with a dequip"),Back.Num(),4))
            TestEqual(TEXT("Freed main-pack space is preferred again"),Back[2],100u);
        Contains(900,100,0);const auto BackWield=Last();
        if(TestTrue(TEXT("Old weapon can be hotkeyed without relogging"),BackWield.Num()>=3))
        {TestEqual(TEXT("Old weapon wield action"),BackWield[0],ACEGameAction::GetAndWieldItem);TestEqual(TEXT("Old weapon GUID preserved"),BackWield[1],901u);}
    }
    // No room for all conflicts must not remove even the first item. Also count
    // slot reservations whose ObjectCreate has not arrived yet.
    for(bool UnknownItem:{false,true})
    {
        Session.ClearWorldState();Session.PlayerGuid=100;Session.State=EACESessionState::InWorld;
        auto Self=Item(100,0,0);Self.ItemsCapacity=1;Session.UpsertWorldObject(Self);
        Session.UpsertWorldObject(Item(900,100,0));
        auto Old=Item(901,0,-1);Old.WielderId=100;Old.CurrentWieldedLocation=ACEEquipMask::MeleeWeapon;Session.UpsertWorldObject(Old);
        auto Shield=Old;Shield.Guid=902;Shield.CurrentWieldedLocation=ACEEquipMask::Shield;Session.UpsertWorldObject(Shield);
        auto Pack=Item(10,100,0,true);Pack.ItemsCapacity=1;Session.UpsertWorldObject(Pack);
        if(UnknownItem){FACEContainerItemRef Ref;Ref.ItemGuid=888;Ref.ContainerType=0;Session.ContainerContents.Add(10,{Ref});}
        FString Message;const auto Handle=Session.OnChatMessage.AddLambda([&](const FString& Text,const FString&,int32){Message=Text;});
        Session.CachedC2SPackets.Reset();Session.SendGetAndWieldItem(900,UnknownItem?ACEEquipMask::MeleeWeapon:ACEEquipMask::TwoHanded);
        TestEqual(TEXT("Insufficient capacity sends no destructive dequip"),Session.CachedC2SPackets.Num(),0);
        TestEqual(TEXT("Insufficient capacity preserves old equipped weapon"),Session.WorldObjects[901].WielderId,100);
        TestFalse(TEXT("No capacity does not leave a pending busy swap"),Session.IsUseBusy());
        TestTrue(TEXT("No capacity explains why the equipment change did not happen"),Message.Contains(TEXT("room")));
        Session.OnChatMessage.Remove(Handle);
    }
    for(bool CancelCombat:{false,true})
    {
        Session.ClearWorldState();Session.PlayerGuid=100;Session.State=EACESessionState::InWorld;
        Session.PlayerVitals.CombatMode=ACECombatMode::Magic;
        auto Old=Item(910,0,-1);Old.WielderId=100;Old.CurrentWieldedLocation=ACEEquipMask::Held;
        Session.UpsertWorldObject(Old);Session.UpsertWorldObject(Item(911,100,0));
        Session.SendGetAndWieldItem(911,ACEEquipMask::Held);
        Session.ApplyServerCombatMode(ACECombatMode::Melee);
        TestEqual(TEXT("Wand removal accepts the temporary unarmed melee state"),Session.PlayerVitals.CombatMode,int32(ACECombatMode::Melee));
        Contains(910,100,0);
        if(CancelCombat)Session.SendChangeCombatMode(ACECombatMode::NonCombat);
        FACEBinaryWriter W;W.WriteUInt32(911);W.WriteInt32(int32(ACEEquipMask::Held));
        FACEBinaryReader R(W.GetData());Session.HandleWieldItem(R);
        TestEqual(TEXT("Wand swap restores magic unless player explicitly leaves combat"),Session.PlayerVitals.CombatMode,
            int32(CancelCombat?ACECombatMode::NonCombat:ACECombatMode::Magic));
    }
    // Bulk gifts may clear the Container quality on the physics queue before
    // the ordered remove event. Follow removal with real reorder ACKs.
    for(bool HasList:{false,true})
    {
        Session.ClearWorldState();Session.PlayerGuid=100;Session.State=EACESessionState::InWorld;
        for(int32 Bag:{10,20})
        {
            Session.UpsertWorldObject(Item(Bag,100,Bag==10?0:1,true));
            TArray<FACEContainerItemRef> Refs;
            for(int32 Index=0;Index<14;++Index)
            {
                const int32 Id=Bag*100+Index;Session.UpsertWorldObject(Item(Id,Bag,Index));Refs.Add({Id,0});
            }
            if(HasList)Session.ContainerContents.Add(Bag,Refs);
            for(int32 Index=0;Index<14;Index+=2)
            {
                FACEBinaryWriter Property;Property.WriteUInt8(1);Property.WriteInt32(Bag*100+Index);Property.WriteUInt32(2);Property.WriteInt32(0);
                FACEBinaryReader PR(Property.GetData());Session.HandlePublicUpdateInstanceId(PR);
                FACEBinaryWriter Remove;Remove.WriteInt32(Bag*100+Index);
                FACEBinaryReader RemovalReader(Remove.GetData());Session.HandleInventoryRemoveObject(RemovalReader);
            }
            TArray<int32> Expected;for(int32 Index=1;Index<14;Index+=2)Expected.Add(Bag*100+Index);
            Check(Bag,Expected);
            for(int32 Index=0;Index<Expected.Num();++Index)
                TestEqual(TEXT("Bulk removal leaves dense authoritative placements"),Session.WorldObjects[Expected[Index]].PlacementPosition,Index);
            Contains(Bag*100+13,Bag,2);Expected.Remove(Bag*100+13);Expected.Insert(Bag*100+13,2);Check(Bag,Expected);
        }
    }
    // A wield removes an ID from retail's dense container list. Physics parent
    // and quality updates can arrive first, but must not lose that source slot.
    for (bool HasList : {false, true}) for (int32 EarlyMessage : {0, 1, 2})
    {
        Session.ClearWorldState(); Session.PlayerGuid=100; Session.State=EACESessionState::InWorld;
        Session.UpsertWorldObject(Item(100,0,INDEX_NONE));
        Session.UpsertWorldObject(Item(10,100,0,true));
        TArray<FACEContainerItemRef> Refs;
        for (int32 Index=0; Index<5; ++Index)
        {
            Session.UpsertWorldObject(Item(1000+Index,10,Index)); Refs.Add({1000+Index,0});
        }
        if (HasList) Session.ContainerContents.Add(10,Refs);
        if (EarlyMessage==1)
        {
            FACEBinaryWriter W; W.WriteUInt32(100); W.WriteUInt32(1001);
            W.WriteInt32(1); W.WriteInt32(1); W.WriteUInt16(0); W.WriteUInt16(1);
            FACEBinaryReader R(W.GetData()); Session.HandleParentEvent(R);
        }
        if (EarlyMessage==2)
        {
            FACEBinaryWriter W; W.WriteUInt8(1); W.WriteUInt32(1001); W.WriteUInt32(3); W.WriteUInt32(100);
            FACEBinaryReader R(W.GetData()); Session.HandlePublicUpdateInstanceId(R);
        }
        TestEqual(TEXT("Attachment preserves source container until ordered wield"),Session.WorldObjects[1001].ContainerId,10);
        Check(10,{1000,1001,1002,1003,1004});
        FACEBinaryWriter Wield; Wield.WriteUInt32(1001); Wield.WriteInt32(int32(ACEEquipMask::Held));
        FACEBinaryReader WR(Wield.GetData()); Session.HandleWieldItem(WR);
        Check(10,{1000,1002,1003,1004});
        TestEqual(TEXT("Wield closes the source placement gap"),Session.WorldObjects[1002].PlacementPosition,1);
        TestEqual(TEXT("Equipped item no longer has an inventory index"),Session.WorldObjects[1001].PlacementPosition,INDEX_NONE);
        // ACE sends physics Pickup before dequip containment. It must not insert
        // a temporary main-pack item (and shift unrelated items a second time).
        Session.UpsertWorldObject(Item(2000,100,0));
        Session.UpsertWorldObject(Item(2001,100,1));
        FACEBinaryWriter Pickup; Pickup.WriteUInt32(1001); Pickup.WriteUInt16(0); Pickup.WriteUInt16(2);
        FACEBinaryReader PR(Pickup.GetData()); Session.HandlePickupEvent(PR);
        TestEqual(TEXT("Visual detach waits for server destination"),Session.WorldObjects[1001].ContainerId,0);
        Contains(1001,10,2);
        Check(10,{1000,1002,1001,1003,1004}); Check(100,{2000,2001});
        TestEqual(TEXT("Dequip does not compact unrelated main-pack slots"),Session.WorldObjects[2001].PlacementPosition,1);
        // Reorder after equipping/dequipping, then consume an item and reorder again.
        Contains(1004,10,1); Check(10,{1000,1004,1002,1001,1003});
        FACEBinaryWriter Remove; Remove.WriteUInt32(1002);
        FACEBinaryReader RemoveReader(Remove.GetData()); Session.HandleInventoryRemoveObject(RemoveReader);
        Contains(1003,10,0); Check(10,{1003,1000,1004,1001});
        const TArray<int32> Expected = {1003,1000,1004,1001};
        for (int32 Index=0; Index<Expected.Num(); ++Index)
            TestEqual(TEXT("Subsequent drags retain dense placement indices"),Session.WorldObjects[Expected[Index]].PlacementPosition,Index);
    }
    // Both weapons can receive physical attachment before either ordered wield.
    // Compact the second pending item's source position when the first leaves.
    Session.ClearWorldState(); Session.PlayerGuid=100;
    for (int32 Index=0; Index<4; ++Index) Session.UpsertWorldObject(Item(3000+Index,100,Index));
    Session.WorldObjects[3000].ParentGuid=100;
    Session.WorldObjects[3002].ParentGuid=100;
    for (const auto& Equipped : {TPair<int32,int64>(3000,ACEEquipMask::MeleeWeapon),TPair<int32,int64>(3002,ACEEquipMask::Shield)})
    {
        FACEBinaryWriter Wield; Wield.WriteUInt32(Equipped.Key); Wield.WriteInt32(int32(Equipped.Value));
        FACEBinaryReader WieldReader(Wield.GetData()); Session.HandleWieldItem(WieldReader);
    }
    TestEqual(TEXT("Concurrent attachments do not leave gaps"),Session.WorldObjects[3003].PlacementPosition,1);
    Check(100,{3001,3003});

    // A bulk transfer completes while D is being dragged (mouse-down index 3).
    // Drops must still insert before the item actually under the cursor.
    Session.ClearWorldState(); Session.PlayerGuid=100; Session.State=EACESessionState::InWorld;
    for (int32 Index=0; Index<6; ++Index) Session.UpsertWorldObject(Item(4000+Index,100,Index));
    for (int32 Id : {4000,4001})
    {
        FACEBinaryWriter W; W.WriteUInt32(Id);
        FACEBinaryReader Removal(W.GetData()); Session.HandleInventoryRemoveObject(Removal);
    }
    TArray<int32> CurrentItems; Session.GetPackItemGuids(100,CurrentItems);
    const int32 DropIndex=ACEInventoryRules::DropInsertionIndex(CurrentItems,4003,4005,true);
    TestEqual(TEXT("Drop resolves source and target after intervening transfers"),DropIndex,2);
    Send(4003,100,DropIndex); Contains(4003,100,DropIndex);
    Check(100,{4002,4004,4003,4005});
    Session.GetPackItemGuids(100,CurrentItems);
    TestEqual(TEXT("Self drop cancels by ID even after source slot changed"),ACEInventoryRules::DropInsertionIndex(CurrentItems,4003,4003,true),INDEX_NONE);
    TestEqual(TEXT("A disappeared destination cancels safely"),ACEInventoryRules::DropInsertionIndex(CurrentItems,4003,4000,true),INDEX_NONE);
    TestEqual(TEXT("Empty-cell drop appends after source removal"),ACEInventoryRules::DropInsertionIndex(CurrentItems,4003,0,true),3);
    TestEqual(TEXT("Partial stack keeps its source slot"),ACEInventoryRules::DropInsertionIndex(CurrentItems,4003,4005,false),3);
    TestEqual(TEXT("Cross-pack drop does not remove a destination slot"),ACEInventoryRules::DropInsertionIndex(CurrentItems,5000,4005,true),3);
    // UIQueue bag snapshots can precede all SmartboxQueue descriptions and
    // PlayerDescription. Preserve both lists, including reverse GUID order.
    Session.ClearWorldState(); Session.PlayerGuid=100;
    auto View=[&](int32 Bag,const TArray<FACEContainerItemRef>& Refs)
    {
        FACEBinaryWriter W; W.WriteInt32(Bag); W.WriteUInt32(Refs.Num());
        for(const auto& Ref:Refs){W.WriteInt32(Ref.ItemGuid);W.WriteInt32(Ref.ContainerType);}
        FACEBinaryReader R(W.GetData()); Session.HandleViewContents(R);
    };
    View(10,{{109,0},{103,0},{107,0}});
    View(20,{{209,0},{203,0},{207,0}});
    TestEqual(TEXT("Unsolicited pack snapshots do not open ground containers"),Session.OpenExternalContainerGuid,0);
    Check(10,{109,103,107}); Check(20,{209,203,207});
    Session.ContainerContents.Add(100,{{10,1},{20,1}}); Session.ApplyPlayerInventoryProfile();
    for(int32 Bag:{10,20})
    {
        Session.UpsertWorldObject(Item(Bag,100,INDEX_NONE,true));
        // The middle item arrives first; absent descriptions must not move.
        Session.UpsertWorldObject(Item(Bag*10+3,Bag,INDEX_NONE));
        Check(Bag,{Bag*10+9,Bag*10+3,Bag*10+7});
        Contains(Bag*10+7,Bag,0);
        Check(Bag,{Bag*10+7,Bag*10+9,Bag*10+3});
        Session.UpsertWorldObject(Item(Bag*10+9,Bag,INDEX_NONE));
        Session.UpsertWorldObject(Item(Bag*10+7,Bag,INDEX_NONE));
        Check(Bag,{Bag*10+7,Bag*10+9,Bag*10+3});
        TArray<int32> Ordered;Session.GetPackItemGuids(Bag,Ordered);
        const int32 Position=ACEInventoryRules::DropInsertionIndex(Ordered,Bag*10+3,Bag*10+9,true);
        Send(Bag*10+3,Bag,Position);Contains(Bag*10+3,Bag,Position);
        Check(Bag,{Bag*10+7,Bag*10+3,Bag*10+9});
    }
    // Manual unequip uses the same room checks as automatic weapon swaps.
    // ACE may otherwise remove equipment and silently restore it on failure.
    for (bool HasList : {false, true})
    {
        Session.ClearWorldState(); Session.PlayerGuid=100; Session.State=EACESessionState::InWorld;
        auto Self=Item(100,0,0); Self.ItemsCapacity=1; Session.UpsertWorldObject(Self);
        auto Bag=Item(10,100,0,true); Bag.ItemsCapacity=1; Session.UpsertWorldObject(Bag);
        Session.UpsertWorldObject(Item(1,100,0));
        auto Weapon=Item(2,0,INDEX_NONE); Weapon.ItemType=ACEItemType::MeleeWeapon;
        Weapon.WielderId=100; Weapon.CurrentWieldedLocation=ACEEquipMask::MeleeWeapon;
        Session.UpsertWorldObject(Weapon);
        if (HasList) Session.ContainerContents.Add(100,{{1,0},{10,1}});
        Session.CachedC2SPackets.Reset(); Session.SendPutItemInContainer(2,100,0);
        uint32 Last=0; for (const auto& P:Session.CachedC2SPackets) Last=FMath::Max(Last,P.Key);
        if (TestTrue(TEXT("Manual unequip finds room outside full main pack"),Last!=0))
        {
            FACEBinaryReader R(Session.CachedC2SPackets[Last].Payload); R.Skip(24);
            TestEqual(TEXT("Manual unequip action"),R.ReadUInt32(),ACEGameAction::PutItemInContainer);
            TestEqual(TEXT("Manual unequip preserves item GUID"),R.ReadUInt32(),2u);
            TestEqual(TEXT("Manual unequip uses first available side pack"),R.ReadUInt32(),10u);
        }
        TestEqual(TEXT("Sending unequip does not predict ownership"),Session.WorldObjects[2].WielderId,100);
        Session.PendingInventoryTransfers.Reset();
        if (HasList) Session.ContainerContents.Add(10,{{99,0}}); // description not yet received
        else Session.UpsertWorldObject(Item(99,10,0));
        for (int32 Destination : {100,10})
        {
            Session.CachedC2SPackets.Reset(); Session.SendPutItemInContainer(2,Destination,0);
            TestEqual(TEXT("Full destination does not send destructive unequip"),Session.CachedC2SPackets.Num(),0);
            TestEqual(TEXT("Full destination keeps the weapon equipped"),Session.WorldObjects[2].WielderId,100);
        }
        // An attachment preceding WieldItem does not make free inventory space.
        Session.WorldObjects[1].ParentGuid=100;
        TestFalse(TEXT("Pending attachment still occupies its inventory slot"),Session.CanStowEquipmentInContainer(100));
    }

    // The physical model and UI ownership have independent message ordering.
    // Cover held casters/melee/shields/ammunition, both inventory list modes,
    // and primary/side-pack destinations without relying on relog recovery.
    for (bool HasList : {false,true}) for (int32 Destination : {100,10})
    for (int64 Location : {ACEEquipMask::MeleeWeapon,ACEEquipMask::Held,ACEEquipMask::Shield,ACEEquipMask::MissileAmmo})
    {
        Session.ClearWorldState(); Session.PlayerGuid=100; Session.State=EACESessionState::InWorld;
        Session.UpsertWorldObject(Item(100,0,0)); Session.UpsertWorldObject(Item(10,100,0,true));
        Session.UpsertWorldObject(Item(1,Destination,0));
        if (HasList) Session.ContainerContents.Add(Destination,{{1,0}});
        auto Weapon=Item(2,0,INDEX_NONE); Weapon.WielderId=100; Weapon.CurrentWieldedLocation=Location;
        Weapon.ParentGuid=100; Weapon.ParentLocation=1; Weapon.bHasPhysicsTimestamps=true;
        Session.UpsertWorldObject(Weapon);
        auto Quality=[&](bool Wielder,int32 Value)
        {
            FACEBinaryWriter W; W.WriteUInt8(1); W.WriteUInt32(2); W.WriteUInt32(Wielder?3:10); W.WriteInt32(Value);
            FACEBinaryReader R(W.GetData());
            if (Wielder) Session.HandlePublicUpdateInstanceId(R); else Session.HandlePublicUpdatePropertyInt(R);
        };
        auto Pickup=[&](uint16 Stamp)
        {
            FACEBinaryWriter W; W.WriteUInt32(2); W.WriteUInt16(0); W.WriteUInt16(Stamp);
            FACEBinaryReader R(W.GetData()); Session.HandlePickupEvent(R);
        };
        auto Wield=[&]()
        {
            FACEBinaryWriter W; W.WriteUInt32(2); W.WriteInt32(int32(Location));
            FACEBinaryReader R(W.GetData()); Session.HandleWieldItem(R);
        };
        Quality(true,0); Quality(false,0); Pickup(1);
        TestEqual(TEXT("Preliminary dequip messages retain acknowledged owner"),Session.WorldObjects[2].WielderId,100);
        TestEqual(TEXT("Physical detach retains acknowledged equipment location"),Session.WorldObjects[2].CurrentWieldedLocation,Location);
        Contains(2,Destination,0); Check(Destination,{2,1});
        TestEqual(TEXT("ContainID clears equipment location"),Session.WorldObjects[2].CurrentWieldedLocation,int64(0));
        Quality(true,100); Quality(false,int32(Location));
        TestEqual(TEXT("Late qualities cannot re-equip contained weapon"),Session.WorldObjects[2].WielderId,0);
        TestEqual(TEXT("Late qualities cannot invent a bag item's equipment location"),Session.WorldObjects[2].CurrentWieldedLocation,int64(0));
        Wield(); Check(Destination,{1});
        // Old pickup/quality zeroes can follow a newer ordered wield receipt.
        Pickup(2); Quality(true,0); Quality(false,0);
        TArray<FACEWorldObject> Equipped; Session.GetEquippedItems(Equipped);
        TestEqual(TEXT("Delayed detach cannot orphan newly equipped item"),Equipped.Num(),1);
        if (Equipped.Num()) TestEqual(TEXT("Equipped item retains exact location"),Equipped[0].CurrentWieldedLocation,Location);
        Contains(2,Destination,1); Pickup(3); Check(Destination,{1,2});
        Session.DeleteWorldObject(2); Check(Destination,{1,2});
        TestTrue(TEXT("World cleanup retains unequipped weapon description"),Session.WorldObjects.Contains(2));
    }
    // ACE consumes the final equipped missile through quality clears (UIQueue)
    // and ObjectDelete (SmartboxQueue), not InventoryRemoveObject. Exercise all
    // receipt orders, including a delete before either quality reaches the UI.
    for (int64 Location : {ACEEquipMask::MissileAmmo, ACEEquipMask::MissileWeapon})
    for (const auto& Order : TArray<TArray<int32>>{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}})
    {
        Session.ClearWorldState(); Session.PlayerGuid=100; Session.State=EACESessionState::InWorld;
        auto Ammo=Item(200,0,INDEX_NONE); Ammo.WielderId=100; Ammo.CurrentWieldedLocation=Location;
        Ammo.StackSize=1; Ammo.MaxStackSize=250; Ammo.bHasPhysicsTimestamps=true;
        Ammo.PhysicsTimestamps[ACEPhysicsTimeStamp::Instance]=7;
        Session.UpsertWorldObject(Ammo); Session.SelectObject(Ammo.Guid,true);
        const auto Revision=Session.InventoryDataRevision;
        for (int32 I=0; I<Order.Num(); ++I)
        {
            FACEBinaryWriter W;
            if(Order[I]==2)
            {
                W.WriteUInt32(200); W.WriteUInt16(7); W.Align();
                FACEBinaryReader R(W.GetData()); Session.HandleObjectDelete(R);
            }
            else
            {
                W.WriteUInt8(1); W.WriteUInt32(200); W.WriteUInt32(Order[I]==0?3:10); W.WriteInt32(0);
                FACEBinaryReader R(W.GetData());
                if(Order[I]==0)Session.HandlePublicUpdateInstanceId(R);else Session.HandlePublicUpdatePropertyInt(R);
            }
            TestEqual(TEXT("Only the complete server removal retires the last missile"),Session.WorldObjects.Contains(200),I<2);
        }
        TArray<FACEWorldObject> Equipped; Session.GetEquippedItems(Equipped);
        TestTrue(TEXT("Final missile releases its equipment slot"),Equipped.IsEmpty());
        TestEqual(TEXT("Consumed missile selection is cleared"),Session.SelectedObject.Guid,0);
        TestTrue(TEXT("Consumed ammo invalidates the inventory snapshot"),Session.InventoryDataRevision>Revision);
        TestTrue(TEXT("Completed removal leaves no pending evidence"),Session.EquipmentRemovalEvidence.IsEmpty());
        auto Refill=Ammo; Refill.Guid=201; Refill.StackSize=250; Refill.WielderId=0; Refill.CurrentWieldedLocation=0; Refill.ContainerId=100;
        Session.UpsertWorldObject(Refill);
        FACEBinaryWriter Wield; Wield.WriteUInt32(201); Wield.WriteInt32(int32(Location));
        FACEBinaryReader WR(Wield.GetData()); Session.HandleWieldItem(WR); Session.GetEquippedItems(Equipped);
        TestEqual(TEXT("Fresh ammo can occupy the slot without relogging"),Equipped.Num(),1);
        if(Equipped.Num())TestEqual(TEXT("Equipped ammo is the replacement stack"),Equipped[0].Guid,201);
    }
    // Wield receipts arriving before ObjectCreate must retain the acknowledged
    // equipment record. They must not invent a destination for other equipment.
    Session.ClearWorldState(); Session.PlayerGuid=100;
    auto Existing=Item(5,0,INDEX_NONE); Existing.WielderId=100; Existing.CurrentWieldedLocation=ACEEquipMask::Held;
    Session.UpsertWorldObject(Existing);
    FACEBinaryWriter EarlyWield; EarlyWield.WriteUInt32(6); EarlyWield.WriteInt32(int32(ACEEquipMask::Held));
    FACEBinaryReader EarlyWieldReader(EarlyWield.GetData()); Session.HandleWieldItem(EarlyWieldReader);
    TestTrue(TEXT("Early wield creates a partial inventory record"),Session.WorldObjects.Contains(6));
    TestEqual(TEXT("Wield does not relocate other items without their receipt"),Session.WorldObjects[5].ContainerId,0);
    Check(100,{});
    Session.UpsertWorldObject(Item(6,0,INDEX_NONE));
    TestEqual(TEXT("Late description retains early wield owner"),Session.WorldObjects[6].WielderId,100);
    TestEqual(TEXT("Late description retains early wield location"),Session.WorldObjects[6].CurrentWieldedLocation,int64(ACEEquipMask::Held));
    Contains(5,100,0); Check(100,{5});
    // Remote equipment still consumes public qualities normally.
    auto Remote=Item(7,0,INDEX_NONE); Remote.WielderId=200; Remote.CurrentWieldedLocation=ACEEquipMask::Held;
    Session.UpsertWorldObject(Remote);
    FACEBinaryWriter RemoteOwner; RemoteOwner.WriteUInt8(1); RemoteOwner.WriteUInt32(7); RemoteOwner.WriteUInt32(3); RemoteOwner.WriteUInt32(0);
    FACEBinaryReader RemoteOwnerReader(RemoteOwner.GetData()); Session.HandlePublicUpdateInstanceId(RemoteOwnerReader);
    FACEBinaryWriter RemoteLocation; RemoteLocation.WriteUInt8(1); RemoteLocation.WriteUInt32(7); RemoteLocation.WriteUInt32(10); RemoteLocation.WriteUInt32(0);
    FACEBinaryReader RemoteLocationReader(RemoteLocation.GetData()); Session.HandlePublicUpdatePropertyInt(RemoteLocationReader);
    TestEqual(TEXT("Remote unequip owner still updates"),Session.WorldObjects[7].WielderId,0);
    TestEqual(TEXT("Remote unequip location still updates"),Session.WorldObjects[7].CurrentWieldedLocation,int64(0));
    Session.SocketC2S->Close(); Sockets->DestroySocket(Session.SocketC2S); Session.SocketC2S=nullptr;
    Receiver->Close(); Sockets->DestroySocket(Receiver);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACENPCStackExchangeTest, "ACE.RetailParity.NPCStackExchange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACENPCStackExchangeTest::RunTest(const FString& Parameters)
{
    auto* GI = NewObject<UGameInstance>(); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); };
    auto* Client = GI->GetSubsystem<UACEClientSubsystem>();
    auto& Session = *Client->GetSession();
    auto* Binder = NewObject<UACEUIGameplayBinder>(); Binder->Client = Client;
    auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto* Receiver = Sockets->CreateSocket(NAME_DGram, TEXT("NPC stack exchange receiver"), false);
    auto Address = Sockets->CreateInternetAddr(); bool Valid = false;
    Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(0);
    if (!Receiver || !Receiver->Bind(*Address))
    {
        if (Receiver) Sockets->DestroySocket(Receiver);
        AddError(TEXT("Could not create isolated NPC stack exchange fixture")); return false;
    }
    ON_SCOPE_EXIT { Receiver->Close(); Sockets->DestroySocket(Receiver); };
    Receiver->GetAddress(*Address);
    Session.SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("NPC stack exchange sender"), false);
    if (!TestNotNull(TEXT("NPC exchange sender exists"), Session.SocketC2S)) return false;
    ON_SCOPE_EXIT
    {
        Session.SocketC2S->Close(); Sockets->DestroySocket(Session.SocketC2S); Session.SocketC2S = nullptr;
    };
    Session.ServerC2SAddr = Address; Session.IssacClient = MakeUnique<FACEIsaac>(123u);

    auto Item = [](int32 Guid, int32 Container, int32 Place, bool Pack = false)
    {
        FACEWorldObject O; O.Guid = Guid; O.ContainerId = Container; O.PlacementPosition = Place;
        O.ItemType = Pack ? ACEItemType::Container : ACEItemType::Misc;
        O.ItemsCapacity = Pack ? 24 : 0; O.StackSize = 1; O.IconId = 0x060011CB;
        return O;
    };
    auto StackUpdate = [&](uint8 Stamp, int32 Count)
    {
        FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::SetStackSize); W.WriteUInt8(Stamp);
        W.WriteUInt32(202); W.WriteUInt32(Count); W.WriteUInt32(Count * 100);
        Session.ReceiveGameMessage(W.GetData());
    };
    auto Give = [&](int32 Count)
    {
        Session.CachedC2SPackets.Reset(); Session.SendGiveObjectRequest(900, 202, Count);
        uint32 Last = 0; for (const auto& Pair : Session.CachedC2SPackets) Last = FMath::Max(Last, Pair.Key);
        if (!TestTrue(TEXT("NPC gift sends a reliable request"), Last != 0)) return;
        FACEBinaryReader R(Session.CachedC2SPackets[Last].Payload); R.Skip(24);
        TestEqual(TEXT("NPC gift uses GiveObjectRequest"), R.ReadUInt32(), ACEGameAction::GiveObjectRequest);
        TestEqual(TEXT("NPC gift retains recipient"), R.ReadUInt32(), 900u);
        TestEqual(TEXT("NPC gift retains original stack GUID"), R.ReadUInt32(), 202u);
        TestEqual(TEXT("NPC gift preserves the offered quantity"), R.ReadInt32(), Count);
    };
    auto CreateStack = [&](int32 Guid, const TCHAR* Name, int32 Count, int32 Value, int32 Container)
    {
        FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::ObjectCreate); W.WriteUInt32(Guid);
        W.WriteUInt8(0x11); W.WriteUInt8(0); W.WriteUInt8(0); W.WriteUInt8(0);
        W.WriteUInt32(0); W.WriteUInt32(0);
        for (int32 I = 0; I < ACEPhysicsTimeStamp::Count; ++I) W.WriteUInt16(1);
        W.Align(); W.WriteUInt32(0x7008); // Value, StackSize, MaxStackSize, Container.
        W.WriteString16L(Name); W.WriteUInt16(Guid == 202 ? 100 : 101); W.WriteUInt16(0x11CB);
        W.WriteUInt32(ACEItemType::Misc); W.WriteUInt32(0); W.Align();
        W.WriteUInt32(Value); W.WriteUInt16(Count); W.WriteUInt16(100); W.WriteUInt32(Container); W.Align();
        Session.ReceiveGameMessage(W.GetData());
    };
    auto Contain = [&](int32 Guid, int32 Container, int32 Place, uint32 Stamp)
    {
        FACEBinaryWriter W;
        W.WriteUInt32(ACEOpcode::GameEvent); W.WriteUInt32(100); W.WriteUInt32(Stamp);
        W.WriteUInt32(ACEGameEvent::InventoryPutObjInContainer);
        W.WriteUInt32(Guid); W.WriteUInt32(Container); W.WriteInt32(Place); W.WriteUInt32(0);
        Session.ReceiveGameMessage(W.GetData()); Session.DrainOutOfOrderS2C();
    };
    auto Reward = [&](int32 Place)
    {
        // ACE sends a real ObjectCreate followed by the ordered containment
        // receipt. The reward must not replace the offered stack.
        CreateStack(300, TEXT("Writ of Refuge"), 1, 1, 100); Contain(300, 100, Place, 1);
    };
    auto ContainerQuality = [&](uint8 Stamp, int32 Container)
    {
        FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::PublicUpdateInstanceId); W.WriteUInt8(Stamp);
        W.WriteUInt32(202); W.WriteUInt32(2); W.WriteUInt32(Container);
        Session.ReceiveGameMessage(W.GetData());
    };

    // Cover the main backpack, a side pack, and deeper ownership; reward
    // ObjectCreate and the stack quantity can arrive on independent streams.
    for (int32 Container : {100, 10, 20}) for (bool RewardFirst : {false, true})
    {
        Session.ClearWorldState(); Session.PlayerGuid = 100; Session.State = EACESessionState::InWorld;
        Session.UpsertWorldObject(Item(100, 0, INDEX_NONE, true));
        Session.UpsertWorldObject(Item(10, 100, 0, true));
        Session.UpsertWorldObject(Item(20, 10, 0, true));
        Session.UpsertWorldObject(Item(201, Container, 0));
        auto Trinkets = Item(202, Container, 1); Trinkets.Name = TEXT("Ancient Falatacot Trinket");
        Trinkets.StackSize = 3; Trinkets.MaxStackSize = 100; Trinkets.Value = 300; Trinkets.WeenieClassId = 100;
        Session.UpsertWorldObject(Trinkets); Session.UpsertWorldObject(Item(203, Container, 2));
        // Include authoritative inventory lists, so preserving only the
        // WorldObjects record cannot conceal a lost bag membership reference.
        for (int32 Bag : {100, 10, 20})
        {
            FACEBinaryWriter Contents; Contents.WriteUInt32(Bag);
            Contents.WriteUInt32((Bag == 20 ? 0 : 1) + (Bag == Container ? 3 : 0));
            if (Bag != 20) { Contents.WriteUInt32(Bag == 100 ? 10 : 20); Contents.WriteUInt32(1); }
            if (Bag == Container) for (int32 Guid : {201, 202, 203})
            { Contents.WriteUInt32(Guid); Contents.WriteUInt32(0); }
            FACEBinaryReader R(Contents.GetData()); Session.HandleViewContents(R);
        }
        Binder->SelectedPackGuid = Container;
        const uint64 BeforeGive = Binder->HashInventoryOverlayState();
        Give(3);
        TestEqual(TEXT("Offering the whole stack does not predict consumption"), Session.WorldObjects[202].StackSize, 3);
        TestEqual(TEXT("Offering leaves the inventory UI unchanged"), Binder->HashInventoryOverlayState(), BeforeGive);

        if (RewardFirst) Reward(Container == 100 ? 3 : 0);
        const uint64 BeforeReply = Binder->HashInventoryOverlayState();
        StackUpdate(11, 2);
        TestTrue(TEXT("Partial hand-in refreshes the inventory count immediately"), Binder->HashInventoryOverlayState() != BeforeReply);
        const uint64 AfterReply = Binder->HashInventoryOverlayState();
        StackUpdate(10, 3);
        FACEBinaryWriter Stale;
        Stale.WriteUInt32(ACEOpcode::PublicUpdatePropertyInt); Stale.WriteUInt8(10);
        Stale.WriteUInt32(202); Stale.WriteUInt32(12); Stale.WriteInt32(0);
        Session.ReceiveGameMessage(Stale.GetData());
        TestEqual(TEXT("Stale stack and quality replies cannot alter the visible remainder"), Binder->HashInventoryOverlayState(), AfterReply);
        if (!RewardFirst) Reward(Container == 100 ? 3 : 0);

        const auto Visible = Client->GetPackItems(Container);
        const auto* Remainder = Visible.FindByPredicate([](const auto& O) { return O.Guid == 202; });
        if (TestNotNull(TEXT("Remainder is visible in its original bag without relogging"), Remainder))
        {
            TestEqual(TEXT("One trinket is consumed per exchange"), Remainder->StackSize, 2);
            TestEqual(TEXT("Remainder retains server value"), Remainder->Value, 200);
            TestEqual(TEXT("Remainder retains original container"), Remainder->ContainerId, Container);
            TestEqual(TEXT("Remainder retains original inventory slot"), Remainder->PlacementPosition, 1);
        }
        TestEqual(TEXT("Nested remainder still belongs to the player"), Session.ResolveContainerRoot(202), 100);
        const auto MainPack = Client->GetPackItems(100);
        const auto* Writ = MainPack.FindByPredicate([](const auto& O) { return O.Guid == 300; });
        if (TestNotNull(TEXT("Reward create is visible alongside the remaining trinkets"), Writ))
        {
            TestEqual(TEXT("Exchange creates one writ"), Writ->StackSize, 1);
            TestEqual(TEXT("Reward name survives wire decode"), Writ->Name, FString(TEXT("Writ of Refuge")));
        }
        TestEqual(TEXT("Partial exchange does not shift the following item"), Session.WorldObjects[203].PlacementPosition, 2);

        StackUpdate(12, 1); Give(1);
        TestTrue(TEXT("Final item waits for authoritative removal"), Session.WorldObjects.Contains(202));
        const uint64 BeforeRemoval = Binder->HashInventoryOverlayState();
        FACEBinaryWriter Removed; Removed.WriteUInt32(ACEOpcode::InventoryRemoveObject); Removed.WriteUInt32(202);
        Session.ReceiveGameMessage(Removed.GetData());
        TestFalse(TEXT("Final hand-in removes only the consumed stack"), Session.WorldObjects.Contains(202));
        TestTrue(TEXT("Final hand-in invalidates the inventory UI"), Binder->HashInventoryOverlayState() != BeforeRemoval);
        TestFalse(TEXT("Final hand-in leaves no phantom inventory slot"), Client->GetPackItems(Container).ContainsByPredicate(
            [](const auto& O) { return O.Guid == 202; }));
        TestEqual(TEXT("Final removal compacts the following item"), Session.WorldObjects[203].PlacementPosition, 1);
        TestTrue(TEXT("Final removal preserves the earlier reward"), Session.WorldObjects.Contains(300));
        StackUpdate(11, 2);
        TestFalse(TEXT("Delayed stack update cannot resurrect a consumed item"), Session.WorldObjects.Contains(202));
    }

    // GDLE Monster::FinishGiveItem transfers the original GUID to the NPC.
    // OnReceiveInventoryItem then consumes one and SimulateGiveObject returns
    // the remainder using MakeAware/CreateObject plus Container IID=player;
    // no second player-owned InventoryMove is required. OBJECT_MSG and the
    // ordered PRIVATE_MSG can arrive in either order.
    for (int32 ReturnOrder : {0, 1, 2})
    {
        Session.ClearWorldState(); Session.PlayerGuid = 100; Session.State = EACESessionState::InWorld;
        Session.UpsertWorldObject(Item(100, 0, INDEX_NONE, true));
        Session.UpsertWorldObject(Item(900, 0, INDEX_NONE));
        Session.UpsertWorldObject(Item(201, 100, 0));
        CreateStack(202, TEXT("Ancient Falatacot Trinket"), 3, 300, 100);
        Session.WorldObjects[202].PlacementPosition = 1;
        Session.UpsertWorldObject(Item(203, 100, 2));
        FACEBinaryWriter Contents; Contents.WriteUInt32(100); Contents.WriteUInt32(3);
        for (int32 Guid : {201, 202, 203}) { Contents.WriteUInt32(Guid); Contents.WriteUInt32(0); }
        FACEBinaryReader R(Contents.GetData()); Session.HandleViewContents(R);
        Binder->SelectedPackGuid = 100; Give(3);
        ContainerQuality(1, 0); ContainerQuality(2, 0);
        TestEqual(TEXT("Preliminary GDLE quality clears retain source ownership"), Session.WorldObjects[202].ContainerId, 100);
        TestEqual(TEXT("Preliminary GDLE quality clears retain source slot"), Session.WorldObjects[202].PlacementPosition, 1);
        auto ReturnDescription = [&] { CreateStack(202, TEXT("Ancient Falatacot Trinket"), 2, 200, 100); };
        if (ReturnOrder == 0) ReturnDescription();
        Contain(202, 900, 0, 1);
        TestEqual(TEXT("GDLE whole-stack receipt transfers ownership to NPC"), Session.WorldObjects[202].ContainerId, 900);
        TestFalse(TEXT("Transferred stack is absent until server returns it"), Client->GetPackItems(100).ContainsByPredicate(
            [](const auto& O) { return O.Guid == 202; }));
        StackUpdate(11, 2);
        if (ReturnOrder == 1) ReturnDescription();
        const uint64 BeforeReturn = Binder->HashInventoryOverlayState();
        const uint64 BeforeRevision = Session.InventoryDataRevision;
        ContainerQuality(3, 100);
        if (ReturnOrder != 1)
        {
            TestTrue(TEXT("Unowned GDLE remainder returns on authoritative Container IID"),
                Binder->HashInventoryOverlayState() != BeforeReturn);
            TestTrue(TEXT("Container-only return invalidates cached plugin inventory"), Session.InventoryDataRevision > BeforeRevision);
            TestTrue(TEXT("Container-only return restores main-pack membership"), Session.ContainerContents[100].ContainsByPredicate(
                [](const auto& Ref) { return Ref.ItemGuid == 202; }));
            TestEqual(TEXT("Retail Container IID return inserts at slot zero"), Session.WorldObjects[202].PlacementPosition, 0);
            TestEqual(TEXT("Returned stack shifts the first surviving item"), Session.WorldObjects[201].PlacementPosition, 1);
            TestEqual(TEXT("Returned stack shifts the second surviving item"), Session.WorldObjects[203].PlacementPosition, 2);
        }
        if (ReturnOrder == 2) ReturnDescription();
        const auto ReturnedItems = Client->GetPackItems(100);
        const auto* Returned = ReturnedItems.FindByPredicate([](const auto& O) { return O.Guid == 202; });
        if (TestNotNull(TEXT("GDLE return keeps the remainder visible without relogging"), Returned))
        {
            TestEqual(TEXT("GDLE return contains the original stack GUID"), Returned->Guid, 202);
            TestEqual(TEXT("GDLE return leaves all but one trinket"), Returned->StackSize, 2);
            TestEqual(TEXT("GDLE return retains the adjusted value"), Returned->Value, 200);
            TestEqual(TEXT("GDLE return is owned by the player"), Returned->ContainerId, 100);
        }
        const uint64 AfterReturn = Binder->HashInventoryOverlayState();
        ContainerQuality(2, 900); // Stale retransmission must not undo the newer return.
        ContainerQuality(4, 100); // Already-owned updates must not reinsert/reorder it.
        TestEqual(TEXT("Stale or duplicate container qualities do not move the returned stack"), Binder->HashInventoryOverlayState(), AfterReturn);
        CreateStack(300, TEXT("Writ of Refuge"), 1, 1, 100); Contain(300, 100, 3, 2);
        TestTrue(TEXT("GDLE reward creation preserves returned stack"), Client->GetPackItems(100).ContainsByPredicate(
            [](const auto& O) { return O.Guid == 202 && O.StackSize == 2; }));
        TestTrue(TEXT("GDLE exchange also exposes its single reward"), Client->GetPackItems(100).ContainsByPredicate(
            [](const auto& O) { return O.Guid == 300 && O.StackSize == 1; }));
        if (ReturnOrder == 0)
        {
            ContainerQuality(5, 901); ContainerQuality(6, 0);
            TestEqual(TEXT("Preliminary foreign/zero IID updates cannot move an owned stack"), Session.WorldObjects[202].ContainerId, 100);
            Contain(202, 900, 0, 3);
            ContainerQuality(10, 901); ContainerQuality(9, 100);
            TestEqual(TEXT("Older return IID cannot override a newer foreign-container quality"), Session.WorldObjects[202].ContainerId, 900);
            ContainerQuality(12, 0); ContainerQuality(11, 100);
            TestEqual(TEXT("Older return IID cannot override a newer cleared-container quality"), Session.WorldObjects[202].ContainerId, 0);
            Contain(202, 900, 0, 4);
            Session.PendingInventoryTransfers.Add(202, FPlatformTime::Seconds());
            ContainerQuality(13, 100);
            TestEqual(TEXT("Container IID cannot complete a pending inventory request"), Session.WorldObjects[202].ContainerId, 900);
            TestTrue(TEXT("Container IID cannot clear pending inventory arbitration"), Session.PendingInventoryTransfers.Contains(202));
            Session.PendingInventoryTransfers.Remove(202);
            ContainerQuality(14, 100);
            TestEqual(TEXT("Unowned item can return after its pending request ends"), Session.WorldObjects[202].ContainerId, 100);
        }
    }
    return true;
}
#endif
