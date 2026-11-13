#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACESession.h"
#include "Protocol/ACEHash32.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Common/UdpSocketBuilder.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEGDLEInteractionTransportTest, "ACE.Network.GDLEInteractionTransport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEGDLEInteractionTransportTest::RunTest(const FString&)
{
    FACESession Clock;
    Clock.PacketTimeOrigin = 1000.0;
    TestEqual(TEXT("Packet intervals start at zero"), Clock.PacketIntervalAt(1000.0), uint16(0));
    TestEqual(TEXT("Packets within one half second share an interval"), Clock.PacketIntervalAt(1000.499), uint16(0));
    TestEqual(TEXT("Retail interval advances twice per second"), Clock.PacketIntervalAt(1000.5), uint16(1));
    TestEqual(TEXT("Long sessions reach the final 16-bit interval"), Clock.PacketIntervalAt(33767.5), uint16(65535));
    TestEqual(TEXT("Retail interval wraps without overflow"), Clock.PacketIntervalAt(33768.0), uint16(0));
    TestEqual(TEXT("An earlier clock sample cannot underflow"), Clock.PacketIntervalAt(999.0), uint16(0));
    Clock.Disconnect();
    TestEqual(TEXT("Disconnect resets the connection clock"), Clock.PacketIntervalAt(2000.0), uint16(0));

    auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FSocket* Receiver = FUdpSocketBuilder(TEXT("GDLEItemCapture")).AsNonBlocking().BoundToPort(0).Build();
    if (!TestNotNull(TEXT("Loopback receiver"), Receiver)) return false;
    auto Address = Sockets->CreateInternetAddr(); Receiver->GetAddress(*Address);
    bool Valid = false; Address->SetIp(TEXT("127.0.0.1"), Valid);
    auto Receive = [&]()
    {
        TArray<uint8> Bytes; Bytes.SetNumUninitialized(512); int32 Read = 0;
        if (!Receiver->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(1))
            || !Receiver->Recv(Bytes.GetData(), Bytes.Num(), Read)) Read = 0;
        Bytes.SetNum(Read); return Bytes;
    };
    constexpr uint32 Item = 0x80000001, Player = 0x50000001, Seed = 0x12345678;
    // Decode packets using the fields consumed by Seedsow/GDLE ProcessEvent,
    // and apply its IncomingBlob timing check to repeated interaction bursts.
    for (bool GDLE : {false, true})
    {
        FACESession Session; Session.Creds.bGDLE = GDLE;
        Session.State = EACESessionState::InWorld; Session.ClientId = 0x1234;
        Session.ServerC2SAddr = Address;
        Session.SocketC2S = FUdpSocketBuilder(TEXT("GDLEItemSender")).AsNonBlocking().BoundToPort(0).Build();
        Session.IssacClient = MakeUnique<FACEIsaac>(Seed);
        Session.PlayerGuid = Player;
        // Placement 2 is an insertion index: keep two destination items in the
        // fixture so normal inventory bounds checks do not clamp it to zero.
        for (int32 Slot = 0; Slot < 2; ++Slot)
        {
            FACEWorldObject Existing; Existing.Guid = int32(Item + 1 + Slot);
            Existing.ContainerId = Player; Existing.PlacementPosition = Slot;
            Session.WorldObjects.Add(Existing.Guid, Existing);
        }
        FACEIsaac Keys(Seed);
        // Unix's process-relative clock can be younger than the synthetic
        // session ages below. Exercise burst traffic against a real positive
        // origin; deterministic interval/wrap coverage is above.
        Session.PacketTimeOrigin = FPlatformTime::Seconds();
        uint16 BaseInterval = 0; double BaseTime = 0; bool SpeedHackRejected = false;
        uint32 ActionSequence = 1;
        for (int32 Step = 0; Step < 80; ++Step)
        {
            for (uint32 Action : {uint32(0x0036), uint32(0x0019), uint32(0x001A)})
            {
                const double SentAt = FPlatformTime::Seconds();
                const uint16 EarliestInterval = Session.PacketIntervalAt(SentAt);
                if (Action == 0x0036) Session.SendUseItem(Item);
                else if (Action == 0x0019) Session.SendPutItemInContainer(Item, Player, 2);
                else Session.SendGetAndWieldItem(Item, 0x100000);
                const auto Bytes = Receive();
                const double ReceivedAt = FPlatformTime::Seconds();
                const uint16 LatestInterval = Session.PacketIntervalAt(ReceivedAt);
                const double Elapsed = ReceivedAt - Session.PacketTimeOrigin;
                if (!TestTrue(TEXT("Item action reaches wire"), Bytes.Num() >= 52)) break;
                FACEBinaryReader R(Bytes);
                const uint32 Sequence = R.ReadUInt32(), Flags = R.ReadUInt32(), Checksum = R.ReadUInt32();
                TestEqual(TEXT("Item recipient matches authenticated session"), R.ReadUInt16(), uint16(0x1234));
                const uint16 Interval = R.ReadUInt16(), Size = R.ReadUInt16(), Iteration = R.ReadUInt16();
                TestTrue(TEXT("Item timestamp is elapsed half seconds, independent of packet count"),
                    Interval >= EarliestInterval && Interval <= LatestInterval);
                TestEqual(TEXT("Checksum covers the corrected timestamp"), Checksum,
                    FACESession::HeaderHash32(Sequence, EACEPacketHeaderFlags(Flags), 0x1234, Interval, Size, Iteration)
                    + (FACEHash32::Calculate(Bytes.GetData() + 20, Size) ^ Keys.Next()));
                R.Skip(12); // fragment id, count, size
                TestEqual(TEXT("Single fragment index"), R.ReadUInt16(), uint16(0));
                TestEqual(TEXT("Retail item actions use weenie queue"), R.ReadUInt16(), ACEQueue::WeenieQueue);
                TestEqual(TEXT("Item action is not a character logoff"), R.ReadUInt32(), uint32(0xF7B1));
                TestEqual(TEXT("Action sequence remains ordered"), R.ReadUInt32(), ActionSequence++);
                TestEqual(TEXT("GDLE receives expected action opcode"), R.ReadUInt32(), Action);
                TestEqual(TEXT("Item identity retains unsigned wire bits"), R.ReadUInt32(), Item);
                if (Action == 0x0019)
                {
                    TestEqual(TEXT("Move destination"), R.ReadUInt32(), Player);
                    TestEqual(TEXT("Move placement"), R.ReadUInt32(), 2u);
                }
                else if (Action == 0x001A) TestEqual(TEXT("Wield location"), R.ReadUInt32(), 0x100000u);
                TestEqual(TEXT("No extra action payload"), R.Remaining(), 0);
                if(Action==0x001A)
                {
                    FACEBinaryWriter Ack;Ack.WriteUInt32(Item);Ack.WriteUInt32(0x100000);
                    FACEBinaryReader AR(Ack.GetData());Session.HandleWieldItem(AR);
                }
                if (!BaseInterval) { BaseInterval = Interval; BaseTime = Elapsed; }
                else
                {
                    const int16 Difference = int16(Interval - BaseInterval);
                    if (Difference > 120 || Difference < 0) { BaseInterval = Interval; BaseTime = Elapsed; }
                    else if (Difference > 30)
                    {
                        const uint32 Expected = uint32((Elapsed - BaseTime) * 2.0);
                        if (!Expected || double(Difference) / Expected > 1.1) SpeedHackRejected = true;
                    }
                }
            }
        }
        TestFalse(TEXT("Normal item bursts cannot trip GDLE's speed-hack check"), SpeedHackRejected);
        // GDLE ObjectMsgs/NotifyUseDone replies retain the same event envelope as
        // retail and ACE; successful inventory notifications must keep us in-world.
        auto Reply = [&](uint32 Event, const TArray<uint32>& Values)
        {
            FACEBinaryWriter W; W.WriteUInt32(0xF7B0); W.WriteUInt32(Player);
            W.WriteUInt32(1); W.WriteUInt32(Event);
            for (uint32 Value : Values) W.WriteUInt32(Value);
            Session.HandleGameMessage(W.GetData());
            TestTrue(TEXT("GDLE inventory reply retains the world session"), Session.State == EACESessionState::InWorld);
        };
        Reply(0x01C7, {0});
        TestFalse(TEXT("Successful use reply releases the action lock"), Session.bUseBusy);
        Reply(0x0022, {Item, Player, 0, 0});
        TestEqual(TEXT("GDLE move reply updates item ownership"), uint32(Session.WorldObjects[Item].ContainerId), Player);
        Reply(0x0023, {Item, 0x100000});
        TestEqual(TEXT("GDLE wield reply equips the item"), Session.WorldObjects[Item].CurrentWieldedLocation, int64(0x100000));
        const double RetryAt = FPlatformTime::Seconds();
        Session.PacketTimeOrigin = RetryAt - FMath::Min(1.125, RetryAt * .5);
        const uint16 EarliestRetryInterval = Session.PacketIntervalAt(RetryAt);
        Session.HandleServerRequestRetransmit({2});
        const auto Retry = Receive();
        const uint16 LatestRetryInterval = Session.PacketIntervalAt(FPlatformTime::Seconds());
        if (TestTrue(TEXT("Lost item action can be retransmitted"), Retry.Num() >= 52))
        {
            FACEBinaryReader R(Retry); R.Skip(14);
            const uint16 RetryInterval = R.ReadUInt16();
            TestTrue(TEXT("Retransmission uses the same half-second clock"),
                RetryInterval >= EarliestRetryInterval && RetryInterval <= LatestRetryInterval);
        }
        // Casting must retain the normal retail movement transport on both
        // ACE and GDLE, including independent strafe/turn and magic stance.
        Session.CurrentStance=ACEMotion::StanceMagic;Session.bUseBusy=true;
        Session.PlayerPosition.CellId=0x01430171;Session.PlayerPosition.Location=FVector(49,-75,0);
        for(float Forward:{1.f,-1.f,0.f})
        {
            const uint32 BeforeSequence=Session.NextGameActionSequence;
            Session.SendMoveToState(Forward,1.f,-1.f,true,true,false);
            const auto Bytes=Receive();
            if(!TestTrue(TEXT("Movement during casting reaches the server"),Bytes.Num()>=100))continue;
            FACEBinaryReader R(Bytes);R.Skip(34);
            TestEqual(TEXT("Slide uses the retail weenie queue"),R.ReadUInt16(),ACEQueue::WeenieQueue);
            TestEqual(TEXT("Slide uses the game-action envelope"),R.ReadUInt32(),uint32(0xF7B1));
            TestEqual(TEXT("Command sequence advances once per movement update"),R.ReadUInt32(),BeforeSequence);
            TestEqual(TEXT("Slide remains standard MoveToState"),R.ReadUInt32(),ACEGameAction::MoveToState);
            const uint32 Flags=R.ReadUInt32();
            TestEqual(TEXT("Run hold is retained during casting"),R.ReadUInt32(),ACEMotion::HoldKeyRun);
            TestEqual(TEXT("Movement never changes the magic stance"),R.ReadUInt32(),ACEMotion::StanceMagic);
            if(Forward!=0)
            {
                TestTrue(TEXT("Active forward command is present"),(Flags&ACERawMotionFlags::ForwardCommand)!=0);
                TestEqual(TEXT("Raw forward/back is preserved for server interpretation"),R.ReadUInt32(),Forward>0?ACEMotion::WalkForward:ACEMotion::WalkBackwards);
                R.ReadUInt32();TestEqual(TEXT("Raw forward speed is not pre-scaled"),R.ReadFloat(),1.f);
            }
            else TestFalse(TEXT("Forward release does not erase strafe or turn"),(Flags&ACERawMotionFlags::ForwardCommand)!=0);
            TestEqual(TEXT("Independent strafe command"),R.ReadUInt32(),ACEMotion::SideStepRight);
            R.ReadUInt32();TestEqual(TEXT("Strafe speed"),R.ReadFloat(),1.f);
            TestEqual(TEXT("Independent turn command"),R.ReadUInt32(),ACEMotion::TurnRight);
            R.ReadUInt32();TestEqual(TEXT("Turn direction"),R.ReadFloat(),-1.f);
            TestEqual(TEXT("Position and collision stay in the reported dungeon"),R.ReadUInt32(),0x01430171u);
            R.Skip(28+8);TestEqual(TEXT("Casting slide retains grounded contact"),R.ReadUInt8(),uint8(1));
        }
        // GDLE ChangeCombatMode(mode, playerRequested=true) only publishes
        // CombatMode when rejecting/correcting the request. Success (including
        // an already-active stance) may produce no property or motion reply.
        for (int32 Mode : {8, 2, 4, 1})
        {
            const int32 Previous = Mode == 1 ? 8 : 1;
            Session.PendingCombatMode = 0;
            Session.ApplyServerCombatMode(Previous);
            const uint32 Revision = Session.GetCombatModeRevision();
            Session.SendChangeCombatMode(Mode);
            const auto Bytes = Receive();
            if (TestTrue(TEXT("Stance request reaches the ordered action queue"), Bytes.Num() >= 52))
            {
                FACEBinaryReader R(Bytes); R.Skip(34);
                TestEqual(TEXT("Stance uses weenie queue"), R.ReadUInt16(), ACEQueue::WeenieQueue);
                TestEqual(TEXT("Stance uses game action envelope"), R.ReadUInt32(), uint32(0xF7B1));
                R.ReadUInt32();
                TestEqual(TEXT("Stance opcode"), R.ReadUInt32(), ACEGameAction::ChangeCombatMode);
                TestEqual(TEXT("Requested stance"), R.ReadInt32(), Mode);
            }
            TestEqual(TEXT("GDLE does not wait for a success property that it never sends"), Session.IsCombatModeConfirmed(Mode), GDLE);
            TestEqual(TEXT("Only GDLE request completion releases the transition revision"), Session.GetCombatModeRevision() != Revision, GDLE);
            TestEqual(TEXT("Prediction does not invent a received combat quality"), Session.PlayerVitals.StatQualityInts.FindRef(40), Previous);
            auto ModeReply = [&](int32 Value)
            {
                FACEBinaryWriter W; W.WriteUInt32(ACEOpcode::PrivateUpdatePropertyInt);
                W.WriteUInt8(1); W.WriteUInt32(40); W.WriteInt32(Value);
                Session.HandleGameMessage(W.GetData());
            };
            if (!GDLE)
            {
                ModeReply(Mode);
                TestTrue(TEXT("ACE still requires its property acknowledgment"), Session.IsCombatModeConfirmed(Mode));
            }
            else
            {
                Session.SendChangeCombatMode(Mode); Receive();
                TestTrue(TEXT("An already-active GDLE stance needs no new reply"), Session.IsCombatModeConfirmed(Mode));
            }
            ModeReply(Previous);
            TestFalse(TEXT("Server correction immediately revokes the requested stance"), Session.IsCombatModeConfirmed(Mode));
            TestTrue(TEXT("Corrected mode is usable on both server types"), Session.IsCombatModeConfirmed(Previous));
        }
        Session.Disconnect(); Receive(); // discard clean disconnect before next fixture
    }
    Receiver->Close(); Sockets->DestroySocket(Receiver);
    return true;
}
#endif
