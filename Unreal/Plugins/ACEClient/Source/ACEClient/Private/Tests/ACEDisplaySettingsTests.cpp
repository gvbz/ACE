#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Blueprint/WidgetTree.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "ShaderCompiler.h"
#include "RenderingThread.h"
#include "HAL/IConsoleManager.h"
#include "ACEDatSubsystem.h"
#include "Dat/ACEDatTextureResolver.h"
#include "ACERuntimeOptions.h"
#include "ACEDisplayView.h"
#include "SceneViewExtension.h"
#include "Framework/Application/SlateApplication.h"
#include "UI/ACEVideoSettingsWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEDisplaySettingsTest,"ACE.Rendering.DisplaySettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEDisplaySettingsTest::RunTest(const FString&)
{
    // Never change the developer's real display preferences during automation.
    const FString PreviousIni=GGameUserSettingsIni;
    GGameUserSettingsIni=FPaths::ProjectSavedDir()/TEXT("Automation/DisplaySettingsPreferences.ini");
    FConfigFile Settings; Settings.NoSave=false; Settings.bCanSaveAllSections=true;
    GConfig->SetFile(GGameUserSettingsIni,&Settings);
    GConfig->EmptySection(TEXT("ACE.Presentation"),GGameUserSettingsIni);
    ON_SCOPE_EXIT { GGameUserSettingsIni=PreviousIni; ACERuntimeOptions::Apply(); };

    TestEqual(TEXT("New installs use neutral brightness"),ACERuntimeOptions::Get(TEXT("ScreenBrightness")),1.f);
    TestEqual(TEXT("New installs use neutral contrast"),ACERuntimeOptions::Get(TEXT("Contrast")),1.f);
    for(const auto& Pair:TArray<FVector2D>{{1.6f,.2f},{2.2f,1.f},{2.8f,2.f}})
    {
        GConfig->SetFloat(TEXT("ACE.Presentation"),TEXT("Brightness"),Pair.X,GGameUserSettingsIni);
        TestTrue(TEXT("Legacy gamma maps to brightness with a neutral midpoint"),
            FMath::IsNearlyEqual(ACERuntimeOptions::Get(TEXT("ScreenBrightness")),float(Pair.Y)));
    }
    ACERuntimeOptions::Set(TEXT("ScreenBrightness"),1.f);
    ACERuntimeOptions::Set(TEXT("Contrast"),1.f);
    ACERuntimeOptions::Apply();

    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
    auto* GI=NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); Context.OwningGameInstance=GI;
    GI->OnWorldChanged(nullptr,World); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

    auto* Options=CreateWidget<UACEVideoSettingsWidget>(GI); Options->TakeWidget();
    {
        auto* Missile=Cast<UCheckBox>(Options->WidgetTree->FindWidget(TEXT("StationaryMissileAttacks")));
        if(!TestNotNull(TEXT("Input Options offers stationary missile attacks"),Missile))return false;
        TestFalse(TEXT("Existing profiles keep their missile behavior"),Missile->IsChecked());
        Missile->SetIsChecked(true);
        TestTrue(TEXT("Missile preference waits for Apply"),Options->HasPendingChanges() && !ACERuntimeOptions::StationaryMissileAttacks());
        Options->ResetVideo();TestFalse(TEXT("Reset discards missile edit"),Missile->IsChecked());
        Missile->SetIsChecked(true);Options->ApplyRuntimeOptions();
        TestTrue(TEXT("Stationary missiles can be enabled independently"),ACERuntimeOptions::StationaryMissileAttacks() && !ACERuntimeOptions::ClassicMovement());
        auto* Movement=Cast<UCheckBox>(Options->WidgetTree->FindWidget(TEXT("ClassicMovement")));
        if(!TestNotNull(TEXT("Classic movement appears in Input Options"),Movement))return false;
        TestFalse(TEXT("Existing profiles default to free casting movement"),Movement->IsChecked());
        Movement->SetIsChecked(true);
        TestTrue(TEXT("Classic movement enables Apply"),Options->HasPendingChanges());
        TestFalse(TEXT("Editing the checkbox leaves active movement alone"),ACERuntimeOptions::ClassicMovement());
        Options->ResetVideo();TestFalse(TEXT("Reset discards the edit"),Movement->IsChecked());
        Movement->SetIsChecked(true);Options->ApplyRuntimeOptions();
        FConfigFile Saved;Saved.Read(GGameUserSettingsIni);float Setting=0;
        TestTrue(TEXT("Apply persists classic movement"),Saved.GetFloat(TEXT("ACE.Presentation"),TEXT("ClassicMovement"),Setting)&&Setting==1.f);
        auto* Reopened=CreateWidget<UACEVideoSettingsWidget>(GI);Reopened->TakeWidget();
        TestTrue(TEXT("Reopening restores classic movement"),CastChecked<UCheckBox>(Reopened->WidgetTree->FindWidget(TEXT("ClassicMovement")))->IsChecked());
        TestTrue(TEXT("Reopening restores stationary missiles"),CastChecked<UCheckBox>(Reopened->WidgetTree->FindWidget(TEXT("StationaryMissileAttacks")))->IsChecked());
        Options->DefaultsVideo();
        TestTrue(TEXT("Defaults stages free movement until applied"),!Movement->IsChecked()&&ACERuntimeOptions::ClassicMovement());
        Options->ApplyRuntimeOptions();
        TestFalse(TEXT("Applying Defaults restores free movement"),ACERuntimeOptions::ClassicMovement());
        TestFalse(TEXT("Applying Defaults restores missile behavior"),ACERuntimeOptions::StationaryMissileAttacks());
        Options->ResetVideo();
    }
    {
        auto* Audio=Cast<UCheckBox>(Options->WidgetTree->FindWidget(TEXT("BackgroundAudio")));
        if(!TestNotNull(TEXT("Background audio toggle exists in Sound Options"),Audio))return false;
        TestFalse(TEXT("New installs retain the previous background-mute default"),Audio->IsChecked());
        Audio->SetIsChecked(true);Audio->OnCheckStateChanged.Broadcast(true);
        TestTrue(TEXT("Background audio enables Apply"),Options->HasPendingChanges());
        TestEqual(TEXT("Audio checkbox does not save before Apply"),ACERuntimeOptions::Get(TEXT("ActiveSoundOnly")),1.f);
        Options->ResetVideo();TestFalse(TEXT("Reset discards the audio draft"),Audio->IsChecked());
        Audio->SetIsChecked(true);Options->ApplyRuntimeOptions();
        TestEqual(TEXT("Apply removes the engine-level background mute"),FApp::GetUnfocusedVolumeMultiplier(),1.f);
        FConfigFile SavedAudio;SavedAudio.Read(GGameUserSettingsIni);float LegacyMute=1;
        TestTrue(TEXT("Background audio persists using the existing inverse preference"),
            SavedAudio.GetFloat(TEXT("ACE.Presentation"),TEXT("ActiveSoundOnly"),LegacyMute)&&LegacyMute==0.f);
        auto* ReopenedAudio=CreateWidget<UACEVideoSettingsWidget>(GI);ReopenedAudio->TakeWidget();
        TestTrue(TEXT("Reopening preserves background audio / legacy opt-out of active-only"),
            Cast<UCheckBox>(ReopenedAudio->WidgetTree->FindWidget(TEXT("BackgroundAudio")))->IsChecked());
        Options->DefaultsVideo();TestFalse(TEXT("Defaults stages background mute"),Audio->IsChecked());
        TestEqual(TEXT("Defaults does not mute audio before Apply"),FApp::GetUnfocusedVolumeMultiplier(),1.f);
        Options->ResetVideo();TestTrue(TEXT("Reset restores the saved background preference"),Audio->IsChecked());
        if(FSlateApplication::IsInitialized())
        {
            auto& App=FSlateApplication::Get();const bool WasActive=App.IsActive();
            ON_SCOPE_EXIT {App.ProcessApplicationActivationEvent(WasActive);ACERuntimeOptions::Apply();};
            App.ProcessApplicationActivationEvent(false);ACERuntimeOptions::Apply();
            TestFalse(TEXT("Audio regression runs with the application inactive"),App.IsActive());
            TestEqual(TEXT("Background effects and plugin alerts remain audible"),ACERuntimeOptions::SoundGain(false),1.f);
            TestEqual(TEXT("Background ambient sounds remain audible"),ACERuntimeOptions::SoundGain(true),1.f);
            Audio->SetIsChecked(false);Options->ApplyRuntimeOptions();
            TestEqual(TEXT("Disabling background audio restores engine mute"),FApp::GetUnfocusedVolumeMultiplier(),0.f);
            TestEqual(TEXT("Inactive emitter/alert gain follows the disabled toggle"),ACERuntimeOptions::SoundGain(false),0.f);
            App.ProcessApplicationActivationEvent(true);ACERuntimeOptions::Apply();
            TestEqual(TEXT("Foreground sounds resume with background audio disabled"),ACERuntimeOptions::SoundGain(false),1.f);
        }
    }
    // Send the same change notifications as a click or drag. A control that
    // writes in its change handler would bypass both Apply and Reset.
    const TCHAR* TerrainKeys[] = {TEXT("PersonalFarTerrain"),TEXT("PersonalFarScenery"),TEXT("PersonalFarSceneryFog"),
        TEXT("PersonalFarWindows"),TEXT("PersonalFarTerrainTextures"),TEXT("PersonalSkyHorizon")};
    TArray<UCheckBox*> TerrainChecks;
    for (const TCHAR* Key : TerrainKeys)
    {
        auto* Check = Cast<UCheckBox>(Options->WidgetTree->FindWidget(Key));
        if (!TestNotNull(Key,Check)) return false;
        Check->SetIsChecked(true);
        Check->OnCheckStateChanged.Broadcast(true);
        TestTrue(FString::Printf(TEXT("%s enables Apply"),Key),Options->HasPendingChanges());
        TestEqual(FString::Printf(TEXT("%s is unchanged until Apply"),Key),ACERuntimeOptions::Get(Key),0.f);
        Options->ResetVideo();
        TestFalse(FString::Printf(TEXT("Reset discards %s"),Key),Check->IsChecked());
        TerrainChecks.Add(Check);
    }
    auto* Radius = Cast<USlider>(Options->WidgetTree->FindWidget(TEXT("PersonalFarTerrainRadius")));
    auto* ObjectRadius = Cast<USlider>(Options->WidgetTree->FindWidget(TEXT("PersonalFarSceneryRadius")));
    if (!TestNotNull(TEXT("Terrain radius slider"),Radius) || !TestNotNull(TEXT("Scenery radius slider"),ObjectRadius)) return false;
    for (auto* Check : TerrainChecks) { Check->SetIsChecked(true); Check->OnCheckStateChanged.Broadcast(true); }
    Radius->SetValue(80); Radius->OnValueChanged.Broadcast(80);
    ObjectRadius->SetValue(18); ObjectRadius->OnValueChanged.Broadcast(18);
    TestEqual(TEXT("Dragging terrain radius only edits the draft"),ACERuntimeOptions::Get(TEXT("PersonalFarTerrainRadius")),43.f);
    TestEqual(TEXT("Dragging object radius only edits the draft"),ACERuntimeOptions::Get(TEXT("PersonalFarSceneryRadius")),6.f);
    Options->ApplyRuntimeOptions();
    FConfigFile TerrainSaved;
    TerrainSaved.Read(GGameUserSettingsIni);
    for (const TCHAR* Key : TerrainKeys)
    {
        float Value = 0;
        TestTrue(FString::Printf(TEXT("Apply persists %s"),Key),TerrainSaved.GetFloat(TEXT("ACE.Presentation"),Key,Value) && Value == 1.f);
    }
    Options->DefaultsVideo();
    for (auto* Check : TerrainChecks) TestFalse(TEXT("Defaults stages a cleared terrain checkbox"),Check->IsChecked());
    TestEqual(TEXT("Defaults stages the default terrain radius"),Radius->GetValue(),43.f);
    TestEqual(TEXT("Defaults stages the default object radius"),ObjectRadius->GetValue(),6.f);
    Options->ResetVideo();
    for (auto* Check : TerrainChecks) TestTrue(TEXT("Reset restores the applied terrain checkbox"),Check->IsChecked());
    TestEqual(TEXT("Reset restores the applied terrain radius"),Radius->GetValue(),80.f);
    TestEqual(TEXT("Reset restores the applied object radius"),ObjectRadius->GetValue(),18.f);
    auto* Brightness=Cast<USlider>(Options->WidgetTree->FindWidget(TEXT("ScreenBrightness")));
    auto* Contrast=Cast<USlider>(Options->WidgetTree->FindWidget(TEXT("Contrast")));
    if(!TestNotNull(TEXT("Brightness control exists"),Brightness)||!TestNotNull(TEXT("Contrast control exists"),Contrast)) return false;
    Brightness->SetValue(.35f); Brightness->OnValueChanged.Broadcast(.35f);
    Contrast->SetValue(1.4f); Contrast->OnValueChanged.Broadcast(1.4f);
    TestEqual(TEXT("Brightness slider updates the renderer immediately"),
        IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Display.Brightness"))->GetFloat(),.35f);
    TestEqual(TEXT("Contrast slider updates the renderer immediately"),
        IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Display.Contrast"))->GetFloat(),1.4f);
    FConfigFile Saved; Saved.Read(GGameUserSettingsIni);
    float SavedBrightness=0,SavedContrast=0;
    Saved.GetFloat(TEXT("ACE.Presentation"),TEXT("ScreenBrightness"),SavedBrightness);
    Saved.GetFloat(TEXT("ACE.Presentation"),TEXT("Contrast"),SavedContrast);
    TestEqual(TEXT("Brightness is written to disk"),SavedBrightness,.35f);
    TestEqual(TEXT("Contrast is written to disk"),SavedContrast,1.4f);
    auto* Reopened=CreateWidget<UACEVideoSettingsWidget>(GI); Reopened->TakeWidget();
    TestEqual(TEXT("Reopening options restores saved brightness"),Cast<USlider>(Reopened->WidgetTree->FindWidget(TEXT("ScreenBrightness")))->GetValue(),.35f);
    TestEqual(TEXT("Reopening options restores saved contrast"),Cast<USlider>(Reopened->WidgetTree->FindWidget(TEXT("Contrast")))->GetValue(),1.4f);
    Options->DefaultsVideo();
    TestEqual(TEXT("Defaults restores neutral brightness"),ACERuntimeOptions::Get(TEXT("ScreenBrightness")),1.f);
    TestEqual(TEXT("Defaults restores neutral contrast"),ACERuntimeOptions::Get(TEXT("Contrast")),1.f);
    ACERuntimeOptions::Set(TEXT("Contrast"),100.f);
    TestEqual(TEXT("Contrast is bounded"),ACERuntimeOptions::Get(TEXT("Contrast")),2.f);
    ACERuntimeOptions::Set(TEXT("Contrast"),1.f); ACERuntimeOptions::Apply();

    if(!FApp::CanEverRender()) { AddError(TEXT("Display settings regression requires RenderOffscreen")); return false; }
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    auto* Owner=World->SpawnActor<AActor>();
    auto* Capture=NewObject<USceneCaptureComponent2D>(Owner); Capture->RegisterComponent();
    auto* Target=NewObject<UTextureRenderTarget2D>(Owner);
    Target->InitCustomFormat(64,64,PF_FloatRGBA,true); Target->UpdateResourceImmediate(true);
    Capture->TextureTarget=Target; Capture->FOVAngle=60;
    Capture->CaptureSource=SCS_FinalColorLDR;
    Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false;
    Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
    Capture->ShowFlags.SetAtmosphere(false); Capture->ShowFlags.SetFog(false);
    Capture->ShowFlags.SetBloom(false); Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetAntiAliasing(false);
    Capture->ShowFlags.SetSeparateTranslucency(true);
    auto Read=[&]() {
        if(GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
        World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
        TArray<FFloat16Color> Pixels; Target->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels);
        return Pixels.Num()==64*64 ? FLinearColor(Pixels[32*64+32]) : FLinearColor(-1,-1,-1,0);
    };
    auto* Card=NewObject<UProceduralMeshComponent>(Owner); Card->RegisterComponent();
    Card->CreateMeshSection(0,{{100,-60,-60},{100,60,-60},{100,60,60},{100,-60,60}},
        {0,2,1,0,3,2,0,1,2,0,2,3},{},{{0,1},{1,1},{1,0},{0,0}}, {},{},false);
    Capture->ShowOnlyComponent(Card);
    auto* Surface=UMaterialInstanceDynamic::Create(Dat->EnsureAceUnlitTexturedMaterialBase(),Owner);
    Surface->SetScalarParameterValue(TEXT("EmissiveStrength"),1.f);
    Surface->SetScalarParameterValue(TEXT("FogAmount"),0.f);
    Card->SetMaterial(0,Surface);
    Surface->SetTextureParameterValue(TEXT("ACETexture"),FACEDatTextureResolver::CreateTransientRgbaUi(1,1,{FColor::White}));
    const FLinearColor Before=Read();
    ACERuntimeOptions::Set(TEXT("ScreenBrightness"),.2f); ACERuntimeOptions::Apply();
    TestTrue(TEXT("Offscreen inventory/character previews are not graded twice"),Read().Equals(Before,.005f));
    // Exercise the exact viewport callback using an explicitly opted-in final
    // color capture. Raw HDR buffers and ordinary UI preview captures stay untouched.
    auto DisplayView=CreateACEDisplayView(GI,true);
    const FLinearColor Dim=Read();
    TestTrue(FString::Printf(TEXT("Minimum brightness dims white highlights: %.3f -> %.3f"),Before.R,Dim.R),
        Before.R>.5f && FMath::Abs(Dim.R-Before.R*.2f)<.01f);

    auto* Glow=NewObject<UProceduralMeshComponent>(Owner); Glow->RegisterComponent();
    Glow->SetProcMeshSection(0,*Card->GetProcMeshSection(0)); Glow->SetWorldLocation(FVector(-10,0,0));
    // Initialize the actual retail particle extension without needing a DAT entry.
    Dat->GetOrCreateParticleMaterial(0);
    auto* GlowMaterial=UMaterialInstanceDynamic::Create(Dat->EnsureAceParticleAdditiveMaterialBase(),Owner);
    GlowMaterial->SetTextureParameterValue(TEXT("ACETexture"),FACEDatTextureResolver::CreateTransientRgbaUi(1,1,{FColor(32,48,64)}));
    GlowMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"),1.f); Glow->SetMaterial(0,GlowMaterial);
    for(bool WithGlow:{false,true}) for(uint8 Grey:{uint8(32),uint8(96),uint8(160),uint8(224)})
    {
        Capture->ClearShowOnlyComponents(); Capture->ShowOnlyComponent(Card);
        if(WithGlow) Capture->ShowOnlyComponent(Glow);
        Surface->SetTextureParameterValue(TEXT("ACETexture"),FACEDatTextureResolver::CreateTransientRgbaUi(1,1,{FColor(Grey,Grey,Grey)}));
        ACERuntimeOptions::Set(TEXT("ScreenBrightness"),1.f); ACERuntimeOptions::Set(TEXT("Contrast"),1.f); ACERuntimeOptions::Apply();
        const auto Neutral=Read();
        for(float Gain:{.2f,1.f,2.f}) for(float Amount:{.5f,1.f,2.f})
        {
            ACERuntimeOptions::Set(TEXT("ScreenBrightness"),Gain); ACERuntimeOptions::Set(TEXT("Contrast"),Amount); ACERuntimeOptions::Apply();
            const auto Actual=Read();
            const auto ExpectedChannel=[&](float V) {return FMath::Clamp(FMath::Clamp((V-.5f)*Amount+.5f,0.f,1.f)*Gain,0.f,1.f);};
            const FLinearColor Expected(ExpectedChannel(Neutral.R),ExpectedChannel(Neutral.G),ExpectedChannel(Neutral.B));
            TestTrue(FString::Printf(TEXT("Rendered grey %u glow %d brightness %.1f contrast %.1f: %s vs %s"),
                Grey,WithGlow,Gain,Amount,*Actual.ToString(),*Expected.ToString()),
                FVector(Actual.R-Expected.R,Actual.G-Expected.G,Actual.B-Expected.B).GetAbsMax()<.015f);
        }
    }
    return true;
}
#endif
