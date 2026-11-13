#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACETestDatDirectory.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEPlayerController.h"
#include "ACEDatSubsystem.h"
#include "ACEEnvCellActor.h"
#include "ACEHoverTooltipWidget.h"
#include "Dat/ACEEnvCellMeshBuilder.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginRouteGround.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerInput.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginRampRouteTest,"ACE.Plugins.RampRoute",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginRampRouteTest::RunTest(const FString&)
{
 const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
  .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
 auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
 auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
 auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);
 Context.OwningGameInstance=GI;Context.SetCurrentWorld(World);GI->OnWorldChanged(nullptr,World);GI->Init();
 ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);};
 auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();auto* C=GI->GetSubsystem<UACEClientSubsystem>();
 auto* H=GI->GetSubsystem<UACEPluginSubsystem>();auto Session=C->GetSession();
 if(!Dat->LoadDatDirectory(ACETestDatDirectory()))return false;
 const uint32 Block=0x61460000;
 const FVector Origin=FACEPosition::AceVectorToUnreal(FVector(0x61*192,0x46*192,0),100);
 // Olthoi120 points 3-5: the reported fourth point lies part way down this ramp.
 TArray<FACEPosition> Route;
 for(const auto& V:TArray<TPair<uint32,FVector>>{{0x61460369,FVector(143.8828125,-170.611328125,0)},
  {0x61460370,FVector(152.173828125,-169.5478515625,-4.196249961853027)},
  {0x6146031D,FVector(168.595703125,-170.0341796875,-6)}})
 {FACEPosition P;P.CellId=V.Key;P.Location=V.Value;Route.Add(P);}
 FACEDatLandblockInfo Info;TestTrue(TEXT("Olthoi dungeon DAT exists"),Dat->LoadLandblockInfo(Block,Info));
 TArray<AACEEnvCellActor*> Rooms;
 for(uint32 Index=0;Index<Info.NumCells;++Index)
 {
  const uint32 Id=Block|(0x100+Index);const auto* Built=Dat->GetOrBuildEnvCellMesh(Id,100);
  if(!Built||Built->Origin.X<130||Built->Origin.X>180||Built->Origin.Y< -185||Built->Origin.Y> -155)continue;
  auto* Room=World->SpawnActor<AACEEnvCellActor>();Room->LoadEnvCell(Id,Origin,100);
  Rooms.Add(Room);
 }
 TestTrue(TEXT("Actual ramp and connected rooms loaded"),Rooms.Num()>=3);
 for(bool Hidden:{true,false})
 {
  for(auto* Room:Rooms){Room->SetActorHiddenInGame(Hidden);Room->SetEnvCellCollisionActive(!Hidden);}
  FACEPluginRouteGround Ground(*World);FACEPluginSightQuery Sight(*World,nullptr);
  for(bool Reverse:{false,true})
  {
   const FVector From=Route[Reverse?2:0].ToUnrealLocation(),To=Route[Reverse?0:2].ToUnrealLocation();
   TArray<FVector> Path;
   TestTrue(TEXT("Continuous floor sampling crosses the full ramp in both directions, even with collision culled"),Ground.Path(From,To,Path));
   TestTrue(TEXT("Ramp line has close floor samples"),Path.Num()>40);
   if(Path.Num()>2)
   {
    double MaxChordError=0;
    for(int32 I=1;I<Path.Num();++I)
    {
     TestTrue(TEXT("No ground line sample skips more than 50cm horizontally"),FVector::Dist2D(Path[I-1],Path[I])<=50.1);
     const double T=FVector::Dist2D(From,Path[I])/FVector::Dist2D(From,To);
     MaxChordError=FMath::Max(MaxChordError,FMath::Abs(Path[I].Z-FMath::Lerp(From.Z,To.Z,T)));
    }
    TestTrue(TEXT("Regression uses a ramp where the direct chord is over a metre from the floor"),MaxChordError>100);
   }
   TestTrue(TEXT("Ramp remains a reachable route entry despite the ground obstructing a straight sight chord"),Ground.Reachable(From,To,Sight));
   TestFalse(TEXT("XY coincidence cannot join another floor"),Ground.Reachable(From,From-FVector(0,0,600),Sight));
  }
 }
 for(auto* Room:Rooms){Room->SetActorHiddenInGame(false);Room->SetEnvCellCollisionActive(true);}
 auto* PC=World->SpawnActor<AACEPlayerController>();PC->Client=C;
 auto* Local=NewObject<ULocalPlayer>(GEngine);GI->AddLocalPlayer(Local,FPlatformUserId::CreateFromInternalId(0));
 PC->SetPlayer(Local);World->AddController(PC);
 auto* Pawn=World->SpawnActor<APawn>();auto* Capsule=NewObject<UCapsuleComponent>(Pawn);
 Capsule->InitCapsuleSize(48,90.75);Pawn->SetRootComponent(Capsule);Pawn->AddInstanceComponent(Capsule);Capsule->RegisterComponent();
 Pawn->SetActorEnableCollision(false);PC->Possess(Pawn);
 PC->PlayerInput=NewObject<UPlayerInput>(PC);PC->SetupInputComponent();
 PC->bRetailCursorInstalled=true;PC->HoverTooltipWidget=CreateWidget<UACEHoverTooltipWidget>(GI,UACEHoverTooltipWidget::StaticClass());
 PC->bEnterWorldLoading=false;PC->bWorldRevealActive=false;
 Session->PlayerGuid=12345;Session->State=EACESessionState::InWorld;
 FACEWorldObject Self;Self.Guid=12345;Self.SetupId=0x02000001;Self.bIsPlayer=true;Self.bIsSelf=true;
 Session->WorldObjects.Add(Self.Guid,Self);C->SetRunSkill(300);
 auto Plugin=H->Find(TEXT("ucm"));Plugin->Enabled=true;Plugin->Running=true;
 FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
 TMap<FString,FVector> NativeResults;
 for(bool Legacy:{false,true})for(bool Reverse:{false,true})for(float Rate:{30.f,60.f,144.f})
 {
  TArray<TSharedPtr<FJsonValue>> Points;
  for(int32 I=0;I<Route.Num();++I)
  {
   const auto& Pos=Route[Reverse?Route.Num()-1-I:I];auto P=MakeShared<FJsonObject>();
   if(Legacy)
   {
    P->SetNumberField(TEXT("type"),0);P->SetNumberField(TEXT("ew"),(0x61*192+Pos.Location.X-24468)/240.);
    P->SetNumberField(TEXT("ns"),(0x46*192+Pos.Location.Y-24468)/240.);P->SetNumberField(TEXT("z"),Pos.Location.Z/240.);
    P->SetArrayField(TEXT("args"),{});
   }
   else {P->SetNumberField(TEXT("cell"),uint32(Pos.CellId));P->SetNumberField(TEXT("x"),Pos.Location.X);P->SetNumberField(TEXT("y"),Pos.Location.Y);P->SetNumberField(TEXT("z"),Pos.Location.Z);P->SetStringField(TEXT("kind"),TEXT("walk"));}
   Points.Add(MakeShared<FJsonValueObject>(P));
  }
  TSharedPtr<FJsonObject> Profile=MakeShared<FJsonObject>();
  if(Legacy){auto Nav=MakeShared<FJsonObject>();Nav->SetStringField(TEXT("format"),TEXT("nav"));Nav->SetNumberField(TEXT("mode"),4);Nav->SetArrayField(TEXT("points"),Points);TArray<FString> Issues;Profile=ACEVTProfile::Convert(Nav,Issues);TestEqual(TEXT("Equivalent VT coordinates import without warnings"),Issues.Num(),0);}
  else Profile->SetArrayField(TEXT("route"),Points);
  Profile->SetBoolField(TEXT("navigation"),true);Profile->SetBoolField(TEXT("buffing"),false);Profile->SetBoolField(TEXT("recovery"),false);Profile->SetStringField(TEXT("combat"),TEXT("off"));
  Plugin->Profile=Profile;FACEPluginVM VM;TestTrue(TEXT("UCM route policy loads"),VM.Load(Source,Error));
  FACEPosition Pose=Route[Reverse?2:0];Pose.SetAceFacingFromUnrealDir2D(Route[1].ToUnrealLocation()-Pose.ToUnrealLocation());
  Session->SetLocalPosition(Pose);PC->PredictedPose=Pose;PC->bHavePredictedPose=true;PC->bHaveLastServerPose=false;
  PC->bJumpAirborne=false;PC->bStandingJumpLocked=false;PC->StepHoldSeconds=0;PC->bLocalPredicting=false;
  Pawn->SetActorLocationAndRotation(Pose.ToUnrealLocation()+FVector(0,0,90.75),Pose.ToUnrealQuat());
  H->MovementOwner.Empty();H->bRouteJoinRequested=false;
  Plugin->NextAction=0;
  auto S=MakeShared<FJsonObject>();S->SetBoolField(TEXT("ready"),true);S->SetNumberField(TEXT("player"),12345);
  for(const TCHAR* K:{TEXT("health"),TEXT("max_health"),TEXT("stamina"),TEXT("max_stamina"),TEXT("mana"),TEXT("max_mana")})S->SetNumberField(K,100);
  bool Completed=false;
  for(int32 Frame=0;Frame<int32(Rate*30);++Frame)
  {
   const auto Current=PC->PredictedPose;auto Position=MakeShared<FJsonObject>();Position->SetNumberField(TEXT("cell"),uint32(Current.CellId));
   Position->SetNumberField(TEXT("x"),Current.Location.X);Position->SetNumberField(TEXT("y"),Current.Location.Y);Position->SetNumberField(TEXT("z"),Current.Location.Z);
   S->SetObjectField(TEXT("position"),Position);S->SetNumberField(TEXT("time"),100+Frame/Rate);
   TSharedPtr<FJsonObject> Intent;
   if(!VM.Step(S,Profile,Intent,Error)){AddError(Error);break;}
   if(Intent)
   {
    FString Status;Intent->TryGetStringField(TEXT("status"),Status);
    if(Status==TEXT("Route complete")){Completed=true;break;}
    FString Action;Intent->TryGetStringField(TEXT("action"),Action);
    if(Action==TEXT("move")){Plugin->NextAction=0;H->Execute(*Plugin,Intent);}
   }
   ++GFrameCounter;PC->PlayerTick(1.f/Rate);
  }
  AddInfo(FString::Printf(TEXT("Olthoi ramp legacy=%d reverse=%d fps=%.0f completed=%d final=%s"),Legacy,Reverse,Rate,Completed,*PC->PredictedPose.Location.ToString()));
  TestTrue(TEXT("Native and imported routes traverse step 4 under real player collision in both directions"),Completed);
  const FString Key=FString::Printf(TEXT("%d %.0f"),Reverse,Rate);
  if(!Legacy)NativeResults.Add(Key,PC->PredictedPose.Location);
  else TestTrue(TEXT("Imported VT and native UCM arrive at the same position"),PC->PredictedPose.Location.Equals(NativeResults[Key],.001));
 }
 Session->State=EACESessionState::Disconnected;
 return !HasAnyErrors();
}
#endif
