#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACEVisibleObjectPick.h"
#include "Mods/ACEPluginSight.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEDatSubsystem.h"
#include "Dat/ACELandblockMeshBuilder.h"
#include "Components/BoxComponent.h"
#include "Engine/DirectionalLight.h"
#include "Components/LightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ShaderCompiler.h"
#include "Materials/MaterialInterface.h"
#include "AssetCompilingManager.h"
#include "MaterialShaderPrecompileMode.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaterTargetTest, "ACE.RetailParity.SubmergedMoar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACEWaterTargetTest::RunTest(const FString&)
{
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game); Context.SetCurrentWorld(World);
    auto* GI = NewObject<UGameInstance>(GEngine); World->SetGameInstance(GI); Context.OwningGameInstance=GI;
    GI->OnWorldChanged(nullptr,World); GI->Init();
    ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
    auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
    if (!TestTrue(TEXT("Retail DAT opens"), Dat && Dat->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call")))) return false;
    constexpr uint32 Block = 0xDC560000;
    const auto* Built = Dat->GetOrBuildLandblockMesh(Block, 100);
    if (!TestNotNull(TEXT("Wading landblock"), Built)) return false;
    auto* Land = World->SpawnActor<AACELandblockActor>();
    auto* Mesh = Land->TerrainMesh.Get();
    const FVector Origin = FACEPosition::AceVectorToUnreal(FVector(220*192,86*192,0),100);
    Land->SetActorLocation(Origin); Mesh->bUseAsyncCooking = false;
    auto* LandShader=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/ACE/RuntimeMaterials/M_ACELandLit_v43.M_ACELandLit_v43"));
    if (!TestNotNull(TEXT("Packaged water-depth shader is available"),LandShader)) return false;
    auto* PreviewParent=Dat->EnsureAceLandMaterialBase();
    Dat->LandMaterialBase=LandShader;
    // A cold editor shader cache can substitute WorldGridMaterial for a loaded
    // asset. Compile the on-disk parent before comparing its water depth on GPU.
    LandShader->CacheShaders(EMaterialShaderPrecompileMode::Synchronous);
    LandShader->EnsureIsComplete();
    for (const auto& Sec : Built->Sections)
    {
        const int32 Index = Mesh->GetNumSections();
        Mesh->CreateMeshSection(Index, Sec.Vertices, Sec.Triangles, Sec.Normals, Sec.UVs, {}, {}, true);
        auto* MID=Cast<UMaterialInstanceDynamic>(Dat->GetOrCreateLandMaterial(Sec.PCode,Sec.BakedPixels,Sec.BakeWidth,Sec.BakeHeight,Sec.SharedBake));
        if (!TestNotNull(TEXT("Retail terrain material"),MID)) return false;
        TestEqual(TEXT("Packaged parent used by the terrain factory"),MID->Parent.Get(),LandShader);
        Mesh->SetMaterial(Index,MID);
        const auto Depths=FACELandblockMeshBuilder::GetWaterDepthsAc(Sec.PCode)*100.f;
        TestTrue(TEXT("Packaged shader receives DAT wading depth"),MID->K2_GetVectorParameterValue(TEXT("WaterBedDepthCm")).Equals(Depths));
    }
    FVector Feet;
    int32 Cell = INDEX_NONE;
    for (int32 I=0; I<64 && Cell==INDEX_NONE; ++I)
    {
        if (Built->CellWaterType[I] != 2) continue;
        Feet = Origin + FVector(-((I/8)*24+12)*100,((I%8)*24+12)*100,0);
        float Z; FVector N;
        if (Dat->SampleOutdoorGroundZ(Feet.X,Feet.Y,100,Z,&N) && N.Z>.999)
        { Feet.Z=Z; Cell=I; }
    }
    if (!TestTrue(TEXT("Flat, 90 cm deep water in a coastal block"), Cell!=INDEX_NONE && Built->BlockWaterType==1)) return false;

    FACEWorldObject Obj; Obj.Guid=0x7100a5a0; Obj.bHasPosition=true; Obj.ItemType=ACEItemType::Creature;
    Obj.Name=TEXT("Pallid Moar"); Obj.WeenieClassId=34869; Obj.SetupId=0x0200149F; Obj.MotionTableId=0x09000192;
    Obj.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
    float Step=0,Height=0,Radius=0; uint32 Anim=0;
    if (!TestTrue(TEXT("Moar has retail physics dimensions"),Dat->TryGetSetupPhysics(Obj.SetupId,Step,Height,Radius,Anim) && Height>0)) return false;
    Obj.Scale=1.f; // stock Pallid Moar
    Obj.Position.CellId=Block | uint32(Cell+1); Obj.Position.SetLocationFromUnreal(Feet);
    auto* Body=World->SpawnActor<AACEWorldEntityActor>(); Body->InitializeFromObject(Obj,100,true);
    Body->SetActorLocation(Feet);
    FBox Bounds(ForceInit);
    if (!TestTrue(TEXT("Actual Moar drawing mesh is loaded"),Body->Appearance->GetVisualWorldBounds(Bounds))) return false;
    const FVector Eye=Feet+FVector(0,-240,180), Aim=Feet+FVector(0,0,Height*Obj.Scale*100*2/3);
    const FVector Center=Bounds.GetCenter();
    TestTrue(TEXT("Aim and visible mesh are below the surface"),Aim.Z<Feet.Z+90 && Bounds.Max.Z<Feet.Z+90);
    FACEPluginSightQuery Sight(*World,nullptr);
    TestFalse(TEXT("Unrefined collision hits the original water sheet"),Sight.Clear(Eye,Aim,15));
    TestTrue(TEXT("Combat corridor reaches the submerged aim point"),Sight.Clear(Eye,Aim,15,true));
    FACEPosition Source=Obj.Position;
    Source.SetLocationFromUnreal(Feet+FVector(0,-240,0));
    GI->GetSubsystem<UACEClientSubsystem>()->GetSession()->SetLocalPosition(Source);
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    TestTrue(TEXT("The live plugin attack and cast gate accepts the submerged Moar"),Host->HasClearCombatSight(Obj));
    TestFalse(TEXT("Solid floor still blocks a shot below the bed"),Sight.Clear(Eye,Feet-FVector(0,0,10),15,true));
    TestTrue(TEXT("A clear underwater sight line remains usable"),Sight.Clear(Feet+FVector(0,-80,30),Aim,10,true));
    TestFalse(TEXT("An underwater ray cannot continue into solid bed"),Sight.Clear(Feet+FVector(0,-80,30),Feet-FVector(0,0,10),10,true));
    // Start just above the middle of the wet cell. The first heightfield hit is
    // water; a second, dry cell in the very same mesh must still stop the ray.
    bool CheckedBank=false;
    const FVector WaterStart=Feet+FVector(100,0,92);
    for (int32 I=0; I<64 && !CheckedBank; ++I)
    {
        if (Built->CellWaterType[I]!=0) continue;
        FVector Bank=Origin+FVector(-((I/8)*24+12)*100,((I%8)*24+12)*100,0);
        float Ground=0;
        if (!Dat->SampleOutdoorGroundZ(Bank.X,Bank.Y,100,Ground)) continue;
        Bank.Z=FMath::Min<double>(Ground-20,Feet.Z+35);
        FHitResult Hit;
        if (!World->LineTraceSingleByChannel(Hit,WaterStart,Bank,ECC_Camera) || Hit.GetComponent()!=Mesh) continue;
        double SolidDistance;
        if (!ACEWaterSight::RefineTerrainHit(*World,Hit,WaterStart,Bank,0,SolidDistance)) continue;
        CheckedBank=true;
        TestTrue(TEXT("A dry bank beyond the water on the same component still occludes"),SolidDistance<FVector::Distance(WaterStart,Bank));
        TestFalse(TEXT("Plugin cannot shoot through the bank"),Sight.Clear(WaterStart,Bank,0,true));
    }
    TestTrue(TEXT("Exercised a water intersection followed by a dry bank"),CheckedBank);
    const FVector End=Center+(Center-Eye).GetSafeNormal()*160;
    TestEqual(TEXT("Manual desktop/VR pick reaches the submerged creature"),ACEVisibleObjectPick::Trace(*World,Eye,End,nullptr),static_cast<AActor*>(Body));

    auto* Wall=World->SpawnActor<AActor>(); auto* Box=NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(Box); Box->SetBoxExtent(FVector(100,8,60));
    Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly); Box->SetCollisionResponseToAllChannels(ECR_Block);
    Box->RegisterComponent(); Wall->SetActorLocation(FMath::Lerp(Eye,Aim,.75));
    TestFalse(TEXT("A submerged obstruction still blocks combat"),Sight.Clear(Eye,Aim,15,true));
    TestFalse(TEXT("The plugin dispatch gate still rejects an obstructed Moar"),Host->HasClearCombatSight(Obj));
    TestNull(TEXT("A submerged obstruction still blocks selection"),ACEVisibleObjectPick::Trace(*World,Eye,End,nullptr));
    Wall->Destroy();

    if (GUsingNullRHI) return true;
    auto* Sun=World->SpawnActor<ADirectionalLight>(); Sun->SetActorRotation(FRotator(-55,45,0));
    Sun->GetLightComponent()->SetIntensity(8.f);
    auto* Capture=NewObject<USceneCaptureComponent2D>(World->SpawnActor<AActor>());
    auto* RT=NewObject<UTextureRenderTarget2D>(Capture); RT->TargetGamma=2.2f;
    RT->InitCustomFormat(256,192,PF_B8G8R8A8,true); RT->UpdateResourceImmediate(true);
    Capture->TextureTarget=RT; Capture->CaptureSource=SCS_FinalColorLDR; Capture->FOVAngle=45;
    Capture->bCaptureEveryFrame=false; Capture->bCaptureOnMovement=false;
    Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetAntiAliasing(false); Capture->ShowFlags.SetFog(false);
    Capture->RegisterComponent(); Capture->SetWorldLocationAndRotation(Eye,(Center-Eye).Rotation());
    World->SendAllEndOfFrameUpdates();
    UMaterialInterface::SubmitRemainingJobsForWorld(World);
    FAssetCompilingManager::Get().FinishAllCompilation();
    if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
    const auto Frame=[&](bool Hide)
    {
        Body->SetActorHiddenInGame(Hide); World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
        TArray<FColor> Pixels; RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels); return Pixels;
    };
    Frame(true); // Warm both terrain textures and capture resources before comparing frames.
    const auto Without=Frame(true), With=Frame(false);
    for (int32 I=0; I<Mesh->GetNumMaterials(); ++I)
        if (auto* M=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(I)))
            M->SetVectorParameterValue(TEXT("WaterBedDepthCm"), FLinearColor::Transparent);
    const auto Opaque=Frame(false), EmptyOpaque=Frame(true);
    const auto Difference=[](const TArray<FColor>& A,const TArray<FColor>& B)
    {
        int32 Changed=0;
        for (int32 I=0; I<A.Num() && I<B.Num(); ++I)
            if (FMath::Abs(int32(A[I].R)-B[I].R)+FMath::Abs(int32(A[I].G)-B[I].G)+FMath::Abs(int32(A[I].B)-B[I].B)>12) ++Changed;
        return Changed;
    };
    Dat->LandMaterialBase=PreviewParent;
    Dat->LandMaterialCache.Reset();
    for (int32 I=0; I<Built->Sections.Num(); ++I)
    {
        const auto& Sec=Built->Sections[I];
        Mesh->SetMaterial(I,Dat->GetOrCreateLandMaterial(Sec.PCode,Sec.BakedPixels,Sec.BakeWidth,Sec.BakeHeight,Sec.SharedBake));
    }
    const auto PreviewEmpty=Frame(true), Preview=Frame(false);
    TestTrue(TEXT("The editor preview also renders the submerged model"),Difference(Preview,PreviewEmpty)>25);
    const int32 OldPixels=Difference(Opaque,EmptyOpaque), NewPixels=Difference(With,Without);
    AddInfo(FString::Printf(TEXT("WCID 34869: %.2f m setup, tested at scale %.3f, cell %08X, %d opaque vs %d corrected creature pixels"),Height,Obj.Scale,uint32(Obj.Position.CellId),OldPixels,NewPixels));
    TestTrue(TEXT("Opaque surface reproduces the invisible target"),OldPixels<5);
    TestTrue(TEXT("Submerged Moar changes visible color pixels against the actual water texture"),NewPixels>25);
    FString Report;
    if (FParse::Value(FCommandLine::Get(),TEXT("ReportExportPath="),Report) && With.Num()==256*192 && Opaque.Num()==With.Num())
    {
        TArray<FColor> Pair;
        for (int32 Y=0; Y<192; ++Y)
        {
            Pair.Append(Opaque.GetData()+Y*256,256); Pair.Append(With.GetData()+Y*256,256);
        }
        IFileManager::Get().MakeDirectory(*Report,true);
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(512,192,Pair,PNG);
        FFileHelper::SaveArrayToFile(PNG,*(Report/TEXT("SubmergedMoar.png")));
    }
    return true;
}
#endif
