#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Framework/Application/SlateApplication.h"
#include "ACEPlayerController.h"
#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEOpcodes.h"
#include "ACEDatSubsystem.h"
#include "ACEEnvCellActor.h"
#include "ACELandblockActor.h"
#include "ACETerrainChunkActor.h"
#include "ACETerrainPresenterComponent.h"
#include "ACELoadingScreenActor.h"
#include "ACERetailPortalAnimation.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEScriptComponent.h"
#include "ACECharacterCreation.h"
#include "ACEHoverTooltipWidget.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ShaderCompiler.h"
#include "RenderingThread.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
#include "GameFramework/SpringArmComponent.h"
#include "ACEInputBindings.h"
#include "ACEKeyboardRouter.h"
#include "Misc/ConfigCacheIni.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Async/TaskGraphInterfaces.h"
#include "Dat/ACEDiskTileCache.h"
#include "HAL/FileManager.h"
#include "ACEWorldEntityActor.h"
#include "../ACEBodySweep.h"
#include "Dat/ACECellTransit.h"
#include "../UI/ACERadarVisuals.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUIElementManager.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Misc/ScopeExit.h"
#include "Engine/Texture2D.h"
#include "GameFramework/GameModeBase.h"
#include "UObject/GarbageCollection.h"

namespace
{
    struct FEntryWorld
    {
        UWorld* World;
        UGameInstance* GI;
        FEntryWorld()
        {
            const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
                .RequiresHitProxies(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
            World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
            auto& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
            Context.SetCurrentWorld(World);
            GI = NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI);
            Context.OwningGameInstance = GI;
            // Runtime Chaos cooking resolves its game world through the mesh's
            // DAT subsystem. Setting the world's GI alone does not set GI->GetWorld().
            GI->OnWorldChanged(nullptr,World);
            GI->Init();
        }
        ~FEntryWorld()
        {
            GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECreatureEscapeTest,"ACE.RetailParity.CreatureEscape",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACECreatureEscapeTest::RunTest(const FString&)
{
    FEntryWorld F;
    auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory()))return false;
    const auto Player=FCollisionShape::MakeCapsule(48,91.75);
    FCollisionQueryParams Q(SCENE_QUERY_STAT(CreatureEscape),true);
    for(uint32 Setup:{0x02000964u,0x02000041u,0x02001121u})
    {
        FACEWorldObject Obj;Obj.Guid=9821;Obj.SetupId=Setup;Obj.ItemType=ACEItemType::Creature;
        Obj.PhysicsState=ACEPhysicsState::Gravity;Obj.bHasPosition=true;
        Obj.Position.CellId=0x016C0101;Obj.Position.Location=FVector(50,50,100);
        const FVector Base=Obj.Position.ToUnrealLocation(100);
        auto* Mob=F.World->SpawnActor<AACEWorldEntityActor>();Mob->InitializeFromObject(Obj,100,true);
        Mob->SetActorLocation(Base);
        TArray<FACEDatCollisionShape> Spheres;bool BSP=false;Dat->GetSetupCollisionShapes(Setup,Spheres,BSP);
        double Width=0;
        for(const auto& S:Spheres)for(double Z:{48.,135.5})
            Width=FMath::Max(Width,FMath::Sqrt(FMath::Max(0.,FMath::Square(48.+S.Radius*100)-FMath::Square(Z-S.Origin.Z*100))));
        const FVector Start=Base+FVector(Width-20,0,91.75);
        FHitResult Hit;
        TestTrue(TEXT("A moving mob can initially overlap the player"),ACEBodySweep::OverlapsBody(*F.World,Start,Player,Q));
        TestFalse(TEXT("Backing out of an overlapping creature remains possible"),ACEBodySweep::Sweep(*F.World,Hit,Start,Start+FVector(5,0,0),Player,Q));
        TestTrue(TEXT("Moving deeper into the creature is still blocked"),ACEBodySweep::Sweep(*F.World,Hit,Start,Start-FVector(5,0,0),Player,Q));
        TestTrue(TEXT("Authored creature overlap cannot forcibly shove the player"),
            ACEBodySweep::Recover(*F.World,Start,Hit.Normal*(Hit.PenetrationDepth+.2f),Hit,Player,Q).Equals(Start,.01));
        auto* Wall=F.World->SpawnActor<AActor>();auto* Box=NewObject<UBoxComponent>(Wall);
        Wall->SetRootComponent(Box);Wall->AddInstanceComponent(Box);Box->SetBoxExtent(FVector(10,1000,200));
        Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Box->SetCollisionResponseToAllChannels(ECR_Block);Box->RegisterComponent();
        Wall->SetActorLocation(Start+FVector(58.1,0,0));
        for(int32 FPS:{30,90,144})for(double Sign:{-1.,1.})
        {
            FVector Position=Start;
            for(int32 Frame=0;Frame<FPS;++Frame)
            {
                const FVector End=Position+FVector(0,Sign*300./FPS,0);
                if(ACEBodySweep::Sweep(*F.World,Hit,Position,End,Player,Q))
                    Position=ACEBodySweep::SlideGrounded(*F.World,Position,End,Hit,Player,Q);
                else Position=End;
            }
            TestTrue(FString::Printf(TEXT("%08X tangent escape beside wall at %d fps (%+.0f)"),Setup,FPS,Sign),Sign*(Position.Y-Start.Y)>290);
            TestTrue(TEXT("Creature escape never lifts or lowers a grounded player"),FMath::IsNearlyEqual(Position.Z,Start.Z,.01));
            TestTrue(TEXT("Escape retains the adjacent wall"),Position.X<=Start.X+.2);
        }
        // The initial creature hit must not conceal a different solid farther along the escape.
        Wall->SetActorLocation(Start+FVector(0,110,0));Box->SetBoxExtent(FVector(1000,10,200));
        TestTrue(TEXT("An escape still sweeps the wall behind the creature"),ACEBodySweep::Sweep(*F.World,Hit,Start,Start+FVector(0,300,0),Player,Q));
        TestEqual(TEXT("Architecture, not the initial mob overlap, blocks the escape"),Hit.GetActor(),Wall);
        Wall->Destroy();
        auto* Other=F.World->SpawnActor<AACEWorldEntityActor>();Other->InitializeFromObject(Obj,100,true);
        Other->SetActorLocation(Base+FVector(Width-20,320,0));
        TestTrue(TEXT("Escaping one mob cannot pass through the next mob"),ACEBodySweep::Sweep(*F.World,Hit,Start,Start+FVector(0,400,0),Player,Q));
        TestEqual(TEXT("The next creature still blocks"),Hit.GetActor(),static_cast<AActor*>(Other));
        Other->Destroy();Mob->Destroy();
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPortalPlacementTest,"ACE.RetailParity.PortalPlacement",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPortalPlacementTest::RunTest(const FString&)
{
    FEntryWorld F;auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory()))return false;
    auto* Client=F.GI->GetSubsystem<UACEClientSubsystem>();
    auto* PC=F.World->SpawnActor<AACEPlayerController>();PC->Client=Client;F.World->AddController(PC);
    auto* Pawn=F.World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
    Capsule->InitCapsuleSize(48,91.75);Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);
    Capsule->RegisterComponent();PC->Possess(Pawn);Pawn->SetActorEnableCollision(false);
    auto* Terrain=NewObject<UACETerrainPresenterComponent>(PC);PC->AddInstanceComponent(Terrain);
    Terrain->RegisterComponent();Terrain->Client=Client;
    FACEPosition Pose;Pose.CellId=0x02910388;Pose.Location=FVector(210,-120,6);
    Pawn->SetActorLocation(Pose.ToUnrealLocation(100)+FVector(0,0,91.75));
    Client->GetSession()->SetLocalPosition(Pose);
    Terrain->LastKnownCellId=Pose.CellId;Terrain->bHasKnownCell=true;
    Dat->SetWorldStreamingAllowed(true);Dat->SetInPortalSpace(true);
    TArray<int32> Ordered;TSet<int32> Needed;Terrain->CollectNeededEnvCells(Ordered,Needed);
    const FVector Origin=FACEPosition::AceVectorToUnreal(FVector(2*192,145*192,0),100);
    for(int32 Id:Ordered)
    {
        Dat->GetOrBuildEnvCellMesh(Id,100);
        auto* Room=F.World->SpawnActor<AACEEnvCellActor>();
        if(Room->LoadEnvCell(Id,Origin,100))Terrain->SpawnedEnvCells.Add(Id,Room);
        else AddError(FString::Printf(TEXT("Cannot load Bore cell %08X"),Id));
    }
    const double Deadline=FPlatformTime::Seconds()+30;
    bool Pending=true;
    while(Pending && FPlatformTime::Seconds()<Deadline)
    {
        ++GFrameCounter;
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        Terrain->UpdateBuildingVisibility();Pending=false;
        for(const auto& Pair:Terrain->SpawnedEnvCells)
        {
            Pair.Value->Tick(.016f);
            if(Pair.Value->GetActorEnableCollision())Pending|=!Pair.Value->IsCollisionCooked()||Pair.Value->HasPendingStaticObjects();
        }
        if(Pending)FPlatformProcess::Sleep(.002f);
    }
    TestFalse(TEXT("Bore arrival physics and static objects finish streaming"),Pending);
    const FVector Feet=Pose.ToUnrealLocation(100);FHitResult Hit;
    FCollisionQueryParams Q(SCENE_QUERY_STAT(BorePlacement),false,Pawn);
    const bool HasFloor=F.World->LineTraceSingleByChannel(Hit,Feet+FVector(0,0,80),Feet-FVector(0,0,3000),ECC_Pawn,Q);
    AddInfo(FString::Printf(TEXT("Bore cells=%d floor=%d deltaZ=%.3f component=%s"),Ordered.Num(),HasFloor,Hit.Location.Z-Feet.Z,*GetNameSafe(Hit.GetComponent())));
    AddInfo(FString::Printf(TEXT("Bore foot point inside arrival cell=%d"),ACECellTransit::SphereIntersectsEnvCell(*Dat,Pose.CellId,Feet+FVector(0,0,48),0,100)));
    FVector Placement;
    TestTrue(TEXT("Singularity Bore's authored portal destination can enter the world"),PC->FindWorldEntryPlacement(Placement));
    AddInfo(FString::Printf(TEXT("Bore placement=%s arrival=%s"),*Placement.ToString(),*Feet.ToString()));
    // Portal entry temporarily ignores dynamic contacts on the server. A crowd
    // at an authoritative destination is not a missing floor or an invalid cell.
    TArray<AActor*> Crowd;
    for(int32 X=-2;X<=2;++X)for(int32 Y=-2;Y<=2;++Y)
    {
        auto* Mob=F.World->SpawnActor<AActor>();auto* Sphere=NewObject<USphereComponent>(Mob);
        Mob->SetRootComponent(Sphere);Mob->AddInstanceComponent(Sphere);Sphere->InitSphereRadius(99.6f);
        Sphere->ComponentTags.Add(TEXT("ACECreatureBody"));Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Sphere->SetCollisionResponseToAllChannels(ECR_Ignore);Sphere->SetCollisionResponseToChannel(ECC_Pawn,ECR_Block);
        Sphere->RegisterComponent();Mob->SetActorLocation(Feet+FVector(X*190,Y*190,99.6));Crowd.Add(Mob);
    }
    TestTrue(TEXT("Creatures at the Bore exit cannot trigger invalid-landing recovery"),PC->FindWorldEntryPlacement(Placement));
    TestTrue(TEXT("Crowded portal arrival retains the server destination"),Placement.Equals(Feet+FVector(0,0,92.25),1.));
    for(auto* Mob:Crowd)Mob->Destroy();
    Pose.CellId=0x01010001;Pose.Location=FVector(40,40,1000);Client->GetSession()->SetLocalPosition(Pose);
    auto* Body=F.World->SpawnActor<AActor>();auto* Sphere=NewObject<USphereComponent>(Body);
    Body->SetRootComponent(Sphere);Body->AddInstanceComponent(Sphere);Sphere->InitSphereRadius(99.6f);
    Sphere->ComponentTags.Add(TEXT("ACECreatureBody"));Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Sphere->SetCollisionResponseToAllChannels(ECR_Block);Sphere->RegisterComponent();
    Body->SetActorLocation(Pose.ToUnrealLocation(100)-FVector(0,0,99.6));
    TestFalse(TEXT("A creature alone cannot make missing portal terrain safe"),PC->FindWorldEntryPlacement(Placement));
    Body->Destroy();
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPortalMotionTest,"ACE.RetailParity.PortalArrivalMotion",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPortalMotionTest::RunTest(const FString&)
{
    FEntryWorld F;
    auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
    if(!TestTrue(TEXT("Retail DAT opens"),Dat->LoadDatDirectory(ACETestDatDirectory())))return false;
    auto* Client=F.GI->GetSubsystem<UACEClientSubsystem>();auto Session=Client->GetSession();
    auto* PC=F.World->SpawnActor<AACEPlayerController>();PC->Client=Client;F.World->AddController(PC);
    auto* Pawn=F.World->SpawnActor<APawn>();auto* Root=NewObject<USceneComponent>(Pawn);
    Pawn->SetRootComponent(Root);Pawn->AddInstanceComponent(Root);Root->RegisterComponent();PC->Possess(Pawn);
    auto* App=NewObject<UACECharacterAppearanceComponent>(Pawn);Pawn->AddInstanceComponent(App);App->RegisterComponent();
    FACEWorldObject Self;Self.Guid=123;Self.SetupId=0x02000001;Self.MotionTableId=0x09000001;Self.bIsPlayer=true;
    if(!TestTrue(TEXT("Player uses actual retail animation geometry"),App->ApplyWorldObject(Self,100,false)))return false;
    Session->PlayerVitals.bValid=true;
    for(uint32 Mode:{ACECombatMode::NonCombat,ACECombatMode::Magic})
    for(float Dt:{1.f/30,1.f/90,1.f/144})
    for(int Scenario=0;Scenario<3;++Scenario)
    {
        Session->PlayerVitals.CombatMode=Mode;
        const uint32 Style=Mode==ACECombatMode::Magic?ACEMotion::StanceMagic:ACEMotion::StanceNonCombat;
        App->SetPreferredStyle(Style);
        App->SetLocomotionInput(1,0,Scenario!=2,1.5f);
        App->TickComponent(.1f,LEVELTICK_All,nullptr);
        if(Scenario==1)
        {
            App->SetHeldActionMotion(0x40000015); // Portal entered during a running landing.
            App->ClearJumpMotionIfAny(true);
            TestEqual(TEXT("Fixture has a pending Falling->Run link"),App->ActionCommand,0x44000007u);
            App->QueueActionMotion(ACEMotion::Ready);
        }
        PC->ForwardAxis=1;PC->RightAxis=1;PC->TurnAxis=1;PC->bAutoRun=true;
        PC->bMouseLookToggled=true;
        // Shared arrival path used by the tunnel reveal and instant teleports.
        PC->InvalidateMovementAfterTeleport();
        TestFalse(TEXT("Teleport cancels autorun as retail does"),PC->bAutoRun);
        TestTrue(TEXT("Teleport retains mouselook intent"),PC->bMouseLookToggled);
        TestTrue(TEXT("No cached movement survives arrival"),PC->ForwardAxis==0 && PC->RightAxis==0 && PC->TurnAxis==0);
        TestTrue(TEXT("Arrival discards locomotion, walk links and queued actions"),App->LocomotionForward==0
            && App->LocomotionStrafe==0 && !App->HasWalkingTransition() && App->ActionCommand==0
            && App->PendingActionCommands.IsEmpty() && App->QueuedHoldAction==0);
        TestEqual(TEXT("Local idle keeps the restored combat stance"),App->PreferredStyle,Style);
        TestEqual(TEXT("Network stop keeps the restored combat stance"),Session->GetCurrentStance(),Style);
        TestTrue(TEXT("Grounded arrival reports contact"),Session->bAutoPosContact);
        // Check the rendered first frame and subsequent three seconds against
        // retail Ready, not merely the state fields: no departed pose crossfade.
        float Elapsed=0;
        for(int Frame=0;Frame<=FMath::CeilToInt(3.f/Dt);++Frame)
        {
            if(Frame>0){App->TickComponent(Dt,LEVELTICK_All,nullptr);Elapsed+=Dt;}
            TArray<FTransform> Expected;int32 Count=0;
            if(!TestTrue(TEXT("Retail Ready resolves"),Dat->EvaluateMotionCommand(0x09000001,ACEMotion::Ready,
                Elapsed,App->GetPartCount(),Expected,100,Count,nullptr,nullptr,Style)))return false;
            for(int Part=0;Part<Count;++Part)
            {
                // Render updates intentionally skip sub-millimeter translation
                // changes. Compare within that tolerance, keeping rotation tight.
                const FTransform& Actual=App->GetPartMesh(Part)->GetRelativeTransform();
                if(!Actual.GetTranslation().Equals(Expected[Part].GetTranslation(),.05)
                    || !Actual.GetRotation().Equals(Expected[Part].GetRotation(),.00002)
                    || !Actual.GetScale3D().Equals(Expected[Part].GetScale3D(),.0001))
                {AddError(FString::Printf(TEXT("Portal idle differs: mode %u scenario %d frame %d part %d"),Mode,Scenario,Frame,Part));return false;}
            }
        }
        // Actual new movement resumes immediately; it is not locked by a cooldown.
        App->SetLocomotionInput(1,0,true,1);App->TickComponent(.1f,LEVELTICK_All,nullptr);
        TestEqual(TEXT("New input resumes running"),App->LocomotionForward,1.f);
    }
    App->SetLocomotionInput(1,0,true,1);
    App->SetHeldActionMotion(0x40000015);App->ClearJumpMotionIfAny(true);
    PC->InvalidateMovementAfterTeleport(true);
    TestEqual(TEXT("Airborne portal arrival uses Falling, never a running landing"),App->ActionCommand,0x40000015u);
    TestTrue(TEXT("Airborne arrival holds Falling and discards old movement"),App->bHoldActionFinal && App->LocomotionForward==0);
    TestTrue(TEXT("Airborne arrival retains prediction and reports no contact"),PC->bJumpAirborne && PC->bLocalPredicting && !Session->bAutoPosContact);
    // Repeated arrivals must also release a previous airborne hold.
    PC->InvalidateMovementAfterTeleport();
    TestTrue(TEXT("Following grounded teleport restores idle/contact"),App->ActionCommand==0 && !PC->bJumpAirborne && Session->bAutoPosContact);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPortalCellLookupTest,"ACE.RetailParity.PortalCellLookup",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPortalCellLookupTest::RunTest(const FString&)
{
    FEntryWorld F;
    auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    struct FArrival { uint32 Hint,Expected; FVector Location; };
    const FArrival Arrivals[]={
        #include "ACEPortalCellFixtures.inl"
    };
    const double Start=FPlatformTime::Seconds();
    for(const auto& Arrival:Arrivals)
    {
        FACEPosition Pose;Pose.CellId=Arrival.Hint;Pose.Location=Arrival.Location;
        uint32 Resolved=0;bool Complete=false;
        TestTrue(FString::Printf(TEXT("Arrival %08X at %s resolves"),Arrival.Hint,*Pose.Location.ToString()),
            Dat->ResolveWorldEntryCell(Pose,Resolved,Complete));
        // Shared room boundaries can have more than one valid cell. The physical
        // arrival test below checks all 104 against actual streamed collision.
        if(Resolved!=Arrival.Expected) AddInfo(FString::Printf(TEXT("Shared boundary %08X -> %08X (reported alternative %08X)"),Arrival.Hint,Resolved,Arrival.Expected));
        if(Arrival.Hint==0x014F01B9u) TestEqual(TEXT("Maggreth resolves to its actual entrance"),Resolved,Arrival.Expected);
        Pose.CellId=Resolved;
        TestTrue(TEXT("Corrected destination is stable"),Dat->ResolveWorldEntryCell(Pose,Resolved,Complete));
        TestEqual(TEXT("Corrected hint stays in its room"),Resolved,uint32(Pose.CellId));
    }
    FACEPosition Invalid;Invalid.CellId=0x014F01B9;Invalid.Location=FVector(3000,3000,-500);
    uint32 Resolved=0;bool Complete=false;
    TestFalse(TEXT("Outside every room has no invented cell"),Dat->ResolveWorldEntryCell(Invalid,Resolved,Complete));
    TestTrue(TEXT("Complete geometry distinguishes an invalid position from missing files"),Complete);
    Invalid.CellId=0xFFFFFFFF;
    TestFalse(TEXT("Missing landblock has no placement"),Dat->ResolveWorldEntryCell(Invalid,Resolved,Complete));
    TestFalse(TEXT("Missing data never qualifies for no-room recall"),Complete);
    AddInfo(FString::Printf(TEXT("Checked %d distinct reported destinations in %.3fs (raw DAT only)"),
        UE_ARRAY_COUNT(Arrivals),FPlatformTime::Seconds()-Start));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPortalReportedPlacementsTest,"ACE.RetailParity.PortalReportedPlacements",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPortalReportedPlacementsTest::RunTest(const FString&)
{
    struct FArrival { uint32 Hint,Expected; FVector Location; };
    const FArrival Arrivals[]={
        #include "ACEPortalCellFixtures.inl"
    };
    for(const auto& Arrival:Arrivals)
    {
        // Retire each independent dungeon fixture before loading the next one.
        ON_SCOPE_EXIT { CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,true); };
        FEntryWorld F;
        auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
        auto* Client=F.GI->GetSubsystem<UACEClientSubsystem>();auto Session=Client->GetSession();
        auto* PC=F.World->SpawnActor<AACEPlayerController>();PC->Client=Client;F.World->AddController(PC);
        if(&Arrival==&Arrivals[0])
        {
            // A real first login indexes portal data before it permits cell data
            // to load. Do not permanently cache "not found" during that gap.
            Dat->SetDatDirectory(ACETestDatDirectory());Dat->BeginBackgroundLoad();
            double Deadline=FPlatformTime::Seconds()+30;
            while(!Dat->IsDatReady()&&FPlatformTime::Seconds()<Deadline)
            { FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);FPlatformProcess::Sleep(.002f); }
            TestTrue(TEXT("Portal-only login index completes"),Dat->IsDatReady());
            TestFalse(TEXT("Cell indexing is deferred until the portal tunnel"),Dat->IsCellReady());
            FACEPosition Initial;Initial.CellId=Arrival.Hint;Initial.Location=Arrival.Location;
            Session->SetLocalPosition(Initial);PC->ResolveWorldEntryDestinationCell();
            TestFalse(TEXT("Deferred cell index must not cache an unresolved arrival"),PC->bWorldEntryPositionChecked);
            Dat->SetWorldStreamingAllowed(true);Deadline=FPlatformTime::Seconds()+30;
            while(!Dat->IsCellReady()&&FPlatformTime::Seconds()<Deadline)
            { FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);FPlatformProcess::Sleep(.002f); }
            if(!TestTrue(TEXT("Arrival cell index completes"),Dat->IsCellReady())) return false;
        }
        else if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
        F.World->SetGameMode(FURL(nullptr,TEXT("/Game/Test?game=/Script/Engine.GameModeBase"),TRAVEL_Absolute));
        auto* GM=F.World->GetAuthGameMode();
        if(!TestNotNull(TEXT("Reported portal game mode"),GM)) return false;
        auto* Terrain=NewObject<UACETerrainPresenterComponent>(GM);
        GM->AddInstanceComponent(Terrain);Terrain->RegisterComponent();Terrain->Client=Client;
        auto* Pawn=F.World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
        Capsule->InitCapsuleSize(48,91.75);Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);
        Capsule->RegisterComponent();PC->Possess(Pawn);Pawn->SetActorEnableCollision(false);
        FACEPosition Pose;Pose.CellId=Arrival.Hint;Pose.Location=Arrival.Location;
        Session->SetLocalPosition(Pose);PC->ResolveWorldEntryDestinationCell();Pose=Session->GetPlayerPosition();
        TestTrue(TEXT("Cell correction preserves server XYZ"),Pose.Location.Equals(Arrival.Location,0));
        Terrain->LastKnownCellId=Pose.CellId;Terrain->bHasKnownCell=true;
        Dat->SetWorldStreamingAllowed(true);Dat->SetInPortalSpace(true);
        TArray<int32> Ordered;TSet<int32> Needed;Terrain->CollectNeededEnvCells(Ordered,Needed);
        const FVector Origin=FACEPosition::AceVectorToUnreal(FVector((Arrival.Hint>>24)*192,((Arrival.Hint>>16)&255)*192,0),100);
        for(int32 Id:Ordered)
        {
            Dat->GetOrBuildEnvCellMesh(Id,100);
            auto* Room=F.World->SpawnActor<AACEEnvCellActor>();
            if(Room->LoadEnvCell(Id,Origin,100)) Terrain->SpawnedEnvCells.Add(Id,Room);
            else AddError(FString::Printf(TEXT("Cannot load reported arrival %08X cell %08X"),Arrival.Hint,Id));
        }
        const double Deadline=FPlatformTime::Seconds()+30;
        do
        {
            ++GFrameCounter;FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
            Terrain->UpdateBuildingVisibility();
            for(const auto& Pair:Terrain->SpawnedEnvCells) Pair.Value->Tick(.016f);
            if(PC->IsWorldEntryGeometryReady()) break;
            FPlatformProcess::Sleep(.002f);
        } while(FPlatformTime::Seconds()<Deadline);
        TestTrue(FString::Printf(TEXT("Reported arrival %08X collision is ready"),Arrival.Hint),PC->IsWorldEntryGeometryReady());
        FVector Placement;AACEPlayerController::EWorldEntryPlacement State;uint32 Cell=0;
        const bool Placed=PC->FindWorldEntryPlacement(Placement,&State,&Cell);
        TestTrue(FString::Printf(TEXT("Reported arrival %08X leaves portal space"),Arrival.Hint),Placed);
        if(Placed)
        {
            TestTrue(TEXT("Final placement uses resident geometry"),PC->IsWorldEntryGeometryReady(Cell));
            if(State==AACEPlayerController::EWorldEntryPlacement::Airborne)
            {
                // Some authored points are 0.7m above their entrance floor,
                // beyond retail's normal step-down. Preserve the short fall
                // while proving that actual resident collision catches it.
                const auto Body=PC->GetPlayerCollisionBody();FHitResult Floor;
                const FVector Start=Placement+Body.Centers[0];
                const bool Supported=F.World->SweepSingleByChannel(Floor,Start,Start-FVector(0,0,200),
                    FQuat::Identity,ECC_Pawn,FCollisionShape::MakeSphere(Body.Radii[0]),
                    FCollisionQueryParams(SCENE_QUERY_STAT(ReportedPortalLanding),false,Pawn));
                TestTrue(FString::Printf(TEXT("Reported arrival %08X has a nearby landing floor"),Arrival.Hint),
                    Supported&&!Floor.bStartPenetrating&&Floor.ImpactNormal.Z>=.5f);
                TestTrue(TEXT("Airborne arrival preserves the authored height"),
                    FMath::IsNearlyEqual(Placement.Z-Capsule->GetScaledCapsuleHalfHeight(),Pose.ToUnrealLocation(100).Z,.01));
                AddInfo(FString::Printf(TEXT("Reported landing %08X drop %.2fcm"),Arrival.Hint,Supported?Floor.Distance:-1));
            }
            else TestTrue(TEXT("Supported arrival is grounded"),State==AACEPlayerController::EWorldEntryPlacement::Grounded);
            TestTrue(TEXT("Arrival stays within retail's local placement search"),FVector::Dist2D(Placement,Pose.ToUnrealLocation(100))<=400.1);
        }
        AddInfo(FString::Printf(TEXT("Reported placement %08X -> %08X ready=%d placed=%d state=%d"),Arrival.Hint,Cell,PC->IsWorldEntryGeometryReady(),Placed,int32(State)));
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPortalDropTest,"ACE.RetailParity.PortalDrops",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPortalDropTest::RunTest(const FString&)
{
    // ACE-World-16PY-Patches portal weenies 43001, 7289 and 7508. Exercise their
    // real DAT geometry, not portal-name exceptions or a fabricated floor check.
    struct FArrival { const TCHAR* Name; uint32 Cell; FVector Location; bool Airborne; bool Water=false; };
    TArray<FArrival> Arrivals = {
        {TEXT("Maggreth stale-cell portal"),0x014F01B9,FVector(30,-60,-12),false},
        {TEXT("Weakened Vault Sewers distant-cell portal"),0x000C0142,FVector(110,0,.005),false},
        {TEXT("Count Phainor's Tomb stale-cell portal"),0x58620396,FVector(240,-390,.005),false},
        {TEXT("Withered Beach coastal water arrival"),0x1B120033,FVector(158.300995,50.575699,-.895),false,true},
        {TEXT("Withered Beach drop into coastal water"),0x1B120033,FVector(158.300995,50.575699,1.105),true,true},
        {TEXT("Fort Tethana town-network arrival"),0x2681001D,FVector(77.7,108.1,240),false},
        // Same real outdoor geometry, with a server-authored drop above it.
        {TEXT("Fort Tethana elevated arrival"),0x2681001D,FVector(77.7,108.1,260),true},
        {TEXT("Aerlinthe Reservoir"),0x02EE03D2,FVector(80,-110,6),true},
        {TEXT("Aerlinthe Lower Reservoir"),0x02ED01B5,FVector(70,-30,-72),true}
    };
    // Exercise independently located river and shallow shoreline cells too.
    // Portal placement must depend on DAT terrain, never a portal name, weenie
    // or destination ID. Author the test drops from real water support heights.
    {
        FEntryWorld F;
        auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
        if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
        auto AddWaterArrivals=[&](uint32 Block,float X,float Y,const TCHAR* GroundName,const TCHAR* DropName)
        {
            Dat->GetOrBuildLandblockMesh(Block,100);
            FACEPosition Pose;Pose.CellId=Block|1;Pose.Location=FVector(X,Y,0);Pose.NormalizeOutdoorLandblock();
            const FVector At=Pose.ToUnrealLocation(100);float Ground=0;FVector Normal;
            if(!Dat->SampleOutdoorGroundZ(At.X,At.Y,100,Ground,&Normal) || Normal.Z<.999f
                || Dat->GetOutdoorWaterDepthCm(At.X,At.Y,100)<=20.f
                || Dat->GetOutdoorBlockWaterType(At.X,At.Y,100)==2) return false;
            Pose.Location.Z=(Ground+.5f)/100;
            Arrivals.Add({GroundName,uint32(Pose.CellId),Pose.Location,false,true});
            Pose.Location.Z+=2;
            Arrivals.Add({DropName,uint32(Pose.CellId),Pose.Location,true,true});
            return true;
        };
        TestTrue(TEXT("Independent DC56 water fixture exists"),AddWaterArrivals(0xDC560000,95.269531f,147.908203f,
            TEXT("DC56 water arrival"),TEXT("DC56 water drop")));
        bool FoundShore=false;
        for(uint32 X=198;X<=203 && !FoundShore;++X)for(uint32 Y=137;Y<=143 && !FoundShore;++Y)
        {
            const uint32 Block=(X<<24)|(Y<<16);Dat->GetOrBuildLandblockMesh(Block,100);
            for(int32 I=4;I<188 && !FoundShore;I+=4)for(int32 J=4;J<188 && !FoundShore;J+=4)
            {
                FACEPosition Pose;Pose.CellId=Block|1;Pose.Location=FVector(I,J,0);
                const FVector At=Pose.ToUnrealLocation(100);
                const float Depth=Dat->GetOutdoorWaterDepthCm(At.X,At.Y,100);
                if(Depth>20.f && Depth<80.f)
                    FoundShore=AddWaterArrivals(Block,I,J,TEXT("Rithwic shallow shoreline arrival"),TEXT("Rithwic shallow shoreline drop"));
            }
        }
        TestTrue(TEXT("Independent shallow riverbank fixture exists"),FoundShore);
    }
    for (const auto& Arrival : Arrivals)
    {
        FEntryWorld F;
        auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
        if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
        auto* Client=F.GI->GetSubsystem<UACEClientSubsystem>();auto Session=Client->GetSession();
        F.World->SetGameMode(FURL(nullptr,TEXT("/Game/Test?game=/Script/Engine.GameModeBase"),TRAVEL_Absolute));
        auto* GM=F.World->GetAuthGameMode();
        if(!TestNotNull(TEXT("Portal drop game mode"),GM)) return false;
        auto* Terrain=NewObject<UACETerrainPresenterComponent>(GM);
        GM->AddInstanceComponent(Terrain);Terrain->RegisterComponent();Terrain->Client=Client;
        auto* PC=F.World->SpawnActor<AACEPlayerController>();PC->Client=Client;F.World->AddController(PC);
        auto* Pawn=F.World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
        Capsule->InitCapsuleSize(48,91.75);Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);
        Capsule->RegisterComponent();PC->Possess(Pawn);Pawn->SetActorEnableCollision(false);
        FACEPosition Pose;Pose.CellId=Arrival.Cell;Pose.Location=Arrival.Location;
        Session->SetLocalPosition(Pose);
        PC->ResolveWorldEntryDestinationCell();
        Pose=Session->GetPlayerPosition();
        Terrain->LastKnownCellId=Pose.CellId;Terrain->bHasKnownCell=true;
        Dat->SetWorldStreamingAllowed(true);Dat->SetInPortalSpace(true);
        const bool Indoor=ACECellTransit::IsIndoorCell(Pose.CellId);
        if(Indoor)
        {
            TArray<int32> Ordered;TSet<int32> Needed;Terrain->CollectNeededEnvCells(Ordered,Needed);
            const FVector Origin=FACEPosition::AceVectorToUnreal(FVector((Arrival.Cell>>24)*192,((Arrival.Cell>>16)&255)*192,0),100);
            for(int32 Id:Ordered)
            {
                Dat->GetOrBuildEnvCellMesh(Id,100);
                auto* Room=F.World->SpawnActor<AACEEnvCellActor>();
                if(Room->LoadEnvCell(Id,Origin,100)) Terrain->SpawnedEnvCells.Add(Id,Room);
                else AddError(FString::Printf(TEXT("Cannot load %s cell %08X"),Arrival.Name,Id));
            }
        }
        else
        {
            const uint32 Id=Arrival.Cell&0xFFFF0000u;Dat->GetOrBuildLandblockMesh(Id,100);
            auto* Land=F.World->SpawnActor<AACELandblockActor>();
            TestTrue(TEXT("Outdoor portal landblock loads"),Land->LoadLandblock(Id,100,true,1,0));
            Terrain->Spawned.Add(Id,Land);
        }
        const double Deadline=FPlatformTime::Seconds()+30;
        do
        {
            ++GFrameCounter;FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
            Terrain->UpdateBuildingVisibility();
            for(const auto& Pair:Terrain->SpawnedEnvCells) Pair.Value->Tick(.016f);
            for(const auto& Pair:Terrain->Spawned) Pair.Value->Tick(.016f);
            if(PC->IsWorldEntryGeometryReady()) break;
            FPlatformProcess::Sleep(.002f);
        } while(FPlatformTime::Seconds()<Deadline);
        TestTrue(FString::Printf(TEXT("%s destination collision finishes loading"),Arrival.Name),PC->IsWorldEntryGeometryReady());
        const FVector Feet=Pose.ToUnrealLocation(100);
        FHitResult Floor;FCollisionQueryParams Q(SCENE_QUERY_STAT(PortalDropFloor),false,Pawn);
        const bool HasFloor=F.World->LineTraceSingleByChannel(Floor,Feet,Feet-FVector(0,0,20000),ECC_Pawn,Q);
        AddInfo(FString::Printf(TEXT("%s floor=%d drop=%.2f AC cells=%d"),Arrival.Name,HasFloor,(Feet.Z-Floor.Location.Z)/100,Terrain->SpawnedEnvCells.Num()));
        FVector Placement;AACEPlayerController::EWorldEntryPlacement State;
        uint32 PlacementCell=0;
        TestTrue(FString::Printf(TEXT("%s has a valid placement"),Arrival.Name),PC->FindWorldEntryPlacement(Placement,&State,&PlacementCell));
        if(Indoor)
            TestTrue(TEXT("Resolved arrival belongs to a real resident room"),PC->IsWorldEntryGeometryReady(PlacementCell)
                && ACECellTransit::SphereIntersectsEnvCell(*Dat,PlacementCell,Feet+FVector(0,0,48),0,100));
        TestTrue(TEXT("Arrival contact matches the actual destination"),
            State==(Arrival.Airborne?AACEPlayerController::EWorldEntryPlacement::Airborne:AACEPlayerController::EWorldEntryPlacement::Grounded));
        TestTrue(TEXT("Portal retains the authored height and XY"),Placement.Equals(Feet+FVector(0,0,91.75),Arrival.Airborne?.1:1.));
        PC->EnterWorldLoadElapsed=46;Session->State=EACESessionState::InWorld;
        TestFalse(TEXT("A long portal animation cannot recall a valid drop"),PC->TickWorldEntryRecovery(.016f,TEXT("spawn-placement")));
        TestFalse(TEXT("A valid drop never attempts lifestone recovery"),PC->bWorldEntryRecoveryAttempted);

        if(Arrival.Water && !Arrival.Airborne)
        {
            TestTrue(TEXT("Water arrival really exercises submerged DAT support"),
                Dat->GetOutdoorWaterDepthCm(Feet.X,Feet.Y,100)>20.f);
            auto* Obstacle=F.World->SpawnActor<AActor>();
            auto* Box=NewObject<UBoxComponent>(Obstacle);Obstacle->SetRootComponent(Box);Obstacle->AddInstanceComponent(Box);
            Box->SetBoxExtent(FVector(75,75,300));Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
            Box->SetCollisionResponseToAllChannels(ECR_Block);Box->RegisterComponent();
            Obstacle->SetActorLocation(Feet+FVector(0,0,100));
            FVector NearbyPlacement;
            TestTrue(TEXT("Obstructed water arrival finds nearby clear support"),PC->FindWorldEntryPlacement(NearbyPlacement));
            const double Distance=FVector::Dist2D(NearbyPlacement,Placement);
            TestTrue(TEXT("Nearby landing moves clear but stays inside retail's four-unit search"),Distance>75 && Distance<=400.1);
            TestFalse(TEXT("A nearby landing avoids lifestone recovery"),PC->TickWorldEntryRecovery(.016f,TEXT("spawn-placement")));
            TestFalse(TEXT("Solid scenery still blocks the original water landing"),NearbyPlacement.Equals(Placement,1));
            Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);Obstacle->Destroy();
        }

        // Use the real reveal cut and movement tick with no held keys. It must
        // neither reapply an old grounded anchor nor send a grounded StopMovement.
        PC->PlayerInput=NewObject<UPlayerInput>(PC);PC->bRetailCursorInstalled=true;
        PC->HoverTooltipWidget=CreateWidget<UACEHoverTooltipWidget>(F.GI,UACEHoverTooltipWidget::StaticClass());
        PC->bHaveLastServerPose=true;PC->LastServerPose=Pose;PC->LastServerPose.Location.Z-=20;
        PC->bPortalExitNotified=true;PC->bWorldRevealActive=true;
        PC->FinishWorldTransition();
        TestFalse(TEXT("Drop leaves the loading tunnel"),PC->bEnterWorldLoading||PC->bWorldRevealActive);
        TestEqual(TEXT("Transition initializes the correct falling state"),PC->bJumpAirborne,Arrival.Airborne);
        TestEqual(TEXT("First arrival packet reports correct ground contact"),Session->bAutoPosContact,!Arrival.Airborne);
        TestTrue(TEXT("Arrival keeps autonomous reporting active"),Session->bForcePositionReporting);
        TestTrue(TEXT("Stale server anchor cannot overwrite validated arrival"),PC->PredictedPose.ToUnrealLocation(100).Equals(Placement-FVector(0,0,91.75),.01));
        TestEqual(TEXT("Arrival reports the resolved room to the server"),uint32(Session->GetPlayerPosition().CellId),PlacementCell);
        if(!Arrival.Airborne)
        {
            if(Arrival.Water)
            {
                for(int32 Frame=0;Frame<15;++Frame)PC->PlayerTick(1.f/90.f);
                TestTrue(TEXT("Wading arrival stays at its DAT support after reveal"),FMath::Abs(Pawn->GetActorLocation().Z-91.75-Feet.Z)<2.f);
                TestTrue(TEXT("Wading retains network ground contact"),Session->bAutoPosContact);
                TestFalse(TEXT("Wading does not trigger recovery after reveal"),PC->bWorldEntryRecoveryAttempted);
            }
            continue;
        }
        TestTrue(TEXT("Drop predicts its fall immediately"),PC->bLocalPredicting);
        for(int32 Frame=0;Frame<15;++Frame) PC->PlayerTick(1.f/90.f);
        TestTrue(TEXT("Gravity advances without a movement or Jump input"),Pawn->GetActorLocation().Z<Placement.Z-5);
        TestTrue(TEXT("Portal drop accelerates downward"),PC->JumpWorldAceVelocity.Z<0);
        for(int32 Frame=0;Frame<900 && PC->bJumpAirborne;++Frame) PC->PlayerTick(1.f/90.f);
        TestFalse(TEXT("Portal fall reaches real destination support"),PC->bJumpAirborne);
        TestTrue(TEXT("Landing restores network ground contact"),Session->bAutoPosContact);
        TestFalse(TEXT("Normal falling/landing never recalls"),PC->bWorldEntryRecoveryAttempted);

        if(!Indoor)
        {
            // Short drops used to be silently snapped to the floor by the four-unit
            // probe. Use the proven landing, then arrive two metres above it.
            FACEPosition ShortDrop=PC->PredictedPose;ShortDrop.Location.Z+=2;
            Session->SetLocalPosition(ShortDrop);
            TestTrue(TEXT("Two-metre portal drop is valid"),PC->FindWorldEntryPlacement(Placement,&State));
            TestTrue(TEXT("Two-metre drop remains airborne"),State==AACEPlayerController::EWorldEntryPlacement::Airborne);
            TestTrue(TEXT("Short drop does not skip its fall"),FMath::IsNearlyEqual(Placement.Z,ShortDrop.ToUnrealLocation(100).Z+91.75,.1));
            Terrain->bHasKnownCell=false;
            TestFalse(TEXT("Unloaded collision cannot qualify as open air"),PC->FindWorldEntryPlacement(Placement,&State));
            TestTrue(TEXT("Missing geometry is not a trapped character"),State==AACEPlayerController::EWorldEntryPlacement::Unavailable);
            PC->EnterWorldLoadElapsed=46;
            TestTrue(TEXT("Missing collision has a bounded load failure"),PC->TickWorldEntryRecovery(.016f,TEXT("cell-collision")));
            TestFalse(TEXT("Missing collision never sends a lifestone recall"),PC->bWorldEntryRecoveryAttempted);
        }
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEShoushiGapTest,"ACE.RetailParity.ShoushiGap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEShoushiGapTest::RunTest(const FString&)
{
    FEntryWorld Fixture;
    auto* Dat=Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    auto Spawn=[&](uint32 Guid,uint32 Setup,FVector Location,FQuat Rotation,uint32 Type) {
        FACEWorldObject O;O.Guid=Guid;O.SetupId=Setup;O.ItemType=Type;
        O.bHasPosition=true;O.Position.CellId=0xDA55001D;O.Position.Location=Location;
        O.Position.RotationW=Rotation.W;O.Position.RotationXYZ=FVector(Rotation.X,Rotation.Y,Rotation.Z);
        auto* A=Fixture.World->SpawnActor<AACEWorldEntityActor>();A->InitializeFromObject(O,100,false);
        A->ConfigureWorldCollision(true);return A;
    };
    auto* NPC=Spawn(0x7DA55088,0x02000001,FVector(86.3401,104.57,20.005),FQuat(0,0,-.994055,-.108883),ACEItemType::Creature);
    auto* Stone=Spawn(0x7DA5505A,0x020002EE,FVector(88.7966,103.088,20.005),FQuat(0,0,-.857553,.514396),0);
    const FVector Across=(Stone->GetActorLocation()-NPC->GetActorLocation()).GetSafeNormal2D();
    const FVector Through(-Across.Y,Across.X,0),Center=(Stone->GetActorLocation()+NPC->GetActorLocation())*.5;
    auto* FloorActor=Fixture.World->SpawnActor<AActor>();auto* Floor=NewObject<UBoxComponent>(FloorActor);
    FloorActor->SetRootComponent(Floor);FloorActor->AddInstanceComponent(Floor);Floor->SetBoxExtent(FVector(1200,1200,10));
    Floor->SetWorldLocation(Center-FVector(0,0,10));Floor->SetCollisionResponseToAllChannels(ECR_Block);Floor->RegisterComponent();
    const auto Body=FCollisionShape::MakeCapsule(48,90.75);FCollisionQueryParams Query(SCENE_QUERY_STAT(ShoushiGap),true);
    int32 Contacts=0;
    AddInfo(FString::Printf(TEXT("Gap geometry NPC=%s stone=%s floor=%s"),*NPC->GetActorLocation().ToString(),*Stone->GetActorLocation().ToString(),*Floor->GetComponentLocation().ToString()));
    for(float Offset:{-90.f,-60.f,-30.f,0.f,30.f,60.f,90.f})for(float Sign:{-1.f,1.f})for(float Dt:{1.f/90,1.f/20})
    {
        FVector P=Center+Across*Offset-Through*Sign*300+FVector(0,0,90.75);
        auto Move=[&](const FVector& D) {
            FHitResult Hit;
            if(ACEBodySweep::Sweep(*Fixture.World,Hit,P,P+D,Body,Query)) {
                ++Contacts;P=ACEBodySweep::SlideGrounded(*Fixture.World,P,P+D,Hit,Body,Query);
            } else P+=D;
        };
        for(int32 I=0;I<FMath::CeilToInt(1.2f/Dt);++I) Move(Through*Sign*400*Dt);
        const FVector Blocked=P;
        if(Offset==0 && Sign==1 && Dt<.02f) {
            FHitResult H;ACEBodySweep::Sweep(*Fixture.World,H,P,P-Through*Sign*400*Dt,Body,Query);
            AddInfo(FString::Printf(TEXT("Gap retreat P=%s component=%s normal=%s depth=%.4f initial=%d time=%.4f"),*P.ToString(),*GetNameSafe(H.GetComponent()),*H.Normal.ToString(),H.PenetrationDepth,H.bStartPenetrating,H.Time));
            const FVector Push=H.Normal*(H.PenetrationDepth+.2f);
            AddInfo(FString::Printf(TEXT("Recovery delta %s"),*(ACEBodySweep::Recover(*Fixture.World,P,Push,H,Body,Query)-P).ToString()));
            TArray<FHitResult> Reverse;Fixture.World->SweepMultiByChannel(Reverse,P+Push,P,FQuat::Identity,ECC_Pawn,Body,Query);
            for(const auto& R:Reverse) AddInfo(FString::Printf(TEXT("Reverse comp=%s N=%s impact=%s pen=%.5f initial=%d time=%.4f"),*GetNameSafe(R.GetComponent()),*R.Normal.ToString(),*R.ImpactNormal.ToString(),R.PenetrationDepth,R.bStartPenetrating,R.Time));
        }
        for(int32 I=0;I<FMath::CeilToInt(.8f/Dt);++I) Move(-Through*Sign*400*Dt);
        TestTrue(FString::Printf(TEXT("Eiichi/lifestone retreat offset %.0f sign %.0f dt %.3f"),Offset,Sign,Dt),FVector::DotProduct(P-Blocked,-Through*Sign)>150);
    }
    TestTrue(TEXT("Actual Shoushi setup shapes block the gap probes"),Contacts>0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERadarArtTest,"ACE.RetailParity.RadarArt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACERadarArtTest::RunTest(const FString&)
{
    FEntryWorld Fixture;auto* Dat=Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    auto* Resources=Dat->GetUiResources();
    for(uint32 Image=1;Image<=12;++Image) {
        const uint32 Id=Resources->ResolveTargetIndicatorId(Image);
        TestTrue(TEXT("Retail enum mapper resolves directional art"),(Id>>24)==6);
        auto* Texture=Resources->ResolveIconTexture(Id);
        TestNotNull(TEXT("Original directional arrow decodes"),Texture);
        AddInfo(FString::Printf(TEXT("Target indicator enum %u = %08X (%dx%d)"),Image,Id,Texture?Texture->GetSizeX():0,Texture?Texture->GetSizeY():0));
    }
    for (float Scale : {1.f,1.5f,2.f})
    {
        const FVector2D S(Scale,Scale);
        const FBox2D Frame=ACERadarVisuals::SelectionFrame(FVector2D(100,200)*S,FVector2D(150,260)*S,FVector2D(800,600)*S,S);
        TestTrue(TEXT("Retail arrows sit outside all four object edges"),Frame.Min.Equals(FVector2D(88,188)*S,.01)
            && Frame.Max.Equals(FVector2D(162,272)*S,.01));
        const FBox2D Edge=ACERadarVisuals::SelectionFrame(FVector2D(-50,-50),FVector2D(900,650)*S,FVector2D(800,600)*S,S);
        TestTrue(TEXT("Partially offscreen corners retain retail eight-pixel margin"),Edge.Min.Equals(FVector2D(8,8)*S,.01)
            && Edge.Max.Equals(FVector2D(792,592)*S,.01));
        const FBox2D Tiny=ACERadarVisuals::SelectionFrame(FVector2D(100,100)*S,FVector2D(101,101)*S,FVector2D(800,600)*S,S);
        TestTrue(TEXT("Distant selections retain full-sized outside corners"),Tiny.GetSize().Equals(FVector2D(25,25)*S,.01));
    }
    TestEqual(TEXT("Left arrow matches retail screenshot"),ACERadarVisuals::ArrowEnum(FVector2D(-1,0)),8u);
    TestEqual(TEXT("Right arrow"),ACERadarVisuals::ArrowEnum(FVector2D(1,0)),9u);
    TestFalse(TEXT("Selected square has open corners"),ACERadarVisuals::Pixel(4,true,3,3));
    TestTrue(TEXT("Selected square has side edges"),ACERadarVisuals::Pixel(4,true,3,0));
    auto* Blip=Resources->ResolveRadarBlip(4,false);
    TestTrue(TEXT("Native minimap masks remain sharp single-mip pixels"),Blip && Blip->Filter==TF_Nearest && Blip->GetNumMips()==1);
    auto* Resolver=Dat->GetTextureResolver();
    auto* UI=Resolver->GetOrCreateUiTexture(0x06004C40);
    TestTrue(TEXT("Actual DAT chrome retains original dimensions and a single bilinear mip"),UI && UI->GetNumMips()==1 && UI->Filter==TF_Bilinear && UI->GetSizeX()==12);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEStreamingRetirementTest, "ACE.RetailParity.StreamingRetirement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEStreamingRetirementTest::RunTest(const FString&)
{
    FEntryWorld Fixture;
    auto* Dat=Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    Dat->EnsureLoaded();
    auto* Client=Fixture.GI->GetSubsystem<UACEClientSubsystem>();
    auto Session=Client->GetSession();
    auto* Owner=Fixture.World->SpawnActor<AActor>();
    auto* Terrain=NewObject<UACETerrainPresenterComponent>(Owner);
    Owner->AddInstanceComponent(Terrain);Terrain->RegisterComponent();Terrain->Client=Client;
    Terrain->TerrainChunkSize=1;Terrain->LandblockClass=AACELandblockActor::StaticClass();
    // ACE world portal 10796: Singularity Bore's actual destination.
    FACEPosition Bore;Bore.CellId=0x02910388;Bore.Location=FVector(210,-120,6);
    Session->SetLocalPosition(Bore);
    TestFalse(TEXT("Singularity Bore is an enclosed dungeon"),Terrain->NeedsExteriorTerrain(Bore.CellId));
    TSharedPtr<FACEBuiltLandblockMesh> Departed=MakeShared<FACEBuiltLandblockMesh>();Departed->bHasHeights=true;Departed->bHasTerrain=true;
    TWeakPtr<FACEBuiltLandblockMesh> OldMesh=Departed;
    Dat->LandblockCache.Add(0xDA550000,Departed);Departed.Reset();
    Dat->GetOrBuildEnvCellMesh(0x7F030133,100);
    Terrain->ResetStreamingForTransition();
    Terrain->LastKnownCellId=Bore.CellId;Terrain->bHasKnownCell=true;
    Terrain->SyncAroundCell(Bore.CellId);
    TestFalse(TEXT("Dungeon arrival releases departed terrain even after actor reset"),OldMesh.IsValid());
    Terrain->SyncEnvCells();
    TestNull(TEXT("Arrival releases old dungeon CPU meshes with no old actors left"),Dat->FindEnvCellMesh(0x7F030133,100));
    TArray<int32> Ordered;TSet<int32> Needed;Terrain->CollectNeededEnvCells(Ordered,Needed);
    const double Start=FPlatformTime::Seconds(),Deadline=Start+45;
    bool Ready=false;
    do {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        Ready=true;
        for(int32 Id:Ordered) Ready &= Dat->RequestEnvCellMesh(Id,100)==UACEDatSubsystem::EACEEnvCellMeshStatus::Ready;
        if(!Ready) FPlatformProcess::Sleep(.002f);
    } while(!Ready && FPlatformTime::Seconds()<Deadline);
    TestTrue(TEXT("Singularity Bore's arrival cells finish with bounded workers"),Ready);
    AddInfo(FString::Printf(TEXT("Singularity Bore arrival: %d cells, %.3fs"),Needed.Num(),FPlatformTime::Seconds()-Start));
    Dat->CancelEnvCellMeshRequestsOutside({});Dat->EvictEnvCellMeshesOutside({},100);
    // Model a worker finishing after the player has left; it must not resurrect
    // either a successful result or a permanent failure for a cancelled request.
    constexpr uint32 Land=0xDA550000, Cell=0x7F030133;
    const uint64 Key=Dat->MakeScaleCacheKey(Cell,100);
    Dat->PendingLandblockIds.Add(Land);Dat->CancelLandblockMeshRequestsOutside({});
    auto LateLand=MakeShared<FACEBuiltLandblockMesh>();LateLand->bHasHeights=true;LateLand->bHasTerrain=true;
    Dat->OnLandblockBuildComplete(Land,100,Dat->LandblockBuildGeneration,true,LateLand);
    TestNull(TEXT("Late cancelled terrain is discarded"),Dat->FindLandblockMesh(Land));
    Dat->PendingEnvCellKeys.Add(Key);Dat->CancelEnvCellMeshRequestsOutside({});
    Dat->OnEnvCellBuildComplete(Cell,100,Key,Dat->EnvCellBuildGeneration,true,MakeShared<FACEBuiltEnvCellMesh>());
    TestNull(TEXT("Late cancelled room is discarded"),Dat->FindEnvCellMesh(Cell,100));
    Dat->PendingEnvCellKeys.Add(Key);
    Dat->OnEnvCellBuildComplete(Cell,100,Key,Dat->EnvCellBuildGeneration-1,true,MakeShared<FACEBuiltEnvCellMesh>());
    TestTrue(TEXT("Stale generation cannot remove the new request for the same cell"),Dat->PendingEnvCellKeys.Contains(Key));
    Dat->PendingEnvCellKeys.Remove(Key);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPortalRetirementTest, "ACE.RetailParity.PortalResourceRetirement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEPortalRetirementTest::RunTest(const FString&)
{
    FEntryWorld Fixture;
    auto* Dat=Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    Dat->EnsureLoaded();Dat->SetWorldStreamingAllowed(false);
    auto* Client=Fixture.GI->GetSubsystem<UACEClientSubsystem>();
    Fixture.World->SetGameMode(FURL(nullptr,TEXT("/Game/Test?game=/Script/Engine.GameModeBase"),TRAVEL_Absolute));
    auto* GM=Fixture.World->GetAuthGameMode();
    if(!TestNotNull(TEXT("Retirement runs on the world's real game mode"),GM)) return false;
    auto* Terrain=NewObject<UACETerrainPresenterComponent>(GM);
    GM->AddInstanceComponent(Terrain);Terrain->RegisterComponent();Terrain->Client=Client;
    auto* PC=Fixture.World->SpawnActor<AACEPlayerController>();PC->Client=Client;
    // Actual v61 crash route: Caul -> Obsidian Rim -> town -> Caul -> Rim.
    const uint32 Route[]={0x09040001,0x2E43000C,0x7D640018,0x09040001,0x2E43000C};
    for(int32 Visit=0;Visit<UE_ARRAY_COUNT(Route)-1;++Visit)
    {
        const uint32 LandId=Route[Visit]&0xFFFF0000u;
        Dat->GetOrBuildLandblockMesh(LandId,100);
        auto* Land=Fixture.World->SpawnActor<AACELandblockActor>();
        TestTrue(TEXT("Actual route landblock renders before departure"),Land->LoadLandblock(LandId,100,false,1,0));
        Terrain->Spawned.Add(LandId,Land);
        FlushRenderingCommands();
        TArray<TWeakObjectPtr<UTexture2D>> DepartedTextures;
        for(const auto& Pair:Dat->LandTextureCache) if(Pair.Value.IsValid()) DepartedTextures.Add(Pair.Value);
        TestTrue(TEXT("Departure includes real GPU terrain textures"),!DepartedTextures.IsEmpty());
        TWeakPtr<FACEBuiltLandblockMesh> DepartedMesh=Dat->LandblockCache.FindRef(LandId);
        FACEPosition Destination;Destination.CellId=Route[Visit+1];Destination.Location=FVector(33,80,7.5);
        Client->GetSession()->SetLocalPosition(Destination);
        PC->PortalRetirement=AACEPlayerController::EPortalRetirement::ClearScene;
        TestFalse(TEXT("Scene retirement yields before destination allocations"),PC->RetirePortalScene());
        TestFalse(TEXT("Distant CPU terrain is retired before collection"),DepartedMesh.IsValid());
        const double Deadline=FPlatformTime::Seconds()+15;
        bool Ready=false;
        do {
            if(PC->PortalRetirement==AACEPlayerController::EPortalRetirement::WaitCollection
                && GetLastGCTime()<=PC->PortalRetirementLastGC) CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,false);
            IncrementalPurgeGarbage(true,.002);
            Ready=PC->RetirePortalScene();
            FPlatformProcess::Sleep(.001f);
        } while(!Ready && FPlatformTime::Seconds()<Deadline);
        TestTrue(TEXT("Bounded destruction and render retirement finish"),Ready);
        for(const auto& Texture:DepartedTextures)
            TestFalse(TEXT("Previous area's terrain texture is gone before next allocation"),Texture.IsValid());
        TestFalse(TEXT("Retirement alone cannot enable destination streaming"),Dat->IsWorldStreamingAllowed());
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailLogoutTest, "ACE.RetailParity.LogoutTransition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACERetailLogoutTest::RunTest(const FString&)
{
    FEntryWorld F;
    auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if(!Dat->LoadDatDirectory(DatDirectory))return false;
    Dat->EnsureLoaded();
    auto* Client=F.GI->GetSubsystem<UACEClientSubsystem>();auto Session=Client->GetSession();
    Session->State=EACESessionState::InWorld;Session->PlayerGuid=0x50000100;
    Session->PlayerVitals.CombatMode=int32(ACECombatMode::NonCombat);
    auto* PC=F.World->SpawnActor<AACEPlayerController>();PC->Client=Client;
    PC->SetPlayer(NewObject<ULocalPlayer>(GEngine));F.World->AddController(PC);
    if(!PC->PlayerCameraManager) {
        PC->PlayerCameraManager=F.World->SpawnActor<APlayerCameraManager>();
        PC->PlayerCameraManager->InitializeFor(PC);
    }
    auto* Pawn=F.World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
    Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);Capsule->InitCapsuleSize(22,88);Capsule->RegisterComponent();
    PC->Possess(Pawn);PC->PlayerInput=NewObject<UPlayerInput>(PC);PC->SetupInputComponent();
    PC->SetViewTarget(Pawn);
    // This isolated world has not begun play, so route the real session event
    // natively rather than relying on Unreal's suppressed actor script dispatch.
    const auto StateHandle=Session->OnStateChanged.AddLambda([PC](EACESessionState State){PC->HandleSessionStateChanged(State);});
    ON_SCOPE_EXIT {Session->OnStateChanged.Remove(StateHandle);};
    FACEWorldObject Self;Self.Guid=Session->PlayerGuid;Self.bIsSelf=true;Self.bIsPlayer=true;
    Self.SetupId=0x02000001;Self.MotionTableId=0x09000001;Self.Scale=1;
    Self.Position.CellId=0xA9A10001;Self.bHasPosition=true;Session->WorldObjects.Add(Self.Guid,Self);
    Session->PlayerPosition=Self.Position;
    auto* App=PC->EnsurePawnAppearance();
    if(!TestTrue(TEXT("Real player DAT appearance loads"),App && App->ApplyWorldObject(Self,100,false)))return false;
    const double Deadline=FPlatformTime::Seconds()+20;
    do {
        PC->PreparePortalScreen();FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        if(PC->LoadingScreenActor && PC->LoadingScreenActor->bMeshReady)break;
        FPlatformProcess::Sleep(.002f);
    } while(FPlatformTime::Seconds()<Deadline);
    auto* Tunnel=PC->LoadingScreenActor.Get();
    if(!TestTrue(TEXT("Logout has the actual retail tunnel ready"),Tunnel && Tunnel->bMeshReady))return false;
    PC->ShowGameHUD();
    auto* Canvas=PC->DatCanvasWidget.Get();auto* Binder=PC->DatGameplayBinder.Get();
    const TSharedPtr<SWidget> Hud=Canvas?Canvas->TakeWidget():TSharedPtr<SWidget>();
    if(!TestTrue(TEXT("Gameplay UI is present"),Canvas && Binder && Canvas->IsVisible()))return false;
    auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("Logout receiver"),false);
    auto Address=Sockets->CreateInternetAddr();bool Valid=false;
    Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
    if(!TestTrue(TEXT("Isolated logout peer binds"),Receiver && Receiver->Bind(*Address)))return false;
    ON_SCOPE_EXIT {Receiver->Close();Sockets->DestroySocket(Receiver);};
    Receiver->GetAddress(*Address);
    Session->SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("Logout sender"),false);
    Session->ServerC2SAddr=Address;Session->IssacClient=MakeUnique<FACEIsaac>(123u);

    Binder->HandleOptionsNamedClick(TEXT("GameplayOptionsTab"));
    TestTrue(TEXT("Leave World uses the normal logout path"),Binder->HandleOptionsNamedClick(TEXT("GameplayOptions_LeaveWorld_Button")));
    TestTrue(TEXT("Logout awaits the server"),Session->IsLogOffPending());
    TestTrue(TEXT("Retail non-PK delay is three seconds"),FMath::IsNearlyEqual(Session->GetLogOffPortalStartTime()-FPlatformTime::Seconds(),3.,.1));
    const double StartTime=Session->GetLogOffPortalStartTime();Client->Logout();
    TestEqual(TEXT("Repeated logout does not restart the animation delay"),Session->GetLogOffPortalStartTime(),StartTime);
    TestFalse(TEXT("Combat is blocked immediately on logout"),Client->SendCastSpell(1));
    FACEObjectMotionState Motion;Motion.CurrentStyle=ACEMotion::StanceNonCombat;
    Motion.ActionCommand=0x1000011e;Motion.ActionSpeed=1;Motion.bNewAction=true;
    PC->HandleMotionUpdate(Self.Guid,Motion);
    PC->PlayerTick(.016f);
    TestEqual(TEXT("Server logout motion survives local input polling"),App->ActionCommand,0x1000011eu);
    TestTrue(TEXT("The world remains visible during the logout motion"),PC->GetViewTarget()==Pawn && !Pawn->IsHidden());
    TestFalse(TEXT("The tunnel is not started before the retail delay"),PC->bEnterWorldLoading);
    TestTrue(TEXT("Logout blocks player movement and look"),PC->IsMoveInputIgnored() && PC->IsLookInputIgnored());
    auto* Effects=NewObject<UACEScriptComponent>(Pawn);Pawn->AddInstanceComponent(Effects);Effects->RegisterComponent();
    Effects->InitializeFromObject(Self,100);Effects->NotifyAppearanceReady();
    auto* Impact=Effects->PlayImpactEffect(0x5B,1.f);
    if(!TestNotNull(TEXT("Incoming hit has its own effect"),Impact))return false;
    Impact->TickComponent(.016f,LEVELTICK_All,nullptr);
    TestFalse(TEXT("Incoming hits remain visible during the logout delay"),Impact->GetOwner()->IsActorBeingDestroyed());

    Session->LogOffPortalStartTime=FPlatformTime::Seconds()-.5;PC->PlayerTick(.016f);
    PC->PlayerCameraManager->UpdateCamera(.016f);
    TestTrue(TEXT("World stretches before switching cameras"),PC->bLogOffWorldFadeStarted && PC->GetViewTarget()==Pawn && PC->PlayerCameraManager->GetFOVAngle()>100.f);
    TestTrue(TEXT("HUD remains visible through world fade"),Canvas->IsVisible());
    Session->LogOffPortalStartTime=FPlatformTime::Seconds()-1.01;PC->PlayerTick(.016f);
    TestTrue(TEXT("One-second world fade enters the prepared tunnel"),PC->bEnterWorldLoading && PC->GetViewTarget()==Tunnel && !Tunnel->IsHidden());
    TestTrue(TEXT("Old world avatar is hidden behind the portal"),Pawn->IsHidden());
    Impact->TickComponent(.016f,LEVELTICK_All,nullptr);
    TestTrue(TEXT("Portal view retires the old world's hit effects"),Impact->GetOwner()->IsActorBeingDestroyed());
    const uint32 Actions=Session->NextGameActionSequence;
    for(int32 Frame=0;Frame<120;++Frame) {Tunnel->Tick(1.f/60.f);PC->PlayerTick(1.f/60.f);}
    TestTrue(TEXT("Logout portal animates while waiting for server"),Tunnel->IsActorTickEnabled() && PC->GetViewTarget()==Tunnel);
    TestFalse(TEXT("Logout cannot run arrival, destination streaming or world reveal"),PC->bDestinationStreamingStarted || PC->bPortalExitNotified || PC->bWorldRevealActive);
    TestEqual(TEXT("Waiting cannot send LoginComplete or world-entry actions"),Session->NextGameActionSequence,Actions);
    TestTrue(TEXT("Gameplay UI is retained until server logout completes"),Canvas->IsVisible());
    Session->LogOffPortalStartTime=FPlatformTime::Seconds()-6.;PC->PlayerTick(.016f);
    TestTrue(TEXT("Elapsed animation alone cannot switch to character selection"),Session->State==EACESessionState::InWorld && PC->bEnterWorldLoading);
    FACEBinaryWriter Ack;Ack.WriteUInt32(ACEOpcode::CharacterLogOff);Session->HandleGameMessage(Ack.GetData());
    TestTrue(TEXT("Logout ack preserves tunnel until roster arrives"),Session->IsLogOffPending() && PC->bEnterWorldLoading);
    FACEBinaryWriter Roster;Roster.WriteUInt32(ACEOpcode::CharacterList);Roster.WriteUInt32(0);Roster.WriteUInt32(1);
    Roster.WriteUInt32(Self.Guid);Roster.WriteString16L(TEXT("Logout tester"));Roster.WriteUInt32(0);
    Roster.WriteUInt32(0);Roster.WriteUInt32(11);Roster.WriteString16L(TEXT("fixture"));
    Session->HandleGameMessage(Roster.GetData());
    TestEqual(TEXT("Authoritative roster ends logout"),Session->State,EACESessionState::CharacterSelect);
    TestFalse(TEXT("Roster clears pending logout and presentation"),Session->IsLogOffPending() || PC->IsWorldTransitionActive());
    TestTrue(TEXT("Roster releases the portal and input lock"),!PC->LoadingScreenActor && !PC->IsMoveInputIgnored() && !PC->IsLookInputIgnored());
    TestFalse(TEXT("Roster leaves portal rendering mode"),Dat->IsInPortalSpace());

    // PK and PKLite share retail's additional twenty seconds, sampled at request.
    for(int32 Status:{ACEPlayerKillerStatus::PK,ACEPlayerKillerStatus::PKLite}) {
        Session->State=EACESessionState::InWorld;Session->PlayerGuid=Self.Guid;
        Session->PlayerVitals.PlayerKillerStatus=Status;Client->Logout();PC->PlayerTick(.016f);
        TestTrue(TEXT("PK logout keeps the retail 23-second delay"),FMath::IsNearlyEqual(Session->GetLogOffPortalStartTime()-FPlatformTime::Seconds(),23.,.1));
        TestFalse(TEXT("PK delay does not hide the world early"),PC->bEnterWorldLoading);
        Session->HandleGameMessage(Roster.GetData());
        TestFalse(TEXT("An early authoritative roster still releases all input"),PC->IsWorldTransitionActive() || PC->IsMoveInputIgnored());
    }
    Session->State=EACESessionState::InWorld;Session->PlayerGuid=Self.Guid;
    Session->PlayerVitals.PlayerKillerStatus=0;
    PC->BeginWorldTransition(TEXT("logout-during-portal"));auto* ExistingTunnel=PC->LoadingScreenActor.Get();
    Client->Logout();PC->PlayerTick(.016f);
    TestTrue(TEXT("Logout inside a portal retains the current tunnel"),PC->LoadingScreenActor==ExistingTunnel && !PC->bLogOffWorldFadeStarted);
    Session->Disconnect();
    TestFalse(TEXT("Disconnect cannot strand the logout animation"),PC->IsWorldTransitionActive() || PC->IsMoveInputIgnored() || PC->IsLookInputIgnored());
    PC->DestroyGameHUD();
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELoadingTransitionTest, "ACE.RetailParity.LoadingTransition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACELoadingTransitionTest::RunTest(const FString&)
{
    FEntryWorld Fixture;
    auto* Dat=Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    Dat->EnsureLoaded();
    auto* Client=Fixture.GI->GetSubsystem<UACEClientSubsystem>();auto Session=Client->GetSession();
    auto* PC=Fixture.World->SpawnActor<AACEPlayerController>();PC->Client=Client;
    PC->SetPlayer(NewObject<ULocalPlayer>(GEngine));Fixture.World->AddController(PC);
    auto* Pawn=Fixture.World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
    Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);Capsule->InitCapsuleSize(22,88);Capsule->RegisterComponent();
    PC->Possess(Pawn);PC->PlayerInput=NewObject<UPlayerInput>(PC);PC->SetupInputComponent();
    const double Deadline=FPlatformTime::Seconds()+15;
    do
    {
        PC->PreparePortalScreen();FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        if(PC->LoadingScreenActor && PC->LoadingScreenActor->bMeshReady) break;
        FPlatformProcess::Sleep(.002f);
    } while(FPlatformTime::Seconds()<Deadline);
    auto* Prepared=PC->LoadingScreenActor.Get();
    if(!TestTrue(TEXT("Lobby prepares the actual portal DAT mesh"),Prepared && Prepared->bMeshReady)) return false;
    TestTrue(TEXT("Prepared portal stays invisible in the lobby"),Prepared->IsHidden());
    TestFalse(TEXT("Prepared portal does not animate in the lobby"),Prepared->IsActorTickEnabled());
    Session->State=EACESessionState::EnteringWorld;PC->HandleSessionStateChanged(Session->State);
    TestTrue(TEXT("Play queues its transition outside the Slate callback"),PC->bPendingEnterWorldTransition);
    PC->PlayerTick(.016f);
    TestTrue(TEXT("Portal starts before server world entry"),PC->bEnterWorldLoading);
    TestTrue(TEXT("Play reuses the prepared tunnel"),PC->LoadingScreenActor==Prepared && Prepared->bMeshReady);
    TestFalse(TEXT("Destination work cannot start before its server pose arrives"),PC->bDestinationStreamingStarted);
    TestFalse(TEXT("Waiting for entry holds terrain installs"),Dat->IsWorldStreamingAllowed());
    // Sanctuary recall, captured from the reported indoor/outdoor bridge.
    FACEPosition Pose;Pose.CellId=0xF4180104;Pose.Location=FVector(36.90,48.70,169.805);
    Session->SetLocalPosition(Pose);Session->State=EACESessionState::InWorld;
    ++GFrameCounter;PC->TickWorldTransition();
    TestFalse(TEXT("Destination allocations wait for old scene retirement"),PC->bDestinationStreamingStarted);
    const double RetirementDeadline=FPlatformTime::Seconds()+15;
    while(!PC->bDestinationStreamingStarted && FPlatformTime::Seconds()<RetirementDeadline)
    {
        if(PC->PortalRetirement==AACEPlayerController::EPortalRetirement::WaitCollection
            && GetLastGCTime()<=PC->PortalRetirementLastGC) CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS,false);
        IncrementalPurgeGarbage(true,.002);
        ++GFrameCounter;PC->TickWorldTransition();
        FPlatformProcess::Sleep(.001f);
    }
    TestTrue(TEXT("Destination work starts on a later portal frame"),PC->bDestinationStreamingStarted);
    TestTrue(TEXT("Destination streaming is enabled"),Dat->IsWorldStreamingAllowed());
    const double HudDeadline=FPlatformTime::Seconds()+15;
    while(!PC->bGameplayUiAssetsReady && FPlatformTime::Seconds()<HudDeadline)
    {
        const int32 Before=PC->GameplayUiPrefetchIndex;
        PC->TryPrefetchGameplayHudAssets();
        TestTrue(TEXT("A HUD batch decodes at most eight textures"),PC->GameplayUiPrefetchIndex-Before<=8);
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
    }
    TestTrue(TEXT("Bounded HUD warming eventually finishes"),PC->bGameplayUiAssetsReady);
    PC->TickWorldTransition();
    // Headless fixture has no viewport to own the Slate tree. Retain it explicitly.
    const TSharedPtr<SWidget> Hud=PC->DatCanvasWidget ? PC->DatCanvasWidget->TakeWidget() : TSharedPtr<SWidget>();
    TestTrue(TEXT("Initial entry exposes the gameplay interface before leaving the tunnel"),
        PC->IsWorldTransitionActive() && PC->DatGameplayBinder && PC->DatCanvasWidget && PC->DatCanvasWidget->IsVisible());
    auto* Terrain=NewObject<UACETerrainPresenterComponent>(PC);PC->AddInstanceComponent(Terrain);Terrain->RegisterComponent();
    Terrain->Client=Client;Terrain->TerrainChunkSize=1;Terrain->LoadRadius=0;Terrain->UnloadRadius=0;
    Terrain->LandblockClass=AACELandblockActor::StaticClass();
    TestTrue(TEXT("Sanctuary needs its exterior before crossing the bridge exit"),Terrain->NeedsExteriorTerrain(Pose.CellId));
    TestTrue(TEXT("Ordinary town interiors also retain exterior streaming"),Terrain->NeedsExteriorTerrain(0x7D630112));
    TestFalse(TEXT("A dungeon does not stream an unrelated outdoor landscape"),Terrain->NeedsExteriorTerrain(0x0179010D));
    Dat->GetOrBuildLandblockMesh(Pose.CellId & 0xFFFF0000u,100);
    Terrain->KickLandblockLoginBurst();
    TestTrue(TEXT("Login burst schedules work without spawning terrain inline"),Terrain->Spawned.IsEmpty());
    const double TerrainDeadline=FPlatformTime::Seconds()+15;
    while(Terrain->Spawned.IsEmpty() && FPlatformTime::Seconds()<TerrainDeadline)
    {
        ++GFrameCounter;Terrain->SyncAroundCell(Pose.CellId);
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        FPlatformProcess::Sleep(.002f);
    }
    TestTrue(TEXT("Sanctuary terrain loads while the player remains on the indoor bridge"),Terrain->Spawned.Contains(0xF4180000));
    TestEqual(TEXT("Loading the exterior does not move the player outdoors"),Client->GetPlayerPosition().CellId,Pose.CellId);
    Session->State=EACESessionState::CharacterSelect;PC->HandleSessionStateChanged(Session->State);
    TestFalse(TEXT("Rejected/cancelled world entry releases loading state"),PC->bEnterWorldLoading || PC->bPendingEnterWorldTransition);
    TestFalse(TEXT("Returning to character selection releases movement input"),PC->IsMoveInputIgnored());
    TestFalse(TEXT("Returning to character selection leaves portal space"),Dat->IsInPortalSpace());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPortalInterfaceTest, "ACE.RetailParity.PortalInterface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEPortalInterfaceTest::RunTest(const FString&)
{
    const FString OriginalConfig=GGameUserSettingsIni;
    GGameUserSettingsIni=FPaths::ProjectSavedDir()/TEXT("Automation/PortalInterface.ini");
    FConfigFile EmptyConfig;GConfig->SetFile(GGameUserSettingsIni,&EmptyConfig);ACEInputBindings::Reload();
    ON_SCOPE_EXIT {GGameUserSettingsIni=OriginalConfig;ACEInputBindings::Reload();};
    FEntryWorld F;
    auto* Dat=F.GI->GetSubsystem<UACEDatSubsystem>();
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if(!Dat->LoadDatDirectory(DatDirectory))return false;
    Dat->EnsureLoaded();
    auto* Client=F.GI->GetSubsystem<UACEClientSubsystem>();auto Session=Client->GetSession();
    Session->State=EACESessionState::InWorld;Session->PlayerGuid=0x50000100;
    auto* PC=F.World->SpawnActor<AACEPlayerController>();PC->Client=Client;
    PC->SetPlayer(NewObject<ULocalPlayer>(GEngine));F.World->AddController(PC);
    PC->PlayerInput=NewObject<UPlayerInput>(PC);PC->SetupInputComponent();
    PC->ShowGameHUD();
    auto* Canvas=PC->DatCanvasWidget.Get();auto* Binder=PC->DatGameplayBinder.Get();
    const TSharedPtr<SWidget> Hud=Canvas ? Canvas->TakeWidget() : TSharedPtr<SWidget>();
    if(!TestTrue(TEXT("Real DAT gameplay canvas and binder are present"),Canvas && Binder && Canvas->IsVisible()))return false;
    auto* Manager=Client->GetUIElementManager();
    auto Inventory=Manager->FindElementByName(TEXT("InventoryPanel_Field"));
    if(Inventory && !Inventory->bVisible)Binder->ToggleGameplayPanel(TEXT("InventoryPanel_Field"));
    if(!TestTrue(TEXT("Inventory view is open before entering"),Inventory && Inventory->bVisible))return false;
    Binder->HandleChatMessage(TEXT("Before portal"),TEXT(""),0);
    Binder->ChatEntry->SetText(FText::FromString(TEXT("Unsent chat draft")));
    const int32 Lines=Binder->ChatLog->GetChildrenCount();
    // Use an isolated loopback peer: verify protocol actions, never a live account.
    auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("Portal UI receiver"),false);
    auto Address=Sockets->CreateInternetAddr();bool Valid=false;
    Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
    if(!TestTrue(TEXT("Loopback peer binds"),Receiver && Receiver->Bind(*Address)))return false;
    ON_SCOPE_EXIT {Receiver->Close();Sockets->DestroySocket(Receiver);};
    Receiver->GetAddress(*Address);
    Session->SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("Portal UI sender"),false);
    Session->ServerC2SAddr=Address;Session->IssacClient=MakeUnique<FACEIsaac>(123u);
    Binder->bCombatRepeatActive=Binder->bCombatAttackRequestPending=true;
    PC->BeginWorldTransition(TEXT("portal-ui-test"));
    TestTrue(TEXT("Portal keeps the existing HUD visible"),Canvas->IsVisible());
    TestTrue(TEXT("Portal keeps the existing inventory window"),Inventory->bVisible && PC->DatGameplayBinder==Binder);
    TestEqual(TEXT("Portal retains the chat draft"),Binder->ChatEntry->GetText().ToString(),FString(TEXT("Unsent chat draft")));
    TestEqual(TEXT("Portal retains chat history"),Binder->ChatLog->GetChildrenCount(),Lines);
    TestTrue(TEXT("Portal keeps the desktop cursor visible"),PC->bShowMouseCursor);
    TestNotNull(TEXT("Retail busy cursor resolves from the DAT mapper"),PC->RetailCursorBusyTex.Get());
    TestTrue(TEXT("Portal blocks movement and camera input"),PC->IsMoveInputIgnored() && PC->IsLookInputIgnored());
    TestFalse(TEXT("Held and repeating attacks are cancelled on entry"),Binder->bCombatRepeatActive || Binder->bCombatAttackRequestPending);
    Binder->HandleChatMessage(TEXT("During portal"),TEXT(""),0);
    TestEqual(TEXT("Incoming chat is still displayed"),Binder->ChatLog->GetChildrenCount(),Lines+1);
    const uint32 BeforeChat=Session->NextGameActionSequence;
    Client->SendChatMessage(TEXT("Portal chat test"));
    TestEqual(TEXT("Outgoing chat remains available"),Session->NextGameActionSequence,BeforeChat+1);
    const int32 Mode=Binder->CombatMode;
    const uint32 BeforeCombat=Session->NextGameActionSequence;
    Binder->ApplyCombatModeInternal(int32(ACECombatMode::Magic),true);
    TestEqual(TEXT("Rejected mode change cannot optimistically alter the UI"),Binder->CombatMode,Mode);
    Client->SendTargetedMeleeAttack(0x70000001,ACEAttackHeight::Medium,.5f);
    Client->SendTargetedMissileAttack(0x70000001,ACEAttackHeight::Medium,.5f);
    TestFalse(TEXT("Portal rejects spell casting"),Client->SendCastSpell(1));
    TestEqual(TEXT("Portal combat emits no game actions"),Session->NextGameActionSequence,BeforeCombat);
    Binder->SyncCombatModeFromServer(int32(ACECombatMode::Melee));
    TestEqual(TEXT("Server mode updates remain authoritative"),Binder->CombatMode,int32(ACECombatMode::Melee));
    Binder->BeginCombatPowerCharge(ACEAttackHeight::High,true);Binder->TickCombatAutoAttack(.016f);
    TestFalse(TEXT("Clicking attack in portal cannot queue an attack for arrival"),Binder->bCombatAttackRequestPending || Binder->bCombatRepeatActive);
    PC->BeginUseApproach(0x70000001,.6f,false,true);
    TestFalse(TEXT("World use cannot start an approach in portal space"),PC->IsUseApproachActive());
    Binder->ChatEntry->SetText(FText::GetEmpty());FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::Cleared);
    // Retail's Options binding is F11 (the logical action key is EKeys::O).
    PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::F11,IE_Pressed,1.f));
    // PlayerTick itself processes the input stack; processing twice discards
    // the edge-triggered key before the controller can poll it.
    PC->PlayerTick(.016f);
    auto Options=Manager->FindElementByName(TEXT("OptionsPanel_Field"));
    TestTrue(TEXT("Options keyboard shortcut works during the actual portal tick"),Options && Options->bVisible);
    PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::F11,IE_Released,0.f));
    PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Escape,IE_Pressed,1.f));PC->PlayerTick(.016f);
    TestTrue(TEXT("Escape closes options while the portal view is active"),Options && !Options->bVisible);
    PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Escape,IE_Released,0.f));
    PC->PlayerInput->ProcessInputStack({},.016f,false);
    PC->SetDesktopInterfaceHidden(true);TestFalse(TEXT("Manual interface hide still works in portal space"),Canvas->IsVisible());
    PC->SetDesktopInterfaceHidden(false);TestTrue(TEXT("Manual interface show still works in portal space"),Canvas->IsVisible());
    PC->SetDesktopInterfaceHidden(true);PC->BeginWorldTransition(TEXT("hidden-ui-recall"));
    TestFalse(TEXT("Another transition honors the user's hidden HUD preference"),Canvas->IsVisible());
    // Destination placement/reveal is exercised by PortalDrops. Verify the UI
    // action gate releases with the controller's transition lifecycle flags.
    PC->bEnterWorldLoading=PC->bWorldRevealActive=false;PC->ShowGameHUD();
    TestFalse(TEXT("Arrival does not override a manually hidden interface"),Canvas->IsVisible());
    PC->SetDesktopInterfaceHidden(false);
    const uint32 BeforeResume=Session->NextGameActionSequence;
    Binder->ApplyCombatModeInternal(int32(ACECombatMode::Magic),true);
    TestEqual(TEXT("Combat mode requests resume outside portal space"),Session->NextGameActionSequence,BeforeResume+1);
    TestEqual(TEXT("Resumed combat updates the UI"),Binder->CombatMode,int32(ACECombatMode::Magic));
    PC->DestroyGameHUD();
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACETerrainPortalRevealTest, "ACE.RetailParity.TerrainPortalReveal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACETerrainPortalRevealTest::RunTest(const FString&)
{
    FEntryWorld Fixture;
    auto* Dat=Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    Dat->EnsureLoaded();
    auto* Owner=Fixture.World->SpawnActor<AActor>();
    auto* Terrain=NewObject<UACETerrainPresenterComponent>(Owner);
    auto* Land=Fixture.World->SpawnActor<AACELandblockActor>();
    auto* Chunk=Fixture.World->SpawnActor<AACETerrainChunkActor>();
    // Both reported characters arrived outdoors on this Caulcano tower. All
    // landscape actors finish loading inside the tunnel, before the reveal.
    Dat->GetOrBuildLandblockMesh(0x09090000,100);
    TestTrue(TEXT("Caulcano destination terrain loads"),Land->LoadLandblock(0x09090000,100,false,1,0));
    TestTrue(TEXT("Caulcano terrain has drawable sections"),Land->IsOutdoorTerrainMeshReady());
    Terrain->bHasKnownCell=true;
    Terrain->LastKnownCellId=0x0909000C;
    Terrain->Spawned.Add(0x09090000,Land);
    Terrain->SpawnedChunks.Add(0x08090000,Chunk);
    for(int32 Visit=0;Visit<2;++Visit)
    {
        Dat->SetInPortalSpace(true);
        Terrain->UpdateBuildingVisibility();
        TestTrue(TEXT("Portal hides complete landblock actors, including scenery"),Land->IsHidden());
        TestTrue(TEXT("Portal hides chunk actors"),Chunk->IsHidden());
        Terrain->UpdateCameraVisibility();
        TestTrue(TEXT("Camera updates cannot reveal terrain during portal space"),Land->IsHidden());
        Dat->SetInPortalSpace(false);
        Terrain->KickLandblockLoginBurst();
        // No new cell, terrain actor, movement event or room entry occurs.
        // Exercise the normal per-frame path used by a stationary login.
        Terrain->UpdateCameraVisibility();
        TestFalse(TEXT("Stationary portal exit restores landblock and scenery visibility"),Land->IsHidden());
        TestFalse(TEXT("Stationary portal exit restores chunk visibility"),Chunk->IsHidden());
        TestFalse(TEXT("Outdoor arrival reveals terrain components too"),Land->TerrainMesh->bHiddenInGame);
        Terrain->UpdateCameraVisibility();
        TestFalse(TEXT("The next camera update retains the revealed world"),Land->IsHidden());
    }
    // Revealing the actor must not bypass the separate baked-terrain policy.
    Terrain->bWcBakedTerrainMode=true;
    Dat->SetInPortalSpace(true);
    Terrain->UpdateBuildingVisibility();
    Dat->SetInPortalSpace(false);
    Terrain->UpdateCameraVisibility();
    TestFalse(TEXT("Baked worlds still reveal their building actors"),Land->IsHidden());
    TestTrue(TEXT("Baked worlds keep duplicate procedural terrain hidden"),Land->TerrainMesh->bHiddenInGame);
    return !HasAnyErrors();
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FACETerrainArrivalTest, "ACE.RetailParity.TerrainArrival",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FACETerrainArrivalTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    Names.Add(TEXT("Desktop")); Commands.Add(TEXT("Desktop"));
    Names.Add(TEXT("ShoushiQuestBudget")); Commands.Add(TEXT("Shoushi"));
}
bool FACETerrainArrivalTest::RunTest(const FString& Parameters)
{
    FEntryWorld Fixture;
    auto* Dat=Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    const double Start=FPlatformTime::Seconds();
    if (!Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    Dat->EnsureLoaded();
    // A unique test fingerprint exercises cold disk misses without removing the
    // user's cache. No maintenance is run with this synthetic fingerprint.
    const uint64 Fingerprint=(uint64(GetTypeHash(FGuid::NewGuid()))<<32)|0x54455354;
    Dat->CachedDatFingerprint=Fingerprint;
    const bool Shoushi=Parameters==TEXT("Shoushi");
    const int32 Radius=Shoushi?3:5, CenterX=Shoushi?0xDA:0x7E, CenterY=Shoushi?0x55:0x65;
    TArray<uint32> Blocks;
    for(int32 X=-Radius;X<=Radius;++X) for(int32 Y=-Radius;Y<=Radius;++Y)
        Blocks.Add((uint32(CenterX + X)<<24)|(uint32(CenterY + Y)<<16));
    Blocks.Sort([CenterX,CenterY](uint32 A,uint32 B)
    {
        auto Distance=[CenterX,CenterY](uint32 V){return FMath::Square(int32(V>>24)-CenterX)+FMath::Square(int32((V>>16)&255)-CenterY);};
        return Distance(A)<Distance(B);
    });
    TArray<AACELandblockActor*> Lands;
    TSet<uint32> Pending;
    for(uint32 Id:Blocks) { Dat->RequestLandblockMesh(Id,100); Pending.Add(Id); }
    const double Deadline=Start+90;
    while(!Pending.IsEmpty() && FPlatformTime::Seconds()<Deadline)
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        for(uint32 Id:Blocks)
        {
            if(!Pending.Contains(Id) || Dat->RequestLandblockMesh(Id,100)!=UACEDatSubsystem::EACELandMeshStatus::Ready) continue;
            auto* Land=Fixture.World->SpawnActor<AACELandblockActor>();
            TestTrue(TEXT("Cold arrival terrain applies to runtime mesh"),Land->LoadLandblock(Id,100,false,1,0));
            Lands.Add(Land); Pending.Remove(Id);
        }
        FPlatformProcess::Sleep(.002f);
    }
    FlushRenderingCommands();
    TestEqual(TEXT("All visible blocks arrive without holes"),Lands.Num(),Blocks.Num());
    for(auto* Land:Lands) TestTrue(TEXT("Every arrival block has renderable terrain"),Land->IsOutdoorTerrainMeshReady());
    TSet<const FACETerrainBlend*> Unique;
    uint64 Bytes=0; int32 Sections=0;
    for(const auto& Pair:Dat->LandblockCache) for(const auto& Sec:Pair.Value->Sections)
    {
        ++Sections;
        if(Sec.SharedBake && !Unique.Contains(Sec.SharedBake.Get()))
        { Unique.Add(Sec.SharedBake.Get()); Bytes+=Sec.SharedBake->GetAllocatedSize(); }
    }
    AddInfo(FString::Printf(TEXT("Cold terrain arrival including DAT indexing, disk writes, runtime mesh application and render flush: %.3fs, %d blocks, %d sections, %d shared blends, %.1f MiB blend storage"),
        FPlatformTime::Seconds()-Start,Lands.Num(),Sections,Unique.Num(),Bytes/(1024.0*1024.0)));
    TestTrue(TEXT("Neighbor sections share their blend storage"),Unique.Num()<Sections/2);
    uint64 CpuMipBytes=0;
    for(const auto& Pair:Dat->LandTextureCache) if(auto* Texture=Pair.Value.Get())
        for(const auto& Mip:Texture->GetPlatformData()->Mips) CpuMipBytes+=Mip.BulkData.GetBulkDataSize();
    TestEqual(TEXT("Loaded terrain retains no duplicate CPU mip chains"),CpuMipBytes,uint64(0));
    Dat->ClearLoadedState(); // joins any outstanding work before removing test-owned files
    for(const FString& Dir:{ACEDiskTileCache::GetLandblocksDir(),ACEDiskTileCache::GetPCodesDir()})
    {
        TArray<FString> Files;
        const FString Suffix=FString::Printf(TEXT("_%016llX"),Fingerprint);
        IFileManager::Get().FindFiles(Files,*(Dir/TEXT("*")),true,false);
        for(const FString& Name:Files)
            if(FPaths::GetBaseFilename(Name).EndsWith(Suffix)) IFileManager::Get().Delete(*(Dir/Name),false,true);
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailWorldEntryTest, "ACE.RetailParity.WorldEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACERetailWorldEntryTest::RunTest(const FString& Parameters)
{
    FEntryWorld Fixture;
    auto* Dat = Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if (!Dat || !Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    auto* Client = Fixture.GI->GetSubsystem<UACEClientSubsystem>();
    auto Session = Client->GetSession();
    auto* Controller = Fixture.World->SpawnActor<AACEPlayerController>();
    Controller->Client = Client;
    auto* Pawn = Fixture.World->SpawnActor<APawn>();
    auto* Capsule = NewObject<UCapsuleComponent>(Pawn);
    Capsule->InitCapsuleSize(22, 88); Pawn->SetRootComponent(Capsule);
    Pawn->AddInstanceComponent(Capsule); Capsule->RegisterComponent(); Controller->Possess(Pawn);
    Pawn->SetActorEnableCollision(false);
    auto* Presenter = NewObject<UACETerrainPresenterComponent>(Controller);
    Controller->AddInstanceComponent(Presenter); Presenter->RegisterComponent();
    Presenter->Client = Client;

    // Exact failing destination from the preserved user log. No manual collision
    // activation: portal-space visibility must prepare the still-hidden destination.
    FACEPosition Pose; Pose.CellId = 0x7D630112; Pose.Location = FVector(36.71, 89.43, 9.20);
    Session->SetLocalPosition(Pose);
	Presenter->LastKnownCellId = Pose.CellId;
	Presenter->bHasKnownCell = true;
    auto* Room = Fixture.World->SpawnActor<AACEEnvCellActor>();
    const FVector Origin = FACEPosition::AceVectorToUnreal(FVector(125 * 192, 99 * 192, 0), 100);
    Dat->GetOrCreateTexturedMaterial(0x08000708);
    TestNotNull(TEXT("Destination mesh is decoded before actor creation"), Dat->GetOrBuildEnvCellMesh(Pose.CellId, 100));
    TestTrue(TEXT("Reported stuck-login room loads from DAT"), Room->LoadEnvCell(Pose.CellId, Origin, 100));
    Presenter->SpawnedEnvCells.Add(Pose.CellId, Room);
    TestFalse(TEXT("Loaded room starts without cooked collision"), Presenter->IsPlayerCellCollisionReady());
    Dat->SetInPortalSpace(true);
    Presenter->UpdateBuildingVisibility();
    TestTrue(TEXT("Hidden destination can finish the collision readiness gate"), Presenter->IsPlayerCellCollisionReady());
    TestTrue(TEXT("Preparing physics does not reveal the destination"), Room->IsHidden());
    FVector Placement;
    TestTrue(TEXT("Actual logged-in position has a supported, unobstructed placement"), Controller->FindWorldEntryPlacement(Placement));
    AddInfo(FString::Printf(TEXT("User destination placement: %s"), *Placement.ToString()));
    for (int32 I = 0; I < 20; ++I) Presenter->UpdateBuildingVisibility();
    TestTrue(TEXT("Repeated hidden updates retain cooked room collision"), Presenter->IsPlayerCellCollisionReady());
    Presenter->SpawnedEnvCells.Reset(); Room->Destroy();

    // The failed dungeon visit entered a cell with no room triangles. Its DAT
    // stabs and adjacent cells must be usable before the portal tunnel releases.
    Pose.CellId = 0x0179010D; Pose.Location = FVector(100, -100, 0);
    Session->SetLocalPosition(Pose); Presenter->LastKnownCellId = Pose.CellId;
    Fixture.World->AddController(Controller);
    Dat->SetWorldStreamingAllowed(true);
    const FVector DungeonOrigin = FACEPosition::AceVectorToUnreal(FVector(192, 121 * 192, 0), 100);
    for (uint32 Id : {0x0179010Du, 0x01790100u, 0x01790114u, 0x0179010Cu,
                     0x01790128u, 0x0179010Eu, 0x01790106u})
    {
        TestNotNull(TEXT("Dungeon cell DAT decodes"), Dat->GetOrBuildEnvCellMesh(Id, 100));
        auto* DungeonRoom = Fixture.World->SpawnActor<AACEEnvCellActor>();
        TestTrue(TEXT("Dungeon cell actor loads"), DungeonRoom->LoadEnvCell(Id, DungeonOrigin, 100));
        Presenter->SpawnedEnvCells.Add(Id, DungeonRoom);
    }
    TestTrue(TEXT("Deferred dungeon floor counts as unfinished before queueing"),
        Presenter->SpawnedEnvCells.FindRef(Pose.CellId)->HasPendingStaticObjects());
    Presenter->UpdateBuildingVisibility();
    const double DungeonDeadline = FPlatformTime::Seconds() + 10;
    do
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        for (const auto& Pair : Presenter->SpawnedEnvCells) Pair.Value->Tick(.016f);
        if (Controller->FindWorldEntryPlacement(Placement)
            && !Presenter->SpawnedEnvCells.FindRef(Pose.CellId)->HasPendingStaticObjects()) break;
        FPlatformProcess::Sleep(.002f);
    } while (FPlatformTime::Seconds() < DungeonDeadline);
    TestTrue(TEXT("Empty dungeon transit cell gets support while portal space stays hidden"),
        Controller->FindWorldEntryPlacement(Placement));
    TestFalse(TEXT("Destination static geometry finishes before reveal"),
        Presenter->SpawnedEnvCells.FindRef(Pose.CellId)->HasPendingStaticObjects());
    TestTrue(TEXT("DAT floor supports arrival at the cell origin"),
        Placement.Equals(Pose.ToUnrealLocation(100) + FVector(0,0,88.5), .1));
    AddInfo(FString::Printf(TEXT("Dungeon placement: %s"), *Placement.ToString()));
    for (const auto& Pair : Presenter->SpawnedEnvCells)
    {
        TestTrue(TEXT("Dungeon geometry stays hidden during placement"), Pair.Value->IsHidden());
        TestTrue(TEXT("Direct transit neighbor collision is active before portal exit"), Pair.Value->GetActorEnableCollision());
        TestTrue(TEXT("Direct transit neighbor collision is cooked before portal exit"), Pair.Value->IsCollisionCooked());
        Pair.Value->Destroy();
    }
    Presenter->SpawnedEnvCells.Reset();

    // Exact rejected Town Network -> Khayyaban arrival from the user log.
    Pose.CellId = 0x9F44001A; Pose.Location = FVector(90, 24.553, 31.890);
    Session->SetLocalPosition(Pose);
    Dat->GetOrBuildLandblockMesh(0x9F440000,100);
    auto* Khayyaban = Fixture.World->SpawnActor<AACELandblockActor>();
    TestTrue(TEXT("Khayyaban terrain loads from DAT"), Khayyaban->LoadLandblock(0x9F440000,100,true,1,0));
    const double TerrainDeadline = FPlatformTime::Seconds() + 5;
    FHitResult TerrainHit;
    const FVector Arrival = Pose.ToUnrealLocation(100);
    bool bTerrainHit=false;
    do
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        bTerrainHit=Fixture.World->LineTraceSingleByChannel(TerrainHit,
            Arrival + FVector(0,0,2000), Arrival - FVector(0,0,2000), ECC_Pawn);
        if (bTerrainHit) break;
        FPlatformProcess::Sleep(.002f);
    } while (FPlatformTime::Seconds() < TerrainDeadline);
    TestTrue(TEXT("Khayyaban has cooked terrain support"),bTerrainHit);
    AddInfo(FString::Printf(TEXT("Khayyaban floor=%d arrival=%s hit=%s normal=%s"), bTerrainHit,
        *Arrival.ToString(), *TerrainHit.Location.ToString(), *TerrainHit.ImpactNormal.ToString()));
    TestTrue(TEXT("Logged Khayyaban portal position has a supported placement"), Controller->FindWorldEntryPlacement(Placement));
    TestTrue(TEXT("Khayyaban correction preserves the server XY"),FVector::Dist2D(Placement,Arrival)<1.f);
    TestTrue(TEXT("Khayyaban correction places the player above the surface"),Placement.Z>=TerrainHit.ImpactPoint.Z);
    AddInfo(FString::Printf(TEXT("Khayyaban placement: %s"), *Placement.ToString()));
    auto* ArrivalMob=Fixture.World->SpawnActor<AActor>();
    auto* ArrivalBody=NewObject<USphereComponent>(ArrivalMob);ArrivalMob->SetRootComponent(ArrivalBody);
    ArrivalMob->AddInstanceComponent(ArrivalBody);ArrivalBody->InitSphereRadius(99.6f);
    ArrivalBody->ComponentTags.Add(TEXT("ACECreatureBody"));ArrivalBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    ArrivalBody->SetCollisionResponseToAllChannels(ECR_Block);ArrivalBody->RegisterComponent();
    ArrivalMob->SetActorLocation(TerrainHit.Location+FVector(0,0,99.6));
    FVector CrowdedPlacement;
    TestTrue(TEXT("Crowded outdoor arrival still corrects old portal terrain height"),Controller->FindWorldEntryPlacement(CrowdedPlacement));
    TestTrue(TEXT("Creature filtering uses corrected outdoor height, not the buried server point"),CrowdedPlacement.Equals(Placement,.01));
    ArrivalMob->Destroy();
    Presenter->LastKnownCellId=Pose.CellId; Presenter->TerrainChunkSize=1;
    Presenter->Spawned.Add(0x9F440000,Khayyaban);
    TestTrue(TEXT("Arrival terrain alone satisfies center readiness"),Presenter->IsLoadRadiusTerrainReadyForRadius(0));
    TestFalse(TEXT("A safe center cannot conceal missing visible neighbors"),Presenter->IsLoadRadiusTerrainReadyForRadius(1));
    for (int32 DX=-1;DX<=1;++DX) for(int32 DY=-1;DY<=1;++DY)
    {
        if(!DX && !DY) continue;
        const int32 Id=((0x9F+DX)<<24)|((0x44+DY)<<16);
        Dat->GetOrBuildLandblockMesh(Id,100);
        auto* Land=Fixture.World->SpawnActor<AACELandblockActor>();
        TestTrue(TEXT("Visible neighboring landblock decodes"),Land->LoadLandblock(Id,100,false,1,0));
        Presenter->Spawned.Add(Id,Land);
    }
    const double CoverageDeadline=FPlatformTime::Seconds()+5;
    while(!Presenter->IsLoadRadiusTerrainReadyForRadius(1) && FPlatformTime::Seconds()<CoverageDeadline)
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        for(const auto& Pair:Presenter->Spawned) Pair.Value->Tick(.016f);
        FPlatformProcess::Sleep(.002f);
    }
    TestTrue(TEXT("Complete visible terrain can release the loading screen"),Presenter->IsLoadRadiusTerrainReadyForRadius(1));
    for(const auto& Pair:Presenter->Spawned) if(Pair.Value!=Khayyaban) Pair.Value->Destroy();
    Presenter->Spawned.Reset();
    Khayyaban->Destroy();

    Pose.CellId = 0x01010001; Pose.Location = FVector(40, 40, 0); Session->SetLocalPosition(Pose);
    const FVector Feet = Pose.ToUnrealLocation(100);
    auto MakeBox = [&](FVector Center, FVector Extent)
    {
        auto* Owner = Fixture.World->SpawnActor<AActor>();
        auto* Box = NewObject<UBoxComponent>(Owner); Owner->SetRootComponent(Box);
        Owner->AddInstanceComponent(Box); Box->SetBoxExtent(Extent);
        Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Box->SetCollisionResponseToAllChannels(ECR_Block); Box->RegisterComponent();
        Owner->SetActorLocation(Center); return Owner;
    };
    TestFalse(TEXT("Empty space is not a safe spawn"), Controller->FindWorldEntryPlacement(Placement));
    auto* Floor = MakeBox(Feet - FVector(0,0,10), FVector(1000,1000,10));
    TestTrue(TEXT("Supported clear spawn succeeds"), Controller->FindWorldEntryPlacement(Placement));
    // Full foot sphere support must succeed on slopes, not only on flat ground.
    for (float Pitch : {15.f,30.f,45.f})
    {
        Floor->SetActorRotation(FRotator(Pitch,0,0));
        TestTrue(TEXT("Admin arrival on a walkable slope has a clear capsule"),Controller->FindWorldEntryPlacement(Placement));
        TestTrue(TEXT("Slope placement stays at the server XY"),FVector::Dist2D(Placement,Feet)<1.f);
    }
    Floor->SetActorRotation(FRotator::ZeroRotator);
    auto* App=NewObject<UACECharacterAppearanceComponent>(Pawn);
    Pawn->AddInstanceComponent(App); App->RegisterComponent(); App->bAlignMeshToCapsuleBottom=true;
    // Manual controller stepping must also advance action/style links, as the
    // world's component tick does in-game; an unticked link locks movement.
    auto TickPlayer=[&](float Dt)
    {
        Controller->PlayerTick(Dt);
        App->TickComponent(Dt,LEVELTICK_All,nullptr);
    };
    Pawn->SetActorScale3D(FVector(.7)); App->EnsureMeshRoot();
    TestTrue(TEXT("Scaled player visual foot does not apply actor scale twice"),
        App->MeshRoot->GetComponentLocation().Equals(Capsule->GetComponentLocation()-FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()),.01));
    Pawn->SetActorScale3D(FVector(1));
    // Exercise the actual appearance/capsule update order. Re-equipping changes
    // Object.Scale and must neither compound capsule size nor move visual feet.
    const int32 PreviousGuid = Session->PlayerGuid;
    FACEWorldObject SelfObject; SelfObject.Guid = 12345; SelfObject.SetupId = 0x02000001;
    SelfObject.MotionTableId = 0x09000001; // real stance links must advance during movement tests
    SelfObject.ItemType = 0x10; SelfObject.Name = TEXT("Scaled local player");
    Session->PlayerGuid = SelfObject.Guid;
    Pawn->SetActorLocation(Feet + FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()));
    for (float ObjectScale : {.7f, 1.1f, .9f, .9f, .7f})
    {
        SelfObject.Scale = ObjectScale; Session->WorldObjects.Add(SelfObject.Guid, SelfObject);
        Controller->ApplyPlayerCapsuleFromSetup(SelfObject.SetupId);
        TestTrue(TEXT("Local scaled appearance builds"), App->ApplyWorldObject(SelfObject,100,false));
        Controller->ApplyPlayerCapsuleFromSetup(SelfObject.SetupId);
        TestTrue(TEXT("Appearance updates keep local visual feet on the same surface"),
            App->MeshRoot->GetComponentLocation().Equals(Feet,.01));
        const float ExpectedHalf = Controller->PlayerSetupHeightAc * ObjectScale * 50.f;
        TestTrue(TEXT("World capsule height applies object scale exactly once"),
            FMath::IsNearlyEqual(Capsule->GetScaledCapsuleHalfHeight(),ExpectedHalf,.01f));
    }
    // Exercise the actual standing movement/ground-snap path with the reported
    // outfit, rather than checking the mesh root alone.
    FACECharacterCreation Clothing; FString ClothingError;
    TestTrue(TEXT("Grounding clothing data loads"),Clothing.Load(*Dat->GetPortalDat(),ClothingError));
    SelfObject.Scale=1; SelfObject.bIsPlayer=true;
    Clothing.ApplyClothing(0x1000069E,SelfObject.SetupId,0,0,SelfObject.Appearance);
    Clothing.ApplyClothing(0x100007E2,SelfObject.SetupId,0,0,SelfObject.Appearance);
    Clothing.ApplyClothing(0x10000867,SelfObject.SetupId,0,0,SelfObject.Appearance);
    Session->WorldObjects.Add(SelfObject.Guid,SelfObject);
    App->ApplyWorldObject(SelfObject,100,false); Controller->ApplyPlayerCapsuleFromSetup(SelfObject.SetupId);
    Session->State=EACESessionState::InWorld;
    Controller->PlayerInput=NewObject<UPlayerInput>(Controller);
    Controller->bRetailCursorInstalled=true; // Headless movement fixture has no Slate viewport.
    Controller->HoverTooltipWidget=CreateWidget<UACEHoverTooltipWidget>(Fixture.GI,UACEHoverTooltipWidget::StaticClass());
    Controller->bEnterWorldLoading=false; Controller->bWorldRevealActive=false;
    Controller->bHavePredictedPose=true; Controller->PredictedPose=Pose;
    Controller->PredictedPose.Location.Z+=.2f;
    Pawn->SetActorLocation(Feet+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()+20));
    for (int32 Frame=0;Frame<30;++Frame)
    {
        TickPlayer(.016f);
    }
    FBox GroundedBounds(ForceInit); App->GetVisualWorldBounds(GroundedBounds);
    AddInfo(FString::Printf(TEXT("Grounded outfit floor=%.3f capsuleFeet=%.3f visualMin=%.3f"),Feet.Z,
        Pawn->GetActorLocation().Z-Capsule->GetScaledCapsuleHalfHeight(),GroundedBounds.Min.Z));
    // Contact recovery adds 0.5 cm plus the sweep's 0.1 cm separation. Include
    // that clearance, while still rejecting the original 20 cm hovering gap.
    TestTrue(TEXT("Stationary player settles to floor contact clearance"),
        FMath::Abs(App->GetMeshRoot()->GetComponentLocation().Z-Feet.Z)<=.65f);
    // Exercise actual key polling and predicted yaw, including forward movement
    {
        const FACEPosition SavedPose=Controller->PredictedPose;
        const FTransform SavedTransform=Pawn->GetActorTransform();
        FACEPosition FallPose; FallPose.CellId=0x7D630001; FallPose.Location=FVector(100,100,230);
        const FVector FallFeet=FallPose.ToUnrealLocation(100);
        auto* Landing=Fixture.World->SpawnActor<AActor>();
        auto* LandingFloor=NewObject<UBoxComponent>(Landing); Landing->SetRootComponent(LandingFloor);
        Landing->AddInstanceComponent(LandingFloor); LandingFloor->SetBoxExtent(FVector(1000,1000,10));
        LandingFloor->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        LandingFloor->SetCollisionResponseToAllChannels(ECR_Block); LandingFloor->RegisterComponent();
        Landing->SetActorLocation(FallFeet-FVector(0,0,3010));
        Controller->LastServerPose=FallPose; Controller->bHaveLastServerPose=true; Controller->bHavePredictedPose=false;
        Controller->StepHoldSeconds=0; Session->SetLocalPosition(FallPose);
        Pawn->SetActorLocation(FallFeet+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()));
        TickPlayer(.016f);
        TestTrue(TEXT("Unsupported idle player enters falling without a Jump action"),Controller->bJumpAirborne);
        for (int32 Frame=0;Frame<40;++Frame) TickPlayer(.016f);
        TestTrue(TEXT("Fall continues without keyboard input"),Pawn->GetActorLocation().Z<FallFeet.Z-100);
        for (int32 Frame=0;Frame<160;++Frame) TickPlayer(.016f);
        TestFalse(TEXT("Stationary fall lands on the lower floor"),Controller->bJumpAirborne);
        TestFalse(TEXT("Landing expires the stale server pose above the cliff"),Controller->bHaveLastServerPose);
        TestTrue(TEXT("Client reaches the bottom of a thirty metre drop"),FMath::Abs(Pawn->GetActorLocation().Z-Capsule->GetScaledCapsuleHalfHeight()-(FallFeet.Z-3000))<3);
        FACEPosition Bottom=Controller->PredictedPose;
        Controller->bLocalPredicting=false; Controller->bHavePredictedPose=true;
        Controller->PredictedPose.Location.Z+=30;
        Controller->SoftReconcilePredictedTowardServer(Bottom,.016f,false);
        TestEqual(TEXT("Idle reconciliation accepts a vertical-only server correction"),Controller->PredictedPose.Location.Z,Bottom.Location.Z);
        Controller->bJumpAirborne=true; Controller->bLocalPredicting=true;
        Controller->bStandingJumpLocked=true; Controller->bHaveJumpLaunchFacing=true;
        ++Session->ForcePositionSeq;
        Controller->HandlePositionUpdate(Client->GetPlayerGuid(),Bottom);
        TestTrue(TEXT("Forced server position is applied while locally predicting"),Pawn->GetActorLocation().Equals(Bottom.ToUnrealLocation(100)+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()),.01));
        TestFalse(TEXT("Forced correction releases the old ballistic arc"),Controller->bJumpAirborne);
        TestFalse(TEXT("Forced landing also releases standing-jump movement lock"),Controller->bStandingJumpLocked);
        TestFalse(TEXT("Forced landing drops stale launch facing"),Controller->bHaveJumpLaunchFacing);
        Landing->Destroy();
        Controller->PredictedPose=SavedPose; Controller->bHavePredictedPose=true; Controller->bHaveLastServerPose=false;
        Controller->bLocalPredicting=false; Controller->StepHoldSeconds=0;
        Session->SetLocalPosition(SavedPose); Pawn->SetActorTransform(SavedTransform);
    }

    // Exercise actual key polling and predicted yaw, including forward movement
    // with an orbit offset (camera follow must not override an explicit turn).
    {
        TGuardValue<FString> InputPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/TurnKeysFixture.ini"));
        FConfigFile InputConfig; InputConfig.NoSave=true;
        GConfig->SetFile(GGameUserSettingsIni,&InputConfig); ACEInputBindings::Reload();
        const bool MouseTurningBefore=Client->IsCharacterOptionSet(0x31);
        const FACEPosition StartPose=Controller->PredictedPose;
        const FTransform StartTransform=Pawn->GetActorTransform();
        auto* TestBoom=NewObject<USpringArmComponent>(Pawn);
        TestBoom->SetupAttachment(Pawn->GetRootComponent());TestBoom->RegisterComponent();
        auto PressKey=[&](FKey Key)
        {
            Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Pressed,1.f));
        };
        for (bool MouseTurning : {false,true}) for (bool Forward : {false,true}) for (bool Run : {false,true})
        {
            Client->SendSetSingleCharacterOption(0x31,MouseTurning);
            for (FKey Key : {EKeys::A,EKeys::D,EKeys::Left,EKeys::Right})
            {
                Controller->PlayerInput->FlushPressedKeys();PressKey(Key);
                if (Forward) PressKey(EKeys::W);
                if (!Run) PressKey(EKeys::LeftShift);
                Controller->PlayerInput->ProcessInputStack({},.016f,false);
                Pawn->SetActorTransform(StartTransform);Controller->PredictedPose=StartPose;
                TestBoom->SetRelativeRotation(FRotator(-15,150,0));
                TickPlayer(.016f);
                const float ExpectedTurn=(Key==EKeys::A || Key==EKeys::Left)?-1.f:1.f;
                TestEqual(TEXT("A/D and arrows remain turn inputs in both mouse modes"),Controller->TurnAxis,ExpectedTurn);
                TestEqual(TEXT("Turn keys never become sidestep inputs"),Controller->RightAxis,0.f);
                // The DAT turn modifier is 1.5 rad/s; retail CMotionInterp's
                // Run modifier multiplies it by 1.5 (also checked in RunSpeedParity).
                const float ExpectedRadians=1.5f*(Run?1.5f:1.f);
                const FQuat Expected=(FQuat(FVector::UpVector,-ExpectedRadians*ExpectedTurn*.016f)*StartPose.GetAcQuat()).GetNormalized();
                TestTrue(TEXT("Keyboard turning changes predicted facing even with camera-follow movement"),Controller->PredictedPose.GetAcQuat().Equals(Expected,.001));
            }
        }
        for (FKey Key : {EKeys::Z,EKeys::C})
        {
            Controller->PlayerInput->FlushPressedKeys();PressKey(Key);
            Controller->PlayerInput->ProcessInputStack({},.016f,false);
            Pawn->SetActorTransform(StartTransform);Controller->PredictedPose=StartPose;
            TestBoom->SetRelativeRotation(FRotator(-15,90,0));
            TickPlayer(.016f);
            TestEqual(TEXT("Retail Z/C sidestep bindings"),Controller->RightAxis,Key==EKeys::Z?-1.f:1.f);
            TestEqual(TEXT("Sidestep does not supply keyboard turn input"),Controller->TurnAxis,0.f);
        }
        Controller->PlayerInput->FlushPressedKeys();
        Controller->PlayerInput->ProcessInputStack({},.016f,false);
        // Feed the physical-key mapping into the real gameplay poll. Num Lock
        // must never turn an orbit key into forward/backward/turn movement.
        for (bool NumLockOn : {false,true}) for (int32 Direction=0;Direction<4;++Direction)
        {
#if PLATFORM_WINDOWS
            const uint32 Scans[]={0x4B,0x4D,0x48,0x50}, Navigation[]={0x25,0x27,0x26,0x28}, Numpad[]={0x64,0x66,0x68,0x62};
            const uint32 Mapped=FACEKeyboardRouter::NumpadVirtualKey(NumLockOn?Numpad[Direction]:Navigation[Direction],Scans[Direction],false);
            const FKey PhysicalKey=FInputKeyManager::Get().GetKeyFromCodes(Mapped,0);
#else
            // Other platform backends already produce Unreal keypad keys.
            // Windows VK values are unrelated to SDL's native key codes.
            const FKey KeypadKeys[]={EKeys::NumPadFour,EKeys::NumPadSix,EKeys::NumPadEight,EKeys::NumPadTwo};
            const FKey PhysicalKey=KeypadKeys[Direction];
#endif
            Controller->PlayerInput->FlushPressedKeys();PressKey(PhysicalKey);
            Controller->PlayerInput->ProcessInputStack({},.016f,false);
            Pawn->SetActorTransform(StartTransform);Controller->PredictedPose=StartPose;Controller->bHavePredictedPose=true;
            Controller->bCameraDefaultsCaptured=true;TestBoom->SetRelativeRotation(FRotator(-15,0,0));
            TickPlayer(.016f);
            TestTrue(TEXT("Numpad camera input never supplies player movement in either Num Lock state"),
                Controller->ForwardAxis==0 && Controller->RightAxis==0 && Controller->TurnAxis==0);
            TestFalse(TEXT("Numpad still moves the camera in either Num Lock state"),TestBoom->GetRelativeRotation().Equals(FRotator(-15,0,0),.001));
        }
        Controller->PlayerInput->FlushPressedKeys();Controller->PlayerInput->ProcessInputStack({},.016f,false);
        TestBoom->TargetArmLength=600;Controller->UserCameraArmLength=600;
        Controller->bCameraDefaultsCaptured=true;
        for(const FKey& Key:{EKeys::Subtract,EKeys::Add})
        {
            // Test each direction from a settled view. Reversing an unfinished
            // zoom can still travel toward the nearer target during its blend.
            Controller->SyncUserCameraArmLength(TestBoom);
            PressKey(Key);Controller->PlayerInput->ProcessInputStack({},.1f,false);
            const float Before=TestBoom->TargetArmLength;
            TickPlayer(.1f);
            TestTrue(TEXT("Physical keypad +/- zoom the camera with retail direction"),Key==EKeys::Subtract ? TestBoom->TargetArmLength<Before : TestBoom->TargetArmLength>Before);
            Controller->PlayerInput->FlushPressedKeys();Controller->PlayerInput->ProcessInputStack({},.1f,false);
        }
        PressKey(ACEInputBindings::NumpadEnterKey());Controller->PlayerInput->ProcessInputStack({},.016f,false);
        TickPlayer(.016f);
        TestTrue(TEXT("Physical keypad Enter toggles overhead mode"),Controller->bCameraMapMode);
        Controller->PlayerInput->FlushPressedKeys();Controller->PlayerInput->ProcessInputStack({},.016f,false);TickPlayer(.016f);
        PressKey(ACEInputBindings::NumpadEnterKey());Controller->PlayerInput->ProcessInputStack({},.016f,false);TickPlayer(.016f);
        TestFalse(TEXT("Second physical keypad Enter returns to normal view"),Controller->bCameraMapMode);
        Controller->PlayerInput->FlushPressedKeys();Controller->PlayerInput->ProcessInputStack({},.016f,false);
        // Tiny mouse turns are below the packet-send threshold but still belong
        // to local prediction; otherwise the next idle tick restores the old yaw.
        Client->SendSetSingleCharacterOption(0x31,true);
        Controller->bHavePredictedPose=false;Controller->bHaveLastServerPose=false;
        Session->SetLocalPosition(StartPose);Pawn->SetActorTransform(StartTransform);
        // The offscreen fixture must not inherit a cursor over another test's
        // Slate controls: this checks turning, not UI gesture interception.
        const FVector2D SavedCursor=FSlateApplication::Get().GetCursorPos();
        FSlateApplication::Get().SetCursorPos(FVector2D(-100000,-100000));
        PressKey(EKeys::RightMouseButton);
        Controller->PlayerInput->ProcessInputStack({},.016f,false);
        TestBoom->SetRelativeRotation(FRotator(-15,.1f,0));
        TickPlayer(.016f);
        const FQuat TinyTurn=Pawn->GetActorQuat();
        TestTrue(TEXT("Sub-threshold mouse turn retains prediction"),Controller->bHavePredictedPose && Controller->bLocalPredicting);
        TestFalse(TEXT("Sub-threshold mouse turn actually changes facing"),TinyTurn.Equals(StartTransform.GetRotation(),.000001f));
        Controller->PlayerInput->FlushPressedKeys();Controller->PlayerInput->ProcessInputStack({},.016f,false);
        TickPlayer(.016f);
        TestTrue(TEXT("Sub-threshold mouse turn survives the next idle frame"),Pawn->GetActorQuat().Equals(TinyTurn,.00001f));
        FSlateApplication::Get().SetCursorPos(SavedCursor);
        // The double-click interaction path must arrive and send Use with any orbit offset.
        // Use the actual controller, collision, network writer and isolated loopback socket.
        auto* ApproachSockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        auto* ApproachReceiver=ApproachSockets->CreateSocket(NAME_DGram,TEXT("Approach fixture receiver"),false);
        auto ApproachAddress=ApproachSockets->CreateInternetAddr(); bool AddressValid=false;
        ApproachAddress->SetIp(TEXT("127.0.0.1"),AddressValid);ApproachAddress->SetPort(0);
        if (!ApproachReceiver || !ApproachReceiver->Bind(*ApproachAddress)) return false;
        ApproachReceiver->GetAddress(*ApproachAddress);
        Session->SocketC2S=ApproachSockets->CreateSocket(NAME_DGram,TEXT("Approach fixture sender"),false);
        Session->ServerC2SAddr=ApproachAddress;Session->IssacClient=MakeUnique<FACEIsaac>(123u);
        auto CountAction=[&](uint32 Action)
        {
            int32 Count=0;
            for (const auto& Pair:Session->CachedC2SPackets)
            {
                FACEBinaryReader R(Pair.Value.Payload);R.Skip(16);
                if (R.ReadUInt32()!=ACEOpcode::GameAction) continue;
                R.ReadUInt32();if (R.ReadUInt32()==Action) ++Count;
            }
            return Count;
        };
        // Exercise real mouse-facing prediction and its outgoing packets, not
        // just the rate helper. Mouse motion stops when input stops; excess
        // camera displacement must not become a queued turn after release.
        Client->SendSetSingleCharacterOption(0x31,true);
        FSlateApplication::Get().SetCursorPos(FVector2D(-100000,-100000));
        for(int32 FPS:{30,60,144,240})
        {
            Controller->ClearServerMoveTo();Controller->bAutoRun=false;
            Controller->PredictedPose=StartPose;Controller->bHavePredictedPose=true;
            Controller->bHaveLastServerPose=false;Controller->bJumpAirborne=false;
            Controller->MouseTurnReportSeconds=.5f;Controller->bMouseTurnSent=false;
            Session->SetLocalPosition(StartPose);Pawn->SetActorTransform(StartTransform);
            PressKey(EKeys::RightMouseButton);
            Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
            Controller->UpdateMouseButtons(true,false,false,false);
            Controller->CameraInputFrameSeconds=1.f/FPS;
            TestBoom->SetRelativeRotation(FRotator(-15,90,0));
            Session->CachedC2SPackets.Reset();
            for(int32 Frame=0;Frame<FPS;++Frame)
            {
                const FQuat Before=Controller->PredictedPose.GetAcQuat();
                // Exercise CameraSet's input clamp. Directly changing the
                // orbit represents a chosen view that the body must catch up
                // to, not excess mouse input that should be discarded.
                Controller->ApplyMouseLookDelta(1000,0,TestBoom);
                TickPlayer(1.f/FPS);
                TestTrue(TEXT("Mouse-facing character obeys retail's angular rate at every FPS"),
                    FMath::IsNearlyEqual(Before.AngularDistance(Controller->PredictedPose.GetAcQuat()),3.375f/FPS,.0001f));
            }
            const int32 Reports=CountAction(ACEGameAction::MoveToState);
            TestTrue(FString::Printf(TEXT("Mouse turn uses half-second reports at %d FPS (got %d)"),FPS,Reports),Reports>=2 && Reports<=3);
            const FQuat AtRest=Controller->PredictedPose.GetAcQuat();
            TickPlayer(1.f/FPS);
            TestTrue(TEXT("Stopped mouse does not leave a queued camera-facing turn"),
                AtRest.Equals(Controller->PredictedPose.GetAcQuat(),.00001f));
            Controller->PlayerInput->FlushPressedKeys();Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
            Controller->MouseTurnReportSeconds=.5f;TickPlayer(1.f/FPS);

            // Airborne rotation is legal in retail while the jump translation
            // remains ballistic. Do not show a local turn but send zero turn.
            Controller->PredictedPose.Location.Z+=5;Controller->bJumpAirborne=true;
            Session->SetLocalPosition(Controller->PredictedPose);
            PressKey(EKeys::D);Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
            Session->CachedC2SPackets.Reset();TickPlayer(1.f/FPS);
            bool SawAirTurn=false;
            for(const auto& Pair:Session->CachedC2SPackets)
            {
                FACEBinaryReader R(Pair.Value.Payload);R.Skip(24);
                if(R.ReadUInt32()!=ACEGameAction::MoveToState)continue;
                const uint32 Flags=R.ReadUInt32();
                TestEqual(TEXT("Airborne keyboard turn has no translation commands"),Flags,uint32(0x703));
                if(Flags!=0x703)continue;
                R.Skip(8);TestEqual(TEXT("Airborne rotation is reported as TurnRight"),R.ReadUInt32(),ACEMotion::TurnRight);
                TestEqual(TEXT("Airborne keyboard turn inherits Run/Walk"),R.ReadUInt32(),0u);
                TestEqual(TEXT("Airborne keyboard turn has normal raw speed"),R.ReadFloat(),1.f);
                R.Skip(40);TestEqual(TEXT("Airborne turn never claims ground contact"),R.ReadUInt8(),uint8(0));
                SawAirTurn=true;
            }
            TestTrue(TEXT("Airborne visual turn reaches the server"),SawAirTurn);
            Controller->PlayerInput->FlushPressedKeys();Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
            Controller->bJumpAirborne=false;Controller->JumpWorldAceVelocity=FVector::ZeroVector;
        }
        FSlateApplication::Get().SetCursorPos(SavedCursor);
        FACEWorldObject UseTarget;UseTarget.Guid=54321;UseTarget.bHasPosition=true;
        UseTarget.ObjectDescriptionFlags=ACEObjectDescFlag::Stuck;UseTarget.ItemUseable=32;
        UseTarget.Name=TEXT("Approach fixture lever");UseTarget.UseRadius=.6f;
        UseTarget.Position=StartPose;
        const FVector StartFeet=StartPose.ToUnrealLocation(100);
        auto ResetApproach=[&](FVector TargetOffset)
        {
            Controller->ClearServerMoveTo();Session->bUseBusy=false;
            // Each case is a new interaction, with no unanswered chess join.
            Session->ChessJoinPendingUntil=0;Session->ChessBoardGuid=0;
            Controller->PredictedPose=StartPose;Controller->bHavePredictedPose=true;
            Controller->bHaveLastServerPose=false;Controller->StepHoldSeconds=0;
            Session->SetLocalPosition(StartPose);Pawn->SetActorTransform(StartTransform);
            UseTarget.Position.SetLocationFromUnreal(StartFeet+TargetOffset,100);
            Session->WorldObjects.Add(UseTarget.Guid,UseTarget);Session->CachedC2SPackets.Reset();
        };
        ResetApproach(FVector(500,0,0));
        Controller->PredictedPose.SetLocationFromUnreal(StartFeet+FVector(80,0,0),100);
        Controller->InteractWithObject(UseTarget.Guid);
        bool SawSynchronizedMove=false;
        for (const auto& Pair:Session->CachedC2SPackets)
        {
            FACEBinaryReader R(Pair.Value.Payload);R.Skip(16);
            if(R.ReadUInt32()!=ACEOpcode::GameAction)continue;
            R.ReadUInt32();if(R.ReadUInt32()!=ACEGameAction::MoveToState)continue;
            const int32 PositionOffset=Pair.Value.Payload.Num()-44;
            FACEBinaryReader UsePoseReader(Pair.Value.Payload);UsePoseReader.Skip(PositionOffset);
            TestEqual(TEXT("Ordered movement carries current cell"),UsePoseReader.ReadUInt32(),uint32(Controller->PredictedPose.CellId));
            const float X=UsePoseReader.ReadFloat(),Y=UsePoseReader.ReadFloat(),Z=UsePoseReader.ReadFloat();
            TestTrue(TEXT("Ordered movement carries current predicted feet, not previous idle pose"),FVector(X,Y,Z).Equals(FVector(Controller->PredictedPose.Location),.001f));
            SawSynchronizedMove=true;
        }
        TestTrue(TEXT("Interaction sends ordered movement before Use"),SawSynchronizedMove);
        TestTrue(TEXT("Interaction installs predicted feet before serializing movement"),Session->GetPlayerPosition().Location.Equals(Controller->PredictedPose.Location,.001f));
        for (bool MouseTurning : {false,true}) for (float Orbit : {-90.f,90.f,180.f})
        {
            Client->SendSetSingleCharacterOption(0x31,MouseTurning);
            ResetApproach(FVector(500,0,0));TestBoom->SetRelativeRotation(FRotator(-15,Orbit,0));
            Controller->InteractWithObject(UseTarget.Guid);
            TestTrue(TEXT("World item interaction starts an approach"),Controller->bServerMoveToActive);
            TestEqual(TEXT("Use is sent before walking so the server can supply approach rules"),CountAction(ACEGameAction::Use),1);
            Session->CachedC2SPackets.Reset();
            for (int32 Frame=0;Frame<240 && Controller->UseApproachPhase!=AACEPlayerController::EACEUseApproachPhase::Holding;++Frame)
                TickPlayer(.016f);
            TestEqual(TEXT("Approach never resends Use on arrival"),CountAction(ACEGameAction::Use),0);
            TestEqual(TEXT("Autonomous approach never cancels the server chain with MoveToState"),CountAction(ACEGameAction::MoveToState),0);
            const FVector Arrived=Controller->PredictedPose.ToUnrealLocation(100);
            TestTrue(TEXT("Orbit offset cannot steer automatic approach away from target"),FMath::Abs(Arrived.Y-StartFeet.Y)<1.f);
            TestTrue(TEXT("Use is sent only after reaching the target cylinder"),
                Controller->GetUseCylinderDistanceCm(UseTarget,Arrived,UseTarget.Position.ToUnrealLocation(100))<=62.f);
        }
        // Stuck game boards are usable world objects; corpses can retain the Dead pose.
        for (bool bCorpse : {false, true}) for (float TargetHeight : {-35.f,0.f,45.f}) for (float Dt : {1.f/30,1.f/90,1.f/144})
        {
            UseTarget.ObjectDescriptionFlags = ACEObjectDescFlag::Stuck
                | (bCorpse ? ACEObjectDescFlag::Corpse : 0u);
            UseTarget.ItemType = bCorpse ? ACEItemType::Container : static_cast<int32>(0x80000000u);
            UseTarget.ItemUseable = 1; // the public special-object descriptor enables Use
            UseTarget.InitialMotionCommand = bCorpse ? ACEMotion::Dead : ACEMotion::Ready;
            ResetApproach(FVector(300,0,TargetHeight));
            Client->SelectObject(UseTarget.Guid);
            Controller->InteractWithSelectedObject();
            TestTrue(TEXT("Chess boards and corpses start a use approach"), Controller->bServerMoveToActive);
            for (int32 Frame=0; Frame<6.f/Dt && Controller->UseApproachPhase!=AACEPlayerController::EACEUseApproachPhase::Holding; ++Frame)
                TickPlayer(Dt);
            TestEqual(TEXT("Chess/corpse arrival sends exactly one retail interaction"),
                CountAction(bCorpse ? ACEGameAction::Use : ACEGameAction::ChessJoin), 1);
            TestEqual(TEXT("Chess boards use JoinGame instead of generic Use"),
                CountAction(bCorpse ? ACEGameAction::ChessJoin : ACEGameAction::Use), 0);
            TestTrue(TEXT("Corpse approach reaches server range for elevated and downhill targets at every frame rate"),
                Controller->GetUseCylinderDistanceCm(UseTarget,Controller->PredictedPose.ToUnrealLocation(100),
                    UseTarget.Position.ToUnrealLocation(100))<=UseTarget.UseRadius*100);
        }
        // Neutral-stick VR interactions share arrival and use networking while
        // preserving the tracked head direction, even approaching sideways/backward.
        auto* ApproachVR=NewObject<UACEVRComponent>(Pawn);
        Pawn->AddInstanceComponent(ApproachVR);ApproachVR->RegisterComponent();
        ApproachVR->PC=Controller;ApproachVR->Client=Client;
        ApproachVR->Settings=NewObject<UACEVRSettings>();ApproachVR->Settings->MovementSmoothing=0;
        ApproachVR->Settings->MovementDirection=0;ApproachVR->bActive=true;ApproachVR->bTracking=true;
        ApproachVR->Head=NewObject<UCameraComponent>(Pawn);Pawn->AddInstanceComponent(ApproachVR->Head);
        ApproachVR->Head->SetupAttachment(Pawn->GetRootComponent());ApproachVR->Head->RegisterComponent();
        ApproachVR->Head->SetUsingAbsoluteRotation(true);
        for (float Yaw : {0.f,90.f,180.f}) for (float Height : {-35.f,45.f})
        {
            ResetApproach(FVector(300,0,Height));
            ApproachVR->Head->SetWorldRotation(FRotator(0,Yaw,0));
            Controller->InteractWithObject(UseTarget.Guid);
            for(int32 Frame=0;Frame<540 && Controller->UseApproachPhase!=AACEPlayerController::EACEUseApproachPhase::Holding;++Frame)
                TickPlayer(1.f/90);
            TestEqual(TEXT("VR corpse approach sends exactly one Use"),CountAction(ACEGameAction::Use),1);
            TestTrue(TEXT("Neutral stick permits VR corpse approach to the full server cylinder range"),
                Controller->GetUseCylinderDistanceCm(UseTarget,Controller->PredictedPose.ToUnrealLocation(100),
                    UseTarget.Position.ToUnrealLocation(100))<=UseTarget.UseRadius*100);
            TestTrue(TEXT("VR approach preserves head-facing at every approach angle"),
                Controller->PredictedPose.GetFacingDotUnreal2D(ApproachVR->GetBodyForward())>.9999f);
            TestEqual(TEXT("VR approach leaves tracked head yaw unchanged"),ApproachVR->Head->GetComponentRotation().Yaw,double(Yaw));
        }
        ResetApproach(FVector(300,0,0));Controller->InteractWithObject(UseTarget.Guid);
        ApproachVR->MoveStick=FVector2D(.5f,0);TickPlayer(1.f/90);
        TestFalse(TEXT("Manual VR stick cancels automatic approach immediately"),Controller->bServerMoveToActive);
        TestFalse(TEXT("Manual VR cancellation clears the pending world Use"),Client->IsUseBusy());
        ApproachVR->MoveStick=FVector2D::ZeroVector;
        ResetApproach(FVector(300,0,0));Controller->InteractWithObject(UseTarget.Guid);
        ApproachVR->bTracking=false;TickPlayer(1.f/90);
        TestFalse(TEXT("Losing tracking cancels VR automatic translation"),Controller->bServerMoveToActive);
        ApproachVR->Head->DestroyComponent();ApproachVR->bActive=false;ApproachVR->DestroyComponent();
        UseTarget.ObjectDescriptionFlags=ACEObjectDescFlag::Stuck;
        UseTarget.ItemType=0; UseTarget.ItemUseable=32; UseTarget.InitialMotionCommand=ACEMotion::Ready;
        // Decode actual MoveTo packets, retaining the start across echoes and all finite limits.
        Session->OnMotionUpdate.AddUObject(Controller,&AACEPlayerController::HandleMotionUpdate);
        Session->OnMoveToFailed.AddUObject(Controller,&AACEPlayerController::HandleMoveToFailed);
        uint16 MotionSequence=1;
        auto ReceiveApproach=[&](float Limit,float Distance=.6f)
        {
            FACEBinaryWriter W;W.WriteUInt32(SelfObject.Guid);W.WriteUInt16(0);
            W.WriteUInt16(MotionSequence++);W.WriteUInt16(0);W.WriteUInt8(0);W.Align();
            W.WriteUInt8(6);W.WriteUInt8(0);W.WriteUInt16(0);W.WriteUInt32(UseTarget.Guid);
            W.WriteUInt32(UseTarget.Position.CellId);W.WriteFloat(UseTarget.Position.Location.X);
            W.WriteFloat(UseTarget.Position.Location.Y);W.WriteFloat(UseTarget.Position.Location.Z);
            W.WriteUInt32(0);W.WriteFloat(Distance);W.WriteFloat(0);W.WriteFloat(Limit);
            W.WriteFloat(1);W.WriteFloat(15);W.WriteFloat(0);W.WriteFloat(1);
            FACEBinaryReader R(W.GetData());Session->HandleUpdateMotion(R);
        };
        // A corpse can already be inside its advertised radius when clicked,
        // while ACE later requests a tighter radius (magic stance crouch).
        // Use actual human Setup dimensions and delay the echo past Holding's
        // old 0.2-second gate. The one Use request must survive resumed movement.
        UseTarget.SetupId=0x02000001;UseTarget.UseRadius=.6f;
        UseTarget.ObjectDescriptionFlags=ACEObjectDescFlag::Stuck|ACEObjectDescFlag::Corpse;
        UseTarget.ItemType=ACEItemType::Container;UseTarget.ItemUseable=1;
        UseTarget.InitialMotionCommand=ACEMotion::Dead;
        float CorpseStep=0,CorpseHeight=0,CorpseRadius=0;uint32 CorpseAnim=0;
        TestTrue(TEXT("Corpse range fixture loads retail Setup dimensions"),
            Dat->TryGetSetupPhysics(UseTarget.SetupId,CorpseStep,CorpseHeight,CorpseRadius,CorpseAnim));
        const float InitialOffset=(Controller->PlayerSetupRadiusAc*Controller->GetLocalCreatureScale()+CorpseRadius+.5f)*100.f;
        for(float EchoDelay:{.0f,.5f,1.5f}) for(float Dt:{1.f/30,1.f/144})
        {
            ResetApproach(FVector(InitialOffset,0,0));
            Controller->InteractWithObject(UseTarget.Guid);
            TestEqual(TEXT("Corpse inside advertised range initially holds"),Controller->UseApproachPhase,
                AACEPlayerController::EACEUseApproachPhase::Holding);
            for(int32 Frame=0;Frame<EchoDelay/Dt;++Frame)TickPlayer(Dt);
            ReceiveApproach(MAX_flt,.4f);
            TestTrue(TEXT("Delayed server radius requires a closer corpse approach"),
                Controller->GetUseCylinderDistanceCm(UseTarget,Controller->PredictedPose.ToUnrealLocation(100),
                    UseTarget.Position.ToUnrealLocation(100))>40.f);
            const int32 MovesBefore=CountAction(ACEGameAction::MoveToState);
            for(int32 Frame=0;Frame<3.f/Dt;++Frame)
            {
                TickPlayer(Dt);
                if(Controller->UseApproachPhase==AACEPlayerController::EACEUseApproachPhase::Holding
                    &&Controller->GetUseCylinderDistanceCm(UseTarget,Controller->PredictedPose.ToUnrealLocation(100),
                        UseTarget.Position.ToUnrealLocation(100))<=40.f)break;
            }
            TestTrue(FString::Printf(TEXT("Delayed corpse approach reaches server radius without another click (delay %.1f, FPS %.0f)"),EchoDelay,1.f/Dt),
                Controller->GetUseCylinderDistanceCm(UseTarget,Controller->PredictedPose.ToUnrealLocation(100),
                    UseTarget.Position.ToUnrealLocation(100))<=40.f);
            TestEqual(TEXT("Delayed corpse approach never repeats Use"),CountAction(ACEGameAction::Use),1);
            TestEqual(TEXT("Resuming approach does not cancel the server's pending Use"),CountAction(ACEGameAction::MoveToState),MovesBefore);
        }
        UseTarget.SetupId=0;UseTarget.ObjectDescriptionFlags=ACEObjectDescFlag::Stuck;
        UseTarget.ItemType=0;UseTarget.ItemUseable=32;UseTarget.InitialMotionCommand=ACEMotion::Ready;
        int32 ChargeErrors=0;
        // A pending mouse-turn release must not cancel a custom direct-grant
        // object after its Use was sent, even if the half-second timer expires.
        ResetApproach(FVector(100,0,0));
        UseTarget.ItemType=0x80;UseTarget.SetupId=0x02001B99;UseTarget.Name=TEXT("Corpse of Daimyo Nobanuga");
        Session->WorldObjects.Add(UseTarget.Guid,UseTarget);
        Controller->bMouseTurnSent=true;Controller->MouseTurnReportSeconds=.49f;
        Controller->InteractWithObject(UseTarget.Guid);
        const int32 BeforeMouseRelease=CountAction(ACEGameAction::MoveToState);
        for(int32 Frame=0;Frame<90;++Frame)TickPlayer(1.f/60);
        TestEqual(TEXT("Previous mouse turn does not cancel direct-grant Use"),CountAction(ACEGameAction::MoveToState),BeforeMouseRelease);
        TestEqual(TEXT("Custom reward use is sent exactly once"),CountAction(ACEGameAction::Use),1);
        Controller->EndUseApproach();Session->ClearPendingUse();UseTarget.SetupId=0;UseTarget.ItemType=0;
        const auto ChatHandle=Session->OnChatMessage.AddLambda([&](const FString& Message,const FString&,int32 Type)
        {
            if (Message==TEXT("You charged too far!"))
            {
                ++ChargeErrors;TestEqual(TEXT("Charge failure uses retail error chat"),Type,ACEChatMessageType::ChatError);
            }
        });
        for (float Displacement : {1499.f,1500.f,1501.f})
        {
            ResetApproach(FVector(10000,0,0));ChargeErrors=0;
            Controller->BeginUseApproach(UseTarget.Guid,.6f,false);ReceiveApproach(15);
            const FACEPosition OriginalStart=Controller->ApproachStartPose;
            Controller->PredictedPose=OriginalStart;
            Controller->PredictedPose.Location.Z+=Displacement/100.f;
            ReceiveApproach(15);
            TestTrue(TEXT("Server echo does not reset the charge starting position"),
                Controller->ApproachStartPose.ToUnrealLocation(100).Equals(OriginalStart.ToUnrealLocation(100),.001));
            Session->CachedC2SPackets.Reset();TickPlayer(.016f);
            TestEqual(FString::Printf(TEXT("Charge distance includes vertical travel %.1f cm and only fails beyond limit"),Displacement),Controller->bServerMoveToActive,Displacement<=1500);
            TestEqual(TEXT("Exceeding charge limit reports failure once"),ChargeErrors,Displacement>1500?1:0);
            TestEqual(TEXT("Failed or distant approach never uses the object"),CountAction(ACEGameAction::Use),0);
            if (Displacement>1500)
            {
                TestTrue(TEXT("Failed approach sends stop to the server"),CountAction(ACEGameAction::MoveToState)>0);
                TestFalse(TEXT("Failure releases forced position reporting"),Session->bForcePositionReporting);
                TickPlayer(.016f);TestEqual(TEXT("Later ticks cannot repeat the failure"),ChargeErrors,1);
            }
        }
        ResetApproach(FVector(2000,0,0));ReceiveApproach(15);ChargeErrors=0;
        Controller->PredictedPose=UseTarget.Position;TickPlayer(.016f);
        TestEqual(TEXT("Arrival wins over charge distance at target"),ChargeErrors,0);
        ResetApproach(FVector(10000,0,0));ReceiveApproach(250);
        TestEqual(TEXT("Finite server limits above 100 are preserved"),Controller->ServerMoveToFailDistance,250.f);
        ReceiveApproach(MAX_flt);
        TestEqual(TEXT("Unlimited server echo clears previous finite limit"),Controller->ServerMoveToFailDistance,0.f);
        for (float Distance : {.4f,0.f,-.2f})
        {
            ReceiveApproach(MAX_flt,Distance);
            TestEqual(TEXT("Server echo preserves reduced, zero and negative use distances"),Controller->ServerMoveToDistance,Distance);
        }
        ChargeErrors=0;FACEBinaryWriter Error;Error.WriteUInt32(0x003D);
        FACEBinaryReader ErrorReader(Error.GetData());Session->HandleWeenieError(ErrorReader);
        TestFalse(TEXT("Server charge error cancels an active local approach"),Controller->bServerMoveToActive);
        TestEqual(TEXT("Server charge error retains its retail message"),ChargeErrors,1);
        Session->OnChatMessage.Remove(ChatHandle);Session->OnMotionUpdate.RemoveAll(Controller);
        Session->OnMoveToFailed.RemoveAll(Controller);Session->WorldObjects.Remove(UseTarget.Guid);
        Session->bUseBusy=false;Controller->ClearServerMoveTo();
        Session->SocketC2S->Close();ApproachSockets->DestroySocket(Session->SocketC2S);Session->SocketC2S=nullptr;
        Session->ServerC2SAddr.Reset();Session->CachedC2SPackets.Reset();
        ApproachReceiver->Close();ApproachSockets->DestroySocket(ApproachReceiver);
        TestBoom->DestroyComponent();
        Client->SendSetSingleCharacterOption(0x31,MouseTurningBefore);
        Pawn->SetActorTransform(StartTransform);Controller->PredictedPose=StartPose;
        TickPlayer(.016f);
    }
    ACEInputBindings::Reload();
    for (uint32 Stance : {ACEMotion::StanceNonCombat,ACEMotion::StanceMagic})
    {
        App->SetPreferredStyle(Stance);
        for (int32 Frame=0;Frame<30;++Frame) App->TickComponent(.016f,LEVELTICK_All,nullptr);
        float LowestVertex=MAX_flt;
        for (int32 Leg : {0,1,2,5,6}) if (auto* Part=Cast<UProceduralMeshComponent>(App->GetPartMesh(Leg)))
            for (int32 Section=0;Section<Part->GetNumSections();++Section)
                for (const auto& Vertex:Part->GetProcMeshSection(Section)->ProcVertexBuffer)
                    LowestVertex=FMath::Min(LowestVertex,float(Part->GetComponentTransform().TransformPosition(Vertex.Position).Z));
        AddInfo(FString::Printf(TEXT("Grounded robe stance=%08X lowestVertex=%.3f floor=%.3f"),Stance,LowestVertex,Feet.Z));
    }
    // Walk both across and along a narrow seat using the actual controller tick.
    // A center ray can already see the floor while the foot still overlaps the
    // seat. The body must descend once after clearing it, not alternate floors.
    auto* BenchSeat=Fixture.World->SpawnActor<AActor>();
    auto* SeatMesh=NewObject<UInstancedStaticMeshComponent>(BenchSeat);
    BenchSeat->SetRootComponent(SeatMesh);BenchSeat->AddInstanceComponent(SeatMesh);
    Dat->GetOrBuildSetupMesh(0x02000120,100,UACEDatSubsystem::ACEPlacementResting);
    auto* SeatAsset=Dat->GetOrCreateSetupStaticMesh(0x02000120,100,true,UACEDatSubsystem::ACEPlacementResting);
    if (!TestNotNull(TEXT("Actual town hall bench physics loads"),SeatAsset)) return false;
    SeatMesh->SetStaticMesh(SeatAsset);SeatMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    SeatMesh->SetCollisionResponseToAllChannels(ECR_Block);SeatMesh->RegisterComponent();
    const FVector SeatCenter=SeatAsset->GetBoundingBox().GetCenter();
    SeatMesh->AddInstance(FTransform(Feet-FVector(SeatCenter.X,SeatCenter.Y,0)),true);
    for (bool Indoor : {false,true})
    {
        // Isolate the same cooked bench in each movement mode; the room/stair
        // transit fixtures separately cover the surrounding town hall geometry.
        FACEPosition BenchPose=Pose;
        if (Indoor) BenchPose.CellId=0x0134010E;
        const FVector BenchFeet=BenchPose.ToUnrealLocation(100);
        Floor->SetActorLocation(BenchFeet-FVector(0,0,10));
        SeatMesh->UpdateInstanceTransform(0,FTransform(BenchFeet-FVector(SeatCenter.X,SeatCenter.Y,0)),true,true,true);
        for (bool Along : {false,true}) for (int32 Direction : {-1,1})
        {
            const FVector StartOffset=Along?FVector(0,-240*Direction,0):FVector(-110*Direction,0,0);
            FACEPosition WalkPose=BenchPose; WalkPose.SetLocationFromUnreal(BenchFeet+StartOffset,100);
            WalkPose.SetAceFacingFromUnrealDir2D(Along?FVector(0,Direction,0):FVector(Direction,0,0));
            Session->SetLocalPosition(WalkPose); Controller->PredictedPose=WalkPose;
            Controller->bHavePredictedPose=true; Controller->bLocalPredicting=true;
            Controller->StepHoldSeconds=0; Controller->bHaveLastServerPose=false;
            Pawn->SetActorLocation(BenchFeet+StartOffset+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()));
            Controller->bAutoRun=true;
            float PreviousZ=BenchFeet.Z, MaxZ=BenchFeet.Z;
            int32 Reversals=0, LastVertical=0, SeatPenetrations=0;
            for (int32 Frame=0;Frame<45;++Frame)
            {
                TickPlayer(.016f);
                const float Z=Pawn->GetActorLocation().Z-Capsule->GetScaledCapsuleHalfHeight();
                const int32 Vertical=Z>PreviousZ+2?1:(Z<PreviousZ-2?-1:0);
                if (Vertical && LastVertical && Vertical!=LastVertical) ++Reversals;
                if (Vertical) LastVertical=Vertical;
                MaxZ=FMath::Max(MaxZ,Z); PreviousZ=Z;
                FHitResult Contact; FCollisionQueryParams Q(NAME_None,true,Pawn); Q.AddIgnoredActor(Floor);
                const FVector Probe=Pawn->GetActorLocation()+FVector(0,0,.5f);
                Fixture.World->SweepSingleByChannel(Contact,Probe,Probe+FVector(0,0,.1f),FQuat::Identity,ECC_Pawn,
                    FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),Capsule->GetScaledCapsuleHalfHeight()),Q);
                if (Contact.Component==SeatMesh && Contact.bStartPenetrating && Contact.PenetrationDepth>.1f) ++SeatPenetrations;
                TestEqual(TEXT("Bench tick retains the tested indoor/outdoor movement mode"),
                    (static_cast<uint32>(Controller->PredictedPose.CellId)&0xFFFFu)>=0x100u,Indoor);
            }
            const FVector Travel=Pawn->GetActorLocation()-(BenchFeet+StartOffset);
            AddInfo(FString::Printf(TEXT("Bench walk indoor=%d along=%d direction=%d travel=%s maxRise=%.2f reversals=%d penetrations=%d finalFeet=%.2f"),
                Indoor,Along,Direction,*Travel.ToString(),MaxZ-BenchFeet.Z,Reversals,SeatPenetrations,PreviousZ-BenchFeet.Z));
            TestTrue(TEXT("Walking crosses the bench without getting stuck"),(Along?Travel.Y:Travel.X)*Direction>(Along?480.f:220.f));
            TestTrue(TEXT("Walking climbs the seat"),MaxZ>BenchFeet.Z+40);
            TestTrue(TEXT("Bench traversal rises then descends without oscillation"),Reversals<=1);
            TestEqual(TEXT("Walking never penetrates the bench seat"),SeatPenetrations,0);
            TestTrue(TEXT("Walking returns to floor contact clearance after clearing the bench"),FMath::Abs(PreviousZ-BenchFeet.Z)<=.65f);
            Controller->bAutoRun=false; Controller->ForwardAxis=0;
        }
    }
    Floor->SetActorLocation(Feet-FVector(0,0,10));
    BenchSeat->Destroy();
    // Use the reported Yaraq login coordinates against actual cooked terrain.
    FACEPosition YaraqPose; YaraqPose.CellId=0x7D64000D; YaraqPose.Location=FVector(35.68,102.89,12.01);
    const FVector YaraqFeet=YaraqPose.ToUnrealLocation(100);
    Dat->GetOrBuildLandblockMesh(0x7D640000,100);
    auto* Yaraq=Fixture.World->SpawnActor<AACELandblockActor>();
    TestTrue(TEXT("Reported Yaraq terrain loads"),Yaraq->LoadLandblock(0x7D640000,100,true,1,0));
    Session->SetLocalPosition(YaraqPose); Controller->PredictedPose=YaraqPose;
    Pawn->SetActorLocation(YaraqFeet+FVector(0,0,Capsule->GetScaledCapsuleHalfHeight()));
    for (int32 Frame=0;Frame<30;++Frame)
    {
        TickPlayer(.016f);
    }
    float YaraqGround=0;
    TestTrue(TEXT("Reported Yaraq floor can be sampled"),Dat->SampleOutdoorGroundZ(YaraqFeet.X,YaraqFeet.Y,100,YaraqGround));
    AddInfo(FString::Printf(TEXT("Yaraq grounding DAT=%.3f capsuleFeet=%.3f meshOrigin=%.3f"),YaraqGround,
        Pawn->GetActorLocation().Z-Capsule->GetScaledCapsuleHalfHeight(),App->GetMeshRoot()->GetComponentLocation().Z));
    TestTrue(TEXT("Player origin meets the actual Yaraq terrain"),FMath::Abs(App->GetMeshRoot()->GetComponentLocation().Z-YaraqGround)<.5f);
    // Collision coordinates alone cannot expose floating caused by pixel depth
    // offset: town terrain used to write a surface 48 cm behind the real floor.
    Yaraq->SetLandEnvCellFloorPriority(true,Dat,true);
    auto* DepthOwner=Fixture.World->SpawnActor<AActor>();
    auto* DepthCapture=NewObject<USceneCaptureComponent2D>(DepthOwner);
    DepthOwner->SetRootComponent(DepthCapture); DepthCapture->RegisterComponent();
    auto* DepthTarget=NewObject<UTextureRenderTarget2D>(DepthCapture);
    // Vulkan's FLinearColor readback supports RGBA32f, but not single-channel
    // R32f. Preserve 32-bit depth precision on both Vulkan and D3D fixtures.
    DepthTarget->RenderTargetFormat=RTF_RGBA32f; DepthTarget->InitAutoFormat(32,32);
    DepthCapture->TextureTarget=DepthTarget; DepthCapture->CaptureSource=SCS_SceneDepth;
    DepthCapture->bCaptureEveryFrame=false; DepthCapture->bCaptureOnMovement=false; DepthCapture->FOVAngle=10;
    DepthCapture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    DepthCapture->ShowOnlyComponent(Yaraq->TerrainMesh);
    DepthCapture->SetWorldLocationAndRotation(FVector(YaraqFeet.X,YaraqFeet.Y,YaraqGround+500),FRotator(-90,0,0));
    if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
    Fixture.World->SendAllEndOfFrameUpdates(); DepthCapture->CaptureScene(); FlushRenderingCommands();
    TArray<FLinearColor> DepthPixels;
    DepthTarget->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(DepthPixels);
    if (TestTrue(TEXT("Terrain depth capture returns pixels"),DepthPixels.Num()==32*32))
    {
        const float Depth=DepthPixels[16*32+16].R;
        AddInfo(FString::Printf(TEXT("Yaraq visible terrain depth %.3f cm; actual floor is 500 cm from camera"),Depth));
        TestTrue(TEXT("Town rendered depth remains within 5 mm of collision ground"),FMath::Abs(Depth-500.f)<.5f);
    }
    DepthOwner->Destroy();
    Yaraq->Destroy(); Session->SetLocalPosition(Pose); Controller->PredictedPose=Pose;
    Session->State=EACESessionState::Disconnected;
    Session->WorldObjects.Remove(SelfObject.Guid); Session->PlayerGuid = PreviousGuid;
    Pawn->SetActorScale3D(FVector(1)); Capsule->SetCapsuleSize(22,88);
    auto* SmallObstacle = MakeBox(Feet + FVector(0,0,100), FVector(30,30,100));
    TestTrue(TEXT("Small obstacle gets a nearby supported placement"), Controller->FindWorldEntryPlacement(Placement));
    TestTrue(TEXT("Placement actually leaves the obstructed point"), FVector::Dist2D(Placement, Feet) > 30);
    SmallObstacle->Destroy();
    auto* Solid = MakeBox(Feet + FVector(0,0,200), FVector(600,600,200));
    TestFalse(TEXT("No valid placement within retail radius stays hidden"), Controller->FindWorldEntryPlacement(Placement));

    // A recovery is allowed only with proven resident destination collision.
    Fixture.World->SetGameMode(FURL(nullptr,TEXT("/Game/Test?game=/Script/Engine.GameModeBase"),TRAVEL_Absolute));
    auto* RecoveryGM=Fixture.World->GetAuthGameMode();
    auto* RecoveryTerrain=NewObject<UACETerrainPresenterComponent>(RecoveryGM);
    RecoveryGM->AddInstanceComponent(RecoveryTerrain);RecoveryTerrain->RegisterComponent();RecoveryTerrain->Client=Client;
    RecoveryTerrain->LastKnownCellId=Pose.CellId;RecoveryTerrain->bHasKnownCell=true;
    const uint32 RecoveryLandId=uint32(Pose.CellId)&0xFFFF0000u;
    Dat->GetOrBuildLandblockMesh(RecoveryLandId,100);
    auto* RecoveryLand=Fixture.World->SpawnActor<AACELandblockActor>();
    TestTrue(TEXT("Blocked arrival landblock loads"),RecoveryLand->LoadLandblock(RecoveryLandId,100,true,1,0));
    RecoveryTerrain->Spawned.Add(RecoveryLandId,RecoveryLand);
    const double RecoveryDeadline=FPlatformTime::Seconds()+10;
    while(!Controller->IsWorldEntryGeometryReady() && FPlatformTime::Seconds()<RecoveryDeadline)
    {
        ++GFrameCounter;FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        RecoveryLand->Tick(.016f);FPlatformProcess::Sleep(.002f);
    }
    TestTrue(TEXT("Blocked arrival has resident collision before considering recall"),Controller->IsWorldEntryGeometryReady());
    // Only loopback traffic. Assert the recovery uses the real retail GameAction,
    // sends once, and cannot reuse the original destination's LoginComplete.
    auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto* Receiver = Sockets->CreateSocket(NAME_DGram, TEXT("World entry recovery fixture"), false);
    if (!TestNotNull(TEXT("Loopback receiver"), Receiver)) return false;
    auto Address = Sockets->CreateInternetAddr(); bool Valid = false;
    Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(0);
    if (!Receiver->Bind(*Address)) { Sockets->DestroySocket(Receiver); return false; }
    Receiver->GetAddress(*Address); Receiver->SetNonBlocking(true);
    Session->SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("World entry sender"), false);
    Session->ServerC2SAddr = Address; Session->State = EACESessionState::InWorld;
    Session->IssacClient = MakeUnique<FACEIsaac>(123u);
    Controller->EnterWorldLoadElapsed = 44;
    TestFalse(TEXT("Slow loading within grace does not recall"), Controller->TickWorldEntryRecovery(.016f, TEXT("cell-collision")));
    Controller->EnterWorldLoadElapsed = 46;
    TestFalse(TEXT("Visual scenery wait does not trigger recall"), Controller->TickWorldEntryRecovery(.016f, TEXT("near-scenery")));
    TestTrue(TEXT("Unsafe destination requests recovery"), Controller->TickWorldEntryRecovery(.016f, TEXT("spawn-placement")));
    for (int32 I = 0; I < 120; ++I) Controller->TickWorldEntryRecovery(1.f / 60.f, TEXT("cell-collision"));
    int32 Recalls = 0;
    const double Until = FPlatformTime::Seconds() + .1;
    while (FPlatformTime::Seconds() < Until)
    {
        uint8 Bytes[2048]; int32 Count = 0;
        if (!Receiver->Recv(Bytes, sizeof(Bytes), Count)) { FPlatformProcess::Sleep(.001f); continue; }
        FACEBinaryReader R(Bytes, Count); R.Skip(20 + 14);
        TestEqual(TEXT("Recall retains weenie queue"), R.ReadUInt16(), ACEQueue::WeenieQueue);
        TestEqual(TEXT("Recall is a GameAction"), R.ReadUInt32(), 0xF7B1u);
        R.ReadUInt32();
        TestEqual(TEXT("Server selects the attuned lifestone (TeleToLifestone)"), R.ReadUInt32(), 0x63u);
        ++Recalls;
    }
    TestEqual(TEXT("Recovery sends exactly one recall across repeated frames"), Recalls, 1);
    FString Gate;
    TestFalse(TEXT("Original destination cannot reveal while recall is pending"), Controller->IsWorldTransitionReady(&Gate, true));
    TestEqual(TEXT("Recall has its own readiness gate"), Gate, FString(TEXT("lifestone-recall")));
    Controller->bPortalExitNotified = true;
    Controller->ResetWorldEntryDestination();
    TestFalse(TEXT("New server destination needs a fresh LoginComplete"), Controller->bPortalExitNotified);
    TestFalse(TEXT("Destination receipt finishes recall wait"), Controller->bAwaitingRecoveryDestination);
    TestEqual(TEXT("New destination has a fresh loading grace"), Controller->EnterWorldLoadElapsed, 0.f);
    TestTrue(TEXT("Destination reset preserves one-attempt limit"), Controller->bWorldEntryRecoveryAttempted);
    TestFalse(TEXT("Unrevealed destination does not start autonomous position reporting"), Session->bForcePositionReporting);
    Controller->EnterWorldLoadElapsed = 46;
    Controller->TickWorldEntryRecovery(.016f, TEXT("cell-collision"));
    TestEqual(TEXT("Unusable lifestone disconnects instead of repeating recall"), Session->State, EACESessionState::Disconnected);
    TestFalse(TEXT("Failed recovery leaves the endless loading state"), Controller->bEnterWorldLoading);
    TestFalse(TEXT("Failure has a user-readable explanation"), Controller->WorldEntryFailureReason.IsEmpty());
    Session->State = EACESessionState::InWorld;
    Controller->bAwaitingRecoveryDestination = true;
    Controller->WorldEntryRecoveryElapsed = 44.99f;
    Controller->TickWorldEntryRecovery(.02f, TEXT("lifestone-recall"));
    TestEqual(TEXT("Rejected or unanswered recall cannot wait forever"), Session->State, EACESessionState::Disconnected);
    TestTrue(TEXT("Unanswered recall explains attunement and server eligibility"), Controller->WorldEntryFailureReason.Contains(TEXT("attuned")));
    // A corrupt destination outside every room used to be misclassified as
    // unfinished collision forever, including on the next login.
    FACEPosition NoRoom;NoRoom.CellId=0x014F01B9;NoRoom.Location=FVector(3000,3000,-500);
    Session->SetLocalPosition(NoRoom);Session->State=EACESessionState::InWorld;
    Controller->bAwaitingRecoveryDestination=false;Controller->bWorldEntryRecoveryAttempted=false;
    Controller->ResolveWorldEntryDestinationCell();
    TestTrue(TEXT("Complete raw geometry search detects no containing room"),Controller->bWorldEntryPositionOutsideCells);
    Controller->EnterWorldLoadElapsed=46;
    TestTrue(TEXT("No-room arrival enters bounded recall recovery"),Controller->TickWorldEntryRecovery(.016f,TEXT("spawn-placement")));
    TestTrue(TEXT("No-room arrival requests one server recall"),Controller->bWorldEntryRecoveryAttempted&&Controller->bAwaitingRecoveryDestination);
    Controller->ResetWorldEntryDestination();
    TestFalse(TEXT("New destination clears the old invalid-position classification"),Controller->bWorldEntryPositionOutsideCells);
    Receiver->Close(); Sockets->DestroySocket(Receiver);
    Floor->Destroy(); Solid->Destroy();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailPortalSpaceTest, "ACE.RetailParity.PortalSpace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACERetailPortalSpaceTest::RunTest(const FString& Parameters)
{
    using ACERetailPortalAnimation::GetAnimLevel;
    for (auto Pair : {TPair<float,int32>(-.1f,0), {.1f,22}, {.25f,146}, {.5f,512}, {.75f,877}, {.9f,1001}, {1.1f,1024}})
        TestEqual(TEXT("Retail UIGlobals fixed-point curve sample"), GetAnimLevel(Pair.Key), Pair.Value);
    FEntryWorld Fixture;
    auto* Dat = Fixture.GI->GetSubsystem<UACEDatSubsystem>();
    if (!Dat || !Dat->LoadDatDirectory(ACETestDatDirectory())) return false;
    auto* Tunnel = Fixture.World->SpawnActor<AACELoadingScreenActor>();
    Tunnel->BeginTunnel();
    const double Deadline = FPlatformTime::Seconds() + 20;
    while (!Tunnel->TryBuildMesh() && FPlatformTime::Seconds() < Deadline)
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        FPlatformProcess::Sleep(.002f);
    }
    if (!TestTrue(TEXT("Retail portal setup builds"), Tunnel->bMeshReady)) return false;
    TestEqual(TEXT("Portal uses the DAT animation DID"), Tunnel->AnimationId, 0x030005AC);
    TestEqual(TEXT("Portal setup contains two animated parts"), Tunnel->Appearance->PartMeshes.Num(), 2);
    TestTrue(TEXT("Camera matches retail AC eye"), Tunnel->CameraBoom->GetRelativeLocation().Equals(FVector(-24,-270,88), .001));
    Tunnel->TeleportYaw = 90; Tunnel->ApplyPortalCamera();
    TestTrue(TEXT("Retail positive roll sends up toward converted AC +X"), Tunnel->Camera->GetUpVector().Equals(FVector(-1,0,0), .001));
    const FVector TestPoint(80, 120, 150);
    const FVector RetailViewPoint = Tunnel->Camera->GetComponentTransform().InverseTransformPosition(
        Tunnel->GetActorTransform().TransformPosition(TestPoint));
    Tunnel->SetTrackedView(true);
    const FTransform UnrolledView(FRotationMatrix::MakeFromXZ(FVector(0, 1, 0), FVector::UpVector).ToQuat(),
        Tunnel->Camera->GetComponentLocation());
    const FVector TrackedViewPoint = UnrolledView.InverseTransformPosition(
        Tunnel->Appearance->GetMeshRoot()->GetComponentTransform().TransformPosition(TestPoint));
    TestTrue(TEXT("Tracked portal rotates geometry to reproduce the retail view without rolling the HMD"),
        TrackedViewPoint.Equals(RetailViewPoint, .01));
    Tunnel->SetTrackedView(false);
    TestTrue(TEXT("Desktop portal restores the authored mesh frame"),
        Tunnel->Appearance->GetMeshRoot()->GetRelativeTransform().Equals(FTransform::Identity, .001));
    Tunnel->TeleportYawStart = 0; Tunnel->TeleportYawEnd = 300;
    Tunnel->TeleportYawElapsed = 0; Tunnel->TeleportYawDuration = 1;
    Tunnel->Tick(.25f);
    TestEqual(TEXT("Camera uses eased numeric interpolation, not shortest-angle interpolation"), Tunnel->TeleportYaw, 300.f * 146.f / 1024.f);
    for (int32 Visit = 0; Visit < 2; ++Visit)
    {
        Tunnel->BeginTunnel(); TestTrue(TEXT("Tunnel can be reused"), Tunnel->TryBuildMesh());
        TestFalse(TEXT("Tunnel actor is the sole animation clock"), Tunnel->Appearance->IsComponentTickEnabled());
        int32 Changed = 0;
        FTransform Previous = Tunnel->Appearance->PartMeshes[0]->GetRelativeTransform();
        FTransform PreviousSecond = Tunnel->Appearance->PartMeshes[1]->GetRelativeTransform();
        for (int32 Frame = 0; Frame < 120; ++Frame)
        {
            Tunnel->Tick(1.f / 120.f);
            const auto Current = Tunnel->Appearance->PartMeshes[0]->GetRelativeTransform();
            const auto Second = Tunnel->Appearance->PartMeshes[1]->GetRelativeTransform();
            if (!Current.Equals(Previous, .0001) || !Second.Equals(PreviousSecond, .0001)) ++Changed;
            Previous = Current;
            PreviousSecond = Second;
        }
        AddInfo(FString::Printf(TEXT("Tunnel visit %d: %d distinct displayed poses over 120 frames"), Visit, Changed));
        TestTrue(TEXT("Forty-fps source keys interpolate on display frames"), Changed > 100);
        TestTrue(TEXT("One second of display time advances animation once"), FMath::IsNearlyEqual(Tunnel->Appearance->AnimTime, 1.f, .001f));
        TestFalse(TEXT("Portal exit stretches the tunnel before showing the world"), Tunnel->TickReveal(.016f, FVector::ZeroVector));
        const float InitialFov = Tunnel->Camera->FieldOfView;
        TestFalse(TEXT("Halfway through tunnel stretch remains in portal view"), Tunnel->TickReveal(.484f, FVector::ZeroVector));
        TestEqual(TEXT("Exit changes the world projection, never the portal camera"), Tunnel->Camera->FieldOfView, InitialFov);
        TestTrue(TEXT("One-second tunnel stretch completes"), Tunnel->TickReveal(.5f, FVector::ZeroVector));
        TestTrue(TEXT("World stretch starts wide"), ACERetailPortalAnimation::StretchFov(90.f, 4.f/3.f, 0.f, true) > 170.f);
        TestTrue(TEXT("World stretch restores normal projection"), FMath::IsNearlyEqual(ACERetailPortalAnimation::StretchFov(90.f, 4.f/3.f, 1.f, true), 90.f, .001f));
    }
    Tunnel->BeginTunnel(); Tunnel->TryBuildMesh();
    Tunnel->TeleportYawStart = 0; Tunnel->TeleportYawEnd = 180;
    Tunnel->TeleportYawElapsed = 0; Tunnel->TeleportYawDuration = 1.2f;
    if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
    auto* Target = NewObject<UTextureRenderTarget2D>(Tunnel);
    Target->ClearColor = FLinearColor::Black; Target->InitCustomFormat(640,480,PF_B8G8R8A8,false);
    auto* Capture = NewObject<USceneCaptureComponent2D>(Tunnel);
    Capture->TextureTarget = Target; Capture->CaptureSource = SCS_FinalColorLDR;
    Capture->FOVAngle = Tunnel->Camera->FieldOfView;
    Capture->bCaptureEveryFrame = false; Capture->bCaptureOnMovement = false;
    Capture->PostProcessSettings = Tunnel->Camera->PostProcessSettings;
    Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetAtmosphere(false);
    Capture->RegisterComponent();
    for (int32 Frame = 0; Frame < 3; ++Frame)
    {
        Tunnel->Tick(Frame == 0 ? 0.f : .3f);
        Capture->SetWorldTransform(Tunnel->Camera->GetComponentTransform());
        Fixture.World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
        TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        int32 Visible = 0;
        for (const auto& Pixel : Pixels) if (FMath::Max3(Pixel.R,Pixel.G,Pixel.B) > 20) ++Visible;
        TestTrue(TEXT("Retail camera sees textured tunnel geometry"), Visible > Pixels.Num() / 10);
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(640,480,Pixels,PNG);
        const FString Path = FPaths::ProjectSavedDir() / TEXT("Automation/RetailParity") / FString::Printf(TEXT("PortalSpace%d.png"),Frame);
        TestTrue(TEXT("Portal frame capture saved"), FFileHelper::SaveArrayToFile(PNG, *Path));
    }
    return true;
}
#endif
