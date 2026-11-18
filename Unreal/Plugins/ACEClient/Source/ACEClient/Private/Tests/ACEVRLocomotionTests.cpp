#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACESession.h"
#include "ACEOpcodes.h"
#include "VR/ACEVRLocomotion.h"
#include "Protocol/ACEIsaac.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVRLocomotionTest,"ACE.VR.UniformLocomotionProtocol",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEVRLocomotionTest::RunTest(const FString&)
{
    for (const bool Running : {false,true})
    for (const float Magnitude : {0.f,.25f,1.f})
    for (int32 Degrees=0;Degrees<360;Degrees+=15)
    {
        const float Angle=FMath::DegreesToRadians(float(Degrees));
        const FVector V=ACEVRLocomotion::Velocity(FMath::Cos(Angle)*Magnitude,FMath::Sin(Angle)*Magnitude,Running,2.6f);
        TestTrue(TEXT("Equal stick length gives equal speed in every direction"),
            FMath::IsNearlyEqual(V.Size2D(),double((Running?10.4f:3.12f)*Magnitude),.00001));
    }
    TestTrue(TEXT("Diagonal input cannot exceed forward speed"),
        FMath::IsNearlyEqual(ACEVRLocomotion::Velocity(1,1,true,2).Size(),8.,.00001));
    TestTrue(TEXT("Invalid stick input cannot enter prediction"),ACEVRLocomotion::Velocity(std::numeric_limits<float>::quiet_NaN(),1,true,2).IsZero());
    TestTrue(TEXT("Burdened walking jump obeys the server leave-ground speed cap"),
        FMath::IsNearlyEqual(ACEVRLocomotion::JumpVelocity(-1,1,false,.2f).Size(),.8,.00001));

    FACESession Sender;
    auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto Address=Sockets->CreateInternetAddr();bool Valid=false;
    Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
    auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("VR locomotion test"),false);
    if(!Receiver)return false;
    ON_SCOPE_EXIT {Receiver->Close();Sockets->DestroySocket(Receiver);};
    if(!TestTrue(TEXT("Loopback binds"),Receiver->Bind(*Address)))return false;
    Receiver->GetAddress(*Address);
    Sender.SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("VR locomotion sender"),false);
    Sender.SocketC2S->SetNonBlocking(true);Sender.ServerC2SAddr=Address;Sender.IssacClient=MakeUnique<FACEIsaac>(123);
    Sender.State=EACESessionState::InWorld;Sender.PlayerGuid=100;Sender.PlayerPosition.CellId=0x7D64000C;
    auto LastPayload=[&]()
    {
        uint32 Last=0;for(const auto& Pair:Sender.CachedC2SPackets)Last=FMath::Max(Last,Pair.Key);
        return Last?Sender.CachedC2SPackets[Last].Payload:TArray<uint8>();
    };
    Sender.VRCapabilities=ACEVRLocomotion::Capability;Sender.SendVRSubscriptions();
    TestTrue(TEXT("A desktop observer never opts into uniform locomotion"),Sender.CachedC2SPackets.IsEmpty());
    Sender.SendMoveToState(-.6f,.8f,0,true,true,false,true);
    const int32 LegacySize=LastPayload().Num();
    TestFalse(TEXT("Capability alone cannot impersonate an active headset"),Sender.SupportsUniformVRMovement());
    Sender.bLocalVRFeedback=true;Sender.VRCapabilities=0;
    Sender.SendMoveToState(-.6f,.8f,0,true,true,false,true);
    TestEqual(TEXT("Older servers receive the unchanged retail packet"),LastPayload().Num(),LegacySize);
    Sender.VRCapabilities=ACEVRLocomotion::Capability;Sender.SendVRSubscriptions();
    auto Subscription=LastPayload();FACEBinaryReader Sub(Subscription);Sub.Skip(36);
    TestEqual(TEXT("Enhanced locomotion is explicitly subscribed"),Sub.ReadUInt32(),ACEVRLocomotion::Subscription);
    Sender.SendMoveToState(-.6f,.8f,0,true,true,false,true);
    auto Enhanced=LastPayload();TestEqual(TEXT("Negotiated input appends only a versioned marker"),Enhanced.Num(),LegacySize+4);
    FACEBinaryReader Wire(Enhanced);Wire.Skip(28);
    TestEqual(TEXT("Raw input retains the retail body"),Wire.ReadUInt32(),uint32(0xff));
    Wire.ReadUInt32();Wire.ReadUInt32();
    TestEqual(TEXT("Head-facing backward motion stays backward"),Wire.ReadUInt32(),ACEMotion::WalkBackwards);
    Wire.ReadUInt32();TestEqual(TEXT("Forward stick magnitude retained"),Wire.ReadFloat(),.6f);
    TestEqual(TEXT("Sideward motion stays sideward"),Wire.ReadUInt32(),ACEMotion::SideStepRight);
    Wire.ReadUInt32();TestEqual(TEXT("Side stick magnitude retained"),Wire.ReadFloat(),.8f);
    Wire.Skip(44);TestEqual(TEXT("Trailing locomotion version marker"),Wire.ReadUInt32(),ACEVRLocomotion::MoveMarker);
    TestEqual(TEXT("No tracking pose is required to report locomotion"),Sender.VRPoseSequence,0u);

    FACESession Desktop;
    Desktop.State=EACESessionState::InWorld;Desktop.PlayerGuid=200;
    FACEWorldObject Remote;Remote.Guid=100;Remote.bIsPlayer=true;Remote.ItemType=ACEItemType::Creature;
    Remote.bHasPhysicsTimestamps=true;Remote.PhysicsTimestamps[ACEPhysicsTimeStamp::Instance]=3;
    Desktop.WorldObjects.Add(Remote.Guid,Remote);
    FACEObjectMotionState Seen;int32 Received=0;
    Desktop.OnMotionUpdate.AddLambda([&](int32,const FACEObjectMotionState& M){Seen=M;++Received;});
    uint16 Sequence=0;
    for(const FVector2D Stick:{FVector2D(0,-1),FVector2D(1,0),FVector2D(-.8,-.6),FVector2D(.95,.2).GetSafeNormal()})
    {
        const FVector V=ACEVRLocomotion::Velocity(Stick.Y,Stick.X,true,2.6f);
        const uint16 Command=Stick.Y>0?7:5;
        const float ForwardRate=V.Y/(Command==7?4.f:3.12f),SideRate=V.X/1.25f;
        FACEBinaryWriter W;W.WriteUInt32(Remote.Guid);W.WriteUInt16(3);W.WriteUInt16(++Sequence);
        W.WriteUInt16(0);W.WriteUInt8(1);W.Align();
        W.WriteUInt8(0);W.WriteUInt8(0);W.WriteUInt16(0x3D);
        W.WriteUInt32(0x1E);W.WriteUInt16(Command);W.WriteUInt16(0xF);W.WriteFloat(ForwardRate);W.WriteFloat(SideRate);W.Align();
        FACEBinaryReader R(W.GetData());Desktop.HandleUpdateMotion(R);
        TestTrue(TEXT("Standard desktop observer retains full forward velocity"),FMath::IsNearlyEqual(Seen.ForwardUnitsPerSecond,float(V.Y),.0001f));
        TestTrue(TEXT("Standard desktop observer does not clamp fast strafe rates"),FMath::IsNearlyEqual(Seen.StrafeUnitsPerSecond,float(V.X),.0001f));
        if(FMath::Abs(V.X)>FMath::Abs(V.Y))
        {
            TestTrue(TEXT("Fast sideways movement chooses the sideways animation"),FMath::Abs(Seen.Strafe)>FMath::Abs(Seen.Forward));
            // The forward cycle and sidestep modifier have independent rates.
            // A fast diagonal strafe cannot speed up the forward root motion.
            const float CycleRate=FMath::IsNearlyZero(V.Y)?SideRate:ForwardRate;
            TestTrue(TEXT("Observer retains the independent authored cycle rate"),FMath::IsNearlyEqual(Seen.AnimPlayRate,FMath::Max(.05f,FMath::Abs(CycleRate)),.0001f));
        }
    }
    TestEqual(TEXT("All standard movement updates reach a desktop without VR capabilities"),Received,4);
    return true;
}
#endif
