#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACEDatSubsystem.h"
#include "ACEEnvCellActor.h"
#include "Dat/ACEEnvCellMeshBuilder.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEInteriorSamplingTest,"ACE.RetailParity.InteriorSampling",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEInteriorSamplingTest::RunTest(const FString&)
{
 const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
  .CreateNavigation(false).CreateAISystem(false);
 auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
 auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);
 auto* GI=NewObject<UGameInstance>(GEngine);World->SetGameInstance(GI);
 Context.OwningGameInstance=GI;Context.SetCurrentWorld(World);GI->Init();
 ON_SCOPE_EXIT { GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false); };
 auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
 if(!Dat->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call")))return false;
 int32 RepeatingSections=0,Ceilings=0;
 // Walls from the reported Town Network room, plus its one-polygon ceiling caps.
 for(uint32 Id:{0x00070127u,0x00070136u,0x00070159u,0x000701A6u,0x000701A7u})
 {
  const auto* Mesh=Dat->GetOrBuildEnvCellMesh(Id,100);
  if(!TestNotNull(TEXT("Town Network sampling fixture loads"),Mesh))continue;
  auto* Room=World->SpawnActor<AACEEnvCellActor>();
  TestTrue(TEXT("Actual room uploads its split draw sections"),Room->LoadEnvCell(Id,FVector(0,134400,0),100));
  TSet<UMaterialInterface*> Expected;
  const auto Actual=Room->CellMesh->GetMaterials();
  const auto SameSampler=[](UMaterialInterface* A,UMaterialInterface* B)
  {
   if(A==B)return true;
   if(!A||!B||A->GetMaterial()!=B->GetMaterial())return false;
   UTexture *TextureA=nullptr,*TextureB=nullptr;
   return A->GetTextureParameterValue(TEXT("ACETexture"),TextureA)
    && B->GetTextureParameterValue(TEXT("ACETexture"),TextureB) && TextureA==TextureB;
  };
  for(const auto& Section:Mesh->Sections)
  {
   if(Section.IsEmpty() || Section.bFullyTransparent)continue;
   auto* Material=Dat->GetOrCreateEnvCellMaterial(Section.SurfaceId,Section.bWrapTexture,!Mesh->CanSeeOutside());
   if(!Material)Material=Dat->GetVertexColorMaterial();
   Expected.Add(Material);
   if(Section.bWrapTexture)++RepeatingSections;
   for(const auto& Normal:Section.Normals)if(Normal.Z<-.35f){++Ceilings;break;}
   // Entrance lighting can instance the material per room. Verify what is
   // sampled, including the wrap-specific texture, independently of identity.
   TestTrue(TEXT("Floor/wall/ceiling split preserves the authored material sampler"),
    Actual.ContainsByPredicate([&](auto* Candidate){return SameSampler(Material,Candidate);}));
  }
  for(auto* Material:Actual)
  {
   bool Found=false;
   for(auto* Candidate:Expected)if(SameSampler(Material,Candidate)){Found=true;break;}
   TestTrue(TEXT("Every emitted room section uses a source sampler variant"),Found);
  }
  AddInfo(FString::Printf(TEXT("Town Network %08X: %d source sections, %d rendered sections"),Id,Mesh->Sections.Num(),Room->CellMesh->GetNumSections()));
  Room->Destroy();
 }
 TestTrue(TEXT("Fixture covers repeated walls and floors"),RepeatingSections>0);
 TestTrue(TEXT("Fixture covers the separate ceiling bucket"),Ceilings>0);
 return !HasAnyErrors();
}
#endif
