#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "ACERuntimeOptions.h"
#include "ACEDatSubsystem.h"
#include "ACEEffectLightSubsystem.h"
#include "ACEEnvCellActor.h"
#include "ACESkyDomeActor.h"
#include "UI/ACEVideoSettingsWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "ProceduralMeshComponent.h"
#include "Scalability.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACELandTextureMipProvider.h"
#include "Engine/Texture2D.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "Engine/TextureLODSettings.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELowEndVideoSettingsTest,"ACE.Rendering.LowEndVideoSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACELowEndVideoSettingsTest::RunTest(const FString&)
{
    auto* Settings=UGameUserSettings::GetGameUserSettings();
    if(!TestNotNull(TEXT("Game settings available"),Settings)) return false;
    const auto PreviousQuality=Settings->ScalabilityQuality;
    const auto PreviousAppliedQuality=Scalability::GetQualityLevels();
    const FString PreviousIni=GGameUserSettingsIni;
    // Other rendering fixtures override budgets with SetByCode. Exercise the
    // user preference at its real priority, independently of test order.
    auto* EffectBudget=IConsoleManager::Get().FindConsoleVariable(TEXT("ace.FXLightBudget"));
    if(!TestNotNull(TEXT("Desktop effect budget exists"),EffectBudget))return false;
    const int32 PreviousBudget=EffectBudget->GetInt();
    const auto PreviousBudgetPriority=static_cast<EConsoleVariableFlags>(EffectBudget->GetFlags()&ECVF_SetByMask);
    EffectBudget->ClearFlags(ECVF_SetByMask);
    GGameUserSettingsIni=FPaths::ProjectSavedDir()/TEXT("Automation/LowEndVideoPreferences.ini");
    FConfigFile Config; Config.NoSave=false; Config.bCanSaveAllSections=true;
    GConfig->SetFile(GGameUserSettingsIni,&Config);
    GConfig->EmptySection(TEXT("ACE.Presentation"),GGameUserSettingsIni);
    ON_SCOPE_EXIT {
        Settings->ScalabilityQuality=PreviousQuality;
        Scalability::SetQualityLevels(PreviousAppliedQuality);
        IConsoleManager::Get().CallAllConsoleVariableSinks();
        GGameUserSettingsIni=PreviousIni; ACERuntimeOptions::Apply();
        EffectBudget->ClearFlags(ECVF_SetByMask);EffectBudget->Set(PreviousBudget,PreviousBudgetPriority);
    };
    Settings->SetOverallScalabilityLevel(3);
    ACERuntimeOptions::Apply();
    TestEqual(TEXT("Existing installs retain full effect lighting by default"),ACERuntimeOptions::DynamicLightingQuality(),2);

    const auto Init=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Init);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
    auto* GI=NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); Context.OwningGameInstance=GI;
    GI->OnWorldChanged(nullptr,World); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call"); FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if(!TestTrue(TEXT("Real doorway DAT loads"),Dat->LoadDatDirectory(DatDirectory))) return false;
    Dat->GetOrBuildEnvCellMesh(0x7D64010C,100);
    auto* Room=World->SpawnActor<AACEEnvCellActor>();
    if(!TestTrue(TEXT("Real entrance room loads"),Room->LoadEnvCell(0x7D64010C,FVector::ZeroVector,100))) return false;
    auto DoorLights=[&]() {
        TArray<UPointLightComponent*> Lights;
        for(TObjectIterator<UPointLightComponent> It;It;++It)
            if(IsValid(*It) && It->IsRegistered() && It->GetWorld()==World && It->GetOwner()
                && It->GetOwner()->ActorHasTag(TEXT("ACEDoorwaySpill"))) Lights.Add(*It);
        return Lights;
    };
    if(!TestTrue(TEXT("Doorway has actual scene lights"),!DoorLights().IsEmpty())) return false;

    auto* Camera=World->SpawnActor<ACameraActor>();
    auto* PC=World->SpawnActor<APlayerController>(); World->AddController(PC);
    if(!PC->PlayerCameraManager) { PC->PlayerCameraManager=World->SpawnActor<APlayerCameraManager>(); PC->PlayerCameraManager->InitializeFor(PC); }
    PC->SetViewTarget(Camera); PC->PlayerCameraManager->UpdateCamera(0.f);
    auto* Lights=World->GetSubsystem<UACEEffectLightSubsystem>();
    TArray<USceneComponent*> Sources;
    for(int32 I=0;I<10;++I)
    {
        auto* Owner=World->SpawnActor<AActor>(); auto* Source=NewObject<USceneComponent>(Owner);
        Owner->SetRootComponent(Source); Source->RegisterComponent(); Owner->SetActorLocation(FVector(100,I*20,100));
        Sources.Add(Source);
    }
    auto Submit=[&]() {
        for(auto* Source:Sources) {
            FACEEffectLightRequest Request; Request.EmitterId=1; Request.Position=Source->GetComponentLocation();
            Lights->SubmitParticles(Source,MakeArrayView(&Request,1));
        }
        Lights->RefreshLights(.05f);
    };
    auto* Sky=World->SpawnActor<AACESkyDomeActor>();
    FACEDatSkyTimeOfDay Key; Key.DirBright=.8f; Key.DirPitch=55; Key.DirHeading=90;
    Key.DirColor=Key.AmbColor=0xFFFFFFFF; Key.AmbBright=.35f;
    Dat->SetIndoorEnvironment(true); Dat->SetBuildingInteriorFill(false);
    Sky->UpdateWorldLighting(Key,Key,0,.5f);
    auto* SkyComponent=Sky->AmbientSkyLightActor ? Sky->AmbientSkyLightActor->GetLightComponent() : nullptr;
    if (!TestNotNull(TEXT("Scene skylight initialized"),SkyComponent)) return false;
    TestEqual(TEXT("Sealed dungeon receives no outdoor skylight"),SkyComponent->Intensity,0.f);
    Dat->SetIndoorEnvironment(false); Sky->UpdateWorldLighting(Key,Key,0,.5f);
    TestTrue(TEXT("Leaving the dungeon restores outdoor skylight"),SkyComponent->Intensity>0.f);
    auto* Options=CreateWidget<UACEVideoSettingsWidget>(GI); Options->TakeWidget();
    auto* Shadow=Cast<UComboBoxString>(Options->WidgetTree->FindWidget(TEXT("ShadowQuality")));
    auto* Dynamic=Cast<UComboBoxString>(Options->WidgetTree->FindWidget(TEXT("DynamicLightingQuality")));
    if(!TestNotNull(TEXT("Shadow dropdown"),Shadow)||!TestNotNull(TEXT("Dynamic lighting dropdown"),Dynamic)) return false;
    TestEqual(TEXT("Shadows offer an explicit Off option"),Shadow->GetOptionAtIndex(0),FString(TEXT("Off")));
    TestEqual(TEXT("Effect lighting defaults to Full"),Dynamic->GetSelectedOption(),FString(TEXT("Full")));
    auto Apply=[&]() {
        // Same rendering preference path as ApplyVideo, without changing the test host's display.
        Options->ApplyRenderingOptions(); Scalability::SetQualityLevels(Settings->ScalabilityQuality);
        IConsoleManager::Get().CallAllConsoleVariableSinks();
        // SaveSettings redirects editor scalability to EditorSettings.ini and
        // reloads the global GameUserSettings path for window positions. Save
        // rendering state directly into this isolated fixture instead.
        Scalability::SaveState(GGameUserSettingsIni); Options->ApplyRuntimeOptions();
        Sky->UpdateWorldLighting(Key,Key,0,.5f); Submit();
        Options->ResetVideo();
    };
    auto* Detail=Cast<UComboBoxString>(Options->WidgetTree->FindWidget(TEXT("TextureQuality")));
    auto* Preset=Cast<UComboBoxString>(Options->WidgetTree->FindWidget(TEXT("GraphicsQuality")));
    if(!TestNotNull(TEXT("World texture detail selector"),Detail))return false;
    TArray<FColor> Pixels;Pixels.Init(FColor(65,123,200,180),128*64);
    TStrongObjectPtr<UTexture2D> Object(FACEDatTextureResolver::CreateTransientRgbaWithMips(128,64,Pixels,true));
    TStrongObjectPtr<UTexture2D> Effect(FACEDatTextureResolver::CreateTransientRgbaWithMips(128,64,Pixels,true,TA_Wrap,TA_Wrap,false,false,4,TF_Bilinear));
    TStrongObjectPtr<UTexture2D> Interface(FACEDatTextureResolver::CreateTransientRgbaUi(128,64,Pixels));
    auto Blend=MakeShared<FACETerrainBlend,ESPMode::ThreadSafe>();Blend->Width=128;Blend->Height=64;Blend->Pixels=Pixels;
    TestTrue(TEXT("Terrain source mips prepared"),Blend->PrepareUploadMips());
    TStrongObjectPtr<UTexture2D> Terrain(UACELandTextureMipProvider::CreateTexture(Dat,Blend));
    const auto* TextureSettings=UDeviceProfileManager::Get().GetActiveProfile()->GetTextureLODSettings();
    for(auto* Texture:{Object.Get(),Terrain.Get()})
    {
        const auto Filter=TextureSettings->GetSamplerFilter(Texture);
        TestTrue(TEXT("World and terrain textures honor anisotropic filtering at shallow view angles"),
            Filter==ETextureSamplerFilter::AnisotropicPoint || Filter==ETextureSamplerFilter::AnisotropicLinear);
    }
    auto GPUSize=[](UTexture2D* Texture) {return Texture->GetResource()->TextureRHI->GetSizeXY();};
    for(int32 Quality:{0,1,2,3,4,0,2})
    {
        Detail->SetSelectedIndex(Quality);Apply();FlushRenderingCommands();
        const int32 Divisor=Quality==0?4:Quality==1?2:1;
        for(auto* Texture:{Object.Get(),Effect.Get(),Terrain.Get()})
        {
            TestEqual(TEXT("Actual GPU texture dimensions obey detail without replacing the texture"),GPUSize(Texture),FIntPoint(128/Divisor,64/Divisor));
            TestEqual(TEXT("Full source remains available to restore quality"),Texture->GetSizeX(),128);
        }
        TestEqual(TEXT("Inventory, fonts and UI are not degraded"),GPUSize(Interface.Get()),FIntPoint(128,64));
        TStrongObjectPtr<UTexture2D> Arriving(FACEDatTextureResolver::CreateTransientRgbaWithMips(128,64,Pixels,true));
        FlushRenderingCommands();
        TestEqual(TEXT("Newly loaded objects use the same selected detail"),GPUSize(Arriving.Get()),FIntPoint(128/Divisor,64/Divisor));
    }
    Preset->SetSelectedIndex(0);
    TestEqual(TEXT("Low preset visibly updates texture control"),Detail->GetSelectedIndex(),0);
    Detail->SetSelectedIndex(1);Apply();FlushRenderingCommands();
    TestEqual(TEXT("An individual texture choice overrides the preset"),GPUSize(Object.Get()),FIntPoint(64,32));
    Preset->SetSelectedIndex(3);Apply();
    for(int32 Level:{3,1,0,2,4,0,3})
    {
        Shadow->SetSelectedIndex(Level); Dynamic->SetSelectedIndex(Level==0?0:Level==1?1:2);
        Apply();
        auto* Sun=Sky->SunLightActor->FindComponentByClass<UDirectionalLightComponent>();
        TestEqual(TEXT("Sun's real shadow flag follows quality"),bool(Sun->CastShadows),Level>0);
        TestTrue(TEXT("Sunlight remains available with shadows off"),Sun->IsVisible() && Sun->Intensity>0.f);
        const bool Mobile=World->GetFeatureLevel()<=ERHIFeatureLevel::ES3_1;
        if(Level>0)
        {
            TestEqual(TEXT("Sun uses the selected cascade count"),Sun->DynamicShadowCascades,Mobile||Level==1?1:Level==2?2:4);
            TestEqual(TEXT("Sun uses the selected shadow distance"),Sun->DynamicShadowDistanceMovableLight,Mobile?4000.f:Level==1?10000.f:Level==2?30000.f:60000.f);
        }
        else TestEqual(TEXT("Renderer globally disables dynamic shadows"),IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShadowQuality"))->GetInt(),0);
        const int32 Budget=Level==0?0:Level==1?2:Mobile?4:8;
        TestEqual(TEXT("Actual effect light count obeys preference"),Lights->GetVisibleLightCount(),Budget);
        TestEqual(TEXT("Disabled/excess lights are released from renderer"),Lights->GetAllocatedLightCount(),Budget);
        for(auto* Light:DoorLights()) TestEqual(TEXT("Loaded doorway obeys preference"),Light->IsVisible(),Level>1);
        TestEqual(TEXT("Hidden room only casts for enabled doorway lights"),bool(Room->CellMesh->bCastHiddenShadow),Level>1);
    }
    auto* LightLimit=Cast<USlider>(Options->WidgetTree->FindWidget(TEXT("EffectLightLimit")));
    if(!TestNotNull(TEXT("User-facing effect light limit"),LightLimit))return false;
    LightLimit->SetValue(32);Apply();
    TestEqual(TEXT("Custom light count reaches the desktop renderer"),IConsoleManager::Get().FindConsoleVariable(TEXT("ace.FXLightBudget"))->GetInt(),32);
    TestEqual(TEXT("Increasing the limit lights all ten nearby sources"),Lights->GetVisibleLightCount(),10);
    auto* CountOptions=CreateWidget<UACEVideoSettingsWidget>(GI);CountOptions->TakeWidget();
    TestEqual(TEXT("Reopened settings retain custom light count"),Cast<USlider>(CountOptions->WidgetTree->FindWidget(TEXT("EffectLightLimit")))->GetValue(),32.f);
    LightLimit->SetValue(4);Apply();
    TestEqual(TEXT("Reducing the limit releases excess lights"),Lights->GetVisibleLightCount(),4);
    LightLimit->SetValue(0);Apply();
    TestEqual(TEXT("Zero limit disables effect lights"),Lights->GetVisibleLightCount(),0);
    LightLimit->SetValue(8);Apply();
    auto* Uniform=Cast<UCheckBox>(Options->WidgetTree->FindWidget(TEXT("UniformDungeonLighting")));
    if (!TestNotNull(TEXT("Config exposes uniform dungeon lighting"),Uniform)) return false;
    TestFalse(TEXT("Torchlight is the default"),Uniform->IsChecked());
    Uniform->SetIsChecked(true); Uniform->OnCheckStateChanged.Broadcast(true);
    TestTrue(TEXT("Uniform mode is immediate"),ACERuntimeOptions::UniformDungeonLighting());
    for (auto* Light:DoorLights()) TestTrue(TEXT("Dungeon preference preserves outdoor doorway lights"),Light->IsVisible());
    const auto* Dungeon=Dat->GetOrBuildEnvCellMesh(0x01D9010B,100);
    if (!TestTrue(TEXT("Reported room has surfaces"),Dungeon && !Dungeon->Sections.IsEmpty())) return false;
    const auto& Surface=Dungeon->Sections[0];
    auto* Wall=Cast<UMaterialInstanceDynamic>(Dat->GetOrCreateEnvCellMaterial(Surface.SurfaceId,Surface.bWrapTexture,true));
    if (!TestNotNull(TEXT("Dungeon wall material created after enabling"),Wall)) return false;
    TestEqual(TEXT("Newly streamed dungeon shell is uniform"),Wall->K2_GetScalarParameterValue(TEXT("EmissiveStrength")),1.f);
    auto* Occupant=UMaterialInstanceDynamic::Create(Wall->GetMaterial(),GI);
    Dat->UpdateWorldObjectLighting(Occupant,true,true);
    TestEqual(TEXT("Entering a uniform dungeon lights the character evenly"),Occupant->K2_GetScalarParameterValue(TEXT("EmissiveStrength")),1.f);
    Shadow->SetSelectedIndex(0); Dynamic->SetSelectedIndex(0);
    TestTrue(TEXT("Editing lighting enables Apply"),Options->HasPendingChanges());
    TestEqual(TEXT("Changing selection waits for Apply"),ACERuntimeOptions::DynamicLightingQuality(),2);
    Options->ResetVideo(); TestEqual(TEXT("Reset discards pending lighting change"),Dynamic->GetSelectedIndex(),2);
    Shadow->SetSelectedIndex(0); Dynamic->SetSelectedIndex(0); Apply();
    FConfigFile Saved; Saved.Read(GGameUserSettingsIni); float SavedLights=-1; int32 SavedShadows=-1;
    float SavedUniform=-1.f;
    Saved.GetFloat(TEXT("ACE.Presentation"),TEXT("UniformDungeonLighting"),SavedUniform);
    TestEqual(TEXT("Uniform dungeon preference reaches disk"),SavedUniform,1.f);
    Saved.GetFloat(TEXT("ACE.Presentation"),TEXT("DynamicLightingQuality"),SavedLights);
    Saved.GetInt(TEXT("ScalabilityGroups"),TEXT("sg.ShadowQuality"),SavedShadows);
    TestEqual(TEXT("Lighting Off persisted to disk"),SavedLights,0.f);
    TestEqual(TEXT("Shadows Off persisted to disk"),SavedShadows,0);
    auto* Reopened=CreateWidget<UACEVideoSettingsWidget>(GI); Reopened->TakeWidget();
    TestTrue(TEXT("Reopened config restores uniform preference"),CastChecked<UCheckBox>(Reopened->WidgetTree->FindWidget(TEXT("UniformDungeonLighting")))->IsChecked());
    TestEqual(TEXT("Reopened lighting dropdown restores Off"),Cast<UComboBoxString>(Reopened->WidgetTree->FindWidget(TEXT("DynamicLightingQuality")))->GetSelectedIndex(),0);
    TestEqual(TEXT("Reopened shadow dropdown restores Off"),Cast<UComboBoxString>(Reopened->WidgetTree->FindWidget(TEXT("ShadowQuality")))->GetSelectedIndex(),0);
    Room->LoadEnvCell(0x7D64010C,FVector::ZeroVector,100);
    for(auto* Light:DoorLights()) TestFalse(TEXT("Newly streamed entrance respects Off"),Light->IsVisible());
    Room->SetSunCastShadow(true);
    TestTrue(TEXT("Suppressing doorway shadows preserves requested outdoor casting"),Room->CellMesh->CastShadow);
    Options->DefaultsVideo(); Apply();
    TestFalse(TEXT("Defaults restores torchlit dungeons"),ACERuntimeOptions::UniformDungeonLighting());
    TestTrue(TEXT("Already loaded wall receives torch lighting again"),Wall->K2_GetScalarParameterValue(TEXT("EmissiveStrength"))<1.f);
    TestTrue(TEXT("Already present character receives torch lighting again"),Occupant->K2_GetScalarParameterValue(TEXT("EmissiveStrength"))<1.f);
    TestEqual(TEXT("Defaults restores full dynamic lighting"),Lights->GetVisibleLightCount(),World->GetFeatureLevel()<=ERHIFeatureLevel::ES3_1?4:8);
    for(auto* Source:Sources) Source->GetOwner()->SetActorLocation(FVector(2200,0,100));
    Dynamic->SetSelectedIndex(1); Apply(); TestEqual(TEXT("Low excludes effect lighting beyond 15m"),Lights->GetVisibleLightCount(),0);
    Dynamic->SetSelectedIndex(2); Apply(); TestTrue(TEXT("Full restores distant effect lighting"),Lights->GetVisibleLightCount()>0);
    return true;
}
#endif
