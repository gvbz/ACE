#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ACEDatSubsystem.h"
#include "ACETypes.h"
#include "Dat/ACECellTransit.h"
#include "Dat/ACEDatCursor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ShaderCompiler.h"
#include "ImageUtils.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEYaraqSceneTest, "ACE.RetailParity.YaraqScene",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACEYaraqSceneTest::RunTest(const FString& Parameters)
{
    auto* Dat = NewObject<UACEDatSubsystem>(NewObject<UGameInstance>());
    if (!Dat->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call"))) return false;
    for (uint32 SurfaceId : {0x0800073Bu,0x08000725u})
    {
        FACEDatDecodedSurface Surface;
        TestTrue(TEXT("Pedestal DAT surface decodes"),Dat->GetTextureResolver()->ResolveSurface(SurfaceId,Surface));
        TestTrue(TEXT("Pedestal textured faces contain source pixels"),Surface.bHasPixels);
        if (Surface.bHasPixels)
        {
            TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Surface.Width,Surface.Height,Surface.Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("Automation/RetailParity/Surface_%08X.png"),SurfaceId)));
        }
    }
    // Warm material initialization before retaining any mesh-cache pointers.
    Dat->GetOrCreateTexturedMaterial(0x08000708);
    // Inspect the authored exterior portal surfaces around the reported Yaraq tower.
    for (uint32 X=0x7D; X<=0x7F; ++X) for (uint32 Y=0x64; Y<=0x66; ++Y)
    {
        FACEDatLandblockInfo Info;
        if (!Dat->LoadLandblockInfo((X<<24)|(Y<<16),Info)) continue;
        for (const auto& Building : Info.Buildings)
        {
            TArray<uint8> Blob;
            if ((Building.ModelId>>24)!=1 || !Dat->GetPortalDat()->ReadFile(Building.ModelId,Blob)) continue;
            FACEDatCursor Cur(Blob); FACEDatGfxObj Gfx;
            if (!ACEDatUnpack::UnpackGfxObj(Cur,Gfx)) continue;
            for (const auto& Entry : Gfx.DrawingPortalIndices)
            {
                const auto* Poly=Gfx.Polygons.Find(Entry.Value);
                if (!Poly || !Gfx.Surfaces.IsValidIndex(Poly->PosSurface)) continue;
                const uint32 SurfaceId=Gfx.Surfaces[Poly->PosSurface];
                FACEDatDecodedSurface S;
                if (!Dat->GetTextureResolver()->ResolveSurface(SurfaceId,S)) continue;
                AddInfo(FString::Printf(TEXT("PortalSurface block=%02X%02X model=%08X origin=%s portal=%d poly=%d surface=%08X solid=%d color=%s transparent=%d alpha=%d"),
                    X,Y,Building.ModelId,*FVector(Building.Origin).ToString(),Entry.Key,Entry.Value,SurfaceId,S.bIsSolid,*S.SolidColor.ToString(),S.bFullyTransparent,S.bUsesAlpha));
            }
        }
    }
    const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .RequiresHitProxies(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
    AActor* Owner = World->SpawnActor<AActor>();
    TMap<uint32, UProceduralMeshComponent*> Cells;
    TMultiMap<uint32, UProceduralMeshComponent*> Furniture;
    auto BlockOrigin = [](uint32 Id) { return FACEPosition::AceVectorToUnreal(
        FVector(((Id>>24)&255)*192,((Id>>16)&255)*192,0),100); };
    TArray<uint32> CellIds;
    for (uint32 Id=0x7D630100; Id<=0x7D630119; ++Id) CellIds.Add(Id);
    for (uint32 Id=0x7D640100; Id<0x7D640174; ++Id) CellIds.Add(Id);
    for (uint32 Id=0x7E650100; Id<=0x7E65010A; ++Id) CellIds.Add(Id);
    for (uint32 Id : CellIds)
    {
        const auto* Built = Dat->GetOrBuildEnvCellMesh(Id,100);
        if (!TestNotNull(TEXT("Yaraq shop DAT cell"),Built)) continue;
        const FACEBuiltEnvCellMesh Mesh = *Built;
        if (Id>=0x7E650100 && Id<=0x7E650106)
            for (const auto& Section : Mesh.Sections)
                TestFalse(TEXT("Interior tower portal cannot draw its opaque blue cover"),
                    Section.SurfaceId==0x080006FD && !Section.bCollisionOnly && !Section.IsEmpty());
        auto* Comp = NewObject<UProceduralMeshComponent>(Owner);
        Comp->RegisterComponent();
        Comp->SetWorldTransform(Mesh.GetCellLocalToLandblock(100)*FTransform(BlockOrigin(Id)));
        int32 Index = 0;
        for (const auto& Section : Mesh.Sections)
        {
            Comp->CreateMeshSection_LinearColor(Index,Section.Vertices,Section.Triangles,Section.Normals,
                Section.UVs,Section.VertexColors,{},false);
            auto* Material = Dat->GetOrCreateTexturedMaterial(Section.SurfaceId);
            Comp->SetMaterial(Index++, Material);
        }
        Cells.Add(Id,Comp);
        for (const auto& Stab : Mesh.StaticObjects)
        {
            auto* Prop=NewObject<UProceduralMeshComponent>(Owner);Prop->RegisterComponent();
            Prop->SetWorldTransform(FTransform(FACEPosition::AceQuatToUnreal(FQuat(Stab.Orientation)),
                BlockOrigin(Id)+FACEPosition::AceVectorToUnreal(FVector(Stab.Origin),100)));
            Dat->ApplySetupToProceduralMesh(Prop,Stab.Id,100,false,UACEDatSubsystem::ACEPlacementResting);
            Furniture.Add(Id,Prop);
        }
    }
    // Include the exterior shell: an all-interiors-only reference incorrectly
    // sees upper rooms through the back of their enclosing exterior wall.
    for (uint32 Block : {0x7D630000u,0x7D640000u,0x7E650000u})
    {
    FACEDatLandblockInfo Info;
    Dat->LoadLandblockInfo(Block,Info);
    for (const auto& Building : Info.Buildings)
    {
        const auto* Built = Dat->GetOrBuildSetupMesh(Building.ModelId,100);
        if (!Built) continue;
        int32 Portals=0; for (const auto& Part:Built->Parts) Portals+=Part.Portals.Num();
        AddInfo(FString::Printf(TEXT("Building %08X model=%08X ports=%d refs=%d"),Block,Building.ModelId,Portals,Building.Portals.Num()));
        if (Building.ModelId==0x010014C3)
        {
            TestEqual(TEXT("Blue doorway retains both retail portal apertures"),Portals,2);
            for (const auto& Part:Built->Parts) for (const auto& Section:Part.Sections)
                TestFalse(TEXT("Opaque blue portal fill is not a drawable wall"),Section.SurfaceId==0x080006FD && !Section.bCollisionOnly && !Section.IsEmpty());
        }
        auto* Shell = NewObject<UProceduralMeshComponent>(Owner); Shell->RegisterComponent();
        Shell->SetWorldTransform(FTransform(FACEPosition::AceQuatToUnreal(FQuat(Building.Orientation)),
            BlockOrigin(Block)+FACEPosition::AceVectorToUnreal(FVector(Building.Origin),100)));
        Dat->ApplySetupToProceduralMesh(Shell,Building.ModelId,100,false);

    }
    }
    // The all-resident reference also needs outdoor terrain. Otherwise it exposes
    // the pedestal's buried rooms (7D640150/151) through the ground and falsely
    // requires the portal traversal to draw them from the shop across the street.
    for (uint32 Block : {0x7D630000u,0x7D640000u,0x7E650000u})
    {
        auto* Terrain = NewObject<UProceduralMeshComponent>(Owner); Terrain->RegisterComponent();
        Terrain->SetWorldLocation(BlockOrigin(Block));
        Dat->GetOrBuildLandblockMesh(Block,100);
        TestTrue(TEXT("Yaraq reference includes the authored outdoor heightfield"),
            Dat->ApplyLandblockToProceduralMesh(Terrain,Block,100));
    }
    if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
    USceneCaptureComponent2D* Capture = nullptr;
    UTextureRenderTarget2D* Target = nullptr;
    if (FApp::CanEverRender())
    {
        Target = NewObject<UTextureRenderTarget2D>(Owner);
        Target->ClearColor = FLinearColor::Black;
        Target->InitCustomFormat(320,240,PF_B8G8R8A8,false);
        Capture = NewObject<USceneCaptureComponent2D>(Owner);
        Capture->TextureTarget = Target; Capture->FOVAngle = 90;
        Capture->bCaptureEveryFrame = false; Capture->bCaptureOnMovement = false;
        Capture->CaptureSource = SCS_FinalColorLDR;
        Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetBloom(false);
        Capture->ShowFlags.SetAtmosphere(false); Capture->ShowFlags.SetAntiAliasing(false);
        Capture->RegisterComponent();
    }
    int32 Views = 0;
    for (uint32 Id : {0x7D630100u,0x7D630104u,0x7D63010Du,0x7D63010Eu,0x7D630112u,0x7D64012Cu,0x7D640131u,0x7D640135u,
        0x7E650100u,0x7E650101u,0x7E650102u,0x7E650103u,0x7E650104u,0x7E650105u,0x7E650106u,0x7E650107u,0x7E650108u,0x7E650109u,0x7E65010Au})
    {
        const auto* Mesh = Dat->FindEnvCellMesh(Id,100);
        if (!Mesh) continue;
        const FTransform Xform = Mesh->GetCellLocalToLandblock(100)*FTransform(BlockOrigin(Id));
        FVector LocalEye = (Mesh->LocalBoundsMin + Mesh->LocalBoundsMax)*0.5;
        const FVector CenterEye = Xform.TransformPosition(LocalEye);
        TestTrue(*FString::Printf(TEXT("%08X camera lies inside its CellBSP"),Id),
            ACECellTransit::SphereIntersectsEnvCell(*Dat,Id,CenterEye,0,100));
        // The camera may be in a neighboring room while the player's feet remain
        // on a floor/portal boundary. Test the real viewer resolver, not just PView
        // with a preselected (and therefore always correct) camera cell.
        if ((Id >> 16) == 0x7E65)
        {
            for (const auto& Portal : Mesh->CellPortals)
            {
                if (Portal.OtherCellId < 0x100 || Portal.OtherCellId == 0xFFFF) continue;
                const uint32 PlayerCell = (Id & 0xFFFF0000u) | Portal.OtherCellId;
                const auto* PlayerMesh = Dat->FindEnvCellMesh(PlayerCell,100);
                if (!PlayerMesh) continue;
                const FTransform PlayerFrame = PlayerMesh->GetCellLocalToLandblock(100)*FTransform(BlockOrigin(PlayerCell));
                const FVector Feet = PlayerFrame.TransformPosition(FVector(0,0,1));
                uint32 Viewer = 0;
                ACECellTransit::ResolveViewerCellId(*Dat,PlayerCell,Feet,CenterEye,100,Viewer);
                TestTrue(*FString::Printf(TEXT("Camera in %08X with feet in %08X resolves to a containing cell (got %08X)"),Id,PlayerCell,Viewer),
                    Dat->IsPointInsideEnvCell(Viewer,CenterEye,100));
            }
        }
        TArray<TPair<FVector,FRotator>> CameraSamples;
        for (int32 Yaw=0; Yaw<360; Yaw+=90) CameraSamples.Add({CenterEye,FRotator(0,Yaw,0)});
        if ((Id>>16)==0x7E65)
        for (int32 PortalIndex=0; PortalIndex<Mesh->CellPortals.Num(); ++PortalIndex)
        {
            if (!Mesh->PortalApertureLocalVerts.IsValidIndex(PortalIndex) || Mesh->PortalApertureLocalVerts[PortalIndex].Num()<3) continue;
            FVector Center=FVector::ZeroVector;
            for (const auto& V:Mesh->PortalApertureLocalVerts[PortalIndex]) Center+=V;
            Center/=Mesh->PortalApertureLocalVerts[PortalIndex].Num();
            const FVector Normal=Mesh->PortalApertureLocalNormals[PortalIndex];
            for (double Distance:{2.,32.})
            {
                const FVector Camera=Xform.TransformPosition(Center-Normal*Distance);
                if (!Dat->IsPointInsideEnvCell(Id,Camera,100)) continue;
                CameraSamples.Add({Camera,Xform.TransformVectorNoScale(Normal).Rotation()});
            }
        }
        for (int32 Sample=0; Sample<CameraSamples.Num(); ++Sample)
        {
            const int32 Yaw=Sample;
            const FVector Eye=CameraSamples[Sample].Key;
            const FRotator Rotation=CameraSamples[Sample].Value;
            const FMatrix Axis(FPlane(0,0,1,0),FPlane(1,0,0,0),FPlane(0,1,0,0),FPlane(0,0,0,1));
            const FMatrix View = FTranslationMatrix(-Eye)*FInverseRotationMatrix(Rotation)*Axis;
            FConvexVolume Frustum;
            GetViewFrustumBounds(Frustum,View*FReversedZPerspectiveMatrix(PI/4,4.0/3.0,10.0,10.0),true);
            TSet<int32> Draw; bool bOutside = false;
            TArray<ACEOutdoorPortalPlan::FAdmittedAperture> Exits;
            ACEOutdoorPortalPlan::CollectIndoorPViewCells(*Dat,Id,Eye,Frustum,100,Draw,bOutside,128,&Exits);
            Dat->SetLandLookOutClip(true,Eye,Exits);
            TestTrue(TEXT("Occupied Yaraq cell remains in the draw set"),Draw.Contains(Id));
            if (!Capture) continue;
            Capture->SetWorldLocationAndRotation(Eye,Rotation);
            TArray<FColor> Reference, Culled;
            auto Render = [&](bool bCull,TArray<FColor>& Pixels)
            {
                for (const auto& Pair : Cells) Pair.Value->SetVisibility(!bCull || Draw.Contains(Pair.Key));
                for (const auto& Pair : Furniture) Pair.Value->SetVisibility(!bCull || Draw.Contains(Pair.Key));
                World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
                Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            };
            Render(false,Reference); Render(true,Culled);
            int32 Lost = 0, Opaque = 0;
            for (int32 I=0; I<Reference.Num() && I<Culled.Num(); ++I)
            {
                const bool bWasDrawn = FMath::Max3(Reference[I].R,Reference[I].G,Reference[I].B)>24;
                if (bWasDrawn) ++Opaque;
                if (bWasDrawn && FMath::Max3(Culled[I].R,Culled[I].G,Culled[I].B)<8) ++Lost;
            }
            AddInfo(FString::Printf(TEXT("Yaraq %08X yaw=%d cells=%d outside=%d missing=%d/%d eye=%s"),Id,Yaw,Draw.Num(),bOutside,Lost,Opaque,*LocalEye.ToString()));
            // Ignore a one-pixel raster edge along the exterior shell. The
            // all-cells reference deliberately overdraws rooms behind that shell.
            int32 MissingArea = 0;
            for (int32 Y=1; Y<239; ++Y) for (int32 X=1; X<319; ++X)
            {
                const int32 I=Y*320+X;
                if (FMath::Max3(Reference[I].R,Reference[I].G,Reference[I].B)<=24) continue;
                bool bHole=true;
                for (int32 DY=-1; DY<=1; ++DY) for (int32 DX=-1; DX<=1; ++DX)
                {
                    const FColor C=Culled[I+DY*320+DX];
                    bHole &= FMath::Max3(C.R,C.G,C.B)<8;
                }
                if (bHole) ++MissingArea;
            }
            TestEqual(*FString::Printf(TEXT("%08X yaw %d portal view preserves room walls"),Id,Yaw),MissingArea,0);
            for (int32 Pass=0; Pass<2; ++Pass)
            {
                TArray64<uint8> Png;
                FImageUtils::PNGCompressImageArray(320,240,Pass ? Culled : Reference,Png);
                const FString Path = FPaths::ProjectSavedDir()/FString::Printf(
                    TEXT("Automation/RetailParity/Yaraq_%08X_%d_%s.png"),Id,Yaw,Pass ? TEXT("portal") : TEXT("all"));
                IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
                FFileHelper::SaveArrayToFile(Png,*Path);
            }
            ++Views;
        }
    }
    AddInfo(FString::Printf(TEXT("Rendered %d Yaraq room views against all resident authored geometry"),Views));
    Dat->GetOrBuildSetupMesh(0x01002A1B,100);
    auto* Pedestal=Dat->GetOrCreateSetupStaticMesh(0x01002A1B,100,false,0,false,true);
    if (TestNotNull(TEXT("Yaraq pedestal uses the runtime building mesh path"),Pedestal))
    {
        auto* Component=NewObject<UStaticMeshComponent>(Owner); Component->SetStaticMesh(Pedestal);
        Dat->BindSetupStaticMeshMaterials(Component,0x01002A1B,100,false,0,false,true);
        const auto* PedestalData=Dat->GetOrBuildSetupMesh(0x01002A1B,100);
        TSet<uint32> PedestalSurfaces;
        int32 MaterialSections=0;
        if(PedestalData)for(const auto& Part:PedestalData->Parts)for(const auto& Section:Part.Sections)
            if(!Section.bCollisionOnly){PedestalSurfaces.Add(Section.SurfaceId);++MaterialSections;}
        TestEqual(TEXT("Pedestal retains both textured surfaces without its PORT fill"),PedestalSurfaces.Num(),2);
        TestEqual(TEXT("Wrapped and clamped uses retain separate material slots"),Component->GetNumMaterials(),MaterialSections);
        TestTrue(TEXT("Pedestal retains its black portal aperture for PView"),PedestalData && !PedestalData->Parts[0].Portals.IsEmpty());
        for (int32 I=0; I<Component->GetNumMaterials(); ++I)
        {
            auto* Material=Cast<UMaterialInstanceDynamic>(Component->GetMaterial(I));
            TestTrue(TEXT("Pedestal material slot has its DAT texture"),Material &&
                Material->K2_GetTextureParameterValue(TEXT("ACETexture")) != nullptr);
        }
    }
    const auto* AmbientCell=Dat->GetOrBuildEnvCellMesh(0x7E650100,100);
    if (AmbientCell && !AmbientCell->Sections.IsEmpty())
    {
        auto* Material=Cast<UMaterialInstanceDynamic>(Dat->GetOrCreateEnvCellMaterial(AmbientCell->Sections[0].SurfaceId));
        if (TestNotNull(TEXT("Room ambient fixture has a material"),Material))
        {
            const FLinearColor Outside(.43f,.32f,.21f);
            Dat->SetInteriorAmbient(Outside); Dat->SetInteriorUsesOutdoorAmbient(false);
            TestTrue(TEXT("Outdoor-connected shop retains its authored presentation when the player is inside"),Material->K2_GetVectorParameterValue(TEXT("InteriorAmbient")).Equals(FLinearColor::White,.001f));
            Dat->SetInteriorUsesOutdoorAmbient(true);
            TestTrue(TEXT("Opening to outdoors preserves the same interior illumination"),Material->K2_GetVectorParameterValue(TEXT("InteriorAmbient")).Equals(FLinearColor::White,.001f));
        }
    }
    World->DestroyWorld(false);
    return true;
}
#endif
