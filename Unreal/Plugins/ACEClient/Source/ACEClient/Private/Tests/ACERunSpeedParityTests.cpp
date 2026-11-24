#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACEPlayerController.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEDatSubsystem.h"
#include "ACEInputBindings.h"
#include "ACERuntimeOptions.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEWorldEntityActor.h"
#include "Protocol/ACEBinaryReader.h"
#include "Protocol/ACEBinaryWriter.h"
#include "ACEHoverTooltipWidget.h"
#include "Dat/ACEDatCursor.h"
#include "Dat/ACEDatFileTypes.h"
#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERunSpeedParityTest,"ACE.RetailParity.RunSpeed",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)

bool FACERunSpeedParityTest::RunTest(const FString&)
{
 const FString OriginalSettings=GGameUserSettingsIni;
 GGameUserSettingsIni=FPaths::ProjectSavedDir()/TEXT("Automation/RunSpeedFixture.ini");
 FConfigFile Config;Config.NoSave=false;Config.bCanSaveAllSections=true;
 GConfig->SetFile(GGameUserSettingsIni,&Config);ACEInputBindings::Reload();
 ON_SCOPE_EXIT{GGameUserSettingsIni=OriginalSettings;ACEInputBindings::Reload();ACERuntimeOptions::Apply();};
 const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
  .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
 auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
 auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
 auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);
 Context.OwningGameInstance=GI;Context.SetCurrentWorld(World);GI->Init();
 auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();auto* Client=GI->GetSubsystem<UACEClientSubsystem>();
 auto Session=Client->GetSession();
 FString DatDir=TEXT("C:/Turbine/Asheron's Call");
 FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDir);
 if(!Dat->LoadDatDirectory(DatDir))return false;

 // Independent evidence: measure the position frames from the retail human
 // RunForward animation, rather than deriving expected travel from our helper.
 TArray<uint8> Blob;Dat->GetPortalDat()->ReadFile(0x03000002,Blob);
 FACEDatCursor Cursor(Blob);FACEDatAnimation Animation;
 if(!ACEDatUnpack::UnpackAnimation(Cursor,Animation))return false;
 FVector Offset=FVector::ZeroVector;Cursor.Seek(16);
 for(uint32 I=0;I<Animation.NumFrames;++I)
 {
  FVector3f Delta; if(!Cursor.Read(Delta)||!Cursor.Skip(16))return false;
  Offset+=FVector(Delta);
 }
 const double DatRunSpeed=Offset.Size2D()*30.0/Animation.NumFrames;
 TestTrue(TEXT("Retail human RunForward root motion averages 4 metres/sec"),FMath::IsNearlyEqual(DatRunSpeed,4.,.00002));

 auto* PC=World->SpawnActor<AACEPlayerController>();PC->Client=Client;
 PC->SetPlayer(NewObject<ULocalPlayer>(GEngine));World->AddController(PC);
 auto* Pawn=World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
 Capsule->InitCapsuleSize(48,88);Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);Capsule->RegisterComponent();
 Pawn->SetActorEnableCollision(false);PC->Possess(Pawn);PC->PlayerInput=NewObject<UPlayerInput>(PC);PC->SetupInputComponent();
 PC->bRetailCursorInstalled=true;PC->HoverTooltipWidget=CreateWidget<UACEHoverTooltipWidget>(GI,UACEHoverTooltipWidget::StaticClass());
 PC->bEnterWorldLoading=false;PC->bWorldRevealActive=false;
 Session->PlayerGuid=12345;Session->State=EACESessionState::InWorld;
 FACEWorldObject Self;Self.Guid=12345;Self.SetupId=0x02000001;Self.bIsPlayer=true;Self.bIsSelf=true;
 Session->WorldObjects.Add(Self.Guid,Self);
 auto* Floor=World->SpawnActor<AActor>();auto* Box=NewObject<UBoxComponent>(Floor);
 Floor->SetRootComponent(Box);Floor->AddInstanceComponent(Box);Box->SetBoxExtent(FVector(8000,8000,50));
 Box->SetCollisionResponseToAllChannels(ECR_Block);Box->RegisterComponent();Floor->SetActorLocation(FVector(-25000,25000,9950));
 const FVector StartFeet(-24000,25000,10000);
 auto* VR=NewObject<UACEVRComponent>(Pawn);Pawn->AddInstanceComponent(VR);VR->RegisterComponent();
 VR->PC=PC;VR->Client=Client;VR->Settings=NewObject<UACEVRSettings>();VR->ActivateRig();
 VR->bTracking=true;VR->Settings->MovementSmoothing=0;VR->Settings->bRun=true;VR->Settings->MovementScale=1;
 VR->Settings->ForwardAssistDegrees=10;VR->Settings->MovementDirection=0;
 PC->InputComponent->AxisBindings.Reset();
 // Retail CommandList::AddCommand/NukeCommand stack opposing directions,
 // instead of summing them to zero. Use the actual controller event path.
 ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
 auto Key=[&](FKey K,bool Down)
 {PC->InputKey(FInputKeyEventArgs::CreateSimulated(K,Down?IE_Pressed:IE_Released,Down?1.f:0.f));
  // This isolated world has no viewport input routing; feed its PlayerInput
  // as well, while retaining the controller's physical event-order capture.
  PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(K,Down?IE_Pressed:IE_Released,Down?1.f:0.f));
  PC->PlayerInput->ProcessInputStack({},1.f/90,false);};
 struct FAxisCase {FKey Positive,Negative,PositiveKey,NegativeKey;};
 for(const auto& Pair:{FAxisCase{EKeys::W,EKeys::S,EKeys::W,EKeys::X},
  {EKeys::E,EKeys::Q,EKeys::C,EKeys::Z},{EKeys::D,EKeys::A,EKeys::D,EKeys::A}})
 {
  Key(Pair.PositiveKey,true);Key(Pair.NegativeKey,true);
  TestEqual(TEXT("Newest opposing movement command takes priority"),ACEInputBindings::MovementAxis(PC,Pair.Positive,Pair.Negative,PC->MovementKeyPressOrder),-1.f);
  Key(Pair.NegativeKey,false);
  TestEqual(TEXT("Releasing it restores the previously held command"),ACEInputBindings::MovementAxis(PC,Pair.Positive,Pair.Negative,PC->MovementKeyPressOrder),1.f);
  Key(Pair.PositiveKey,false);
 }
 Key(EKeys::C,true);Key(EKeys::A,true);
 TestEqual(TEXT("Mouse-facing turn and strafe keys share newest-command priority"),
  ACEInputBindings::MovementAxis(PC,EKeys::E,EKeys::Q,PC->MovementKeyPressOrder,EKeys::D,EKeys::A),-1.f);
 Key(EKeys::A,false);
 TestEqual(TEXT("Mouse-facing release restores the held strafe"),
  ACEInputBindings::MovementAxis(PC,EKeys::E,EKeys::Q,PC->MovementKeyPressOrder,EKeys::D,EKeys::A),1.f);
 Key(EKeys::C,false);
 ACEInputBindings::BeginEdit();ACEInputBindings::Set(EKeys::S,0,FInputChord(EKeys::F2));ACEInputBindings::Commit();
 Key(EKeys::W,true);Key(EKeys::F2,true);
 TestEqual(TEXT("Command order follows a rebound physical backward key"),
  ACEInputBindings::MovementAxis(PC,EKeys::W,EKeys::S,PC->MovementKeyPressOrder),-1.f);
 Key(EKeys::F2,false);Key(EKeys::W,false);
 ACEInputBindings::BeginEdit();ACEInputBindings::Defaults();ACEInputBindings::Commit();
 auto* App=NewObject<UACECharacterAppearanceComponent>(Pawn);Pawn->AddInstanceComponent(App);App->RegisterComponent();
 App->PlayActionMotion(0x40000034,2.f,ACEMotion::StanceMagic);
 App->SetLocomotionInput(0,1,true,1);
 TestEqual(TEXT("Strafe retains the active casting gesture"),App->ActionCommand,0x40000034u);
 // SetLocomotionInput is deliberately shared with remote presentation;
 // only the local command edge may interrupt a server casting substate.
 App->InterruptCastWithMovement();
 TestEqual(TEXT("New local forward command preserves the unfinished cast link"),App->ActionCommand,0x40000034u);
 App->bHoldActionFinal=true;
 App->InterruptCastWithMovement();
 TestEqual(TEXT("New local forward command removes the held casting tail"),App->ActionCommand,0u);
 App->PlayActionMotion(0x1000006F,2.f,ACEMotion::StanceMagic);
 App->InterruptCastWithMovement();
 TestEqual(TEXT("Movement does not truncate a queued scarab windup action"),App->ActionCommand,0x1000006Fu);
 App->ClearActionMotion();
 App->PlayActionMotion(0x40000034,2.f,ACEMotion::StanceMagic);
 App->QueueActionMotion(0x1000006F,2.f,ACEMotion::StanceMagic);
 App->InterruptCastWithMovement();
 TestEqual(TEXT("Fast buff movement retains the following scarab action"),App->PendingActionCommands.Num(),1);
 App->ClearActionMotion();
 App->PlayActionMotion(0x40000034,2.f,ACEMotion::StanceMagic);
 App->PlayActionMotion(0x1000006F,2.f,ACEMotion::StanceMagic);
 TestEqual(TEXT("A quick next buff cannot truncate the preceding cast link"),App->ActionCommand,0x40000034u);
 TestEqual(TEXT("The next windup waits behind the unfinished cast link"),App->PendingActionCommands.Num(),1);
 App->ClearActionMotion();
 // Retail TakeControlFromServer reapplies the whole command state even if
 // a cast arrives after strafe/turn were sent. A wire-level replay catches
 // the race that axis-only tests miss, as well as a swallowed key release.
 {
  auto* Sockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
  auto* Receiver=Sockets->CreateSocket(NAME_DGram,TEXT("Cast movement receiver"),false);
  ON_SCOPE_EXIT {if(Receiver){Receiver->Close();Sockets->DestroySocket(Receiver);}};
  auto Address=Sockets->CreateInternetAddr();bool Valid=false;
  Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(0);
  if(!Receiver||!Receiver->Bind(*Address))return false;
  Receiver->GetAddress(*Address);
  Session->SocketC2S=Sockets->CreateSocket(NAME_DGram,TEXT("Cast movement sender"),false);
  Session->ServerC2SAddr=Address;Session->IssacClient=MakeUnique<FACEIsaac>(123u);
  ON_SCOPE_EXIT {
   Session->SocketC2S->Close();Sockets->DestroySocket(Session->SocketC2S);Session->SocketC2S=nullptr;
   Session->CachedC2SPackets.Reset();Session->IssacClient.Reset();
  };
  auto CheckMovement=[&](bool Side,bool Turn)
  {
   if(!TestEqual(TEXT("One retail movement update per command transition"),Session->CachedC2SPackets.Num(),1))return;
   const auto& Bytes=Session->CachedC2SPackets.CreateConstIterator()->Value.Payload;
   FACEBinaryReader Reader(Bytes);Reader.Skip(24);
   TestEqual(TEXT("Control handoff uses MoveToState"),Reader.ReadUInt32(),ACEGameAction::MoveToState);
   const uint32 Flags=Reader.ReadUInt32();
   TestEqual(TEXT("Packet contains the current sidestep"),bool(Flags&ACERawMotionFlags::SideStepCommand),Side);
   TestEqual(TEXT("Packet contains the current turn"),bool(Flags&ACERawMotionFlags::TurnCommand),Turn);
   TestEqual(TEXT("Slide-cast does not fabricate action commands"),Flags>>11,0u);
   // Standard movement payload ends with four physics stamps and contact.
   FACEBinaryReader Tail(Bytes);Tail.Skip(Bytes.Num()-12);
   Tail.ReadUInt16();
   TestEqual(TEXT("Movement acknowledges the newest server control stamp"),Tail.ReadUInt16(),Session->ServerControlSeq);
  };
  VR->bActive=false;Session->ApplyServerCombatMode(8);Session->SetCurrentStance(ACEMotion::StanceMagic);
  Session->PlayerVitals.bValid=true;Session->PlayerVitals.RunSkillCurrent=593;
  Session->PlayerVitals.Health=Session->PlayerVitals.MaxHealth=500;
  Session->PlayerVitals.Stamina=Session->PlayerVitals.MaxStamina=500;
  FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
  Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);
  for(int32 Rate:{30,60,144,240})for(int32 Mode:{0,1,2})for(bool Late:{false,true})
  {
   const bool Side=Mode!=1,Turn=Mode!=0;
   PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
   Session->SetLocalPosition(Pose);Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
   PC->bJumpAirborne=PC->bStandingJumpLocked=false;
   auto Press=[&](bool Down){if(Side)Key(EKeys::C,Down);if(Turn)Key(EKeys::A,Down);};
   FACEObjectMotionState Cast;Cast.CurrentStyle=ACEMotion::StanceMagic;Cast.ActionCommand=0x40000034;
   auto ReceiveCast=[&]{++Session->ServerControlSeq;PC->HandleMotionUpdate(Self.Guid,Cast);};
   if(!Late)ReceiveCast();
   Press(true);PC->PlayerTick(1.f/Rate);
   if(Late)ReceiveCast();
   else {Press(false);PC->PlayerTick(1.f/Rate);ReceiveCast();Press(true);}
   Session->CachedC2SPackets.Reset();
   const FVector Before=Pawn->GetActorLocation();const FQuat Facing=Pawn->GetActorQuat();
   Session->bUseBusy=true;
   PC->PlayerTick(1.f/Rate);
   CheckMovement(Side,Turn);
   TestFalse(TEXT("Strafe/turn claims server movement control"),PC->bServerControlledMotion);
   TestEqual(TEXT("Control handoff preserves the unfinished cast link while sliding"),App->ActionCommand,0x40000034u);
   if(Side)TestTrue(TEXT("Strafing continues immediately after the cast update"),FVector::Dist2D(Before,Pawn->GetActorLocation())>1.);
   if(Turn)TestFalse(TEXT("Turning continues immediately after the cast update"),Pawn->GetActorQuat().Equals(Facing,.00001));
   Session->CachedC2SPackets.Reset();PC->PlayerTick(1.f/Rate);
   TestTrue(TEXT("Unchanged held input is not resent every render frame"),Session->CachedC2SPackets.IsEmpty());
   Press(false);Session->CachedC2SPackets.Reset();PC->PlayerTick(1.f/Rate);
   CheckMovement(false,false);
   TestFalse(TEXT("Releasing movement while cast is pending stops server movement"),Session->bMoving);
   Session->CachedC2SPackets.Reset();PC->PlayerTick(1.f/Rate);
   TestTrue(TEXT("Pending cast does not repeatedly send a release"),Session->CachedC2SPackets.IsEmpty());
   Session->bUseBusy=false;
  }
  // Casting retains the responsive forward/backward movement from before
  // the combat-link restriction, including late server motion updates.
  for(int32 Rate:{30,144}) for(bool Late:{false,true}) for(bool Back:{false,true})
  {
   App->ClearActionMotion();App->ResetWalkingMotion();
   PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
   Session->SetLocalPosition(Pose);Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
   FACEObjectMotionState Cast;Cast.CurrentStyle=ACEMotion::StanceMagic;Cast.ActionCommand=0x40000034;
   const FKey MoveKey=Back?EKeys::X:EKeys::W;
   if(Late){Key(MoveKey,true);PC->PlayerTick(1.f/Rate);}
   ++Session->ServerControlSeq;PC->HandleMotionUpdate(Self.Guid,Cast);
   if(!Late)Key(MoveKey,true);
   Session->CachedC2SPackets.Reset();const FVector Before=Pawn->GetActorLocation();
   PC->PlayerTick(1.f/Rate);
   if(TestEqual(TEXT("Casting forward handoff sends one movement packet"),Session->CachedC2SPackets.Num(),1))
   {
    FACEBinaryReader Wire(Session->CachedC2SPackets.CreateConstIterator()->Value.Payload);Wire.Skip(24);
    TestEqual(TEXT("Casting handoff uses MoveToState"),Wire.ReadUInt32(),ACEGameAction::MoveToState);
    TestTrue(TEXT("Raw forward intent is preserved on the wire"),(Wire.ReadUInt32()&ACERawMotionFlags::ForwardCommand)!=0);
   }
   for(int32 I=0;I<4;++I)PC->PlayerTick(1.f/Rate);
   const float ExpectedSpeed=Back?Client->GetLocomotionSpeed(false)*Client->GetRunRate()*.65f:Client->GetLocomotionSpeed(true);
   TestTrue(TEXT("Casting movement travels at the normal requested speed"),
    FMath::IsNearlyEqual(Pawn->GetActorLocation().X-Before.X,(Back?-1.f:1.f)*ExpectedSpeed*100.f*5.f/Rate,.05));
   TestEqual(TEXT("Walking during casting retains its animation"),App->ActionCommand,Cast.ActionCommand);
   TestFalse(TEXT("Reapplied held forward does not latch the cast's cyclic tail"),App->bHoldActionFinalAfterFinish);
   Session->CachedC2SPackets.Reset();PC->PlayerTick(1.f/Rate);
   TestTrue(TEXT("Held casting movement does not spam movement packets"),Session->CachedC2SPackets.IsEmpty());
   const FVector ReleasePosition=Pawn->GetActorLocation();
   Key(MoveKey,false);Session->CachedC2SPackets.Reset();PC->PlayerTick(1.f/Rate);
   CheckMovement(false,false);
   TestFalse(TEXT("Releasing forward/backward stops network movement during casting"),Session->bMoving);
   TestTrue(TEXT("Releasing forward/backward stops local casting movement"),FVector::Dist2D(ReleasePosition,Pawn->GetActorLocation())<.01);
   App->ClearActionMotion();
  }
  // Start with an already transmitted move owned by automation. Its release
  // must survive multiple frozen ticks without cancelling the pending use.
  Key(EKeys::W,true);PC->PlayerTick(1.f/90);
  PC->bManualMovementCommandSent=false;Session->bUseBusy=true;Key(EKeys::W,false);
  Session->CachedC2SPackets.Reset();
  PC->PlayerTick(1.f/90);PC->PlayerTick(1.f/90);
  TestTrue(TEXT("Frozen release leaves the use chain and last wire move intact"),Session->CachedC2SPackets.IsEmpty() && Session->bReportedTranslation);
  Session->bUseBusy=false;PC->PlayerTick(1.f/90);CheckMovement(false,false);
  TestFalse(TEXT("First free tick stops the previously reported forward motion"),Session->bReportedTranslation);
  Session->CachedC2SPackets.Reset();PC->PlayerTick(1.f/90);
  TestTrue(TEXT("Deferred stop is sent exactly once"),Session->CachedC2SPackets.IsEmpty());

  Key(EKeys::W,true);PC->PlayerTick(1.f/90);
  FACEObjectMotionState Read;Read.ForwardCommand=0x1c;Read.CurrentStyle=ACEMotion::StanceMagic;
  PC->HandleMotionUpdate(Self.Guid,Read);
  Session->CachedC2SPackets.Reset();Key(EKeys::W,false);PC->PlayerTick(1.f/90);
  TestTrue(TEXT("A reading substate can suspend a reported walk without cancelling the book"),Session->CachedC2SPackets.IsEmpty() && Session->bReportedTranslation);
  PC->HandleUseDone(0);PC->PlayerTick(1.f/90);CheckMovement(false,false);
  TestFalse(TEXT("Closing the book reconciles the released walk"),Session->bReportedTranslation);

  Session->SetLocalMissileTravel(false);
  for (bool Strict : {false,true})
  {
   ACERuntimeOptions::Set(TEXT("StationaryMissileAttacks"),Strict ? 1 : 0);ACERuntimeOptions::Apply();
   auto CheckShot=[&](float F,float R,float T,bool Ground,bool Traveling)
   {
    Session->SendMoveToState(F,R,T,true,Ground);Session->CachedC2SPackets.Reset();
    Session->bServerAttackInProgress=false;
    Session->SendTargetedMissileAttack(101,ACEAttackHeight::Medium,.5f);
    const bool Allowed=!Strict || !Traveling;
    TestEqual(TEXT("Missile packet respects the player's movement preference"),Session->CachedC2SPackets.Num(),Allowed?1:0);
    TestEqual(TEXT("Only a transmitted shot starts attack tracking"),Session->IsServerAttackInProgress(),Allowed);
   };
   CheckShot(1,0,0,true,true);CheckShot(-1,0,0,true,true);CheckShot(0,1,0,true,true);
   CheckShot(0,0,0,false,true);CheckShot(0,0,1,true,false);
   const uint32 PreviousCaps=Session->VRCapabilities;Session->VRCapabilities|=7u;
   for (bool Walking : {true,false})
   {
    Session->SendMoveToState(Walking?1.f:0.f,0,0,true,true);
    TestEqual(TEXT("Tracked missile release follows the same option"),
     Session->SendVRCombat(3,Pose.CellId,200,0,101,FVector(0,0,1),FVector::YAxisVector,1,0),!Strict || !Walking);
   }
   Session->VRCapabilities=PreviousCaps;
   Session->SetLocalMissileTravel(true);CheckShot(0,0,0,true,true);
   Session->SetLocalMissileTravel(false);CheckShot(0,0,0,true,false);
  }
  ACERuntimeOptions::Set(TEXT("StationaryMissileAttacks"),0);ACERuntimeOptions::Apply();
  Session->bServerAttackInProgress=false;
  // Strafe alone must not discard locally controlled emotes. Reclaiming
  // movement control must also preserve unfinished authored actions.
  PC->bServerControlledMotion=false;App->SetHeldActionMotion(0x43000145u,ACEMotion::StanceNonCombat);
  Key(EKeys::C,true);PC->PlayerTick(1.f/90);
  TestEqual(TEXT("Locally controlled Play Possum still slides"),App->ActionCommand,0x43000145u);
  Key(EKeys::C,false);PC->PlayerTick(1.f/90);App->ClearActionMotion();
  FACEObjectMotionState Power;Power.CurrentStyle=ACEMotion::StanceMagic;Power.ActionCommand=0x1000006F;
  PC->HandleMotionUpdate(Self.Guid,Power);Key(EKeys::C,true);PC->PlayerTick(1.f/90);
  TestEqual(TEXT("Movement handoff preserves the scarab windup action"),App->ActionCommand,0x1000006Fu);
  Key(EKeys::C,false);PC->PlayerTick(1.f/90);App->ClearActionMotion();
 }
 // A pending cast marks the session use-busy. A complete native buff cycle
 // must cancel cast substates without translating even at low frame rates.
 {
  auto* Plugins=GI->GetSubsystem<UACEPluginSubsystem>();VR->bActive=false;
  FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
  Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
  PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
  Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
  Session->bUseBusy=true;Session->bMoving=false;Session->ApplyServerCombatMode(8);Session->SetCurrentStance(ACEMotion::StanceMagic);
  Session->PlayerVitals.bValid=true;Session->PlayerVitals.RunSkillCurrent=800;
  Session->PlayerVitals.Stamina=Session->PlayerVitals.MaxStamina=500;
  auto ArmTap=[&]()
  {
   Plugins->PendingSpell=123;Plugins->PendingSpellAt=FPlatformTime::Seconds();
   Plugins->PendingSpellConfirmed=false;Plugins->PendingSpellFizzled=false;
   Plugins->FastCastOwner=TEXT("ucm");Plugins->FastCastStarted=true;
   Plugins->FastCastStartedAt=FPlatformTime::Seconds();Plugins->FastCastMovementApplied=false;
  };
  for(int32 Rate:{10,30,60,90,144,240})
  {
   const FVector Before=Pawn->GetActorLocation();
   for(int32 Buff=0;Buff<40;++Buff)
   {
    ArmTap();App->ClearActionMotion();App->PlayActionMotion(0x40000034,2.f,ACEMotion::StanceMagic);
    PC->PlayerTick(1.f/Rate);
    TestTrue(TEXT("Buff tap consumed while session is use-busy"),Plugins->FastCastMovementApplied);
    TestEqual(TEXT("Fast buff edge preserves the unfinished local cast link"),App->ActionCommand,0x40000034u);
    TestEqual(TEXT("Fast buff never sets a held backward axis"),PC->ForwardAxis,0.f);
    TestFalse(TEXT("Release reaches session in the same frame"),Session->bMoving);
    // Neither a lost result nor additional render frames may move the player.
    for(int32 Frame=0;Frame<4;++Frame)PC->PlayerTick(1.f/Rate);
   }
   TestTrue(FString::Printf(TEXT("40 buffs at %d Hz leave the player in place"),Rate),
    Pawn->GetActorLocation().Equals(Before,.01));
  }
  ArmTap();App->ClearActionMotion();App->PlayActionMotion(0x1000006F,2.f,ACEMotion::StanceMagic);
  PC->PlayerTick(1.f/90);
  TestEqual(TEXT("Tap does not skip a scarab windup action"),App->ActionCommand,0x1000006Fu);
  App->ClearActionMotion();
  ArmTap();float F=0,R=0,T=0;
  Plugins->ApplyMovement(PC,F,R,T,false,true,false,FVector::ForwardVector);
  TestFalse(TEXT("Blocked UI cannot dispatch a tap"),Plugins->FastCastMovementApplied);
  Plugins->FastCastStartedAt=FPlatformTime::Seconds()-.13;
  Plugins->ApplyMovement(PC,F,R,T,false,false,false,FVector::ForwardVector);
  TestTrue(TEXT("Unblocking cannot replay an expired tap"),!Plugins->FastCastMovementApplied&&Plugins->FastCastOwner.IsEmpty());
  ArmTap();F=1;
  Plugins->ApplyMovement(PC,F,R,T,true,false,false,FVector::ForwardVector);
  TestTrue(TEXT("Manual movement overrides the tap"),F==1&&!Plugins->FastCastMovementApplied&&Plugins->FastCastOwner.IsEmpty());
  PC->bJumpCharging=true;TestFalse(TEXT("Tap cannot interrupt jump charge"),PC->TryFastBuffTap());PC->bJumpCharging=false;
  PC->bJumpAirborne=true;TestFalse(TEXT("Tap cannot interrupt a jump"),PC->TryFastBuffTap());PC->bJumpAirborne=false;
  PC->bEnterWorldLoading=true;TestFalse(TEXT("Tap cannot run during portal loading"),PC->TryFastBuffTap());PC->bEnterWorldLoading=false;
  PC->bServerMoveToActive=true;TestFalse(TEXT("Tap cannot interrupt a use approach"),PC->TryFastBuffTap());PC->bServerMoveToActive=false;
  Session->SetCurrentStance(ACEMotion::StanceNonCombat);
  TestFalse(TEXT("Tap cannot report a stale non-magic stance while buffing"),PC->TryFastBuffTap());
  Session->SetCurrentStance(ACEMotion::StanceMagic);
  Session->bUseBusy=false;Plugins->PendingSpell=0;PC->PlayerTick(1.f/90);
 }
 struct FCase { int32 Skill; float Scale,Burden; int32 Stamina; double Speed; };
 const FCase Cases[]={
  {593,1,0,500,12.2257250946},{593,1.1f,0,500,13.4482976041},
  {799,1,0,500,12.7977977978},{800,1,0,500,18.0},
  {801,1,0,500,12.8021978022},{1000,1,0,500,13.1666666667},
  {593,1,1.5f,500,8.1128625473},{800,1,0,0,4.0},{0,1,0,500,4.0}};
 for(bool bVR:{false,true})for(int32 Rate:{30,72,90,144})for(const auto& C:Cases)
 {
  VR->bActive=bVR;VR->bInventoryOpen=VR->bSettingsOpen=VR->bKeyboardOpen=false;
  VR->MoveStick=FVector2D(.04f,.93f);VR->SmoothedMoveStick=FVector2D::ZeroVector;
  Session->WorldObjects[Self.Guid].Scale=C.Scale;
  Session->PlayerVitals.bValid=true;Session->PlayerVitals.RunSkillCurrent=C.Skill;
  Session->PlayerVitals.Health=Session->PlayerVitals.MaxHealth=500;
  Session->PlayerVitals.Stamina=C.Stamina;Session->PlayerVitals.MaxStamina=500;
  Session->OnVitalsUpdated.Broadcast(Session->PlayerVitals);Client->SetBurden(C.Burden);
  FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
  Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
  PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
  PC->bLocalPredicting=false;PC->bJumpAirborne=false;PC->bStandingJumpLocked=false;PC->StepHoldSeconds=0;
  Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
  PC->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W,IE_Pressed,1.f));
  PC->PlayerInput->ProcessInputStack({},1.f/Rate,false);
  for(int32 Frame=0;Frame<Rate;++Frame)
  {
   VR->Head->SetWorldLocationAndRotation(Pawn->GetActorLocation()+FVector(0,0,77),FRotator::ZeroRotator);
   PC->PlayerTick(1.f/Rate);
  }
  const double Actual=FVector::Dist2D(StartFeet,Pawn->GetActorLocation())/100.;
  const FString Label=FString::Printf(TEXT("%s %dHz run=%d size=%.2f burden=%.1f stamina=%d: %.5fm expected %.5fm"),
   bVR?TEXT("VR full stick"):TEXT("Desktop W"),Rate,C.Skill,C.Scale,C.Burden,C.Stamina,Actual,C.Speed);
  AddInfo(Label);TestTrue(*Label,FMath::Abs(Actual-C.Speed)<.006);
  TestFalse(TEXT("Flat-ground run remains grounded"),PC->bJumpAirborne);
  PC->PlayerInput->FlushPressedKeys();PC->PlayerInput->ProcessInputStack({},1.f/Rate,false);
 }
 for(bool Run:{false,true})
 {
  VR->bActive=false;Session->WorldObjects[Self.Guid].Scale=1;
  Session->PlayerVitals.RunSkillCurrent=593;Session->PlayerVitals.Stamina=500;
  Session->OnVitalsUpdated.Broadcast(Session->PlayerVitals);Client->SetBurden(0);
  FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
  Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
  PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
  PC->bJumpAirborne=false;PC->bStandingJumpLocked=false;PC->StepHoldSeconds=0;
  Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
  Key(EKeys::X,true);if(!Run)Key(EKeys::LeftShift,true);
  for(int Frame=0;Frame<90;++Frame)PC->PlayerTick(1.f/90);
  const double Expected=3.12*.65*(Run?12.2257250946/4.:1.);
  AddInfo(FString::Printf(TEXT("Backpedal run=%d actual=%.6f expected=%.6f"),Run,FVector::Dist2D(StartFeet,Pawn->GetActorLocation())/100.,Expected));
  TestTrue(TEXT("Backward movement uses negative WalkForward, including Run hold rate"),
   FMath::Abs(FVector::Dist2D(StartFeet,Pawn->GetActorLocation())/100.-Expected)<.006);
  Key(EKeys::X,false);
  PC->PredictedPose=Pose;Session->SetLocalPosition(Pose);PC->bHaveLastServerPose=false;
  Key(EKeys::D,true);
  for(int Frame=0;Frame<90;++Frame)PC->PlayerTick(1.f/90);
  const float Yaw=FMath::RadiansToDegrees(Pose.GetAcQuat().AngularDistance(PC->PredictedPose.GetAcQuat()));
  TestTrue(TEXT("Local turning agrees with retail turn modifier and Run multiplier"),
   FMath::Abs(Yaw-FMath::RadiansToDegrees(1.5f)*(Run?1.5f:1.f))<.02f);
  Key(EKeys::D,false);if(!Run)Key(EKeys::LeftShift,false);
 }
 // Leave-ground inherits the same interpreted backward/side speed as a run.
 // Cover the direction missed by the forward-only arc tests below.
 for(bool Run:{false,true})for(float Size:{.8f,1.2f})for(float Side:{-1.f,0.f,1.f})
 {
  VR->bActive=false;Session->bHasPlayerEncumbrance=false;Client->SetBurden(0);
  Session->WorldObjects[Self.Guid].Scale=Size;Session->PlayerVitals.Stamina=500;Client->SetRunSkill(593);
  PC->bJumpAirborne=false;PC->bStandingJumpLocked=false;PC->bRunning=Run;
  PC->bJumpCharging=true;PC->JumpChargeExtent=.5f;PC->ReleaseJump(-1,Side);
  const double Back=-3.12*.65*(Run?12.2257250946/4.:1.);
  TestTrue(TEXT("Backward jump carries the retail run modifier independent of body size"),
   FMath::Abs(PC->JumpLocalAceVelocity.Y-Back)<.00001);
  TestTrue(TEXT("Diagonal back jump retains the capped sidestep component"),
   FMath::Abs(PC->JumpLocalAceVelocity.X-Side*(Run?3.75:1.56))<.00001);
 }
 PC->bJumpAirborne=false;PC->bJumpCharging=false;PC->bStandingJumpLocked=false;
 PC->ForwardAxis=PC->RightAxis=0;
 Client->SetBurden(2.f);PC->BeginJumpCharge();
 TestFalse(TEXT("Retail 200 percent burden gate prevents charging a jump"),PC->bJumpCharging);
 Client->SetBurden(1.99f);PC->BeginJumpCharge();
 TestTrue(TEXT("Just below 200 percent still permits jump charging"),PC->bJumpCharging);
 Client->SetBurden(2.1f);PC->JumpChargeExtent=1.f;PC->ReleaseJump(1,0);
 TestFalse(TEXT("Becoming overloaded during the charge does not predict a rejected jump"),PC->bJumpAirborne);
 TestFalse(TEXT("Rejected jump releases standing-charge lock"),PC->bStandingJumpLocked);
 TestFalse(TEXT("Rejected jump clears charge"),PC->bJumpCharging);
 Client->SetBurden(0);
 // Retail CMotionInterp::get_state_velocity / CACQualities::InqJumpVelocity:
 // the airborne velocity is not scaled by creature size like grounded root motion.
 for(bool Tracked:{false,true})for(int Rate:{30,90,144})for(float Size:{1.f,1.1f})for(float Extent:{.25f,1.f})
 {
  VR->bActive=Tracked;VR->MoveStick=FVector2D(0,1);VR->Settings->bRun=true;
  Session->WorldObjects[Self.Guid].Scale=Size;Session->bHasPlayerEncumbrance=false;Client->SetBurden(0);
  Session->PlayerVitals.RunSkillCurrent=593;Session->PlayerVitals.JumpSkillCurrent=400;
  Session->PlayerVitals.Stamina=500;Session->OnVitalsUpdated.Broadcast(Session->PlayerVitals);
  FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
  Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
  PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
  PC->bStandingJumpLocked=false;PC->StepHoldSeconds=0;PC->bRunning=true;
  Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
  PC->bJumpCharging=true;PC->JumpChargeExtent=Extent;PC->ReleaseJump(1,0);
  const double Height=(400./1700.*22.2+.05)*Extent;
  const double Flight=2*FMath::Sqrt(Height*19.6)/9.8;
  double Peak=0;int Frames=0;
  for(;Frames<Rate*5 && PC->bJumpAirborne;++Frames)
  {
   VR->Head->SetWorldLocationAndRotation(Pawn->GetActorLocation()+FVector(0,0,77),FRotator::ZeroRotator);
   PC->PlayerTick(1.f/Rate);
   Peak=FMath::Max(Peak,(Pawn->GetActorLocation().Z-88-StartFeet.Z)/100.);
  }
  const double Distance=FVector::Dist2D(StartFeet,Pawn->GetActorLocation())/100.;
  TestTrue(TEXT("Jump apex agrees with retail skill/charge formula"),FMath::Abs(Peak-Height)<.02);
  TestTrue(TEXT("Jump duration is frame-rate independent"),FMath::Abs(double(Frames)/Rate-Flight)<2./Rate);
  TestTrue(TEXT("Jump range agrees with retail leave-ground run speed"),FMath::Abs(Distance-12.2257250946*Flight)<12.2257250946*2./Rate+.02);
  TestFalse(TEXT("Completed jump releases airborne animation and input"),PC->bJumpAirborne);
  const FVector Touchdown=Pawn->GetActorLocation();
  // A 30 Hz VR frame contains three movement substeps; its final substep
  // may already have coasted after contact. Start this comparison at the
  // sampled pose/velocity, rather than counting that time a second time.
  const double TouchdownSpeed=PC->LandingWorldAceVelocity.Size2D();
  VR->MoveStick=FVector2D::ZeroVector;
  TestTrue(TEXT("Landing preserves horizontal physics velocity"),PC->LandingWorldAceVelocity.Size2D()>10.);
  for(int I=0;I<Rate/2;++I)
  {
   VR->Head->SetWorldLocation(Pawn->GetActorLocation()+FVector(0,0,77));PC->PlayerTick(1.f/Rate);
  }
  const double Glide=FVector::Dist2D(Touchdown,Pawn->GetActorLocation())/100.;
  TestTrue(TEXT("Retail friction permits a brief glide instead of gluing feet at contact"),Glide>2.5 && Glide<3.5);
  // Independent CPhysicsObj::UpdatePhysicsInternal reference: friction is
  // applied before displacement, and the pre-friction speed owns the cutoff.
  double ReferenceSpeed=TouchdownSpeed,ReferenceDistance=0;
  for(int I=0;I<Rate/2;++I)
  {
   ReferenceSpeed=ReferenceSpeed*ReferenceSpeed < .0625+.0002 ? 0 : ReferenceSpeed*FMath::Pow(.05,1./Rate);
   ReferenceDistance+=ReferenceSpeed/Rate;
  }
  TestTrue(TEXT("Half-second glide matches retail friction, not an arbitrary distance multiplier"),FMath::Abs(Glide-ReferenceDistance)<.025);
  for(int I=Rate/2;I<Rate*4;++I)
  {
   ReferenceSpeed=ReferenceSpeed*ReferenceSpeed < .0625+.0002 ? 0 : ReferenceSpeed*FMath::Pow(.05,1./Rate);
   ReferenceDistance+=ReferenceSpeed/Rate;
   VR->Head->SetWorldLocation(Pawn->GetActorLocation()+FVector(0,0,77));PC->PlayerTick(1.f/Rate);
  }
  const double FullGlide=FVector::Dist2D(Touchdown,Pawn->GetActorLocation())/100.;
  TestTrue(TEXT("Full coast reaches the retail friction distance without early cancellation"),FMath::Abs(FullGlide-ReferenceDistance)<.025);
  TestTrue(TEXT("Coast ends at retail's small-velocity cutoff"),PC->LandingWorldAceVelocity.IsNearlyZero());
  TestFalse(TEXT("Landing glide does not re-enter airborne animation"),PC->bJumpAirborne);
  AddInfo(FString::Printf(TEXT("Jump tracked=%d rate=%d scale=%.1f charge=%.2f apex=%.3fm range=%.3fm time=%.3fs"),Tracked,Rate,Size,Extent,Peak,Distance,double(Frames)/Rate));
 }
 // Authored landing displacement uses the collision root and normal AutoPos,
 // including when the presentation mesh is not being ticked/rendered.
 for(bool Tracked:{false,true})for(int Rate:{30,90,144})for(float Size:{1.f,1.1f})for(bool Blocked:{false,true})
 {
  VR->bActive=Tracked;VR->MoveStick=VR->SmoothedMoveStick=FVector2D::ZeroVector;
  Session->WorldObjects[Self.Guid].Scale=Size;
  FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
  Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
  PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
  PC->bJumpAirborne=false;PC->bStandingJumpLocked=false;PC->LandingWorldAceVelocity=FVector::ZeroVector;
  Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
  TestTrue(TEXT("Run landing track loads from retail DAT"),Dat->BuildTransitionRootTrack(0x09000001,0x40000015,0x44000007,ACEMotion::StanceNonCombat,PC->LandingRootTrack));
  AActor* Wall=nullptr;
  if(Blocked)
  {
   Wall=World->SpawnActor<AActor>();auto* Shape=NewObject<UBoxComponent>(Wall);
   Wall->SetRootComponent(Shape);Wall->AddInstanceComponent(Shape);Shape->SetBoxExtent(FVector(10,500,500));
   Shape->SetCollisionResponseToAllChannels(ECR_Block);Shape->RegisterComponent();Wall->SetActorLocation(StartFeet+FVector(90,0,100));
  }
  for(int I=0;I<Rate;++I)
  {
   VR->Head->SetWorldLocationAndRotation(Pawn->GetActorLocation()+FVector(0,0,77),FRotator::ZeroRotator);
   PC->PlayerTick(1.f/Rate);
  }
  const double Travel=FVector::Dist2D(StartFeet,Pawn->GetActorLocation())/100.;
  if(Blocked)TestTrue(TEXT("Authored landing motion stops against walls instead of bypassing collision"),Travel<.4);
  // CSequence consumes four .152m landing steps, then the run cycle's
  // .133333m entry frame. The held fifth landing frame is not movement.
  else TestTrue(TEXT("Controller applies full size-scaled landing displacement"),FMath::Abs(Travel-.741333*Size)<.003);
  TestTrue(TEXT("Reported position follows the resolved landing feet"),FVector::Dist2D(Client->GetPlayerPosition().ToUnrealLocation(100),Pawn->GetActorLocation())<.01);
  TestFalse(TEXT("Authored landing motion stays grounded"),PC->bJumpAirborne);
  TestFalse(TEXT("Landing track finishes instead of repeating its last frame"),PC->LandingRootTrack.IsActive());
  TestFalse(TEXT("Completed landing releases extra position reporting"),Session->bForcePositionReporting);
  if(Wall)Wall->Destroy();
 }
 // Retail permits charging during flight but checks contact when executing.
 for(bool Tracked:{false,true})
 {
  VR->bActive=Tracked;PC->bJumpAirborne=true;PC->bJumpCharging=false;
  PC->bStandingJumpLocked=false;PC->JumpWorldAceVelocity=FVector(0,4,-2);
  PC->BeginJumpCharge();
  TestTrue(TEXT("Airborne player can charge the next jump"),PC->bJumpCharging);
  PC->JumpChargeExtent=.6f;PC->ReleaseJump(1,0);
  TestTrue(TEXT("Early release does not double-jump or replace flight velocity"),
   PC->bJumpAirborne && PC->JumpWorldAceVelocity.Equals(FVector(0,4,-2)) && !PC->bJumpCharging);
  FACEPosition Landing;Landing.CellId=0x01010001;Landing.SetLocationFromUnreal(StartFeet+FVector(0,0,20),100);
  Session->SetLocalPosition(Landing);PC->PredictedPose=Landing;PC->bHavePredictedPose=true;
  Pawn->SetActorLocation(StartFeet+FVector(0,0,108));VR->MoveStick=FVector2D::ZeroVector;
  PC->BeginJumpCharge();PC->JumpChargeExtent=.6f;
  for(int I=0;I<90 && PC->bJumpAirborne;++I)
  {
   VR->Head->SetWorldLocation(Pawn->GetActorLocation()+FVector(0,0,77));PC->PlayerTick(1.f/90);
  }
  TestTrue(TEXT("Holding through touchdown retains the accumulated charge"),!PC->bJumpAirborne && PC->bJumpCharging && PC->JumpChargeExtent>=.6f);
  PC->ReleaseJump(1,0);
  TestTrue(TEXT("Release after contact launches the charged follow-up jump"),PC->bJumpAirborne && PC->JumpWorldAceVelocity.Z>1.f);
  PC->bJumpAirborne=false;PC->bStandingJumpLocked=false;
 }
 Session->PlayerVitals.Strength=100;Session->PlayerVitals.CarryingCapacityAugs=0;
 Session->PlayerVitals.Stamina=500;Client->SetRunSkill(593);
 Session->bHasPlayerEncumbrance=true;Session->PlayerEncumbranceVal=22500;
 Client->SetBurden(0); // Deliberately stale UI value.
 TestTrue(TEXT("Weight packets affect speed without an inventory-panel refresh"),FMath::IsNearlyEqual(Client->GetLocomotionSpeed(true),8.1128625f,.00001f));
 Session->PlayerVitals.CarryingCapacityAugs=5;
 TestTrue(TEXT("Carrying augmentations use the server/retail capacity"),FMath::IsNearlyEqual(Client->GetLocomotionSpeed(true),12.225725f,.00001f));
 VR->bActive=false;
 // Strafe cadence must use the interpreted SideStep speed, independently of
 // the forward run rate. Exercise the real controller and compare its pose
 // with the authored DAT cycle and a server UpdateMotion for another player.
 TFunction<void()> CheckStrafe = [&]
 {
  Self.MotionTableId=0x09000001;Session->WorldObjects[Self.Guid].Scale=1.f;
  if(!TestTrue(TEXT("Strafe avatar builds"),App->ApplyWorldObject(Self,100,false)))return;
  App->SetPreviewCapture(true);
  auto Other=Self;Other.Guid=23456;Other.bIsSelf=false;
  Session->WorldObjects.Add(Other.Guid,Other);
  auto* Remote=World->SpawnActor<AACEWorldEntityActor>();Remote->InitializeFromObject(Other,100,true);
  auto* RemoteApp=Remote->Appearance.Get();RemoteApp->SetPreviewCapture(true);
  if(!TestEqual(TEXT("Remote strafe avatar has the same parts"),RemoteApp->GetPartCount(),App->GetPartCount()))return;
  FACEObjectMotionState Received;
  const auto Handle=Session->OnMotionUpdate.AddLambda([&](int32 Guid,const FACEObjectMotionState& Motion)
   {if(Guid==Other.Guid){Received=Motion;Remote->ApplyMotionState(Motion);}});
  ON_SCOPE_EXIT{Session->OnMotionUpdate.Remove(Handle);Remote->Destroy();};
  struct FStrafeCase {int32 Skill,Stamina;float Burden,RunSideRate;};
  const FStrafeCase StrafeCases[]={{0,500,0,1.248f},{150,500,0,2.71885714f},
   {593,500,0,3.f},{593,500,1.5f,2.5312131f},{593,0,0,1.248f}};
  uint16 MotionStamp=0;
  for(int32 FPS:{30,90,144})for(bool Run:{false,true})for(int Direction:{-1,1})
  for(uint32 Style:{ACEMotion::StanceNonCombat,ACEMotion::StanceMagic,0x8000003eu,0x8000003fu})
  for(const auto& C:StrafeCases)
  {
   PC->PlayerInput->FlushPressedKeys();PC->PlayerInput->ProcessInputStack({},1.f/FPS,false);
   PC->bJumpAirborne=PC->bJumpCharging=PC->bStandingJumpLocked=false;
   PC->LandingWorldAceVelocity=FVector::ZeroVector;PC->LandingRootTrack.Reset();PC->StepHoldSeconds=0;
   PC->bServerMoveToActive=false;PC->ForwardSent=PC->RightSent=0;PC->bWasMoving=false;
   Session->bHasPlayerEncumbrance=false;Client->SetBurden(C.Burden);Client->SetRunSkill(C.Skill);
   Session->PlayerVitals.Stamina=C.Stamina;
   FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
   Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
   PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
   Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
   for(auto* Appearance:{App,RemoteApp})
   {
    Appearance->ClearActionMotion();Appearance->ResetWalkingMotion();Appearance->PreferredStyle=Style;
    Appearance->AnimTime=Appearance->DeferredPoseDeltaTime=0;Appearance->LocomotionForward=Appearance->LocomotionStrafe=0;
   }
   const float ExpectedRate=Run?C.RunSideRate:1.248f;
   // Start from an arbitrary idle phase. Retail starts the new sidestep
   // immediately instead of easing from this pose for a quarter second.
   App->AnimTime=.37f;App->bWasLocomotionMoving=false;
   App->StanceBlendAlpha=1.f;App->StanceBlendFrom.Reset();
   FACEBinaryWriter W;W.WriteUInt32(Other.Guid);W.WriteUInt16(0);W.WriteUInt16(++MotionStamp);W.WriteUInt16(0);W.WriteUInt8(0);W.Align();
   W.WriteUInt8(0);W.WriteUInt8(0);W.WriteUInt16(uint16(Style));
   W.WriteUInt32(0x18);W.WriteUInt16(0x000f);W.WriteFloat(Direction*ExpectedRate);W.Align();
   FACEBinaryReader Reader(W.GetData());Session->HandleUpdateMotion(Reader);
   TestTrue(TEXT("Network strafe preserves the already interpreted animation multiplier"),FMath::IsNearlyEqual(Received.AnimPlayRate,ExpectedRate,.00001f));
   Key(Direction>0?EKeys::C:EKeys::Z,true);if(!Run)Key(EKeys::LeftShift,true);
   const int32 Frames=FMath::RoundToInt(FPS*.7f);
   for(int32 Frame=0;Frame<Frames;++Frame)
   {
    PC->PlayerTick(1.f/FPS);App->TickComponent(1.f/FPS,LEVELTICK_All,nullptr);RemoteApp->TickComponent(1.f/FPS,LEVELTICK_All,nullptr);
    if(Frame==0)
    {
     TArray<FTransform> FirstStep;int32 FirstCount=0;
     if(!Dat->EvaluateMotionCommand(Self.MotionTableId,ACEMotion::SideStepRight,Direction*ExpectedRate/FPS,
      App->GetPartCount(),FirstStep,100,FirstCount,nullptr,nullptr,Style))return;
     for(int32 Part=0;Part<FirstCount;++Part)
      TestTrue(TEXT("First strafe frame immediately uses the authored step pose"),App->GetPartMesh(Part)->GetRelativeTransform().Equals(FirstStep[Part],.04));
    }
   }
   TestTrue(TEXT("Local strafe cadence matches retail's walk multiplier and run cap"),FMath::IsNearlyEqual(App->LocomotionPlayRate,ExpectedRate,.00001f));
   const double Time=double(Frames)/FPS,Travel=FVector::Dist2D(StartFeet,Pawn->GetActorLocation())/100.;
   TestTrue(TEXT("Cadence correction preserves retail lateral travel"),FMath::Abs(Travel-Time*ExpectedRate*1.25)<.005);
   TArray<FTransform> Expected;int32 Count=0;
   if(!TestTrue(TEXT("Retail strafe cycle evaluates"),Dat->EvaluateMotionCommand(Self.MotionTableId,ACEMotion::SideStepRight,
    Direction*Time*ExpectedRate,App->GetPartCount(),Expected,100,Count,nullptr,nullptr,Style)))return;
   for(int32 Part=0;Part<Count;++Part)
   {
    TestTrue(TEXT("Local feet and body follow the retail DAT strafe phase"),App->GetPartMesh(Part)->GetRelativeTransform().Equals(Expected[Part],.04));
    TestTrue(TEXT("Remote strafe phase matches local without a second speed multiplier"),RemoteApp->GetPartMesh(Part)->GetRelativeTransform().Equals(Expected[Part],.04));
   }
  }
 };
 CheckStrafe();
 // Navigation can reduce analog input to stop precisely at a close waypoint.
 // Its DAT gait phase must cover the same distance as physical prediction.
 TFunction<void()> CheckRoutePose = [&]
 {
  auto* Plugins=GI->GetSubsystem<UACEPluginSubsystem>();
  PC->PlayerInput->FlushPressedKeys();PC->PlayerInput->ProcessInputStack({},1.f/90,false);
  VR->bActive=false;Session->bHasPlayerEncumbrance=false;Client->SetBurden(0);Client->SetRunSkill(593);
  Session->PlayerVitals.Stamina=500;Session->bUseBusy=false;
  auto Reset=[&](uint32 Style,float Distance)
  {
   PC->bJumpAirborne=PC->bJumpCharging=PC->bStandingJumpLocked=false;
   PC->LandingWorldAceVelocity=FVector::ZeroVector;PC->LandingRootTrack.Reset();PC->StepHoldSeconds=0;
   PC->bServerMoveToActive=false;PC->ForwardSent=PC->RightSent=0;PC->bWasMoving=false;
   FACEPosition Pose;Pose.CellId=0x01010001;Pose.SetLocationFromUnreal(StartFeet,100);
   Pose.SetAceFacingFromUnrealDir2D(FVector::XAxisVector);Session->SetLocalPosition(Pose);
   PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
   Pawn->SetActorLocationAndRotation(StartFeet+FVector(0,0,88),Pose.ToUnrealQuat());
   App->ResetMotionForTeleport();App->PreferredStyle=Style;App->SetPreviewCapture(true);
   Plugins->MoveTarget=Pose;Plugins->MoveTarget.SetLocationFromUnreal(StartFeet+FVector(Distance,0,0),100);
   Plugins->MovementOwner=TEXT("ucm");Plugins->MoveExpires=FPlatformTime::Seconds()+100;
   Plugins->LastProgress=FPlatformTime::Seconds();Plugins->LastMovePosition=StartFeet;Plugins->MoveArrivalRadius=1;
  };
  for(int FPS:{15,90,144})for(uint32 Style:{ACEMotion::StanceNonCombat,ACEMotion::StanceMagic,0x8000003eu,0x8000003fu})
  {
   Reset(Style,5);
   App->AnimTime=.37f; // Starting from an arbitrary idle frame must not skip the first step.
   PC->PlayerTick(1.f/FPS);App->TickComponent(1.f/FPS,LEVELTICK_All,nullptr);
   const double Travel=FVector::Dist2D(StartFeet,Pawn->GetActorLocation())/100.;
   TestTrue(TEXT("Close route waypoint uses fractional movement without overshoot"),PC->ForwardAxis>0&&PC->ForwardAxis<1&&FMath::Abs(Travel-.05)<.002);
   TestTrue(TEXT("Navigation gait phase matches distance when arrival limits the axis"),FMath::Abs(App->AnimTime-Travel/DatRunSpeed)<.0001);
   TArray<FTransform> Expected;int32 Count=0;
   if(!TestTrue(TEXT("Route gait resolves the retail run cycle"),Dat->EvaluateMotionCommand(Self.MotionTableId,ACEMotion::RunForward,
    Travel/DatRunSpeed,App->GetPartCount(),Expected,100,Count,nullptr,nullptr,Style)))return;
   for(int Part=0;Part<Count;++Part)
    TestTrue(TEXT("First navigation step uses the DAT pose without an idle glide"),App->GetPartMesh(Part)->GetRelativeTransform().Equals(Expected[Part],.04));
  }
  for(uint32 Action:{0u,0x40000018u,0x40000016u,0x40000034u})
  {
   Reset(Action==0x40000016u?0x8000003eu:ACEMotion::StanceMagic,5000);
   if(Action)App->PlayActionMotion(Action,1.f,App->PreferredStyle);
   for(int Frame=0;Frame<180;++Frame)
   {PC->PlayerTick(1.f/90);App->TickComponent(1.f/90,LEVELTICK_All,nullptr);}
   TestTrue(TEXT("Route travel resumes locomotion after a previous activity"),App->ActionCommand==0&&App->LocomotionForward>0);
  }
  Plugins->MovementOwner.Empty();Plugins->MoveExpires=0;
 };
 CheckRoutePose();
 GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);
 return !HasAnyErrors();
}
#endif
