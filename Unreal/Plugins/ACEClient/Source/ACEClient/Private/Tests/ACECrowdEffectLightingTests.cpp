#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Async/TaskGraphInterfaces.h"
#include "ACEEffectLightSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACEWorldEntityActor.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACESession.h"
#include "ACERuntimeOptions.h"
#include "Misc/ConfigCacheIni.h"
#include "ACEEnvCellActor.h"
#include "Dat/ACEEnvCellTileCache.h"
#include "Dat/ACEOutdoorPortalPlan.h"
#include "RHICommandList.h"
#include "Engine/GameInstance.h"
#include "Misc/ScopeExit.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProceduralMeshComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "UObject/UObjectIterator.h"

namespace ACECrowdLightingTests
{
    struct FDungeonPreferences
    {
        FString OriginalIni=GGameUserSettingsIni;
        IConsoleVariable* Mode=IConsoleManager::Get().FindConsoleVariable(TEXT("ace.DungeonFixtureMode"));
        int32 PreviousMode=Mode->GetInt();
        FDungeonPreferences()
        {
            GGameUserSettingsIni=FPaths::ProjectSavedDir()/TEXT("Automation/DungeonFixturePreferences.ini");
            FConfigFile Config; Config.NoSave=true; GConfig->SetFile(GGameUserSettingsIni,&Config);
            ACERuntimeOptions::Apply();
        }
        ~FDungeonPreferences() { Mode->Set(PreviousMode,ECVF_SetByCode); GGameUserSettingsIni=OriginalIni; ACERuntimeOptions::Apply(); }
    };

    struct FBudgetScope
    {
        IConsoleVariable* Desktop = IConsoleManager::Get().FindConsoleVariable(TEXT("ace.FXLightBudget"));
        IConsoleVariable* Mobile = IConsoleManager::Get().FindConsoleVariable(TEXT("ace.FXLightBudgetMobile"));
        IConsoleVariable* Distance = IConsoleManager::Get().FindConsoleVariable(TEXT("ace.FXLightMaxDistance"));
        int32 PreviousDesktop = Desktop ? Desktop->GetInt() : 0;
        int32 PreviousMobile = Mobile ? Mobile->GetInt() : 0;
        float PreviousDistance = Distance ? Distance->GetFloat() : 0.f;

        FBudgetScope()
        {
            if (Distance) Distance->Set(3500.f, ECVF_SetByCode);
        }

        ~FBudgetScope()
        {
            if (Desktop) Desktop->Set(PreviousDesktop, ECVF_SetByCode);
            if (Mobile) Mobile->Set(PreviousMobile, ECVF_SetByCode);
            if (Distance) Distance->Set(PreviousDistance, ECVF_SetByCode);
        }
        bool IsValid() const { return Desktop && Mobile && Distance; }
        void Set(int32 Value) const
        {
            Desktop->Set(Value, ECVF_SetByCode);
            Mobile->Set(Value, ECVF_SetByCode);
        }
    };

    struct FSource
    {
        AActor* Owner = nullptr;
        UActorComponent* Component = nullptr;
        FACEEffectLightRequest Request;
    };

    struct FWorld
    {
        UWorld* World = nullptr;
        UACEEffectLightSubsystem* Lights = nullptr;
        ACameraActor* View = nullptr;
        APlayerController* PC = nullptr;

        FWorld()
        {
            const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
                .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
            World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            Lights = World->GetSubsystem<UACEEffectLightSubsystem>();
            View = World->SpawnActor<ACameraActor>();
            PC = World->SpawnActor<APlayerController>();
            World->AddController(PC);
            if (!PC->PlayerCameraManager)
            {
                PC->PlayerCameraManager = World->SpawnActor<APlayerCameraManager>();
                PC->PlayerCameraManager->InitializeFor(PC);
            }
            PC->PlayerCameraManager->bUseClientSideCameraUpdates = false;
            PC->SetViewTarget(View);
            SetView(FVector::ZeroVector);
        }
        ~FWorld()
        {
            GEngine->DestroyWorldContext(World);
            World->DestroyWorld(false);
        }
        void SetView(const FVector& Position)
        {
            View->SetActorLocation(Position);
            PC->PlayerCameraManager->UpdateCamera(0.f);
        }
        FSource Source(const FVector& Position, float Intensity = 10.f, float Radius = 320.f)
        {
            FSource Result;
            Result.Owner = World->SpawnActor<AActor>();
            auto* Root = NewObject<USceneComponent>(Result.Owner);
            Result.Owner->AddInstanceComponent(Root);
            Result.Owner->SetRootComponent(Root);
            Root->SetMobility(EComponentMobility::Movable);
            Root->RegisterComponent();
            Result.Owner->SetActorLocation(Position);
            Result.Component = Root;
            Result.Request.EmitterId = 1;
            Result.Request.Position = Position;
            Result.Request.Color = FLinearColor(1.f, .6f, .2f);
            Result.Request.Intensity = Intensity;
            Result.Request.Radius = Radius;
            return Result;
        }
        void Submit(const FSource& Source)
        {
            Lights->SubmitParticles(Source.Component, MakeArrayView(&Source.Request, 1));
        }
        void SubmitAll(const TArray<FSource>& Sources)
        {
            for (const auto& Source : Sources) Submit(Source);
        }
        TArray<UPointLightComponent*> ActualLights(bool VisibleOnly = true) const
        {
            TArray<UPointLightComponent*> Result;
            for (TObjectIterator<UPointLightComponent> It; It; ++It)
            {
                auto* Light = *It;
                if (IsValid(Light) && Light->GetWorld() == World && Light->IsRegistered()
                    && (!VisibleOnly || (Light->IsVisible() && !Light->bHiddenInGame && Light->Intensity > 0.f)))
                    Result.Add(Light);
            }
            return Result;
        }
        float TotalIntensity() const
        {
            float Result = 0.f;
            for (const auto* Light : ActualLights()) Result += Light->Intensity;
            return Result;
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEDungeonFixtureLightTest, "ACE.Rendering.DungeonFixtureLights",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEDungeonFixtureLightTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FDungeonPreferences Preferences;
    FBudgetScope Budget; Budget.Set(8);
    FWorld F;
    auto* GI=NewObject<UGameInstance>(GEngine);
    GEngine->GetWorldContextFromWorldChecked(F.World).OwningGameInstance=GI;
    F.World->SetGameInstance(GI); GI->OnWorldChanged(nullptr,F.World); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); };
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    FString Directory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
    if (!TestTrue(TEXT("Retail dungeon DAT opens"),Dat->LoadDatDirectory(Directory))) return false;
    constexpr uint32 CellId=0x01430100;
    const auto* Mesh=Dat->GetOrBuildEnvCellMesh(CellId,100);
    if (!TestNotNull(TEXT("Two-lamp room decoded"),Mesh)) return false;
    const FString CaptureDir=FPaths::ProjectSavedDir()/TEXT("Automation/DungeonFixtureLights");
    IFileManager::Get().MakeDirectory(*CaptureDir,true);
    const FString TilePath=CaptureDir/TEXT("Room.acec");
    FACEBuiltEnvCellMesh Cached;
    if (!TestTrue(TEXT("Fixture metadata survives a tile cache reload"),
        ACEEnvCellTileCache::Save(TilePath,*Mesh,100,1) && ACEEnvCellTileCache::Load(TilePath,Cached,100,1) && Cached.PointLights.Num()==2)) return false;
    auto* Room=F.World->SpawnActor<AACEEnvCellActor>();
    if (!TestTrue(TEXT("Room and fixtures streamed"),Room->LoadEnvCell(CellId,FVector::ZeroVector,100))) return false;
    Room->SetEnvCellHiddenInGame(false);
    F.SetView(FVector(0,-2000,160));
    F.Lights->RefreshLights(.1f); F.Lights->RefreshLights(1.f);
    auto Lamps=F.ActualLights();
    if (!TestEqual(TEXT("Both DAT fixture points persist without particle submissions"),Lamps.Num(),2)) return false;
    for (auto* Lamp : Lamps)
    {
        TestTrue(TEXT("Enclosed lamps cast local shadows"),Lamp->CastShadows && Lamp->LightingChannels.bChannel1 && !Lamp->LightingChannels.bChannel0);
        TestEqual(TEXT("Authored four-metre falloff uses retail static light factor"),Lamp->AttenuationRadius,520.f);
        TestTrue(TEXT("Lamp is at the authored elevated wall mount"),FMath::Abs(Lamp->GetComponentLocation().Z-209.f)<2.f);
    }
    TestTrue(TEXT("Room shell occludes local lights"),Room->CellMesh->CastShadow && Room->CellMesh->bCastDynamicShadow);
    for (const auto& Stab : Cached.StaticObjects)
    {
        Dat->GetOrBuildSetupMesh(Stab.Id,100,UACEDatSubsystem::ACEPlacementResting);
        if (!TestTrue(TEXT("Real dungeon scenery spawned"),Room->TrySpawnOneStaticObject(Dat,Stab))) return false;
    }
    if (!FApp::CanEverRender()) return false;
    const FVector Mount=Lamps[0]->GetComponentLocation();
    const FVector Eye=Mount+(FVector(0,-2000,0)-Mount).GetSafeNormal2D()*300+FVector(0,0,30);
    auto* Target=NewObject<UTextureRenderTarget2D>(Room);
    constexpr int32 Size=512;
    Target->InitCustomFormat(Size,Size,PF_FloatRGBA,false);
    auto* Capture=NewObject<USceneCaptureComponent2D>(Room);
    Capture->TextureTarget=Target; Capture->CaptureSource=SCS_SceneColorHDRNoAlpha;
    Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false; Capture->bAlwaysPersistRenderingState=true;
    Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetFog(false); Capture->ShowFlags.SetAntiAliasing(false);
    Capture->SetWorldLocationAndRotation(Eye,(Mount-FVector(0,0,160)-Eye).Rotation()); Capture->RegisterComponent();
    if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
    const double ReadyDeadline=FPlatformTime::Seconds()+5.;
    do
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        F.World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
        if (Room->CellMesh->GetSceneProxy() && !Room->CellMesh->IsPSOPrecaching()) break;
        FPlatformProcess::SleepNoStats(.001f);
    } while (FPlatformTime::Seconds()<ReadyDeadline);
    if (!TestNotNull(TEXT("Dungeon shell has a render proxy"),Room->CellMesh->GetSceneProxy())) return false;
    auto CaptureFrame=[&](const TCHAR* Name)
    {
        F.World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
        TArray<FLinearColor> Pixels;
        Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels,FReadSurfaceDataFlags(RCM_MinMax));
        TArray<FColor> Display;
        Display.Reserve(Pixels.Num());
        for (const auto& P:Pixels) Display.Add(P.CopyWithNewOpacity(1.f).ToFColor(true));
        if (Display.Num()==Size*Size)
        {
            TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size,Size,Display,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(CaptureDir/Name));
        }
        return Pixels;
    };
    Preferences.Mode->Set(0,ECVF_SetByCode); Budget.Set(0); F.Lights->RefreshLights(.1f);
    const auto Ambient=CaptureFrame(TEXT("Ambient.png"));
    Preferences.Mode->Set(2,ECVF_SetByCode); Budget.Set(8); F.Lights->RefreshLights(.1f);
    const auto Shadow=CaptureFrame(TEXT("Shadow.png"));
    // Same room, exposure, scenery and fixtures. Only disable the shadow maps
    // for the control image; measure shadows cast by the real torch and room.
    for (auto* Lamp:F.ActualLights()) Lamp->SetCastShadows(false);
    const auto Lit=CaptureFrame(TEXT("Unshadowed.png"));
    if (!TestTrue(TEXT("Dungeon scene captured three times"),Lit.Num()==Size*Size && Shadow.Num()==Lit.Num() && Ambient.Num()==Lit.Num())) return false;
    int LitPixels=0, ShadowPixels=0;
    for (int I=0; I<Lit.Num(); ++I)
    {
        if (Shadow[I].GetLuminance()-Ambient[I].GetLuminance()>.01f) ++LitPixels;
        if (Lit[I].GetLuminance()-Shadow[I].GetLuminance()>.01f) ++ShadowPixels;
    }
    AddInfo(FString::Printf(TEXT("Dungeon 01430100: %d lit pixels, %d shadowed pixels"),LitPixels,ShadowPixels));
    TestTrue(TEXT("Fixture illuminates visible dungeon surfaces"),LitPixels>200);
    TestTrue(TEXT("Dungeon scenery casts visible shadows from the DAT fixtures"),ShadowPixels>100);
    ACERuntimeOptions::Set(TEXT("UniformDungeonLighting"),1); ACERuntimeOptions::Apply();
    F.Lights->RefreshLights(.1f);
    TestEqual(TEXT("Uniform rooms release both local lights"),F.Lights->GetVisibleLightCount(),0);
    const auto Uniform=CaptureFrame(TEXT("Uniform.png"));
    ACERuntimeOptions::Set(TEXT("UniformDungeonLighting"),0); ACERuntimeOptions::Apply();
    F.Lights->RefreshLights(.1f);
    const auto Restored=CaptureFrame(TEXT("Restored.png"));
    if (!TestTrue(TEXT("Both lighting modes render"),Uniform.Num()==Shadow.Num() && Restored.Num()==Shadow.Num())) return false;
    int Brightened=0, RestoredPixels=0;
    for (int I=0; I<Shadow.Num(); ++I)
    {
        Brightened += Uniform[I].GetLuminance()>Shadow[I].GetLuminance()+.02f;
        RestoredPixels += Restored[I].Equals(Shadow[I],.005f);
    }
    TestTrue(TEXT("Uniform mode brightens shadowed surfaces in the loaded dungeon"),Brightened>200);
    TestTrue(TEXT("Disabling uniform restores the same shadowed room"),RestoredPixels>Shadow.Num()*.99);
    const FVector IntoRoom=(FVector(0,-2000,0)-Mount).GetSafeNormal2D();
    const FVector BacklitEye=Mount+IntoRoom*100.-FVector(0,0,70);
    const FVector FloorAhead=Mount+IntoRoom*230.-FVector(0,0,209);
    Capture->SetWorldLocationAndRotation(BacklitEye,(FloorAhead-BacklitEye).Rotation());
    TestTrue(TEXT("Test torch mount is behind the camera"),FVector::DotProduct(Mount-BacklitEye,Capture->GetForwardVector())<0.);
    for (auto* Lamp:F.ActualLights()) Lamp->SetVisibility(Lamp->GetComponentLocation().Equals(Mount));
    const auto Backlit=CaptureFrame(TEXT("OffscreenTorch.png"));
    Preferences.Mode->Set(0,ECVF_SetByCode); Budget.Set(0); F.Lights->RefreshLights(.1f);
    const auto Control=CaptureFrame(TEXT("OffscreenTorchOff.png"));
    int32 BacklitPixels=0;
    if (!TestTrue(TEXT("Backlit floor captured"),Backlit.Num()==Size*Size && Backlit.Num()==Control.Num())) return false;
    for (int I=0; I<Control.Num(); ++I) BacklitPixels+=Backlit[I].GetLuminance()-Control[I].GetLuminance()>.01f;
    TestTrue(TEXT("Offscreen torch still illuminates visible floor"),BacklitPixels>100);
    Room->Destroy(); F.Lights->RefreshLights(.1f);
    TestEqual(TEXT("Unloading the room releases its fixtures"),F.Lights->GetVisibleLightCount(),0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEDungeonHeadingLightTest, "ACE.Rendering.DungeonTorchHeading",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEDungeonHeadingLightTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FDungeonPreferences Preferences;
    FBudgetScope Budget;
    FWorld F;
    auto* GI=NewObject<UGameInstance>(GEngine);
    GEngine->GetWorldContextFromWorldChecked(F.World).OwningGameInstance=GI;
    F.World->SetGameInstance(GI); GI->OnWorldChanged(nullptr,F.World); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); };
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    FString Directory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
    if (!TestTrue(TEXT("Dungeon DAT opens"),Dat->LoadDatDirectory(Directory))) return false;
    FACEPosition Pose;
    Pose.CellId=0x01D9010B; Pose.Location=FVector(31.136580,-26.668570,.006);
    GI->GetSubsystem<UACEClientSubsystem>()->GetSession()->SetLocalPosition(Pose);
    const auto* Cell=Dat->GetOrBuildEnvCellMesh(Pose.CellId,100);
    if (!TestNotNull(TEXT("Reported dungeon cell decoded"),Cell)) return false;
    const auto Neighbors=Cell->VisibleCells;
    TArray<AACEEnvCellActor*> Rooms;
    for (uint16 Id : Neighbors)
    {
        const uint32 FullId=(uint32(Pose.CellId)&0xffff0000u)|Id;
        const auto* Neighbor=Dat->GetOrBuildEnvCellMesh(FullId,100);
        if (!Neighbor || Neighbor->PointLights.IsEmpty()) continue;
        auto* Room=F.World->SpawnActor<AACEEnvCellActor>();
        if (!TestTrue(TEXT("Adjacent torch room streamed"),Room->LoadEnvCell(FullId,FVector::ZeroVector,100))) return false;
        Rooms.Add(Room);
    }
    auto* Pawn=F.World->SpawnActor<APawn>();
    auto* Root=NewObject<USceneComponent>(Pawn);
    Pawn->SetRootComponent(Root); Root->RegisterComponent();
    Pawn->SetActorLocation(FACEPosition::AceVectorToUnreal(Pose.Location,100));
    F.PC->Possess(Pawn); F.PC->SetViewTarget(F.View);
    Budget.Distance->Set(1900.f,ECVF_SetByCode);
    for (const int32 Mode : {0,2})
    {
        Preferences.Mode->Set(Mode,ECVF_SetByCode);
        Budget.Set(2);
        TMap<FVector,TPair<float,bool>> InitialLights;
        for (int32 Angle=0; Angle<=360; Angle+=30)
        {
            const FRotator Heading(0,Angle,0);
            Pawn->SetActorRotation(Heading);
            F.SetView(Pawn->GetActorLocation()-Heading.Vector()*500.+FVector(0,0,200));
            for (int32 I=0; I<Rooms.Num(); ++I) Rooms[I]->SetEnvCellHiddenInGame((Angle/30+I)%2==0);
            F.Lights->RefreshLights(.1f);
            const auto Lamps=F.ActualLights();
            if (Angle==0)
            {
                if (!TestTrue(TEXT("Nearby fixtures receive light slots"),Lamps.Num()>=2)) return false;
                if (Mode==2) TestTrue(TEXT("All eleven authored fixtures bypass the effect cap and distance limit"),Lamps.Num()==11);
                for (auto* Lamp:Lamps) InitialLights.Add(Lamp->GetComponentLocation(),{Lamp->Intensity,bool(Lamp->CastShadows)});
            }
            if (!TestEqual(TEXT("Torch count stays stable while turning"),Lamps.Num(),InitialLights.Num())) return false;
            for (auto* Lamp:Lamps)
            {
                const auto* Previous=InitialLights.Find(Lamp->GetComponentLocation());
                if (!TestTrue(TEXT("Same torch keeps its intensity and shadow assignment"),
                    Previous && FMath::IsNearlyEqual(Previous->Key,Lamp->Intensity) && Previous->Value==bool(Lamp->CastShadows))) return false;
            }
        }
    }
    auto* ShadowQuality=IConsoleManager::Get().FindConsoleVariable(TEXT("sg.ShadowQuality"));
    const int32 PreviousShadowQuality=ShadowQuality->GetInt();
    ON_SCOPE_EXIT { ShadowQuality->SetWithCurrentPriority(PreviousShadowQuality); };
    Preferences.Mode->Set(-1,ECVF_SetByCode);
    Budget.Set(0); // The transient FX count is independent of full desktop fixtures.
    for (const int32 Quality : {1,2,3,4})
    {
        ShadowQuality->SetWithCurrentPriority(Quality);
        F.Lights->RefreshLights(.1f);
        const auto Lamps=F.ActualLights();
        TestEqual(TEXT("Shadow quality preserves every fixture"),Lamps.Num(),11);
        int32 ShadowCount=0;
        for (const auto* Lamp:Lamps) ShadowCount+=bool(Lamp->CastShadows);
        TestEqual(TEXT("Only shadow count scales with shadow quality"),ShadowCount,Quality>=3 ? 11 : 2);
    }
    // Model a camera-visible room outside the occupied cell's visibility list.
    const auto* ExtraCell=Dat->GetOrBuildEnvCellMesh(0x015D0251,100);
    if (!TestTrue(TEXT("Additional room contains fixtures"),ExtraCell && !ExtraCell->PointLights.IsEmpty())) return false;
    const int32 ExtraLights=ExtraCell->PointLights.Num();
    auto* Extra=F.World->SpawnActor<AACEEnvCellActor>();
    if (!TestTrue(TEXT("Additional fixture room streamed"),Extra->LoadEnvCell(0x015D0251,FVector::ZeroVector,100))) return false;
    for (bool bHidden : {true,false,true})
    {
        Extra->SetEnvCellHiddenInGame(bHidden);
        F.Lights->RefreshLights(.1f);
        TestEqual(TEXT("Camera-visible room supplies lights beyond player visibility list"),
            F.ActualLights().Num(),11+(bHidden ? 0 : ExtraLights));
    }
    Extra->Destroy();
    // Keep the camera fixed and walk between two real torch mounts.
    Preferences.Mode->Set(0,ECVF_SetByCode);
    Budget.Set(1);
    const auto* TorchRoom=Dat->FindEnvCellMesh(0x01D90111,100);
    if (!TestTrue(TEXT("Adjacent room has both wall torches"),TorchRoom && TorchRoom->PointLights.Num()==2)) return false;
    for (const auto& Torch : TorchRoom->PointLights)
    {
        Pawn->SetActorLocation(Torch.BlockPosition-FVector(0,0,150));
        F.Lights->RefreshLights(.1f);
        const auto Lamps=F.ActualLights();
        if (!TestTrue(TEXT("Walking shifts the budget to the closer torch"),
            Lamps.Num()==1 && Lamps[0]->GetComponentLocation().Equals(Torch.BlockPosition,1.f))) return false;
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEDungeonLightPerformanceTest, "ACE.Performance.DungeonFixtureLighting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)
bool FACEDungeonLightPerformanceTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    if (!TestTrue(TEXT("GPU timestamps and a renderer available"),FApp::CanEverRender() && GSupportsTimestampRenderQueries)) return false;
    AddInfo(FString::Printf(TEXT("DungeonBench adapter=%s shadowQuality=%d r.ShadowQuality=%d"),*GRHIAdapterName,
        IConsoleManager::Get().FindConsoleVariable(TEXT("sg.ShadowQuality"))->GetInt(),
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShadowQuality"))->GetInt()));
    FDungeonPreferences Preferences;
    FBudgetScope Budget; Budget.Set(8);
    FWorld F;
    auto* GI=NewObject<UGameInstance>(GEngine);
    GEngine->GetWorldContextFromWorldChecked(F.World).OwningGameInstance=GI;
    F.World->SetGameInstance(GI); GI->OnWorldChanged(nullptr,F.World); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); };
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    FString Directory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
    if (!Dat->LoadDatDirectory(Directory)) return false;
    auto* Pawn=F.World->SpawnActor<APawn>();
    auto* Root=NewObject<USceneComponent>(Pawn); Pawn->SetRootComponent(Root); Root->RegisterComponent();
    F.PC->Possess(Pawn); F.PC->SetViewTarget(F.View);
    auto* Target=NewObject<UTextureRenderTarget2D>(Pawn);
    const FIntPoint Size=FParse::Param(FCommandLine::Get(),TEXT("DungeonBench1440p")) ? FIntPoint(2560,1440) : FIntPoint(1920,1080);
    Target->InitCustomFormat(Size.X,Size.Y,PF_FloatRGBA,false);
    auto* Capture=NewObject<USceneCaptureComponent2D>(Pawn);
    Capture->TextureTarget=Target; Capture->FOVAngle=90; Capture->CaptureSource=SCS_SceneColorHDRNoAlpha;
    Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false; Capture->bAlwaysPersistRenderingState=true;
    Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetFog(false);
    Capture->RegisterComponent();
    constexpr int32 Frames=24;
    struct FTiming { FRenderQueryRHIRef Start, End; };
    TArray<FTiming> Queries;
    for (int32 I=0; I<Frames; ++I) Queries.Add({RHICreateRenderQuery(RQT_AbsoluteTime),RHICreateRenderQuery(RQT_AbsoluteTime)});
    for (uint32 Id : {0x01D9010Bu,0x015D0251u})
    {
        FACEPosition Pose; Pose.CellId=Id;
        Pose.Location=Id==0x01D9010B ? FVector(31.136580,-26.668570,.006) : FVector(20,-63,36.01);
        GI->GetSubsystem<UACEClientSubsystem>()->GetSession()->SetLocalPosition(Pose);
        const FVector Feet=FACEPosition::AceVectorToUnreal(Pose.Location,100);
        Pawn->SetActorLocation(Feet);
        const auto* Cell=Dat->GetOrBuildEnvCellMesh(Id,100);
        if (!TestNotNull(TEXT("Benchmark room decoded"),Cell)) return false;
        auto Cells=Cell->VisibleCells; Cells.AddUnique(uint16(Id));
        TArray<AACEEnvCellActor*> Rooms;
        for (uint16 Short : Cells)
        {
            const uint32 FullId=(Id&0xffff0000u)|Short;
            const auto* Mesh=Dat->GetOrBuildEnvCellMesh(FullId,100);
            if (!Mesh || Mesh->IsEmpty()) continue;
            const auto Stabs=Mesh->StaticObjects;
            auto* Room=F.World->SpawnActor<AACEEnvCellActor>();
            if (!Room->LoadEnvCell(FullId,FVector::ZeroVector,100)) return false;
            Room->SetEnvCellHiddenInGame(false);
            for (const auto& Stab:Stabs)
            {
                Dat->GetOrBuildSetupMesh(Stab.Id,100,UACEDatSubsystem::ACEPlacementResting);
                Room->TrySpawnOneStaticObject(Dat,Stab);
            }
            Rooms.Add(Room);
        }
        TArray<AACEWorldEntityActor*> Movers;
        for (int32 I=0; I<2; ++I)
        {
            FACEWorldObject Mob; Mob.Guid=0x7100B000+I; Mob.ItemType=ACEItemType::Creature;
            Mob.SetupId=0x02001A33; Mob.MotionTableId=0x0900000C; Mob.Position=Pose;
            auto* Actor=F.World->SpawnActor<AACEWorldEntityActor>();
            Actor->InitializeFromObject(Mob,100,true);
            if (!TestTrue(TEXT("Moving shadow caster loaded"),Actor->Appearance->HasAppearance())) return false;
            Actor->Appearance->SetPartsCastShadow(true);
            Movers.Add(Actor);
        }
        if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
        TArray<double> Gpu[3],Cpu[3]; int32 Counts[3]={}, InView[3]={}, Shadows[3]={};
        TArray<FColor> Images[3];
        for (int Mode : {0,1,2,1,2,0,2,0,1})
        {
            Preferences.Mode->Set(Mode,ECVF_SetByCode);
            // Identical pan and geometry in each pass. Warm the shadow caches
            // and PSOs before timestamping; rotate the order between passes.
            for (int I=-8; I<Frames; ++I)
            {
                const float Pan=12.f*FMath::Sin(2.f*PI*FMath::Max(0,I)/Frames);
                const FRotator Rotation(-8, (Id==0x01D9010B ? 103.5f : 90.f)+Pan,0);
                const FVector Eye=Feet+FVector(0,0,165);
                for (int J=0; J<Movers.Num(); ++J)
                    Movers[J]->SetActorLocation(Feet+FVector((J ? 170. : -150.)+Pan*3.,260.+J*35.,0.));
                F.View->SetActorRotation(Rotation); F.SetView(Eye);
                Capture->SetWorldLocationAndRotation(Eye,Rotation);
                const double Start=FPlatformTime::Seconds();
                F.Lights->RefreshLights(1.f/60.f);
                if (I>=0) Cpu[Mode].Add((FPlatformTime::Seconds()-Start)*1000.);
                F.World->SendAllEndOfFrameUpdates();
                if (I>=0)
                {
                    ENQUEUE_RENDER_COMMAND(BeginDungeonTiming)([Q=Queries[I].Start](FRHICommandListImmediate& Cmd){ Cmd.EndRenderQuery(Q); });
                }
                Capture->CaptureScene();
                if (I>=0)
                {
                    ENQUEUE_RENDER_COMMAND(EndDungeonTiming)([Q=Queries[I].End](FRHICommandListImmediate& Cmd){ Cmd.EndRenderQuery(Q); });
                }
                if (I==-1) FlushRenderingCommands();
            }
            ENQUEUE_RENDER_COMMAND(ReadDungeonTimings)([&Queries,&Gpu,Mode](FRHICommandListImmediate& Cmd)
            {
                Cmd.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
                for (auto& Q:Queries)
                {
                    uint64 A=0,B=0;
                    if (RHIGetRenderQueryResult(Q.Start,A,true) && RHIGetRenderQueryResult(Q.End,B,true) && B>A)
                        Gpu[Mode].Add(double(B-A)/1000.);
                }
            });
            FlushRenderingCommands();
            FVector Camera; FConvexVolume Frustum;
            if (!ACEOutdoorPortalPlan::TryGetPlayerViewFrustum(F.World,Camera,Frustum)) return false;
            const auto Lamps=F.ActualLights(); Counts[Mode]=Lamps.Num(); InView[Mode]=0; Shadows[Mode]=0;
            for (const auto* L:Lamps)
            {
                InView[Mode]+=Frustum.IntersectSphere(L->GetComponentLocation(),L->AttenuationRadius);
                Shadows[Mode]+=bool(L->CastShadows);
            }
            if (Images[Mode].IsEmpty())
            {
                auto* Resource=Target->GameThread_GetRenderTargetResource();
                if (!Resource->ReadPixels(Images[Mode],FReadSurfaceDataFlags(RCM_MinMax))) return false;
                const FString Out=FPaths::ProjectSavedDir()/TEXT("Automation/DungeonLightBenchmark");
                IFileManager::Get().MakeDirectory(*Out,true);
                for (auto& Pixel:Images[Mode]) Pixel.A=255;
                TArray64<uint8> Png;
                FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Images[Mode],Png);
                FFileHelper::SaveArrayToFile(Png,*(Out/FString::Printf(TEXT("%08X-%dp-mode%d.png"),Id,Size.Y,Mode)));
            }
        }
        for (int Mode=0; Mode<3; ++Mode)
        {
            Gpu[Mode].Sort(); Cpu[Mode].Sort();
            if (!TestEqual(TEXT("All frames returned GPU timestamps"),Gpu[Mode].Num(),Frames*3)) return false;
            const int P50=Frames*3/2, P95=FMath::CeilToInt(Frames*3*.95)-1;
            AddInfo(FString::Printf(TEXT("DungeonBench cell=%08X mode=%d resolution=%dx%d frames=%d lights=%d frustum=%d shadows=%d gpu_ms_p50=%.3f gpu_ms_p95=%.3f light_cpu_ms_p50=%.4f light_cpu_ms_p95=%.4f"),
                Id,Mode,Size.X,Size.Y,Gpu[Mode].Num(),Counts[Mode],InView[Mode],Shadows[Mode],Gpu[Mode][P50],Gpu[Mode][P95],Cpu[Mode][P50],Cpu[Mode][P95]));
        }
        TestTrue(TEXT("Uncapped benchmark actually exercises additional fixtures"),Counts[2]>Counts[0]);
        int32 Drawn=0, Changed=0;
        for (int32 I=0; I<Images[0].Num(); ++I)
        {
            const auto& A=Images[0][I]; const auto& B=Images[2][I];
            Drawn+=FMath::Max3(B.R,B.G,B.B)>10;
            Changed+=FMath::Max3(FMath::Abs(int32(A.R)-B.R),FMath::Abs(int32(A.G)-B.G),FMath::Abs(int32(A.B)-B.B))>5;
        }
        TestTrue(TEXT("Captured dungeon geometry fills the view"),Drawn>Size.X*Size.Y/10);
        AddInfo(FString::Printf(TEXT("DungeonBench image cell=%08X drawn=%d changed=%d of %d pixels"),Id,Drawn,Changed,Images[0].Num()));
        for (auto* Room:Rooms) Room->Destroy();
        for (auto* Actor:Movers) Actor->Destroy();
        F.Lights->RefreshLights(.1f);
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECrowdLightBudgetTest, "ACE.Rendering.CrowdEffectLighting.Budget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECrowdLightBudgetTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FBudgetScope Budget;
    if (!TestTrue(TEXT("Both platform light budgets exist"), Budget.IsValid())) return false;
    Budget.Set(8);
    FWorld F;
    if (!TestNotNull(TEXT("Each world owns a light subsystem"), F.Lights)) return false;
    TArray<FSource> Sources;
    for (int32 I = 0; I < 128; ++I) Sources.Add(F.Source(FVector(I % 8, (I / 8) % 8, 100 + I / 64)));
    F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("128 sources select only eight lights"), F.Lights->GetVisibleLightCount(), 8);
    TestEqual(TEXT("Renderer has only eight visible actual point lights"), F.ActualLights().Num(), 8);
    TestEqual(TEXT("Unselected sources do not allocate hidden private light pools"), F.ActualLights(false).Num(), 8);
    TestTrue(TEXT("A packed crowd's total light energy stays bounded"), F.TotalIntensity() > 0.f && F.TotalIntensity() <= 12.001f);
    const int32 InitialPoolSize = F.Lights->GetAllocatedLightCount();

    for (int32 I = 0; I < Sources.Num(); ++I)
    {
        Sources[I].Request.Position = FVector((I % 8) * 150.f, ((I / 8) % 4) * 150.f, 100.f + (I / 32) * 150.f);
        Sources[I].Request.Radius = 30.f;
    }
    F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Dispersed sources remain globally bounded"), F.ActualLights().Num(), 8);
    TestTrue(TEXT("Separated effects retain their individual light strength"), F.TotalIntensity() > 70.f);
    TestEqual(TEXT("Moving to a new crowd reuses the same pool"), F.Lights->GetAllocatedLightCount(), InitialPoolSize);

    Budget.Set(2); F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Reducing the budget hides excess lights"), F.ActualLights().Num(), 2);
    Budget.Set(0); F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Zero budget disables all effect illumination"), F.ActualLights().Num(), 0);
    for (const auto& Source : Sources)
        F.Lights->UpdateParticlePosition(Source.Component, 1, FVector::ZeroVector);
    TestEqual(TEXT("Presentation updates cannot resurrect rejected lights"), F.ActualLights().Num(), 0);
    Budget.Set(8); F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Increasing the budget restores selected effects"), F.ActualLights().Num(), 8);
    TestEqual(TEXT("Budget changes do not grow a second light pool"), F.Lights->GetAllocatedLightCount(), InitialPoolSize);

    for (const auto& Source : Sources) F.Lights->RemoveSource(Source.Component);
    F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Stopping all sources clears actual illumination"), F.ActualLights().Num(), 0);
    auto Single = F.Source(FVector(100, 0, 0));
    TArray<FACEEffectLightRequest> Emitters;
    for (uint32 Id = 1; Id <= 3; ++Id)
    {
        auto Request = Single.Request; Request.EmitterId = Id; Emitters.Add(Request);
    }
    F.Lights->SubmitParticles(Single.Component, Emitters); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Three emitters on one carried item receive one light"), F.Lights->GetVisibleLightCount(Single.Component), 1);
    int32 Selected = 0;
    for (uint32 Id = 1; Id <= 3; ++Id) Selected += F.Lights->IsParticleSelected(Single.Component, Id) ? 1 : 0;
    TestEqual(TEXT("Selection and actual source light counts agree"), Selected, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECrowdLightSelectionTest, "ACE.Rendering.CrowdEffectLighting.SelectionAndLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECrowdLightSelectionTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FBudgetScope Budget;
    if (!TestTrue(TEXT("Light budgets exist"), Budget.IsValid())) return false;
    Budget.Set(1);
    FWorld F;
    if (!TestNotNull(TEXT("Light manager initializes"), F.Lights)) return false;
    auto Far = F.Source(FVector(1500, 0, 0));
    F.Submit(Far); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("First available source is selected"), F.Lights->IsParticleSelected(Far.Component, 1));
    auto Near = F.Source(FVector(100, 0, 0));
    F.Submit(Far); F.Submit(Near); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("A newly arriving nearby effect replaces the far incumbent"), F.Lights->IsParticleSelected(Near.Component, 1));
    TestFalse(TEXT("A source cannot monopolize the budget by registering first"), F.Lights->IsParticleSelected(Far.Component, 1));
    F.Lights->UpdateParticlePosition(Far.Component, 1, FVector(1, 0, 0));
    TestFalse(TEXT("Updating a rejected particle cannot select it outside the budget pass"), F.Lights->IsParticleSelected(Far.Component, 1));

    // Nearly equidistant sources must not exchange ownership on tiny head motion.
    Far.Request.Position = FVector(-101, 0, 0);
    F.Submit(Far); F.Submit(Near); F.Lights->RefreshLights(.05f);
    auto* StableLight = F.Lights->FindParticleLight(Near.Component, 1);
    F.SetView(FVector(-1, 0, 0));
    F.Submit(Far); F.Submit(Near); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("Small camera motion retains the close incumbent"), F.Lights->IsParticleSelected(Near.Component, 1));
    TestTrue(TEXT("Stable selection reuses its point light"), F.Lights->FindParticleLight(Near.Component, 1) == StableLight);
    Far.Request.Position = FVector(1500, 0, 0); F.SetView(FVector(1500, 0, 0));
    F.Submit(Far); F.Submit(Near); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("Meaningful camera movement updates proximity selection"), F.Lights->IsParticleSelected(Far.Component, 1));

    F.Lights->RemoveSource(Near.Component);
    F.Lights->RemoveEmitter(Far.Component, 1); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Removing the selected emitter retires its illumination"), F.ActualLights().Num(), 0);
    F.Submit(Far); F.Lights->RefreshLights(.05f);
    F.Lights->SubmitParticles(Far.Component, TConstArrayView<FACEEffectLightRequest>()); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("An empty submission clears a previous emitter set"), F.ActualLights().Num(), 0);
    F.Submit(Far); F.Lights->RefreshLights(.05f);
    F.Lights->RefreshLights(.30f);
    TestEqual(TEXT("An idle particle source expires without a final component tick"), F.ActualLights().Num(), 0);
    F.Submit(Far); F.Lights->RefreshLights(.40f);
    TestEqual(TEXT("A fresh submission survives the frame that follows a long hitch"), F.ActualLights().Num(), 1);
    F.Lights->RefreshLights(.30f);
    TestEqual(TEXT("An untouched source still expires after the hitch frame"), F.ActualLights().Num(), 0);
    F.Submit(Far); F.Lights->RefreshLights(.05f);
    Far.Owner->SetActorHiddenInGame(true); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Hidden owners stop illuminating the scene"), F.ActualLights().Num(), 0);
    Far.Owner->SetActorHiddenInGame(false); F.Submit(Far); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("A visible owner can submit again"), F.ActualLights().Num(), 1);
    Far.Owner->Destroy(); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Destroyed owners cannot leave pooled lights visible"), F.ActualLights().Num(), 0);

    auto Persistent = F.Source(FVector(200, 0, 0), 6.4f);
    F.SetView(FVector::ZeroVector);
    F.Lights->SetScriptLight(Persistent.Component, true, Persistent.Request); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Authored SetLight uses the shared pool"), F.Lights->GetVisibleLightCount(Persistent.Component), 1);
    Persistent.Owner->SetActorLocation(FVector(250, 30, 20));
    F.Lights->RefreshLights(.40f);
    auto* ScriptLight = F.Lights->FindScriptLight(Persistent.Component);
    if (!TestNotNull(TEXT("Authored light persists while its source is idle"), ScriptLight)) return false;
    TestTrue(TEXT("Idle authored light follows the moving owner"), ScriptLight->GetComponentLocation().Equals(Persistent.Owner->GetActorLocation(), .01f));
    F.Lights->RemoveParticles(Persistent.Component); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Removing particles preserves the independent authored light"), F.ActualLights().Num(), 1);
    F.Lights->SetScriptLight(Persistent.Component, false, Persistent.Request); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("SetLight off releases authored illumination"), F.ActualLights().Num(), 0);
    F.Lights->SetScriptLight(Persistent.Component, true, Persistent.Request); F.Lights->RefreshLights(.05f);
    F.Lights->RemoveSource(Persistent.Component); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Source end removes both particle and authored illumination"), F.ActualLights().Num(), 0);

    auto Mixed = F.Source(FVector(100, 0, 0));
    F.Submit(Mixed); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("Mixed source first establishes an inferred incumbent"), F.Lights->IsParticleSelected(Mixed.Component, 1));
    auto Authored = Mixed.Request;
    Authored.Color = FLinearColor(.2f, .8f, 1.f); Authored.Intensity = 6.4f;
    F.Lights->SetScriptLight(Mixed.Component, true, Authored);
    F.Submit(Mixed); F.Lights->RefreshLights(.05f);
    auto* AuthoredLight = F.Lights->FindScriptLight(Mixed.Component);
    if (!TestNotNull(TEXT("Explicit SetLight replaces the same source's inferred incumbent"), AuthoredLight)) return false;
    TestFalse(TEXT("Inferred light cannot mask a later authored hook"), F.Lights->IsParticleSelected(Mixed.Component, 1));
    TestTrue(TEXT("The selected authored hook retains its intensity"), FMath::IsNearlyEqual(AuthoredLight->Intensity, 6.4f, .001f));
    TestTrue(TEXT("The selected authored hook retains its color"), AuthoredLight->GetLightColor().Equals(Authored.Color, .01f));
    TestEqual(TEXT("Authored and inferred requests still share one source slot"), F.Lights->GetVisibleLightCount(Mixed.Component), 1);
    F.Lights->SetScriptLight(Mixed.Component, false, Authored);
    F.Submit(Mixed); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("SetLight off restores the source's still-live inferred effect"), F.Lights->IsParticleSelected(Mixed.Component, 1));
    TestNull(TEXT("Disabled authored light no longer owns a pool slot"), F.Lights->FindScriptLight(Mixed.Component));
    F.Lights->RemoveSource(Mixed.Component); F.Lights->RefreshLights(.05f);

    auto DestroyedComponent = F.Source(FVector(50, 0, 0));
    F.Submit(DestroyedComponent); F.Lights->RefreshLights(.05f);
    DestroyedComponent.Component->DestroyComponent(); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Destroyed source components also retire their light"), F.ActualLights().Num(), 0);
    TestTrue(TEXT("Repeated selection, destruction and restart do not grow the pool"), F.Lights->GetAllocatedLightCount() <= 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELocalHeldLightTest, "ACE.Rendering.CrowdEffectLighting.LocalHeldWeapons",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACELocalHeldLightTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FBudgetScope Budget;
    if (!TestTrue(TEXT("Light budgets exist"), Budget.IsValid())) return false;
    FWorld F;
    auto* Pawn = F.World->SpawnActor<APawn>();
    auto* Root = NewObject<USceneComponent>(Pawn);
    Pawn->AddInstanceComponent(Root); Pawn->SetRootComponent(Root); Root->RegisterComponent();
    F.PC->Possess(Pawn); F.PC->SetViewTarget(F.View);
    auto Main = F.Source(FVector(100, -50, 100));
    auto Off = F.Source(FVector(100, 50, 100));
    Main.Owner->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
    Off.Owner->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
    Off.Request.Color = FLinearColor(.1f, .3f, 1.f);
    TArray<FSource> Crowd;
    for (int32 I = 0; I < 32; ++I) Crowd.Add(F.Source(FVector(I, 0, 50)));
    // Seed the pool with closer, incumbent scenery/remote effects before equipping.
    Budget.Set(8); F.SubmitAll(Crowd); F.Lights->RefreshLights(.05f);
    for (int32 Limit : {8, 4, 2})
    {
        Budget.Set(Limit);
        for (float Side : {-1.f, 1.f})
        {
            F.SetView(FVector(0, Side * 70, 80));
            F.SubmitAll(Crowd); F.Submit(Main); F.Submit(Off); F.Lights->RefreshLights(.05f);
            TestTrue(TEXT("Main hand keeps illumination among closer effects"), F.Lights->IsParticleSelected(Main.Component, 1));
            TestTrue(TEXT("Offhand has equal priority among closer effects"), F.Lights->IsParticleSelected(Off.Component, 1));
            TestEqual(TEXT("Held priority stays inside the configured light cap"), F.Lights->GetVisibleLightCount(), Limit);
            TestTrue(TEXT("Both hands still share the crowd energy bound"), F.TotalIntensity() <= 12.001f);
        }
    }
    // An invisible black overlay must neither replace the colored emitter nor
    // consume an overlap share that dims every other source.
    auto Black = Off.Request; Black.EmitterId = 2; Black.Color = FLinearColor::Black;
    Black.Position = F.View->GetActorLocation();
    TArray<FACEEffectLightRequest> Mixed{Off.Request, Black};
    F.Lights->SubmitParticles(Off.Component, Mixed); F.Submit(Main); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("Black overlay cannot take the offhand's light"), F.Lights->IsParticleSelected(Off.Component, 1));
    TestFalse(TEXT("Black overlay receives no scene light"), F.Lights->IsParticleSelected(Off.Component, 2));
    F.Lights->SetScriptLight(Off.Component, true, Black); F.Lights->RefreshLights(.05f);
    TestTrue(TEXT("An invalid black hook cannot suppress live colored particles"), F.Lights->IsParticleSelected(Off.Component, 1));
    // Dropping the item returns it to ordinary proximity selection.
    Off.Owner->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    F.SetView(FVector::ZeroVector); F.SubmitAll(Crowd); F.Submit(Main); F.Submit(Off); F.Lights->RefreshLights(.05f);
    TestFalse(TEXT("Dropped items no longer reserve a local held slot"), F.Lights->IsParticleSelected(Off.Component, 1));
    Budget.Set(0); F.Submit(Main); F.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Disabling effect lights also disables held lights"), F.Lights->GetVisibleLightCount(), 0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECrowdLightWorldIsolationTest, "ACE.Rendering.CrowdEffectLighting.WorldIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECrowdLightWorldIsolationTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FBudgetScope Budget;
    if (!TestTrue(TEXT("Light budgets exist"), Budget.IsValid())) return false;
    Budget.Set(8);
    FWorld A, B;
    if (!TestTrue(TEXT("Concurrent worlds have distinct managers"), A.Lights && B.Lights && A.Lights != B.Lights)) return false;
    TArray<FSource> First, Second;
    for (int32 I = 0; I < 16; ++I)
    {
        First.Add(A.Source(FVector(I * 2, 0, 100)));
        Second.Add(B.Source(FVector(I * 2, 0, 100)));
    }
    A.SubmitAll(First); B.SubmitAll(Second);
    A.Lights->RefreshLights(.05f); B.Lights->RefreshLights(.05f);
    TestEqual(TEXT("First world has its own full budget"), A.ActualLights().Num(), 8);
    TestEqual(TEXT("Second world has its own full budget"), B.ActualLights().Num(), 8);
    for (const auto& Source : First) A.Lights->RemoveSource(Source.Component);
    A.Lights->RefreshLights(.05f);
    TestEqual(TEXT("Stopping the first world hides only its pool"), A.ActualLights().Num(), 0);
    TestEqual(TEXT("The second world's selections remain visible"), B.ActualLights().Num(), 8);
    B.Lights->RefreshLights(.30f);
    TestEqual(TEXT("Expiry advances independently in the second world"), B.ActualLights().Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECrowdLightWorldTickTest, "ACE.Rendering.CrowdEffectLighting.PostActorTick",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECrowdLightWorldTickTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FBudgetScope Budget;
    if (!TestTrue(TEXT("Light budgets exist"), Budget.IsValid())) return false;
    Budget.Set(2);
    FWorld A, B;
    if (!TestTrue(TEXT("Both post-actor callbacks initialize"), A.Lights && B.Lights)) return false;
    auto Left = A.Source(FVector(-400, 0, 100), 10.f, 160.f);
    auto Right = A.Source(FVector(400, 0, 100), 10.f, 160.f);
    A.Submit(Left); A.Submit(Right);
    // Exercise the real world dispatch rather than directly refreshing the manager.
    A.World->Tick(LEVELTICK_All, .05f);
    TestEqual(TEXT("A real world tick dispatches effect-light selection"), A.Lights->GetVisibleLightCount(), 2);
    auto* LeftLight = A.Lights->FindParticleLight(Left.Component, 1);
    auto* RightLight = A.Lights->FindParticleLight(Right.Component, 1);
    if (!TestTrue(TEXT("Real dispatch creates both selected lights"), LeftLight && RightLight)) return false;
    TestTrue(TEXT("Separated effects begin at their full strengths"), A.TotalIntensity() > 19.f);

    // These submissions model the latest poses produced in TG_PostUpdateWork.
    // Broadcast the actual engine delegate to verify its world filtering and its
    // use of those current-frame poses before rendering.
    const FVector Together(100, 20, 100);
    Left.Request.Position = Together; Right.Request.Position = Together;
    A.Submit(Left); A.Submit(Right);
    FWorldDelegates::OnWorldPostActorTick.Broadcast(B.World, LEVELTICK_All, .40f);
    TestTrue(TEXT("Another world's post-actor event cannot apply pending transforms"),
        LeftLight->GetComponentLocation().Equals(FVector(-400, 0, 100), .001f));
    TestTrue(TEXT("Another world's clock cannot expire or renormalize these lights"),
        A.Lights->GetVisibleLightCount() == 2 && A.TotalIntensity() > 19.f);
    FWorldDelegates::OnWorldPostActorTick.Broadcast(A.World, LEVELTICK_All, .05f);
    TestTrue(TEXT("Post-actor selection consumes this frame's latest left pose"), LeftLight->GetComponentLocation().Equals(Together, .001f));
    TestTrue(TEXT("Post-actor selection consumes this frame's latest right pose"), RightLight->GetComponentLocation().Equals(Together, .001f));
    TestTrue(TEXT("Current-frame overlap is normalized before rendering"), A.TotalIntensity() > 0.f && A.TotalIntensity() <= 12.001f);
    TestEqual(TEXT("Foreign world remains without submitted effects"), B.Lights->GetVisibleLightCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECrowdLightCostTest, "ACE.Rendering.CrowdEffectLighting.SelectionCost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECrowdLightCostTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    FBudgetScope Budget;
    if (!TestTrue(TEXT("Light budgets exist"), Budget.IsValid())) return false;
    Budget.Set(8);
    FWorld F;
    if (!TestNotNull(TEXT("Light manager initializes"), F.Lights)) return false;
    TArray<FSource> Sources;
    for (int32 I = 0; I < 128; ++I) Sources.Add(F.Source(FVector((I % 16) * 40, (I / 16) * 40, 100)));
    F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    const int32 AllocatedBefore = F.Lights->GetAllocatedLightCount();
    constexpr int32 Samples = 96;
    const double Start = FPlatformTime::Seconds();
    for (int32 Frame = 0; Frame < Samples; ++Frame)
    {
        F.SetView(FVector((Frame % 16) * 40, 0, 0));
        for (auto& Source : Sources)
        {
            Source.Request.Position.Z = 100.f + (Frame % 3);
            F.Submit(Source);
        }
        F.Lights->RefreshLights(1.f / 15.f);
    }
    const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0 / Samples;
    AddInfo(FString::Printf(TEXT("Crowd light CPU workload: %d sources, %d actual visible lights, %.4f ms/sample over %d submission+selection samples. Excludes GPU rendering and particle simulation."),
        Sources.Num(), F.ActualLights().Num(), Milliseconds, Samples));
    TestEqual(TEXT("Measured moving crowd remains capped"), F.ActualLights().Num(), 8);
    TestEqual(TEXT("Measured steady workload allocates no additional light components"), F.Lights->GetAllocatedLightCount(), AllocatedBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECrowdLightSurfaceTest, "ACE.Rendering.CrowdEffectLighting.SurfaceRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECrowdLightSurfaceTest::RunTest(const FString&)
{
    using namespace ACECrowdLightingTests;
    if (!FApp::CanEverRender()) { AddError(TEXT("SurfaceRender requires an active renderer")); return false; }
    FBudgetScope Budget;
    if (!TestTrue(TEXT("Light budgets exist"), Budget.IsValid())) return false;
    Budget.Set(8);
    FWorld F;
    if (!TestNotNull(TEXT("Light manager initializes"), F.Lights)) return false;
    auto* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    auto* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!TestTrue(TEXT("Neutral receiver assets load"), Plane && Material)) return false;
    auto* ReceiverActor = F.World->SpawnActor<AActor>();
    auto* Receiver = NewObject<UStaticMeshComponent>(ReceiverActor);
    ReceiverActor->AddInstanceComponent(Receiver); ReceiverActor->SetRootComponent(Receiver);
    Receiver->SetMobility(EComponentMobility::Movable); Receiver->SetStaticMesh(Plane); Receiver->SetMaterial(0, Material);
    Receiver->SetCollisionEnabled(ECollisionEnabled::NoCollision); Receiver->SetCastShadow(false);
    Receiver->SetWorldScale3D(FVector(8.f)); Receiver->RegisterComponent();
    // Packaged builds defer the mesh proxy while its PSOs compile. This test
    // captures inside one automation tick, so service completion tasks before
    // measuring light instead of accidentally comparing three empty frames.
    const double ReadyDeadline=FPlatformTime::Seconds()+5.;
    do
    {
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        F.World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
        if (Receiver->GetSceneProxy() && !Receiver->IsPSOPrecaching()) break;
        FPlatformProcess::SleepNoStats(.001f);
    } while (FPlatformTime::Seconds()<ReadyDeadline);
    if (!TestTrue(TEXT("Receiver is ready before measuring illumination"),Receiver->GetSceneProxy()!=nullptr && !Receiver->IsPSOPrecaching())) return false;
    constexpr int32 Size = 128;
    auto* Target = NewObject<UTextureRenderTarget2D>(ReceiverActor);
    Target->InitCustomFormat(Size, Size, PF_FloatRGBA, false);
    auto* Capture = NewObject<USceneCaptureComponent2D>(ReceiverActor);
    Capture->TextureTarget = Target; Capture->FOVAngle = 60.f;
    Capture->bCaptureEveryFrame = false; Capture->bCaptureOnMovement = false;
    Capture->CaptureSource = SCS_SceneColorHDRNoAlpha;
    Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetBloom(false);
    Capture->ShowFlags.SetAtmosphere(false); Capture->ShowFlags.SetFog(false); Capture->ShowFlags.SetAntiAliasing(false);
    Capture->SetWorldLocationAndRotation(FVector(0, 0, 900), FRotator(-90, 0, 0));
    Capture->RegisterComponent();
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/CrowdEffectLighting");
    IFileManager::Get().MakeDirectory(*Directory, true);
    auto Read = [&](const TCHAR* Name)
    {
        if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
        F.World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
        TArray<FLinearColor> Pixels;
        Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
        TArray<FColor> Display;
        Display.Reserve(Pixels.Num());
        for (const auto& Pixel : Pixels) Display.Add(Pixel.ToFColor(true));
        if (Display.Num() == Size * Size)
        {
            TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size, Size, Display, PNG);
            FFileHelper::SaveArrayToFile(PNG, *(Directory / Name));
        }
        return Pixels;
    };
    const auto Dark = Read(TEXT("NoLights.png"));
    TArray<FSource> Sources;
    Sources.Add(F.Source(FVector(0, 0, 150), 12.f, 600.f));
    Sources[0].Request.Color = FLinearColor::White;
    F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    const auto Isolated = Read(TEXT("Isolated.png"));
    for (int32 I = 1; I < 128; ++I)
    {
        auto Source = F.Source(FVector(0, 0, 150), 12.f, 600.f);
        Source.Request.Color = FLinearColor::White; Sources.Add(Source);
    }
    F.SubmitAll(Sources); F.Lights->RefreshLights(.05f);
    const auto Crowd = Read(TEXT("Crowd128.png"));
    if (!TestTrue(TEXT("All receiver captures contain a full frame"), Dark.Num() == Size * Size
        && Isolated.Num() == Dark.Num() && Crowd.Num() == Dark.Num())) return false;
    double SingleEnergy = 0., CrowdEnergy = 0.;
    float MaximumDifference = 0.f;
    for (int32 I = 0; I < Dark.Num(); ++I)
    {
        SingleEnergy += FMath::Max(0.f, Isolated[I].GetLuminance() - Dark[I].GetLuminance());
        CrowdEnergy += FMath::Max(0.f, Crowd[I].GetLuminance() - Dark[I].GetLuminance());
        MaximumDifference = FMath::Max(MaximumDifference, FMath::Abs(Crowd[I].GetLuminance() - Isolated[I].GetLuminance()));
    }
    TestTrue(TEXT("The isolated effect measurably illuminates the receiver"), SingleEnergy > 1.0);
    TestTrue(TEXT("128 overlapping sources do not wash the surface brighter than an isolated light"), CrowdEnergy <= SingleEnergy * 1.10 + .01);
    TestTrue(TEXT("Crowd normalization preserves a visible local light"), CrowdEnergy >= SingleEnergy * .75);
    TestEqual(TEXT("Rendered crowd still submits only eight point lights"), F.ActualLights().Num(), 8);
    AddInfo(FString::Printf(TEXT("Surface comparison: isolated energy %.4f, 128-source energy %.4f, max per-pixel difference %.6f. PNGs: %s"),
        SingleEnergy, CrowdEnergy, MaximumDifference, *Directory));
    return true;
}
#endif
