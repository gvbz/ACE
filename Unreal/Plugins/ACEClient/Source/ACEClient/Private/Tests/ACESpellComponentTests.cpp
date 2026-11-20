#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACETestDatDirectory.h"
#include "Engine/GameInstance.h"
#include "Components/TextBlock.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Mods/ACEPluginSubsystem.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUIGameplayBinder.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailSpellComponentsTest, "ACE.RetailParity.SpellComponents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACERetailSpellComponentsTest::RunTest(const FString&)
{
    auto* GI = NewObject<UGameInstance>();
    GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); };
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    if (!Dat->LoadDatDirectory(ACETestDatDirectory()) || !Dat->EnsureSpellTableLoaded()) return false;
    auto* Client = GI->GetSubsystem<UACEClientSubsystem>();
    Client->Session = MakeShared<FACESession>();
    auto& Session = *Client->Session;
    Session.State = EACESessionState::InWorld;
    Session.PlayerGuid = 100;
    Session.AccountName = TEXT("spell-component-regression");
    Session.ContainerContents.Add(Session.PlayerGuid);
    auto* Plugins=GI->GetSubsystem<UACEPluginSubsystem>();
    auto SnapshotComponents=[&]()
    {
        const auto Book=Plugins->Snapshot()->GetArrayField(TEXT("spells"));
        TestEqual(TEXT("Native automation snapshot contains the selected known spell"),Book.Num(),1);
        return Book.Num()?Book[0]->AsObject()->GetObjectField(TEXT("components")):MakeShared<FJsonObject>();
    };

    auto* Resources = NewObject<UACEUIResourceResolver>(); Resources->Initialize(Dat);
    auto* Manager = NewObject<UACEUIElementManager>(); Manager->Initialize();
    auto* Layout = NewObject<UACEUILayoutResolver>(); Layout->Initialize(Dat, Manager);
    if (!Layout->LoadLayout(0x21000005)) return false;
    auto* Canvas = NewObject<UACEUICanvasWidget>(); Canvas->Initialize();
    Canvas->InitializeCanvas(Manager); Canvas->SetResourceResolver(Resources);
    auto* Binder = NewObject<UACEUIGameplayBinder>();
    Binder->Initialize(Client, Manager, Canvas, nullptr);
    ON_SCOPE_EXIT { Binder->Shutdown(); };

    // Exercise the real containment receipt: a foci belongs to the player's
    // separate container list, never the ordinary item grid.
    auto MoveFocus = [&](int32 Container)
    {
        FACEBinaryWriter W; W.WriteUInt32(200); W.WriteUInt32(Container); W.WriteInt32(0); W.WriteInt32(2);
        FACEBinaryReader R(W.GetData()); Session.HandleInventoryPutObjInContainer(R);
    };
    auto Inspect = [&](uint32 Spell)
    {
        Binder->ExaminedSpellId = Spell; Binder->bExaminationDismissed = false;
        Binder->ShowExamination(true); Binder->RefreshExaminationOverlay();
        return Binder->ExamBody ? Binder->ExamBody->GetText().ToString() : FString();
    };
    auto RefreshOpenInspection = [&]()
    {
        // This is the existing per-frame path: no close/reopen or new selection.
        Binder->RefreshExaminationOverlay();
        return Binder->ExamBody ? Binder->ExamBody->GetText().ToString() : FString();
    };
    // ClientMagicSystem::GetAppropriateSpellFormula, SpellComponentTable::SchoolOfMagic2WCID.
    const uint32 FocusWCIDs[] = {0, 15271, 15270, 15269, 15268, 43173};
    const uint32 InfusedQualities[] = {0, 297, 296, 295, 294, 328};
    uint32 SelfSpell = 0;
    for (uint32 School = 1; School <= 5; ++School)
    {
        uint32 Spell = 0;
        for (const auto& Pair : Dat->SpellInfoCache)
            if (Pair.Value.School == School && Pair.Value.IconPowerLevel == 6
                && (School != 2 || (Pair.Value.Bitfield & 8u)))
            { Spell = Pair.Key; break; }
        if (!TestTrue(FString::Printf(TEXT("Retail level-six spell exists for school %u"), School), Spell != 0)) continue;
        if (School == 2) SelfSpell = Spell;
        Session.KnownSpells={int32(Spell)};
        const FString Original = Inspect(Spell);
        TestTrue(TEXT("Ordinary formula has ingredients"), Original.Contains(TEXT("Ingredients:")));
        TestFalse(TEXT("Ordinary formula uses colored rather than prismatic tapers"), Original.Contains(TEXT("Prismatic Taper")));
        TMap<FString,int32> Ordinary,Focused;
        TestTrue(TEXT("Automation resolves the full ordinary formula"),Dat->TryGetPluginSpellComponents(Spell,false,Ordinary));
        TestTrue(TEXT("Ordinary automation checks more than its scarab"),Ordinary.Num()>1);
        for(const auto& Part:Ordinary)TestTrue(TEXT("Automation and inspection agree on ordinary ingredients"),Original.Contains(Part.Key));
        TestFalse(TEXT("No focus before ownership"),(Dat->GetPlayerSpellFocusMask()&(1u<<School))!=0);
        TestFalse(TEXT("Cached automation initially uses ordinary components"),SnapshotComponents()->HasField(TEXT("Prismatic Taper")));

        FACEWorldObject Focus; Focus.Guid = 200; Focus.WeenieClassId = FocusWCIDs[School];
        Focus.ObjectDescriptionFlags = ACEObjectDescFlag::RequiresPackSlot;
        Session.WorldObjects.Add(Focus.Guid, Focus);
        MoveFocus(Session.PlayerGuid);
        TestFalse(TEXT("The foci is excluded from the ordinary inventory grid"),
            Client->GetPackItems(Session.PlayerGuid).ContainsByPredicate([](const auto& Item) { return Item.Guid == 200; }));
        TestTrue(TEXT("The foci occupies a backpack slot"),
            Client->GetPlayerPacks().ContainsByPredicate([](const auto& Item) { return Item.Guid == 200; }));
        const FString FocusDetails = RefreshOpenInspection();
        TestTrue(TEXT("Open inspection immediately switches to four prismatic tapers"), FocusDetails.Contains(TEXT("Prismatic Taper (4)")));
        TestTrue(TEXT("Automation recognizes the same equipped focus"),(Dat->GetPlayerSpellFocusMask()&(1u<<School))!=0);
        TestTrue(TEXT("Automation resolves the full focused formula"),Dat->TryGetPluginSpellComponents(Spell,true,Focused));
        TestEqual(TEXT("Level-six casting needs all four prismatic tapers"),Focused.FindRef(TEXT("Prismatic Taper")),4);
        TestEqual(TEXT("Focus receipt invalidates cached spell component requirements"),SnapshotComponents()->GetNumberField(TEXT("Prismatic Taper")),4.);
        for(const auto& Part:Focused)TestTrue(TEXT("Automation and inspection agree on focused ingredients"),FocusDetails.Contains(Part.Key));
        TestNotEqual(TEXT("Foci changes the actual displayed requirements"), FocusDetails, Original);

        Session.WorldObjects[200].WeenieClassId = FocusWCIDs[School % 5 + 1];
        TestEqual(TEXT("A different school's foci cannot replace this formula"), RefreshOpenInspection(), Original);
        Session.WorldObjects[200].WeenieClassId = FocusWCIDs[School];
        MoveFocus(900); // Chest, corpse, or another pack: not the player's side slots.
        TestEqual(TEXT("Moving the foci out restores the ordinary formula without reopening"), RefreshOpenInspection(), Original);
        auto& Bag = Session.WorldObjects.FindOrAdd(900); Bag.Guid = 900; Bag.ContainerId = Session.PlayerGuid;
        TestEqual(TEXT("A foci nested in an owned bag is not an equipped magic pack"), RefreshOpenInspection(), Original);
        TestFalse(TEXT("Nested foci cannot bypass ordinary component checks"),(Dat->GetPlayerSpellFocusMask()&(1u<<School))!=0);
        TestFalse(TEXT("Removing focus invalidates cached focused requirements"),SnapshotComponents()->HasField(TEXT("Prismatic Taper")));
        Session.WorldObjects.Remove(900);
        Session.PlayerVitals.StatQualityInts.Add(InfusedQualities[School], 1);
        TestEqual(TEXT("Infused school augmentation supplies the same formula without an item"), RefreshOpenInspection(), FocusDetails);
        TestTrue(TEXT("Automation uses augmented focus without an item"),(Dat->GetPlayerSpellFocusMask()&(1u<<School))!=0);
        TestEqual(TEXT("Augmentation invalidates cached ordinary requirements"),SnapshotComponents()->GetNumberField(TEXT("Prismatic Taper")),4.);
        Session.PlayerVitals.StatQualityInts[InfusedQualities[School]] = 0;
        TestEqual(TEXT("Removing the augmentation immediately restores ordinary requirements"), RefreshOpenInspection(), Original);
        MoveFocus(Session.PlayerGuid);
        FACEBinaryWriter Removal; Removal.WriteUInt32(200);
        FACEBinaryReader RemovalReader(Removal.GetData()); Session.HandleInventoryRemoveObject(RemovalReader);
        TestEqual(TEXT("Removing the foci cannot leave stale requirements in the open panel"), RefreshOpenInspection(), Original);
    }

    // Login ordering must not require a second login to recognize the foci:
    // object before description, object after description, or description before
    // the server assigns the player ID (the footer temporarily belongs to 0).
    for (int32 Order = 0; Order < 3; ++Order)
    {
        Session.ClearWorldState(); Session.PlayerGuid = Order == 2 ? 0 : 100;
        Session.State = EACESessionState::EnteringWorld; Session.bLoginCompleteSent = true;
        FACEWorldObject Focus; Focus.Guid = 200; Focus.WeenieClassId = 15270; Focus.ContainerId = 100;
        Focus.ObjectDescriptionFlags = ACEObjectDescFlag::RequiresPackSlot;
        if (Order == 0) Session.UpsertWorldObject(Focus);
        FACEBinaryWriter Login;
        Login.WriteUInt32(0); Login.WriteUInt32(0); // No qualities, generic weenie type.
        Login.WriteUInt32(0); Login.WriteUInt32(0); // Attribute/skill vector flags.
        Login.WriteUInt32(0x20); // PlayerModule: chat filters present.
        for (int32 I = 0; I < 3; ++I) Login.WriteUInt32(0); // Options, first bar, filters.
        Login.WriteUInt32(1); Login.WriteUInt32(200); Login.WriteUInt32(2); // Foci footer entry.
        Login.WriteUInt32(0); // No wielded equipment.
        FACEBinaryReader Description(Login.GetData()); Session.HandlePlayerDescription(Description);
        if (Order == 2)
        {
            FACEBinaryWriter Create; Create.WriteUInt32(100);
            FACEBinaryReader Player(Create.GetData()); Session.HandlePlayerCreate(Player);
        }
        if (Order != 0) Session.UpsertWorldObject(Focus);
        Session.State = EACESessionState::InWorld;
        TestTrue(TEXT("Every login ordering recognizes the foci without a relog"),
            Inspect(SelfSpell).Contains(TEXT("Prismatic Taper (4)")));
    }

    // The cast wire format has no component/foci flag. With only scarabs and
    // prismatic tapers, manual casting must still reach ACE's component check.
    auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto* Receiver = Sockets->CreateSocket(NAME_DGram, TEXT("Spell component fixture"), false);
    if (!TestNotNull(TEXT("Loopback receiver"), Receiver)) return false;
    ON_SCOPE_EXIT { Sockets->DestroySocket(Receiver); };
    auto Address = Sockets->CreateInternetAddr(); bool Valid = false;
    Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(0);
    if (!TestTrue(TEXT("Bind isolated casting endpoint"), Receiver->Bind(*Address))) return false;
    Receiver->GetAddress(*Address);
    Session.SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("Spell component sender"), false);
    Session.ServerC2SAddr = Address; Session.IssacClient = MakeUnique<FACEIsaac>(123u);
    for (const auto& Component : Dat->GetSpellComponents())
        if (Component.ComponentId == 6 || Component.ComponentId == 188)
        {
            FACEWorldObject Item; Item.Guid = 300 + Component.ComponentId;
            Item.WeenieClassId = Component.Wcid; Item.ContainerId = Session.PlayerGuid; Item.StackSize = 100;
            Session.WorldObjects.Add(Item.Guid, Item);
        }
    FACEWorldObject Focus; Focus.Guid = 200; Focus.WeenieClassId = 15270;
    Focus.ObjectDescriptionFlags = ACEObjectDescFlag::RequiresPackSlot;
    Session.WorldObjects.Add(Focus.Guid, Focus); MoveFocus(Session.PlayerGuid);
    for (bool Infused : {false, true})
    {
        if (Infused) { MoveFocus(900); Session.PlayerVitals.StatQualityInts.Add(296, 1); }
        Session.CachedC2SPackets.Reset();
        TestTrue(TEXT("Manual cast is not rejected for missing old components"), Client->SendCastSpell(SelfSpell));
        uint32 Last = 0; for (const auto& Packet : Session.CachedC2SPackets) Last = FMath::Max(Last, Packet.Key);
        if (!TestTrue(TEXT("Casting actually queues a server packet"), Last != 0)) continue;
        FACEBinaryReader Wire(Session.CachedC2SPackets[Last].Payload); Wire.Skip(16);
        TestEqual(TEXT("Retail game-action envelope"), Wire.ReadUInt32(), ACEOpcode::GameAction); Wire.ReadUInt32();
        TestEqual(TEXT("Retail targeted cast opcode"), Wire.ReadUInt32(), ACEGameAction::CastTargetedSpell);
        TestEqual(TEXT("Self spell target"), Wire.ReadUInt32(), uint32(Session.PlayerGuid));
        TestEqual(TEXT("Requested spell ID"), Wire.ReadUInt32(), SelfSpell);
        TestFalse(TEXT("No client-selected formula or component payload"), Wire.CanRead(1));
    }
    return !HasAnyErrors();
}
#endif
