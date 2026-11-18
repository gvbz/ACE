#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEWorldEntityActor.h"
#include "ACEScriptComponent.h"
#include "ACESession.h"
#include "ACEOpcodes.h"
#include "VR/ACEVRRemoteAvatarComponent.h"
#include "VR/ACEVRSettings.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/CapsuleComponent.h"
#include "ACEParticleBatchComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "HAL/IConsoleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVRRenderReplicationTest, "ACE.VR.RenderingAndReplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEVRRenderReplicationTest::RunTest(const FString& Parameters)
{
	const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
		.CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	auto* GI = NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); GI->Init();
	ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
	if (!TestTrue(TEXT("Retail DAT opens"), Dat->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call")))) return false;
	TestTrue(TEXT("Default calibration uses character eye height"), GetDefault<UACEVRSettings>()->bMatchCharacterHeight);
	// Actual WCID 8904 ObjDesc, calculated by the server from ClothingTable.
	FACEObjDesc Stone; Stone.PaletteBaseId=0x04000BF8;
	FACEObjDescSubPalette Palette; Palette.SubPaletteId=0x04000BF1; Palette.Offset=0; Palette.NumColors=2048; Stone.SubPalettes.Add(Palette);
	FACEObjDescTextureChange Texture; Texture.PartIndex=0; Texture.OldTexture=0x05000ADD; Texture.NewTexture=0x050010D7; Stone.TextureChanges.Add(Texture);
	FACEDatDecodedSurface Yellow;
	TestTrue(TEXT("Focusing Stone texture replacement decodes"),Dat->GetTextureResolver()->ResolveSurfaceWithAppearance(0x0800021A,0,&Stone,Yellow));
	double Red=0,Green=0,Blue=0;
	for (auto C:Yellow.Pixels) { Red+=C.R; Green+=C.G; Blue+=C.B; }
	AddInfo(FString::Printf(TEXT("Focusing Stone mean RGB %.1f %.1f %.1f"),Red/Yellow.Pixels.Num(),Green/Yellow.Pixels.Num(),Blue/Yellow.Pixels.Num()));
	TestTrue(TEXT("Stone uses its yellow palette after texture replacement"),Red>Blue*1.5 && Green>Blue*1.3);

	// Render the same real DAT geometry with both paths and two distinct fades.
	auto* Actor=World->SpawnActor<AActor>();
	auto* Root=NewObject<USceneComponent>(Actor); Actor->SetRootComponent(Root); Root->RegisterComponent();
	Actor->SetActorLocation(FVector(-2800000,2400000,10000));
	auto* Scripts=NewObject<UACEScriptComponent>(Actor); Actor->AddInstanceComponent(Scripts); Scripts->RegisterComponent();
	UACEScriptComponent::FActiveEmitter Emitter;
	Emitter.Info.GfxObjId=0x010016C8; Emitter.Info.MaxParticles=128; Emitter.Info.ParticleType=1; Emitter.Info.Lifespan=10;
	Emitter.Info.StartScale=Emitter.Info.FinalScale=1; Emitter.Parent=Root;
	for(int32 I=0;I<128;++I) Scripts->SpawnParticle(Emitter);
	if (!TestNotNull(TEXT("128 particles share an emitter batch"),Emitter.Batch.Get())) return false;
	TestEqual(TEXT("All particles retained"),Emitter.Batch->GetParticleCount(),128);
	TestTrue(TEXT("Batch does not allocate one mesh component per particle"),Emitter.Particles.ContainsByPredicate([](const auto& P){return !P.Mesh.IsValid();}));
	// Removing a middle instance must keep every remaining particle's fade/transform mapping.
	Scripts->ReleaseParticleVisual(Emitter,Emitter.Particles[3]); Emitter.Particles.RemoveAtSwap(3);
	TSet<int32> Indices; for(const auto& P:Emitter.Particles) Indices.Add(P.InstanceIndex);
	TestEqual(TEXT("Removal preserves unique live instance indices"),Indices.Num(),127);
	Emitter.Batch->ClearParticles(); Emitter.Particles.Reset(); Scripts->SpawnParticle(Emitter);
	auto* Reference=NewObject<UProceduralMeshComponent>(Actor); Reference->RegisterComponent();
	TestTrue(TEXT("Reference DAT geometry builds"),Dat->ApplyParticleGfxToProceduralMesh(Reference,0x010016C8,100,false));
	Reference->SetWorldLocation(Actor->GetActorLocation());
	const auto Location=Actor->GetActorLocation();
	Emitter.Particles[0].Position=Location;
	auto* Target=NewObject<UTextureRenderTarget2D>(Actor); Target->InitCustomFormat(256,256,PF_FloatRGBA,false); Target->UpdateResourceImmediate();
	auto* Capture=NewObject<USceneCaptureComponent2D>(Actor); Capture->TextureTarget=Target; Capture->bCaptureEveryFrame=false;
	Capture->bCaptureOnMovement=false; Capture->CaptureSource=SCS_SceneColorHDRNoAlpha;
	Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetBloom(false); Capture->ShowFlags.SetAtmosphere(false);
	Capture->ShowFlags.SetAntiAliasing(false); Capture->ShowFlags.SetLighting(false);
	Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->SetWorldLocation(Location+FVector(0,-300,0)); Capture->SetWorldRotation(FRotator(0,90,0)); Capture->RegisterComponent();
	auto Read=[&](bool Batch,float Fade)
	{
		Scripts->ApplyParticleVisual(Emitter,Emitter.Particles[0],FQuat::Identity,FVector(1),Fade);
		Emitter.Batch->FlushParticles();
		for(int32 I=0;I<Reference->GetNumMaterials();++I)
		{
			auto* Mid=Scripts->EnsurePrivateMaterialInstance(Reference,I); if(Mid) Mid->SetScalarParameterValue(TEXT("OpacityMul"),Fade);
		}
		Capture->ShowOnlyComponents.Reset(); Capture->ShowOnlyComponents.Add(Batch?static_cast<UPrimitiveComponent*>(Emitter.Batch.Get()):Reference);
		if(GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
		World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
		TArray<FLinearColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels); return Pixels;
	};
	for (int32 Blend=0; Blend<2; ++Blend)
	{
		if (Blend==1)
		{
			for (UProceduralMeshComponent* Mesh : {static_cast<UProceduralMeshComponent*>(Emitter.Batch.Get()),Reference})
			{
				auto* Parent=Mesh==Reference ? Dat->EnsureAceParticleTranslucentMaterialBase() : Dat->EnsureAceBatchedParticleMaterialBase(false);
				auto* Mid=UMaterialInstanceDynamic::Create(Parent,Mesh); Mid->CopyMaterialUniformParameters(Mesh->GetMaterial(0));
				Mid->SetScalarParameterValue(TEXT("OpacityMul"),1.f); Mesh->SetMaterial(0,Mid);
			}
		}
	double Bright[2]={};
	for(int32 Step=0;Step<2;++Step)
	{
		const float Fade=Step==0?.25f:.8f; const auto A=Read(false,Fade); const auto B=Read(true,Fade);
		double Error=0,Energy=0;
		for(int32 I=0;I<A.Num();++I) { Error+=FMath::Abs(A[I].R-B[I].R)+FMath::Abs(A[I].G-B[I].G)+FMath::Abs(A[I].B-B[I].B); Energy+=A[I].R+A[I].G+A[I].B; Bright[Step]+=B[I].R+B[I].G+B[I].B; }
		AddInfo(FString::Printf(TEXT("Particle fade %.2f reference=%.2f batch=%.2f error=%.5f"),Fade,Energy,Bright[Step],Error/FMath::Max(1.,Energy)));
		TestTrue(TEXT("Actual particle renders visible pixels"),Energy>1.);
		TestTrue(TEXT("Batched particle matches reference transparency"),Error/FMath::Max(1.,Energy)<.03);
	}
	TestTrue(TEXT("Per-instance fade changes rendered brightness"),Bright[1]>Bright[0]*1.5);
	}
	// Retail's keyed celestial cards reject low-alpha edge pixels; clouds keep
	// their continuous blend. Exercise the actual sky shader at clear depth.
	auto* EdgeTexture=UTexture2D::CreateTransient(1,1,PF_B8G8R8A8);
	const FColor EdgePixel(255,255,255,80);
	void* TextureData=EdgeTexture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData,&EdgePixel,sizeof(EdgePixel)); EdgeTexture->GetPlatformData()->Mips[0].BulkData.Unlock(); EdgeTexture->UpdateResource();
	auto* SkyMid=UMaterialInstanceDynamic::Create(Dat->EnsureAceSkyTranslucentMaterialBase(),Reference);
	SkyMid->SetTextureParameterValue(TEXT("ACETexture"),EdgeTexture);
	SkyMid->SetVectorParameterValue(TEXT("SkyAmbient"),FLinearColor::White);
	Reference->SetMaterial(0,SkyMid);
	auto Energy=[](const TArray<FLinearColor>& Pixels) { double Sum=0; for(auto C:Pixels) Sum+=C.R+C.G+C.B; return Sum; };
	const double Soft=Energy(Read(false,1.f));
	SkyMid->SetScalarParameterValue(TEXT("SkyAlphaTestReference"),100.f/255.f);
	const double Keyed=Energy(Read(false,1.f));
	TestTrue(TEXT("Soft sky alpha remains visible"),Soft>1.);
	TestTrue(TEXT("Retail moon cutoff removes low-alpha card borders"),Keyed<Soft*.01);
	Emitter.Batch->DestroyComponent(); Reference->DestroyComponent(); Capture->DestroyComponent();

	// Receive a real wire payload and exercise the remote avatar component.
	auto* Client=GI->GetSubsystem<UACEClientSubsystem>(); Client->Session=MakeShared<FACESession>();
	auto& Session=*Client->Session; Session.State=EACESessionState::InWorld;
	// No local VR rig or outgoing pose: a desktop observer negotiates playback.
	FACEBinaryWriter Capabilities; for(uint32 V:{0u,1u,0xF7D0u,1u,16u|32768u|131072u}) Capabilities.WriteUInt32(V);
	FACEBinaryReader CapabilitiesReader(Capabilities.GetData()); Session.HandleGameEvent(CapabilitiesReader);
	TestEqual(TEXT("Desktop playback does not transmit tracking"),Session.VRPoseSequence,0u);
	FACEWorldObject RemoteObject; RemoteObject.Guid=123; RemoteObject.Name=TEXT("VR pose fixture"); RemoteObject.bIsPlayer=true;
	RemoteObject.SetupId=0x02000001; RemoteObject.MotionTableId=0x09000001; RemoteObject.bHasPosition=true; RemoteObject.Position.CellId=0x7D640019;
	Session.WorldObjects.Add(123,RemoteObject);
	auto* Entity=World->SpawnActor<AACEWorldEntityActor>(); Entity->InitializeFromObject(RemoteObject,100,true);
	auto* Remote=Entity->FindComponentByClass<UACEVRRemoteAvatarComponent>();
	if (!TestNotNull(TEXT("Network player has pose playback"),Remote)) return false;
	FACEBinaryWriter W; W.WriteUInt32(123); W.WriteUInt32(1); W.WriteUInt32(1); W.WriteUInt32(RemoteObject.Position.CellId);
	W.WriteUInt32(0); W.WriteUInt32(7); W.WriteFloat(1.7575f); W.WriteFloat(0);
	const FVector Positions[]={FVector(0,0,1.7575),FVector(.35,-.4,2.1),FVector(.4,.35,1.2)};
	const FQuat Rotations[]={FRotator(20,30,10).Quaternion(),FRotator(0,0,20).Quaternion(),FQuat::Identity};
	for(int32 I=0;I<3;++I) for(double V:{Positions[I].X,Positions[I].Y,Positions[I].Z,Rotations[I].X,Rotations[I].Y,Rotations[I].Z,Rotations[I].W}) W.WriteFloat(V);
	auto ReceivePose=[&](const TArray<uint8>& Payload)
	{
		FACEBinaryWriter Event;
		for(uint32 V:{ACEOpcode::GameEvent,100u,1u,0xF7D1u}) Event.WriteUInt32(V);
		Event.WriteBytes(Payload); Session.HandleGameMessage(Event.GetData());
	};
	ReceivePose(W.GetData());
	FACEVRPose Pose; TestTrue(TEXT("Received pose is available to the renderer"),Session.GetVRPose(123,Pose));
	// Object creation now evaluates retail idle immediately. Compare to the
	// authored bind pose, not that (deliberately different) initial animation.
	FTransform LegBefore;
	TestTrue(TEXT("VR fixture has an authored leg bind pose"),Entity->Appearance->GetPartBindTransform(2,LegBefore));
	Remote->TickComponent(.05f,LEVELTICK_All,nullptr);
	TestTrue(TEXT("Remote upper body is controlled by tracking"),Entity->Appearance->bVRPoseControlled);
	TestTrue(TEXT("Stationary VR lower body uses the neutral bind pose"),Entity->Appearance->GetPartMesh(2)->GetRelativeTransform().Equals(LegBefore));
	TestTrue(TEXT("Remote raised hand is visibly above the head"),Entity->Appearance->GetPartMesh(12)->GetComponentLocation().Z > Entity->Appearance->GetPartMesh(16)->GetComponentLocation().Z);
	const FTransform StationaryLeg=Entity->Appearance->GetPartMesh(2)->GetRelativeTransform();
	Entity->Appearance->SetPreferredStyle(0x8000003Fu);
	Entity->Appearance->SetLocomotionInput(1,0,true,1);
	Entity->Appearance->TickComponent(.1f,LEVELTICK_All,nullptr);
	Remote->TickComponent(.05f,LEVELTICK_All,nullptr);
	TestTrue(TEXT("VR legs cannot run in place from a stale network motion or combat stance"),Entity->Appearance->GetPartMesh(2)->GetRelativeTransform().Equals(StationaryLeg));
	for(int I=0;I<8;++I){Entity->AddActorWorldOffset(FVector(8,0,0));Remote->TickComponent(.02f,LEVELTICK_All,nullptr);}
	TestFalse(TEXT("Actual body travel advances VR gait"),Entity->Appearance->GetPartMesh(2)->GetRelativeTransform().Equals(StationaryLeg,.01));
	// Held entities are children of tracked body parts. Revealing a remote
	// avatar must never reveal the weapon's collision capsule along with it.
	auto* Held=World->SpawnActor<AACEWorldEntityActor>();
	Held->AttachToComponent(Entity->Appearance->GetPartMesh(15),FAttachmentTransformRules::KeepRelativeTransform);
	auto* Proxy=Held->FindComponentByClass<UCapsuleComponent>();
	Entity->Appearance->SetAppearanceVisible(false); Entity->Appearance->SetAppearanceVisible(true);
	TestFalse(TEXT("Revealing a friend's avatar keeps the held collision proxy invisible"),Proxy->IsVisible());
	TestTrue(TEXT("Held collision proxy stays hidden in game"),Proxy->bHiddenInGame);
	Held->Destroy();
	// Equipment poses use the actual rendered root, including the authored
	// placement; a remote observer must not need a first shot to correct it.
	FACEWorldObject Crossbow;Crossbow.Guid=124;Crossbow.SetupId=0x0200134D;
	Crossbow.ItemType=ACEItemType::MissileWeapon;Crossbow.CurrentWieldedLocation=ACEEquipMask::MissileWeapon;
	Crossbow.DefaultCombatStyle=0x20;Crossbow.WielderId=Crossbow.ParentGuid=123;Crossbow.ParentLocation=2;
	Session.WorldObjects.Add(124,Crossbow);
	auto* CrossbowActor=World->SpawnActor<AACEWorldEntityActor>();CrossbowActor->InitializeFromObject(Crossbow,100,true);
	FACEWorldObject Bolt=Crossbow;Bolt.Guid=125;Bolt.SetupId=0x02001A87;Bolt.CurrentWieldedLocation=ACEEquipMask::MissileAmmo;Bolt.AmmoType=2;
	Session.WorldObjects.Add(125,Bolt);
	auto* BoltActor=World->SpawnActor<AACEWorldEntityActor>();BoltActor->InitializeFromObject(Bolt,100,true);
	FACEBinaryWriter V2;for(uint32 V:{123u,2u,2u,uint32(RemoteObject.Position.CellId),0u,7u})V2.WriteUInt32(V);
	V2.WriteFloat(1.7575f);V2.WriteFloat(0);
	const FTransform HeldPose(FRotator(13,45,70),FVector(.45,-.30,1.30)),AmmoPose(FRotator(0,90,0),FVector(.6,-.28,1.40));
	for(int32 I=0;I<5;++I)
	{
		if(I==3){V2.WriteUInt32(124);V2.WriteUInt32(125);}
		const FTransform T=I<3 ? FTransform(Rotations[I],Positions[I]) : I==3 ? HeldPose : AmmoPose;
		const auto P=T.GetLocation();const auto Q=T.GetRotation();
		for(double V:{P.X,P.Y,P.Z,Q.X,Q.Y,Q.Z,Q.W})V2.WriteFloat(V);
	}
	const FVector RootPosition=Entity->GetActorLocation()/100;
	for(double V:{RootPosition.X,RootPosition.Y,RootPosition.Z})V2.WriteFloat(V);
	TestEqual(TEXT("Equipment event wire size"),V2.GetData().Num(),192);
	ReceivePose(V2.GetData());Remote->TickComponent(.05f,LEVELTICK_All,nullptr);
	TestTrue(TEXT("Remote crossbow accepts equipment pose before firing"),Remote->UpdateMissileAttachment(CrossbowActor));
	TestTrue(TEXT("Remote crossbow keeps local render orientation"),CrossbowActor->GetActorQuat().Equals(HeldPose.GetRotation(),.001));
	TestTrue(TEXT("Remote crossbow keeps local grip offset"),CrossbowActor->GetActorLocation().Equals(Entity->GetActorLocation()+HeldPose.GetLocation()*100,.001));
	TestTrue(TEXT("Remote bolt is visible before the first shot"),Remote->UpdateMissileAttachment(BoltActor) && !BoltActor->IsHidden());
	TestTrue(TEXT("Remote bolt is centered at the transmitted rail location"),BoltActor->GetActorLocation().Equals(Entity->GetActorLocation()+AmmoPose.GetLocation()*100,.001));
	Session.RequestVRCapabilities(true);
	Remote->TickComponent(.05f,LEVELTICK_All,nullptr);
	TestTrue(TEXT("Enabling the viewer's headset keeps the same remote tracked hand"),
		Entity->Appearance->GetPartMesh(12)->GetComponentLocation().Z > Entity->Appearance->GetPartMesh(16)->GetComponentLocation().Z);
	// Standard movement packets must reach a desktop avatar even when its
	// observer has negotiated tracked hands. Exercise the actual opcode router.
	FACEWorldObject DesktopObject=RemoteObject; DesktopObject.Guid=126;
	DesktopObject.Position.Location=FVector(50,50,100);
	Session.WorldObjects.Add(126,DesktopObject);
	auto* Desktop=World->SpawnActor<AACEWorldEntityActor>();Desktop->InitializeFromObject(DesktopObject,100,true);
	Desktop->bClampToGround=false;Desktop->bRetryGroundClamp=false;
	auto* DesktopRemote=Desktop->FindComponentByClass<UACEVRRemoteAvatarComponent>();
	// Compare with the ordinary actor path. Authored DAT entry/exit motion is
	// not instantaneous constant-speed movement; RemoteMovementReplay checks its
	// displacement against the clips separately.
	auto* Ordinary=World->SpawnActor<AACEWorldEntityActor>();Ordinary->InitializeFromObject(DesktopObject,100,true);
	Ordinary->bClampToGround=false;Ordinary->bRetryGroundClamp=false;
	const auto MotionHandle=Session.OnMotionUpdate.AddLambda([&](int32 Guid,const FACEObjectMotionState& Motion)
		{if(Guid==126){Desktop->ApplyMotionState(Motion);Ordinary->ApplyMotionState(Motion);}});
	auto ReceiveMotion=[&](uint16 Sequence,uint16 Command)
	{
		FACEBinaryWriter Motion;Motion.WriteUInt32(ACEOpcode::UpdateMotion);Motion.WriteUInt32(126);
		Motion.WriteUInt16(0);Motion.WriteUInt16(Sequence);Motion.WriteUInt16(0);Motion.WriteUInt8(1);Motion.Align();
		Motion.WriteUInt8(0);Motion.WriteUInt8(0);Motion.WriteUInt16(0x3D);
		Motion.WriteUInt32(6);Motion.WriteUInt16(Command);Motion.WriteFloat(1);Motion.Align();
		Session.HandleGameMessage(Motion.GetData());
	};
	ReceiveMotion(1,7);
	const FVector DesktopStart=Desktop->GetActorLocation();
	const FTransform DesktopLeg=Desktop->Appearance->GetPartMesh(2)->GetRelativeTransform();
	for(int32 Frame=0;Frame<18;++Frame)
	{
		Desktop->Tick(1.f/90);Desktop->Appearance->TickComponent(1.f/90,LEVELTICK_All,nullptr);
		DesktopRemote->TickComponent(1.f/90,LEVELTICK_All,nullptr);
		Ordinary->Tick(1.f/90);Ordinary->Appearance->TickComponent(1.f/90,LEVELTICK_All,nullptr);
	}
	TestTrue(TEXT("VR viewer sees standard desktop movement with the same authored displacement"),
		FVector::Distance(DesktopStart,Desktop->GetActorLocation())>1 && Desktop->GetActorLocation().Equals(Ordinary->GetActorLocation(),.01));
	TestFalse(TEXT("VR viewer sees desktop running animation"),Desktop->Appearance->GetPartMesh(2)->GetRelativeTransform().Equals(DesktopLeg,.01));
	TestFalse(TEXT("Desktop avatar never inherits the observer's tracked animation mode"),Desktop->Appearance->bVRPoseControlled);
	ReceiveMotion(2,3);
	for(int32 Frame=0;Frame<90;++Frame)
	{
		Desktop->Tick(1.f/90);Desktop->Appearance->TickComponent(1.f/90,LEVELTICK_All,nullptr);
		DesktopRemote->TickComponent(1.f/90,LEVELTICK_All,nullptr);
		Ordinary->Tick(1.f/90);Ordinary->Appearance->TickComponent(1.f/90,LEVELTICK_All,nullptr);
	}
	TestTrue(TEXT("VR observer preserves the authored stopping transition"),Desktop->GetActorLocation().Equals(Ordinary->GetActorLocation(),.01));
	const FVector DesktopStopped=Desktop->GetActorLocation();
	Desktop->Tick(.1f);
	TestTrue(TEXT("Desktop stop packet stops movement for a VR viewer"),Desktop->GetActorLocation().Equals(DesktopStopped,.1));
	Session.OnMotionUpdate.Remove(MotionHandle);Session.WorldObjects.Remove(126);Desktop->Destroy();Ordinary->Destroy();
	const double Received=FPlatformTime::Seconds();
	auto First=Session.VRPoses[123].Current;First.ReceivedAt=Received-.1;
	Session.VRPoses[123]=FACERemoteVRPose();Session.VRPoses[123].Add(First);
	auto Next=First;++Next.Sequence;Next.ReceivedAt=Received-.05;Next.Root+=FVector(1,0,-.5);
	Session.VRPoses[123].Add(Next);
	Session.GetVRPose(123,Pose);
	TestTrue(TEXT("Body movement interpolates along slopes with tracked equipment"),Pose.Root.X>RootPosition.X+.3 && Pose.Root.X<RootPosition.X+.7 && Pose.Root.Z<RootPosition.Z);
	Entity->SetActorTickEnabled(false);
	const FVector BeforeTrackedMove=Entity->GetActorLocation();
	Remote->TickComponent(1.f/90,LEVELTICK_All,nullptr);
	const FVector AfterTrackedMove=Entity->GetActorLocation();
	TestTrue(TEXT("Tracked root advances smoothly even while ordinary actor movement sleeps"),
		AfterTrackedMove.X>BeforeTrackedMove.X && AfterTrackedMove.X<=Next.Root.X*100
		&& AfterTrackedMove.Z<BeforeTrackedMove.Z && AfterTrackedMove.Z>=Next.Root.Z*100);
	// A standard position can arrive while the final tracked pose is still
	// buffered. Rendering that old pose must not erase this newer correction.
	FACEPosition Resume=RemoteObject.Position;Resume.bIsGrounded=true;
	Resume.Location+=FVector(-2,0,0);
	Entity->bClampToGround=false;Entity->bRetryGroundClamp=false;
	const auto PositionHandle=Session.OnPositionUpdate.AddLambda([&](int32 Guid,const FACEPosition& Position)
		{if(Guid==123)Entity->ApplyACEPosition(Position);});
	FACEBinaryWriter PositionEvent;
	for(uint32 V:{ACEOpcode::UpdatePosition,123u,0x74u,uint32(Resume.CellId)})PositionEvent.WriteUInt32(V);
	for(double V:{Resume.Location.X,Resume.Location.Y,Resume.Location.Z,double(Resume.RotationW)})PositionEvent.WriteFloat(V);
	for(uint16 V:{uint16(0),uint16(1),uint16(0),uint16(0)})PositionEvent.WriteUInt16(V);
	Session.HandleGameMessage(PositionEvent.GetData());
	Session.OnPositionUpdate.Remove(PositionHandle);
	Remote->TickComponent(.01f,LEVELTICK_All,nullptr);
	Entity->SetActorTickEnabled(false);
	Session.VRPoses[123].Current.ReceivedAt-=1;
	Remote->TickComponent(.05f,LEVELTICK_All,nullptr);
	TestTrue(TEXT("Tracking expiry wakes the ordinary movement/correction tick"),Entity->IsActorTickEnabled());
	TestFalse(TEXT("Stale tracking restores retail animation"),Entity->Appearance->bVRPoseControlled);
	TestFalse(TEXT("Stale tracking restores retail held placement"),Remote->UpdateMissileAttachment(CrossbowActor));
	for(int32 Frame=0;Frame<180;++Frame)if(Entity->IsActorTickEnabled())Entity->Tick(1.f/90);
	TestTrue(TEXT("Leaving tracking preserves the newest ordinary position update"),Entity->GetActorLocation().Equals(Resume.ToUnrealLocation(100),.2));
	CrossbowActor->Destroy();BoltActor->Destroy();
	// Compare the exact first-contact result, not just an approximate count.
	// Many arc segments miss all rigid parts in a dense candidate set.
	Entity->SetActorScale3D(FVector(1.2,.8,1.1));Entity->SetActorRotation(FRotator(0,37,0));
	struct FContactResult { bool Hit;float Along; };
	TArray<FContactResult> ReferenceContacts;
	double ContactMs[2]={};int32 Counts[2]={};
	for(int32 Mode=0;Mode<2;++Mode)
	{
		const double Start=FPlatformTime::Seconds();
		for(int32 Repeat=0;Repeat<40;++Repeat)
		{
			// Include the cost of preparing 32 bodies for 48 segments each.
			FBox Prepared[32];if(Mode)for(auto& B:Prepared) B=Entity->GetProjectileContactBounds(5);
			for(int32 I=0;I<1536;++I)
			{
			const FVector A=Entity->GetActorLocation()+FVector((I%48-24)*15,(I/48-16)*20,(I%7)*30);
			const FVector B=A+FVector(40,12,-8);
			float Along=0;const bool Hit=(!Mode || FBox(A.ComponentMin(B),A.ComponentMax(B)).Intersect(Prepared[I/48])) && Entity->FindProjectileContact(A,B,5,Along);Counts[Mode]+=Hit;
			if(!Repeat)
			{
				if(!Mode) ReferenceContacts.Add({Hit,Along});
				else {TestEqual(TEXT("Bounds rejection preserves contact"),Hit,ReferenceContacts[I].Hit);
					if(Hit) TestTrue(TEXT("Bounds rejection preserves earliest contact time"),FMath::IsNearlyEqual(Along,ReferenceContacts[I].Along,.0001f));}
			}
			}
		}
		ContactMs[Mode]=(FPlatformTime::Seconds()-Start)*1000/40;
	}
	TestTrue(TEXT("Contact benchmark includes actual hits"),Counts[0]>0);
	AddInfo(FString::Printf(TEXT("Projectile contact CPU benchmark, 1536 segments: reference %.3f ms, bounds rejection %.3f ms (not frame time)"),ContactMs[0],ContactMs[1]));
	return true;
}
#endif
