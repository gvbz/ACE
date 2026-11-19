#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalFarScenery.h"
#include "ACEDatSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ProceduralMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "ImageUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarSceneryFadeTest, "ACE.Personal.FarScenery.Fade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarSceneryFadeTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFarScenery;
	TestEqual(TEXT("A radius of eight leaves a cell inside its invisible prefetched ring"), FadeRadius(8, 19200.f), 151200.f);
	for (int32 Radius : {2, 3, 8, LastRing})
	{
		const int32 Prepared = PlacementRing(Radius);
		TestTrue(TEXT("Prepared ring remains within supported placement data"), Prepared <= LastRing);
		if (Radius < LastRing)
			TestTrue(TEXT("Even the minimum radius retains meaningful handoff coverage"), FadeRadius(Radius, 19200.f) > NearEdge(Radius, 19200.f));
		// The old prepared rectangle extends from -Prepared*B to
		// (Prepared+1)*B. Move one full block, then sweep its local position;
		// a 12m camera boom still stays inside invisible old coverage.
		for (float Local : {0.f, 9600.f, 19200.f})
		{
			const float Camera = 19200.f + Local + 1200.f;
			const float OldEdge = (Prepared + 1) * 19200.f;
			TestTrue(TEXT("Async one-block movement and boom cannot expose old prepared edge"), FadeRadius(Radius, 19200.f) <= OldEdge - Camera);
		}
	}
	TestEqual(TEXT("Full size coverage before the fade band"), FadeAmount(799.f, 1000.f), 1.f);
	TestTrue(TEXT("Mid-band size coverage is partial"), FMath::IsNearlyEqual(FadeAmount(900.f, 1000.f), .5f));
	TestEqual(TEXT("Size coverage reaches zero before removal"), FadeAmount(1000.f, 1000.f), 0.f);
	TestTrue(TEXT("Hardware culling waits for the visible bounds"), CullDistance(1000.f, 120.f) > 1000.f);
	TestEqual(TEXT("Huge objects can opt out of size culling"), FadeAmount(10000.f, 0.f), 1.f);

	const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
		.CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
	auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
	auto* GI = NewObject<UGameInstance>(GEngine);
	GI->InitializeStandalone();
	auto* InitialWorld = GI->GetWorld();
	GI->GetWorldContext()->SetCurrentWorld(World);
	World->SetGameInstance(GI);
	InitialWorld->DestroyWorld(false);
	ON_SCOPE_EXIT { GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
	UMaterialInterface* Originals[] = {Dat->EnsureAceOutdoorLitMaterialBase(), Dat->EnsureAceOutdoorLitMaskedMaterialBase(), Dat->EnsureAceBuildingShellMaterialBase()};
	TArray<UMaterialInstanceDynamic*> Faded;
	for (UMaterialInterface* Original : Originals)
	{
		if (!TestNotNull(TEXT("Original material exists"), Original)) return false;
		const EBlendMode Blend = Original->GetBlendMode();
		const float Clip = Original->GetMaterial()->OpacityMaskClipValue;
		auto* Source = UMaterialInstanceDynamic::Create(Original, Dat);
		auto* Far = Cast<UMaterialInstanceDynamic>(Dat->GetFarSceneryMaterial(Source));
		if (!TestNotNull(TEXT("Far material is created"), Far)) return false;
		Faded.Add(Far);
		TestNotEqual(TEXT("Near and far use separate shader parents"), Far->GetMaterial(), Original->GetMaterial());
		TestEqual(TEXT("Ordinary material retains its blend"), Original->GetBlendMode(), Blend);
		TestEqual(TEXT("Ordinary cutout threshold is unchanged"), Original->GetMaterial()->OpacityMaskClipValue, Clip);
		TestEqual(TEXT("Far coverage still writes depth"), Far->GetBlendMode(), BLEND_Masked);
		TestTrue(TEXT("Far coverage supports MSAA"), Far->GetMaterial()->bUseAlphaToCoverage);
		TestEqual(TEXT("Source remapping is reused"), Dat->GetFarSceneryMaterial(Source), static_cast<UMaterialInterface*>(Far));
#if WITH_EDITORONLY_DATA
		TSet<UMaterialParameterCollection*> Collections;
		UMaterialExpressionCustom* Coverage = nullptr;
		for (UMaterialExpression* Expression : Far->GetMaterial()->GetExpressions())
		{
			if (auto* Parameter = Cast<UMaterialExpressionCollectionParameter>(Expression)) Collections.Add(Parameter->Collection);
			if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression); Custom && Custom->Description == TEXT("FarSceneryCoverage")) Coverage = Custom;
		}
		TestEqual(TEXT("Far material stays within UE's two-collection limit"), Collections.Num(), 2);
		TestTrue(TEXT("Original world fog remains connected"), Collections.Contains(Dat->GetRuntimeFogCollection()));
		TestTrue(TEXT("Shared coverage is the second collection"), Collections.Contains(Dat->GetRuntimeFarSceneryCollection()));
		if (TestNotNull(TEXT("Dedicated coverage node exists"), Coverage))
		{
			const auto* Input = Coverage->Inputs.FindByPredicate([](const FCustomInput& In) { return In.InputName == TEXT("Cutoff"); });
			const auto* Cutoff = Input ? Cast<UMaterialExpressionConstant>(Input->Input.Expression) : nullptr;
			TestTrue(TEXT("Coverage retains the original alpha cutoff"), Cutoff && Cutoff->R == Clip);
			if (Original == Originals[2])
			{
				const auto* MaskInput = Coverage->Inputs.FindByPredicate([](const FCustomInput& In) { return In.InputName == TEXT("A"); });
				const auto* Door = MaskInput ? Cast<UMaterialExpressionCustom>(MaskInput->Input.Expression) : nullptr;
				TestTrue(TEXT("Building doorway cutouts feed coverage"), Door && Door->Description == TEXT("BuildingDoorwaySlabClip"));
			}
		}
#endif
	}
	Dat->SetWorldEmissiveScale(.4f, .4f, FLinearColor(.2f,.3f,.4f));
	for (auto* Far : Faded)
		TestEqual(TEXT("Far materials continue receiving clock lighting updates"), Far->K2_GetScalarParameterValue(TEXT("EmissiveStrength")), .4f);
	// Panel colors and room vertex/doorway lighting must not acquire outdoor fill.
	TArray<UMaterialInstanceDynamic*> Fixed;
	for (auto* Parent : Dat->GetRuntimeMaterialParents())
	{
		if (Parent->GetName() != TEXT("M_ACEEnvCell_v3") && Parent->GetName() != TEXT("M_ACEEnvCellMasked_v3")
			&& Parent != Originals[0]) continue;
		auto* Source = UMaterialInstanceDynamic::Create(Parent, Dat);
		Source->SetScalarParameterValue(TEXT("EmissiveStrength"), .17f);
		Source->SetScalarParameterValue(TEXT("FogAmount"), .23f);
		Source->SetVectorParameterValue(TEXT("InteriorAmbient"), FLinearColor(.1f,.2f,.3f));
		auto* Far = Cast<UMaterialInstanceDynamic>(Dat->GetFarSceneryMaterial(Source, true));
		if (!TestNotNull(TEXT("Radius-only panel/room material exists"), Far)) return false;
		Fixed.Add(Far);
		TestEqual(TEXT("Radius-only material preserves local fog"), Far->K2_GetScalarParameterValue(TEXT("FogAmount")), .23f);
#if WITH_EDITORONLY_DATA
		TSet<UMaterialParameterCollection*> Collections;
		bool bDoorLighting = false;
		for (UMaterialExpression* Expression : Far->GetMaterial()->GetExpressions())
		{
			if (auto* Parameter = Cast<UMaterialExpressionCollectionParameter>(Expression)) Collections.Add(Parameter->Collection);
			if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression); Custom && Custom->Code.Contains(TEXT("Normal0"))) bDoorLighting = true;
		}
		TestEqual(TEXT("Rooms/panels stay within UE's two-collection limit"), Collections.Num(), 2);
		if (Parent != Originals[0]) TestTrue(TEXT("Room doorway vertex-light shading survives"), bDoorLighting);
#endif
	}
	TestEqual(TEXT("Opaque, masked room and panel coverage variants tested"), Fixed.Num(), 3);
	Dat->SetWorldEmissiveScale(.6f, .6f, FLinearColor::White);
	for (auto* Far : Fixed)
		TestEqual(TEXT("Outdoor clock updates leave special room/panel lighting alone"), Far->K2_GetScalarParameterValue(TEXT("EmissiveStrength")), .17f);
	if (!FApp::CanEverRender()) return !HasAnyErrors();

	// Independent rendered probe: stationary geometry fades from shared radius or
	// CPD size changes without recreating the material or editing its parameters.
	auto* Actor = World->SpawnActor<AActor>();
	auto* Card = NewObject<UProceduralMeshComponent>(Actor);
	Card->SetCustomPrimitiveDataFloat(SizeFadePrimitiveDataIndex, 0.f);
	Card->RegisterComponent();
	TArray<FVector> Normals;
	Normals.Init(-FVector::ForwardVector, 4);
	TArray<FProcMeshTangent> Tangents;
	Tangents.Init(FProcMeshTangent(FVector::RightVector, false), 4);
	// Mirrored halves share the same view and resolve: the left half keeps the
	// original parent, while the right half exercises far coverage. Comparing
	// separate captures can mistake changing capture state for a material change.
	Card->CreateMeshSection_LinearColor(0, {FVector(900,0,-1000),FVector(900,1000,-1000),FVector(900,1000,1000),FVector(900,0,1000)},
		{0,1,2,0,2,3}, Normals, {FVector2D(0,1),FVector2D(1,1),FVector2D(1,0),FVector2D(0,0)}, {}, Tangents, false);
	Card->CreateMeshSection_LinearColor(1, {FVector(900,-1000,-1000),FVector(900,0,-1000),FVector(900,0,1000),FVector(900,-1000,1000)},
		{0,1,2,0,2,3}, Normals, {FVector2D(0,1),FVector2D(1,1),FVector2D(1,0),FVector2D(0,0)}, {}, Tangents, false);
	auto* White = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
	*static_cast<FColor*>(White->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE)) = FColor::White;
	White->GetPlatformData()->Mips[0].BulkData.Unlock();
	White->SRGB = true;
	White->NeverStream = true;
	White->UpdateResource();
	auto* Ordinary = UMaterialInstanceDynamic::Create(Originals[0], Actor);
	TArray<UMaterialInstanceDynamic*> ProbeMaterials = Faded;
	ProbeMaterials.Add(Ordinary);
	for (auto* Far : ProbeMaterials)
	{
		Far->SetTextureParameterValue(TEXT("ACETexture"), White);
		Far->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
		Far->SetVectorParameterValue(TEXT("WorldAmbientTint"), FLinearColor::White);
		Far->SetScalarParameterValue(TEXT("FogAmount"), 0.f);
	}
	Card->SetMaterial(0, Faded[0]);
	Card->SetMaterial(1, Ordinary);
	for (auto* Material : {Ordinary, Faded[0]})
	{
		TestEqual(TEXT("Probe uses its white texture"), Material->K2_GetTextureParameterValue(TEXT("ACETexture")), static_cast<UTexture*>(White));
		TestEqual(TEXT("Probe emissive fill is explicitly white"), Material->K2_GetScalarParameterValue(TEXT("EmissiveStrength")), 1.f);
		TestEqual(TEXT("Probe disables material fog"), Material->K2_GetScalarParameterValue(TEXT("FogAmount")), 0.f);
		TestTrue(TEXT("Probe ambient tint is explicitly white"), Material->K2_GetVectorParameterValue(TEXT("WorldAmbientTint")).Equals(FLinearColor::White));
	}
	auto* Target = NewObject<UTextureRenderTarget2D>(Actor);
	constexpr int32 Width = 256, Height = 128;
	Target->InitCustomFormat(Width, Height, PF_FloatRGBA, false);
	auto* Capture = NewObject<USceneCaptureComponent2D>(Actor);
	Capture->TextureTarget = Target;
	Capture->FOVAngle = 90.f;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->CaptureSource = SCS_SceneColorHDRNoAlpha;
	Capture->ShowFlags.SetEyeAdaptation(false);
	Capture->ShowFlags.SetBloom(false);
	Capture->ShowFlags.SetAtmosphere(false);
	Capture->ShowFlags.SetAmbientOcclusion(false);
	Capture->ShowFlags.SetMotionBlur(false);
	// Exercise the lit material's emissive output; disabling Lighting invokes
	// the editor's unlit diffuse override instead of this controlled white fill.
	Capture->ShowFlags.SetLighting(true);
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->ShowOnlyComponents.Add(Card);
	Capture->RegisterComponent();
	const bool MobileLdr = World->GetFeatureLevel() == ERHIFeatureLevel::ES3_1
		&& IConsoleManager::Get().FindConsoleVariable(TEXT("r.MobileHDR"))->GetInt() == 0;
	int32 ProbeHalfExtent = 16;
	FLinearColor LastMean = FLinearColor::Black;
	FLinearColor LastReference = FLinearColor::Black;
	auto Read = [&](const TCHAR* Name)
	{
		TArray<FLinearColor> Pixels;
		// The first capture/readback after replacing a transient material can be
		// blank. Complete its GPU readback and deferred component updates before
		// measuring the unchanged state; a render-thread flush alone is insufficient.
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
			World->SendAllEndOfFrameUpdates();
			Capture->CaptureScene();
			FlushRenderingCommands();
			// RCM_UNorm rescales float targets by the extrema of the whole image;
			// preserve actual resolved HDR values for quantitative comparisons.
			Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax));
		}
		// Mobile LDR scene-color readback is gamma-2 encoded. Compare resolved
		// coverage in linear space, matching the existing retail weather probe.
		if (MobileLdr) for (auto& Pixel : Pixels) { Pixel.R *= Pixel.R; Pixel.G *= Pixel.G; Pixel.B *= Pixel.B; }
		TArray<FColor> Display;
		for (const auto& Pixel : Pixels) Display.Add(Pixel.ToFColorSRGB());
		TArray64<uint8> PNG;
		FImageUtils::PNGCompressImageArray(Width, Height, Display, PNG);
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/FarSceneryFade");
		IFileManager::Get().MakeDirectory(*Directory, true);
		FFileHelper::SaveArrayToFile(PNG, *(Directory / (FString(Name) + TEXT(".png"))));
		LastMean = FLinearColor::Transparent;
		LastReference = FLinearColor::Transparent;
		for (int32 Y = Height / 2 - ProbeHalfExtent; Y < Height / 2 + ProbeHalfExtent; ++Y)
			for (int32 X = 0; X < ProbeHalfExtent; ++X)
			{
				LastMean += Pixels[Y * Width + Width / 2 + X];
				LastReference += Pixels[Y * Width + Width / 2 - 1 - X];
			}
		LastMean /= 2 * FMath::Square(ProbeHalfExtent);
		LastReference /= 2 * FMath::Square(ProbeHalfExtent);
		float Minimum = MAX_flt, Maximum = -MAX_flt;
		for (const auto& Pixel : Pixels) { Minimum = FMath::Min(Minimum, Pixel.R); Maximum = FMath::Max(Maximum, Pixel.R); }
		AddInfo(FString::Printf(TEXT("Coverage capture %s: far %.6f, same-frame ordinary %.6f, frame minimum %.6f, maximum %.6f"), Name, LastMean.R, LastReference.R, Minimum, Maximum));
		return LastMean.R;
	};
	auto CheckFullCoverage = [&](const TCHAR* Name, const TCHAR* Assertion, float Tolerance = .02f)
	{
		const float Value = Read(Name);
		TestTrue(Assertion, FMath::IsNearlyEqual(Value, LastReference.R, Tolerance));
	};
	Dat->SetFarSceneryFadeRadius(World, 0.f, true);
	Read(TEXT("PairedWarmup"));
	const float Full = Read(TEXT("Full"));
	const float OrdinaryFull = LastReference.R;
	TestTrue(TEXT("Ordinary reference produces measurable coverage"), OrdinaryFull > .1f);
	TestTrue(TEXT("Full coverage preserves the ordinary parent output"), FMath::IsNearlyEqual(Full, OrdinaryFull, .02f));
	CheckFullCoverage(TEXT("FullRepeat"), TEXT("Unchanged full coverage still matches its same-frame ordinary reference"));
	Dat->SetFarSceneryFadeRadius(World, 1600.f, true);
	CheckFullCoverage(TEXT("LargeRadiusFull"), TEXT("A large radius preserves full coverage just like an unlimited radius"));
	Dat->SetFarSceneryFadeRadius(World, 0.f, true);
	Card->SetCustomPrimitiveDataFloat(SizeFadePrimitiveDataIndex, 1000.f);
	const float SizeMid = Read(TEXT("SizeMid"));
	TestTrue(TEXT("Compiled size coverage fades gradually"), SizeMid > LastReference.R * .2f && SizeMid < LastReference.R * .8f);
	Card->SetCustomPrimitiveDataFloat(SizeFadePrimitiveDataIndex, 800.f);
	TestTrue(TEXT("Compiled size coverage vanishes before culling"), Read(TEXT("SizeGone")) < .001f);
	Card->SetCustomPrimitiveDataFloat(SizeFadePrimitiveDataIndex, 0.f);
	Dat->SetFarSceneryFadeRadius(World, 1000.f, true);
	const float RadiusMid = Read(TEXT("RadiusMid"));
	AddInfo(FString::Printf(TEXT("Rendered far coverage means: ordinary %.5f, full %.5f, size-mid %.5f, radius-mid %.5f"), OrdinaryFull, Full, SizeMid, RadiusMid));
	TestTrue(TEXT("Shared radius fades existing material gradually"), RadiusMid > LastReference.R * .2f && RadiusMid < LastReference.R * .8f);
	Dat->SetFarSceneryFadeRadius(World, 800.f, true);
	TestTrue(TEXT("Shared radius vanishes before the stream edge"), Read(TEXT("RadiusGone")) < .001f);
	Dat->SetFarSceneryFadeRadius(World, 800.f, false);
	CheckFullCoverage(TEXT("Disabled"), TEXT("Diagnostic toggle restores coverage without a rebuild"), .01f);
	Dat->SetFarSceneryFadeRadius(World, 0.f, true);
	CheckFullCoverage(TEXT("ZeroEndsAfterTransitions"), TEXT("Zero endpoints restore full coverage after size/radius transitions"));
	Dat->SetFarSceneryFadeRadius(World, 1600.f, true, FVector(0., 45000., 0.));
	TestTrue(TEXT("The streamed player's anchor controls radius coverage"), Read(TEXT("AnchorOutside")) < .001f);
	Dat->SetFarSceneryFadeRadius(World, 1600.f, true);
	ProbeHalfExtent = 1;
	const float OrbitReference = Read(TEXT("OrbitReference"));
	const float OrdinaryOrbitReference = LastReference.R;
	TestTrue(TEXT("Near radius coverage preserves the same-pose ordinary reference"), FMath::IsNearlyEqual(OrbitReference, OrdinaryOrbitReference, .02f));
	int32 OrbitIndex = 0;
	for (const FVector& Camera : {FVector(-45000.,0.,0.), FVector(-40000.,20000.,0.), FVector(-40000.,-20000.,0.)})
	{
		Capture->SetWorldLocation(Camera);
		Capture->SetWorldRotation((FVector(900.,0.,0.) - Camera).Rotation());
		const float Orbit = Read(*FString::Printf(TEXT("Orbit%d"), ++OrbitIndex));
		const float OrdinaryOrbit = LastReference.R;
		TestTrue(TEXT("Distant ordinary reference remains visible"), OrdinaryOrbit > .1f);
		TestTrue(TEXT("450m zoom/orbit leaves stationary radius coverage unchanged"),
			FMath::IsNearlyEqual(Orbit / FMath::Max(OrdinaryOrbit, .001f), OrbitReference / FMath::Max(OrdinaryOrbitReference, .001f), .05f));
		AddInfo(FString::Printf(TEXT("Far radius orbit %d: %.5f, ordinary %.5f; near %.5f, near ordinary %.5f"), OrbitIndex, Orbit, OrdinaryOrbit, OrbitReference, OrdinaryOrbitReference));
	}
	Capture->DestroyComponent();
	Card->DestroyComponent();
	return !HasAnyErrors();
}
#endif
