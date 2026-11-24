#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACEDatSubsystem.h"
#include "Engine/GameInstance.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "ShaderCompiler.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPackagingMaterialsTest, "ACE.Packaging.Materials",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEPackagingMaterialsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
	TStrongObjectPtr<UACEDatSubsystem> Dat(NewObject<UACEDatSubsystem>(Instance.Get()));
	const auto Parents = Dat->GetRuntimeMaterialParents();
	TestEqual(TEXT("Complete world/UI/filtered VR widget/pointer/particle/sky/comfort shader set"), Parents.Num(), 46);
	for (const TCHAR* Name : {TEXT("M_ACELandLit_v43"),TEXT("M_ACELegionsSkyAdd_v2"),TEXT("M_ACELegionsSkyAlpha_v1"),TEXT("M_ACESkyColorFill_v8"),
		TEXT("M_ACEFarScenery_v1_0"),TEXT("M_ACEFarScenery_v1_1"),TEXT("M_ACEFarScenery_v1_2"),
		TEXT("M_ACEFarScenery_v1_3"),TEXT("M_ACEFarScenery_v1_4")})
		TestTrue(Name,Parents.ContainsByPredicate([Name](const UMaterialInterface* Parent)
		{
			return Parent && Parent->GetName()==Name;
		}));
	const auto* Horizon = LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/ACE/RuntimeMaterials/M_ACESkyColorFill_v8.M_ACESkyColorFill_v8"));
	float Falloff = -1.f;
	TestTrue(TEXT("Cookable horizon supports a forecast-dependent fade"), Horizon &&
		Horizon->GetScalarParameterValue(TEXT("HorizonFalloff"),Falloff) && Falloff == 0.f);
	TestTrue(TEXT("World pointer material included"), Parents.ContainsByPredicate([](const UMaterialInterface* Parent)
	{
		return Parent && Parent->GetName() == TEXT("M_ACEVRPointerWorld_v1");
	}));
	TestTrue(TEXT("Overlay pointer material included"), Parents.ContainsByPredicate([](const UMaterialInterface* Parent)
	{
		return Parent && Parent->GetName() == TEXT("M_ACEVRPointerOverlay_v1");
	}));
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	for (UMaterialInterface* Parent : Parents)
	{
		if (!TestNotNull(TEXT("Runtime parent loaded"), Parent)) continue;
		const FString Name = Parent->GetName();
		if (Name.Contains(TEXT("OutdoorLit")))
		{
			float Selection=-1.f;
			TestTrue(Name+TEXT(" supports per-object selection without another draw pass"),Parent->GetScalarParameterValue(TEXT("ACESelectionStrength"),Selection));
			TestEqual(Name+TEXT(" is not highlighted by default"),Selection,0.f);
		}
#if !WITH_EDITOR
		TestTrue(Name + TEXT(" is a cooked asset"), Parent->GetPathName().StartsWith(TEXT("/Game/ACE/RuntimeMaterials/")));
		TestFalse(Name + TEXT(" is persistent"), Parent->HasAnyFlags(RF_Transient));
#endif
		const FMaterialResource* Resource = Parent->GetMaterialResource(GMaxRHIShaderPlatform);
		if (!TestNotNull(Name + TEXT(" resource"), Resource)) continue;
		TestNotNull(Name + TEXT(" compiled shader map"), Resource->GetGameThreadShaderMap());
#if WITH_EDITOR
		for (const FString& Error : Resource->GetCompileErrors()) AddError(Name + TEXT(": ") + Error);
#endif
		float Fog = 0;
		if (Parent->GetScalarParameterValue(TEXT("FogAmount"), Fog))
		{
			auto* Mid = UMaterialInstanceDynamic::Create(Parent, Dat.Get());
			Mid->SetScalarParameterValue(TEXT("FogAmount"), .37f);
			TestEqual(Name + TEXT(" runtime fog parameter"), Mid->K2_GetScalarParameterValue(TEXT("FogAmount")), .37f);
		}
	}
	return true;
}
#endif
