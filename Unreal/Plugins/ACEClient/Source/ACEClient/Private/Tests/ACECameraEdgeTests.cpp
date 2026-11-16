#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACECameraRetail.h"
#include "ACEOrbitCameraBoom.h"
#include "ACECameraSettings.h"
#include "ACETerrainPresenterComponent.h"
#include "ACELandblockActor.h"
#include "ACERuntimeOptions.h"
#include "ACERetailPortalAnimation.h"
#include "ACELoadingScreenActor.h"
#include "ACEKeyboardRouter.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "ACELedgeSlide.h"
#include "ACEPlayerController.h"
#include "ACEInputBindings.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "GameFramework/Pawn.h"
#include "Engine/GameInstance.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECameraEdgeTest,"ACE.RetailParity.CameraAndEdges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECameraEdgeTest::RunTest(const FString& Parameters)
{
    // Use a separate settings file: this test must not change player preferences.
    ON_SCOPE_EXIT { ACEInputBindings::Reload(); };
    TGuardValue<FString> SettingsPath(GGameUserSettingsIni,FPaths::ProjectSavedDir()/TEXT("Automation/CameraFixture.ini"));
    FConfigFile FixtureConfig; FixtureConfig.NoSave=false; FixtureConfig.bCanSaveAllSections=true;
    GConfig->SetFile(GGameUserSettingsIni,&FixtureConfig);
    ACEInputBindings::Reload();
    // Numeric reference projections from SmartBox::GetOverrideFovDistance
    // and Render's vertical perspective, before Unreal's horizontal conversion.
    TestEqual(TEXT("Retail default keyboard orbit is angle(8) times adjustment(40)"),ACECameraRetail::NumpadOrbitDegreesPerSecond,320.f);
    TestEqual(TEXT("Retail mouse-turning preference quarters keyboard pitch"),ACECameraRetail::KeyboardPitchRate(1,true),80.f);
    TestEqual(TEXT("Camera adjustment slider scales keyboard pitch"),ACECameraRetail::KeyboardPitchRate(.25f,false),80.f);
    TestTrue(TEXT("Retail 4:3 60-degree preference"),FMath::IsNearlyEqual(ACECameraRetail::HorizontalFovDegrees(4.f/3,60),62.15513f,.0001f));
    TestTrue(TEXT("Retail 16:9 default projection"),FMath::IsNearlyEqual(ACECameraRetail::HorizontalFovDegrees(16.f/9,90),83.90130f,.0001f));
    TestTrue(TEXT("Retail ultrawide 120-degree preference"),FMath::IsNearlyEqual(ACECameraRetail::HorizontalFovDegrees(21.f/9,120),99.53647f,.0001f));
    for(float Aspect : {4.f/3,16.f/9,21.f/9}) for(float Degrees : {10.f,60.f,90.f,120.f,160.f})
    {
        const float Lens=ACECameraRetail::HorizontalFovDegrees(Aspect,Degrees);
        TestTrue(TEXT("Portal lens blend ends at the configured FOV"),FMath::IsNearlyEqual(ACERetailPortalAnimation::StretchFov(Lens,Aspect,1.f,true),Lens,.001f));
        TestTrue(TEXT("Portal lens blend is monotonic"),ACERetailPortalAnimation::StretchFov(Lens,Aspect,.25f,true)>=ACERetailPortalAnimation::StretchFov(Lens,Aspect,.75f,true));
    }
    GConfig->SetFloat(TEXT("ACE.Presentation"),TEXT("FieldOfView"),1.2f,GGameUserSettingsIni);
    TestEqual(TEXT("Old FOV multiplier migrates to degrees"),ACERuntimeOptions::Get(TEXT("FieldOfViewDegrees")),108.f,.001f);
    ACERuntimeOptions::Set(TEXT("FieldOfViewDegrees"),90.f);
    TestEqual(TEXT("Unset mouse speed uses the faster default"),ACECameraSettings::GetMouseDegreesPerPixel(),0.75f);
    const uint32 Scans[]={0x52,0x4F,0x50,0x51,0x4B,0x4C,0x4D,0x47,0x48,0x49};
    TestEqual(TEXT("Keypad Enter is distinct from chat Enter"),FACEKeyboardRouter::NumpadVirtualKey(0x0D,0x1C,true),uint32(0x1000D));
    TestEqual(TEXT("Main Enter remains Enter"),FACEKeyboardRouter::NumpadVirtualKey(0x0D,0x1C,false),uint32(0x0D));
    const uint32 Navigation[]={0x2D,0x23,0x28,0x22,0x25,0x0C,0x27,0x24,0x26,0x21};
    for (int32 N=0;N<10;++N)
    {
        TestEqual(TEXT("Num Lock off still resolves the physical numpad key"),FACEKeyboardRouter::NumpadVirtualKey(Navigation[N],Scans[N],false),uint32(0x60+N));
        TestEqual(TEXT("Dedicated navigation keys remain unchanged"),FACEKeyboardRouter::NumpadVirtualKey(Navigation[N],Scans[N],true),Navigation[N]);
        TestEqual(TEXT("Num Lock on keeps the same numpad key"),FACEKeyboardRouter::NumpadVirtualKey(0x60+N,Scans[N],false),uint32(0x60+N));
    }
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    {
        auto* Camera=NewObject<UCameraComponent>();
        ACECameraRetail::ApplyFovToCamera(Camera,1920,1080,120.f);
        TestEqual(TEXT("Camera component uses the converted preference"),Camera->FieldOfView,
            ACECameraRetail::HorizontalFovDegrees(16.f/9,120.f));
        TestTrue(TEXT("Projection keeps the calculated horizontal FOV"),Camera->bOverrideAspectRatioAxisConstraint
            && Camera->AspectRatioAxisConstraint==AspectRatio_MaintainXFOV && !Camera->bConstrainAspectRatio);
        for (const FIntPoint Size : {FIntPoint(2048,1536),FIntPoint(1920,1080),FIntPoint(3440,1440)})
        {
            ACECameraRetail::ApplyFovToCamera(Camera,Size.X,Size.Y,120.f,true);
            TestEqual(TEXT("High sky view keeps the retail 120-degree preference across window shapes"),Camera->FieldOfView,
                ACECameraRetail::HorizontalFovDegrees(float(Size.X)/Size.Y,120.f));
            TestFalse(TEXT("Sky view does not compound map distance with wide-FOV LOD degradation"),Camera->bUseFieldOfViewForLOD);
            ACECameraRetail::ApplyFovToCamera(Camera,Size.X,Size.Y,120.f,false);
            TestTrue(TEXT("Leaving sky view restores ordinary FOV-based LOD"),Camera->bUseFieldOfViewForLOD);
        }
        ACERuntimeOptions::Set(TEXT("FieldOfViewDegrees"),110.f);
        auto* Portal=World->SpawnActor<AACELoadingScreenActor>();
        TestEqual(TEXT("Portal tunnel uses the configured world lens"),Portal->Camera->FieldOfView,
            ACECameraRetail::HorizontalFovDegrees(16.f/9,110.f));
        Portal->Destroy();
        ACERuntimeOptions::Set(TEXT("FieldOfViewDegrees"),90.f);
    }
    auto* Controller=World->SpawnActor<AACEPlayerController>();
    World->AddController(Controller); // Fixture worlds do not run normal BeginPlay registration.
    if (FParse::Param(FCommandLine::Get(),TEXT("RenderOffScreen")))
        TestFalse(TEXT("Offscreen clients do not install a native keyboard handler"),
            FACEKeyboardRouter::Create(Controller,[]{return true;}).IsValid());
    auto* GI=NewObject<UGameInstance>();
    auto* Client=NewObject<UACEClientSubsystem>(GI);
    Client->Session=MakeShared<FACESession>(); Client->Session->State=EACESessionState::InWorld;
    Controller->Client=Client; Controller->bRetailCursorInstalled=true;
    {
        auto& Session=*Client->Session;
        TGuardValue<int32> Self(Session.PlayerGuid,1);
        TGuardValue<FACEPosition> Position(Session.PlayerPosition,FACEPosition{});
        TGuardValue<FACESelectedObject> Selection(Session.SelectedObject,FACESelectedObject{});
        ON_SCOPE_EXIT { Session.WorldObjects.Reset(); };
        for (const uint32 Cell : {0xDA55001Du,0x0143014Fu})
        {
            Session.WorldObjects.Reset();
            Session.PlayerPosition.CellId=Cell;
            Session.PlayerPosition.Location=FVector(100,100,20);
            const double Range=Cell==0xDA55001D?75.:25.;
            auto Add=[&](int32 Guid,double X,double Z=0.)
            {
                FACEWorldObject Obj; Obj.Guid=Guid; Obj.ItemType=ACEItemType::Creature;
                Obj.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable; Obj.bHasPosition=true;
                Obj.Position=Session.PlayerPosition; Obj.Position.Location+=FVector(X,0,Z);
                Session.WorldObjects.Add(Guid,Obj);
            };
            Add(2,10); Add(3,Range); Add(4,Range+.1); Add(5,Range*2);
            TestEqual(TEXT("Nearest targeting ignores retained distant monsters"),Controller->FindNearbyTarget(true),2);
            Client->SelectObject(2);
            TestEqual(TEXT("Next includes exact retail range boundary"),Controller->FindNearbyTarget(true,1),3);
            Client->SelectObject(3);
            TestEqual(TEXT("Next wraps instead of reaching cached monsters beyond range"),Controller->FindNearbyTarget(true,1),2);
            Client->SelectObject(2);
            TestEqual(TEXT("Previous wraps within the same range"),Controller->FindNearbyTarget(true,-1),3);
            // Keep every previously seen object in the cache, as in the report.
            Session.PlayerPosition.Location.X-=Range*2;
            for (int32 Direction : {-1,0,1})
                TestEqual(TEXT("Walking away makes all cached targets ineligible immediately"),Controller->FindNearbyTarget(true,Direction),0);
            Session.PlayerPosition.Location.X+=Range*2;
            TestEqual(TEXT("Returning restores nearby targets without another spawn packet"),Controller->FindNearbyTarget(true),2);
            // Range is planar, while height still contributes to ordering.
            Session.WorldObjects.Reset(); Add(2,10,100); Add(3,20);
            TestEqual(TEXT("Retail height weighting orders targets inside planar range"),Controller->FindNearbyTarget(true),3);
          TestEqual(TEXT("Height alone does not impose a spherical cutoff"),Controller->FindNearbyTarget(true,0,3),2);
          // Retail monster selection includes hostile players, notably playable
          // Olthoi. It must not blindly include protected humans or dead players.
          Session.WorldObjects.Reset();Add(2,10);Add(3,20);
          Session.WorldObjects[2].bIsPlayer=true;
          Session.WorldObjects[2].ObjectDescriptionFlags=ACEObjectDescFlag::FreePkStatus;
          TestEqual(TEXT("Nearest monster includes Free-PK Olthoi"),Controller->FindNearbyTarget(true),2);
          Session.WorldObjects[2].ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
          TestEqual(TEXT("NPK player is excluded despite attackable descriptor"),Controller->FindNearbyTarget(true),3);
          Add(1,0);Session.WorldObjects[1].bIsPlayer=true;
          Session.WorldObjects[1].ObjectDescriptionFlags=ACEObjectDescFlag::PlayerKiller;
          Session.WorldObjects[2].ObjectDescriptionFlags=ACEObjectDescFlag::PlayerKiller;
          TestEqual(TEXT("PK Olthoi can cycle to a PK player"),Controller->FindNearbyTarget(true),2);
          Session.WorldObjects[2].bDying=true;
          TestEqual(TEXT("Dead PK player cannot retain auto-target"),Controller->FindNearbyTarget(true),3);
        }
        Session.PlayerPosition.CellId=0xDA550001; Session.PlayerPosition.Location=FVector(190,10,0);
        Session.WorldObjects.Reset();
        FACEWorldObject Across; Across.Guid=2; Across.ItemType=ACEItemType::Creature;
        Across.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable; Across.bHasPosition=true;
        Across.Position.CellId=0xDB550001; Across.Position.Location=FVector(2,10,0);
        Session.WorldObjects.Add(2,Across);
        TestEqual(TEXT("Adjacent landblock uses global distance rather than local coordinates"),Controller->FindNearbyTarget(true),2);
        Session.PlayerPosition.CellId=0xDA550100;
        Session.PlayerPosition.Location.X=150;
        TestEqual(TEXT("Entering an indoor cell immediately reduces selection range"),Controller->FindNearbyTarget(true),0);
    }
    TestTrue(TEXT("Mouse turning defaults on with fresh local settings and retail's off bit"),Client->IsCharacterOptionSet(0x31));
    const uint32 InitialOptions2=Client->GetCharacterOptions2();
    Client->SendSetSingleCharacterOption(0x31,false);
    FConfigFile SavedTurning;SavedTurning.Read(GGameUserSettingsIni);
    bool SavedTurningValue=true;SavedTurning.GetBool(TEXT("ACE.Camera"),TEXT("UseMouseTurning"),SavedTurningValue);
    TestFalse(TEXT("An explicit mouse-turning opt-out is saved"),SavedTurningValue);
    GConfig->SetFile(GGameUserSettingsIni,&SavedTurning);
    TestFalse(TEXT("The opt-out survives settings reload"),Client->IsCharacterOptionSet(0x31));
    Client->SendCharacterOptions(Client->GetCharacterOptions1(),InitialOptions2);
    TestTrue(TEXT("Resetting an options snapshot restores mouse turning too"),Client->IsCharacterOptionSet(0x31));
    Client->SendSetSingleCharacterOption(0x31,false);
    auto* Pawn=World->SpawnActor<APawn>();
    auto* Boom=NewObject<UACEOrbitCameraBoom>(Pawn);
    Pawn->SetRootComponent(Boom); Boom->RegisterComponent(); Controller->Possess(Pawn);
    Boom->TargetArmLength=300.f;
    Controller->UpdateMouseButtons(true,false,false,false);
    TestFalse(TEXT("Mouse turning OFF never starts orbit or capture"),Controller->bMouseLookActive);
    Controller->ApplyMouseLookDelta(100,100,Boom);
    TestTrue(TEXT("Mouse turning OFF ignores orbit deltas"),Boom->GetRelativeRotation().IsZero());
    TestTrue(TEXT("Right click still identifies when mouse turning is OFF"),Controller->UpdateMouseButtons(false,false,false,false));
    Controller->bInstantMouseLookHeld=true;
    Controller->UpdateMouseButtons(true,false,false,false);
    TestTrue(TEXT("Bound instant mouse look works with the ordinary mouse-turn option off"),Controller->bMouseLookActive);
    Controller->ApplyMouseLookDelta(2,0,Boom);
    TestTrue(TEXT("Instant mouse look applies camera motion"),FMath::Abs(Boom->GetRelativeRotation().Yaw)>1.f);
    Controller->bInstantMouseLookHeld=false;
    Controller->UpdateMouseButtons(false,false,false,false);
    TestFalse(TEXT("Releasing instant mouse look releases capture"),Controller->bMouseLookActive);
    Boom->SetRelativeRotation(FRotator::ZeroRotator);
    Client->SendSetSingleCharacterOption(0x31,true);
    Controller->UpdateMouseButtons(false,true,false,false);
    TestFalse(TEXT("Left mouse alone does not move the player or capture selection clicks"),Controller->bMouseForwardActive || Controller->bMouseLookActive);
    Controller->UpdateMouseButtons(true,false,false,true);
    TestFalse(TEXT("A right press over the UI cannot start orbit"),Controller->bMouseLookActive);
    Controller->UpdateMouseButtons(true,true,false,false);
    TestFalse(TEXT("Dragging from the UI into the world cannot start mouse movement"),Controller->bMouseForwardActive);
    TestFalse(TEXT("Releasing a UI gesture does not identify a world object"),Controller->UpdateMouseButtons(false,false,false,false));
    Controller->UpdateMouseButtons(true,false,false,false);
    TestTrue(TEXT("Enabled right mouse starts captured orbit"),Controller->bMouseLookActive && Controller->bMouseLookUsesCapture);
    Controller->PlayerInput=NewObject<UPlayerInput>(Controller);
    ACEInputBindings::BeginEdit();ACEInputBindings::Set(ACEInputBindings::Action(TEXT("ClosestMonster")),0,FInputChord(EKeys::Tab));ACEInputBindings::Commit();
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Pressed,1.f));
    Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
    TestTrue(TEXT("A rebound Tab targeting key is consumed before Slate focus navigation"),Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Tab,IE_Pressed,1.f)));
    Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
    TestTrue(TEXT("Targeting preserves held movement and mouse look"),Controller->IsInputKeyDown(EKeys::W) && Controller->bMouseLookActive && !Controller->bShowMouseCursor);
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Tab,IE_Released,0.f));Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Released,0.f));
    ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
    Boom->SetRelativeRotation(FRotator(-15,90,0));
    Controller->ApplyMouseLookDelta(2,0,Boom);
    TestTrue(TEXT("Default mouse speed turns 1.5 degrees for two pixels"),FMath::IsNearlyEqual(Boom->GetRelativeRotation().Yaw,91.5,0.001));
    Controller->ApplyMouseLookDelta(0,0,Boom);
    Controller->ApplyMouseLookDelta(2,0,Boom);
    TestTrue(TEXT("Pausing between short movements does not reintroduce a dead zone"),Boom->GetRelativeRotation().Yaw>90.8);
    Controller->ApplyMouseLookDelta(0,20,Boom);
    TestTrue(TEXT("Moving the mouse down tilts the view down"),Boom->GetRelativeRotation().Pitch<-19);
    ACECameraSettings::SetMouseTurnSpeed(2.f);
    Boom->SetRelativeRotation(FRotator(-15,90,0));
    Controller->ApplyMouseLookDelta(1,10,Boom);
    TestTrue(TEXT("Changing speed immediately scales yaw below the turn limit"),FMath::IsNearlyEqual(Boom->GetRelativeRotation().Yaw,91.5,0.001));
    TestTrue(TEXT("Changing speed immediately scales pitch"),FMath::IsNearlyEqual(Boom->GetRelativeRotation().Pitch,-30.0,0.001));
    FConfigFile SavedCamera; SavedCamera.Read(GGameUserSettingsIni);
    float SavedSpeed=0; SavedCamera.GetFloat(TEXT("ACE.Camera"),TEXT("MouseTurnSpeed"),SavedSpeed);
    TestEqual(TEXT("Mouse speed is written to disk"),SavedSpeed,2.f);
    GConfig->SetFile(GGameUserSettingsIni,&SavedCamera);
    TestEqual(TEXT("Mouse speed survives a settings reload"),ACECameraSettings::GetMouseTurnSpeed(),2.f);
    ACECameraSettings::SetMouseTurnSpeed(999.f);
    TestEqual(TEXT("Mouse speed has a safe upper bound"),ACECameraSettings::GetMouseTurnSpeed(),3.f);
    ACECameraSettings::SetMouseTurnSpeed(-1.f);
    TestEqual(TEXT("Mouse speed has a positive lower bound"),ACECameraSettings::GetMouseTurnSpeed(),0.1f);
    ACECameraSettings::SetMouseTurnSpeed(ACECameraSettings::DefaultMouseTurnSpeed);
    for(bool X:{false,true}) for(bool Y:{false,true})
    {
        ACECameraSettings::SetMouseInversion(X,Y);
        Boom->SetRelativeRotation(FRotator(-20,0,0));
        Controller->ApplyMouseLookDelta(2,8,Boom);
        TestTrue(TEXT("Mouse horizontal inversion is independent"),FMath::IsNearlyEqual(Boom->GetRelativeRotation().Yaw,X?-1.5:1.5,.01));
        TestTrue(TEXT("Mouse vertical inversion is independent"),FMath::IsNearlyEqual(Boom->GetRelativeRotation().Pitch,Y?-14.:-26.,.01));
        FConfigFile Reload;Reload.Read(GGameUserSettingsIni);bool SavedX=false,SavedY=false;
        Reload.GetBool(TEXT("ACE.Camera"),TEXT("InvertMouseX"),SavedX);Reload.GetBool(TEXT("ACE.Camera"),TEXT("InvertMouseY"),SavedY);
        TestTrue(TEXT("Both axis preferences persist independently"),SavedX==X && SavedY==Y);
    }
    ACECameraSettings::SetMouseInversion(false,false);
    Controller->ApplyMouseLookDelta(0,10000,Boom);
    TestTrue(TEXT("Orbit pitch remains clamped"),FMath::IsNearlyEqual(Boom->GetRelativeRotation().Pitch,-89.0,0.01));
    Controller->UpdateMouseButtons(true,true,false,false);
    TestTrue(TEXT("Both buttons advance the player"),Controller->bMouseForwardActive);
    Controller->UpdateMouseButtons(true,false,false,false);
    TestFalse(TEXT("Releasing left stops advancing while right continues orbit"),Controller->bMouseForwardActive);
    TestTrue(TEXT("Releasing left leaves orbit active"),Controller->bMouseLookActive);
    TestFalse(TEXT("Releasing a movement gesture never identifies"),Controller->UpdateMouseButtons(false,false,false,false));
    Controller->UpdateMouseButtons(true,true,false,false);
    TestFalse(TEXT("Releasing right first also never identifies"),Controller->UpdateMouseButtons(false,true,false,false));
    TestFalse(TEXT("Releasing right stops mouse movement"),Controller->bMouseForwardActive);
    TestTrue(TEXT("Releasing right keeps held left mouse in free orbit"),Controller->bMouseLookActive && !Controller->bMouseForwardActive);
    Controller->UpdateMouseButtons(false,false,false,false);
    TestTrue(TEXT("Releasing both orbit buttons restores the cursor"),Controller->bShowMouseCursor && !Controller->bMouseLookUsesCapture);
    Controller->UpdateMouseButtons(true,true,false,false);
    Client->SendSetSingleCharacterOption(0x31,false);
    Controller->UpdateMouseButtons(true,true,false,false);
    TestFalse(TEXT("Disabling the option cancels active orbit and forward movement"),Controller->bMouseLookActive || Controller->bMouseForwardActive);
    Client->SendSetSingleCharacterOption(0x31,true);
    Controller->UpdateMouseButtons(true,true,false,false);
    TestFalse(TEXT("Re-enabling while held requires a fresh world press"),Controller->bMouseLookActive);
    TestFalse(TEXT("A cancelled gesture cannot become an inspect click"),Controller->UpdateMouseButtons(false,false,false,false));
    Controller->UpdateMouseButtons(true,true,false,false);
    Controller->UpdateMouseButtons(true,true,true,false);
    TestFalse(TEXT("Chat/keybind focus cancels mouse controls"),Controller->bMouseLookActive || Controller->bMouseForwardActive);
    Controller->UpdateMouseButtons(false,false,false,false);
    Controller->UpdateMouseButtons(true,false,false,false);
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Pressed,1.f));
    Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
    TestTrue(TEXT("An ordinary enabled RMB click still identifies"),Controller->UpdateMouseButtons(false,false,false,false));
    TestTrue(TEXT("Releasing an inspect click preserves held keyboard movement"),Controller->IsInputKeyDown(EKeys::W));
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Released,0.f));
    Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
    // Exercise wheel math without depending on the physical desktop cursor:
    // headless automation can run with that cursor over the editor's own UI.
    if (Controller->IsMouseOverBlockingUI())
    {
        Controller->ApplyCameraWheelZoom(1);
        TestEqual(TEXT("Wheel over an interface leaves camera distance alone"),Boom->TargetArmLength,300.f);
    }
    for (bool bMouseTurning : {false,true})
    {
        Client->SendSetSingleCharacterOption(0x31,bMouseTurning);
        Controller->AdjustMouseCameraDistance(1);
        const float ZoomedIn=Controller->UserCameraArmLength;
        TestTrue(TEXT("Wheel zooms in with mouse turning either OFF or ON"),ZoomedIn<300.f);
        Controller->AdjustMouseCameraDistance(-1);
        TestTrue(TEXT("Wheel zooms out with mouse turning either OFF or ON"),Controller->UserCameraArmLength>ZoomedIn);
        const float BeforeZero=Boom->TargetArmLength;
        Controller->AdjustMouseCameraDistance(0);
        TestEqual(TEXT("Zero wheel input does not move the camera"),Boom->TargetArmLength,BeforeZero);
        ACEInputBindings::BeginEdit();
        Controller->ApplyCameraWheelZoom(1);
        TestEqual(TEXT("Key binding focus still blocks zoom in both turning modes"),Boom->TargetArmLength,BeforeZero);
        ACEInputBindings::Cancel();
    }
    // With Alt+Z the canvas no longer receives wheel events. Reproduce the
    // viewport's pressed/released/axis sequence, including fractional wheels.
    // Keep the physical cursor clear of the test host's separate launcher UI.
    const FVector2D SavedWheelCursor=FSlateApplication::Get().GetCursorPos();
    FSlateApplication::Get().SetCursorPos(FVector2D(-100000,-100000));
    Controller->SetDesktopInterfaceHidden(true);
    TestFalse(TEXT("Hidden HUD wheel fixture has no blocking UI under the cursor"),Controller->IsMouseOverBlockingUI());
    TestTrue(TEXT("Wheel regression runs with the desktop interface hidden"),Controller->IsDesktopInterfaceHidden());
    Controller->CameraInputFrameSeconds=1.f/60;
    for (float Delta : {1.f,-1.f,.5f,-.5f})
    {
        Boom->TargetArmLength=Controller->UserCameraArmLength=300.f;
        const FKey WheelKey=Delta>0.f?EKeys::MouseScrollUp:EKeys::MouseScrollDown;
        Controller->InputKey(FInputKeyEventArgs::CreateSimulated(WheelKey,IE_Pressed,1.f));
        Controller->InputKey(FInputKeyEventArgs::CreateSimulated(WheelKey,IE_Released,1.f));
        TestEqual(TEXT("Wheel key events do not apply a second zoom step"),Controller->UserCameraArmLength,300.f);
        TestTrue(TEXT("Hidden HUD viewport wheel event is handled"),Controller->InputKey(
            FInputKeyEventArgs::CreateSimulated(EKeys::MouseWheelAxis,IE_Axis,Delta)));
        TestTrue(TEXT("Hidden HUD wheel zoom preserves direction and fractional distance"),
            FMath::IsNearlyEqual(Controller->UserCameraArmLength,
                300.f*FMath::Pow(1.f+(Delta>0.f?-1.f:1.f)*8.f/60,FMath::Abs(Delta)),.001f));
    }
    const float BeforeEditing=Controller->UserCameraArmLength;
    ACEInputBindings::BeginEdit();
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseWheelAxis,IE_Axis,1.f));
    TestEqual(TEXT("Viewport wheel does not zoom while editing bindings"),Controller->UserCameraArmLength,BeforeEditing);
    ACEInputBindings::Set(EKeys::Add,1,FInputChord(EKeys::MouseScrollDown));
    ACEInputBindings::Set(EKeys::Subtract,1,FInputChord(EKeys::MouseScrollUp));
    ACEInputBindings::Commit();
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseWheelAxis,IE_Axis,1.f));
    TestTrue(TEXT("Hidden HUD wheel respects rebound zoom direction"),Controller->UserCameraArmLength>BeforeEditing);
    ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();
    const FKey WheelAction=ACEInputBindings::Action(TEXT("ClosestMonster"));
    ACEInputBindings::Set(WheelAction,0,FInputChord(EKeys::MouseScrollUp));ACEInputBindings::Commit();
    const float BeforeHotkey=Controller->UserCameraArmLength;
    Controller->SetDesktopInterfaceHidden(false);
    Controller->PlayerInput->FlushPressedKeys();
    Controller->ApplyCameraWheelZoom(1.f,true);
    Controller->PlayerInput->ProcessInputStack({},.016f,false);
    TestTrue(TEXT("Visible canvas wheel is forwarded to gameplay hotkeys"),ACEInputBindings::Pressed(Controller,WheelAction));
    TestEqual(TEXT("Rebound visible wheel does not zoom"),Controller->UserCameraArmLength,BeforeHotkey);
    Controller->SetDesktopInterfaceHidden(true);Controller->PlayerInput->FlushPressedKeys();
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseScrollUp,IE_Pressed,1.f));
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseScrollUp,IE_Released,0.f));
    Controller->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::MouseWheelAxis,IE_Axis,1.f));
    Controller->PlayerInput->ProcessInputStack({},.016f,false);
    TestTrue(TEXT("Hidden HUD wheel still reaches gameplay hotkeys"),ACEInputBindings::Pressed(Controller,WheelAction));
    TestEqual(TEXT("Rebound hidden wheel does not zoom"),Controller->UserCameraArmLength,BeforeHotkey);
    ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
    Controller->SetDesktopInterfaceHidden(false);
    FSlateApplication::Get().SetCursorPos(SavedWheelCursor);
    Controller->ResetCameraToRetailDefaults(Boom);
    Controller->bHavePredictedPose=true;
    Controller->PredictedPose.CellId=0x7D640014;
    const float SavedArm = Boom->TargetArmLength;
    const FRotator SavedRotation = Boom->GetRelativeRotation();
    Controller->SetCameraMapMode(Boom,true);
    TestEqual(TEXT("Retail map camera is 450 game units away"),Boom->TargetArmLength,450.f*Controller->GetCameraScaleCm());
    TestTrue(TEXT("Map camera looks down with retail's direction"),FMath::IsNearlyEqual(Boom->GetRelativeRotation().Pitch,ACECameraRetail::LookDownPitchDegrees(),.01));
    TestFalse(TEXT("Map camera is not pulled into intervening roofs"),Boom->bDoCollisionTest);
    auto* Terrain=NewObject<UACETerrainPresenterComponent>(Pawn);
    auto* Scenery=World->SpawnActor<AACELandblockActor>();
    Terrain->Spawned.Add(0x7D650000,Scenery);
    Scenery->ApplyDegradeCull(57000.f);
    Terrain->TickDegradeController(.016f);
    TestEqual(TEXT("Sky mode disables the resident scenery fade/cull used by ordinary views"),Scenery->AppliedDegradeEndCullCm,0.f);
    Controller->SyncUserCameraArmLength(Boom);
    TestEqual(TEXT("Following ticks cannot clamp map mode to ordinary zoom"),Boom->TargetArmLength,450.f*Controller->GetCameraScaleCm());
    Controller->SetCameraMapMode(Boom,false);
    Terrain->TickDegradeController(.016f);
    TestTrue(TEXT("Exiting sky view immediately restores normal scenery distance policy"),Scenery->AppliedDegradeEndCullCm>0.f);
    Terrain->Spawned.Reset();Scenery->Destroy();
    TestEqual(TEXT("Exiting map mode restores manual camera distance"),Boom->TargetArmLength,SavedArm);
    TestTrue(TEXT("Exiting map mode restores angle and collision"),Boom->GetRelativeRotation().Equals(SavedRotation,.01) && Boom->bDoCollisionTest);
    Controller->SetCameraMapMode(Boom,true);
    Controller->ResetCameraToRetailDefaults(Boom);
    TestFalse(TEXT("Reset camera leaves map mode"),Controller->bCameraMapMode);

    // Exercise the actual keyboard polling and spring-arm socket, rather than
    // checking a pitch constant alone. Holding Lower used to orbit the map
    // camera thousands of units below the player with collision disabled.
    Controller->PlayerInput=NewObject<UPlayerInput>(Controller);
    const FKey MapKey=ACEInputBindings::Get(ACEInputBindings::Action(TEXT("CameraViewMapMode")),0).Key;
    for (uint32 Cell : {0u,0x7D640100u,0x01430171u})
    {
        Controller->PredictedPose.CellId=Cell;
        Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(MapKey,IE_Pressed,1.f));
        Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
        Controller->UpdateOrbitCamera(1.f/60);
        TestFalse(TEXT("Keypad Enter cannot put an indoor or unknown cell into exterior map mode"),Controller->bCameraMapMode);
        TestTrue(TEXT("Rejected indoor map request preserves ordinary distance and collision"),
            FMath::IsNearlyEqual(Boom->TargetArmLength,SavedArm,.01f) && Boom->bDoCollisionTest);
        Controller->PlayerInput->FlushPressedKeys();
        Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
        Controller->UpdateOrbitCamera(1.f/60);
    }
    Controller->PredictedPose.CellId=0x7D640014;
    Controller->SetCameraMapMode(Boom,true);
    Controller->PredictedPose.CellId=0x7D640100;
    Controller->UpdateOrbitCamera(1.f/60);
    TestFalse(TEXT("Entering a building immediately exits the exterior camera view"),Controller->bCameraMapMode);
    TestTrue(TEXT("Indoor transition restores the saved ordinary camera"),
        FMath::IsNearlyEqual(Boom->TargetArmLength,SavedArm,.01f)
        && Boom->GetRelativeRotation().Equals(SavedRotation,.01) && Boom->bDoCollisionTest);
    Controller->PredictedPose.CellId=0x7D640014;
    for (const int32 FPS : {30,90})
    {
        Controller->ResetCameraToRetailDefaults(Boom);
        Boom->SetWorldLocation(FVector(0,0,1200));
        Controller->SetCameraMapMode(Boom,true);
        const float MapArm=Boom->TargetArmLength;
        Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::NumPadTwo,IE_Pressed,1.f));
        double MinimumHeight=TNumericLimits<double>::Max();
        for (int32 Frame=0;Frame<FPS*50;++Frame)
        {
            Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
            Controller->UpdateOrbitCamera(1.f/FPS);
            Boom->TickComponent(1.f/FPS,LEVELTICK_All,nullptr);
            MinimumHeight=FMath::Min(MinimumHeight,
                Boom->GetSocketLocation(USpringArmComponent::SocketName).Z-Boom->GetComponentLocation().Z);
        }
        TestTrue(TEXT("Holding keypad Lower keeps the overhead camera above its pivot at 30/90 FPS"),MinimumHeight>0.0);
        TestTrue(TEXT("Lower translates the overhead camera while retaining its downward angle"),
            Controller->bCameraMapMode && Boom->TargetArmLength<MapArm
            && FMath::IsNearlyEqual(Boom->GetRelativeRotation().Pitch,ACECameraRetail::LookDownPitchDegrees(),.01));
        TestTrue(TEXT("Lower stops before crossing the player"),
            FMath::IsNearlyEqual(Boom->TargetArmLength,ACECameraRetail::CloserMinLengthAc*Controller->GetCameraScaleCm(),.01));
        Controller->PlayerInput->FlushPressedKeys();
        Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
        Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::NumPadEight,IE_Pressed,1.f));
        for (int32 Frame=0;Frame<FPS;++Frame)
        {
            Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
            Controller->UpdateOrbitCamera(1.f/FPS);
        }
        TestTrue(TEXT("Raise translates back out without inverting or losing overhead mode"),
            Controller->bCameraMapMode && Boom->TargetArmLength>ACECameraRetail::CloserMinLengthAc*Controller->GetCameraScaleCm()
            && FMath::IsNearlyEqual(Boom->GetRelativeRotation().Pitch,ACECameraRetail::LookDownPitchDegrees(),.01));
        Controller->PlayerInput->FlushPressedKeys();
        Controller->PlayerInput->ProcessInputStack({},1.f/FPS,false);
        Controller->SetCameraMapMode(Boom,false);
        TestTrue(TEXT("Leaving an adjusted map view restores the saved ordinary view"),
            FMath::IsNearlyEqual(Boom->TargetArmLength,SavedArm,.01)
            && Boom->GetRelativeRotation().Equals(SavedRotation,.01) && Boom->bDoCollisionTest);
    }
    // Physical +/- must retain CameraSet distances at both 4:3 windowed and
    // widescreen viewport sizes. FOV changes the lens, never the zoom goal.
    auto* Lens=NewObject<UCameraComponent>(Pawn);
    Lens->SetupAttachment(Boom); Lens->RegisterComponent();
    for (FIntPoint View : {FIntPoint(2048,1536),FIntPoint(1920,1080),FIntPoint(3440,1440)})
    for (float Fov : {60.f,90.f,120.f}) for (bool Closer : {true,false})
    {
        Controller->ResetCameraToRetailDefaults(Boom);
        Boom->TargetArmLength=Controller->UserCameraArmLength=300.f;
        ACERuntimeOptions::Set(TEXT("FieldOfViewDegrees"),Fov);
        ACECameraRetail::ApplyFovToCamera(Lens,View.X,View.Y,Fov);
        Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Closer?EKeys::Subtract:EKeys::Add,IE_Pressed,1.f));
        Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
        Controller->UpdateOrbitCamera(1.f/60);
        TestTrue(TEXT("Physical numpad minus moves closer and plus farther independently of FOV/aspect"),
            FMath::IsNearlyEqual(Controller->UserCameraArmLength,300.f*(1.f+(Closer?-1.f:1.f)*8.f/60),.001f));
        Controller->PlayerInput->FlushPressedKeys();
        Controller->PlayerInput->ProcessInputStack({},1.f/60,false);
    }
    ACERuntimeOptions::Set(TEXT("FieldOfViewDegrees"),90.f);
    // A wheel event and one held-key frame use identical CameraSet factors.
    for(float Dt : {1.f/30,1.f/60,1.f/144}) for(bool Closer : {true,false})
    {
        Controller->ResetCameraToRetailDefaults(Boom);
        Boom->SetRelativeRotation(FRotator(-20,90,0));
        Boom->TargetArmLength=Controller->UserCameraArmLength=300.f;
        Controller->CameraInputFrameSeconds=Dt;
        Controller->AdjustMouseCameraDistance(Closer?1.f:-1.f);
        const float Expected=300.f*(1.f+(Closer?-1.f:1.f)*8.f*Dt);
        TestTrue(TEXT("Wheel follows retail's elapsed-frame step"),FMath::IsNearlyEqual(Controller->UserCameraArmLength,Expected,.001f));
        TestEqual(TEXT("Zoom changes the goal without snapping the viewer"),Boom->TargetArmLength,300.f);
        Controller->SyncUserCameraArmLength(Boom,Dt);
        TestTrue(TEXT("Viewer distance blends with retail translation stiffness"),FMath::IsNearlyEqual(Boom->TargetArmLength,FMath::Lerp(300.f,Expected,.45f*Dt*10),.001f));
        Controller->UserCameraArmLength=300.f;Controller->StepCameraZoom(Boom,Closer,Dt);
        TestTrue(TEXT("Keyboard and wheel share the same target step"),FMath::IsNearlyEqual(Controller->UserCameraArmLength,Expected,.001f));
    }
    Controller->ResetCameraToRetailDefaults(Boom);
    Controller->UserCameraArmLength=51.f;
    Controller->StepCameraZoom(Boom,true,1.f/60);
    TestTrue(TEXT("Minimum zoom rejects the step without entering first person"),Controller->UserCameraArmLength==51.f && !Controller->bCameraInHead);
    Controller->SetCameraInHead(Boom,true);
    Controller->AdjustMouseCameraDistance(1);
    TestTrue(TEXT("Closer in first person keeps first person"),Controller->bCameraInHead);
    Controller->AdjustMouseCameraDistance(-1);
    TestTrue(TEXT("Farther exits first person at the retail offset"),!Controller->bCameraInHead && FMath::IsNearlyEqual(Controller->UserCameraArmLength,ACECameraRetail::OffsetLengthCm(-.6f,.5f,Controller->GetCameraScaleCm()),.001f));
    Boom->SetRelativeRotation(FRotator(-20,90,0));
    Controller->UserCameraArmLength=1000.f;
    Controller->StepCameraZoom(Boom,false,1.f/144);
    TestTrue(TEXT("Tilted view may zoom beyond ten units of arm length while each horizontal axis remains below ten"),Controller->UserCameraArmLength>1000.f);
    const float AtLimit=Controller->UserCameraArmLength;
    Controller->StepCameraZoom(Boom,false,.05f);
    TestEqual(TEXT("Out-of-bounds zoom step leaves the previous offset intact"),Controller->UserCameraArmLength,AtLimit);
    Controller->ResetCameraToRetailDefaults(Boom);
    Boom->SetRelativeRotation(FRotator(-89,90,0));
    Controller->UserCameraArmLength=Boom->TargetArmLength=10000.f;
    for(int32 Frame=0;Frame<120;++Frame)
    {
        Controller->ApplyCameraOrbitDelta(Boom,0,.5f);
        Controller->SyncUserCameraArmLength(Boom,1.f/60);
    }
    TestEqual(TEXT("Lowering a far, raised camera preserves the requested zoom"),Controller->UserCameraArmLength,10000.f);
    TestEqual(TEXT("Lowering blends only angle, not zoom distance"),Boom->TargetArmLength,10000.f);
    Controller->StepCameraZoom(Boom,true,1.f/60);
    TestTrue(TEXT("A rotated far offset can always zoom closer"),Controller->UserCameraArmLength<10000.f);
    Controller->ResetCameraToRetailDefaults(Boom);
    for (const FKey& Key : {EKeys::NumPadFour,EKeys::NumPadSix})
    {
        Controller->SetCameraMapMode(Boom,true);
        Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key,IE_Pressed,1.f));
        Controller->PlayerInput->ProcessInputStack({},.016f,false);
        Controller->UpdateOrbitCamera(.016f);
        TestTrue(TEXT("Horizontal keyboard rotation restores ordinary distance and collision before turning"),
            !Controller->bCameraMapMode && !Controller->bCameraLookDown && Boom->bDoCollisionTest
            && FMath::IsNearlyEqual(Boom->TargetArmLength,SavedArm,.01));
        Controller->PlayerInput->FlushPressedKeys();
        Controller->PlayerInput->ProcessInputStack({},.016f,false);
        Controller->ResetCameraToRetailDefaults(Boom);
    }
    Client->SendSetSingleCharacterOption(0x31,true);
    Controller->UpdateMouseButtons(true,false,false,false);
    for (const bool bMap : {false,true}) for (const bool bInvertY : {false,true})
    {
        ACECameraSettings::SetMouseInversion(false,bInvertY);
        if (bMap) Controller->SetCameraMapMode(Boom,true);
        else Controller->SetCameraLookDown(Boom,true);
        for (const float MouseY : {-100000.f,100000.f,0.f})
        {
            Controller->ApplyMouseLookDelta(0,MouseY,Boom);
            // No physics scene in this fixture. Retain and assert the mode's
            // collision setting, but tick the socket without world sweeps.
            const bool bCollision=Boom->bDoCollisionTest;
            Boom->bDoCollisionTest=false;
            Boom->TickComponent(.016f,LEVELTICK_All,nullptr);
            Boom->bDoCollisionTest=bCollision;
            TestTrue(TEXT("Extreme/inverted vertical mouse input cannot swing either overhead view below the pivot"),
                Controller->bCameraLookDown && Controller->bCameraMapMode==bMap
                && Boom->GetSocketLocation(USpringArmComponent::SocketName).Z>Boom->GetComponentLocation().Z
                && FMath::IsNearlyEqual(Boom->GetRelativeRotation().Pitch,ACECameraRetail::LookDownPitchDegrees(),.01));
        }
        Controller->ApplyMouseLookDelta(10,1,Boom);
        TestTrue(TEXT("Diagonal mouse motion restores the ordinary arm before applying tilt"),
            !Controller->bCameraLookDown && !Controller->bCameraMapMode && Boom->bDoCollisionTest
            && FMath::IsNearlyEqual(Boom->TargetArmLength,SavedArm,.01));
        Controller->ResetCameraToRetailDefaults(Boom);
    }
    ACECameraSettings::SetMouseInversion(false,false);
    Controller->UpdateMouseButtons(false,false,false,false);
    for (const float Wheel : {-1.f,1.f})
    {
        Controller->SetCameraMapMode(Boom,true);
        Controller->AdjustMouseCameraDistance(Wheel);
        TestTrue(TEXT("Wheel zoom exits map mode with ordinary distance and collision restored"),
            !Controller->bCameraMapMode && Boom->bDoCollisionTest
            && Boom->TargetArmLength<=ACECameraRetail::FartherMaxXYAc*Controller->GetCameraScaleCm());
        Controller->ResetCameraToRetailDefaults(Boom);
    }
    Boom->bDoCollisionTest=false; Boom->bEnableCameraLag=true;
    Boom->bEnableCameraRotationLag=false; Boom->CameraLagSpeed=ACECameraRetail::TranslationLagSpeed;
    Boom->SetWorldRotation(FRotator::ZeroRotator); Boom->TargetArmLength=300.f;
    Boom->bEnableCameraLag=false; Boom->SetWorldLocation(FVector::ZeroVector);
    Boom->TickComponent(1.f/60,LEVELTICK_All,nullptr); Boom->bEnableCameraLag=true;
    for(int32 I=1;I<=120;++I) { Boom->SetWorldLocation(FVector(I*10.f,0,0)); Boom->TickComponent(1.f/60,LEVELTICK_All,nullptr); }
    const float RunningDistance=1200.f-Boom->GetSocketLocation(USpringArmComponent::SocketName).X;
    TestTrue(TEXT("Running gives the retail translation-stiffness pullback"),RunningDistance>400 && RunningDistance<450);
    for(int32 I=0;I<120;++I) Boom->TickComponent(1.f/60,LEVELTICK_All,nullptr);
    TestTrue(TEXT("Stopping restores the chosen camera distance"),FMath::Abs(1200.f-Boom->GetSocketLocation(USpringArmComponent::SocketName).X-300.f)<.1f);
    TestEqual(TEXT("Movement lag does not change manual zoom"),Boom->TargetArmLength,300.f);
    // Measure the rendered socket, not just the clamped requested pitch.
    // The camera's smoothing can introduce roll even when both endpoints are upright.
    {
        auto* OrbitPawn=World->SpawnActor<APawn>();
        auto* Root=NewObject<USceneComponent>(OrbitPawn); OrbitPawn->SetRootComponent(Root); Root->RegisterComponent();
        auto* Orbit=NewObject<UACEOrbitCameraBoom>(OrbitPawn); Orbit->SetupAttachment(Root); Orbit->RegisterComponent();
        Orbit->bDoCollisionTest=false; Orbit->TargetArmLength=300.f;
        Orbit->bInheritPitch=false; Orbit->bInheritRoll=false;
        Orbit->CameraRotationLagSpeed=4.5f;
        Client->SendSetSingleCharacterOption(0x31,true);
        Controller->UpdateMouseButtons(true,false,false,false);
        double MaxRoll=0, MaxPitch=0;
        for(float Dt:{1.f/30,1.f/60,1.f/144}) for(bool Invert:{false,true})
        {
            ACECameraSettings::SetMouseInversion(Invert,Invert);
            Orbit->bEnableCameraRotationLag=false;
            Orbit->SetRelativeRotation(FRotator(-70,170,0)); Orbit->TickComponent(Dt,LEVELTICK_All,nullptr);
            Orbit->bEnableCameraRotationLag=true;
            for(int32 Frame=0;Frame<180;++Frame)
            {
                // Diagonal drags into the pole, then away; cross the yaw wrap
                // and rotate the parent as happens when mouse turning the player.
                Root->SetWorldRotation(FRotator(0,Frame*.5,0));
                const float Direction=Invert?-1.f:1.f;
                Controller->ApplyMouseLookDelta(Direction*12.f,Direction*(Frame%40<20?8.f:-1.f),Orbit);
                Orbit->TickComponent(Dt,LEVELTICK_All,nullptr);
                const FRotator Eye=Orbit->GetSocketRotation(USpringArmComponent::SocketName);
                MaxRoll=FMath::Max(MaxRoll,FMath::Abs(Eye.Roll));
                MaxPitch=FMath::Max(MaxPitch,FMath::Abs(Eye.Pitch));
            }
        }
        AddInfo(FString::Printf(TEXT("Near-pole camera maximum rendered roll %.3f degrees; pitch %.3f degrees"),MaxRoll,MaxPitch));
        TestTrue(TEXT("Diagonal mouse orbit keeps the rendered view upright near straight down"),MaxRoll<.01);
        TestTrue(TEXT("Rotation smoothing stays within the requested pitch limit"),MaxPitch<=89.01);
        TestTrue(TEXT("Camera smoothing preserves configured inheritance"),!Orbit->bInheritPitch && Orbit->bInheritYaw && !Orbit->bInheritRoll);
        TestTrue(TEXT("Reading the rotation target still returns the unsmoothed player request"),
            FMath::IsNearlyEqual(Orbit->GetTargetRotation().Pitch,Orbit->GetRelativeRotation().Pitch,.001));
        TArray<FRotator> SettledViews;
        for(float Dt:{1.f/30,1.f/60,1.f/144})
        {
            Root->SetWorldRotation(FRotator::ZeroRotator);
            Orbit->bEnableCameraRotationLag=false;
            Orbit->SetRelativeRotation(FRotator(-70,170,0)); Orbit->TickComponent(Dt,LEVELTICK_All,nullptr);
            Orbit->bEnableCameraRotationLag=true;
            Orbit->SetRelativeRotation(FRotator(-89,-170,0));
            for(int32 Frame=0;Frame<FMath::RoundToInt(.5f/Dt);++Frame) Orbit->TickComponent(Dt,LEVELTICK_All,nullptr);
            const auto Eye=Orbit->GetSocketRotation(USpringArmComponent::SocketName);
            const float ExpectedYaw=-170.f-20.f*FMath::Pow(1.f-4.5f*Dt,FMath::RoundToInt(.5f/Dt));
            TestTrue(TEXT("Yaw wrap follows retail's linear stiffness on the short upright path"),FMath::Abs(Eye.Yaw-ExpectedYaw)<.01 && FMath::Abs(Eye.Roll)<.01);
            SettledViews.Add(Eye);
        }
        TestTrue(TEXT("Orbit smoothing is consistent at 30, 60 and 144 FPS"),
            SettledViews[0].Equals(SettledViews[1],.5) && SettledViews[0].Equals(SettledViews[2],.5));
        Orbit->CameraRotationLagSpeed=10.f; Orbit->SetRelativeRotation(FRotator(-10,50,0));
        Orbit->TickComponent(1.f/144,LEVELTICK_All,nullptr);
        TestTrue(TEXT("Retail maximum stiffness bypasses rotation lag"),Orbit->GetSocketRotation(USpringArmComponent::SocketName).Equals(FRotator(-10,50,0),.01));
        ACECameraSettings::SetMouseInversion(false,false);
        Controller->UpdateMouseButtons(false,false,false,false);
        OrbitPawn->Destroy();
    }
    Controller->UpdateMouseButtons(false,false,false,false);
    ACECameraSettings::SetToggleMouseLook(true);
    Controller->UpdateMouseButtons(true,false,false,false);
    Controller->UpdateMouseButtons(false,false,false,false);
    TestTrue(TEXT("Toggle mouse look survives right-button release"),Controller->bMouseLookActive && Controller->bMouseLookToggled);
    Controller->MouseLookPressX=123;Controller->MouseLookPressY=234;
    Controller->MouseLookTravelPixels=40;
    Controller->bShowMouseCursor=true; // loading screen temporarily owns input
    Controller->SetMouseLookActive(true);
    TestTrue(TEXT("Portal recapture preserves active toggle and hides cursor"),Controller->bMouseLookToggled && !Controller->bShowMouseCursor);
    TestEqual(TEXT("Portal recapture retains cursor restore X"),Controller->MouseLookPressX,123.f);
    TestEqual(TEXT("Portal recapture does not turn a drag into an inspect click"),Controller->MouseLookTravelPixels,40.f);
    Controller->UpdateMouseButtons(true,false,false,false);
    Controller->UpdateMouseButtons(false,false,false,false);
    TestFalse(TEXT("Second right-button press releases toggle mouse look"),Controller->bMouseLookActive);
    ACECameraSettings::SetToggleMouseLook(false);
    Controller->Client=nullptr;
    World->DestroyWorld(false);
    for (float Yaw : {0.f, 45.f, 180.f, 270.f, 450.f})
    {
        const float Turn=ACECameraRetail::FollowTurnDegrees(Yaw,1.f/60,true,true);
        const float Angle=FMath::UnwindDegrees(Yaw-90.f);
        TestTrue(TEXT("Mouse facing cannot exceed retail's turn rate"),
            FMath::Abs(Turn)<=FMath::RadiansToDegrees(3.375f)/60.f+.0001f);
        TestTrue(TEXT("Mouse facing progresses toward the view without overshoot"),
            FMath::Abs(FMath::UnwindDegrees(Angle-Turn))<=FMath::Abs(Angle)+.0001f);
    }
    TestEqual(TEXT("Orbit at rest does not turn the character"),ACECameraRetail::FollowTurnDegrees(180,1.f/60,false),0.f);
    float Yaw60=180, Yaw30=180;
    for (int32 I=0; I<60; ++I) Yaw60-=ACECameraRetail::FollowTurnDegrees(Yaw60,1.f/60,true);
    for (int32 I=0; I<30; ++I) Yaw30-=ACECameraRetail::FollowTurnDegrees(Yaw30,1.f/30,true);
    TestTrue(TEXT("Camera follow converges behind the character"),FMath::Abs(Yaw60-90)<0.1f);
    TestTrue(TEXT("Camera follow is frame-rate independent"),FMath::Abs(Yaw60-Yaw30)<0.001f);
    auto Support=[](const FVector2D& P) { return P.X <= 0; };
    const FVector2D Straight=ACELedgeSlide::Resolve(FVector2D(-1,0),FVector2D(10,0),Support);
    TestTrue(TEXT("Head-on edge input does not bounce sideways"),Straight.X<=0 && FMath::Abs(Straight.Y)<0.001);
    FVector2D LocalEdge(-1,0);
    for(int32 I=0; I<20; ++I)
        LocalEdge=ACELedgeSlide::Resolve(LocalEdge,FVector2D(10,0),Support,true);
    TestTrue(TEXT("Rejected local edge steps retain the previous contact instead of creeping outward"),
        LocalEdge.Equals(FVector2D(-1,0),.001));
    FVector2D At(-1,0);
    for (int32 I=0; I<100; ++I)
    {
        const FVector2D Next=ACELedgeSlide::Resolve(At,FVector2D(5,5),Support);
        TestTrue(TEXT("Diagonal ledge slide stays supported and never reverses"),Next.X<=0 && Next.Y>=At.Y);
        At=Next;
    }
    TestTrue(TEXT("Diagonal input makes progress along the edge"),At.Y>300);
    return true;
}
#endif
