#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "ACEDatSubsystem.h"
#include "ACESkyDomeActor.h"
#include "ACEScriptComponent.h"
#include "Dat/ACERetailGameTime.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/ScopeExit.h"
#include "Scalability.h"
#include "HAL/IConsoleManager.h"
#include "ShaderCompiler.h"
#include "RenderingThread.h"
#include "ImageUtils.h"
#include "ACERuntimeOptions.h" // personal: sky-horizon

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailWeatherTest, "ACE.RetailParity.Weather",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACERetailWeatherTest::RunTest(const FString& Parameters)
{
    const auto SavedQuality=Scalability::GetQualityLevels();
    auto Quality=SavedQuality; Quality.SetFromSingleQualityLevel(3);
    Scalability::SetQualityLevels(Quality);
    ON_SCOPE_EXIT { Scalability::SetQualityLevels(SavedQuality); };
    const float SkyHorizonSaved=ACERuntimeOptions::Get(TEXT("PersonalSkyHorizon")); ACERuntimeOptions::Set(TEXT("PersonalSkyHorizon"),0.f); ON_SCOPE_EXIT { ACERuntimeOptions::Set(TEXT("PersonalSkyHorizon"),SkyHorizonSaved); }; // personal: sky-horizon
    FString Directory = TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(), TEXT("RetailDatDir="), Directory);
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .RequiresHitProxies(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
    auto* GI=NewObject<UGameInstance>(GEngine); GI->InitializeStandalone();
    auto* InitialWorld=GI->GetWorld();
    GI->GetWorldContext()->SetCurrentWorld(World); World->SetGameInstance(GI);
    InitialWorld->DestroyWorld(false);
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    if (!TestTrue(TEXT("Open retail weather DAT"), Dat->LoadDatDirectory(Directory))) return false;
    // Luminous Yaraq scenery retains its authored emission when ambient changes.
    auto* LampSource=Dat->GetOrCreateTexturedMaterial(0x0800012Du);
    auto* Lamp=Dat->GetWorldObjectMaterial(LampSource);
    float LampEmission=0.f;
    TestTrue(TEXT("Lamp world material retains source luminosity"),Lamp && Lamp->GetScalarParameterValue(TEXT("AuthoredLuminosity"),LampEmission) && LampEmission>.99f);
    Dat->SetWorldEmissiveScale(.02f);
    LampEmission=0.f;
    TestTrue(TEXT("Night lighting cannot erase the lamp's emission"),Lamp && Lamp->GetScalarParameterValue(TEXT("AuthoredLuminosity"),LampEmission) && LampEmission>.99f);
    const auto* Region = Dat->GetRegionSkyInfo();
    if (!TestNotNull(TEXT("Region sky data"), Region)) return false;
    TestEqual(TEXT("Server epoch reaches retail noon at tick 210"), ACERetailGameTime::DayFraction(210.0,*Region), .5f);
    TestEqual(TEXT("Retail weather seed includes PY 10"), ACERetailGameTime::DaySeed(210.0,*Region), uint32(3600));
    TestEqual(TEXT("Retail midnight starts the next day at tick 4020"), ACERetailGameTime::DayFraction(4020.0,*Region), 0.f);
    auto* Sky = World->SpawnActor<AACESkyDomeActor>();
    Sky->DatSubsystem = Dat;
    Sky->WeatherScripts->InitializeForEnvironment(100.f);
    Sky->bDriveWorldLighting = false;
    Sky->bDriveWorldFog = false;
    Sky->SetActorLocation(FVector(-2800000,2400000,10000));
    auto* PC=World->SpawnActor<APlayerController>();
    if (!PC->PlayerCameraManager) PC->PlayerCameraManager=World->SpawnActor<APlayerCameraManager>();
    PC->PlayerCameraManager->InitializeFor(PC);
    auto* Viewer=World->SpawnActor<ACameraActor>(); Viewer->SetActorLocation(Sky->GetActorLocation());
    PC->SetViewTarget(Viewer); PC->PlayerCameraManager->UpdateCamera(.016f);
    // Sky transforms and replacements retain the source topology and discrete
    // headings. A radial remap/reversed shell recreates the reported color panels.
    for (uint32 Id : {0x010015F1u,0x010015EFu,0x01004C36u})
    {
        AACESkyDomeActor::FSkySlot Slot;
        Slot.Def.DefaultGfxObjectId=Id; Slot.Def.TexVelocityX=Id==0x01004C36u ? .01f : 0.f;
        Sky->RebuildSlotMesh(Slot,Id);
        const auto* Source=Dat->GetOrBuildSetupMesh(Id,100.f*Sky->SkyDistanceScale);
        if (TestNotNull(TEXT("Authored celestial mesh loads"),Source) && TestNotNull(TEXT("Celestial draw mesh exists"),Slot.Mesh))
        {
            int32 Index=0;
            for (const auto& Part:Source->Parts) for (const auto& Sec:Part.Sections)
            {
                if (Sec.IsEmpty() || Sec.bFullyTransparent || Sec.bCollisionOnly) continue;
                const auto* Draw=Slot.Mesh->GetProcMeshSection(Index++);
                bool Same=Draw && Draw->ProcVertexBuffer.Num()==Sec.Vertices.Num() && Draw->ProcIndexBuffer.Num()==Sec.Triangles.Num();
                if (Same)
                {
                    for (int32 I=0;I<Sec.Vertices.Num();++I) Same &= FVector(Draw->ProcVertexBuffer[I].Position).Equals(Part.BindTransform.TransformPosition(Sec.Vertices[I]),.1);
                    for (int32 I=0;I<Sec.Triangles.Num();++I) Same &= Draw->ProcIndexBuffer[I]==uint32(Sec.Triangles[I]);
                }
                TestTrue(TEXT("Sky retains authored vertices and winding"),Same);
            }
        }
        Sky->DestroySlotMesh(Slot);
    }
    {
    Sky->ActiveGroup.TimesOfDay.SetNum(2);
    FACEDatSkyObjectReplace A,B; A.ObjectIndex=0;B.ObjectIndex=0;
    A.Rotate=270;B.Rotate=0;A.GfxObjId=0x010015F1;B.GfxObjId=0x010015F0;
    A.Transparent=0;B.Transparent=100;
    Sky->ActiveGroup.TimesOfDay[0].Replaces.Add(A);
    Sky->ActiveGroup.TimesOfDay[1].Replaces.Add(B);
    AACESkyDomeActor::FCelestialState State; FACEDatSkyObject Def;
    Sky->ResolveCelestialState(.5,0,1,.75,0,Def,State);
    TestEqual(TEXT("A sky heading does not spin between replacement keys"),State.HeadingDeg,270.f);
    TestEqual(TEXT("Sky mesh changes at the next key, not mid-interval"),State.GfxId,0x010015F1u);
    TestEqual(TEXT("Opacity still interpolates between time keys"),State.Transparent,75.f);
    Sky->ActiveGroup.TimesOfDay.Reset();
    }
    // DAT rainy group has visible rain at 0.10 and clear skies at 0.50.
    for (const auto& Group : Region->DayGroups)
        if (Group.DayName == TEXT("Rainy")) Sky->ActiveGroup = Group;
    int32 RainCount = 0;
    for (int32 I=0; I<Sky->ActiveGroup.Objects.Num(); ++I)
    {
        const auto& Def = Sky->ActiveGroup.Objects[I];
        if (Def.DefaultGfxObjectId != 0x01004C44 && Def.DefaultGfxObjectId != 0x01004C42) continue;
        auto& Slot = Sky->Slots.AddDefaulted_GetRef();
        Slot.Def = Def; Slot.ObjectIndex = I;
        Slot.bWeather = (Def.Properties & 4) != 0; Slot.bAfterPass = (Def.Properties & 1) != 0;
        Slot.bForceWeatherZ = Slot.bWeather && (Def.Properties & 8) == 0;
        Sky->RebuildSlotMesh(Slot, Def.DefaultGfxObjectId);
        if (Slot.Mesh && Slot.Mesh->GetNumSections() > 0) ++RainCount;
        const auto* Authored = Dat->GetOrBuildSetupMesh(Def.DefaultGfxObjectId,100.f);
        if (Slot.Mesh && Authored && Authored->Parts.Num())
        {
            const auto* Section = Slot.Mesh->GetProcMeshSection(0);
            const auto& Source = Authored->Parts[0].Sections[0];
            bool bSame = Section && Section->ProcIndexBuffer.Num() == Source.Triangles.Num();
            if (bSame) for (int32 T=0; T<Source.Triangles.Num(); ++T) bSame &= Section->ProcIndexBuffer[T] == uint32(Source.Triangles[T]);
            TestTrue(TEXT("Weather preserves authored polygon sides and winding"), bSame);
        }
    }
    TestEqual(TEXT("Both retail rain curtains have drawable meshes"), RainCount, 2);
    // Frame::grotate rotates the already headed frame about the world -Y axis.
    // At heading=90 and rotation=90 the AC +X basis (UE -X) ends along AC +Y,
    // rather than the AC +Z produced by applying the rotations in reverse.
    if (!Sky->Slots.IsEmpty() && Sky->Slots[0].Mesh)
    {
        AACESkyDomeActor::FCelestialState Frame;Frame.HeadingDeg=90;Frame.RotationDeg=90;
        Sky->ApplySlotFrame(Sky->Slots[0],Frame);
        const FVector Axis=Sky->Slots[0].Mesh->GetRelativeRotation().Quaternion().RotateVector(FVector(-1,0,0));
        TestTrue(TEXT("Celestial frames use retail global rotation after heading"),Axis.Equals(FVector(0,1,0),.001));
    }
    Sky->SetWeatherEnabled(true);
    Sky->UpdateSky(.1f, .1f);
    for (auto& Slot : Sky->Slots)
    {
        TestTrue(TEXT("Rain is enabled at the authored rainy time"), Slot.Mesh && Slot.Mesh->IsVisible());
        if (Slot.Mesh) TestEqual(TEXT("GameSky fixes weather at absolute Z=-120 AC"), Slot.Mesh->GetComponentLocation().Z, -12000.0);
    }
    if (FApp::CanEverRender() && RainCount == 2)
    {
        if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
        auto* Target = NewObject<UTextureRenderTarget2D>(Sky);
        Target->ClearColor = FLinearColor::Black;
        Target->InitCustomFormat(256,256,PF_B8G8R8A8,false);
        auto* Capture = NewObject<USceneCaptureComponent2D>(Sky);
        Capture->TextureTarget = Target;
        Capture->SetWorldLocation(Sky->GetActorLocation());
        Capture->FOVAngle = 90.f;
        Capture->bCaptureEveryFrame = false; Capture->bCaptureOnMovement = false;
        Capture->CaptureSource = SCS_FinalColorLDR;
        Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetBloom(false);
        Capture->ShowFlags.SetAtmosphere(false); Capture->ShowFlags.SetAntiAliasing(false);
        Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
        for (auto& Slot : Sky->Slots) Capture->ShowOnlyComponents.Add(Slot.Mesh);
        Capture->RegisterComponent();
        auto Read = [&]() {
            World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
            TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels); return Pixels;
        };
        auto LitPixels = [](const TArray<FColor>& Pixels) { int32 N=0; for (FColor P : Pixels) if (P.R>24 || P.G>24 || P.B>24) ++N; return N; };
        const auto RainPixels = Read();
        TestTrue(TEXT("Retail rain streaks render from inside the camera-centered curtain"), LitPixels(RainPixels)>100);
        AddInfo(FString::Printf(TEXT("Rain GPU pixels: %d / %d"), LitPixels(RainPixels), RainPixels.Num()));
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(256,256,RainPixels,PNG);
        FFileHelper::SaveArrayToFile(PNG, *(FPaths::ProjectSavedDir()/TEXT("Automation/RetailParity/Weather.png")));

        // A curtain is not a screen overlay: opaque world geometry must stop
        // the rain behind it. Exercise actual DAT geometry, offset eye origins,
        // a wall and downward views of sloping ground in desktop/mobile renderers.
        auto* Occluder=NewObject<UProceduralMeshComponent>(Sky);
        Occluder->RegisterComponent(); Occluder->SetWorldLocation(Sky->GetActorLocation());
        Occluder->SetMaterial(0,UMaterial::GetDefaultMaterial(MD_Surface));
        Capture->ShowOnlyComponents.Add(Occluder);
        auto CompareOcclusion=[&](const TCHAR* Label)
        {
            for (float EyeOffset : {-3.2f,3.2f})
            {
                Capture->SetWorldLocation(Sky->GetActorLocation()+FVector(0,EyeOffset,0));
                for (auto& Slot:Sky->Slots) Slot.Mesh->SetVisibility(false);
                const auto Dry=Read();
                for (auto& Slot:Sky->Slots) Slot.Mesh->SetVisibility(true);
                const auto Wet=Read();
                int32 Changed=0;
                for (int32 P=0;P<Dry.Num();++P) if (Dry[P]!=Wet[P]) ++Changed;
                TestEqual(FString::Printf(TEXT("%s occludes rain for eye %.1f"),Label,EyeOffset),Changed,0);
            }
        };
        Occluder->CreateMeshSection_LinearColor(0,
            {FVector(20,-10000,-10000),FVector(20,10000,-10000),FVector(20,10000,10000),FVector(20,-10000,10000)},
            {0,2,1,0,3,2,0,1,2,0,2,3},{},{},{},{},false);
        CompareOcclusion(TEXT("Opaque wall"));
        Occluder->CreateMeshSection_LinearColor(0,
            {FVector(-10000,-10000,-5020),FVector(10000,-10000,4980),FVector(10000,10000,4980),FVector(-10000,10000,-5020)},
            {0,2,1,0,3,2,0,1,2,0,2,3},{},{},{},{},false);
        Capture->SetWorldRotation(FRotator(-65,0,0));
        CompareOcclusion(TEXT("Sloping terrain"));
        Occluder->DestroyComponent(); Capture->ShowOnlyComponents.Remove(Occluder);
        Capture->SetWorldLocation(Sky->GetActorLocation()); Capture->SetWorldRotation(FRotator::ZeroRotator);
        TestTrue(TEXT("Occlusion does not disable unobstructed rain"),LitPixels(Read())>100);
        for (auto* Material:{Dat->EnsureAceWeatherTranslucentMaterialBase(),Dat->EnsureAceWeatherAdditiveMaterialBase()})
            TestTrue(TEXT("Both weather blend modes use per-sample hardware world depth"),Material && !Material->GetMaterial()->bDisableDepthTest);
        Sky->UpdateSky(.1f,.25f);
        const auto MovedPixels = Read();
        TestTrue(TEXT("DAT texture velocity animates rainfall"), RainPixels != MovedPixels);
        Sky->SetWeatherEnabled(false);
        TestEqual(TEXT("Indoor/disabled weather produces no rain pixels"), LitPixels(Read()), 0);
        Sky->SetWeatherEnabled(true);
        Sky->UpdateSky(.1f, .1f);
        const auto ResumedPixels = Read();
        TestTrue(TEXT("Rain returns after leaving a building"), LitPixels(ResumedPixels)>100);
        Sky->UpdateSky(.1f, .25f);
        TestTrue(TEXT("Rain keeps moving after leaving a building"), ResumedPixels != Read());
        for (const auto& Slot : Sky->Slots)
            for (int32 I=0; I<Slot.Mids.Num(); ++I)
                TestTrue(TEXT("Weather stop preserves the animated sky material"), Slot.Mesh->GetMaterial(I)==Slot.Mids[I]);
        Sky->UpdateSky(.5f,0.f);
        TestEqual(TEXT("Rain remains hidden during the authored clear interval"), LitPixels(Read()), 0);
    }
    // Render complete authored sky groups, including the horizon. An opaque
    // foreground must occlude the sky even though UE submits translucency later.
    if (FApp::CanEverRender())
    {
        Sky->ClearSky();
        auto* Foreground=NewObject<UProceduralMeshComponent>(Sky);
        Foreground->RegisterComponent();
        const FVector View=Sky->GetActorLocation();
        TArray<FVector> V={FVector(500,-10000,-10000),FVector(500,10000,-10000),
            FVector(500,10000,0),FVector(500,-10000,0)};
        Foreground->CreateMeshSection_LinearColor(0,V,{0,2,1,0,3,2,0,1,2,0,2,3},{},{},{},{},false);
        auto* ForegroundMaterial=UMaterialInstanceDynamic::Create(Dat->EnsureAceUnlitTexturedMaterialBase(),Sky);
        auto* White=UTexture2D::CreateTransient(1,1,PF_B8G8R8A8);
        *static_cast<FColor*>(White->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE))=FColor::White;
        White->GetPlatformData()->Mips[0].BulkData.Unlock();White->UpdateResource();
        ForegroundMaterial->SetTextureParameterValue(TEXT("ACETexture"),White);
        ForegroundMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"),.25f);
        ForegroundMaterial->SetScalarParameterValue(TEXT("FogAmount"),0.f);
        Foreground->SetMaterial(0,ForegroundMaterial);Foreground->SetWorldLocation(View);
        auto* Target=NewObject<UTextureRenderTarget2D>(Sky);
        Target->InitCustomFormat(640,360,PF_FloatRGBA,false);
        auto* Capture=NewObject<USceneCaptureComponent2D>(Sky);
        Capture->TextureTarget=Target;Capture->SetWorldLocation(View);Capture->FOVAngle=90;
        Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->CaptureSource=SCS_SceneColorHDRNoAlpha;
        Capture->ShowFlags.SetEyeAdaptation(false);Capture->ShowFlags.SetBloom(false);
        Capture->ShowFlags.SetAtmosphere(false);Capture->ShowFlags.SetAntiAliasing(false);Capture->ShowFlags.SetLighting(false);
        Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
        Capture->ShowOnlyComponents.Add(Foreground);Capture->RegisterComponent();
        const bool MobileLdr=World->GetFeatureLevel()==ERHIFeatureLevel::ES3_1 && IConsoleManager::Get().FindConsoleVariable(TEXT("r.MobileHDR"))->GetInt()==0;
        auto Read=[&]() {
            if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
            World->SendAllEndOfFrameUpdates();Capture->CaptureScene();FlushRenderingCommands();
            TArray<FLinearColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
            // Mobile LDR's scene-color capture already contains gamma-2 output.
            // Normalize it before comparing linear colors or encoding a PNG.
            if (MobileLdr) for (auto& P:Pixels) { P.R*=P.R;P.G*=P.G;P.B*=P.B; }
            return Pixels;
        };
        const auto ForegroundOnly=Read();
        TestTrue(TEXT("Foreground occluder produces a visible reference"),ForegroundOnly[270*640+320].R>.1f);
        // Exercise the compiled shader, not just the parameter values: existing
        // instances must see collection changes without receiving any MID edits.
        ForegroundMaterial->SetScalarParameterValue(TEXT("FogAmount"),1.f);
        Dat->SetWorldDistanceFog(0.f,400.f,FLinearColor::Red,1.f);
        const auto RedFog=Read()[270*640+320];
        TestTrue(TEXT("Shared fog reaches the rendered surface"),RedFog.R>.9f && RedFog.G<.01f && RedFog.B<.01f);
        Dat->SetWorldDistanceFog(0.f,400.f,FLinearColor::Blue,1.f);
        const auto BlueFog=Read()[270*640+320];
        TestTrue(TEXT("A collection update changes an existing rendered material"),BlueFog.B>.9f && BlueFog.R<.01f && BlueFog.G<.01f);
        Dat->SetWorldDistanceFog(0.f,400.f,FLinearColor::Blue,0.f);
        TestTrue(TEXT("Shared fog can be disabled without changing local masks"),Read()[270*640+320].Equals(ForegroundOnly[270*640+320],.002f));
        Dat->SetWorldDistanceFog(0.f,400.f,FLinearColor::Red,1.f);
        ForegroundMaterial->SetScalarParameterValue(TEXT("FogAmount"),0.f);
        TestTrue(TEXT("An interior or preview opt-out stays unfogged"),Read()[270*640+320].Equals(ForegroundOnly[270*640+320],.002f));
        Dat->SetWorldDistanceFog(20000.f,90000.f,FLinearColor(.55f,.62f,.78f,1.f),1.f);
        {
            auto* Card=NewObject<UProceduralMeshComponent>(Sky);Card->RegisterComponent();Card->SetWorldLocation(View);
            Card->CreateMeshSection_LinearColor(0,{FVector(500,-500,-500),FVector(500,500,-500),FVector(500,500,500),FVector(500,-500,500)},
                {0,2,1,0,3,2,0,1,2,0,2,3},{FVector::ForwardVector,FVector::ForwardVector,FVector::ForwardVector,FVector::ForwardVector},
                {FVector2D(0,1),FVector2D(1,1),FVector2D(1,0),FVector2D(0,0)},{},{},false);
            auto* Glow=UMaterialInstanceDynamic::Create(Dat->EnsureAceSkyAdditiveMaterialBase(),Sky);
            Glow->SetTextureParameterValue(TEXT("ACETexture"),White);Glow->SetScalarParameterValue(TEXT("EmissiveStrength"),1.f);
            Glow->SetScalarParameterValue(TEXT("OpacityMul"),.1f);Card->SetMaterial(0,Glow);
            Capture->ShowOnlyComponents.Reset();Capture->ShowOnlyComponents.Add(Card);
            const auto Faded=Read();
            const float Expected=MobileLdr ? .01f : FMath::Pow((.1f+.055f)/1.055f,2.4f);
            TestTrue(TEXT("A 10-percent sky glow retains retail brightness in the active renderer"),FMath::IsNearlyEqual(Faded[180*640+320].R,Expected,.002f));
            Glow->SetScalarParameterValue(TEXT("SkyCardEdgeFade"),1.f);
            const auto Feathered=Read();
            TestTrue(TEXT("Glow-card edge fades without dimming its center"),Feathered[180*640+2].R<Faded[180*640+2].R*.05f
                && FMath::IsNearlyEqual(Feathered[180*640+320].R,Faded[180*640+320].R,.001f));
            // A white foreground edge against a white sky must remain white.
            // Resolved-depth rejection leaves dark pixels where MSAA covers
            // only some samples, especially visible against the bright sky.
            Glow->SetScalarParameterValue(TEXT("SkyCardEdgeFade"),0.f);
            Glow->SetScalarParameterValue(TEXT("OpacityMul"),1.f);
            ForegroundMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"),1.f);
            auto Sloped=V;Sloped[2].Z=1730.3;Sloped[3].Z=-1729.7;
            Foreground->CreateMeshSection_LinearColor(0,Sloped,{0,2,1,0,3,2,0,1,2,0,2,3},{},{},{},{},false);
            Capture->ShowOnlyComponents.Add(Foreground);Capture->ShowFlags.SetAntiAliasing(true);
            const auto Edge=Read();float Minimum=1.f;
            for(int32 Y=145;Y<215;++Y)for(int32 X=220;X<420;++X)Minimum=FMath::Min(Minimum,Edge[Y*640+X].R);
            TestTrue(TEXT("Sky fills partially covered foreground edge samples without dark fringes"),Minimum>.94f);
            ForegroundMaterial->SetScalarParameterValue(TEXT("EmissiveStrength"),.25f);
            Foreground->CreateMeshSection_LinearColor(0,V,{0,2,1,0,3,2,0,1,2,0,2,3},{},{},{},{},false);
            Capture->ShowFlags.SetAntiAliasing(false);
            Card->DestroyComponent();Capture->ShowOnlyComponents.Reset();Capture->ShowOnlyComponents.Add(Foreground);
            for (uint32 SurfaceId:{0x0800003Fu,0x08000040u,0x08000041u})
            {
                auto* Source=Dat->GetOrCreateParticleMaterial(SurfaceId);UTexture* Original=nullptr;Source->GetTextureParameterValue(TEXT("ACETexture"),Original);
                auto* SkyParticle=Dat->CreateSkyParticleMaterial(Source,Sky);UTexture* Texture=nullptr;
                TestTrue(TEXT("Celestial particle material retains its texture"),SkyParticle && SkyParticle->GetTextureParameterValue(TEXT("ACETexture"),Texture));
                auto* Image=Cast<UTexture2D>(Texture);
                TestTrue(TEXT("Sky particles clamp their edges and retain full image detail"),Image && Image->AddressX==TA_Clamp && Image->AddressY==TA_Clamp && Image->GetNumMips()==1);
                UTexture* Unchanged=nullptr;Source->GetTextureParameterValue(TEXT("ACETexture"),Unchanged);
                TestTrue(TEXT("Sky conversion leaves world particle textures unchanged"),Original==Unchanged);
            }
        }
        for (int32 GroupIndex=0;GroupIndex<Region->DayGroups.Num();++GroupIndex)
        {
            const auto& Group=Region->DayGroups[GroupIndex];
            Sky->ClearSky();Sky->ActiveGroup=Group;Sky->EnsureColorFillMesh();
            for (int32 I=0;I<Group.Objects.Num();++I)
            {
                const auto& Def=Group.Objects[I];
                if (!Def.DefaultGfxObjectId || (Def.Properties&5)!=0) continue;
                auto& Slot=Sky->Slots.AddDefaulted_GetRef();Slot.Def=Def;Slot.ObjectIndex=I;
                Slot.PesScriptId=Def.DefaultPesObjectId;
                Sky->RebuildSlotMesh(Slot,Def.DefaultGfxObjectId);
            }
            for (float Fraction:{.1f,.15f,.18f,.5f,.87f})
            {
                int32 A,B;float Alpha;
                Sky->BracketTimeOfDay(Group.TimesOfDay,Fraction,A,B,Alpha);
                Sky->UpdateColorFill(FMath::Lerp(Sky->ArgbToLinear(Group.TimesOfDay[A].WorldFogColor),Sky->ArgbToLinear(Group.TimesOfDay[B].WorldFogColor),Alpha));
                Sky->UpdateSky(Fraction,0.f);
                Sky->WeatherScripts->TickComponent(1.f/30.f,LEVELTICK_All,nullptr);
                if (!Sky->WeatherScripts->EnvironmentSkyMaterials.IsEmpty())
                {
                    auto* Cloud=Sky->WeatherScripts->EnvironmentSkyMaterials[0].Get();
                    FLinearColor Ambient;
                    TestTrue(TEXT("Scripted sky clouds receive the current ambient light"),Cloud && Cloud->GetVectorParameterValue(TEXT("SkyAmbient"),Ambient)
                        && Ambient.Equals(Sky->WeatherScripts->EnvironmentAmbient,.001f));
                }

                if (Sky->Slots.ContainsByPredicate([](const auto& Slot){return Slot.PesScriptId==0x330007DB;}))
                {
                    TestTrue(TEXT("Sky fixture includes the real scripted cloud particles"),!Sky->WeatherScripts->ActiveEmitters.IsEmpty());
                    for (const auto& Emitter:Sky->WeatherScripts->ActiveEmitters)
                        if (Emitter.AuthoredEmitterId==0x32000455)
                            TestEqual(TEXT("Long-lived sky clouds retain their DAT lifespan"),Emitter.Info.Lifespan,3300.0);
                    const int32 Before=Sky->WeatherScripts->ActiveEmitters.Num();
                    Sky->SetWeatherEnabled(false);
                    TestEqual(TEXT("Disabling weather retains before-pass celestial particles"),Sky->WeatherScripts->ActiveEmitters.Num(),Before);
                    Sky->SetWeatherEnabled(true);
                }
                // Retail dims its background at night; it does not hide it at a
                // hardcoded clock threshold independent of the authored keys.
                TestTrue(TEXT("Authored background remains visible at every clock sample"),Sky->Slots[0].Mesh && Sky->Slots[0].Mesh->IsVisible());
                Capture->ShowOnlyComponents.Reset();Capture->ShowOnlyComponents.Add(Foreground);
                Capture->ShowOnlyComponents.Add(Sky->ColorFillMesh);
                for (auto& Slot:Sky->Slots) if (Slot.Mesh) Capture->ShowOnlyComponents.Add(Slot.Mesh);
                for (auto& Emitter:Sky->WeatherScripts->ActiveEmitters)
                    for (auto& Particle:Emitter.Particles) if (Particle.Mesh.IsValid()) Capture->ShowOnlyComponents.Add(Particle.Mesh.Get());
                if (GroupIndex==0 && Fraction==.15f && FParse::Param(FCommandLine::Get(),TEXT("SkyLayerDiagnostics")))
                {
                    const auto AllComponents=Capture->ShowOnlyComponents;
                    Capture->SetWorldRotation(FRotator(55,0,0));
                    auto SaveLayer=[&](const FString& Name)
                    {
                        const auto Image=Read(); TArray<FColor> Display;
                        for(const auto& Pixel:Image) Display.Add(Pixel.ToFColorSRGB());
                        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(640,360,Display,PNG);
                        FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/RetailParity/")+Name+TEXT(".png")));
                    };
                    SaveLayer(TEXT("Sky_All_Zenith"));
                    for (const auto& Slot:Sky->Slots) if(Slot.Mesh)
                    {
                        Capture->ShowOnlyComponents.Reset();Capture->ShowOnlyComponents.Add(Slot.Mesh);
                        SaveLayer(FString::Printf(TEXT("Sky_Layer_%02d_%08X"),Slot.ObjectIndex,Slot.ActiveGfxId));
                    }
                    Capture->ShowOnlyComponents.Reset();
                    for (auto& Emitter:Sky->WeatherScripts->ActiveEmitters)
                        for(auto& Particle:Emitter.Particles) if(Particle.Mesh.IsValid()) Capture->ShowOnlyComponents.Add(Particle.Mesh.Get());
                    SaveLayer(TEXT("Sky_Particle_Layers"));
                    Capture->ShowOnlyComponents=AllComponents;
                }
                for (int32 Yaw : {0,90,180,270})
                {
                    Capture->SetWorldRotation(FRotator(0,Yaw,0));
                    Viewer->SetActorRotation(FRotator(0,Yaw,0)); PC->PlayerCameraManager->UpdateCamera(.016f);
                    Sky->WeatherScripts->TickEmitters(0.f);
                    Foreground->SetWorldRotation(FRotator(0,Yaw,0));
                    if (GroupIndex==10 && Fraction==.15f && Yaw==90)
                    {
                        // Exercise the saved parents used by a packaged game as well
                        // as freshly generated editor parents, at real world distances.
                        const auto SourcePixels=Read();
                        for (auto& Slot:Sky->Slots) if (Slot.Mesh)
                            for (int32 M=0; M<Slot.Mids.Num(); ++M)
                            {
                                const FString Name=Slot.Mids[M]->Parent->GetName();
                                auto* Baked=LoadObject<UMaterialInterface>(nullptr,*FString::Printf(TEXT("/Game/ACE/RuntimeMaterials/%s.%s"),*Name,*Name));
                                if (!TestNotNull(TEXT("Saved sky material is present"),Baked)) continue;
#if WITH_EDITOR
                                // Offscreen captures do not pump the editor's on-demand
                                // shader requests. Prepare draw permutations before the
                                // synchronous readback. Cooked games already include them.
                                Baked->GetMaterial()->ForceRecompileForRendering();
#endif
                                auto* Mid=UMaterialInstanceDynamic::Create(Baked,Sky);
                                Mid->CopyParameterOverrides(Slot.Mids[M]); Slot.Mesh->SetMaterial(M,Mid);
                            }
                        const auto BakedPixels=Read();
                        int32 Changed=0;
                        for(int32 I=0;I<SourcePixels.Num();++I)
                            if(!SourcePixels[I].Equals(BakedPixels[I],.02f)) ++Changed;
                        TestTrue(TEXT("Saved sky shaders match the editor sky"),Changed<640*360/100);
                        TArray<FColor> Display;for(const auto& P:BakedPixels)Display.Add(P.ToFColorSRGB());
                        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(640,360,Display,PNG);
                        FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/RetailParity/Sky_BakedParents.png")));
                        for(auto& Slot:Sky->Slots) if(Slot.Mesh) for(int32 M=0;M<Slot.Mids.Num();++M) Slot.Mesh->SetMaterial(M,Slot.Mids[M]);
                    }
                    if (GroupIndex==16 && Fraction==.1f)
                    {
                        Capture->ShowOnlyComponents.Remove(Foreground);
                        const auto Horizon=Read();
                        // Independent rainy-night DAT fixture from the user's Yaraq
                        // comparison: purple clouds and horizon, not a green day dome
                        // leaking through an incorrectly additive star/cloud layer.
                        FLinearColor Mean=FLinearColor::Black;
                        for (int32 Y=185; Y<260; ++Y)
                            for (int32 X=0; X<640; ++X) Mean+=Horizon[Y*640+X];
                        TestTrue(TEXT("Rainy night horizon retains its authored purple tint in every direction"),
                            Mean.R>Mean.G*1.25f && Mean.B>Mean.G*1.25f);

                        TArray<FColor> Display;for (const auto& Pixel:Horizon) Display.Add(Pixel.ToFColorSRGB());
                        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(640,360,Display,PNG);
                        FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("Automation/RetailParity/Sky_Horizon_%03d.png"),Yaw)));
                        Capture->ShowOnlyComponents.Add(Foreground);
                    }
                    const auto Pixels=Read();
                    TestTrue(TEXT("Horizon sky stays behind geometry in every direction"),Pixels[270*640+320].Equals(ForegroundOnly[270*640+320],.0001f));
                    if (GroupIndex==16 && Fraction==.1f)
                    {
                        for (float DistanceScale:{200.f,1000.f})
                        {
                            Foreground->SetWorldScale3D(FVector(DistanceScale));
                            const auto Distant=Read();
                            if (!Distant[270*640+320].Equals(ForegroundOnly[270*640+320],.0001f))
                            {
                                const auto SkyComponents=Capture->ShowOnlyComponents;
                                Capture->ShowOnlyComponents.Reset();Capture->ShowOnlyComponents.Add(Foreground);
                                const auto DistantOnly=Read();
                                Capture->ShowOnlyComponents=SkyComponents;
                                AddInfo(FString::Printf(TEXT("Distant sky Yaw=%d Scale=%.0f near=%s distantOnly=%s withSky=%s"),
                                    Yaw,DistanceScale,*ForegroundOnly[270*640+320].ToString(),*DistantOnly[270*640+320].ToString(),*Distant[270*640+320].ToString()));
                            }
                            TestTrue(TEXT("Scripted sky effects stay behind terrain at 1km and 5km"),
                                Distant[270*640+320].Equals(ForegroundOnly[270*640+320],.0001f));
                        }
                        Foreground->SetWorldScale3D(FVector::OneVector);
                    }

                }
            }
        }
        Capture->DestroyComponent();Foreground->DestroyComponent();Sky->ClearSky();
    }
    // Exercise material instances created before and after a weather change. The
    // old outdoor cache missed fog updates, and entering a room disabled the
    // outdoor world's fog as well as the room's.
    auto* Outdoor = Cast<UMaterialInstanceDynamic>(Dat->GetOrCreateOutdoorLitMaterial(0x08000708));
    auto* Indoor = Cast<UMaterialInstanceDynamic>(Dat->GetOrCreateEnvCellMaterial(0x08000708));
    if (TestNotNull(TEXT("DAT outdoor fog material"), Outdoor) && TestNotNull(TEXT("DAT indoor material"), Indoor))
    {
        auto* Collection = Dat->GetRuntimeFogCollection();
        auto* Fog = World->GetParameterCollectionInstance(Collection);
        if (!TestNotNull(TEXT("World owns its shared fog parameters"), Fog)) return false;
        auto Scalar = [Fog](const TCHAR* Name) { float Value = -1.f; Fog->GetScalarParameterValue(Name, Value); return Value; };
        UMaterialParameterCollection* Bound = nullptr;
        TestTrue(TEXT("Existing outdoor shader uses the shared fog collection"),
            Outdoor->GetParameterCollectionParameterValue(TEXT("FogStart"), Bound) && Bound == Collection);
        FACEDatSkyTimeOfDay A, B;
        A.MinWorldFog=0; A.MaxWorldFog=1000; A.WorldFogColor=0xFF204060;
        B.MinWorldFog=20; B.MaxWorldFog=1400; B.WorldFogColor=0xFF6080A0;
        Sky->FogClampLandblocks=0;
        Dat->SetIndoorEnvironment(true);
        Sky->UpdateFog(A,B,.5f);
        TestEqual(TEXT("Retail fog preserves the interpolated near distance"), Scalar(TEXT("FogStart")), 1000.f);
        TestEqual(TEXT("Retail fog far distance is not silently capped at 900m"), Scalar(TEXT("FogEnd")), 120000.f);
        TestEqual(TEXT("Landscape seen from indoors retains distance fog"), Outdoor->K2_GetScalarParameterValue(TEXT("FogAmount")), 1.f);
        TestEqual(TEXT("Indoor material does not inherit outdoor distance fog"), Indoor->K2_GetScalarParameterValue(TEXT("FogAmount")), 0.f);
        FLinearColor SharedColor;
        TestTrue(TEXT("Cached scenery receives the weather fog color"), Fog->GetVectorParameterValue(TEXT("FogColor"), SharedColor) && SharedColor.Equals(Sky->LastFillColor,.001f));
        Dat->SetIndoorEnvironment(false);
        Sky->UpdateFog(A,A,0);
        TestEqual(TEXT("Authored zero fog start is preserved"), Scalar(TEXT("FogStart")), 0.f);
        TestEqual(TEXT("Reused scenery receives the next weather interval"), Scalar(TEXT("FogEnd")), 100000.f);
        auto* Shell = Cast<UMaterialInstanceDynamic>(Dat->GetOrCreateBuildingShellMaterial(0x08000708));
        if (TestNotNull(TEXT("DAT shell created after weather update"),Shell))
            TestTrue(TEXT("New building material shares the current fog"), Shell->GetParameterCollectionParameterValue(TEXT("FogEnd"), Bound) && Bound == Collection);
        // Weather changes must not resurrect per-object fog disabled by interior
        // presentation or a preview material, or rewrite ordinary instance data.
        Outdoor->SetScalarParameterValue(TEXT("FogAmount"), .37f);
        const int32 LocalScalarCount = Outdoor->ScalarParameterValues.Num();
        const int32 LocalVectorCount = Outdoor->VectorParameterValues.Num();
        A.Begin=0.f; B.Begin=.5f; Sky->ActiveGroup.TimesOfDay={A,B}; Sky->bDriveWorldFog=true;
        Sky->UpdateSky(.1f,1.f/90.f);
        const float FirstFog=Scalar(TEXT("FogEnd"));
        Sky->UpdateSky(.101f,1.f/90.f);
        TestEqual(TEXT("Slow fog changes keep their bounded update interval"),Scalar(TEXT("FogEnd")),FirstFog);
        Sky->UpdateSky(.101f,.1f);
        TestTrue(TEXT("Fog advances at its bounded update interval"),Scalar(TEXT("FogEnd"))>FirstFog);
        Sky->UpdateSky(.4f,1.f/90.f);
        TestEqual(TEXT("A server clock jump applies fog immediately"),Scalar(TEXT("FogEnd")),132000.f);
        TestEqual(TEXT("Weather preserves local fog masks"), Outdoor->K2_GetScalarParameterValue(TEXT("FogAmount")), .37f);
        TestEqual(TEXT("Fog updates add no scalar overrides to objects"), Outdoor->ScalarParameterValues.Num(), LocalScalarCount);
        TestEqual(TEXT("Fog updates add no vector overrides to objects"), Outdoor->VectorParameterValues.Num(), LocalVectorCount);
    }
    Sky->ClearSky();
    return true;
}
#endif
