#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Misc/ScopeExit.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "ACEPlayerController.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEDatSubsystem.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEInputBindings.h"
#include "ACEHoverTooltipWidget.h"
#include "ACEOrbitCameraBoom.h"
#include "ACERuntimeOptions.h"
#include "Dat/ACEDatCursor.h"
#include "ACEWorldEntityActor.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWalkingStepTest,"ACE.RetailParity.WalkingStep",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEWalkingStepTest::RunTest(const FString&)
{
 const FString OriginalSettings=GGameUserSettingsIni;
 GGameUserSettingsIni=FPaths::ProjectSavedDir()/TEXT("Automation/WalkingStepFixture.ini");
 FConfigFile Config;Config.NoSave=true;GConfig->SetFile(GGameUserSettingsIni,&Config);
 ACEInputBindings::Reload();ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
 ON_SCOPE_EXIT{GGameUserSettingsIni=OriginalSettings;ACEInputBindings::Reload();ACERuntimeOptions::Apply();};
 const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
  .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
 auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
 auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
 auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);
 Context.OwningGameInstance=GI;Context.SetCurrentWorld(World);GI->OnWorldChanged(nullptr,World);GI->Init();
 ON_SCOPE_EXIT{GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);};
 auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();auto* Client=GI->GetSubsystem<UACEClientSubsystem>();
 auto Session=Client->GetSession();
 if(!TestTrue(TEXT("Retail DAT loads"),Dat->LoadDatDirectory(ACETestDatDirectory())))return false;
 // CSequence excludes High on forward exit and enters the next positive
 // clip at Low. Reverse traversal subtracts every frame, including Low.
 double RetailStep=0,ReverseStep=0,ExitFirst=0;
 for(uint32 Id:{0x03000006u,0x03000009u})
 {
  TArray<uint8> Blob;Dat->GetPortalDat()->ReadFile(Id,Blob);
  FACEDatCursor Cursor(Blob);FACEDatAnimation Anim;
  if(!ACEDatUnpack::UnpackAnimation(Cursor,Anim))return false;
  for(int I=0;I<Anim.PositionFrames.Num();++I)
  {
   ReverseStep+=Anim.PositionFrames[I].GetTranslation().Y;
   if(I+1<Anim.PositionFrames.Num())RetailStep+=Anim.PositionFrames[I].GetTranslation().Y;
  }
  if(Id==0x03000009u){ExitFirst=Anim.PositionFrames[0].GetTranslation().Y;RetailStep+=ExitFirst;}
 }
 TestTrue(TEXT("Retail forward step matches ACE Sequence endpoint"),FMath::Abs(RetailStep-.758917)<.00001);
 TestTrue(TEXT("Retail reverse step includes the boundary frame"),FMath::Abs(ReverseStep-.780484)<.00001);
 auto* PC=World->SpawnActor<AACEPlayerController>();PC->Client=Client;
 PC->SetPlayer(NewObject<ULocalPlayer>(GEngine));World->AddController(PC);
 auto* Pawn=World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
 Capsule->InitCapsuleSize(48,88);Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);Capsule->RegisterComponent();
 Pawn->SetActorEnableCollision(false);PC->Possess(Pawn);PC->PlayerInput=NewObject<UPlayerInput>(PC);PC->SetupInputComponent();
 PC->InputComponent->AxisBindings.Reset();PC->bRetailCursorInstalled=true;
 PC->HoverTooltipWidget=CreateWidget<UACEHoverTooltipWidget>(GI,UACEHoverTooltipWidget::StaticClass());
 PC->bEnterWorldLoading=false;PC->bWorldRevealActive=false;
 Session->PlayerGuid=12345;Session->State=EACESessionState::InWorld;
 FACEWorldObject Self;Self.Guid=12345;Self.SetupId=0x02000001;Self.MotionTableId=0x09000001;Self.bIsPlayer=Self.bIsSelf=true;
 Session->WorldObjects.Add(Self.Guid,Self);
 auto* App=NewObject<UACECharacterAppearanceComponent>(Pawn);Pawn->AddInstanceComponent(App);App->RegisterComponent();
 if(!TestTrue(TEXT("Player appearance loads"),App->ApplyWorldObject(Self,100,false)))return false;
 App->PreferredStyle=ACEMotion::StanceNonCombat;
 auto* Floor=World->SpawnActor<AActor>();auto* Box=NewObject<UBoxComponent>(Floor);
 Floor->SetRootComponent(Box);Floor->AddInstanceComponent(Box);Box->SetBoxExtent(FVector(8000,8000,50));
 Box->SetCollisionResponseToAllChannels(ECR_Block);Box->RegisterComponent();Floor->SetActorLocation(FVector(-25000,25000,9950));
 const FVector StartFeet(-24000,25000,10000);
 auto Reset=[&]
 {
  PC->PlayerInput->FlushPressedKeys();PC->PlayerInput->ProcessInputStack({},1.f/90,false);
  App->ResetWalkingMotion();App->ClearActionMotion();
  FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
  Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
  PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
  PC->bLocalPredicting=false;PC->bJumpAirborne=false;PC->bJumpCharging=false;PC->bStandingJumpLocked=false;PC->StepHoldSeconds=0;
  PC->LandingWorldAceVelocity=FVector::ZeroVector;PC->LandingRootTrack.Reset();
  PC->ForwardSent=PC->RightSent=0;PC->bWasMoving=false;
  Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
 };
 auto Key=[&](FKey K,bool Down)
 {PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,Down?IE_Pressed:IE_Released,Down?1.f:0.f));
  PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(K,Down?IE_Pressed:IE_Released,Down?1.f:0.f));
  PC->PlayerInput->ProcessInputStack({},1.f/90,false);};
 auto Tick=[&](float Dt){PC->PlayerTick(Dt);App->TickComponent(Dt,LEVELTICK_All,nullptr);};
 for(float Dt:{1.f/30,1.f/90,1.f/144})for(int Hold:{1,3,7})for(int Direction:{1,-1})
 {
  Reset();Key(EKeys::LeftShift,true);const FKey K=Direction>0?EKeys::W:EKeys::X;Key(K,true);
  for(int I=0;I<Hold;++I)Tick(Dt);
  Key(K,false);Key(EKeys::LeftShift,false);Tick(Dt);
  TestFalse(TEXT("Key release stops raw network motion immediately"),Session->bMoving);
  TestTrue(TEXT("Remaining step owns position reporting after key-up"),Session->bForcePositionReporting);
  TestTrue(TEXT("Remaining step owns local collision prediction"),PC->bLocalPredicting);
  for(int I=0;I<2/Dt;++I)Tick(Dt);
  const double Travel=(Pawn->GetActorLocation().X-StartFeet.X)/100.;
  const FString Label=FString::Printf(TEXT("%.0fHz %d-frame tap direction=%d travels %.6fm"),1/Dt,Hold,Direction,Travel);
  AddInfo(Label);TestTrue(*Label,FMath::Abs(Travel-(Direction>0?RetailStep:-ReverseStep))<.002);
  TestFalse(TEXT("Completed step releases forced reporting"),Session->bForcePositionReporting);
  TestFalse(TEXT("Completed step has no pending animation"),App->HasWalkingTransition());
 }
 // Held walking retains its cycle after entry; stopping adds only the exit.
 for (bool Run : {false,true}) for (float Dt : {1.f/30,1.f/90,1.f/144})
 {
  Reset();Key(EKeys::LeftShift,!Run);Key(EKeys::W,true);Tick(Dt);
  Key(EKeys::W,false);Key(ACEInputBindings::Get(ACEInputBindings::Action(TEXT("Stop")),0).Key,true);Tick(Dt);
  TestTrue(TEXT("Explicit Stop retains the unfinished walk/run step like retail"),App->HasWalkingTransition());
  TestTrue(TEXT("Explicit Stop keeps collision prediction until the step finishes"),PC->bLocalPredicting);
  for(int I=0;I<2/Dt;++I)Tick(Dt);
  TestFalse(TEXT("Holding Stop cannot restart the completed step"),App->HasWalkingTransition());
  if(!Run)TestTrue(TEXT("Stop completes the same authored walk displacement as key release"),FMath::Abs((Pawn->GetActorLocation().X-StartFeet.X)/100.-RetailStep)<.002);
 }
 Reset();Key(EKeys::LeftShift,true);Key(EKeys::W,true);
 for(int I=0;I<90;++I)Tick(1.f/90);
 Key(EKeys::W,false);for(int I=0;I<90;++I)Tick(1.f/90);
 TestTrue(TEXT("Held walking continues after the entry clip"),FMath::Abs((Pawn->GetActorLocation().X-StartFeet.X)/100.-(RetailStep-ExitFirst+(1.-11./30)*3.12))<.003);
 Reset();Key(EKeys::LeftShift,true);Key(EKeys::C,true);Tick(1.f/90);Key(EKeys::C,false);Tick(1.f/90);
 const FVector StrafeEnd=Pawn->GetActorLocation();for(int I=0;I<90;++I)Tick(1.f/90);
 TestTrue(TEXT("Strafe still permits a micro-step and stops on release"),FVector::Dist2D(StartFeet,StrafeEnd)<10 && FVector::Dist2D(StrafeEnd,Pawn->GetActorLocation())<.01);
 TestFalse(TEXT("Strafe never queues forward links"),App->HasWalkingTransition());
 // Remaining authored movement is collision-resolved, not a teleport after key-up.
 auto* Wall=World->SpawnActor<AActor>();auto* WallBox=NewObject<UBoxComponent>(Wall);
 Wall->SetRootComponent(WallBox);Wall->AddInstanceComponent(WallBox);WallBox->SetBoxExtent(FVector(10,200,500));
 WallBox->SetCollisionResponseToAllChannels(ECR_Block);WallBox->RegisterComponent();Wall->SetActorLocation(StartFeet+FVector(80,0,0));
 Reset();Key(EKeys::LeftShift,true);Key(EKeys::W,true);Tick(1.f/90);Key(EKeys::W,false);
 for(int I=0;I<150;++I)Tick(1.f/90);
 TestTrue(TEXT("Finishing a step cannot carry the player through a wall"),Pawn->GetActorLocation().X-StartFeet.X<65);
 TestFalse(TEXT("A blocked step finishes without accumulating movement"),App->HasWalkingTransition());
 Wall->Destroy();
 // Other Unreal clients use the same retained links for the remote pose.
 auto* Remote=World->SpawnActor<AACEWorldEntityActor>();Self.Guid=23456;Self.bIsSelf=false;
 Remote->InitializeFromObject(Self,100,true);auto* RemoteApp=Remote->Appearance.Get();
 if(!TestEqual(TEXT("Remote fixture builds the same avatar parts"),RemoteApp->GetPartCount(),App->GetPartCount()))return false;
 RemoteApp->PreferredStyle=ACEMotion::StanceNonCombat;RemoteApp->SetPreviewCapture(true);
 Reset();Key(EKeys::LeftShift,true);Key(EKeys::W,true);RemoteApp->SetLocomotionInput(1,0,false,1);
 Tick(1.f/90);RemoteApp->TickComponent(1.f/90,LEVELTICK_All,nullptr);
 Key(EKeys::W,false);RemoteApp->SetLocomotionInput(0,0,true,1);
 for(int I=0;I<60;++I)
 {
  Tick(1.f/90);RemoteApp->TickComponent(1.f/90,LEVELTICK_All,nullptr);
  if(I>25)for(int32 Part=0;Part<App->GetPartCount();++Part)
   if(App->GetPartMesh(Part) && RemoteApp->GetPartMesh(Part))
    TestTrue(TEXT("Remote and local step poses agree after key release"),
     App->GetPartMesh(Part)->GetRelativeTransform().Equals(RemoteApp->GetPartMesh(Part)->GetRelativeTransform(),.002));
 }
 Reset();Key(EKeys::LeftShift,true);Key(EKeys::W,true);Tick(1.f/90);
 PC->bJumpAirborne=true;Tick(1.f/90);
 TestFalse(TEXT("Jump discards ground-only step motion"),App->HasWalkingTransition());
 Reset();Key(EKeys::LeftShift,true);Key(EKeys::W,true);Tick(1.f/90);Key(EKeys::LeftShift,false);Tick(1.f/90);
 TestTrue(TEXT("Walk to run preserves the authored transition"),App->HasWalkingTransition());
 Reset();Key(EKeys::LeftShift,true);Key(EKeys::W,true);Tick(1.f/90);
 Session->State=EACESessionState::CharacterSelect;Tick(1.f/90);
 TestFalse(TEXT("Leaving the world discards the unfinished step"),App->HasWalkingTransition());
 Session->State=EACESessionState::InWorld;
 Reset();Key(EKeys::W,true);Tick(1.f/90);Key(EKeys::W,false);
 Tick(1.f/90);
 TestTrue(TEXT("Run release retains an authored step"),App->HasWalkingTransition());
 for(int I=0;I<180;++I)Tick(1.f/90);
 TestFalse(TEXT("Run exit eventually returns to idle"),App->HasWalkingTransition());
 Reset();Key(EKeys::D,true);Tick(1.f/90);
 TestTrue(TEXT("Local turn input reaches the animation channel"),!FMath::IsNearlyZero(App->LocomotionTurn));
 TArray<FTransform> TurnA,TurnB;int32 TurnCount=0;
 TestTrue(TEXT("Actual retail turn motion evaluates"),Dat->EvaluateMotionCommand(0x09000001,ACEMotion::TurnRight,.05,App->GetPartCount(),TurnA,100,TurnCount,nullptr,nullptr,ACEMotion::StanceNonCombat));
 Dat->EvaluateMotionCommand(0x09000001,ACEMotion::TurnRight,.3,App->GetPartCount(),TurnB,100,TurnCount,nullptr,nullptr,ACEMotion::StanceNonCombat);
 bool Changed=false;for(int I=0;I<FMath::Min(TurnA.Num(),TurnB.Num());++I)Changed|=!TurnA[I].Equals(TurnB[I],.01);
 TestTrue(TEXT("Turning has animated frames, not an idle pose"),Changed);
 Reset();App->PlayActionMotion(0x1000005B,1,ACEMotion::StanceSwordCombat);
 Key(EKeys::W,true);Tick(1.f/90);
 TestTrue(TEXT("Active melee swing locks player translation"),FVector::Dist2D(Pawn->GetActorLocation(),StartFeet)<.01);
 TestTrue(TEXT("Melee lock follows the live action"),App->IsMovementLockedByAction());
 App->ClearActionMotion();Tick(1.f/90);
 TestTrue(TEXT("Movement resumes when the swing finishes"),PC->ForwardAxis>0);
 Reset();App->PreferredStyle=ACEMotion::StanceSwordCombat;App->SetPreferredStyle(ACEMotion::StanceNonCombat);
 TestTrue(TEXT("Returning to peace plays the stance linkage and pauses movement"),App->IsMovementLockedByAction());
 ACERuntimeOptions::Set(TEXT("ClassicMovement"),0); ACERuntimeOptions::Apply();
 // Default casting permits ordinary movement throughout its authored
 // animation. Cover both PowerUp families and forward/backward walk/run.
 for(uint32 Command : {0x40000034u,0x400000D3u,0x400000E0u,0x400000E1u,
   0x1000006Fu,0x10000078u,0x1000012Bu,0x10000134u})
 for(float Dt : {1.f/30,1.f/144}) for(bool Run : {false,true}) for(bool Back : {false,true})
 {
  Reset();App->PreferredStyle=ACEMotion::StanceMagic;
  App->PlayActionMotion(Command,1,ACEMotion::StanceMagic);
  Key(EKeys::LeftShift,!Run);Key(Back?EKeys::X:EKeys::W,true);
  TestFalse(TEXT("Cast and windup animations never block forward movement"),App->HasPendingCombatForwardMotion());
  for(int32 Frame=0;Frame<3;++Frame)
  {
   const FVector Before=Pawn->GetActorLocation();Tick(Dt);
   const float Speed=(Back ? Client->GetLocomotionSpeed(false)*(Run?Client->GetRunRate():1.f)*.65f
     : Client->GetLocomotionSpeed(Run));
   TestTrue(*FString::Printf(TEXT("Casting %08X keeps %s speed, direction %d at %.0fHz"),Command,Run?TEXT("run"):TEXT("walk"),Back?-1:1,1/Dt),
    FMath::IsNearlyEqual(Pawn->GetActorLocation().X-Before.X,(Back?-1.f:1.f)*Speed*100.f*Dt,.05));
   TestEqual(TEXT("Movement preserves the active casting animation"),App->ActionCommand,Command);
  }
 }
 ACERuntimeOptions::Set(TEXT("ClassicMovement"),1); ACERuntimeOptions::Apply();
 auto CastMotion=[&](uint32 Command,float Speed=1.f)
 {
  FACEObjectMotionState Motion;
  Motion.CurrentStyle=ACEMotion::StanceMagic;
  if(Command==ACEMotion::Ready) Motion.ForwardCommand=Command;
  else {Motion.ActionCommand=Command;Motion.ActionSpeed=Speed;}
  PC->HandleMotionUpdate(Session->PlayerGuid,Motion);
 };
 for(float Dt:{1.f/30,1.f/144}) for(bool Back:{false,true})
 {
  Reset();App->PreferredStyle=ACEMotion::StanceMagic;
  const float CastSpeed=Back?2.f:1.f;
  CastMotion(0x1000006F);CastMotion(0x1000012B);CastMotion(0x40000034,CastSpeed);
  Key(EKeys::LeftShift,Back);Key(Back?EKeys::X:EKeys::W,true);
  bool SawSecondWindup=false;int32 Frames=0,GestureFrames=0,ExitFrames=0;
  while(App->HasPendingCastForwardMotion() && Frames++<12/Dt)
  {
   SawSecondWindup|=App->ActionCommand==0x1000012B;
   if(App->ActionCommand==0x40000034)++GestureFrames;
   if(App->ActionCommand==ACEMotion::Ready && App->ActionFromCommand==0x40000034)++ExitFrames;
   Tick(Dt);
   TestTrue(TEXT("Classic cast holds forward travel across the queued links"),FMath::Abs(Pawn->GetActorLocation().X-StartFeet.X)<.01);
  }
  TestTrue(TEXT("Forward input is sent during the lock"),Back?PC->ForwardSent<0:PC->ForwardSent>0);
  TestTrue(TEXT("Both queued windups finish"),SawSecondWindup&&Frames<12/Dt);
  // Retail human clip 0x030005A4 uses frames 0..24 at 24 fps on entry and 24..44 at 30 fps on exit.
  TestTrue(TEXT("Cast entry follows the motion packet speed"),FMath::Abs(GestureFrames*Dt-25.f/(24.f*CastSpeed))<=Dt+.0001f);
  TestTrue(TEXT("Cast exit retains its own authored speed"),FMath::Abs(ExitFrames*Dt-21.f/30.f)<=Dt+.0001f);
  for(int I=0;I<1/Dt;++I)Tick(Dt);
  TestTrue(TEXT("Held forward or backward resumes in its original direction"),(Pawn->GetActorLocation().X-StartFeet.X)*(Back?-1:1)>10);
 }
 Reset();App->PreferredStyle=ACEMotion::StanceMagic;CastMotion(0x40000034);
 Key(EKeys::C,true);Tick(1.f/30);
 TestTrue(TEXT("Strafe moves laterally during the cast entry"),FMath::Abs(Pawn->GetActorLocation().Y-StartFeet.Y)>1);
 TestEqual(TEXT("A sidestep preserves the gesture"),App->ActionCommand,0x40000034u);
 Key(EKeys::C,false);Key(EKeys::D,true);Tick(1.f/30);
 TestTrue(TEXT("Turn is independent of the cast entry"),!FMath::IsNearlyZero(PC->TurnAxis));
 Key(EKeys::D,false);for(int I=0;I<90;++I)Tick(1.f/30);
 TestTrue(TEXT("Gesture rests at its retail endpoint until replaced"),App->bHoldActionFinal&&App->ActionCommand==0x40000034);
 PC->BeginJumpCharge();TestTrue(TEXT("Component gesture permits jump charge"),PC->bJumpCharging);
 PC->ReleaseJump(0,0);TestFalse(TEXT("Component gesture rejects premature jump release"),PC->bJumpAirborne);
 CastMotion(ACEMotion::Ready);
 TestTrue(TEXT("Server Ready starts the authored exit at the held pose"),App->HasPendingCastForwardMotion()&&App->ActionFromCommand==0x40000034);
 for(int I=0;I<120;++I)Tick(1.f/30);
 PC->BeginJumpCharge();PC->ReleaseJump(0,0);
 TestTrue(TEXT("Jump releases after the protected gesture ends"),PC->bJumpAirborne);
 Reset();App->PreferredStyle=ACEMotion::StanceMagic;CastMotion(0x400000D3);
 PC->BeginJumpCharge();PC->ReleaseJump(0,0);
 TestTrue(TEXT("A permitted jump breaks CastSpell into Falling"),PC->bJumpAirborne&&App->ActionCommand==0x40000015u);
 Reset();App->PreferredStyle=ACEMotion::StanceMagic;
 Key(EKeys::W,true);for(int I=0;I<90;++I)Tick(1.f/90);
 const FVector AtCast=Pawn->GetActorLocation();CastMotion(0x400000E0);
 Key(EKeys::C,true);Tick(1.f/90);
 TestTrue(TEXT("Casting mid-run locks the held forward key and preserves diagonal strafe"),
  FMath::Abs(Pawn->GetActorLocation().X-AtCast.X)<.01&&FMath::Abs(Pawn->GetActorLocation().Y-AtCast.Y)>1);
 CastMotion(ACEMotion::Ready);CastMotion(ACEMotion::Ready);
 TestEqual(TEXT("Repeated Ready echoes queue one cast exit"),App->PendingActionCommands.Num(),1);
 Reset();App->PreferredStyle=ACEMotion::StanceMagic;CastMotion(0x400000E1);
 Key(EKeys::W,true);Tick(1.f/90);
 ACERuntimeOptions::Set(TEXT("ClassicMovement"),0); ACERuntimeOptions::Apply();Tick(1.f/90);
 TestTrue(TEXT("Applying free movement takes effect mid-cast"),Pawn->GetActorLocation().X-StartFeet.X>1);
 // Missile links still own forward travel until the draw/reload finishes.
 struct FCombatCase {uint32 Command,Style;};
 for(const auto& C : {FCombatCase{0x40000016,ACEMotion::StanceBowCombat},{0x40000022,ACEMotion::StanceBowCombat},
   {0x40000016,ACEMotion::StanceCrossbow},{0x40000022,ACEMotion::StanceAtlatl},
   {0x40000022,ACEMotion::StanceThrownWeapon}})
 for(float Dt : {1.f/30,1.f/144}) for(bool Run : {false,true}) for(bool Back : {false,true})
 {
  Reset();App->PreferredStyle=C.Style;
  App->PlayActionMotion(C.Command,1,C.Style);
  Key(EKeys::LeftShift,!Run);Key(Back?EKeys::X:EKeys::W,true);
  TestTrue(TEXT("Combat entry owns the forward motion channel"),App->HasPendingCombatForwardMotion());
  int32 Frames=0;
  while(App->HasPendingCombatForwardMotion()&&Frames++<6/Dt)
  {
   const FVector Before=Pawn->GetActorLocation();Tick(Dt);
   TestTrue(*FString::Printf(TEXT("Motion %08X style %08X cannot run through an unfinished link"),C.Command,C.Style),
    FVector::Dist2D(Before,Pawn->GetActorLocation())<.01);
  }
  TestTrue(TEXT("Combat sequence finishes without requiring key release"),Frames>1&&Frames<6/Dt);
  const FVector Before=Pawn->GetActorLocation();
  for(int32 I=0;I<1/Dt;++I)Tick(Dt);
  TestTrue(TEXT("Held movement resumes after the combat link completes"),FVector::Dist2D(Before,Pawn->GetActorLocation())>10);
 }
 Reset();App->PreferredStyle=ACEMotion::StanceNonCombat;
 Reset();
 auto* Boom=NewObject<UACEOrbitCameraBoom>(Pawn);Boom->SetupAttachment(Capsule);Boom->RegisterComponent();
 Boom->SetRelativeRotation(FRotator(0,90,0));
 const float SavedSlope=ACERuntimeOptions::Get(TEXT("CameraAlignToSlope"));
 ACERuntimeOptions::Set(TEXT("CameraAlignToSlope"),1);
 PC->bHaveCameraSlopePosition=false;PC->CameraSlopeVelocities.Reset();
 Floor->SetActorLocation(StartFeet-FVector(0,0,50));Floor->SetActorRotation(FRotator(15,0,0));
 PC->UpdateCameraSlope(Boom,1.f/60);
 Pawn->AddActorWorldOffset(FVector(10,0,0));PC->UpdateCameraSlope(Boom,1.f/60);
 TestTrue(TEXT("Rear camera follows a physical slope along AC's forward axis without mouse input"),FMath::IsNearlyEqual(Boom->SlopePitch,15.f,.05f));
 Boom->SetRelativeRotation(FRotator(0,180,0));PC->UpdateCameraSlope(Boom,1.f/60);
 TestEqual(TEXT("Side camera keeps the chosen orientation"),Boom->SlopePitch,0.f);
 Boom->SetRelativeRotation(FRotator(0,90,0));
 PC->bHaveCameraSlopePosition=false;PC->CameraSlopeVelocities.Reset();
 PC->bJumpAirborne=true; // Airborne fallback follows the smoothed velocity with retail's 0.1 Z factor.
 PC->UpdateCameraSlope(Boom,1.f/60);
 Pawn->AddActorWorldOffset(FVector(10,10,10));PC->UpdateCameraSlope(Boom,1.f/60);
 TestTrue(TEXT("Slope camera adjusts without any mouse input"),Boom->SlopePitch>0);
 ACERuntimeOptions::Set(TEXT("CameraAlignToSlope"),0);PC->UpdateCameraSlope(Boom,1.f/60);
 TestEqual(TEXT("Slope option disables automatic pitch"),Boom->SlopePitch,0.f);
 ACERuntimeOptions::Set(TEXT("CameraAlignToSlope"),SavedSlope);
 return true;
}
#endif
