#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACESession.h"

namespace
{
TArray<uint8> DataIdUpdate(uint8 Stamp, uint32 Guid, uint32 Key, uint32 Value, bool bPublic = true)
{
	FACEBinaryWriter W;
	W.WriteUInt32(bPublic ? 0x2D8 : 0x2D7); W.WriteUInt8(Stamp);
	if (bPublic) W.WriteUInt32(Guid);
	W.WriteUInt32(Key); W.WriteUInt32(Value);
	return W.GetData();
}
// Layout 0 = private; 1 = retail public; 2 = ACE public.
TArray<uint8> StringUpdate(uint8 Stamp, uint32 Guid, uint32 Key, const TCHAR* Value, int32 Layout)
{
	FACEBinaryWriter W;
	W.WriteUInt32(Layout ? 0x2D6 : 0x2D5); W.WriteUInt8(Stamp);
	if (Layout == 1) W.WriteUInt32(Guid);
	W.WriteUInt32(Key);
	if (Layout == 2) W.WriteUInt32(Guid);
	W.Align(); W.WriteString16L(Value);
	return W.GetData();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELivePropertyUpdatesTest, "ACE.Network.LivePropertyUpdates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FACELivePropertyUpdatesTest::RunTest(const FString&)
{
	FACESession Session;
	Session.PlayerGuid = 0x50001234;
	FACEWorldObject Item;
	Item.Guid = int32(0xF06A016A); Item.ContainerId = Session.PlayerGuid;
	Item.Position.CellId = 0x12340101; Item.Position.Location.X = 15;
	Item.DefaultScriptId = 0x33000001;
	Session.WorldObjects.Add(Item.Guid, Item);
	int32 ActorRefreshes = 0;
	Session.OnObjectCreated.AddLambda([&](const FACEWorldObject&) { ++ActorRefreshes; });
	// All presentation DataIDs must accept zero. Stamp filtering still prevents
	// a late nonzero value from resurrecting a removed icon layer/innate spell.
	for (uint32 Key : {8u, 28u, 44u, 50u, 52u})
	{
		auto ReadValue = [&]() -> uint32
		{
			const auto& O = Session.WorldObjects[Item.Guid];
			switch (Key)
			{
			case 8: return O.IconId;
			case 28: return O.SpellDID;
			case 44: return O.PublicPhysicsScript;
			case 50: return O.IconOverlayId;
			default: return O.IconUnderlayId;
			}
		};
		const uint32 Value = Key == 44 ? 123 : Key == 28 ? 100 : 0x06001234;
		uint64 Before = Session.GetInventoryDataRevision();
		Session.ReceiveGameMessage(DataIdUpdate(254, Item.Guid, Key, Value));
		TestEqual(TEXT("Nonzero DataID reaches the live object"), ReadValue(), Value);
		TestEqual(TEXT("Changed DataID refreshes cached inventory"), Session.GetInventoryDataRevision(), Before + 1);
		Before = Session.GetInventoryDataRevision();
		Session.ReceiveGameMessage(DataIdUpdate(254, Item.Guid, Key, Value));
		TestEqual(TEXT("Identical retransmission does not churn inventory caches"), Session.GetInventoryDataRevision(), Before);
		Session.ReceiveGameMessage(DataIdUpdate(255, Item.Guid, Key, 0));
		TestEqual(TEXT("Explicit zero clears the live DataID"), ReadValue(), 0u);
		Session.ReceiveGameMessage(DataIdUpdate(254, Item.Guid, Key, Value));
		TestEqual(TEXT("Late nonzero cannot resurrect cleared metadata"), ReadValue(), 0u);
		Session.ReceiveGameMessage(DataIdUpdate(0, Item.Guid, Key, Value));
		TestEqual(TEXT("DataID stamps wrap at 256"), ReadValue(), Value);
		auto Truncated = DataIdUpdate(2, Item.Guid, Key, 99);
		Truncated.Pop(); Session.ReceiveGameMessage(Truncated);
		Session.ReceiveGameMessage(DataIdUpdate(1, Item.Guid, Key, 0));
		TestEqual(TEXT("Truncated DataID does not consume its stamp"), ReadValue(), 0u);
	}
	TestEqual(TEXT("Metadata update does not recreate actors"), ActorRefreshes, 0);
	TestEqual(TEXT("Public script does not replace the physics default script"), Session.WorldObjects[Item.Guid].DefaultScriptId, 0x33000001);
	TestEqual(TEXT("Metadata does not move an object"), Session.WorldObjects[Item.Guid].Position.Location.X, Item.Position.Location.X);
	Session.WorldObjects[Item.Guid].ContainerId = 0;
	Session.WorldObjects[Item.Guid].ParentGuid = Session.PlayerGuid;
	const uint64 HeldRevision = Session.GetInventoryDataRevision();
	Session.ReceiveGameMessage(DataIdUpdate(3, Item.Guid, 28, 77));
	TestEqual(TEXT("Attached caster before Wielder update still refreshes inventory"), Session.GetInventoryDataRevision(), HeldRevision + 1);
	const uint64 UnknownRevision = Session.GetInventoryDataRevision();
	Session.ReceiveGameMessage(DataIdUpdate(1, 0x12345678, 8, 0x06001234));
	Session.ReceiveGameMessage(DataIdUpdate(1, Item.Guid, 999, 0x06001234));
	TestFalse(TEXT("DataID updates do not manufacture unknown objects"), Session.WorldObjects.Contains(0x12345678));
	TestEqual(TEXT("Unsupported DataIDs do not churn inventory"), Session.GetInventoryDataRevision(), UnknownRevision);
	FACEWorldObject Player; Player.Guid = Session.PlayerGuid;
	Session.WorldObjects.Add(Player.Guid, Player);
	Session.ReceiveGameMessage(DataIdUpdate(10, Player.Guid, 8, 0x06001234, false));
	Session.ReceiveGameMessage(DataIdUpdate(11, Player.Guid, 8, 0));
	Session.ReceiveGameMessage(DataIdUpdate(10, Player.Guid, 8, 0x06001234, false));
	TestEqual(TEXT("Private/public DataIDs share the player timestamp"), Session.WorldObjects[Player.Guid].IconId, 0);
	Session.ReceiveGameMessage(DataIdUpdate(3, Item.Guid, 8, 0x06001234));
	Session.ReceiveGameMessage(DataIdUpdate(3, Item.Guid, 50, 0x06001235));
	FACEAppraisalInfo Appraisal;
	Session.OnAppraisal.AddLambda([&](const FACEAppraisalInfo& Info) { Appraisal = Info; });
	FACEBinaryWriter Identify;
	Identify.WriteUInt32(Item.Guid); Identify.WriteUInt32(0x1000); Identify.WriteUInt32(1);
	Identify.WriteUInt16(2); Identify.WriteUInt16(16);
	Identify.WriteUInt32(8); Identify.WriteUInt32(0);
	Identify.WriteUInt32(50); Identify.WriteUInt32(0);
	FACEBinaryReader IdentifyReader(Identify.GetData()); Session.HandleIdentifyObjectResponse(IdentifyReader);
	TestEqual(TEXT("Appraisal zero clears the known icon too"), Session.WorldObjects[Item.Guid].IconId, 0);
	TestEqual(TEXT("Appraisal zero clears the known overlay"), Session.WorldObjects[Item.Guid].IconOverlayId, 0);
	TestEqual(TEXT("Inspect result cannot reuse the icon that its appraisal cleared"), Appraisal.IconDid, 0);
	TestEqual(TEXT("Successful appraisal without mana records absence"), Session.WorldObjects[Item.Guid].ItemCurrentMana, -2);
	for (uint32 Key : {107u, 108u})
	{
		auto ManaUpdate = [&](uint8 Stamp, int32 Value)
		{
			FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::PublicUpdatePropertyInt); W.WriteUInt8(Stamp);
			W.WriteUInt32(Item.Guid); W.WriteUInt32(Key); W.WriteInt32(Value); return W.GetData();
		};
		auto Read = [&]() { return Key == 107 ? Session.WorldObjects[Item.Guid].ItemCurrentMana : Session.WorldObjects[Item.Guid].ItemMaximumMana; };
		Session.ReceiveGameMessage(ManaUpdate(254, 5000));
		TestEqual(TEXT("GDLE live item mana is retained"), Read(), 5000);
		Session.ReceiveGameMessage(ManaUpdate(255, 0));
		Session.ReceiveGameMessage(ManaUpdate(254, 5000));
		TestEqual(TEXT("Late recharge packet cannot resurrect depleted mana"), Read(), 0);
		Session.ReceiveGameMessage(ManaUpdate(0, 100));
		TestEqual(TEXT("Mana quality stamp wraps normally"), Read(), 100);
		auto Truncated = ManaUpdate(2, 5000); Truncated.Pop(); Session.ReceiveGameMessage(Truncated);
		Session.ReceiveGameMessage(ManaUpdate(1, 20));
		TestEqual(TEXT("Truncated mana message does not consume timestamp"), Read(), 20);
		FACEBinaryWriter Remove; Remove.WriteUInt32(0x1D2); Remove.WriteUInt8(2); Remove.WriteUInt32(Item.Guid); Remove.WriteUInt32(Key);
		Session.ReceiveGameMessage(Remove.GetData()); Session.ReceiveGameMessage(ManaUpdate(1, 5000));
		TestEqual(TEXT("Mana removal clears the reading and rejects older updates"), Read(), -2);
	}
	FACEBinaryWriter ManaID; ManaID.WriteUInt32(Item.Guid); ManaID.WriteUInt32(1); ManaID.WriteUInt32(1);
	ManaID.WriteUInt16(2); ManaID.WriteUInt16(16); ManaID.WriteUInt32(107); ManaID.WriteInt32(75); ManaID.WriteUInt32(108); ManaID.WriteInt32(100);
	FACEBinaryReader ManaReader(ManaID.GetData()); Session.HandleIdentifyObjectResponse(ManaReader);
	TestEqual(TEXT("Later successful appraisal replaces public current mana"), Session.WorldObjects[Item.Guid].ItemCurrentMana, 75);
	TestEqual(TEXT("Later successful appraisal replaces public maximum mana"), Session.WorldObjects[Item.Guid].ItemMaximumMana, 100);
	Session.UpsertWorldObject(Item);
	TestEqual(TEXT("Repeated description preserves mana for the same item instance"), Session.WorldObjects[Item.Guid].ItemCurrentMana, 75);
	Session.UpsertWorldObject(Item, true);
	TestEqual(TEXT("A replacement instance cannot inherit another item's mana"), Session.WorldObjects[Item.Guid].ItemCurrentMana, -1);

	// A string property can arrive on either protocol layout, including before
	// the player ObjectCreate. They must all use the same timestamp identity.
	Session.WorldObjects.Remove(Player.Guid);
	constexpr uint32 CustomKey = 0x50005678;
	for (int32 Layout : {0, 1, 2})
	{
		Session.ReceiveGameMessage(StringUpdate(10 + Layout, Player.Guid, CustomKey, Layout == 2 ? TEXT("ACE") : TEXT("Retail"), Layout));
		TestEqual(TEXT("Private/retail/ACE strings reach the intended custom key"),
			Session.PlayerVitals.QualityStrings.FindRef(CustomKey), FString(Layout == 2 ? TEXT("ACE") : TEXT("Retail")));
	}
	Session.ReceiveGameMessage(StringUpdate(11, Player.Guid, CustomKey, TEXT("Stale"), 0));
	TestEqual(TEXT("Public and private string formats share ordering"), Session.PlayerVitals.QualityStrings.FindRef(CustomKey), FString(TEXT("ACE")));
	Session.ReceiveGameMessage(StringUpdate(13, Player.Guid, CustomKey, TEXT(""), 1));
	TestTrue(TEXT("Empty string remains an explicit quality value"), Session.PlayerVitals.QualityStrings.Contains(CustomKey));
	TestTrue(TEXT("Empty string clears the old contents"), Session.PlayerVitals.QualityStrings[CustomKey].IsEmpty());
	for (int32 Layout : {0, 1, 2})
	{
		const auto Complete = StringUpdate(30 + Layout, Player.Guid, CustomKey, TEXT("X"), Layout);
		for (int32 Length = 0; Length < Complete.Num(); ++Length)
		{
			auto Truncated = Complete; Truncated.SetNum(Length);
			Session.ReceiveGameMessage(Truncated);
		}
	}
	Session.ReceiveGameMessage(StringUpdate(14, Player.Guid, CustomKey, TEXT("After malformed"), 2));
	TestEqual(TEXT("Truncated header/text/padding never poisons a string timestamp"),
		Session.PlayerVitals.QualityStrings.FindRef(CustomKey), FString(TEXT("After malformed")));
	Session.ReceiveGameMessage(StringUpdate(1, Player.Guid, 5, TEXT("New template"), 1));
	TestEqual(TEXT("Retail string refreshes character template"), Session.PlayerVitals.TemplateName, FString(TEXT("New template")));
	Session.ReceiveGameMessage(StringUpdate(254, Player.Guid, 9001, TEXT("Before wrap"), 2));
	Session.ReceiveGameMessage(StringUpdate(0, Player.Guid, 9001, TEXT("After wrap"), 1));
	Session.ReceiveGameMessage(StringUpdate(255, Player.Guid, 9001, TEXT("Late"), 0));
	TestEqual(TEXT("String ordering wraps across server layouts"), Session.PlayerVitals.QualityStrings.FindRef(9001), FString(TEXT("After wrap")));
	for (int32 Layout : {1, 2})
		Session.ReceiveGameMessage(StringUpdate(100, Item.Guid, CustomKey, TEXT("Other object"), Layout));
	TestEqual(TEXT("Other objects cannot replace player string qualities"),
		Session.PlayerVitals.QualityStrings.FindRef(CustomKey), FString(TEXT("After malformed")));
	return true;
}
#endif
