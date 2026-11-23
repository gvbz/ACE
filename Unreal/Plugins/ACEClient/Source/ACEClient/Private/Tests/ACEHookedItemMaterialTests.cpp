#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "ACEDatSubsystem.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACEWorldEntityActor.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "ShaderCompiler.h"
#include "ImageUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEHookedItemMaterialTest,"ACE.Rendering.HookedItemMaterials",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACEHookedItemMaterialTest::RunTest(const FString&)
{
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
    auto* GI=NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); Context.OwningGameInstance=GI;
    GI->OnWorldChanged(nullptr,World); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    FString Directory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),Directory);
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    if(!TestTrue(TEXT("Retail DAT opens"),Dat && Dat->LoadDatDirectory(Directory))) return false;

    auto CheckBlackSwatch=[this](UMaterialInterface* Material)
    {
        UTexture* Texture=nullptr;
        if(!TestTrue(TEXT("Solid-color material has its authored texture"),Material &&
            Material->GetTextureParameterValue(TEXT("ACETexture"),Texture))) return;
        auto* Swatch=Cast<UTexture2D>(Texture);
        if(!TestNotNull(TEXT("Solid-color texture is valid"),Swatch)) return;
        if(!TestTrue(TEXT("Solid color uses a one-pixel swatch"),Swatch->GetSizeX()==1 && Swatch->GetSizeY()==1)) return;
        if(!FApp::CanEverRender()) return;
        const auto* Resource=Swatch->GetResource();
        if(!TestNotNull(TEXT("Swatch has a render resource"),Resource)) return;
        // UE can discard the transient mip's CPU bytes after upload. Check
        // the texture the material samples instead of its former upload buffer.
        TArray<FColor> Pixels;
        ENQUEUE_RENDER_COMMAND(ACEReadSolidSwatch)([Resource,&Pixels](FRHICommandListImmediate& Cmd)
        {
            if(Resource->TextureRHI)
                Cmd.ReadSurfaceData(Resource->TextureRHI,FIntRect(0,0,1,1),Pixels,FReadSurfaceDataFlags());
        });
        FlushRenderingCommands();
        if(TestEqual(TEXT("Swatch readback has one pixel"),Pixels.Num(),1))
            TestEqual(TEXT("Authored black survives material conversion"),Pixels[0],FColor::Black);
    };
    // UV wrapping must not change a solid surface's color or force a fallback
    // that lighting/examination then mistake for a white textured surface.
    for(uint8 WrapAxes=0; WrapAxes<4; ++WrapAxes)
    {
        auto* Material=Dat->GetOrCreateResolvedMaterial(0x080000DF,0,FACEObjDesc(),0.f,WrapAxes);
        CheckBlackSwatch(Material);
        CheckBlackSwatch(Dat->GetWorldObjectMaterial(Material));
        CheckBlackSwatch(Dat->GetUniformInteriorMaterial(Material));
        CheckBlackSwatch(Dat->CreateExaminationMaterial(Material,GI));
    }
    auto* VertexMaterial=Dat->GetVertexColorMaterial();
    auto* VertexInstance=UMaterialInstanceDynamic::Create(VertexMaterial,GI);
    for(UMaterialInterface* Material : {VertexMaterial,static_cast<UMaterialInterface*>(VertexInstance)})
    {
        TestTrue(TEXT("World lighting preserves vertex-color fallback"),Dat->GetWorldObjectMaterial(Material)==Material);
        TestTrue(TEXT("Interior lighting preserves vertex-color fallback"),Dat->GetUniformInteriorMaterial(Material)==Material);
        TestTrue(TEXT("Examination preserves vertex-color fallback"),Dat->CreateExaminationMaterial(Material,GI)==Material);
    }

    // WCID 25284, Chalk Board: no ObjDesc texture/palette replacements. A wall
    // hook retains WCID 9686 and Container while displaying setup 02001002.
    FACEWorldObject Hook; Hook.Guid=0x71002011; Hook.WeenieClassId=9686; Hook.Name=TEXT("Chalk Board");
    Hook.ItemType=ACEItemType::Container; Hook.bIsHouseHook=true;
    Hook.ObjectDescriptionFlags=ACEObjectDescFlag::Openable | ACEObjectDescFlag::Stuck;
    Hook.SetupId=0x02001002; Hook.PlacementId=103; Hook.Scale=.6f;
    Hook.bHasPosition=true; Hook.Position.CellId=0x016C0101;
    auto* Actor=World->SpawnActor<AACEWorldEntityActor>(); Actor->InitializeFromObject(Hook,100,true);
    if(!TestTrue(TEXT("Hooked chalkboard builds"),Actor->Appearance->HasAppearance())) return false;
    const auto Built=Dat->GetOrBuildSetupAppearanceShared(Hook.SetupId,Hook.Appearance,100,Hook.PlacementId);
    if(!TestTrue(TEXT("Chalkboard mesh is available"),Built.IsValid())) return false;
    bool bFoundBlackSurface=false,bFoundTexturedFrame=false;
    for(int32 P=0; P<Built->Parts.Num(); ++P)
    {
        auto* Mesh=Cast<UProceduralMeshComponent>(Actor->Appearance->GetPartMesh(P));
        if(!TestNotNull(TEXT("Chalkboard part mesh exists"),Mesh)) return false;
        int32 SectionIndex=0;
        for(const auto& Section:Built->Parts[P].Sections)
        {
            if(Section.bCollisionOnly) { ++SectionIndex; continue; }
            FACEDatDecodedSurface Decoded;
            TestTrue(TEXT("Chalkboard DAT surface resolves"),Dat->GetTextureResolver()->ResolveSurface(Section.SurfaceId,Decoded));
            auto* Material=Mesh->GetMaterial(SectionIndex++);
            if(Decoded.bIsSolid)
            {
                bFoundBlackSurface=true;
                TestEqual(TEXT("Retail chalkboard surface is black"),Decoded.SolidColor,FLinearColor::Black);
                CheckBlackSwatch(Material);
            }
            else
            {
                bFoundTexturedFrame=true;
                UTexture* Texture=nullptr;
                TestTrue(TEXT("Chalkboard frame retains its texture"),Material &&
                    Material->GetTextureParameterValue(TEXT("ACETexture"),Texture) && Texture &&
                    Texture->GetSurfaceWidth()==Decoded.Width && Texture->GetSurfaceHeight()==Decoded.Height);
            }
        }
    }
    TestTrue(TEXT("Hook displays black board and textured frame"),bFoundBlackSurface && bFoundTexturedFrame);
    if(FApp::CanEverRender())
    {
        auto* View=World->SpawnActor<AActor>();
        auto* Capture=NewObject<USceneCaptureComponent2D>(View); View->AddInstanceComponent(Capture); Capture->RegisterComponent();
        auto* RT=NewObject<UTextureRenderTarget2D>(View);
        RT->ClearColor=FLinearColor(.1f,.05f,.2f,1.f); RT->InitCustomFormat(512,512,PF_FloatRGBA,true); RT->UpdateResourceImmediate(true);
        Capture->TextureTarget=RT; Capture->ProjectionType=ECameraProjectionMode::Orthographic;
        Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false;
        Capture->CaptureSource=ESceneCaptureSource::SCS_SceneColorHDR;
        Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
        Capture->ShowOnlyActorComponents(Actor);
        Capture->ShowFlags.SetAtmosphere(false); Capture->ShowFlags.SetFog(false);
        Capture->ShowFlags.SetBloom(false); Capture->ShowFlags.SetEyeAdaptation(false);
        FBox Bounds; Actor->Appearance->GetVisualWorldBounds(Bounds);
        const FVector Extent=Bounds.GetExtent(),Center=Bounds.GetCenter();
        const FVector Axis=Extent.X<Extent.Y && Extent.X<Extent.Z ? FVector::ForwardVector
            : Extent.Y<Extent.Z ? FVector::RightVector : FVector::UpVector;
        Capture->OrthoWidth=Extent.GetMax()*2.4f;
        const FString Output=FPaths::ProjectSavedDir()/TEXT("Automation/HookedChalkboard");
        IFileManager::Get().MakeDirectory(*Output,true);
        if(GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
        World->SendAllEndOfFrameUpdates();
        for(int32 Side : {-1,1})
        {
            Capture->SetWorldLocationAndRotation(Center+Axis*Side*300,(-Axis*Side).Rotation());
            Capture->CaptureScene(); FlushRenderingCommands();
            TArray<FColor> Pixels; RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            if(!TestEqual(TEXT("Chalkboard render readback succeeds"),Pixels.Num(),512*512)) return false;
            const auto& CenterPixel=Pixels[256*512+256];
            if(Side==-1)
                TestTrue(TEXT("Hooked chalkboard renders black instead of white"),CenterPixel.R<8 && CenterPixel.G<8 && CenterPixel.B<8);
            else
                TestTrue(TEXT("Chalkboard back still renders its texture"),CenterPixel.R>8 && CenterPixel.G>8 && CenterPixel.B>8);
            const auto& FramePixel=Pixels[256*512+56];
            TestTrue(TEXT("Textured frame renders on both sides"),FramePixel.R>8 && FramePixel.G>8 && FramePixel.B>8);
            for(auto& Pixel:Pixels) Pixel.A=255;
            TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(512,512,Pixels,PNG);
            TestTrue(TEXT("Chalkboard render saved"),FFileHelper::SaveArrayToFile(PNG,*(Output/FString::Printf(TEXT("Chalkboard-%d.png"),Side))));
        }
    }
    return true;
}
#endif
