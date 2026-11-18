#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACEDatSubsystem.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEWorldEntityActor.h"
#include "ACESkyDomeActor.h"
#include "ACELandblockActor.h"
#include "ACETerrainChunkActor.h"
#include "ACEEnvCellActor.h"
#include "ProceduralMeshComponent.h"
#include "PrimitiveSceneProxy.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Scalability.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECachedWorldDrawTest, "ACE.Rendering.CachedWorldDraws",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECachedWorldDrawTest::RunTest(const FString&)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .RequiresHitProxies(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* GI = NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    auto* Owner = World->SpawnActor<AActor>();
    auto* Mesh = NewObject<UProceduralMeshComponent>(Owner); Owner->SetRootComponent(Mesh);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision); Mesh->SetCastShadow(false); Mesh->RegisterComponent();
    auto* Material = UMaterialInstanceDynamic::Create(Dat->EnsureAceUnlitTexturedMaterialBase(), Owner);
    Material->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.65f);
    Mesh->SetMaterial(0, Material);
    auto* Target = NewObject<UTextureRenderTarget2D>(Owner); Target->InitCustomFormat(160, 160, PF_B8G8R8A8, false);
    auto* Capture = NewObject<USceneCaptureComponent2D>(Owner); Capture->TextureTarget = Target;
    Capture->bCaptureEveryFrame = false; Capture->bCaptureOnMovement = false; Capture->CaptureSource = SCS_FinalColorLDR;
    Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetBloom(false); Capture->ShowFlags.SetAtmosphere(false);
    Capture->ShowFlags.SetFog(false); Capture->ShowFlags.SetAntiAliasing(false);
    Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyComponents.Add(Mesh); Capture->RegisterComponent();
    auto Render = [&]()
    {
        if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
        World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
        TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels); return Pixels;
    };
    auto* Cached = IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Render.CachedWorldDraws"));
    const int32 Previous = Cached->GetInt(); Cached->Set(1, ECVF_SetByCode);
    ON_SCOPE_EXIT { Cached->Set(Previous, ECVF_SetByCode); };
    TArray<TArray<FColor>> Reference;
    const TArray<FVector> Vertices = {{100,-32,-30},{100,32,-30},{100,32,30},{100,-32,30}};
    for (bool UseCache : {false, true})
    {
        Mesh->bPreferCachedDraws = UseCache; Mesh->MarkRenderStateDirty();
        Mesh->SetWorldTransform(FTransform::Identity);
        Material->SetScalarParameterValue(TEXT("EmissiveStrength"), .65f);
        Mesh->CreateMeshSection(0, Vertices, {0,2,1,0,3,2,0,1,2,0,2,3}, {}, {}, {}, {}, false);
        int32 Stage = 0;
        auto Compare = [&](const TCHAR* Name, bool Visible)
        {
            const auto Pixels = Render();
            TestEqual(TEXT("Capture has expected dimensions"), Pixels.Num(), 160*160);
            int32 Lit = 0; for (const auto& Pixel : Pixels) Lit += FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>20;
            if (Visible) TestTrue(FString::Printf(TEXT("%s draws geometry"), Name), Lit>100);
            else TestEqual(FString::Printf(TEXT("%s hides geometry"), Name), Lit, 0);
            if (!UseCache) Reference.Add(Pixels);
            else
            {
                int64 Error = 0;
                for (int32 I=0; I<Pixels.Num(); ++I)
                    Error += FMath::Abs(int32(Pixels[I].R)-Reference[Stage][I].R)
                        + FMath::Abs(int32(Pixels[I].G)-Reference[Stage][I].G)
                        + FMath::Abs(int32(Pixels[I].B)-Reference[Stage][I].B);
                TestTrue(FString::Printf(TEXT("%s cached/reference pixels agree"), Name), Error<160*160);
            }
            ++Stage;
        };
        Compare(TEXT("Initial"), true);
        auto* ResidentProxy = Mesh->SceneProxy;
        auto CheckResident = [&]()
        {
            if(UseCache)
            {
                TestFalse(TEXT("Visibility does not schedule a geometry rebuild"),Mesh->IsRenderStateDirty());
                TestTrue(TEXT("Visibility retains uploaded terrain/room geometry"),Mesh->SceneProxy==ResidentProxy);
            }
        };
        Owner->SetActorHiddenInGame(true);CheckResident();Compare(TEXT("Hidden room actor"), false);CheckResident();
        Owner->SetActorHiddenInGame(false);CheckResident();Compare(TEXT("Restored room actor"), true);CheckResident();
        Mesh->SetHiddenInGame(true);CheckResident();Compare(TEXT("Hidden terrain"), false);CheckResident();
        Mesh->SetVisibility(false);CheckResident();Compare(TEXT("Hidden terrain visibility"), false);CheckResident();
        Mesh->SetHiddenInGame(false);CheckResident();Compare(TEXT("Terrain still invisible"), false);CheckResident();
        Mesh->SetVisibility(true);CheckResident();Compare(TEXT("Restored terrain"), true);CheckResident();
        Mesh->SetWorldLocation(FVector(0,12,7)); Compare(TEXT("Moved"), true);
        Material->SetScalarParameterValue(TEXT("EmissiveStrength"), .22f); Compare(TEXT("Material parameter"), true);
        Mesh->SetMeshSectionVisible(0, false); Compare(TEXT("Hidden section"), false);
        Mesh->SetMeshSectionVisible(0, true); Compare(TEXT("Restored section"), true);
        TArray<FVector> Smaller = Vertices; for (auto& V : Smaller) { V.Y*=.5f; V.Z*=.5f; }
        Mesh->UpdateMeshSection(0, Smaller, {}, {}, {}, {}); Compare(TEXT("Vertex edit"), true);
        Mesh->ClearMeshSection(0); Compare(TEXT("Cleared"), false);
    }
    // Portal admission must take effect immediately: old hardware occlusion
    // results cannot suppress a room after PView exposes its doorway. Verify
    // both render paths and that explicit portal hiding still removes pixels.
    for (bool UseCache : {false, true})
    {
        auto* Room = World->SpawnActor<AACEEnvCellActor>();
        auto* RoomMesh = Room->CellMesh.Get();
        RoomMesh->bPreferCachedDraws = UseCache;
        RoomMesh->SetMaterial(0, Material);
        RoomMesh->CreateMeshSection(0, Vertices, {0,2,1,0,3,2,0,1,2,0,2,3}, {}, {}, {}, {}, false);
        Capture->ShowOnlyComponents.Reset(); Capture->ShowOnlyComponents.Add(RoomMesh);
        const auto VisiblePixels = Render();
        if (TestNotNull(TEXT("Room has a render proxy"), RoomMesh->SceneProxy))
            TestFalse(TEXT("Portal room cannot reuse stale GPU occlusion"), RoomMesh->SceneProxy->CanBeOccluded());
        int32 Lit = 0; for (const auto& Pixel : VisiblePixels) Lit += FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>20;
        TestTrue(TEXT("Admitted room draws its wall"), Lit>100);
        RoomMesh->SetMeshDrawVisible(false);
        Lit = 0; for (const auto& Pixel : Render()) Lit += FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>20;
        TestEqual(TEXT("PView can still hide the room"), Lit, 0);
        RoomMesh->SetMeshDrawVisible(true);
        TestTrue(TEXT("Readmitted room restores its wall"), Render()==VisiblePixels);
        Capture->ShowOnlyComponents.Reset(); Room->Destroy();
    }
    // Hiding only terrain must preserve the uploaded building/furniture meshes
    // attached to that root, and leave their independently controlled draw visible.
    for(bool Chunk:{false,true})
    {
        auto* Land=Chunk?nullptr:World->SpawnActor<AACELandblockActor>();
        auto* TerrainChunk=Chunk?World->SpawnActor<AACETerrainChunkActor>():nullptr;
        auto* TerrainMesh=Chunk?TerrainChunk->TerrainMesh.Get():Land->TerrainMesh.Get();
        auto* TerrainMaterial=UMaterialInstanceDynamic::Create(Dat->EnsureAceLandMaterialBase(),TerrainMesh);
        TerrainMesh->SetMaterial(0,TerrainMaterial);
        TerrainMesh->CreateMeshSection(0,Vertices,{0,2,1,0,3,2,0,1,2,0,2,3},{},{},{},{},false);
        auto* Scenery=NewObject<UProceduralMeshComponent>(TerrainMesh->GetOwner());
        Scenery->SetupAttachment(TerrainMesh);Scenery->bPreferCachedDraws=true;
        Scenery->SetCollisionEnabled(ECollisionEnabled::NoCollision);Scenery->SetCastShadow(false);
        Scenery->RegisterComponent();Scenery->SetMaterial(0,Material);
        Scenery->CreateMeshSection(0,Vertices,{0,2,1,0,3,2,0,1,2,0,2,3},{},{},{},{},false);
        Capture->ShowOnlyComponents.Reset();Capture->ShowOnlyComponents.Add(Scenery);
        const auto Before=Render();auto* SceneryProxy=Scenery->SceneProxy;
        TestNotNull(TEXT("Attached scenery has uploaded geometry"),SceneryProxy);
        int32 VisiblePixels=0;for(const auto& Pixel:Before)VisiblePixels+=FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>20;
        TestTrue(TEXT("Attached scenery is actually visible before terrain changes"),VisiblePixels>100);
        for(bool Hidden:{true,false})
        {
            if(Chunk)TerrainChunk->SetOutdoorTerrainHiddenInGame(Hidden);else Land->SetOutdoorTerrainHiddenInGame(Hidden);
            TestFalse(TEXT("Terrain visibility does not rebuild attached scenery"),Scenery->IsRenderStateDirty());
            const auto After=Render();
            TestTrue(TEXT("Terrain retains its attached scenery buffers"),Scenery->SceneProxy==SceneryProxy);
            TestTrue(TEXT("Hiding terrain leaves the independently visible building unchanged"),After==Before);
        }
        // The indoor floor-priority switch affects even outdoor landblocks without
        // buildings. It changes a MID scalar, not geometry or material identity.
        Capture->ShowOnlyComponents.Reset();Capture->ShowOnlyComponents.Add(TerrainMesh);
        Render();
        auto* TerrainProxy=TerrainMesh->SceneProxy;
        TestNotNull(TEXT("Terrain geometry has a resident render proxy"),TerrainProxy);
        if (TerrainProxy) TestTrue(TEXT("Outdoor terrain retains hardware occlusion"), TerrainProxy->CanBeOccluded());
        for(bool Priority:{true,false,true,false})
        {
            if(Chunk)TerrainChunk->SetLandEnvCellFloorPriority(Priority,Dat,true);
            else Land->SetLandEnvCellFloorPriority(Priority,Dat,true);
            TestFalse(TEXT("Doorway depth priority does not rebuild terrain buffers"),TerrainMesh->IsRenderStateDirty());
            TestEqual(TEXT("Doorway retains the authored floor-depth adjustment"),
                TerrainMaterial->K2_GetScalarParameterValue(TEXT("LandDepthBias")),Priority?.5f:0.f);
            const auto RetainedPixels=Render();
            TestTrue(TEXT("Crossing retains the terrain render proxy"),TerrainMesh->SceneProxy==TerrainProxy);
            // Compare with the former rebuild path: retaining buffers must not
            // change the visible result of the new material parameter.
            TerrainMesh->MarkRenderStateDirty();
            TestTrue(TEXT("Parameter-only update matches rebuilt terrain pixels"),Render()==RetainedPixels);
            TerrainProxy=TerrainMesh->SceneProxy;
        }
        Capture->ShowOnlyComponents.Reset();
        if(Chunk)TerrainChunk->Destroy();else Land->Destroy();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEMobileShadowTest, "ACE.Rendering.MobileShadowReceiver",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEMobileShadowTest::RunTest(const FString&)
{
    const auto PreviousQuality=Scalability::GetQualityLevels();
    auto Quality=PreviousQuality; Quality.ShadowQuality=3; Scalability::SetQualityLevels(Quality);
    IConsoleManager::Get().CallAllConsoleVariableSinks();
    ON_SCOPE_EXIT { Scalability::SetQualityLevels(PreviousQuality); IConsoleManager::Get().CallAllConsoleVariableSinks(); };
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .RequiresHitProxies(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* GI = NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    FString Directory=TEXT("C:/Turbine/Asheron's Call"); FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
    if (!TestTrue(TEXT("Open DAT for real human shadow"),Dat->LoadDatDirectory(Directory))) return false;
    // AC world coordinates exceed Unreal's default primitive octree extent.
    const FVector Center(2500000, 2500000, 0);
    auto* ReceiverActor = World->SpawnActor<AActor>();
    auto* Receiver = NewObject<UProceduralMeshComponent>(ReceiverActor); ReceiverActor->SetRootComponent(Receiver);
    Receiver->bPreferCachedDraws=true; Receiver->SetCastShadow(false);
    Receiver->CreateMeshSection(0,{{-600,-600,0},{600,-600,0},{600,600,0},{-600,600,0}},
        {0,2,1,0,3,2},{FVector::UpVector,FVector::UpVector,FVector::UpVector,FVector::UpVector},{},{},{},false);
    Receiver->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
    Receiver->RegisterComponent(); ReceiverActor->SetActorLocation(Center);
    FACEWorldObject Human; Human.Guid=0x50000034; Human.SetupId=0x02000001; Human.MotionTableId=0x09000001;
    auto* Player=World->SpawnActor<AACEWorldEntityActor>(); Player->InitializeFromObject(Human,100,true);
    Player->SetActorLocation(Center); Player->Appearance->SetPartsCastShadow(true,false);
    auto* Sky=World->SpawnActor<AACESkyDomeActor>();
    FACEDatSkyTimeOfDay Key; Key.DirBright=.8f; Key.DirPitch=55; Key.DirHeading=90;
    Key.DirColor=0xFFFFFFFF; Key.AmbColor=0xFFFFFFFF; Key.AmbBright=.35f;
    Sky->UpdateWorldLighting(Key,Key,0,.5f);
    auto* Light=Sky->SunLightActor->FindComponentByClass<UDirectionalLightComponent>();
    const bool Mobile=World->GetFeatureLevel()==ERHIFeatureLevel::ES3_1;
    TestTrue(TEXT("Startup permits the mobile shadow region"),IConsoleManager::Get().FindConsoleVariable(TEXT("r.Shadow.CSM.MaxCascades"))->GetInt()>=1);
    TestEqual(TEXT("Startup enables receivers beyond the engine octree"),IConsoleManager::Get().FindConsoleVariable(TEXT("r.Mobile.Shadow.CSMShaderCullingMethod"))->GetInt(),0);
    TestEqual(TEXT("Mobile shadows have no internal quality split that can sweep with head pitch"),Light->DynamicShadowCascades,Mobile?1:4);
    TestEqual(TEXT("Detailed mobile shadows cover forty metres"),Light->DynamicShadowDistanceMovableLight,Mobile?4000.f:60000.f);
    auto* Target=NewObject<UTextureRenderTarget2D>(ReceiverActor); Target->InitCustomFormat(320,240,PF_B8G8R8A8,false);
    auto* Capture=NewObject<USceneCaptureComponent2D>(ReceiverActor); Capture->TextureTarget=Target;
    Capture->CaptureSource=SCS_FinalColorLDR; Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false;
    Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetBloom(false); Capture->ShowFlags.SetAtmosphere(false);
    Capture->ShowFlags.SetFog(false); Capture->ShowFlags.SetAntiAliasing(false); Capture->ShowFlags.SetDynamicShadows(true);
    const FVector Eye=Center+FVector(140,-400,320); Capture->SetWorldLocationAndRotation(Eye,(Center+FVector(80,0,25)-Eye).Rotation());
    Capture->RegisterComponent();
    if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
    auto Render=[&](const TCHAR* Name)
    {
        World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
        TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        TArray64<uint8> Png; FImageUtils::PNGCompressImageArray(320,240,Pixels,Png);
        const FString Path=FPaths::ProjectSavedDir()/TEXT("PerformanceShadowTests")/Name;
        FFileHelper::SaveArrayToFile(Png,*Path); return Pixels;
    };
    // Compare real, multi-part animated geometry and its shadow without
    // recreating cached commands between transform changes.
    TArray<FTransform> OriginalParts;
    for(int32 I=0;I<Player->Appearance->GetPartCount();++I)
        OriginalParts.Add(Player->Appearance->GetPartMesh(I)->GetRelativeTransform());
    TArray<TArray<FColor>> DynamicPoses;
    auto* ActorCache=IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Render.CachedActorDraws"));
    const int32 SavedActorCache=ActorCache->GetInt();
    ON_SCOPE_EXIT { ActorCache->Set(SavedActorCache,ECVF_SetByCode); };
    for(bool CachedDraws : {false,true})
    {
        ActorCache->Set(CachedDraws?1:0,ECVF_SetByCode);
        Player->SetActorLocationAndRotation(Center,FRotator::ZeroRotator);
        for(int32 I=0;I<OriginalParts.Num();++I)
        {
            auto* Part=Cast<UProceduralMeshComponent>(Player->Appearance->GetPartMesh(I));
            TestEqual(TEXT("Live draw-cache toggle updates existing body parts"),Part->bPreferCachedDraws,CachedDraws);
            Part->SetRelativeTransform(OriginalParts[I]);
        }
        for(int32 Pose=0;Pose<3;++Pose)
        {
            if(Pose==1) Player->SetActorLocationAndRotation(Center+FVector(35,20,0),FRotator(0,37,0));
            if(Pose==2 && OriginalParts.Num()>15)
            {
                auto* Arm=Player->Appearance->GetPartMesh(15);
                FTransform Raised=OriginalParts[15];
                Raised.AddToTranslation(FVector(0,0,60));
                Raised.SetRotation((FQuat(FVector::ForwardVector,.7)*Raised.GetRotation()).GetNormalized());
                Arm->SetRelativeTransform(Raised);
            }
            const auto Pixels=Render(*FString::Printf(TEXT("Actor-Cache%d-Pose%d.png"),CachedDraws,Pose));
            if(!CachedDraws) DynamicPoses.Add(Pixels);
            else
            {
                int64 Error=0;
                for(int32 I=0;I<Pixels.Num();++I)
                    Error+=FMath::Abs(int32(Pixels[I].R)-DynamicPoses[Pose][I].R)
                        +FMath::Abs(int32(Pixels[I].G)-DynamicPoses[Pose][I].G)
                        +FMath::Abs(int32(Pixels[I].B)-DynamicPoses[Pose][I].B);
                TestTrue(TEXT("Cached moving parts preserve visible model and shadow pixels"),Error<Pixels.Num());
            }
        }
    }
    TestTrue(TEXT("Moving the model actually changes the rendered fixture"),DynamicPoses[0]!=DynamicPoses[1]);
    TestTrue(TEXT("Moving an individual arm actually changes the rendered fixture"),DynamicPoses[1]!=DynamicPoses[2]);
    Player->SetActorLocationAndRotation(Center,FRotator::ZeroRotator);
    for(int32 I=0;I<OriginalParts.Num();++I) Player->Appearance->GetPartMesh(I)->SetRelativeTransform(OriginalParts[I]);
    const auto With=Render(TEXT("Shadow-On.png"));
    Player->Appearance->SetPartsCastShadow(false,false); const auto Without=Render(TEXT("Shadow-Off.png"));
    int32 Darkened=0;
    for (int32 I=0;I<With.Num();++I) Darkened+=int32(Without[I].R)-With[I].R>12;
    AddInfo(FString::Printf(TEXT("Feature level %d: human shadow darkens %d receiver pixels"),int32(World->GetFeatureLevel()),Darkened));
    TestTrue(TEXT("Runtime cascades cast a human shadow at AC world coordinates"),Darkened>80);
    Player->Appearance->SetPartsCastShadow(true,false);
    Quality.ShadowQuality=0; Scalability::SetQualityLevels(Quality);
    IConsoleManager::Get().CallAllConsoleVariableSinks(); Sky->UpdateWorldLighting(Key,Key,0,.5f);
    const auto SettingsOff=Render(TEXT("Shadow-Settings-Off.png"));
    // Changing scalability also changes sky rendering. Isolate the caster
    // at the same quality so that background changes cannot count as shadow.
    Player->Appearance->SetPartsCastShadow(false,false);
    const auto SettingsOffControl=Render(TEXT("Shadow-Settings-Off-Control.png"));
    Player->Appearance->SetPartsCastShadow(true,false);
    int32 RemainingShadow=0;
    for(int32 I=0;I<SettingsOffControl.Num();++I) RemainingShadow+=int32(SettingsOffControl[I].R)-SettingsOff[I].R>12;
    TestTrue(TEXT("Video Shadows Off removes rendered shadows while preserving the scene"),RemainingShadow<10);
    Quality.ShadowQuality=3; Scalability::SetQualityLevels(Quality);
    // ApplyNonResolutionSettings runs these sinks in gameplay. They rebuild
    // cached mesh passes that were created while shadow quality was zero.
    IConsoleManager::Get().CallAllConsoleVariableSinks(); Sky->UpdateWorldLighting(Key,Key,0,.5f);
    const auto SettingsRestored=Render(TEXT("Shadow-Settings-Restored.png"));
    int32 RestoredShadow=0;
    for(int32 I=0;I<Without.Num();++I) RestoredShadow+=int32(Without[I].R)-SettingsRestored[I].R>12;
    TestTrue(TEXT("Video shadow quality restores real shadows without reloading the world"),RestoredShadow>80);
    // The first-person player hides body parts from the owner, but their actual
    // geometry must still cast shadows. HiddenInGame exercises the same hidden
    // shadow path without making this off-screen capture a player view.
    Player->Appearance->SetPartsCastShadow(true,false);
    for(int32 I=0;I<Player->Appearance->GetPartCount();++I)
        if(auto* Part=Cast<UPrimitiveComponent>(Player->Appearance->GetPartMesh(I)))
        { Part->SetCastHiddenShadow(true); Part->SetHiddenInGame(true); }
    const auto HiddenWith=Render(TEXT("Hidden-Body-Shadow-On.png"));
    Player->Appearance->SetPartsCastShadow(false,false);
    const auto HiddenWithout=Render(TEXT("Hidden-Body-Shadow-Off.png"));
    int32 GroundShadow=0, PreservedShadow=0;
    for(int32 I=0;I<HiddenWith.Num();++I)
    {
        // Exclude the visible character itself: its self-shadow is necessarily
        // absent once hidden. Compare the same ground pixels in both captures.
        const bool Ground=FMath::Abs(int32(Without[I].R)-HiddenWithout[I].R)<5
            && FMath::Abs(int32(Without[I].G)-HiddenWithout[I].G)<5
            && FMath::Abs(int32(Without[I].B)-HiddenWithout[I].B)<5;
        if(Ground && int32(Without[I].R)-With[I].R>12)
        { ++GroundShadow; PreservedShadow+=int32(HiddenWithout[I].R)-HiddenWith[I].R>12; }
    }
    AddInfo(FString::Printf(TEXT("Hidden body preserves %d of %d shadowed ground pixels"),PreservedShadow,GroundShadow));
    TestTrue(TEXT("Hiding the body preserves its full shadow silhouette"),GroundShadow>80 && PreservedShadow>=GroundShadow*.95f);
    return true;
}
#endif
