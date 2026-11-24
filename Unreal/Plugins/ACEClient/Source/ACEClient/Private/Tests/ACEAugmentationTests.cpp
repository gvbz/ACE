#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Mods/ACEPluginEquipment.h"
#include "Engine/GameInstance.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEAugmentationTest, "ACE.RetailParity.Augmentations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACEAugmentationTest::RunTest(const FString&)
{
    auto* GI = NewObject<UGameInstance>(); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); };
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(), TEXT("RetailDatDir="), DatDirectory);
    if (!TestTrue(TEXT("Retail stat formulas load"), Dat->LoadDatDirectory(DatDirectory))) return false;
    auto* Client = GI->GetSubsystem<UACEClientSubsystem>();
    auto& Session = *Client->GetSession();
    Session.StatResolver = [Dat](auto& V, const auto& E) { Dat->RecomputePlayerStats(V, E); };
    Session.PlayerGuid = 123; Session.State = EACESessionState::InWorld;
    auto& V = Session.PlayerVitals; V.bValid = true;
    auto Int = [&](int32 Key, int32 Value)
    {
        FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::PrivateUpdatePropertyInt);
        W.WriteUInt8(1); W.WriteUInt32(Key); W.WriteInt32(Value); Session.HandleGameMessage(W.GetData());
    };
    auto Attribute = [&](int32 Id, int32 Start)
    {
        FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::PrivateUpdateAttribute);
        W.WriteUInt8(1); W.WriteUInt32(Id); W.WriteUInt32(100); W.WriteUInt32(Start); W.WriteUInt32(100000);
        Session.HandleGameMessage(W.GetData());
    };
    auto WriteSkill = [](FACEBinaryWriter& W, int32 Id, int32 Class, int32 Init, int32 Ranks)
    {
        W.WriteUInt32(Id); W.WriteUInt16(Ranks); W.WriteUInt16(0); W.WriteUInt32(Class);
        W.WriteUInt32(100000); W.WriteUInt32(Init); W.WriteUInt32(0); W.WriteDouble(0);
    };
    auto Skill = [&](int32 Id, int32 Class, int32 Init, int32 Ranks)
    {
        FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::PrivateUpdateSkill); W.WriteUInt8(1);
        WriteSkill(W, Id, Class, Init, Ranks); Session.HandleGameMessage(W.GetData());
    };
    for (int32 Id = 1; Id <= 6; ++Id) Attribute(Id, 50);
    {
        // Ivory Fire Throwing Dart: 290 base Missile Weapons, with a +45 skill enchantment active.
        Skill(47, 2, 0, 0);
        Skill(47, 2, 0, 288 - V.Skills[0].Base);
        FACEActiveEnchantment Buff; Buff.StatModType = 0x8010; Buff.StatModKey = 47; Buff.StatModValue = 45;
        Session.ActiveEnchantments = { Buff }; Session.NotifyVitalsChanged();
        FACEAppraisalInfo Dart; Dart.bSuccess = true; Dart.IntProperties = {{158, 2}, {159, 47}, {160, 290}};
        TestTrue(TEXT("Without Jack the dart reports base 288 despite the buff"),
            ACEPluginEquipment::WieldFailure(Dart, V).Contains(TEXT("requires 290 (have 288)")));
        Int(326, 1);
        TestEqual(TEXT("Buffed Missile Weapons with Jack"), V.Skills[0].Current, 338);
        TestEqual(TEXT("The skill window shows only the temporary +45"), V.Skills[0].GetDisplayModifier(), 45);
        TestTrue(TEXT("Jack qualifies the dart"), ACEPluginEquipment::WieldFailure(Dart, V).IsEmpty());
        Dart.IntProperties[160] = 293;
        TestTrue(TEXT("An exact permanent base qualifies"), ACEPluginEquipment::WieldFailure(Dart, V).IsEmpty());
        Dart.IntProperties[160] = 294;
        TestTrue(TEXT("One point short reports the same base as the skill window"),
            ACEPluginEquipment::WieldFailure(Dart, V).Contains(TEXT("requires 294 (have 293)")));
        Session.ActiveEnchantments.Reset(); Int(326, 0);
    }
    // Every current skill plus an untrained one: proficiency must not leak to
    // defenses, tinkering, summoning, or the other combat families.
    for (int32 Id = 6; Id <= 54; ++Id) Skill(Id, Id == 21 ? 1 : Id % 2 ? 3 : 2, 10, 100);
    TMap<int32, int32> BaseBefore;
    for (const auto& S : V.Skills) BaseBefore.Add(S.SkillId, S.Base);
    Int(300, 1); Int(301, 1); Int(302, 1); Int(365, 3); Int(326, 1); Int(344, 2);
    FACEActiveEnchantment Vitae; Vitae.bVitae = true; Vitae.StatModValue = .6f;
    Session.ActiveEnchantments = { Vitae }; Session.NotifyVitalsChanged();
    // CACQualities::InqSkill applies LumAugAllSkills and combat proficiency
    // before EnchantSkill, then Jack of All Trades and specialized luminance.
    for (const auto& S : V.Skills)
    {
        const bool Proficiency = TArray<int32>{31,32,33,34,41,43,44,45,46,47,49}.Contains(S.SkillId);
        const int32 Base = BaseBefore[S.SkillId] + 3 + (Proficiency ? 10 : 0);
        TestEqual(FString::Printf(TEXT("Skill %d base augmentation"), S.SkillId), S.Base, Base);
        TestEqual(FString::Printf(TEXT("Skill %d vitae ordering"), S.SkillId), S.Current,
            FMath::RoundToInt(Base * .6f) + 5 + (S.AdvancementClass == 3 ? 4 : 0));
    }
    Int(390, 5); // ACE Enlightenment also grants +1 per level to trained skills.
    for (const auto& S : V.Skills)
    {
        const bool Proficiency = TArray<int32>{31,32,33,34,41,43,44,45,46,47,49}.Contains(S.SkillId);
        TestEqual(TEXT("Enlightenment respects training and scales with vitae"), S.Current,
            FMath::RoundToInt((BaseBefore[S.SkillId] + 3 + (Proficiency ? 10 : 0)
                + (S.AdvancementClass >= 2 ? 5 : 0)) * .6f) + 5 + (S.AdvancementClass == 3 ? 4 : 0));
    }
    Session.ActiveEnchantments.Reset(); Session.NotifyVitalsChanged();
    const int32 HealthBefore = V.MaxHealth, StaminaBefore = V.MaxStamina, ManaBefore = V.MaxMana;
    const int32 RunBefore = V.RunSkillCurrent, JumpBefore = V.JumpSkillCurrent;
    // DoAugmentation sends the new StartingValue, already including the +5.
    // The augmentation counter must not add a second +5 on top of that packet.
    const int32 AttributeProperties[] = {218,219,221,220,222,223};
    for (int32 Id = 1; Id <= 6; ++Id)
    {
        Attribute(Id, 55); Int(AttributeProperties[Id-1], 1);
        TestEqual(TEXT("Innate gem applies once, to the correct attribute"), V.GetAttributeBase(Id), 155);
    }
    TestEqual(TEXT("Endurance augmentation updates health"), V.MaxHealth, HealthBefore + 3);
    TestEqual(TEXT("Endurance augmentation updates stamina"), V.MaxStamina, StaminaBefore + 5);
    TestEqual(TEXT("Self augmentation updates mana"), V.MaxMana, ManaBefore + 5);
    TestEqual(TEXT("Quickness augmentation updates run"), V.RunSkillCurrent, RunBefore + 5);
    TestEqual(TEXT("Strength/coordination augmentation updates jump"), V.JumpSkillCurrent, JumpBefore + 5);
    Int(24, 12);
    for (const auto& Pair : {TPair<int32,int32>(224,40),{225,18},{226,29},{227,30},{228,28}})
    {
        Skill(Pair.Value, 2, 5, 100);
        Skill(Pair.Value, 3, 10, 120); Int(Pair.Key, 1);
        const auto* S = V.Skills.FindByPredicate([&](const auto& Row) { return Row.SkillId == Pair.Value; });
        TestEqual(TEXT("Specialization gem uses server advancement class"), S->AdvancementClass, 3);
        TestEqual(TEXT("Specialization gem preserves recalculated ranks"), S->Ranks, 120);
        TestEqual(TEXT("Specialization gem updates the innate skill bonus"), S->InitLevel, 10);
    }
    TestEqual(TEXT("Specialization gems do not spend skill credits"), V.AvailableSkillCredits, 12);
    Int(5, 30000); Int(230, 0);
    const float BurdenBefore = Client->GetMovementBurden();
    Int(230, 5);
    TestTrue(TEXT("Five carry gems double capacity immediately"),
        FMath::IsNearlyEqual(Client->GetMovementBurden(), BurdenBefore * .5f));

    FACEWorldObject Self; Self.Guid = Session.PlayerGuid; Self.bIsSelf = true;
    Self.ItemsCapacity = 102; Self.ContainersCapacity = 7; Session.UpsertWorldObject(Self);
    for (int32 I = 0; I < 8; ++I)
    {
        FACEWorldObject Pack; Pack.Guid = 200 + I; Pack.ItemsCapacity = 24; Pack.ItemType = ACEItemType::Container;
        Pack.ContainerId = I < 7 ? Self.Guid : 0; Pack.PlacementPosition = I;
        Session.UpsertWorldObject(Pack);
    }
    TestEqual(TEXT("Seven filled pack slots cannot accept another pack"), Client->FindPickupContainer(207, Self.Guid), 0);
    const uint64 BeforeCapacity = Session.InventoryDataRevision;
    Int(7, 8); Int(229, 1);
    TestEqual(TEXT("Extra pack gem opens the eighth slot without relogging"),
        Session.WorldObjects[Self.Guid].ContainersCapacity, 8);
    TestEqual(TEXT("New slot is available to actual pickup routing"), Client->FindPickupContainer(207, Self.Guid), Self.Guid);
    TestTrue(TEXT("Capacity update invalidates inventory consumers"), Session.InventoryDataRevision > BeforeCapacity);
    Session.UpsertWorldObject(Self); // Older CreateObject from the other ordered queue.
    TestEqual(TEXT("Late object description cannot remove the augmented slot"), Session.WorldObjects[Self.Guid].ContainersCapacity, 8);
    const uint64 AfterCapacity = Session.InventoryDataRevision;
    Int(229, 1); Session.NotifyVitalsChanged();
    TestEqual(TEXT("No repeated capacity bonus or invalidation"), Session.InventoryDataRevision, AfterCapacity);

    // These bonuses are calculated by the server (damage, resistance, rewards,
    // death protection, regen, duration and infused foci). Retain every quality
    // for character info/plugins without fabricating client stat increments.
    for (int32 Property : {231,232,233,234,235,236,237,238,240,241,242,243,244,245,246,294,295,296,297,298,299,309,310,328})
    {
        Int(Property, 1);
        TestEqual(TEXT("Server-owned augmentation quality is available immediately"), V.StatQualityInts.FindRef(Property), 1);
    }
    const FACEPlayerVitals Saved = V;
    FACEBinaryWriter Login; Login.WriteUInt32(1); Login.WriteUInt32(1);
    Login.WriteUInt16(Saved.StatQualityInts.Num()); Login.WriteUInt16(64);
    for (const auto& P : Saved.StatQualityInts) { Login.WriteUInt32(P.Key); Login.WriteInt32(P.Value); }
    Login.WriteUInt32(3); Login.WriteUInt32(0); Login.WriteUInt32(0x3F);
    for (int32 Id = 1; Id <= 6; ++Id) { Login.WriteUInt32(100); Login.WriteUInt32(55); Login.WriteUInt32(100000); }
    Login.WriteUInt16(Saved.Skills.Num()); Login.WriteUInt16(64);
    for (const auto& S : Saved.Skills) WriteSkill(Login, S.SkillId, S.AdvancementClass, S.InitLevel, S.Ranks);
    for (int32 Order = 0; Order < 3; ++Order)
    {
        FACESession Relog; Relog.PlayerGuid = Order == 2 ? 0 : Self.Guid; Relog.StatResolver = Session.StatResolver;
        if (Order != 1) Relog.UpsertWorldObject(Self);
        FACEBinaryReader R(Login.GetData()); Relog.HandlePlayerDescription(R);
        if (Order == 1) Relog.UpsertWorldObject(Self);
        if (Order == 2)
        {
            Relog.bLoginCompleteSent = true;
            FACEBinaryWriter Player; Player.WriteUInt32(Self.Guid);
            FACEBinaryReader PR(Player.GetData()); Relog.HandlePlayerCreate(PR);
        }
        TestEqual(TEXT("Relog restores capacity regardless of player/description/object order"), Relog.WorldObjects[Self.Guid].ContainersCapacity, 8);
        TestEqual(TEXT("Relog restores carry gems"), Relog.PlayerVitals.CarryingCapacityAugs, 5);
        for (int32 I = 0; I < Saved.Skills.Num(); ++I)
            TestEqual(TEXT("Augmented skill survives login unchanged"), Relog.PlayerVitals.Skills[I].Current, Saved.Skills[I].Current);
    }

    // Exercise the real Use/ConfirmationResponse wire protocol, without an
    // account or live server. Acceptance alone must never grant a bonus/spend XP.
    auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto* Receiver = Sockets->CreateSocket(NAME_DGram, TEXT("Augmentation receiver"), false);
    auto Address = Sockets->CreateInternetAddr(); bool Valid = false;
    Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(0);
    ON_SCOPE_EXIT { if (Receiver) { Receiver->Close(); Sockets->DestroySocket(Receiver); }
        if (Session.SocketC2S) { Session.SocketC2S->Close(); Sockets->DestroySocket(Session.SocketC2S); Session.SocketC2S = nullptr; } };
    if (!TestTrue(TEXT("Isolated confirmation transport binds"), Receiver && Receiver->Bind(*Address))) return false;
    Receiver->GetAddress(*Address);
    Session.SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("Augmentation sender"), false);
    Session.ServerC2SAddr = Address; Session.IssacClient = MakeUnique<FACEIsaac>(123u);
    FACEWorldObject Gem; Gem.Guid = 500; Gem.ContainerId = Self.Guid; Gem.ItemType = ACEItemType::Gem;
    Gem.Name = TEXT("Might of the Seventh Mule"); Session.UpsertWorldObject(Gem);
    V.AvailableExperience = 4000000000LL;
    for (bool Accept : {false, true})
    {
        Session.CachedC2SPackets.Reset(); Session.SendUseItem(Gem.Guid);
        FACEBinaryWriter Request; Request.WriteUInt32(Self.Guid); Request.WriteUInt32(1); Request.WriteUInt32(0x274);
        Request.WriteUInt32(6); Request.WriteUInt32(77);
        Request.WriteString16L(TEXT("This action will augment your character and will cost 1,000,000,000 available experience."));
        FACEBinaryReader R(Request.GetData()); Session.HandleGameEvent(R);
        TestEqual(TEXT("Augmentation requires server confirmation type six"), Session.GetConfirmations().Num(), 1);
        TestTrue(TEXT("Augmentation accept/decline is transmitted"), Session.RespondToConfirmation(6, 77, Accept));
        bool FoundUse = false, FoundReply = false;
        for (const auto& P : Session.CachedC2SPackets)
        {
            FACEBinaryReader Wire(P.Value.Payload); Wire.Skip(16);
            if (!Wire.CanRead(12) || Wire.ReadUInt32() != ACEOpcode::GameAction) continue;
            Wire.ReadUInt32(); const uint32 Action = Wire.ReadUInt32();
            if (Action == ACEGameAction::Use) FoundUse = Wire.ReadUInt32() == uint32(Gem.Guid);
            if (Action == 0x275) FoundReply = Wire.ReadUInt32() == 6 && Wire.ReadUInt32() == 77 && Wire.ReadUInt32() == uint32(Accept);
        }
        TestTrue(TEXT("Gem uses retail Use action"), FoundUse);
        TestTrue(TEXT("Confirmation preserves server type, context and answer"), FoundReply);
        TestEqual(TEXT("Only the server can deduct augmentation XP"), V.AvailableExperience, 4000000000LL);
        TestTrue(TEXT("Confirmation does not locally destroy the gem"), Session.WorldObjects.Contains(Gem.Guid));
        TestFalse(TEXT("A resolved confirmation cannot be sent twice"), Session.RespondToConfirmation(6, 77, Accept));
    }
    FACEBinaryWriter XP; XP.WriteUInt32(ACEOpcode::PrivateUpdatePropertyInt64); XP.WriteUInt8(1); XP.WriteUInt32(2); XP.WriteUInt64(3000000000ULL);
    Session.HandleGameMessage(XP.GetData());
    TestEqual(TEXT("Server XP purchase update keeps its full 64-bit value"), V.AvailableExperience, 3000000000LL);
    FACEBinaryWriter Consume; Consume.WriteUInt32(ACEOpcode::InventoryRemoveObject); Consume.WriteUInt32(Gem.Guid);
    Session.HandleGameMessage(Consume.GetData());
    TestFalse(TEXT("Server consumption removes the augmentation gem"), Session.WorldObjects.Contains(Gem.Guid));
    FString Chat; int32 ChatType = 0;
    const auto ChatHandle = Session.OnChatMessage.AddLambda([&](const FString& Text, const FString&, int32 Type)
        { Chat = Text; ChatType = Type; });
    ON_SCOPE_EXIT { Session.OnChatMessage.Remove(ChatHandle); };
    FACEBinaryWriter Success; Success.WriteUInt32(0x055B); Success.WriteString16L(Gem.Name);
    FACEBinaryReader SR(Success.GetData()); Session.HandleWeenieErrorWithString(SR);
    TestEqual(TEXT("Retail augmentation success message names the granted gem"), Chat,
        TEXT("Congratulations! You have succeeded in acquiring the Might of the Seventh Mule augmentation."));
    TestEqual(TEXT("Augmentation success keeps the retail System channel"), ChatType, ACEChatMessageType::System);
    FACEBinaryWriter Failure; Failure.WriteUInt32(0x0559);
    FACEBinaryReader FR(Failure.GetData()); Session.HandleWeenieError(FR);
    TestTrue(TEXT("Insufficient experience explains why the gem was not applied"), Chat.Contains(TEXT("not have enough unspent experience")));
    return true;
}
#endif
