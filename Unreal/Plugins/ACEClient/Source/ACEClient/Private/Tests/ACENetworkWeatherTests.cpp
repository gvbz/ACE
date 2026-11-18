#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACESession.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACESkyDomeActor.h"
#include "Sky/ACESkyMotion.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ACETerrainPresenterComponent.h"
#include "Protocol/ACEHash32.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "RenderingThread.h"
#include "Misc/ScopeExit.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailNetworkWeatherTest, "ACE.RetailParity.NetworkWeather",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACERetailNetworkWeatherTest::RunTest(const FString& Parameters)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .RequiresHitProxies(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* GI = NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    FString Directory = TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
    if (!TestTrue(TEXT("Open retail weather DAT"),Dat->LoadDatDirectory(Directory))) return false;
    auto* Client = GI->GetSubsystem<UACEClientSubsystem>();
    auto Session = Client->GetSession();
    auto* Sky = World->SpawnActor<AACESkyDomeActor>();
    Sky->DatSubsystem = Dat; Sky->ClientSubsystem = Client;
    auto* Presenter = NewObject<UACETerrainPresenterComponent>(Sky);
    Presenter->Client = Client; Presenter->bHasKnownCell = true; Presenter->LastKnownCellId = 0x7D640001;
    Sky->AddInstanceComponent(Presenter); Presenter->RegisterComponent();
    auto Packet = [](uint32 Seq,EACEPacketHeaderFlags Flags,const TArray<uint8>& Body,uint32 Key=0)
    {
        const uint32 Hash = FACESession::HeaderHash32(Seq,Flags,0,0,Body.Num(),1) + (FACEHash32::Calculate(Body)^Key);
        FACEBinaryWriter W; W.WriteUInt32(Seq); W.WriteUInt32(uint32(Flags)); W.WriteUInt32(Hash);
        W.WriteUInt16(0); W.WriteUInt16(0); W.WriteUInt16(Body.Num()); W.WriteUInt16(1); W.WriteBytes(Body);
        return W.GetData();
    };
    auto Deliver = [&](const TArray<uint8>& Bytes) { Session->HandleDatagram(Bytes.GetData(),Bytes.Num(),true); };
    uint32 Seq = 2;
    FACEIsaac ServerKeys(0x12345678u);
    auto SyncPacket = [&](uint32 Sequence,double Ticks)
    {
        FACEBinaryWriter W; W.WriteDouble(Ticks);
        return Packet(Sequence,EACEPacketHeaderFlags::TimeSync|EACEPacketHeaderFlags::EncryptedChecksum,W.GetData(),ServerKeys.Next());
    };
    auto Sync = [&](double Ticks) { Deliver(SyncPacket(Seq++,Ticks)); Sky->Tick(.51f); };
    auto Handshake = [&](double Ticks)
    {
        // PacketOutboundConnectRequest wire layout. No socket, account, or live server.
        Session->State = EACESessionState::AwaitConnectRequest;
        FACEBinaryWriter W; W.WriteDouble(Ticks); W.WriteUInt64(123); W.WriteUInt32(0);
        W.WriteUInt32(0x12345678u); W.WriteUInt32(0x87654321u); W.WriteUInt32(0);
        Deliver(Packet(1,EACEPacketHeaderFlags::ConnectRequest,W.GetData()));
        Seq = 2; ServerKeys = FACEIsaac(0x12345678u); Sky->Tick(.51f);
    };
    auto RainCount = [&]()
    {
        int32 N=0;
        for (const auto& Slot: Sky->Slots)
            if ((Slot.ActiveGfxId==0x01004C44 || Slot.ActiveGfxId==0x01004C42) && Slot.Mesh && Slot.Mesh->IsVisible()) ++N;
        return N;
    };
    auto StarsVisible = [&]()
    {
        for (const auto& Slot: Sky->Slots)
            if (Slot.Def.DefaultGfxObjectId==0x010015EF && Slot.Mesh) return Slot.Mesh->IsVisible();
        return false;
    };
    Sky->Tick(1.f);
    TestFalse(TEXT("Unconnected preview cannot latch a network weather group"),Sky->bBuilt);
    TestEqual(TEXT("Unknown clock honors the caller's fallback"),Client->GetGameDayFraction(.25f),.25f);

    // Independent fixtures from GameTime::CalcDayBegin, SkyDesc::CalcPresentDayGroup
    // and client_portal.dat. Day 39756 hashes to rainy group 19; 39755 to clear 10.
    const double RainNight = 39756.0*7620.0-3600.0+762.0;
    const double RainNoon = 39756.0*7620.0-3600.0+3810.0;
    const double ClearNoon = 39755.0*7620.0-3600.0+3810.0;
    Handshake(RainNight);
    int32 Clouds=0,Celestial=0;
    for(const auto& Slot:Sky->Slots)
    {
        if(Slot.bWeather || Slot.bAfterPass) continue;
        const bool Cloud=!FMath::IsNearlyZero(Slot.Def.TexVelocityX) || !FMath::IsNearlyZero(Slot.Def.TexVelocityY);
        if(Cloud)++Clouds;else ++Celestial;
        if(Slot.Mesh) TestEqual(TEXT("Live sky component uses the intended atmospheric layer"),Slot.Mesh->TranslucencySortPriority,Sky->SlotDrawOrder(Slot));
        for(const auto& Other:Sky->Slots)
            if(Cloud && !Other.bWeather && !Other.bAfterPass && FMath::IsNearlyZero(Other.Def.TexVelocityX) && FMath::IsNearlyZero(Other.Def.TexVelocityY))
                TestTrue(TEXT("Cloud sheets composite in front of planets even if DAT index is earlier"),Sky->SlotDrawOrder(Slot)>Sky->SlotDrawOrder(Other));
    }
    TestTrue(TEXT("Real rainy DAT supplies both celestial and cloud layers"),Clouds>0 && Celestial>0);
    TestTrue(TEXT("Handshake installs the server clock"),Client->HasGameTime());
    TestEqual(TEXT("Server day selects retail rainy group"),Sky->ActiveGroup.DayName,FString(TEXT("Rainy")));
    TestEqual(TEXT("Rain curtains are visible at rainy 0.10"),RainCount(),2);
    TestTrue(TEXT("Network nighttime draws the star shell"),StarsVisible());
    const auto NightFill = Sky->LastFillColor;
    const float NightLight = Sky->AppliedDirIntensity;
    const double T0 = Client->GetGameTimeTicks();
    Session->PortalYearTicksRealtime -= 20.0; // advance the monotonic clock without a blocking sleep
    TestTrue(TEXT("Clock extrapolates between 20-second server updates"),Client->GetGameTimeTicks()>=T0+20.0);
    Sync(RainNoon);
    TestTrue(TEXT("TimeSync changes the phase within the same day"),FMath::IsNearlyEqual(Client->GetGameDayFraction(),.5f,.001f));
    TestEqual(TEXT("TimeSync stops rain in the DAT clear interval"),RainCount(),0);
    TestFalse(TEXT("Network noon hides the star shell"),StarsVisible());
    TestFalse(TEXT("Network noon updates sky/fog tint"),Sky->LastFillColor.Equals(NightFill,.01f));
    TestTrue(TEXT("Network noon updates actual world lighting"),Sky->AppliedDirIntensity>NightLight);

    // Exercise the real authenticated TimeSync -> actor -> material route.
    // DeltaSeconds stays fixed: accelerated timestamps must move the clouds.
    for (double Rate : {.125, 4., 64., 1000.})
    {
        Sync(ClearNoon);
        const auto* Cloud = Sky->Slots.FindByPredicate([](const auto& S)
        { return !S.bWeather && !S.bAfterPass && S.Mesh && S.Mesh->IsVisible() && S.Def.TexVelocityX != 0.f; });
        if (!TestNotNull(TEXT("Clear forecast provides a scrolling cloud"),Cloud)) return false;
        const int32 Index = Cloud->ObjectIndex;
        Sync(ClearNoon + Rate*.25);
        const auto& After = Sky->Slots[Index];
        const auto UV = After.Mids[0]->K2_GetVectorParameterValue(TEXT("UVOffset"));
        const double Expected = ACESkyMotion::Offset(ClearNoon+Rate*.25,After.Def.TexVelocityX);
        const double Difference = FMath::Abs(double(UV.R)-Expected);
        TestTrue(TEXT("Cloud UV follows accelerated server seconds, not frame delta"),FMath::Min(Difference,1.-Difference)<.003);
        TestTrue(TEXT("Timelapse packet reaches the current sky date"),Sky->BuiltDayNumber==39755);
    }
    Sync(RainNoon);

    // Repeatedly disabling an already-disabled real-time capture calls an
    // unguarded UE setter, which queues six cube faces and GPU readback per frame.
    auto* Ambient = Sky->AmbientSkyLightActor ? Sky->AmbientSkyLightActor->GetLightComponent() : nullptr;
    if (TestNotNull(TEXT("Weather creates ambient skylight"), Ambient))
    {
        World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
        TestFalse(TEXT("Skylight state is initially settled"), Ambient->IsRenderStateDirty());
        for (int32 Frame = 0; Frame < 10; ++Frame) Sky->Tick(1.f / 90.f);
        TestFalse(TEXT("Lighting ticks do not recreate or recapture the skylight"), Ambient->IsRenderStateDirty());
    }

    Sync(RainNight);
    Dat->SetInPortalSpace(true); Presenter->UpdateSkyWeatherState();
    TestEqual(TEXT("Portal entry immediately suppresses rain"),RainCount(),0);
    Sync(ClearNoon);
    TestEqual(TEXT("Clock correction during a portal rebuilds the day group"),Sky->ActiveGroup.DayName,FString(TEXT("Clear")));
    Dat->SetInPortalSpace(false); Presenter->UpdateSkyWeatherState(); Sky->Tick(.016f);
    TestEqual(TEXT("Portal exit does not restore the previous rainy group"),RainCount(),0);
    Sync(RainNight);
    Presenter->LastKnownCellId=0x7D640100; Presenter->UpdateSkyWeatherState();
    TestEqual(TEXT("Interior occupancy suppresses rain"),RainCount(),0);
    Presenter->LastKnownCellId=0x7D640001; Presenter->UpdateSkyWeatherState(); Sky->Tick(.016f);
    TestEqual(TEXT("Outdoor return restores current rainfall"),RainCount(),2);

    // Cross midnight through elapsed time, not another packet or a portal rebuild.
    Sync(39757.0*7620.0-3600.0-1.0);
    Session->PortalYearTicksRealtime -= 2.0; Sky->Tick(.016f);
    TestEqual(TEXT("Clock extrapolation rolls into the next retail day"),Sky->BuiltDayNumber,39757);
    TestEqual(TEXT("Rollover uses group 7 from the new retail date"),Sky->PickDayGroup(*Dat->GetRegionSkyInfo(),Client->GetGameTimeTicks()),7);
    Sync(RainNight); // backwards correction must also rebuild
    TestEqual(TEXT("Backwards server correction restores the correct date"),Sky->BuiltDayNumber,39756);
    // Exact clock captured in the user's .9 session: DAT group 7 rains during
    // both this night and noon. Equal server clocks intentionally share weather.
    Sync(302951982.695954);
    TestEqual(TEXT("Reported session selects rainy group 7"),Sky->PickDayGroup(*Dat->GetRegionSkyInfo(),Client->GetGameTimeTicks()),7);
    TestEqual(TEXT("Reported night has both authored rain curtains"),RainCount(),2);
    TestTrue(TEXT("Reported timestamp really is a retail night interval"),StarsVisible());
    Sync(39757.0*7620.0-3600.0+3810.0);
    TestEqual(TEXT("Group 7 deliberately retains rainfall at noon"),RainCount(),2);
    TestFalse(TEXT("A full rainy day still advances to daylight"),StarsVisible());
    Session->ClearWorldState();
    TestTrue(TEXT("Character switch on the same server preserves its clock"),Client->HasGameTime());
    Session->Disconnect(); Sky->Tick(.016f);
    TestFalse(TEXT("Disconnect clears the old server clock"),Client->HasGameTime());
    TestFalse(TEXT("Disconnect tears down the previous sky"),Sky->bBuilt);
    TestEqual(TEXT("No old rain survives disconnect"),Sky->Slots.Num(),0);
    Handshake(ClearNoon);
    TestEqual(TEXT("New server handshake replaces the old rainy night"),Sky->ActiveGroup.DayName,FString(TEXT("Clear")));
    TestFalse(TEXT("New server noon does not inherit stars"),StarsVisible());

    // TimeSync includes time spent waiting for the bounded dispatch pass.
    // A UDP gap itself must not hold the synchronized clock back.
    const auto Earlier = SyncPacket(Seq++,RainNight);
    const uint32 QueuedSeq=Seq++;
    const auto Later = SyncPacket(QueuedSeq,RainNoon);
    Session->bDeferOrderedDispatch=true;
    Deliver(Later);
    if (!TestTrue(TEXT("Deferred time sync retains its packet receipt"), Session->OutOfOrderS2CPackets.Contains(QueuedSeq))) return false;
    const double Arrival = Session->OutOfOrderS2CPackets.FindChecked(QueuedSeq).ReceivedAt;
    Session->OutOfOrderS2CPackets.FindChecked(QueuedSeq).ReceivedAt -= 12.0;
    Deliver(Earlier);Session->bDeferOrderedDispatch=false;Session->DrainOutOfOrderS2C();Sky->Tick(.016f);
    TestTrue(TEXT("A queued sync retains its receipt time"),Session->PortalYearTicksRealtime<=Arrival-12.0);
    TestTrue(TEXT("Waiting for dispatch cannot freeze the game clock"),Client->GetGameTimeTicks()>=RainNoon+12.0);
    auto Bad = SyncPacket(Seq,RainNight); Bad[8]^=1; Deliver(Bad);
    TestEqual(TEXT("Invalid encrypted checksum cannot change weather time"),Session->PortalYearTicksAtConnect,RainNoon);
    Sync(std::numeric_limits<double>::quiet_NaN());
    TestEqual(TEXT("NaN timestamp cannot poison the clock"),Session->PortalYearTicksAtConnect,RainNoon);
    Sync(std::numeric_limits<double>::infinity());
    TestEqual(TEXT("Infinite timestamp cannot poison the clock"),Session->PortalYearTicksAtConnect,RainNoon);
    Sync(-1.0);
    TestEqual(TEXT("Negative timestamp cannot poison the clock"),Session->PortalYearTicksAtConnect,RainNoon);
    Sync(0.0);
    TestTrue(TEXT("Zero server epoch is valid, not unknown"),Client->HasGameTime());
    TestTrue(TEXT("Zero epoch uses retail Morntide phase, not fallback noon"),
        FMath::IsNearlyEqual(Client->GetGameDayFraction(),3600.f/7620.f,.001f));
    Session->SetState(EACESessionState::Failed); Sky->Tick(.016f);
    TestFalse(TEXT("Connection failure clears clock and sky"),Client->HasGameTime() || Sky->bBuilt);
    return true;
}
#endif
