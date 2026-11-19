#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ACEDatSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Components/SceneCaptureComponent2D.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"

// This diagnostic records the actual world-material shader before tone mapping.
// AC1's fixed-function device disables sRGB texture sampling and framebuffer
// conversion, so its fog lerp operates on display RGB. HDR readback lets us
// distinguish that from a linear-light lerp without lighting/exposure ambiguity.
// Add -RequireRetailFogColor to make the display-RGB comparison a strict check;
// the ordinary probe reports the discrepancy without blessing either baseline.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEFogColorProbe, "ACE.RetailParity.FogColorProbe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEFogColorProbe::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender())
    {
        AddWarning(TEXT("FogColorProbe needs a rendering RHI; no pixel comparison was performed."));
        return true;
    }
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .RequiresHitProxies(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
    auto* GI = NewObject<UGameInstance>(GEngine);
    GI->InitializeStandalone();
    auto* InitialWorld = GI->GetWorld();
    GI->GetWorldContext()->SetCurrentWorld(World);
    World->SetGameInstance(GI);
    InitialWorld->DestroyWorld(false);
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    if (World->GetFeatureLevel() <= ERHIFeatureLevel::ES3_1)
    {
        AddWarning(TEXT("FogColorProbe currently isolates desktop HDR scene color; mobile output was not compared."));
        return true;
    }
    // The float destination does not control the intermediate scene color.
    // Effects quality can select scene color format 3 (11/11/10-bit RGB),
    // whose unequal precision cannot support the neutral-color comparison.
    auto* SceneColorFormat = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SceneColorFormat"));
    if (!TestNotNull(TEXT("HDR scene color precision setting exists"), SceneColorFormat)) return false;
    const int32 PreviousSceneColorFormat = SceneColorFormat->GetInt();
    SceneColorFormat->SetWithCurrentPriority(4);
    ON_SCOPE_EXIT { SceneColorFormat->SetWithCurrentPriority(PreviousSceneColorFormat); };
    TestEqual(TEXT("Fog probe uses equal 16-bit scene color channels"), SceneColorFormat->GetInt(), 4);
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    auto* Owner = World->SpawnActor<AActor>();
    auto* Parent = Dat->EnsureAceUnlitTexturedMaterialBase();
    if (!TestNotNull(TEXT("World fog material parent exists"), Parent)) return false;
    auto* Material = UMaterialInstanceDynamic::Create(Parent, Owner);
    if (!TestNotNull(TEXT("World fog material exists"), Material)) return false;
    auto* White = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
    *static_cast<FColor*>(White->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE)) = FColor::White;
    White->GetPlatformData()->Mips[0].BulkData.Unlock();
    White->SRGB = true;
    White->UpdateResource();
    Material->SetTextureParameterValue(TEXT("ACETexture"), White);
    Material->SetScalarParameterValue(TEXT("FogAmount"), 1.f);

    auto* Plane = NewObject<UProceduralMeshComponent>(Owner);
    Plane->RegisterComponent();
    Plane->CreateMeshSection_LinearColor(0,
        {FVector(0,-10000,-10000), FVector(0,10000,-10000), FVector(0,10000,10000), FVector(0,-10000,10000)},
        {0,2,1,0,3,2,0,1,2,0,2,3}, {}, {}, {}, {}, false);
    Plane->SetMaterial(0, Material);
    auto* Target = NewObject<UTextureRenderTarget2D>(Owner);
    // Odd dimensions put the sampled pixel exactly on the central camera ray.
    constexpr int32 Size = 9;
    Target->InitCustomFormat(Size, Size, PF_FloatRGBA, false);
    auto* Capture = NewObject<USceneCaptureComponent2D>(Owner);
    Capture->TextureTarget = Target;
    Capture->FOVAngle = 90.f;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->CaptureSource = SCS_SceneColorHDRNoAlpha;
    Capture->ShowFlags.SetEyeAdaptation(false);
    Capture->ShowFlags.SetBloom(false);
    Capture->ShowFlags.SetAtmosphere(false);
    Capture->ShowFlags.SetAntiAliasing(false);
    Capture->ShowFlags.SetLighting(false);
    Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowOnlyComponents.Add(Plane);
    Capture->RegisterComponent();
    if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
    auto ReadCenter = [&]()
    {
        World->SendAllEndOfFrameUpdates();
        Capture->CaptureScene();
        FlushRenderingCommands();
        TArray<FLinearColor> Pixels;
        // Preserve raw HDR values instead of normalizing each channel against
        // image extrema, which would alter the transfer function being measured.
        Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax));
        return Pixels.IsValidIndex((Size / 2) * Size + Size / 2)
            ? Pixels[(Size / 2) * Size + Size / 2] : FLinearColor(-1.f, -1.f, -1.f, 1.f);
    };
    auto ToDisplay = [](float Linear)
    {
        return Linear <= .0031308f ? 12.92f * Linear : 1.055f * FMath::Pow(Linear, 1.f / 2.4f) - .055f;
    };
    const bool RequireRetail = FParse::Param(FCommandLine::Get(), TEXT("RequireRetailFogColor"));
    FString CSV = TEXT("surface_display,fog_display,fog_fraction,captured_linear,captured_display,retail_display,display_error,captured_green,captured_blue\n");
    float MaxDisplayError = 0.f;
    for (float Surface : {0.f, 1.f})
    {
        const float Fog = 1.f - Surface;
        Material->SetScalarParameterValue(TEXT("EmissiveStrength"), Surface);
        Dat->SetWorldDistanceFog(1000.f, 2000.f, FLinearColor(Fog, Fog, Fog, 1.f), 1.f);
        float Previous = Surface;
        for (float Fraction : {0.f, .25f, .5f, .75f, 1.f})
        {
            Plane->SetWorldLocation(FVector(1000.f + 1000.f * Fraction, 0, 0));
            const FLinearColor Pixel = ReadCenter();
            const float Display = ToDisplay(Pixel.R);
            const float Retail = FMath::Lerp(Surface, Fog, Fraction);
            const float Error = FMath::Abs(Display - Retail);
            MaxDisplayError = FMath::Max(MaxDisplayError, Error);
            const FString Label = FString::Printf(TEXT("surface %.0f, fog %.0f, fraction %.2f"), Surface, Fog, Fraction);
            TestTrue(*FString::Printf(TEXT("Neutral bounded readback: %s"), *Label),
                Pixel.R >= -.002f && Pixel.R <= 1.002f && FMath::Abs(Pixel.G - Pixel.R) < .002f && FMath::Abs(Pixel.B - Pixel.R) < .002f);
            TestTrue(*FString::Printf(TEXT("Monotonic fog transition: %s"), *Label),
                Surface == 0.f ? Pixel.R >= Previous - .002f : Pixel.R <= Previous + .002f);
            if (Fraction == 0.f || Fraction == 1.f || RequireRetail)
                TestTrue(*FString::Printf(TEXT("Display RGB reference: %s"), *Label), Error < .008f);
            Previous = Pixel.R;
            AddInfo(FString::Printf(TEXT("FogColorProbe %s: captured linear %.6f, encoded display %.6f, retail display %.6f, error %.6f, RGB=(%.6f,%.6f,%.6f)"),
                *Label, Pixel.R, Display, Retail, Error, Pixel.R, Pixel.G, Pixel.B));
            CSV += FString::Printf(TEXT("%.0f,%.0f,%.2f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
                Surface, Fog, Fraction, Pixel.R, Display, Retail, Error, Pixel.G, Pixel.B);
        }
    }
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/RetailParity");
    IFileManager::Get().MakeDirectory(*Directory, true);
    TestTrue(TEXT("Save measured fog color samples"), FFileHelper::SaveStringToFile(CSV, *(Directory / TEXT("FogColorProbe.csv"))));
    AddInfo(FString::Printf(TEXT("FogColorProbe maximum display RGB deviation from retail: %.6f. This isolates the material blend; it does not measure the final game tone mapper or AC1 vertex interpolation."), MaxDisplayError));
    return true;
}
#endif
