#include "ACEDatSubsystem.h"
#include "ACERuntimeOptions.h"
#include "Sky/ACELegionsSkyRenderer.h"
#include "ACEProfiling.h"
#include "ACESpellFormula.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Dat/ACEStreamingBudget.h"
#include "ACELoginSettings.h"
#include "ACERetailParticleView.h"
#include "ACEDisplayView.h"
#include "UI/ACEUIResourceResolver.h"
#include "VR/ACEVRWidgetComponent.h"
#include "VR/ACEVRPointerVisuals.h"
#include "Personal/PersonalFarScenery.h"

UACEUIResourceResolver* UACEDatSubsystem::GetUiResources()
{
	if (!SharedUiResources)
	{
		SharedUiResources = NewObject<UACEUIResourceResolver>(this);
		SharedUiResources->Initialize(this);
	}
	return SharedUiResources;
}
#include "ACELandblockActor.h"
#include "ACETypes.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACEDatCursor.h"
#include "Dat/ACELandblockMeshCache.h"
#include "Dat/ACEDiskTileCache.h"
#include "Dat/ACEEnvCellTileCache.h"
#include "Dat/ACESceneryTileCache.h"
#include "Dat/ACEPCodeTileCache.h"
#include "Dat/ACELandSurfaceAtlas.h"
#include "Dat/ACELandTextureMipProvider.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "LandscapeProxy.h"
#include "LandscapeComponent.h"
#include "EngineUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "TextureResource.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"
#include "Async/InheritedContext.h"
#include "Templates/Function.h"
#include "Math/ConvexHull2d.h"

#if WITH_EDITORONLY_DATA
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2DArray.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionPixelNormalWS.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"

#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionShadowReplace.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpression.h"
#include "MaterialDomain.h"
#include "MaterialSceneTextureId.h"
#endif

namespace
{
	// Long blocking file/index work must not be eligible for TaskGraph busy-wait
	// execution by a game thread doing a synchronous package load. Preserve UE's
	// time/memory context explicitly on the dedicated thread-pool path.
	void RunDatBackgroundWork(TUniqueFunction<void()> Work)
	{
		auto Context = MakeShared<UE::FInheritedContextBase, ESPMode::ThreadSafe>();
		Context->CaptureInheritedContext();
		Async(EAsyncExecution::ThreadPool, [Context, Work = MoveTemp(Work)]() mutable
		{
			auto Scope = Context->RestoreInheritedContext();
			check(!IsInGameThread());
			TRACE_CPUPROFILER_EVENT_SCOPE(ACE_DatBackgroundWorker);
			Work();
		});
	}
	TAutoConsoleVariable<int32> CVarSkipUnusedPortalCamera(TEXT("ace.Render.SkipUnusedPortalCamera"),1,
		TEXT("Avoid terrain uniform updates for an unused outdoor portal camera. Set 0 for profiling."));
	enum class EACEAceMaterialKind : uint8
	{
		Opaque,
		Masked,
		Translucent,
		Additive,
		// Solid-color sky/celestial surfaces: no texture; emissive + opacity come from the
		// mesh vertex color the setup builder baked from the DAT surface color.
		VertexColorTranslucent,
        VertexColorAlpha,
		/** Day-dome backdrop — opaque unlit (avoids full-screen translucent overdraw). */
		VertexColorOpaque,
		LegionsSkyAlpha,
		LegionsSkyAdd
	};

	template<typename SurfaceType> bool IsDatPortalFillSurface(const SurfaceType& Decoded)
	{
		if (!Decoded.bFullyTransparent || Decoded.bClipMap)
		{
			return false;
		}
		const FLinearColor& C = Decoded.SolidColor;
		const bool bMagenta = Decoded.bIsSolid && C.R > 0.15f && C.B > 0.15f && C.G < 0.08f;
		return bMagenta || (C.A < 0.08f && Decoded.Translucency > 0.85f);
	}

	UMaterialParameterCollection* RuntimeFogCollection()
	{
		static TWeakObjectPtr<UMaterialParameterCollection> Cached;
		if (Cached.IsValid()) return Cached.Get();
		constexpr const TCHAR* PackageName = TEXT("/Game/ACE/RuntimeMaterials/MPC_ACEWorldFog_v2");
		constexpr const TCHAR* ObjectPath = TEXT("/Game/ACE/RuntimeMaterials/MPC_ACEWorldFog_v2.MPC_ACEWorldFog_v2");
		UMaterialParameterCollection* Collection = nullptr;
#if WITH_EDITORONLY_DATA
		if (FPackageName::DoesPackageExist(PackageName))
			Collection = LoadObject<UMaterialParameterCollection>(nullptr, ObjectPath);
		if (!Collection)
		{
			Collection = NewObject<UMaterialParameterCollection>(CreatePackage(PackageName),
				TEXT("MPC_ACEWorldFog_v2"), RF_Public | RF_Standalone);
			for (const auto& Pair : { TPair<FName, float>(TEXT("FogStart"), 20000.f),
				TPair<FName, float>(TEXT("FogEnd"), 90000.f), TPair<FName, float>(TEXT("FogAmount"), 1.f) })
			{
				auto& Parameter = Collection->ScalarParameters.AddDefaulted_GetRef();
				Parameter.ParameterName = Pair.Key;
				Parameter.DefaultValue = Pair.Value;
			}
			auto& Color = Collection->VectorParameters.AddDefaulted_GetRef();
			Color.ParameterName = TEXT("FogColor");
			Color.DefaultValue = FLinearColor(0.55f, 0.62f, 0.78f, 1.f);
			// Share the atmospheric uniforms: UE supports two collections per
			// material and terrain also needs the portal visibility collection.
			auto& Offset = Collection->VectorParameters.AddDefaulted_GetRef();
			Offset.ParameterName = TEXT("CloudOffset"); Offset.DefaultValue = FLinearColor::Transparent;
			Collection->PostEditChange();
		}
#else
		Collection = LoadObject<UMaterialParameterCollection>(nullptr, ObjectPath);
#endif
		Cached = Collection;
		return Collection;
	}

	UMaterialParameterCollection* RuntimePortalCollection()
	{
		static TWeakObjectPtr<UMaterialParameterCollection> Cached;
		if (Cached.IsValid()) return Cached.Get();
		constexpr const TCHAR* Package = TEXT("/Game/ACE/RuntimeMaterials/MPC_ACEPortalView_v1");
		constexpr const TCHAR* Path = TEXT("/Game/ACE/RuntimeMaterials/MPC_ACEPortalView_v1.MPC_ACEPortalView_v1");
		UMaterialParameterCollection* Collection = nullptr;
#if WITH_EDITORONLY_DATA
		if (FPackageName::DoesPackageExist(Package)) Collection = LoadObject<UMaterialParameterCollection>(nullptr, Path);
		if (!Collection)
		{
			Collection = NewObject<UMaterialParameterCollection>(CreatePackage(Package), TEXT("MPC_ACEPortalView_v1"), RF_Public | RF_Standalone);
			for (FName Name : {FName("LookOutEnable"), FName("PortalViewCount")})
			{
				auto& Parameter = Collection->ScalarParameters.AddDefaulted_GetRef();
				Parameter.ParameterName = Name; Parameter.DefaultValue = 0.f;
			}
			auto& Camera = Collection->VectorParameters.AddDefaulted_GetRef();
			Camera.ParameterName = TEXT("LookOutCam"); Camera.DefaultValue = FLinearColor::Black;
			Collection->PostEditChange();
		}
#else
		Collection = LoadObject<UMaterialParameterCollection>(nullptr, Path);
#endif
		Cached = Collection;
		return Collection;
	}

	UMaterialParameterCollection* RuntimeFarSceneryCollection()
	{
		static TWeakObjectPtr<UMaterialParameterCollection> Cached;
		if (Cached.IsValid()) return Cached.Get();
		constexpr const TCHAR* Package = TEXT("/Game/ACE/RuntimeMaterials/MPC_ACEFarScenery_v1");
		constexpr const TCHAR* Path = TEXT("/Game/ACE/RuntimeMaterials/MPC_ACEFarScenery_v1.MPC_ACEFarScenery_v1");
		UMaterialParameterCollection* Collection = nullptr;
#if WITH_EDITORONLY_DATA
		if (FPackageName::DoesPackageExist(Package)) Collection = LoadObject<UMaterialParameterCollection>(nullptr, Path);
		if (!Collection)
		{
			Collection = NewObject<UMaterialParameterCollection>(CreatePackage(Package), TEXT("MPC_ACEFarScenery_v1"), RF_Public | RF_Standalone);
			auto& Limits = Collection->VectorParameters.AddDefaulted_GetRef();
			Limits.ParameterName = TEXT("FarSceneryLimits");
			// X = radius in cm, Y = enabled, ZW = player XY. No radius until streaming starts.
			Limits.DefaultValue = FLinearColor(0.f, 1.f, 0.f, 0.f);
			Collection->PostEditChange();
		}
#else
		Collection = LoadObject<UMaterialParameterCollection>(nullptr, Path);
#endif
		Cached = Collection;
		return Collection;
	}

	UMaterial* LoadCookedAceMaterial(const TCHAR* Name)
	{
		const FString Path = FString::Printf(TEXT("/Game/ACE/RuntimeMaterials/%s.%s"), Name, Name);
		UMaterial* Material = LoadObject<UMaterial>(nullptr, *Path);
		if (!Material) UE_LOG(LogTemp, Error, TEXT("ACE: missing cooked material %s; recook the package"), *Path);
		return Material;
	}

#if WITH_EDITORONLY_DATA
	/**
	 * Unlit ACETexture materials with optional script overlays.
	 * Opaque/Masked match the pre-regression graphs (Sample RGB → Emissive, default UV0).
	 * Translucent/Additive keep OpacityMul. Diffuse/Emissive/UVOffset params exist on all
	 * so script hooks can set them; color multiplies only apply where needed.
	 */
		void WireRetailDistanceFog(UMaterial* Mat, UMaterialEditorOnlyData* EditorOnly,
			UMaterialExpression* EmissiveIn, int32 EmissiveInOutput,
			UMaterialExpression* BaseIn, int32 BaseInOutput)
		{
			if (!Mat || !EditorOnly || !EmissiveIn)
			{
				return;
			}
			UMaterialExpressionWorldPosition* WorldPos = NewObject<UMaterialExpressionWorldPosition>(Mat);
			WorldPos->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
			WorldPos->MaterialExpressionEditorX = 80;
			WorldPos->MaterialExpressionEditorY = 360;

			UMaterialExpressionCameraPositionWS* CamPos = NewObject<UMaterialExpressionCameraPositionWS>(Mat);
			CamPos->MaterialExpressionEditorX = 80;
			CamPos->MaterialExpressionEditorY = 300;

			UMaterialExpressionDistance* Dist = NewObject<UMaterialExpressionDistance>(Mat);
			Dist->A.Expression = WorldPos;
			Dist->B.Expression = CamPos;
			Dist->MaterialExpressionEditorX = 280;
			Dist->MaterialExpressionEditorY = 330;

			auto SharedFogParameter = [Mat](const TCHAR* Name)
			{
				auto* Parameter = NewObject<UMaterialExpressionCollectionParameter>(Mat);
				Parameter->Collection = RuntimeFogCollection();
				Parameter->ParameterName = Name;
				Parameter->ParameterId = Parameter->Collection->GetParameterId(Name);
				return Parameter;
			};
			auto* FogStart = SharedFogParameter(TEXT("FogStart"));
			FogStart->MaterialExpressionEditorX = 80;
			FogStart->MaterialExpressionEditorY = 480;

			auto* FogEnd = SharedFogParameter(TEXT("FogEnd"));
			FogEnd->MaterialExpressionEditorX = 80;
			FogEnd->MaterialExpressionEditorY = 540;

			UMaterialExpressionScalarParameter* FogAmount = NewObject<UMaterialExpressionScalarParameter>(Mat);
			FogAmount->ParameterName = TEXT("FogAmount");
			FogAmount->DefaultValue = 1.f;
			FogAmount->MaterialExpressionEditorX = 80;
			FogAmount->MaterialExpressionEditorY = 600;

			auto* FogColor = SharedFogParameter(TEXT("FogColor"));
			FogColor->MaterialExpressionEditorX = 80;
			FogColor->MaterialExpressionEditorY = 660;
			auto* SharedAmount = SharedFogParameter(TEXT("FogAmount"));
			auto* EffectiveAmount = NewObject<UMaterialExpressionMultiply>(Mat);
			EffectiveAmount->A.Expression = FogAmount;
			EffectiveAmount->B.Expression = SharedAmount;

			UMaterialExpressionSubtract* DistMinusStart = NewObject<UMaterialExpressionSubtract>(Mat);
			DistMinusStart->A.Expression = Dist;
			DistMinusStart->B.Expression = FogStart;
			DistMinusStart->MaterialExpressionEditorX = 480;
			DistMinusStart->MaterialExpressionEditorY = 300;

			UMaterialExpressionSubtract* EndMinusStart = NewObject<UMaterialExpressionSubtract>(Mat);
			EndMinusStart->A.Expression = FogEnd;
			EndMinusStart->B.Expression = FogStart;
			EndMinusStart->MaterialExpressionEditorX = 480;
			EndMinusStart->MaterialExpressionEditorY = 360;

			UMaterialExpressionMax* Span = NewObject<UMaterialExpressionMax>(Mat);
			Span->A.Expression = EndMinusStart;
			Span->ConstB = 1.f;
			Span->MaterialExpressionEditorX = 680;
			Span->MaterialExpressionEditorY = 360;

			UMaterialExpressionDivide* Linear = NewObject<UMaterialExpressionDivide>(Mat);
			Linear->A.Expression = DistMinusStart;
			Linear->B.Expression = Span;
			Linear->MaterialExpressionEditorX = 860;
			Linear->MaterialExpressionEditorY = 330;

			UMaterialExpressionMultiply* Scaled = NewObject<UMaterialExpressionMultiply>(Mat);
			Scaled->A.Expression = Linear;
			Scaled->B.Expression = EffectiveAmount;
			Scaled->MaterialExpressionEditorX = 1040;
			Scaled->MaterialExpressionEditorY = 330;

			UMaterialExpressionSaturate* Sat = NewObject<UMaterialExpressionSaturate>(Mat);
			Sat->Input.Expression = Scaled;
			Sat->MaterialExpressionEditorX = 1220;
			Sat->MaterialExpressionEditorY = 330;

			// Retail fixed-function fog interpolates linearly from FOGSTART to FOGEND.
			UMaterialExpressionPower* Factor = NewObject<UMaterialExpressionPower>(Mat);
			Factor->Base.Expression = Sat;
			Factor->ConstExponent = 1.f;
			Factor->MaterialExpressionEditorX = 1400;
			Factor->MaterialExpressionEditorY = 330;

			UMaterialExpressionLinearInterpolate* EmisFog = NewObject<UMaterialExpressionLinearInterpolate>(Mat);
			EmisFog->A.Expression = EmissiveIn;
			EmisFog->A.OutputIndex = EmissiveInOutput;
			EmisFog->B.Expression = FogColor;
			EmisFog->B.SetMask(1, 1, 1, 1, 0);
			EmisFog->Alpha.Expression = Factor;
			EmisFog->MaterialExpressionEditorX = 1580;
			EmisFog->MaterialExpressionEditorY = 0;

			EditorOnly->ExpressionCollection.AddExpression(WorldPos);
			EditorOnly->ExpressionCollection.AddExpression(CamPos);
			EditorOnly->ExpressionCollection.AddExpression(Dist);
			EditorOnly->ExpressionCollection.AddExpression(FogStart);
			EditorOnly->ExpressionCollection.AddExpression(FogEnd);
			EditorOnly->ExpressionCollection.AddExpression(FogAmount);
			EditorOnly->ExpressionCollection.AddExpression(FogColor);
			EditorOnly->ExpressionCollection.AddExpression(SharedAmount);
			EditorOnly->ExpressionCollection.AddExpression(EffectiveAmount);
			EditorOnly->ExpressionCollection.AddExpression(DistMinusStart);
			EditorOnly->ExpressionCollection.AddExpression(EndMinusStart);
			EditorOnly->ExpressionCollection.AddExpression(Span);
			EditorOnly->ExpressionCollection.AddExpression(Linear);
			EditorOnly->ExpressionCollection.AddExpression(Scaled);
			EditorOnly->ExpressionCollection.AddExpression(Sat);
			EditorOnly->ExpressionCollection.AddExpression(Factor);
			EditorOnly->ExpressionCollection.AddExpression(EmisFog);
			EditorOnly->EmissiveColor.Expression = EmisFog;

			if (BaseIn)
			{
				// DefaultLit lighting of a FogColor BaseColor PLUS emissive FogColor
				// double-adds the fog and blows distant trees/flora to white.
				// Fade the lit albedo with fog; emissive carries the single FogColor lerp.
				UMaterialExpressionOneMinus* InvFog = NewObject<UMaterialExpressionOneMinus>(Mat);
				InvFog->Input.Expression = Factor;
				InvFog->MaterialExpressionEditorX = 520;
				InvFog->MaterialExpressionEditorY = 80;

				UMaterialExpressionMultiply* BaseFade = NewObject<UMaterialExpressionMultiply>(Mat);
				BaseFade->A.Expression = BaseIn;
				BaseFade->A.OutputIndex = BaseInOutput;
				BaseFade->B.Expression = InvFog;
				BaseFade->MaterialExpressionEditorX = 700;
				BaseFade->MaterialExpressionEditorY = 80;
				EditorOnly->ExpressionCollection.AddExpression(InvFog);
				EditorOnly->ExpressionCollection.AddExpression(BaseFade);
				EditorOnly->BaseColor.Expression = BaseFade;
			}
		}

#endif
		UMaterial* CreateAceOverlayMaterial(const TCHAR* Name, EACEAceMaterialKind Kind, bool bDisableFog = false, bool bDisableDepthTest = false, bool bForceUvWrap = false, bool bEmissiveTimesOpacity = false, bool bTwoSided = true, bool bWorldLit = false, bool bOpacityTexAlphaOnly = false, bool bEmissiveTimesTexAlpha = false, bool bStaticVertexLighting = false, bool bExaminationLighting = false, bool bSurfaceShadow = false, bool bRetailSky = false, bool bRetailParticle = false, bool bBatchedOpacity = false)
	{
#if WITH_EDITORONLY_DATA
		// PIE / SurfaceUnpack flush nulls the UPROPERTY but leaves the transient
		// UMaterial. NewObject with the same name then fails → callers fall back
		// (opaque day-dome → translucent × A=0 → black sky).
		const bool bLegionsSky=Kind==EACEAceMaterialKind::LegionsSkyAlpha || Kind==EACEAceMaterialKind::LegionsSkyAdd;
		if (bLegionsSky)
		{
			Kind=Kind==EACEAceMaterialKind::LegionsSkyAdd ? EACEAceMaterialKind::Additive : EACEAceMaterialKind::Translucent;
			bRetailSky=true;
		}
		auto ApplyMeshUsages = [](UMaterial* M)
		{
			if (!M)
			{
				return;
			}
			// PMC uses the static-mesh shader permutation (UE 5.8 has no MATUSAGE_ProceduralMesh).
			M->SetUsageByFlag(MATUSAGE_StaticMesh, true);
			M->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
			M->CheckMaterialUsage(MATUSAGE_StaticMesh);
			M->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		};
		if (UMaterial* Existing = FindObject<UMaterial>(GetTransientPackage(), Name))
		{
			// Do not CheckMaterialUsage here — that recompiles the permutation and
			// Unlit samples the default white ACETexture until the shader is ready
			// (full-screen flicker on sky/weather/particles).
			return Existing;
		}
		UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), Name, RF_Public | RF_Transient);
		if (!Mat)
		{
			return nullptr;
		}

		Mat->MaterialDomain = MD_Surface;
		// Reserve the desktop AfterDOF buffer for additive particle colors.
		// Other DAT transparency remains in the world pass. Mobile/VR continue
		// their native composition because the view does not enable our flag.
		Mat->TranslucencyPass = bRetailParticle ? MTP_AfterDOF : MTP_BeforeDOF;
		Mat->TwoSided = bTwoSided;
		const bool bLitWorld = bWorldLit
			&& (Kind == EACEAceMaterialKind::Opaque || Kind == EACEAceMaterialKind::Masked);
		Mat->SetShadingModel(bLitWorld ? MSM_DefaultLit : MSM_Unlit);
		if (bDisableFog)
		{
			// Sky dome/celestial quads: height fog must never wash the sky itself.
			// Do not set bIsSky — UE sky-pass materials often never draw on procedural
			// meshes, which left a black void (no cube, clouds, or stars).
			Mat->bUseTranslucencyVertexFog = false;
		}
		if (bDisableDepthTest)
		{
			// Rain sheets are camera-local weather overlays — draw over world geometry.
			Mat->bDisableDepthTest = true;
		}
		if (Kind == EACEAceMaterialKind::Additive)
		{
			// Small bright star dots sparkle under TAA/TSR without responsive AA.
			Mat->bEnableResponsiveAA = true;
		}
		switch (Kind)
		{
		case EACEAceMaterialKind::Opaque:
		case EACEAceMaterialKind::VertexColorOpaque:
			Mat->BlendMode = BLEND_Opaque;
			break;
		case EACEAceMaterialKind::Masked:
			Mat->BlendMode = BLEND_Masked;
			// Keep the proven cutout behavior: mobile alpha-to-coverage made
			// distant foliage flicker more in the native headset comparison.
			Mat->bUseAlphaToCoverage = false;
			// Soft clip (0.1) leaves filtered white/bright leaf fringes visible as outlines.
			Mat->OpacityMaskClipValue = 0.42f;
			break;
		case EACEAceMaterialKind::Translucent:
			Mat->BlendMode = BLEND_Translucent;
			// Unlit sky/FX — avoid volumetric translucency lighting permutations (very pink in F5).
			Mat->TranslucencyLightingMode = TLM_Surface;
			break;
		case EACEAceMaterialKind::Additive:
			Mat->BlendMode = BLEND_Additive;
			Mat->TranslucencyLightingMode = TLM_Surface;
			break;
		case EACEAceMaterialKind::VertexColorAlpha:
        case EACEAceMaterialKind::VertexColorTranslucent:
			Mat->BlendMode = BLEND_Translucent;
			Mat->TranslucencyLightingMode = TLM_Surface;
			break;
		}

		UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
		Sample->ParameterName = TEXT("ACETexture");
		Sample->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		Sample->SamplerType = SAMPLERTYPE_Color;
		// Starfield UVs tile past [0,1] (~0.4–4.6). Texture Address Wrap alone was unreliable
		// on transient PMC params — force shared Wrap + Frac so the tile repeats.
		if (bForceUvWrap)
		{
			Sample->SamplerSource = SSM_Wrap_WorldGroupSettings;
		}
		Sample->MaterialExpressionEditorX = -450;
		Sample->MaterialExpressionEditorY = 0;

		UMaterialExpressionVectorParameter* UvOffset = NewObject<UMaterialExpressionVectorParameter>(Mat);
		UvOffset->ParameterName = TEXT("UVOffset");
		UvOffset->DefaultValue = FLinearColor(0.f, 0.f, 0.f, 0.f);
		UvOffset->MaterialExpressionEditorX = -450;
		UvOffset->MaterialExpressionEditorY = 420;

		UMaterialExpressionScalarParameter* DiffuseStrength = NewObject<UMaterialExpressionScalarParameter>(Mat);
		DiffuseStrength->ParameterName = TEXT("DiffuseStrength");
		DiffuseStrength->DefaultValue = 1.f;
		DiffuseStrength->MaterialExpressionEditorX = -450;
		DiffuseStrength->MaterialExpressionEditorY = 160;

		UMaterialExpressionScalarParameter* EmissiveStrength = NewObject<UMaterialExpressionScalarParameter>(Mat);
		EmissiveStrength->ParameterName = TEXT("EmissiveStrength");
		EmissiveStrength->DefaultValue = bLitWorld ? 0.14f : 1.f;
		EmissiveStrength->MaterialExpressionEditorX = -450;
		EmissiveStrength->MaterialExpressionEditorY = 240;

		UMaterialExpressionScalarParameter* OpacityMul = NewObject<UMaterialExpressionScalarParameter>(Mat);
		OpacityMul->ParameterName = TEXT("OpacityMul");
		OpacityMul->DefaultValue = 1.f;
		OpacityMul->MaterialExpressionEditorX = -450;
		OpacityMul->MaterialExpressionEditorY = 320;

		// UV0 + UVOffset.xy → sampler coordinates (script/door + sky cloud UV scrolling).
		UMaterialExpressionTextureCoordinate* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
		TexCoord->CoordinateIndex = 0;
		TexCoord->MaterialExpressionEditorX = -900;
		TexCoord->MaterialExpressionEditorY = 380;

		UMaterialExpressionAppendVector* UvXY = NewObject<UMaterialExpressionAppendVector>(Mat);
		UvXY->A.Expression = UvOffset;
		UvXY->A.OutputIndex = 1; // R
		UvXY->B.Expression = UvOffset;
		UvXY->B.OutputIndex = 2; // G
		UvXY->MaterialExpressionEditorX = -700;
		UvXY->MaterialExpressionEditorY = 440;

		UMaterialExpressionAdd* UvAdd = NewObject<UMaterialExpressionAdd>(Mat);
		UvAdd->A.Expression = TexCoord;
		UvAdd->B.Expression = UvXY;
		UvAdd->MaterialExpressionEditorX = -600;
		UvAdd->MaterialExpressionEditorY = 380;

		// Preserve UV derivatives through the lookup. Frac before a mipmapped
		// sample creates a false large gradient at every integer boundary,
		// selecting a coarse mip and drawing colored lines around sky cards.
		Sample->Coordinates.Expression = UvAdd;

		UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
		if (!EditorOnly)
		{
			return nullptr;
		}

		EditorOnly->ExpressionCollection.AddExpression(Sample);
		EditorOnly->ExpressionCollection.AddExpression(UvOffset);
		EditorOnly->ExpressionCollection.AddExpression(TexCoord);
		EditorOnly->ExpressionCollection.AddExpression(UvXY);
		EditorOnly->ExpressionCollection.AddExpression(UvAdd);
		EditorOnly->ExpressionCollection.AddExpression(DiffuseStrength);
		EditorOnly->ExpressionCollection.AddExpression(EmissiveStrength);
		EditorOnly->ExpressionCollection.AddExpression(OpacityMul);

		// Batched particles carry independent fades in vertex alpha. This also
		// works on the legacy desktop renderer and the Quest mobile renderer.
		UMaterialExpression* EffectiveOpacity = OpacityMul;
		if (bBatchedOpacity)
		{
			auto* Data = NewObject<UMaterialExpressionVertexColor>(Mat);
			auto* Fade = NewObject<UMaterialExpressionMultiply>(Mat);
			Fade->A.Expression = OpacityMul; Fade->B.Expression = Data; Fade->B.OutputIndex = 4;
			for (UMaterialExpression* E : TArray<UMaterialExpression*>{Data, Fade})
				EditorOnly->ExpressionCollection.AddExpression(E);
			EffectiveOpacity = Fade;
		}


		if (bLegionsSky)
		{
			// The Legions clear sky reuses one image in both texture stages, with
			// independent UV motion. The DAT chooses the RGB and alpha operations.
			auto& Expressions=EditorOnly->ExpressionCollection;
			auto* Texture=NewObject<UMaterialExpressionTextureObjectParameter>(Mat);
			Texture->Texture=Sample->Texture;
			Texture->ParameterName=Sample->ParameterName;
			auto* Velocity=NewObject<UMaterialExpressionVectorParameter>(Mat);
			Velocity->ParameterName=TEXT("UVVelocity");
			Velocity->DefaultValue=FLinearColor::Transparent;
			auto* Clock=NewObject<UMaterialExpressionTime>(Mat);
			auto* Stage=NewObject<UMaterialExpressionCustom>(Mat);
			Stage->OutputType=CMOT_Float4;
			auto Input=[Stage](const TCHAR* Name,UMaterialExpression* Source,int32 Output)
			{
				FCustomInput& In=Stage->Inputs.AddDefaulted_GetRef();
				In.InputName=Name;
				In.Input.Connect(Output,Source);
			};
			for (const TCHAR* Param : {TEXT("MaterialFactor"),TEXT("StageOps"),TEXT("SkyTint")})
			{
				auto* Vector=NewObject<UMaterialExpressionVectorParameter>(Mat);
				Vector->ParameterName=Param;
				Vector->DefaultValue=FLinearColor::White;
				Expressions.AddExpression(Vector);
				Input(Param,Vector,5);
			}
			Input(TEXT("Image"),Texture,0);
			Input(TEXT("UV"),TexCoord,0);
			Input(TEXT("Offset"),UvOffset,5);
			Input(TEXT("Velocity"),Velocity,5);
			Input(TEXT("Seconds"),Clock,0);
			Stage->Code=TEXT(R"(
				float4 scroll=frac(Offset+Velocity*Seconds);
				float4 first=Texture2DSample(Image,ImageSampler,UV+scroll.xy);
				float alpha=MaterialFactor.a*lerp(1,first.a,StageOps.g);
				float3 linearRGB=first.rgb;
				[branch] if (StageOps.b>.5 || StageOps.a>.5 || any(StageOps.r*MaterialFactor.rgb!=1)) {
					float3 a=lerp(first.rgb*12.92,1.055*pow(max(first.rgb,0),1./2.4)-.055,step(.0031308,first.rgb));
					float3 rgb=saturate(StageOps.r*a*MaterialFactor.rgb);
					[branch] if (StageOps.b>.5 || StageOps.a>.5) {
						float4 second=Texture2DSample(Image,ImageSampler,UV+scroll.zw);
						float3 b=lerp(second.rgb*12.92,1.055*pow(max(second.rgb,0),1./2.4)-.055,step(.0031308,second.rgb));
						if (StageOps.b>.5) rgb=saturate(rgb+b-.5);
						if (StageOps.a>.5) alpha=saturate(2*alpha*second.a);
					}
					linearRGB=lerp(rgb/12.92,pow((rgb+.055)/1.055,2.4),step(.04045,rgb));
				}
				float3 emission=linearRGB*SkyTint.rgb;
			)");
			if (Kind == EACEAceMaterialKind::Additive)
			{
				auto* Backdrop = NewObject<UMaterialExpressionScalarParameter>(Mat);
				Backdrop->ParameterName = TEXT("DaylightBackdrop");
				Backdrop->DefaultValue = 0.f;
				Expressions.AddExpression(Backdrop);
				Input(TEXT("Backdrop"),Backdrop,0);
				auto* SunGlow = NewObject<UMaterialExpressionScalarParameter>(Mat);
				SunGlow->ParameterName = TEXT("SunHorizonGlow");
				SunGlow->DefaultValue = 0.f;
				Expressions.AddExpression(SunGlow);
				Input(TEXT("SunGlow"),SunGlow,0);
				// Low sun in Legions has a broad rose halo. Preserve the luminance
				// profile of the additive flare and the hot centre of the disc.
				Stage->Code += TEXT(R"(
					float peak = max(max(emission.r, emission.g), emission.b);
					emission = lerp(emission, peak * float3(1, .46, .58),
						SunGlow * (1 - .65 * smoothstep(.4, 1, peak)));
				)");
				// The original adds display RGB; UE blends linear light. With a
				// gamma-two approximation, (background + source)^2 - background^2
				// includes this cross term. The day palette estimates the background
				// without sampling scene color or adding another sky pass. A finite
				// slope at black keeps quantization in the sun's outer glow subdued.
				// The LDR mobile base pass already converts to gamma before blending.
				Stage->Code += TEXT(R"(
					#if !defined(OUTPUT_GAMMA_SPACE) || !OUTPUT_GAMMA_SPACE
						[branch] if (Backdrop > 0) emission += 2 * Backdrop * (sqrt(max(emission, 0) + .01) - .1);
					#endif
				)");
			}
			Stage->Code += TEXT("return float4(emission,alpha*SkyTint.a);");
			auto* RGB=NewObject<UMaterialExpressionComponentMask>(Mat);
			RGB->R=RGB->G=RGB->B=true;
			RGB->A=false;
			RGB->Input.Connect(0,Stage);
			auto* Alpha=NewObject<UMaterialExpressionComponentMask>(Mat);
			Alpha->R=Alpha->G=Alpha->B=false;
			Alpha->A=true;
			Alpha->Input.Connect(0,Stage);
			for (UMaterialExpression* E : TArray<UMaterialExpression*>{Texture,Velocity,Clock,Stage,RGB,Alpha})
				Expressions.AddExpression(E);
			EditorOnly->EmissiveColor.Connect(0,RGB);
			EditorOnly->Opacity.Connect(0,Alpha);
		}
		else if (Kind == EACEAceMaterialKind::VertexColorAlpha)
        {
            auto* VC=NewObject<UMaterialExpressionVertexColor>(Mat);
            EditorOnly->ExpressionCollection.AddExpression(VC);
            EditorOnly->EmissiveColor.Expression=VC;
            EditorOnly->Opacity.Expression=VC;EditorOnly->Opacity.OutputIndex=4;
        }
        else if (Kind == EACEAceMaterialKind::VertexColorTranslucent)
        {
			// The backdrop fills the sky. The horizon copy fades out above eye level.
			UMaterialExpressionVectorParameter* FillColor = NewObject<UMaterialExpressionVectorParameter>(Mat);
			FillColor->ParameterName = TEXT("FillColor");
			FillColor->DefaultValue = FLinearColor(0.55f, 0.62f, 0.78f, 1.f);
			FillColor->MaterialExpressionEditorX = -450;
			FillColor->MaterialExpressionEditorY = -120;
			EditorOnly->ExpressionCollection.AddExpression(FillColor);

			UMaterialExpressionMultiply* EmissiveMul = NewObject<UMaterialExpressionMultiply>(Mat);
			EmissiveMul->A.Expression = FillColor;
			EmissiveMul->A.OutputIndex = 0;
			EmissiveMul->B.Expression = EmissiveStrength;
			EmissiveMul->MaterialExpressionEditorX = -150;
			EmissiveMul->MaterialExpressionEditorY = -120;
			EditorOnly->ExpressionCollection.AddExpression(EmissiveMul);

			EditorOnly->EmissiveColor.Expression = EmissiveMul;
			auto* Horizon = NewObject<UMaterialExpressionScalarParameter>(Mat);
			Horizon->ParameterName = TEXT("HorizonFalloff");
			Horizon->DefaultValue = 0.f;
			auto* View = NewObject<UMaterialExpressionCameraVectorWS>(Mat);
			auto* Coverage = NewObject<UMaterialExpressionCustom>(Mat);
			Coverage->OutputType = CMOT_Float1;
			Coverage->Code = TEXT("return Opacity * (1 - smoothstep(0, 1, -ViewDir.z * Horizon));");
			for (const auto& Pair : {TPair<FName,UMaterialExpression*>(TEXT("Opacity"),EffectiveOpacity),
				{TEXT("Horizon"),Horizon}, {TEXT("ViewDir"),View}})
			{
				FCustomInput Input;
				Input.InputName = Pair.Key;
				Input.Input.Expression = Pair.Value;
				Coverage->Inputs.Add(Input);
			}
			for (UMaterialExpression* E : TArray<UMaterialExpression*>{Horizon, View, Coverage})
				EditorOnly->ExpressionCollection.AddExpression(E);
			EditorOnly->Opacity.Connect(0,Coverage);
		}
		else if (Kind == EACEAceMaterialKind::VertexColorOpaque)
		{
			// Solid sky surfaces have no texture — the day-dome gradient lives entirely in the
			// per-vertex color the setup builder baked from FACEDatDecodedSurface::SolidColor.
			// Sampling the (unset) ACETexture parameter here is what painted a big white square.
			UMaterialExpressionVertexColor* VertColor = NewObject<UMaterialExpressionVertexColor>(Mat);
			VertColor->MaterialExpressionEditorX = -450;
			VertColor->MaterialExpressionEditorY = -120;
			EditorOnly->ExpressionCollection.AddExpression(VertColor);

			UMaterialExpressionMultiply* EmissiveMul = NewObject<UMaterialExpressionMultiply>(Mat);
			EmissiveMul->A.Expression = VertColor;
			EmissiveMul->A.OutputIndex = 0; // RGB
			EmissiveMul->B.Expression = EmissiveStrength;
			EmissiveMul->MaterialExpressionEditorX = -150;
			EmissiveMul->MaterialExpressionEditorY = -120;
			EditorOnly->ExpressionCollection.AddExpression(EmissiveMul);

			EditorOnly->EmissiveColor.Expression = EmissiveMul;
		}
		else if (Kind == EACEAceMaterialKind::Opaque || Kind == EACEAceMaterialKind::Masked)
		{
			// Texture RGB * EmissiveStrength → emissive. Also drive BaseColor so Unlit/PMC
			// permutations that sample diffuse still show DAT detail (Emissive-only looked flat).
			UMaterialExpressionMultiply* EmissiveMul = NewObject<UMaterialExpressionMultiply>(Mat);
			EmissiveMul->A.Expression = Sample;
			EmissiveMul->A.OutputIndex = 0;
			EmissiveMul->B.Expression = EmissiveStrength;
			EmissiveMul->MaterialExpressionEditorX = -150;
			EmissiveMul->MaterialExpressionEditorY = 0;
			EditorOnly->ExpressionCollection.AddExpression(EmissiveMul);
			EditorOnly->EmissiveColor.Expression = EmissiveMul;
            if (bLitWorld && !bStaticVertexLighting)
            {
                auto* Tint = NewObject<UMaterialExpressionVectorParameter>(Mat);
                Tint->ParameterName = TEXT("WorldAmbientTint"); Tint->DefaultValue = FLinearColor::White;
                // Authored surface emission is independent of changing world ambient.
                // Small lamps must remain luminous at night without making their glass additive.
                auto* Luminosity = NewObject<UMaterialExpressionScalarParameter>(Mat);
                Luminosity->ParameterName = TEXT("AuthoredLuminosity"); Luminosity->DefaultValue = 0.f;
                auto* Fill = NewObject<UMaterialExpressionCustom>(Mat);
                Fill->OutputType = CMOT_Float3;
                Fill->Code = TEXT("return saturate(T*E + L);");
                for (auto Pair : {TPair<const TCHAR*,UMaterialExpression*>(TEXT("T"),Tint),
                    TPair<const TCHAR*,UMaterialExpression*>(TEXT("E"),EmissiveStrength),
                    TPair<const TCHAR*,UMaterialExpression*>(TEXT("L"),Luminosity)})
                { FCustomInput I; I.InputName=Pair.Key; I.Input.Expression=Pair.Value; Fill->Inputs.Add(I); }
                EditorOnly->ExpressionCollection.AddExpression(Luminosity);
                EditorOnly->ExpressionCollection.AddExpression(Tint);
                EditorOnly->ExpressionCollection.AddExpression(Fill);
                EmissiveMul->B.Expression=Fill;
            }
			if (bStaticVertexLighting)
			{
				auto* VC = NewObject<UMaterialExpressionVertexColor>(Mat);
				auto* Ambient = NewObject<UMaterialExpressionVectorParameter>(Mat);
				Ambient->ParameterName = TEXT("InteriorAmbient");
				Ambient->DefaultValue = FLinearColor(0.15f,0.15f,0.15f);
				auto* Light = NewObject<UMaterialExpressionCustom>(Mat);
				Light->OutputType = CMOT_Float3;
				// D3DPolyRender::drawMeshSubset uses unit diffuse/ambient when the
				// static-light vertex stream supplies emissive. Surface diffuse must
				// not attenuate those already-baked lights a second time.
				Light->Code = TEXT("float f=1; float4 c[2]={Door0,Door1}; float4 n[2]={Normal0,Normal1}; for(int i=0;i<2;i++){ if(c[i].w>0){ float3 d=P-c[i].xyz; float depth=-dot(d,n[i].xyz); float side=abs(dot(d,float3(-n[i].y,n[i].x,0))); float width=1-smoothstep(c[i].w,c[i].w+70,side); float height=1-smoothstep(n[i].w,n[i].w+70,abs(d.z)); f=min(f,1-0.4*(1-smoothstep(0,180,max(0,depth)))*width*height); }} return saturate(V + A)*f;");
				Light->Inputs.Reset();
				auto Input = [&](const TCHAR* Name, UMaterialExpression* Expression)
				{
					FCustomInput In; In.InputName = Name; In.Input.Expression = Expression; Light->Inputs.Add(In);
				};
				Input(TEXT("V"),VC); Input(TEXT("D"),DiffuseStrength);
				Input(TEXT("A"),Ambient); Input(TEXT("L"),EmissiveStrength);
				auto* WorldPosition=NewObject<UMaterialExpressionWorldPosition>(Mat);
				EditorOnly->ExpressionCollection.AddExpression(WorldPosition);
				Input(TEXT("P"),WorldPosition);
				for (const TCHAR* ParameterName : {TEXT("Door0"),TEXT("Door1"),TEXT("Normal0"),TEXT("Normal1")})
				{
					auto* Parameter=NewObject<UMaterialExpressionVectorParameter>(Mat);
					Parameter->ParameterName=FName(ParameterName); Parameter->DefaultValue=FLinearColor::Transparent;
					EditorOnly->ExpressionCollection.AddExpression(Parameter);
					// VectorParameter's default output is RGB; append its alpha extent.
					auto* Vector=NewObject<UMaterialExpressionAppendVector>(Mat);
					Vector->A.Expression=Parameter; Vector->B.Expression=Parameter; Vector->B.OutputIndex=4;
					EditorOnly->ExpressionCollection.AddExpression(Vector); Input(ParameterName,Vector);
				}
				EditorOnly->ExpressionCollection.AddExpression(VC);
				EditorOnly->ExpressionCollection.AddExpression(Ambient);
				EditorOnly->ExpressionCollection.AddExpression(Light);
				EmissiveMul->B.Expression = Light;
			}
			UMaterialExpression* BaseSrc = Sample;
			int32 BaseSrcOut = 0;
			if (bLitWorld)
			{
				// Split DAT albedo: emissive fill + shadowable DefaultLit. Full BaseColor
				// plus EmissiveStrength ~0.9 washed out player CSM on buildings/foliage.
				UMaterialExpressionOneMinus* LitFrac = NewObject<UMaterialExpressionOneMinus>(Mat);
				LitFrac->Input.Expression = EmissiveStrength;
				LitFrac->MaterialExpressionEditorX = -150;
				LitFrac->MaterialExpressionEditorY = 160;
				UMaterialExpressionMultiply* BaseMul = NewObject<UMaterialExpressionMultiply>(Mat);
				BaseMul->A.Expression = Sample;
				BaseMul->A.OutputIndex = 0;
				BaseMul->B.Expression = LitFrac;
				BaseMul->MaterialExpressionEditorX = 20;
				BaseMul->MaterialExpressionEditorY = 80;
				EditorOnly->ExpressionCollection.AddExpression(LitFrac);
				EditorOnly->ExpressionCollection.AddExpression(BaseMul);
				BaseSrc = BaseMul;
				BaseSrcOut = 0;

				UMaterialExpressionConstant* Rough = NewObject<UMaterialExpressionConstant>(Mat);
				Rough->R = 1.f;
				Rough->MaterialExpressionEditorX = -150;
				Rough->MaterialExpressionEditorY = 220;
				UMaterialExpressionConstant* Spec = NewObject<UMaterialExpressionConstant>(Mat);
				Spec->R = 0.f;
				Spec->MaterialExpressionEditorX = -150;
				Spec->MaterialExpressionEditorY = 280;
				UMaterialExpressionConstant* Metal = NewObject<UMaterialExpressionConstant>(Mat);
				Metal->R = 0.f;
				Metal->MaterialExpressionEditorX = -150;
				Metal->MaterialExpressionEditorY = 340;
				EditorOnly->ExpressionCollection.AddExpression(Rough);
				EditorOnly->ExpressionCollection.AddExpression(Spec);
				EditorOnly->ExpressionCollection.AddExpression(Metal);
				EditorOnly->Roughness.Expression = Rough;
				EditorOnly->Specular.Expression = Spec;
				EditorOnly->Metallic.Expression = Metal;
			}
			EditorOnly->BaseColor.Expression = BaseSrc;
			EditorOnly->BaseColor.OutputIndex = BaseSrcOut;
			if (bExaminationLighting)
			{
				// CreatureMode ambient .3; gmCG3DView distant light 2 at (.3,1.9,.65).
				// Retail clamps vertex illumination before modulating the DAT texture.
				auto* Normal = NewObject<UMaterialExpressionVertexNormalWS>(Mat);
				auto* Light = NewObject<UMaterialExpressionCustom>(Mat);
				Light->OutputType = CMOT_Float1;
				Light->Code = TEXT("return saturate(L + D * (0.3 + 2.0 * max(0.0, dot(normalize(N), normalize(float3(0.3,-1.9,0.65))))));");
				const auto Input=[&](const TCHAR* Name,UMaterialExpression* Expression){FCustomInput I;I.InputName=Name;I.Input.Expression=Expression;Light->Inputs.Add(I);};
				Input(TEXT("N"),Normal);Input(TEXT("D"),DiffuseStrength);Input(TEXT("L"),EmissiveStrength);
				auto* Interpolated = NewObject<UMaterialExpressionVertexInterpolator>(Mat);
				Interpolated->Input.Expression=Light;
				EditorOnly->ExpressionCollection.AddExpression(Normal);
				EditorOnly->ExpressionCollection.AddExpression(Light);
				EditorOnly->ExpressionCollection.AddExpression(Interpolated);
				EmissiveMul->B.Expression=Interpolated;
			}
			if (!bDisableFog)
			{
				WireRetailDistanceFog(Mat, EditorOnly, EmissiveMul, 0, BaseSrc, BaseSrcOut);
			}
			if (Kind == EACEAceMaterialKind::Masked)
			{
				EditorOnly->OpacityMask.Expression = Sample;
				EditorOnly->OpacityMask.OutputIndex = 4;
			}
		}
		else
		{
			UMaterialExpressionMultiply* DiffuseMul = NewObject<UMaterialExpressionMultiply>(Mat);
			DiffuseMul->A.Expression = Sample;
			DiffuseMul->A.OutputIndex = 0;
			DiffuseMul->B.Expression = DiffuseStrength;
			DiffuseMul->MaterialExpressionEditorX = -150;
			DiffuseMul->MaterialExpressionEditorY = 0;
			EditorOnly->ExpressionCollection.AddExpression(DiffuseMul);

			UMaterialExpressionMultiply* EmissiveMul = NewObject<UMaterialExpressionMultiply>(Mat);
			EmissiveMul->A.Expression = DiffuseMul;
			EmissiveMul->B.Expression = EmissiveStrength;
			EmissiveMul->MaterialExpressionEditorX = 50;
			EmissiveMul->MaterialExpressionEditorY = 0;
			EditorOnly->ExpressionCollection.AddExpression(EmissiveMul);

            if (bRetailSky)
            {
                // D3DPolyRender + CMaterial: emissive + ambient + diffuse sunlight.
                // SetDiffuseSimple changes Diffuse only; Ambient remains unit white.
                auto* Normal = NewObject<UMaterialExpressionVertexNormalWS>(Mat);
                auto* Light = NewObject<UMaterialExpressionCustom>(Mat);
                Light->OutputType = CMOT_Float3;
                Light->Code = TEXT("return saturate(L + A + D*S*max(0.0,dot(normalize(N),normalize(V))));");
                const auto Input=[&](const TCHAR* Name,UMaterialExpression* Expression){ FCustomInput I;I.InputName=Name;I.Input.Expression=Expression;Light->Inputs.Add(I); };
                Input(TEXT("N"),Normal); Input(TEXT("D"),DiffuseStrength); Input(TEXT("L"),EmissiveStrength);
                for (const auto& Pair : {TPair<const TCHAR*,const TCHAR*>(TEXT("A"),TEXT("SkyAmbient")),
                    TPair<const TCHAR*,const TCHAR*>(TEXT("S"),TEXT("SkySunColor")),
                    TPair<const TCHAR*,const TCHAR*>(TEXT("V"),TEXT("SkySunDirection"))})
                {
                    auto* Param=NewObject<UMaterialExpressionVectorParameter>(Mat);
                    Param->ParameterName=Pair.Value;
                    Param->DefaultValue=FLinearColor(0,0,1);
                    EditorOnly->ExpressionCollection.AddExpression(Param); Input(Pair.Key,Param);
                }
                auto* Interpolated=NewObject<UMaterialExpressionVertexInterpolator>(Mat);
                Interpolated->Input.Expression=Light;
                auto* Modulate=NewObject<UMaterialExpressionCustom>(Mat);
                Modulate->OutputType=CMOT_Float3;
                // Retail fixed-function texture modulation occurred in display space.
                Modulate->Code=TEXT("float3 t=lerp(T*12.92,1.055*pow(max(T,0.0),1.0/2.4)-0.055,step(0.0031308,T)); float3 c=saturate(t*C);");
                if (Kind==EACEAceMaterialKind::Additive)
                {
                    // Retail SRCALPHA/ONE fades in display space. Mobile LDR
                    // square-roots the entire additive contribution after applying
                    // opacity: leaving fade there brightens almost-black card edges.
                    Modulate->Code+=TEXT(" float edge=1.0-length(2.0*U-1.0); c*=saturate(A*O)*lerp(1.0,smoothstep(0.0,0.2,edge),saturate(F));");
                    FCustomInput A;A.InputName=TEXT("A");A.Input.Expression=Sample;A.Input.OutputIndex=4;
                    FCustomInput O;O.InputName=TEXT("O");O.Input.Expression=EffectiveOpacity;
                    Modulate->Inputs.Add(A);Modulate->Inputs.Add(O);
                    auto* Feather=NewObject<UMaterialExpressionScalarParameter>(Mat);
                    Feather->ParameterName=TEXT("SkyCardEdgeFade");Feather->DefaultValue=0.f;
                    EditorOnly->ExpressionCollection.AddExpression(Feather);
                    FCustomInput U;U.InputName=TEXT("U");U.Input.Expression=TexCoord;
                    FCustomInput F;F.InputName=TEXT("F");F.Input.Expression=Feather;
                    Modulate->Inputs.Add(U);Modulate->Inputs.Add(F);
                }
                Modulate->Code+=TEXT("\n#if OUTPUT_GAMMA_SPACE\nreturn c*c;\n#else\nreturn lerp(c/12.92,pow((c+0.055)/1.055,2.4),step(0.04045,c));\n#endif\n");
                FCustomInput T; T.InputName=TEXT("T"); T.Input.Expression=Sample; Modulate->Inputs.Add(T);
                FCustomInput C; C.InputName=TEXT("C"); C.Input.Expression=Interpolated; Modulate->Inputs.Add(C);
                for (UMaterialExpression* E : TArray<UMaterialExpression*>{Normal,Light,Interpolated,Modulate})
                    EditorOnly->ExpressionCollection.AddExpression(E);
                // Leave the shared output/opacity plumbing intact.
                EmissiveMul->A.Expression=Modulate;
                EmissiveMul->B.Expression=nullptr; EmissiveMul->ConstB=1.f;
            }

			UMaterialExpression* OpacityOut = Sample;
			int32 OpacityOutIndex = 4;
			if (!bOpacityTexAlphaOnly)
			{
				UMaterialExpressionMultiply* Opacity = NewObject<UMaterialExpressionMultiply>(Mat);
				Opacity->A.Expression = Sample;
				Opacity->A.OutputIndex = 4;
				Opacity->B.Expression = EffectiveOpacity;
				Opacity->MaterialExpressionEditorX = -150;
				Opacity->MaterialExpressionEditorY = 160;
				EditorOnly->ExpressionCollection.AddExpression(Opacity);
				OpacityOut = Opacity;
				OpacityOutIndex = 0;
			}

			// Retail additive = color * intensity. Do NOT also multiply emissive by
			// OpacityMul on world FX — that double-dimmed Town Network swirls / spell FX
			// (emissive × OpacityMul × opacity(tex.A × OpacityMul)).
			// Sky shells opt in: Unlit translucency often ignores Opacity, so the
			// starfield (opaque-A black + dots) stayed visible all day.
			// Particle additive: EmissiveTimesOpacity fades StartTrans (Unlit often
			// ignores Opacity) but Opacity stays tex.A so we do not square OpacityMul.
			UMaterialExpression* EmissiveOut = EmissiveMul;
			if (bEmissiveTimesOpacity)
			{
				UMaterialExpressionMultiply* EmisOpac = NewObject<UMaterialExpressionMultiply>(Mat);
				EmisOpac->A.Expression = EmissiveMul;
				EmisOpac->B.Expression = EffectiveOpacity;
				EmisOpac->MaterialExpressionEditorX = 200;
				EmisOpac->MaterialExpressionEditorY = 0;
				EditorOnly->ExpressionCollection.AddExpression(EmisOpac);
				EmissiveOut = EmisOpac;
			}
			EditorOnly->EmissiveColor.Expression = EmissiveOut;
			if (bEmissiveTimesTexAlpha)
			{
				// Unlit often ignores the Opacity pin, so sprite holes must live in
				// emissive (frost / smoke / spark quads with shape in tex.A).
				UMaterialExpressionMultiply* EmisA = NewObject<UMaterialExpressionMultiply>(Mat);
				EmisA->A.Expression = EmissiveOut;
				EmisA->B.Expression = Sample;
				EmisA->B.OutputIndex = 4;
				EmisA->MaterialExpressionEditorX = 350;
				EmisA->MaterialExpressionEditorY = 0;
				EditorOnly->ExpressionCollection.AddExpression(EmisA);
				EditorOnly->EmissiveColor.Expression = EmisA;
			}
			EditorOnly->Opacity.Expression = OpacityOut;
			EditorOnly->Opacity.OutputIndex = OpacityOutIndex;
            if (bRetailSky && Kind==EACEAceMaterialKind::Additive)
            {
                auto* One=NewObject<UMaterialExpressionConstant>(Mat);One->R=1.f;
                EditorOnly->ExpressionCollection.AddExpression(One);
                EditorOnly->Opacity.Expression=One;EditorOnly->Opacity.OutputIndex=0;
            }
            if (bRetailParticle)
            {
                // D3DPolyRender::SetSurface uses SRCALPHA/ONE for Alpha|Additive.
                // Enabled desktop views accumulate display-space source colors
                // in the reserved particle pass, then composite after tone mapping.
                // Cancel pre-exposure here so exposure cannot tint/scale that data.
                // Mobile LDR already blends in display space; raw HDR captures
                // and other views retain their ordinary linear contribution.
                auto* Brightness = NewObject<UMaterialExpressionScalarParameter>(Mat);
                Brightness->ParameterName = TEXT("ParticleBrightness");
                Brightness->DefaultValue = 1.f;
                auto* Source = NewObject<UMaterialExpressionCustom>(Mat);
                Source->OutputType = CMOT_Float3;
                Source->Code = TEXT("float3 t=lerp(T*12.92,1.055*pow(max(T,0.0),1.0/2.4)-0.055,step(0.0031308,T)); float3 c=saturate(t*D*L)*saturate(A*O);\n#if OUTPUT_GAMMA_SPACE\nreturn c*c*max(B,0.0);\n#else\nif ((View.PostVolumeUserFlags & 1073741824) != 0) return c * max(B,0.0) * View.OneOverPreExposure;\nreturn T*D*L*saturate(A*O)*max(B,0.0);\n#endif\n");
                const auto Input = [&](const TCHAR* Name, UMaterialExpression* Expression, int32 Index=0)
                { FCustomInput I; I.InputName=Name; I.Input.Expression=Expression; I.Input.OutputIndex=Index; Source->Inputs.Add(I); };
                Input(TEXT("T"),Sample); Input(TEXT("D"),DiffuseStrength); Input(TEXT("L"),EmissiveStrength);
                Input(TEXT("A"),Sample,4); Input(TEXT("O"),EffectiveOpacity);
                Input(TEXT("B"),Brightness);
                auto* One = NewObject<UMaterialExpressionConstant>(Mat); One->R=1.f;
                EditorOnly->ExpressionCollection.AddExpression(Brightness);
                EditorOnly->ExpressionCollection.AddExpression(Source);
                EditorOnly->ExpressionCollection.AddExpression(One);
                EditorOnly->EmissiveColor.Expression=Source;
                // Alpha is already included in the source contribution above.
                EditorOnly->Opacity.Expression=One; EditorOnly->Opacity.OutputIndex=0;
            }
            if (bSurfaceShadow)
            {
                // World glass/crystal keeps continuous visible transparency,
                // while the ordinary dynamic shadow pass uses its alpha cutout.
                // Sky, weather and particle bases do not enable this path.
                Mat->bCastDynamicShadowAsMasked=true;
                Mat->OpacityMaskClipValue=.1f;
                EditorOnly->OpacityMask.Expression=OpacityOut;
                EditorOnly->OpacityMask.OutputIndex=OpacityOutIndex;
            }
		}

		if (Kind == EACEAceMaterialKind::Opaque || Kind == EACEAceMaterialKind::Masked)
		{
			// Per-primitive PixelDepthOffset (CustomPrimitiveData.0). EnvCell outdoor peeks
			// push interiors behind Setup shells; default 0 leaves characters/furniture alone.
			UMaterialExpressionScalarParameter* PrimPdo = NewObject<UMaterialExpressionScalarParameter>(Mat);
			PrimPdo->ParameterName = TEXT("PrimitiveDepthBias");
			PrimPdo->DefaultValue = 0.f;
			PrimPdo->bUseCustomPrimitiveData = true;
			PrimPdo->PrimitiveDataIndex = 0;
			PrimPdo->MaterialExpressionEditorX = -450;
			PrimPdo->MaterialExpressionEditorY = 700;
			EditorOnly->ExpressionCollection.AddExpression(PrimPdo);
			EditorOnly->PixelDepthOffset.Expression = PrimPdo;
		}

        if (!bRetailSky && !bRetailParticle && !bStaticVertexLighting && !bExaminationLighting)
        {
            // Per-object selection in the existing surface pass. Mask/opacity
            // and depth remain unchanged: no rectangle, extra mesh or full-screen
            // post-process, and no outline visible through a wall.
            auto* Strength=NewObject<UMaterialExpressionScalarParameter>(Mat);
            Strength->ParameterName=TEXT("ACESelectionStrength"); Strength->DefaultValue=0.f;
            Strength->bUseCustomPrimitiveData=true; Strength->PrimitiveDataIndex=1;
            auto* Normal=NewObject<UMaterialExpressionPixelNormalWS>(Mat);
            auto* View=NewObject<UMaterialExpressionCameraVectorWS>(Mat);
            auto* Highlight=NewObject<UMaterialExpressionCustom>(Mat);
            Highlight->OutputType=CMOT_Float3;
            Highlight->Code=TEXT("float rim=1-saturate(abs(dot(N,V))); return saturate(S)*float3(0.65,0.35,0.06)*(0.18+rim*rim*0.82);");
            for (const auto& Pair : {TPair<FName,UMaterialExpression*>(TEXT("S"),Strength),{TEXT("N"),Normal},{TEXT("V"),View}})
            { FCustomInput Input; Input.InputName=Pair.Key; Input.Input.Expression=Pair.Value; Highlight->Inputs.Add(Input); }
            auto* Sum=NewObject<UMaterialExpressionAdd>(Mat);
            Sum->A=EditorOnly->EmissiveColor; Sum->B.Expression=Highlight;
            for (UMaterialExpression* Expression : TArray<UMaterialExpression*>{Strength,Normal,View,Highlight,Sum})
                EditorOnly->ExpressionCollection.AddExpression(Expression);
            EditorOnly->EmissiveColor.Expression=Sum; EditorOnly->EmissiveColor.OutputIndex=0;
        }

        if (bRetailSky && !bDisableDepthTest)
        {
            // GameSky draws before terrain without depth writes. Our late sky
            // pass must use hardware depth testing: a resolved SceneDepth test
            // turns partially covered MSAA edge pixels into all-or-nothing holes.
            // Move vertices out along their eye ray, preserving the exact screen
            // projection and authored layer order, behind the streamed world.
            if (Mat->BlendMode == BLEND_Opaque) Mat->BlendMode = BLEND_Translucent;
            Mat->TranslucencyLightingMode = TLM_Surface;
            Mat->bDisableDepthTest = false;
            auto* Position=NewObject<UMaterialExpressionWorldPosition>(Mat);
            Position->WorldPositionShaderOffset=WPT_ExcludeAllShaderOffsets;
            auto* Eye=NewObject<UMaterialExpressionCameraPositionWS>(Mat);
            auto* Ray=NewObject<UMaterialExpressionSubtract>(Mat);
            Ray->A.Expression=Position;Ray->B.Expression=Eye;
            auto* Offset=NewObject<UMaterialExpressionMultiply>(Mat);
            Offset->A.Expression=Ray;Offset->ConstB=1023.f;
            EditorOnly->ExpressionCollection.AddExpression(Position);
            EditorOnly->ExpressionCollection.AddExpression(Eye);
            EditorOnly->ExpressionCollection.AddExpression(Ray);
            EditorOnly->ExpressionCollection.AddExpression(Offset);
            EditorOnly->WorldPositionOffset.Expression=Offset;
        }

		if (bRetailSky)
		{
			// Respect each DAT object's clamp/wrap addressing, including moon cards.
			Sample->SamplerSource = SSM_FromTextureAsset;
			if (Kind == EACEAceMaterialKind::Translucent || Kind == EACEAceMaterialKind::Additive)
			{
				auto* Ref = NewObject<UMaterialExpressionScalarParameter>(Mat);
				Ref->ParameterName = TEXT("SkyAlphaTestReference"); Ref->DefaultValue = 0.f;
				auto* Cutout = NewObject<UMaterialExpressionCustom>(Mat);
				Cutout->OutputType = CMOT_Float1; Cutout->Code = TEXT("clip(A-R); return O;");
				FCustomInput A; A.InputName=TEXT("A"); A.Input.Expression=Sample; A.Input.OutputIndex=4;
				FCustomInput R; R.InputName=TEXT("R"); R.Input.Expression=Ref;
				FCustomInput O; O.InputName=TEXT("O"); O.Input.Expression=EditorOnly->Opacity.Expression; O.Input.OutputIndex=EditorOnly->Opacity.OutputIndex;
				Cutout->Inputs={A,R,O};
				EditorOnly->ExpressionCollection.AddExpression(Ref); EditorOnly->ExpressionCollection.AddExpression(Cutout);
				EditorOnly->Opacity.Expression=Cutout; EditorOnly->Opacity.OutputIndex=0;
			}
		}
		Mat->PreEditChange(nullptr);
		Mat->UpdateCachedExpressionData();
		Mat->PostEditChange();
		ApplyMeshUsages(Mat);
#if WITH_EDITOR
		Mat->ForceRecompileForRendering();
#endif
		return Mat;
#else
		return LoadCookedAceMaterial(Name);
#endif
	}
}

UMaterialInterface* FACELegionsSkyRenderer::MaterialParent(bool bAdditive)
{
	return CreateAceOverlayMaterial(bAdditive ? TEXT("M_ACELegionsSkyAdd_v2") : TEXT("M_ACELegionsSkyAlpha_v1"),
		bAdditive ? EACEAceMaterialKind::LegionsSkyAdd : EACEAceMaterialKind::LegionsSkyAlpha,
		/*bDisableFog*/true, /*bDisableDepthTest*/false, /*bForceUvWrap*/true, /*bEmissiveTimesOpacity*/false, /*bTwoSided*/true);
}

UMaterialInterface* UACEDatSubsystem::GetVRComfortMaterial()
{
	static const TCHAR* Name = TEXT("M_ACEVRComfort_v1");
#if WITH_EDITORONLY_DATA
	if (auto* Existing = FindObject<UMaterial>(GetTransientPackage(), Name)) return Existing;
	auto* Material = NewObject<UMaterial>(GetTransientPackage(), Name, RF_Public | RF_Transient);
	Material->MaterialDomain = MD_Surface;
	Material->SetShadingModel(MSM_Unlit);
	// Premultiplied alpha explicitly preserves coverage on mobile Substrate's
	// unlit path. A black additive contribution cannot obscure the world.
	Material->BlendMode = BLEND_AlphaComposite;
	Material->TwoSided = true;
	Material->bDisableDepthTest = true;
	Material->bUseTranslucencyVertexFog = false;
	auto* Black = NewObject<UMaterialExpressionConstant>(Material);
	Black->R = 0.f;
	auto* Opacity = NewObject<UMaterialExpressionScalarParameter>(Material);
	Opacity->ParameterName = TEXT("OpacityMul"); Opacity->DefaultValue = 1.f;
	auto* Data = Material->GetEditorOnlyData();
	Data->ExpressionCollection.AddExpression(Black);
	Data->ExpressionCollection.AddExpression(Opacity);
	Data->EmissiveColor.Expression = Black;
	Data->Opacity.Expression = Opacity;
	Material->SetUsageByFlag(MATUSAGE_StaticMesh, true);
	Material->PreEditChange(nullptr);
	Material->UpdateCachedExpressionData();
	Material->PostEditChange();
	return Material;
#else
	return LoadCookedAceMaterial(Name);
#endif
}

UMaterialParameterCollection* UACEDatSubsystem::GetRuntimeFogCollection() const
{
	return RuntimeFogCollection();
}

UMaterialParameterCollection* UACEDatSubsystem::GetRuntimePortalCollection() const
{
	return RuntimePortalCollection();
}

UMaterialParameterCollection* UACEDatSubsystem::GetRuntimeFarSceneryCollection() const
{
	return RuntimeFarSceneryCollection();
}

void UACEDatSubsystem::SetFarSceneryFadeRadius(UWorld* World, float RadiusCm, bool bEnabled, const FVector& Anchor)
{
	if (World)
		if (auto* Collection = RuntimeFarSceneryCollection())
			if (auto* Instance = World->GetParameterCollectionInstance(Collection))
				Instance->SetVectorParameterValue(TEXT("FarSceneryLimits"), FLinearColor(FMath::Max(RadiusCm, 0.f), bEnabled ? 1.f : 0.f, Anchor.X, Anchor.Y));
}

TArray<UMaterialInterface*> UACEDatSubsystem::GetRuntimeMaterialParents()
{
	TArray<UMaterialInterface*> Parents = {
		EnsureInvisibleCollisionMaterial(), GetVertexColorMaterial(), GetTerrainOverlayMaterial(), EnsureAceUnlitTexturedMaterialBase(), EnsureAceLandMaterialBase(),
		GetPortalStencilWriterMaterial(), GetPortalDepthApertureMaterial(),
		EnsureAceBuildingShellMaterialBase(), EnsureAceUnlitMaskedMaterialBase(), EnsureAceOutdoorLitMaterialBase(),
		EnsureAceOutdoorLitMaskedMaterialBase(), EnsureAceUnlitTranslucentMaterialBase(), EnsureAceUnlitAdditiveMaterialBase(),
		EnsureFarSceneryMaterialBase(0), EnsureFarSceneryMaterialBase(1), EnsureFarSceneryMaterialBase(2),
		EnsureFarSceneryMaterialBase(3), EnsureFarSceneryMaterialBase(4),
		EnsureAceParticleTranslucentMaterialBase(), EnsureAceParticleAdditiveMaterialBase(),
		EnsureAceBatchedParticleMaterialBase(false), EnsureAceBatchedParticleMaterialBase(true),
		EnsureAceSkyTranslucentMaterialBase(), EnsureAceSkyTranslucentWrapMaterialBase(), EnsureAceSkyOpaqueMaterialBase(),
		EnsureAceSkyAdditiveMaterialBase(), EnsureAceSkyVertexColorMaterialBase(), EnsureAceSkyColorFillMaterialBase(),
		FACELegionsSkyRenderer::MaterialParent(false), FACELegionsSkyRenderer::MaterialParent(true),
		EnsureAceWeatherTranslucentMaterialBase(), EnsureAceWeatherAdditiveMaterialBase(), GetVRComfortMaterial(),
		UACEVRWidgetComponent::GetFilteredMaterial(),
		ACEVRPointerVisuals::GetMaterial(false), ACEVRPointerVisuals::GetMaterial(true)
	};
	for (bool bMasked : { false, true })
	{
		const auto Kind = bMasked ? EACEAceMaterialKind::Masked : EACEAceMaterialKind::Opaque;
		Parents.Add(CreateAceOverlayMaterial(bMasked ? TEXT("M_ACEEnvCellMasked_v3") : TEXT("M_ACEEnvCell_v3"),
			Kind, false, false, false, false, false, true, false, false, true));
		for (bool bTwoSided : { false, true })
		{
			const uint8 Key = uint8(bMasked ? BLEND_Masked : BLEND_Opaque) | (bTwoSided ? 0x80 : 0);
			Parents.Add(CreateAceOverlayMaterial(*FString::Printf(TEXT("M_ACEInteriorObject_%u"), Key),
				Kind, false, false, false, false, bTwoSided));
			Parents.Add(CreateAceOverlayMaterial(*FString::Printf(TEXT("M_ACEExamination_v2_%u"), Key),
				Kind, true, false, false, false, bTwoSided, false, false, false, false, true));
		}
	}
	return Parents;
}

bool UACEDatSubsystem::PrepareRuntimeMaterials()
{
#if WITH_EDITORONLY_DATA
	// The editor constructs transient material graphs instead of using cooked parents.
	return true;
#else
	check(IsInGameThread());
	if (bRuntimeMaterialPreloadStarted)
	{
		return !RuntimeMaterialPreload || RuntimeMaterialPreload->HasLoadCompleted();
	}
	bRuntimeMaterialPreloadStarted = true;
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_PrepareRuntimeMaterials);
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(
		FName(TEXT("/Game/ACE/RuntimeMaterials")), Assets, false, true);
	TArray<FSoftObjectPath> Paths;
	for (const FAssetData& Asset : Assets)
	{
		Paths.Add(Asset.GetSoftObjectPath());
	}
	if (Paths.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE: runtime material registry is empty; falling back to on-demand loading"));
		return true;
	}
	// Keep this small, cooked material library resident for the session. First-use
	// LoadObject calls during character/terrain creation can then resolve in memory,
	// rather than flushing the loader while DAT indexing/cache maintenance is busy.
	const int32 Count = Paths.Num();
	const double Start = FPlatformTime::Seconds();
	RuntimeMaterialPreload = UAssetManager::GetStreamableManager().RequestAsyncLoad(MoveTemp(Paths),
		FStreamableDelegate::CreateWeakLambda(this, [this, Count, Start]()
		{
			TArray<UObject*> Loaded;
			if (RuntimeMaterialPreload) RuntimeMaterialPreload->GetLoadedAssets(Loaded);
			int32 LoadedCount = 0;
			for (UObject* Asset : Loaded) if (Asset) ++LoadedCount;
			UE_LOG(LogTemp, Log, TEXT("ACE: runtime material preload complete: %d/%d assets in %.1fms"),
				LoadedCount, Count, (FPlatformTime::Seconds() - Start) * 1000.0);
			if (LoadedCount != Count)
				UE_LOG(LogTemp, Warning, TEXT("ACE: incomplete material preload; missing parents will report during appearance creation"));
		}), FStreamableManager::DefaultAsyncLoadPriority, false, false, TEXT("ACE runtime materials"));
	UE_LOG(LogTemp, Log, TEXT("ACE: asynchronously preparing %d runtime material assets"), Count);
	return !RuntimeMaterialPreload || RuntimeMaterialPreload->HasLoadCompleted();
#endif
}

void UACEDatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	DisplayView = CreateACEDisplayView(GetGameInstance());
	PrepareRuntimeMaterials();
#if PLATFORM_ANDROID || PLATFORM_LINUX
    // DAT files are supplied separately in the app's writable saved-data folder.
	DatDirectory = FPaths::ProjectSavedDir() / TEXT("DAT");
    UE_LOG(LogTemp, Log, TEXT("AC DAT directory: %s"), *DatDirectory);
#endif
	FACELoginSettings LoginSettings;
	if (ACELoginSettings::Load(LoginSettings) && !LoginSettings.DatDirectory.IsEmpty())
		DatDirectory = LoginSettings.DatDirectory;
	if (bAutoLoadOnInitialize)
	{
		BeginBackgroundLoad();
	}
}

void UACEDatSubsystem::Deinitialize()
{
	DisplayView.Reset();
	RetailParticleView.Reset();
	if (RuntimeMaterialPreload) RuntimeMaterialPreload->CancelHandle();
	RuntimeMaterialPreload.Reset();
	bRuntimeMaterialPreloadStarted = false;
	++BackgroundLoadGeneration; // invalidate any in-flight worker callback
	bBackgroundLoadInProgress = false;
	ClearLoadedState();
	Super::Deinitialize();
}

void UACEDatSubsystem::TrackRuntimeTexture(UTexture2D* Tex)
{
	if (Tex)
	{
		RuntimeTextureKeep.Add(Tex);
	}
}

void UACEDatSubsystem::UntrackRuntimeTexture(UTexture2D* Tex)
{
	if (Tex)
	{
		RuntimeTextureKeep.Remove(Tex);
	}
}

void UACEDatSubsystem::Tick(float DeltaTime)
{
	if (!bPortalLoaded || !bWorldStreamingAllowed)
	{
		return;
	}
	MemoryTrimAccum += DeltaTime;
	if (MemoryTrimAccum >= 2.f)
	{
		MemoryTrimAccum = 0.0;
		TrimMemoryCaches();
	}
}

FString UACEDatSubsystem::DescribeResourceUsage() const
{
    const int32 Materials=TexturedMaterialCache.Num()+BuildingShellMaterialCache.Num()+OutdoorLitMaterialCache.Num()+EnvCellMaterialCache.Num()+ParticleMaterialCache.Num();
    return FString::Printf(TEXT("DAT resources: terrain=%d cells=%d setupMeshes=%d staticMeshes=%d surfaceMaterials=%d liveSurfaceKeys=%d appearanceMaterials=%d appearanceTextures=%d"),
        LandblockCache.Num(),EnvCellMeshCache.Num(),SetupMeshCache.Num(),SetupStaticMeshCache.Num(),Materials,SurfaceMaterials.Num(),ResolvedMaterialCache.Num(),ResolvedTextureCache.Num());
}

void UACEDatSubsystem::TrimMemoryCaches()
{
	ACE_PROFILE_SCOPE(DatMaintenance);
#if PLATFORM_ANDROID
	TrimTextureResolverCaches(512, 256, 192ll << 20);
	TrimSetupMeshCache(512);
	TrimSetupStaticMeshCache(256);
	TrimResolvedTextureCache(384);
#else
	TrimTextureResolverCaches(1024, 512, 768ll << 20);
	TrimSetupMeshCache(2048);
	TrimSetupStaticMeshCache(1024);
	TrimResolvedTextureCache(1024);
#endif
	if (Builder)
	{
		Builder->TrimGfxCache(256);
	}
}

void UACEDatSubsystem::RetireWorldMeshesOutside(const TSet<int32>& KeepLandblockIds, float WorldScale)
{
	CancelLandblockMeshRequestsOutside(KeepLandblockIds);
	EvictLandblockMeshesOutside(KeepLandblockIds);
	TSet<int32> KeepCells;
	auto KeepCell = [&](uint64 Key)
	{
		const uint32 Cell = static_cast<uint32>(Key >> 32);
		if (KeepLandblockIds.Contains(static_cast<int32>(Cell & 0xFFFF0000u))) KeepCells.Add(Cell);
	};
	for (const auto& Pair : EnvCellMeshCache) KeepCell(Pair.Key);
	for (uint64 Key : PendingEnvCellKeys) KeepCell(Key);
	CancelEnvCellMeshRequestsOutside(KeepCells);
	EvictEnvCellMeshesOutside(KeepCells, WorldScale);
}

void UACEDatSubsystem::LogLandTextureMemory(const TCHAR* Phase) const
{
	int32 Count = 0;
	uint64 Bytes = 0;
	for (const auto& Pair : LandTextureCache)
	{
		if (const UTexture2D* Texture = Pair.Value.Get())
		{
			++Count;
			Bytes += Texture->CalcTextureMemorySizeEnum(TMC_AllMips);
		}
	}
	UE_LOG(LogTemp, Log, TEXT("ACE: portal resources %s: land textures=%d %.1f MiB; terrain meshes=%d room meshes=%d"),
		Phase, Count, Bytes / 1048576.0, LandblockCache.Num(), EnvCellMeshCache.Num());
}

void UACEDatSubsystem::ClearLoadedState()
{
	bPendingDiskCacheMaintenance = false;
	// Invalidate in-flight landblock workers, then wait so they release DAT handles.
	++LandblockBuildGeneration;
	PendingLandblockBuilds.Reset();
	PendingLandblockIds.Reset();
    CancelledLandblockIds.Reset();
	FailedLandblockMeshIds.Reset();
	++SetupBuildGeneration;
	PendingSetupBuilds.Reset();
	PendingSetupKeys.Reset();
	FailedSetupKeys.Reset();
	++EnvCellBuildGeneration;
	PendingEnvCellBuilds.Reset();
	PendingEnvCellKeys.Reset();
    CancelledEnvCellKeys.Reset();
	FailedEnvCellKeys.Reset();
	CachedDatFingerprint = 0;
	const double WaitStart = FPlatformTime::Seconds();
	while ((ActiveLandblockBuilds.GetValue() > 0
		|| ActiveSetupBuilds.GetValue() > 0
		|| ActiveEnvCellBuilds.GetValue() > 0)
		&& (FPlatformTime::Seconds() - WaitStart) < 5.0)
	{
		FPlatformProcess::Sleep(0.001f);
	}

	if (TextureResolver)
	{
		TextureResolver->ReleaseTextureObjects();
	}
	SetupMeshCache.Reset();
	ClearAppearanceMeshCache();
	++LandblockMeshRevision;
	LandblockCache.Reset();
	LandblockInfoCache.Reset();
	DoorwayGeometryCache.Reset();
	SetupRuntimeMetadataCache.Reset();
	UncachedSetupRuntimeMetadata = FSetupRuntimeMetadata();
	BuildingInteriorFootprints.Reset();
	EnvCellMeshCache.Reset();
	SetupStaticMeshCache.Reset();
	SetupStaticMeshSlotMaterials.Reset();
	InvisibleCollisionMaterial = nullptr;
	RuntimeTextureKeep.Reset();
	ParticleEmitterInfoCache.Reset();
	GfxLightEstimateCache.Reset();
	PhysicsScriptCache.Reset();
	PhysicsScriptTableCache.Reset();
	WaveCache.Reset();
	SoundTableCache.Reset();
	SpellInfoCache.Reset();
	bSpellTableLoaded = false;
	SkillInfoCache.Reset();
	bSkillTableLoaded = false;
	bVitalFormulasLoaded = false;
	const uint32 DefaultVitalFormulas[3][6] = {{0,1,0,2,2,0},{0,1,0,1,2,0},{0,1,0,1,6,0}};
	FMemory::Memcpy(VitalFormulas, DefaultVitalFormulas, sizeof(VitalFormulas));
	bXpTableLoaded = false;
	AttributeXpList.Reset();
	VitalXpList.Reset();
	TrainedSkillXpList.Reset();
	SpecializedSkillXpList.Reset();
	CharacterLevelXPList.Reset();
	SceneCache.Reset();
	RegionSceneTables = FACEDatRegionSceneTables();
	bRegionSceneTablesLoaded = false;
	RegionSky = FACEDatRegionSky();
	bRegionSkyLoaded = false;
	RegionSoundInfo = FACEDatRegionSoundInfo();
	bRegionSoundLoaded = false;
	ResetSurfaceMaterialCache(TexturedMaterialCache,0);
	ResetSurfaceMaterialCache(EnvCellMaterialCache,3);
	ResetSurfaceMaterialCache(OutdoorLitMaterialCache,2);
	InteriorObjectMaterials.Reset();
	DungeonObjectMaterials.Reset();
	WorldObjectMaterials.Reset();
	FarSceneryMaterials.Reset();
	FarSceneryFixedMaterials.Reset();
	WorldLightingInstances.Reset();
	InteriorObjectMaterialBases.Reset();
	StaticMeshMaterialCache.Reset();
	ResetSurfaceMaterialCache(BuildingShellMaterialCache,1);
	ResetSurfaceMaterialCache(ParticleMaterialCache,4);
	LandMaterialCache.Reset();
	LandTextureCache.Reset();
	ResolvedMaterialCache.Reset();
	for (auto& Pair : ResolvedTextureCache)
	{
		if (Pair.Value)
		{
			Pair.Value->RemoveFromRoot();
		}
	}
	ResolvedTextureCache.Reset();
	LiveResolvedMaterials.Reset();
	LiveResolvedTextures.Reset();
	ResolvedCacheLastUse.Reset();
	ResolvedCacheClock = 0;
	MotionPlayer.Reset();
	Builder.Reset();
	LandBuilder.Reset();
	EnvCellBuilder.Reset();
	LandAtlas.Reset();
	LandGpuMaterialBase = nullptr;
	LandGpuMaterialInstance = nullptr;
	TextureResolver.Reset();
	PortalDat.Reset();
	HighResDat.Reset();
	CellDat.Reset();
	TexturedMaterialBase = nullptr;
	LandMaterialBase = nullptr;
	BuildingShellMaterialBase = nullptr;
	bLandLookOutClip = false;

	bPortalLoaded = false;
}

bool UACEDatSubsystem::LoadDatDirectory(const FString& Directory)
{
	// Prefer BeginBackgroundLoad — this sync path is kept for tools/tests only.
	// Supersede both portal and cell workers before replacing their resources.
	// Texture uploads can pump game-thread tasks; an older completion must not
	// delete the resolver out from under a UI texture/font upload in progress.
	++BackgroundLoadGeneration;
	bBackgroundLoadInProgress = false;
	bCellBackgroundLoadInProgress = false;
	DatLoadError.Reset();
	FailedDatDirectory.Reset();
	DatDirectory = Directory;
	ClearLoadedState();

	const double StartTime = FPlatformTime::Seconds();
	const FString PortalPath = FPaths::Combine(DatDirectory, TEXT("client_portal.dat"));
	PortalDat = MakeUnique<FACEDatDatabase>();
	if (!PortalDat->Open(PortalPath))
	{
		PortalDat.Reset();
		UE_LOG(LogTemp, Error, TEXT("ACEDat: failed to open portal dat at %s"), *PortalPath);
		return false;
	}

	if (bLoadHighResDat)
	{
		const FString HighResPath = FPaths::Combine(DatDirectory, TEXT("client_highres.dat"));
		if (FPaths::FileExists(HighResPath))
		{
			HighResDat = MakeUnique<FACEDatDatabase>();
			if (!HighResDat->Open(HighResPath))
			{
				HighResDat.Reset();
			}
		}
	}

	const FString CellPath = FPaths::Combine(DatDirectory, TEXT("client_cell_1.dat"));
	if (FPaths::FileExists(CellPath))
	{
		CellDat = MakeUnique<FACEDatDatabase>();
		if (!CellDat->Open(CellPath))
		{
			CellDat.Reset();
		}
	}

	TextureResolver = MakeUnique<FACEDatTextureResolver>(PortalDat.Get(), HighResDat.Get());
	TextureResolver->SetGcOwner(this);
	Builder = MakeUnique<FACESetupMeshBuilder>(PortalDat.Get(), HighResDat.Get(), TextureResolver.Get());
	LandBuilder = MakeUnique<FACELandblockMeshBuilder>(PortalDat.Get(), CellDat.Get(), TextureResolver.Get());
	EnvCellBuilder = MakeUnique<FACEEnvCellMeshBuilder>(CellDat.Get(), PortalDat.Get(), TextureResolver.Get());
	RebuildLandSurfaceAtlas();
	MotionPlayer = MakeUnique<FACEDatMotionPlayer>(PortalDat.Get());
	bPortalLoaded = true;
	UE_LOG(LogTemp, Log, TEXT("ACEDat: ready in %.2fs (portal=%d files, highres=%s, cell=%s)"),
		FPlatformTime::Seconds() - StartTime,
		PortalDat->GetFileCount(),
		HighResDat ? TEXT("yes") : TEXT("no"),
		CellDat ? TEXT("yes") : TEXT("no"));
	return true;
}

void UACEDatSubsystem::BeginBackgroundLoad(bool bRetryFailed)
{
	if (bPortalLoaded || bBackgroundLoadInProgress)
	{
		return;
	}
	// A missing installation cannot recover by re-opening it every frame. Retry
	// after an explicit login attempt or a change to the configured directory.
	if (!DatLoadError.IsEmpty() && FailedDatDirectory == DatDirectory && !bRetryFailed) return;
	DatLoadError.Reset();
	FailedDatDirectory.Reset();

	bBackgroundLoadInProgress = true;
	const int32 Generation = ++BackgroundLoadGeneration;
	const FString Dir = DatDirectory;
	bool bWantHighRes = bLoadHighResDat;
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UWorld* World = GI->GetWorld())
		{
			if (World->IsPlayInEditor())
			{
				// Login UI only needs portal textures — highres doubles DAT index RAM during PIE.
				bWantHighRes = false;
			}
		}
	}
	TWeakObjectPtr<UACEDatSubsystem> WeakThis(this);

	UE_LOG(LogTemp, Log, TEXT("ACEDat: background index starting (%s, highres=%s)"),
		*Dir, bWantHighRes ? TEXT("yes") : TEXT("no"));

	RunDatBackgroundWork([WeakThis, Dir, bWantHighRes, Generation]()
	{
		TSharedRef<UACEDatSubsystem::FBackgroundLoadResult> Result = MakeShared<UACEDatSubsystem::FBackgroundLoadResult>();
		const double StartTime = FPlatformTime::Seconds();

		const FString PortalPath = FPaths::Combine(Dir, TEXT("client_portal.dat"));
		Result->Portal = MakeUnique<FACEDatDatabase>();
		if (!Result->Portal->Open(PortalPath))
		{
			Result->Portal.Reset();
			Result->Error = FString::Printf(TEXT("failed to open portal dat at %s"), *PortalPath);
			Result->bOk = false;
		}
		else
		{
			if (bWantHighRes)
			{
				const FString HighResPath = FPaths::Combine(Dir, TEXT("client_highres.dat"));
				if (FPaths::FileExists(HighResPath))
				{
					Result->HighRes = MakeUnique<FACEDatDatabase>();
					if (!Result->HighRes->Open(HighResPath))
					{
						Result->HighRes.Reset();
					}
				}
			}

			// client_cell_1.dat (805k+ files) is deferred until world streaming is allowed —
			// opening it on the login screen blocked PIE for seconds and spiked RAM.

			Result->bOk = true;
		}

		Result->ElapsedSeconds = FPlatformTime::Seconds() - StartTime;

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Generation]()
		{
			if (UACEDatSubsystem* Self = WeakThis.Get())
			{
				Self->InstallBackgroundLoadResult(Result, Generation);
			}
		});
	});
}

void UACEDatSubsystem::SetWorldStreamingAllowed(bool bAllowed)
{
	const bool bWasAllowed = bWorldStreamingAllowed;
	bWorldStreamingAllowed = bAllowed;
	if (bAllowed && !bWasAllowed)
	{
		if (bPortalLoaded && !CellDat)
		{
			BeginCellDatBackgroundLoad();
		}
		TryStartDiskCacheMaintenance();
	}
}

void UACEDatSubsystem::BeginCellDatBackgroundLoad()
{
	if (CellDat || bCellBackgroundLoadInProgress || !bPortalLoaded || !DatLoadError.IsEmpty())
	{
		return;
	}

	bCellBackgroundLoadInProgress = true;
	const int32 Generation = ++BackgroundLoadGeneration;
	const FString Dir = DatDirectory;
	TWeakObjectPtr<UACEDatSubsystem> WeakThis(this);

	UE_LOG(LogTemp, Log, TEXT("ACEDat: cell dat background index starting (%s)"), *Dir);

	RunDatBackgroundWork([WeakThis, Dir, Generation]()
	{
		const double StartTime = FPlatformTime::Seconds();
		TUniquePtr<FACEDatDatabase> Cell;
		const FString CellPath = FPaths::Combine(Dir, TEXT("client_cell_1.dat"));
		if (FPaths::FileExists(CellPath))
		{
			Cell = MakeUnique<FACEDatDatabase>();
			if (!Cell->Open(CellPath))
			{
				Cell.Reset();
			}
		}

		const double ElapsedSeconds = FPlatformTime::Seconds() - StartTime;

		AsyncTask(ENamedThreads::GameThread, [WeakThis, CellPtr = MoveTemp(Cell), Generation, ElapsedSeconds]() mutable
		{
			const bool bCellOk = CellPtr.IsValid();
			if (UACEDatSubsystem* Self = WeakThis.Get())
			{
				Self->InstallCellDatBackgroundLoadResult(MoveTemp(CellPtr), Generation);
			}
			if (bCellOk)
			{
				UE_LOG(LogTemp, Log, TEXT("ACEDat: cell dat ready in %.2fs"), ElapsedSeconds);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("ACEDat: cell dat missing or failed to open after %.2fs"), ElapsedSeconds);
			}
		});
	});
}

void UACEDatSubsystem::InstallCellDatBackgroundLoadResult(TUniquePtr<FACEDatDatabase>&& Cell, int32 Generation)
{
	if (Generation != BackgroundLoadGeneration)
	{
		return;
	}

	bCellBackgroundLoadInProgress = false;
	if (!Cell)
	{
		DatLoadError = FString::Printf(TEXT("World data is missing or unreadable: %s. Close the game and repair the DAT installation, then restart."),
			*(DatDirectory / TEXT("client_cell_1.dat")));
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: %s"), *DatLoadError);
		return;
	}

	DatLoadError.Reset();
	CellDat = MoveTemp(Cell);
	CachedDatFingerprint = 0;
	TryStartDiskCacheMaintenance();
	if (PortalDat && TextureResolver)
	{
		LandBuilder = MakeUnique<FACELandblockMeshBuilder>(PortalDat.Get(), CellDat.Get(), TextureResolver.Get());
		EnvCellBuilder = MakeUnique<FACEEnvCellMeshBuilder>(CellDat.Get(), PortalDat.Get(), TextureResolver.Get());
	}
	// Jobs queued before CellDat existed sit in PendingLandblockBuilds; Pump returned
	// immediately. Start them now so portal space is not stuck on cell-collision.
	PumpLandblockBuildQueue();
	PumpEnvCellBuildQueue();
}

void UACEDatSubsystem::InstallBackgroundLoadResult(TSharedRef<FBackgroundLoadResult> Result, int32 Generation)
{
	if (Generation != BackgroundLoadGeneration)
	{
		return; // superseded or subsystem tearing down
	}

	bBackgroundLoadInProgress = false;

	if (!Result->bOk || !Result->Portal)
	{
		DatLoadError = Result->Error;
		FailedDatDirectory = DatDirectory;
		UE_LOG(LogTemp, Error, TEXT("ACEDat: background index failed after %.2fs — %s"),
			Result->ElapsedSeconds, *Result->Error);
		return;
	}

	// First portal install — nothing to clear. ClearLoadedState() waits on worker threads
	// and was stalling PIE login when called on a fresh GameInstance.
	if (bPortalLoaded)
	{
		ClearLoadedState();
	}

	PortalDat = MoveTemp(Result->Portal);
	HighResDat = MoveTemp(Result->HighRes);
	CellDat = MoveTemp(Result->Cell);
	TextureResolver = MakeUnique<FACEDatTextureResolver>(PortalDat.Get(), HighResDat.Get());
	TextureResolver->SetGcOwner(this);
	Builder = MakeUnique<FACESetupMeshBuilder>(PortalDat.Get(), HighResDat.Get(), TextureResolver.Get());
	LandBuilder = MakeUnique<FACELandblockMeshBuilder>(PortalDat.Get(), CellDat.Get(), TextureResolver.Get());
	EnvCellBuilder = MakeUnique<FACEEnvCellMeshBuilder>(CellDat.Get(), PortalDat.Get(), TextureResolver.Get());
	RebuildLandSurfaceAtlas();
	MotionPlayer = MakeUnique<FACEDatMotionPlayer>(PortalDat.Get());
	bPortalLoaded = true;
	AppliedSurfaceUnpackVersion = 88; // must match EnsureLoaded SurfaceUnpackVersion
	// Keep world streaming off until the player enters the world with a character.
	bWorldStreamingAllowed = false;

	UE_LOG(LogTemp, Log, TEXT("ACEDat: background index ready in %.2fs (portal=%d files, highres=%s, cell=deferred)"),
		Result->ElapsedSeconds,
		PortalDat->GetFileCount(),
		HighResDat ? TEXT("yes") : TEXT("no"));

	CachedDatFingerprint = 0;
	bPendingDiskCacheMaintenance = true;
	TryStartDiskCacheMaintenance();

	UE_LOG(LogTemp, Log, TEXT("ACEDat: portal install complete (game thread)"));
}

void UACEDatSubsystem::TryStartDiskCacheMaintenance()
{
	// Login indexes the portal first and defers the cell DAT until world entry.
	// A portal-only fingerprint must never purge the full world's disk caches.
	if (!bPendingDiskCacheMaintenance || !PortalDat || !CellDat || !bWorldStreamingAllowed) return;
	bPendingDiskCacheMaintenance = false;
	const uint64 Fp = ComputeDatFingerprint();
	RunDatBackgroundWork([Fp]()
	{
		ACEDiskTileCache::MaintainAfterDatLoad(Fp);
	});
}

bool UACEDatSubsystem::EnsureLoaded()
{
	if (bPortalLoaded)
	{
		static constexpr int32 SurfaceUnpackVersion = 88; // v88: world textures back to RGBA (DXT transients were broken)
		if (AppliedSurfaceUnpackVersion != SurfaceUnpackVersion)
		{
			SetupMeshCache.Reset();
			ClearAppearanceMeshCache();
			SetupStaticMeshCache.Reset();
			SetupStaticMeshSlotMaterials.Reset();
			InvisibleCollisionMaterial = nullptr;
			RuntimeTextureKeep.Reset();
			++LandblockMeshRevision;
			LandblockCache.Reset();
			LandblockInfoCache.Reset();
			DoorwayGeometryCache.Reset();
			SetupRuntimeMetadataCache.Reset();
			UncachedSetupRuntimeMetadata = FSetupRuntimeMetadata();
			BuildingInteriorFootprints.Reset();
			EnvCellMeshCache.Reset();
			// Rebuild EnvCellBuilder so EnvironmentCache re-unpacks CellBSP nodes.
			if (CellDat && PortalDat && TextureResolver)
			{
				EnvCellBuilder = MakeUnique<FACEEnvCellMeshBuilder>(CellDat.Get(), PortalDat.Get(), TextureResolver.Get());
			}
			// Mesh caches only — do NOT ReleaseTextureObjects / null sky bases (orphans sky MIDs).
			ResetSurfaceMaterialCache(TexturedMaterialCache,0);
			ResetSurfaceMaterialCache(EnvCellMaterialCache,3);
			ResetSurfaceMaterialCache(OutdoorLitMaterialCache,2);
			StaticMeshMaterialCache.Reset();
			ResetSurfaceMaterialCache(BuildingShellMaterialCache,1);
			ResetSurfaceMaterialCache(ParticleMaterialCache,4);
			LandMaterialCache.Reset();
			LookOutLandMaterialCache.Reset();
			LandTextureCache.Reset();
			ResolvedMaterialCache.Reset();
			LiveResolvedMaterials.Reset();
			TexturedMaterialBase = nullptr;
			LandMaterialBase = nullptr;
			LandLookOutMaterialBase = nullptr;
			BuildingShellMaterialBase = nullptr;
			PortalStencilWriterMaterial = nullptr;
			bPortalLookOutLand = false;
			bLandLookOutClip = false;

			MaskedMaterialBase = nullptr;
			TranslucentMaterialBase = nullptr;
			AdditiveMaterialBase = nullptr;
			ParticleAdditiveMaterialBase = nullptr;
			ParticleTranslucentMaterialBase = nullptr;
			// v87: 13bd InstOpac was compiled into Sky*/Weather* — must rebuild or PIE
			// samples PerInstanceCustomData[0] on meshes with an empty custom-data array.
			SkyTranslucentMaterialBase = nullptr;
			SkyTranslucentWrapMaterialBase = nullptr;
			SkyOpaqueMaterialBase = nullptr;
			SkyAdditiveMaterialBase = nullptr;
			SkyVertexColorMaterialBase = nullptr;
			SkyColorFillMaterialBase = nullptr;
			WeatherTranslucentMaterialBase = nullptr;
			WeatherAdditiveMaterialBase = nullptr;
			GfxLightEstimateCache.Reset();
			if (TextureResolver)
			{
				TextureResolver->InvalidateSurfaceCache();
				TextureResolver->DiscardNonSkyRuntimeTextures();
				// Intentionally NOT ReleaseTextureObjects — destroys UTextures sky still references.
			}
			AppliedSurfaceUnpackVersion = SurfaceUnpackVersion;
			bLandMeshReloadRequested = true;
			++SkyMaterialGeneration;
			UE_LOG(LogTemp, Log,
				TEXT("ACEDat: flushed mesh/sky overlay caches after Surface unpack fix (v%d) — world textures RGBA"),
				SurfaceUnpackVersion);
		}
		return true;
	}

	// Never block the game thread indexing DATs — kick a worker and let callers retry.
	BeginBackgroundLoad();
	return false;
}

void UACEDatSubsystem::RebuildLandSurfaceAtlas()
{
	LandGpuMaterialInstance = nullptr;
	LandGpuMaterialBase = nullptr;
	LandAtlas.Reset();
	if (LandBuilder)
	{
		LandBuilder->SetLandSurfaceAtlas(nullptr);
	}
	// Full Texture2DArray atlas build is deferred: the GPU land material is disabled,
	// and decoding every terrain/alpha surface at DAT open was expensive. Re-enable
	// when bEnableGpuLandMaterial is turned back on in GetOrCreateLandGpuMaterial.
	UE_LOG(LogTemp, Log, TEXT("ACEDat: LandSurfaceAtlas skipped (CPU TexMerge bake active)"));
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateLandGpuMaterial()
{
	// Disabled until Texture2DArray Custom HLSL is validated — applying a broken
	// land material paints the whole ground black.
	static constexpr bool bEnableGpuLandMaterial = false;
	if (!bEnableGpuLandMaterial)
	{
		return nullptr;
	}
	if (!LandAtlas || !LandAtlas->IsReady() || !LandAtlas->GetTerrainArray())
	{
		return nullptr;
	}
	if (LandGpuMaterialInstance)
	{
		return LandGpuMaterialInstance;
	}

#if WITH_EDITORONLY_DATA
	if (!LandGpuMaterialBase)
	{
		UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ACELandGpuTexMerge_v1"), RF_Public | RF_Transient);
		if (!Mat)
		{
			return nullptr;
		}
		Mat->MaterialDomain = MD_Surface;
		Mat->TwoSided = false;
		Mat->SetShadingModel(MSM_Unlit);
		Mat->BlendMode = BLEND_Opaque;
		Mat->SetUsageByFlag(MATUSAGE_StaticMesh, true);

		UMaterialExpressionTextureSampleParameter2DArray* LandSample = NewObject<UMaterialExpressionTextureSampleParameter2DArray>(Mat);
		LandSample->ParameterName = TEXT("LandTextures");
		LandSample->SamplerType = SAMPLERTYPE_Color;
		LandSample->MaterialExpressionEditorX = -800;
		LandSample->MaterialExpressionEditorY = 0;

		UMaterialExpressionTextureSampleParameter2DArray* AlphaSample = NewObject<UMaterialExpressionTextureSampleParameter2DArray>(Mat);
		AlphaSample->ParameterName = TEXT("AlphaTextures");
		AlphaSample->SamplerType = SAMPLERTYPE_LinearColor;
		AlphaSample->MaterialExpressionEditorX = -800;
		AlphaSample->MaterialExpressionEditorY = 200;

		UMaterialExpressionTextureCoordinate* UV0 = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
		UV0->CoordinateIndex = 0;
		UV0->MaterialExpressionEditorX = -1200;
		UV0->MaterialExpressionEditorY = -100;

		UMaterialExpressionTextureCoordinate* UV1 = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
		UV1->CoordinateIndex = 1;
		UV1->MaterialExpressionEditorX = -1200;
		UV1->MaterialExpressionEditorY = 0;

		UMaterialExpressionTextureCoordinate* UV2 = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
		UV2->CoordinateIndex = 2;
		UV2->MaterialExpressionEditorX = -1200;
		UV2->MaterialExpressionEditorY = 100;

		UMaterialExpressionTextureCoordinate* UV3 = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
		UV3->CoordinateIndex = 3;
		UV3->MaterialExpressionEditorX = -1200;
		UV3->MaterialExpressionEditorY = 200;

		UMaterialExpressionVertexColor* VC = NewObject<UMaterialExpressionVertexColor>(Mat);
		VC->MaterialExpressionEditorX = -1200;
		VC->MaterialExpressionEditorY = 300;

		UMaterialExpressionCustom* Custom = NewObject<UMaterialExpressionCustom>(Mat);
		Custom->Description = TEXT("ACE Land GPU TexMerge");
		Custom->OutputType = CMOT_Float3;
		Custom->Code = TEXT(
			"float3 uvb = float3(UV0.xy, 0);\n"
			"float baseL = UV1.x;\n"
			"float o0L = UV1.y;\n"
			"float a0L = UV3.x;\n"
			"float roadL = UV3.y;\n"
			"float3 color = float3(0.35,0.4,0.25);\n"
			"if (baseL >= 0.0) { color = Texture2DArraySample(LandTex, float3(UV0.xy, baseL)).rgb; }\n"
			"if (o0L >= 0.0 && a0L >= 0.0) {\n"
			"  float3 over = Texture2DArraySample(LandTex, float3(UV0.xy, o0L)).rgb;\n"
			"  float a = Texture2DArraySample(AlphaTex, float3(UV2.xy, a0L)).r;\n"
			"  float w = 1.0 - saturate(a);\n"
			"  color = lerp(color, over, w);\n"
			"}\n"
			"if (roadL >= 0.0) {\n"
			"  float3 road = Texture2DArraySample(LandTex, float3(UV0.xy, roadL)).rgb;\n"
			"  float ra = Texture2DArraySample(AlphaTex, float3(VC.ba, a0L >= 0.0 ? a0L : 0.0)).r;\n"
			"  float rw = 1.0 - saturate(ra);\n"
			"  if (VC.b >= 0.0) { color = lerp(color, road, rw); }\n"
			"}\n"
			"return color;\n");
		Custom->Inputs.Reset();
		{
			FCustomInput In;
			In.InputName = TEXT("LandTex");
			In.Input.Expression = LandSample;
			Custom->Inputs.Add(In);
		}
		{
			FCustomInput In;
			In.InputName = TEXT("AlphaTex");
			In.Input.Expression = AlphaSample;
			Custom->Inputs.Add(In);
		}
		{
			FCustomInput In;
			In.InputName = TEXT("UV0");
			In.Input.Expression = UV0;
			Custom->Inputs.Add(In);
		}
		{
			FCustomInput In;
			In.InputName = TEXT("UV1");
			In.Input.Expression = UV1;
			Custom->Inputs.Add(In);
		}
		{
			FCustomInput In;
			In.InputName = TEXT("UV2");
			In.Input.Expression = UV2;
			Custom->Inputs.Add(In);
		}
		{
			FCustomInput In;
			In.InputName = TEXT("UV3");
			In.Input.Expression = UV3;
			Custom->Inputs.Add(In);
		}
		{
			FCustomInput In;
			In.InputName = TEXT("VC");
			In.Input.Expression = VC;
			Custom->Inputs.Add(In);
		}
		Custom->MaterialExpressionEditorX = -400;
		Custom->MaterialExpressionEditorY = 0;

		UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
		if (!EditorOnly)
		{
			return nullptr;
		}
		EditorOnly->ExpressionCollection.AddExpression(LandSample);
		EditorOnly->ExpressionCollection.AddExpression(AlphaSample);
		EditorOnly->ExpressionCollection.AddExpression(UV0);
		EditorOnly->ExpressionCollection.AddExpression(UV1);
		EditorOnly->ExpressionCollection.AddExpression(UV2);
		EditorOnly->ExpressionCollection.AddExpression(UV3);
		EditorOnly->ExpressionCollection.AddExpression(VC);
		EditorOnly->ExpressionCollection.AddExpression(Custom);
		EditorOnly->BaseColor.Expression = Custom;
		Mat->PreEditChange(nullptr);
		Mat->PostEditChange();
		LandGpuMaterialBase = Mat;
	}

	if (LandGpuMaterialBase)
	{
		LandGpuMaterialInstance = UMaterialInstanceDynamic::Create(LandGpuMaterialBase, this);
		if (LandGpuMaterialInstance)
		{
			LandGpuMaterialInstance->SetTextureParameterValue(TEXT("LandTextures"), Cast<UTexture>(LandAtlas->GetTerrainArray()));
			LandGpuMaterialInstance->SetTextureParameterValue(TEXT("AlphaTextures"), Cast<UTexture>(LandAtlas->GetAlphaArray()));
			return LandGpuMaterialInstance;
		}
	}
#endif
	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::GetTerrainOverlayMaterial()
{
    if(!TerrainOverlayMaterial)TerrainOverlayMaterial=CreateAceOverlayMaterial(
        TEXT("M_ACETerrainOverlay_v1"),EACEAceMaterialKind::VertexColorAlpha,
        true,false,false,false,true);
    return TerrainOverlayMaterial;
}

UMaterialInterface* UACEDatSubsystem::GetVertexColorMaterial()
{
	if (VertexColorMaterial)
	{
		return VertexColorMaterial;
	}

    // Fallback geometry still carries DAT colors. Ship this shader with the client;
    // EngineDebugMaterials are excluded from normal cooking.
    VertexColorMaterial = CreateAceOverlayMaterial(TEXT("M_ACEVertexColor_v1"),
        EACEAceMaterialKind::VertexColorOpaque);
    if (!VertexColorMaterial)
        VertexColorMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
    return VertexColorMaterial;
}

UMaterialInterface* UACEDatSubsystem::EnsureInvisibleCollisionMaterial()
{
	if (InvisibleCollisionMaterial)
	{
		return InvisibleCollisionMaterial;
	}
#if WITH_EDITORONLY_DATA
	if (UMaterial* Existing = FindObject<UMaterial>(GetTransientPackage(), TEXT("M_ACEInvisibleCollision_v1")))
	{
		InvisibleCollisionMaterial = Existing;
		return Existing;
	}
	UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ACEInvisibleCollision_v1"), RF_Public | RF_Transient);
	if (!Mat)
	{
		return nullptr;
	}
	Mat->MaterialDomain = MD_Surface;
	Mat->TwoSided = true;
	Mat->SetShadingModel(MSM_Unlit);
	Mat->BlendMode = BLEND_Masked;
	Mat->OpacityMaskClipValue = 0.5f;
	Mat->SetUsageByFlag(MATUSAGE_StaticMesh, true);
	Mat->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	UMaterialExpressionConstant* Zero = NewObject<UMaterialExpressionConstant>(Mat);
	Zero->R = 0.f;
	Zero->MaterialExpressionEditorX = -200;
	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return nullptr;
	}
	EditorOnly->ExpressionCollection.AddExpression(Zero);
	EditorOnly->OpacityMask.Expression = Zero;
	EditorOnly->EmissiveColor.Expression = Zero;
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	Mat->CheckMaterialUsage(MATUSAGE_StaticMesh);
	Mat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
#if WITH_EDITOR
	Mat->ForceRecompileForRendering();
#endif
	InvisibleCollisionMaterial = Mat;
	UE_LOG(LogTemp, Log, TEXT("ACEDat: created invisible collision material (PhysicsBSP hulls stay off-screen)"));
	return InvisibleCollisionMaterial;
#else
	InvisibleCollisionMaterial = LoadCookedAceMaterial(TEXT("M_ACEInvisibleCollision_v1"));
	return InvisibleCollisionMaterial;
#endif
}

UMaterialInterface* UACEDatSubsystem::EnsureAceUnlitTexturedMaterialBase()
{
	static constexpr int32 WorldLitGraphVersion = 13;
	static int32 AppliedWorldLitGraphVersion = 0;
	if (AppliedWorldLitGraphVersion != WorldLitGraphVersion)
	{
		AppliedWorldLitGraphVersion = WorldLitGraphVersion;
		TexturedMaterialBase = nullptr;
		ResetSurfaceMaterialCache(TexturedMaterialCache,0);
	}
	if (TexturedMaterialBase)
	{
		return TexturedMaterialBase;
	}

	// Authored polygon sides now have separate fans, surfaces, normals and UVs.
	// Retail GfxObj walls are prelit/unlit. DefaultLit BaseColor goes black at night
	// (no CSM / failed skylight) while ClipMap trim stayed emissive — pitch-black shells.
	TexturedMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEUnlitTex_v13"), EACEAceMaterialKind::Opaque,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, /*bWorldLit*/ false);
	if (TexturedMaterialBase)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: created runtime Unlit ACETexture material v13 (authored polygon sides)"));
		return TexturedMaterialBase;
	}

	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceLandMaterialBase()
{
	// PDO / material graph bumps without a full landmesh wipe.
	static constexpr int32 LandMatGraphVersion = 42;
	static int32 AppliedLandMatGraphVersion = 0;
	if (AppliedLandMatGraphVersion != LandMatGraphVersion)
	{
		AppliedLandMatGraphVersion = LandMatGraphVersion;
		LandMaterialBase = nullptr;
		LandMaterialCache.Reset();
	}
	if (LandMaterialBase)
	{
		return LandMaterialBase;
	}

#if WITH_EDITORONLY_DATA
	// The aperture mask must run in the depth pass too. Clipping only BaseColor
	// on an opaque material leaves invisible terrain depth occluding interiors.
	UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ACELandLit_v42"), RF_Public | RF_Transient);
	if (!Mat)
	{
		return nullptr;
	}
	// World-space fog and cloud coordinates span kilometres on mobile.
	Mat->FloatPrecisionMode = MFPM_Full;
	Mat->MaterialDomain = MD_Surface;
	Mat->TwoSided = false;
	Mat->SetShadingModel(MSM_DefaultLit);
	Mat->BlendMode = BLEND_Masked;
	Mat->OpacityMaskClipValue = 0.5f;
	Mat->SetUsageByFlag(MATUSAGE_StaticMesh, true);
	Mat->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);

	UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
	Sample->ParameterName = TEXT("ACETexture");
		Sample->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	Sample->SamplerType = SAMPLERTYPE_Color;
	Sample->MaterialExpressionEditorX = -450;
	Sample->MaterialExpressionEditorY = 0;

	UMaterialExpressionScalarParameter* EmissiveStrength = NewObject<UMaterialExpressionScalarParameter>(Mat);
	EmissiveStrength->ParameterName = TEXT("EmissiveStrength");
	EmissiveStrength->DefaultValue = 0.14f;
	EmissiveStrength->MaterialExpressionEditorX = -450;
	EmissiveStrength->MaterialExpressionEditorY = 160;

	UMaterialExpressionMultiply* Emis = NewObject<UMaterialExpressionMultiply>(Mat);
	Emis->A.Expression = Sample;
	Emis->A.OutputIndex = 0;
	Emis->B.Expression = EmissiveStrength;
	Emis->MaterialExpressionEditorX = -100;
	Emis->MaterialExpressionEditorY = 0;

	UMaterialExpressionConstant* Rough = NewObject<UMaterialExpressionConstant>(Mat);
	Rough->R = 1.f;
	Rough->MaterialExpressionEditorX = -100;
	Rough->MaterialExpressionEditorY = 160;
	UMaterialExpressionConstant* Spec = NewObject<UMaterialExpressionConstant>(Mat);
	Spec->R = 0.f;
	Spec->MaterialExpressionEditorX = -100;
	Spec->MaterialExpressionEditorY = 220;
	UMaterialExpressionConstant* Metal = NewObject<UMaterialExpressionConstant>(Mat);
	Metal->R = 0.f;
	Metal->MaterialExpressionEditorX = -100;
	Metal->MaterialExpressionEditorY = 280;

	UMaterialExpressionScalarParameter* DepthBias = NewObject<UMaterialExpressionScalarParameter>(Mat);
	DepthBias->ParameterName = TEXT("LandDepthBias");
	DepthBias->DefaultValue = 0.f;
	DepthBias->MaterialExpressionEditorX = -450;
	DepthBias->MaterialExpressionEditorY = 340;

	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return nullptr;
	}
	EditorOnly->ExpressionCollection.AddExpression(Sample);
	EditorOnly->ExpressionCollection.AddExpression(EmissiveStrength);
	EditorOnly->ExpressionCollection.AddExpression(Emis);
	EditorOnly->ExpressionCollection.AddExpression(Rough);
	EditorOnly->ExpressionCollection.AddExpression(Spec);
	EditorOnly->ExpressionCollection.AddExpression(Metal);
	EditorOnly->ExpressionCollection.AddExpression(DepthBias);
	EditorOnly->EmissiveColor.Expression = Emis;
	UMaterialExpressionOneMinus* LandLitFrac = NewObject<UMaterialExpressionOneMinus>(Mat);
	LandLitFrac->Input.Expression = EmissiveStrength;
	LandLitFrac->MaterialExpressionEditorX = -100;
	LandLitFrac->MaterialExpressionEditorY = 80;
	UMaterialExpressionMultiply* LandBaseMul = NewObject<UMaterialExpressionMultiply>(Mat);
	LandBaseMul->A.Expression = Sample;
	LandBaseMul->A.OutputIndex = 0;
	LandBaseMul->B.Expression = LandLitFrac;
	LandBaseMul->MaterialExpressionEditorX = 80;
	LandBaseMul->MaterialExpressionEditorY = 80;
	EditorOnly->ExpressionCollection.AddExpression(LandLitFrac);
	EditorOnly->ExpressionCollection.AddExpression(LandBaseMul);
	UMaterialExpressionWorldPosition* LandWorldPos = NewObject<UMaterialExpressionWorldPosition>(Mat);
	// AC2 SurfaceDesc modulates terrain before fog, including its ambient term.
	// One moving map across every tile. The stock sky skips the texture fetch.
	auto* Mask = NewObject<UMaterialExpressionTextureObjectParameter>(Mat);
	Mask->ParameterName = TEXT("CloudMask");
	Mask->Texture = Sample->Texture;
	auto* Motion = NewObject<UMaterialExpressionVectorParameter>(Mat);
	Motion->ParameterName = TEXT("CloudMotion");
	Motion->DefaultValue = FLinearColor::Transparent;
	auto* CloudOffset = NewObject<UMaterialExpressionCollectionParameter>(Mat);
	CloudOffset->Collection = RuntimeFogCollection();
	CloudOffset->ParameterName = TEXT("CloudOffset");
	CloudOffset->ParameterId = CloudOffset->Collection->GetParameterId(CloudOffset->ParameterName);
	auto* Shaded = NewObject<UMaterialExpressionCustom>(Mat);
	Shaded->OutputType = CMOT_Float3;
	auto Input = [Shaded](FName Name, UMaterialExpression* Expression, int32 Output = 0)
	{
		auto& Pin = Shaded->Inputs.AddDefaulted_GetRef();
		Pin.InputName = Name;
		Pin.Input.Connect(Output,Expression);
	};
	Input(TEXT("Land"),Sample); Input(TEXT("Position"),LandWorldPos); Input(TEXT("Motion"),Motion,5);
	Input(TEXT("Offset"),CloudOffset); Input(TEXT("Mask"),Mask);
	// Motion.a follows the cloud sheet's rendered opacity, including its native
	// visibility window. Clear skies bypass the mask, and thinning clouds fade
	// the same shadow uniformly across near and far terrain.
	Shaded->Code = TEXT("[branch] if (Motion.x <= 0 || Motion.a <= 0) return Land; float2 uv=Position.xy*Motion.x+Offset.xy; return Land*lerp(1.0,Texture2DSample(Mask,MaskSampler,uv).rgb,saturate(Motion.a));");
	auto* SkyTint = NewObject<UMaterialExpressionVectorParameter>(Mat);
	SkyTint->ParameterName = TEXT("LandSkyTint");
	SkyTint->DefaultValue = FLinearColor::White;
	auto* AmbientLand = NewObject<UMaterialExpressionMultiply>(Mat);
	AmbientLand->A.Connect(0,Shaded);
	AmbientLand->B.Connect(0,SkyTint);
	LandBaseMul->A.Connect(0,Shaded);
	Emis->A.Connect(0,AmbientLand);
	for (UMaterialExpression* Expr : TArray<UMaterialExpression*>{Mask,Motion,CloudOffset,Shaded,SkyTint,AmbientLand})
		EditorOnly->ExpressionCollection.AddExpression(Expr);
	LandWorldPos->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	LandWorldPos->MaterialExpressionEditorX = -450;
	LandWorldPos->MaterialExpressionEditorY = 420;
	UMaterialExpressionScalarParameter* InteriorClipEnable = NewObject<UMaterialExpressionScalarParameter>(Mat);
	InteriorClipEnable->ParameterName = TEXT("InteriorClipEnable");
	InteriorClipEnable->DefaultValue = 0.f;
	InteriorClipEnable->MaterialExpressionEditorX = -450;
	InteriorClipEnable->MaterialExpressionEditorY = 480;
	UMaterialExpressionVectorParameter* InteriorClipMin = NewObject<UMaterialExpressionVectorParameter>(Mat);
	InteriorClipMin->ParameterName = TEXT("InteriorClipMin");
	InteriorClipMin->DefaultValue = FLinearColor::Black;
	InteriorClipMin->MaterialExpressionEditorX = -450;
	InteriorClipMin->MaterialExpressionEditorY = 540;
	UMaterialExpressionVectorParameter* InteriorClipMax = NewObject<UMaterialExpressionVectorParameter>(Mat);
	InteriorClipMax->ParameterName = TEXT("InteriorClipMax");
	InteriorClipMax->DefaultValue = FLinearColor::Black;
	InteriorClipMax->MaterialExpressionEditorX = -450;
	InteriorClipMax->MaterialExpressionEditorY = 600;
	UMaterialExpressionScalarParameter* LookOutEnable = NewObject<UMaterialExpressionScalarParameter>(Mat);
	LookOutEnable->ParameterName = TEXT("LookOutEnable");
	LookOutEnable->DefaultValue = 0.f;
	LookOutEnable->MaterialExpressionEditorX = -450;
	LookOutEnable->MaterialExpressionEditorY = 660;
	auto MakeLookVec = [&](const TCHAR* Name, int32 Y) -> UMaterialExpressionVectorParameter*
	{
		UMaterialExpressionVectorParameter* P = NewObject<UMaterialExpressionVectorParameter>(Mat);
		P->ParameterName = Name;
		P->DefaultValue = FLinearColor::Black;
		P->MaterialExpressionEditorX = -450;
		P->MaterialExpressionEditorY = Y;
		return P;
	};
	UMaterialExpressionVectorParameter* LookOutCam = MakeLookVec(TEXT("LookOutCam"), 720);
	UMaterialExpressionTextureObjectParameter* LookOutViews = NewObject<UMaterialExpressionTextureObjectParameter>(Mat);
    LookOutViews->ParameterName = TEXT("PortalViews");
    LookOutViews->SamplerType = SAMPLERTYPE_LinearColor;
    if (!LandPortalViewTexture)
    {
        LandPortalViewTexture = UTexture2D::CreateTransient(FACEPortalViewMask::TextureWidth, 1, PF_A32B32G32R32F);
        LandPortalViewTexture->SRGB = false;
        LandPortalViewTexture->NeverStream = true;
        LandPortalViewTexture->Filter = TF_Nearest;
        auto& Mip = LandPortalViewTexture->GetPlatformData()->Mips[0];
        void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memzero(Data, Mip.BulkData.GetBulkDataSize());
        Mip.BulkData.Unlock();
        LandPortalViewTexture->UpdateResource();
    }
    LookOutViews->Texture = LandPortalViewTexture;
    UMaterialExpressionScalarParameter* LookOutViewCount = NewObject<UMaterialExpressionScalarParameter>(Mat);
    LookOutViewCount->ParameterName = TEXT("PortalViewCount");
    LookOutViewCount->DefaultValue = 0.f;
	// One world uniform update replaces hundreds of per-texture MID updates on
	// every tracked head movement. Isolated previews can retain local parameters.
	auto* UseSharedPortal = NewObject<UMaterialExpressionScalarParameter>(Mat);
	UseSharedPortal->ParameterName = TEXT("UseSharedPortal");
	UseSharedPortal->DefaultValue = 0.f;
	EditorOnly->ExpressionCollection.AddExpression(UseSharedPortal);
	auto SharedPortal = [&](UMaterialExpression* Local, const TCHAR* Name, bool bVector = false)
	{
		auto* Shared = NewObject<UMaterialExpressionCollectionParameter>(Mat);
		Shared->Collection = RuntimePortalCollection(); Shared->ParameterName = Name;
		Shared->ParameterId = Shared->Collection->GetParameterId(Name);
		auto* Select = NewObject<UMaterialExpressionLinearInterpolate>(Mat);
		Select->A.Expression = Local; Select->B.Expression = Shared; Select->Alpha.Expression = UseSharedPortal;
		if (bVector)
		{
			// Vector parameters expose RGB here; collection vectors expose RGBA.
			auto* RGB = NewObject<UMaterialExpressionComponentMask>(Mat);
			RGB->Input.Expression = Shared; RGB->R = RGB->G = RGB->B = true; RGB->A = false;
			EditorOnly->ExpressionCollection.AddExpression(RGB);
			Select->B.Expression = RGB;
		}
		EditorOnly->ExpressionCollection.AddExpression(Shared);
		EditorOnly->ExpressionCollection.AddExpression(Select);
		return Select;
	};
	auto* PortalEnable = SharedPortal(LookOutEnable, TEXT("LookOutEnable"));
	auto* PortalCamera = SharedPortal(LookOutCam, TEXT("LookOutCam"), true);
	auto* PortalCount = SharedPortal(LookOutViewCount, TEXT("PortalViewCount"));
	UMaterialExpressionCustom* InteriorClip = NewObject<UMaterialExpressionCustom>(Mat);
	auto* PortalEye = NewObject<UMaterialExpressionCameraPositionWS>(Mat);
	EditorOnly->ExpressionCollection.AddExpression(PortalEye);
	InteriorClip->Description = TEXT("LandPortalListAndInteriorClip");
	InteriorClip->OutputType = CMOT_Float1;
	InteriorClip->Code = TEXT(
        "float3 w = W;\n"
        "if (LookOut >= 0.5f)\n"
        "{\n"
        "  float3 p = w - CamP; float3 eye = ActualEye - CamP;\n"
        "  uint record = 0; bool entered = false;\n"
        "  [loop] for (uint view = 0; view < (uint)ViewCount; ++view)\n"
        "  {\n"
        "    float4 header = Views.Load(int3(record % 64, record / 64, 0));\n"
        "    uint count = (uint)header.x;\n"
        "    ++record;\n"
        "    uint firstPlane = record; record += count;\n"
        "    float4 front = Views.Load(int3(firstPlane % 64, firstPlane / 64, 0));\n"
        "    bool inside = count == 0 || dot(front.xyz,p) <= front.w + 0.001f;\n"
        "    [branch] if (!inside) continue;\n"
        "    [loop] for (uint i = 1; i < count; ++i)\n"
        "    {\n"
        "      uint planeRecord = firstPlane + i;\n"
        "      float4 plane = Views.Load(int3(planeRecord % 64, planeRecord / 64, 0));\n"
        "        float d = dot(front.xyz, plane.xyz);\n"
        "        float3 a = (front.xyz - plane.xyz*d) * front.w / max(0.000001f,1.0f-d*d);\n"
        "        float3 edge = cross(front.xyz,plane.xyz);\n"
        "        float3 n = cross(edge,a-eye); if(dot(n,plane.xyz)<0) n=-n;\n"
        "        if (dot(n,p-eye) > 0.001f) { inside = false; break; }\n"
        "    }\n"
        "    if (inside) { if (LookOut < 1.5f || header.y > 0.5f) return 1.0f; entered = true; }\n"
        "  }\n"
        "  return LookOut > 1.5f && !entered ? 1.0f : 0.0f;\n"
        "}\n"
        "if (Enable >= 0.5f)\n"
        "{\n"
        "  if (w.x >= Mn.x && w.x <= Mx.x && w.y >= Mn.y && w.y <= Mx.y) return 0.0f;\n"
        "}\n"
        "return 1.0f;\n");
	InteriorClip->Inputs.Reset();
	{ FCustomInput Eye; Eye.InputName = TEXT("ActualEye"); Eye.Input.Expression = PortalEye; InteriorClip->Inputs.Add(Eye); }
	{
		FCustomInput InW;
		InW.InputName = TEXT("W");
		InW.Input.Expression = LandWorldPos;
		InteriorClip->Inputs.Add(InW);
		FCustomInput InE;
		InE.InputName = TEXT("Enable");
		InE.Input.Expression = InteriorClipEnable;
		InteriorClip->Inputs.Add(InE);
		FCustomInput InMn;
		InMn.InputName = TEXT("Mn");
		InMn.Input.Expression = InteriorClipMin;
		InteriorClip->Inputs.Add(InMn);
		FCustomInput InMx;
		InMx.InputName = TEXT("Mx");
		InMx.Input.Expression = InteriorClipMax;
		InteriorClip->Inputs.Add(InMx);
		FCustomInput InLo;
		InLo.InputName = TEXT("LookOut");
		InLo.Input.Expression = PortalEnable;
		InteriorClip->Inputs.Add(InLo);
		FCustomInput InCam;
		InCam.InputName = TEXT("CamP");
		InCam.Input.Expression = PortalCamera;
		InteriorClip->Inputs.Add(InCam);
        FCustomInput InViews;
        InViews.InputName = TEXT("Views");
        InViews.Input.Expression = LookOutViews;
        InteriorClip->Inputs.Add(InViews);
        FCustomInput InCount;
        InCount.InputName = TEXT("ViewCount");
        InCount.Input.Expression = PortalCount;
        InteriorClip->Inputs.Add(InCount);
	}
	InteriorClip->MaterialExpressionEditorX = -100;
	InteriorClip->MaterialExpressionEditorY = 420;
	UMaterialExpressionMultiply* LandEmisClipped = NewObject<UMaterialExpressionMultiply>(Mat);
	LandEmisClipped->A.Expression = Emis;
	LandEmisClipped->B.Expression = InteriorClip;
	LandEmisClipped->MaterialExpressionEditorX = 80;
	LandEmisClipped->MaterialExpressionEditorY = 0;
	UMaterialExpressionMultiply* LandBaseClipped = NewObject<UMaterialExpressionMultiply>(Mat);
	LandBaseClipped->A.Expression = LandBaseMul;
	LandBaseClipped->B.Expression = InteriorClip;
	LandBaseClipped->MaterialExpressionEditorX = 80;
	LandBaseClipped->MaterialExpressionEditorY = 160;
	EditorOnly->ExpressionCollection.AddExpression(LandWorldPos);
	EditorOnly->ExpressionCollection.AddExpression(InteriorClipEnable);
	EditorOnly->ExpressionCollection.AddExpression(InteriorClipMin);
	EditorOnly->ExpressionCollection.AddExpression(InteriorClipMax);
	EditorOnly->ExpressionCollection.AddExpression(LookOutEnable);
	EditorOnly->ExpressionCollection.AddExpression(LookOutCam);
	EditorOnly->ExpressionCollection.AddExpression(LookOutViews);
	EditorOnly->ExpressionCollection.AddExpression(LookOutViewCount);
	EditorOnly->ExpressionCollection.AddExpression(InteriorClip);
	// Portal clipping belongs to the player's eyes. A shadow camera must still
	// see terrain as an occluder, regardless of the headset's doorway view.
	auto* ShadowMask = NewObject<UMaterialExpressionShadowReplace>(Mat);
	auto* ShadowOpaque = NewObject<UMaterialExpressionConstant>(Mat); ShadowOpaque->R=1.f;
	ShadowMask->Default.Expression=InteriorClip; ShadowMask->Shadow.Expression=ShadowOpaque;
	EditorOnly->ExpressionCollection.AddExpression(ShadowMask);
	EditorOnly->ExpressionCollection.AddExpression(ShadowOpaque);
	EditorOnly->OpacityMask.Expression = ShadowMask;
	EditorOnly->ExpressionCollection.AddExpression(LandEmisClipped);
	EditorOnly->ExpressionCollection.AddExpression(LandBaseClipped);
	EditorOnly->EmissiveColor.Expression = LandEmisClipped;
	EditorOnly->BaseColor.Expression = LandBaseClipped;
	EditorOnly->BaseColor.OutputIndex = 0;
	EditorOnly->Roughness.Expression = Rough;
	EditorOnly->Specular.Expression = Spec;
	EditorOnly->Metallic.Expression = Metal;
	// Slight PixelDepthOffset so StabList EnvCell floors win z vs coplanar LScape
	// (outdoor look-in). Do not raise EnvCell geometry.
	EditorOnly->PixelDepthOffset.Expression = DepthBias;
	WireRetailDistanceFog(Mat, EditorOnly, LandEmisClipped, 0, LandBaseClipped, 0);

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	Mat->CheckMaterialUsage(MATUSAGE_StaticMesh);
	Mat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
#if WITH_EDITOR
	Mat->ForceRecompileForRendering();
#endif
	LandMaterialBase = Mat;
	UE_LOG(LogTemp, Log, TEXT("ACEDat: created Lit land material v42 (cloud opacity + PortalList + indoor footprint clip)"));
	return LandMaterialBase;
#else
	LandMaterialBase = LoadCookedAceMaterial(TEXT("M_ACELandLit_v42"));
	return LandMaterialBase;
#endif
}

UMaterialInterface* UACEDatSubsystem::EnsureAceLandLookOutMaterialBase()
{
	// The active portal-plane mask supports opaque depth rendering. The obsolete
	// CustomStencil graph used SceneTexture in a masked material, which UE cannot compile.
	return EnsureAceLandMaterialBase();
}

void UACEDatSubsystem::SetSkyAnimationTime(double Seconds)
{
	// One shared uniform update, independent of the number of terrain tiles.
	if (auto* World = GetWorld())
		if (auto* Collection = RuntimeFogCollection())
			if (auto* Instance = World->GetParameterCollectionInstance(Collection))
			{
				auto Wrap = [Seconds](double Velocity)
				{
					const double Offset = FMath::Fmod(Seconds * Velocity, 1.);
					return float(Offset < 0. ? Offset + 1. : Offset);
				};
				Instance->SetVectorParameterValue(TEXT("CloudOffset"),FLinearColor(Wrap(LandCloudMotion.G),Wrap(LandCloudMotion.B),0,0));
			}
}

void UACEDatSubsystem::RegisterLandSky(UMaterialInstanceDynamic* Material)
{
	LandSkyInstances.Add(Material);
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"), WorldEmissiveScale);
	if (LandCloudMask) Material->SetTextureParameterValue(TEXT("CloudMask"),LandCloudMask);
	Material->SetVectorParameterValue(TEXT("CloudMotion"),LandCloudMotion);
	Material->SetVectorParameterValue(TEXT("LandSkyTint"),LandSkyTint);
}

void UACEDatSubsystem::SetLandSky(UTexture2D* Mask,const FLinearColor& Motion,const FLinearColor& Tint)
{
	const bool MaskChanged = LandCloudMask != Mask;
	// Cloud opacity drifts with the sky palette. Coalesce sub-byte changes,
	// but always clear/restart shadows immediately and retain the last applied
	// value so accumulated drift and newly registered terrain stay consistent.
	const bool MotionChanged = MaskChanged || LandCloudMotion.R != Motion.R || LandCloudMotion.G != Motion.G || LandCloudMotion.B != Motion.B ||
		(LandCloudMotion.A <= 0.f) != (Motion.A <= 0.f) || FMath::Abs(LandCloudMotion.A - Motion.A) >= 1.f / 255.f;
	const bool TintChanged = !LandSkyTint.Equals(Tint,.008f);
	if (!MaskChanged && !MotionChanged && !TintChanged) return;
	if (MaskChanged) LandCloudMask = Mask;
	if (MotionChanged) LandCloudMotion = Motion;
	if (TintChanged) LandSkyTint = Tint;
	for (auto It=LandSkyInstances.CreateIterator(); It; ++It)
	{
		auto* Mid=It->Get();
		if (!Mid) { It.RemoveCurrent(); continue; }
		if (MaskChanged) Mid->SetTextureParameterValue(TEXT("CloudMask"),Mask);
		if (MotionChanged) Mid->SetVectorParameterValue(TEXT("CloudMotion"),Motion);
		if (TintChanged) Mid->SetVectorParameterValue(TEXT("LandSkyTint"),Tint);
	}
}

UMaterialInterface* UACEDatSubsystem::GetPortalStencilWriterMaterial()
{
	if (PortalStencilWriterMaterial)
	{
		return PortalStencilWriterMaterial;
	}
#if WITH_EDITORONLY_DATA
	UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ACEPortalStencilWriter_v1"), RF_Public | RF_Transient);
	if (!Mat)
	{
		return nullptr;
	}
	Mat->MaterialDomain = MD_Surface;
	Mat->TwoSided = true;
	Mat->SetShadingModel(MSM_Unlit);
	Mat->BlendMode = BLEND_Opaque;
	Mat->SetUsageByFlag(MATUSAGE_StaticMesh, true);
	UMaterialExpressionConstant* Black = NewObject<UMaterialExpressionConstant>(Mat);
	Black->R = 0.f;
	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return nullptr;
	}
	EditorOnly->ExpressionCollection.AddExpression(Black);
	EditorOnly->EmissiveColor.Expression = Black;
	EditorOnly->BaseColor.Expression = Black;
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
#if WITH_EDITOR
	Mat->ForceRecompileForRendering();
#endif
	PortalStencilWriterMaterial = Mat;
	return PortalStencilWriterMaterial;
#else
	PortalStencilWriterMaterial = LoadCookedAceMaterial(TEXT("M_ACEPortalStencilWriter_v1"));
	return PortalStencilWriterMaterial;
#endif
}

UMaterialInterface* UACEDatSubsystem::GetPortalDepthApertureMaterial()
{
	if (PortalDepthApertureMaterial)
	{
		return PortalDepthApertureMaterial;
	}
#if WITH_EDITORONLY_DATA
	UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ACEPortalDepthAperture_v1"), RF_Public | RF_Transient);
	if (!Mat)
	{
		return nullptr;
	}
	Mat->MaterialDomain = MD_Surface;
	Mat->TwoSided = true;
	Mat->SetShadingModel(MSM_Unlit);
	Mat->BlendMode = BLEND_Opaque;
	Mat->SetUsageByFlag(MATUSAGE_StaticMesh, true);
	UMaterialExpressionConstant* Black = NewObject<UMaterialExpressionConstant>(Mat);
	Black->R = 0.f;
	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return nullptr;
	}
	EditorOnly->ExpressionCollection.AddExpression(Black);
	EditorOnly->EmissiveColor.Expression = Black;
	EditorOnly->BaseColor.Expression = Black;
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
#if WITH_EDITOR
	Mat->ForceRecompileForRendering();
#endif
	PortalDepthApertureMaterial = Mat;
	return PortalDepthApertureMaterial;
#else
	PortalDepthApertureMaterial = LoadCookedAceMaterial(TEXT("M_ACEPortalDepthAperture_v1"));
	return PortalDepthApertureMaterial;
#endif
}

UMaterialInterface* UACEDatSubsystem::EnsureAceBuildingShellMaterialBase()
{
	if (BuildingShellMaterialBase)
	{
		return BuildingShellMaterialBase;
	}
#if WITH_EDITORONLY_DATA
	// Opaque walls that clip in world-space doorway slabs (Custom Primitive / MID params).
	// SceneTexture CustomStencil is not bound in the deferred base pass, so it cannot
	// punch holes. Discarded masked pixels skip Z/GBuffer — EnvCells show through.
	if (UMaterial* Existing = FindObject<UMaterial>(GetTransientPackage(), TEXT("M_ACEBuildingShell_v13")))
	{
		BuildingShellMaterialBase = Existing;
		return Existing;
	}
	UMaterial* Mat = NewObject<UMaterial>(GetTransientPackage(), TEXT("M_ACEBuildingShell_v13"), RF_Public | RF_Transient);
	if (!Mat)
	{
		return nullptr;
	}
	Mat->MaterialDomain = MD_Surface;
	Mat->TwoSided = false;
	Mat->SetShadingModel(MSM_DefaultLit);
	Mat->BlendMode = BLEND_Masked;
	Mat->OpacityMaskClipValue = 0.5f;
	Mat->SetUsageByFlag(MATUSAGE_StaticMesh, true);
	Mat->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);

	UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
	Sample->ParameterName = TEXT("ACETexture");
		Sample->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	Sample->SamplerType = SAMPLERTYPE_Color;
	Sample->MaterialExpressionEditorX = -450;
	Sample->MaterialExpressionEditorY = 0;

	UMaterialExpressionScalarParameter* EmissiveStrength = NewObject<UMaterialExpressionScalarParameter>(Mat);
	EmissiveStrength->ParameterName = TEXT("EmissiveStrength");
	EmissiveStrength->DefaultValue = 0.10f;
	EmissiveStrength->MaterialExpressionEditorX = -450;
	EmissiveStrength->MaterialExpressionEditorY = 160;

	UMaterialExpressionMultiply* Emis = NewObject<UMaterialExpressionMultiply>(Mat);
	Emis->A.Expression = Sample;
	Emis->A.OutputIndex = 0;
	auto* AmbientTint = NewObject<UMaterialExpressionVectorParameter>(Mat);
    AmbientTint->ParameterName=TEXT("WorldAmbientTint"); AmbientTint->DefaultValue=FLinearColor::White;
    auto* AmbientFill = NewObject<UMaterialExpressionMultiply>(Mat);
    AmbientFill->A.Expression=EmissiveStrength; AmbientFill->B.Expression=AmbientTint;
    Mat->GetEditorOnlyData()->ExpressionCollection.AddExpression(AmbientTint);
    Mat->GetEditorOnlyData()->ExpressionCollection.AddExpression(AmbientFill);
    Emis->B.Expression=AmbientFill;
	Emis->MaterialExpressionEditorX = -100;
	Emis->MaterialExpressionEditorY = 0;

	UMaterialExpressionOneMinus* ShellLitFrac = NewObject<UMaterialExpressionOneMinus>(Mat);
	ShellLitFrac->Input.Expression = EmissiveStrength;
	ShellLitFrac->MaterialExpressionEditorX = -100;
	ShellLitFrac->MaterialExpressionEditorY = 80;
	UMaterialExpressionMultiply* ShellBaseMul = NewObject<UMaterialExpressionMultiply>(Mat);
	ShellBaseMul->A.Expression = Sample;
	ShellBaseMul->A.OutputIndex = 0;
	ShellBaseMul->B.Expression = ShellLitFrac;
	ShellBaseMul->MaterialExpressionEditorX = 80;
	ShellBaseMul->MaterialExpressionEditorY = 80;

	UMaterialExpressionConstant* Rough = NewObject<UMaterialExpressionConstant>(Mat);
	Rough->R = 1.f;
	Rough->MaterialExpressionEditorX = -100;
	Rough->MaterialExpressionEditorY = 160;
	UMaterialExpressionConstant* Spec = NewObject<UMaterialExpressionConstant>(Mat);
	Spec->R = 0.f;
	Spec->MaterialExpressionEditorX = -100;
	Spec->MaterialExpressionEditorY = 220;
	UMaterialExpressionConstant* Metal = NewObject<UMaterialExpressionConstant>(Mat);
	Metal->R = 0.f;
	Metal->MaterialExpressionEditorX = -100;
	Metal->MaterialExpressionEditorY = 280;

	UMaterialExpressionWorldPosition* WorldPos = NewObject<UMaterialExpressionWorldPosition>(Mat);
	WorldPos->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	WorldPos->MaterialExpressionEditorX = -450;
	WorldPos->MaterialExpressionEditorY = 320;

	auto MakeDoorVec = [&](const TCHAR* Name, const FLinearColor& Default, int32 Y) -> UMaterialExpressionVectorParameter*
	{
		UMaterialExpressionVectorParameter* P = NewObject<UMaterialExpressionVectorParameter>(Mat);
		P->ParameterName = Name;
		P->DefaultValue = Default;
		P->MaterialExpressionEditorX = -450;
		P->MaterialExpressionEditorY = Y;
		return P;
	};
	UMaterialExpressionVectorParameter* Door0Center = MakeDoorVec(TEXT("Door0Center"), FLinearColor::Black, 380);
	UMaterialExpressionVectorParameter* Door0Normal = MakeDoorVec(TEXT("Door0Normal"), FLinearColor(0.f, 0.f, 1.f), 440);
	UMaterialExpressionVectorParameter* Door0Extent = MakeDoorVec(TEXT("Door0Extent"), FLinearColor(0.f, 0.f, 80.f), 500);
	UMaterialExpressionVectorParameter* Door1Center = MakeDoorVec(TEXT("Door1Center"), FLinearColor::Black, 560);
	UMaterialExpressionVectorParameter* Door1Normal = MakeDoorVec(TEXT("Door1Normal"), FLinearColor(0.f, 0.f, 1.f), 620);
	UMaterialExpressionVectorParameter* Door1Extent = MakeDoorVec(TEXT("Door1Extent"), FLinearColor(0.f, 0.f, 80.f), 680);

	UMaterialExpressionCustom* DoorClip = NewObject<UMaterialExpressionCustom>(Mat);
	DoorClip->Description = TEXT("BuildingDoorwaySlabClip");
	DoorClip->OutputType = CMOT_Float1;
	DoorClip->Code = TEXT(
		"float m0 = 1.0f;\n"
		"if (E0.x >= 1.0f)\n"
		"{\n"
		"  float3 Nn = normalize(N0);\n"
		"  float3 T = abs(Nn.z) < 0.9f ? float3(0,0,1) : float3(1,0,0);\n"
		"  float3 R = normalize(cross(T, Nn));\n"
		"  float3 Uax = cross(Nn, R);\n"
		"  float3 L = W - C0;\n"
		"  float x = abs(dot(L, R));\n"
		"  float y = abs(dot(L, Uax));\n"
		"  float z = abs(dot(L, Nn));\n"
		"  m0 = (x < E0.x && y < E0.y && z < E0.z) ? 0.0f : 1.0f;\n"
		"}\n"
		"float m1 = 1.0f;\n"
		"if (E1.x >= 1.0f)\n"
		"{\n"
		"  float3 Nn = normalize(N1);\n"
		"  float3 T = abs(Nn.z) < 0.9f ? float3(0,0,1) : float3(1,0,0);\n"
		"  float3 R = normalize(cross(T, Nn));\n"
		"  float3 Uax = cross(Nn, R);\n"
		"  float3 L = W - C1;\n"
		"  float x = abs(dot(L, R));\n"
		"  float y = abs(dot(L, Uax));\n"
		"  float z = abs(dot(L, Nn));\n"
		"  m1 = (x < E1.x && y < E1.y && z < E1.z) ? 0.0f : 1.0f;\n"
		"}\n"
		"return min(m0, m1);\n");
	DoorClip->Inputs.Reset();
	auto AddIn = [&](const TCHAR* Name, UMaterialExpression* Expr)
	{
		FCustomInput In;
		In.InputName = Name;
		In.Input.Expression = Expr;
		DoorClip->Inputs.Add(In);
	};
	AddIn(TEXT("W"), WorldPos);
	AddIn(TEXT("C0"), Door0Center);
	AddIn(TEXT("N0"), Door0Normal);
	AddIn(TEXT("E0"), Door0Extent);
	AddIn(TEXT("C1"), Door1Center);
	AddIn(TEXT("N1"), Door1Normal);
	AddIn(TEXT("E1"), Door1Extent);
	DoorClip->MaterialExpressionEditorX = -100;
	DoorClip->MaterialExpressionEditorY = 320;

	UMaterialEditorOnlyData* EditorOnly = Mat->GetEditorOnlyData();
	if (!EditorOnly)
	{
		return nullptr;
	}
	EditorOnly->ExpressionCollection.AddExpression(Sample);
	EditorOnly->ExpressionCollection.AddExpression(EmissiveStrength);
	EditorOnly->ExpressionCollection.AddExpression(Emis);
	EditorOnly->ExpressionCollection.AddExpression(ShellLitFrac);
	EditorOnly->ExpressionCollection.AddExpression(ShellBaseMul);
	EditorOnly->ExpressionCollection.AddExpression(Rough);
	EditorOnly->ExpressionCollection.AddExpression(Spec);
	EditorOnly->ExpressionCollection.AddExpression(Metal);
	EditorOnly->ExpressionCollection.AddExpression(WorldPos);
	EditorOnly->ExpressionCollection.AddExpression(Door0Center);
	EditorOnly->ExpressionCollection.AddExpression(Door0Normal);
	EditorOnly->ExpressionCollection.AddExpression(Door0Extent);
	EditorOnly->ExpressionCollection.AddExpression(Door1Center);
	EditorOnly->ExpressionCollection.AddExpression(Door1Normal);
	EditorOnly->ExpressionCollection.AddExpression(Door1Extent);
	EditorOnly->ExpressionCollection.AddExpression(DoorClip);
	EditorOnly->EmissiveColor.Expression = Emis;
	EditorOnly->BaseColor.Expression = ShellBaseMul;
	EditorOnly->BaseColor.OutputIndex = 0;
	EditorOnly->Roughness.Expression = Rough;
	EditorOnly->Specular.Expression = Spec;
	EditorOnly->Metallic.Expression = Metal;
	EditorOnly->OpacityMask.Expression = DoorClip;
	WireRetailDistanceFog(Mat, EditorOnly, Emis, 0, ShellBaseMul, 0);

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	Mat->CheckMaterialUsage(MATUSAGE_StaticMesh);
	Mat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
#if WITH_EDITOR
	Mat->ForceRecompileForRendering();
#endif
	BuildingShellMaterialBase = Mat;
	UE_LOG(LogTemp, Log, TEXT("ACEDat: created building-shell doorway material v13 (lit + CSM-friendly)"));
	return BuildingShellMaterialBase;
#else
	BuildingShellMaterialBase = LoadCookedAceMaterial(TEXT("M_ACEBuildingShell_v13"));
	return BuildingShellMaterialBase;
#endif
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateBuildingShellMaterial(uint32 SurfaceId, bool bWrapTexture)
{
	const uint32 CacheKey = FACEDatTextureResolver::WorldTextureKey(SurfaceId, bWrapTexture);
	static constexpr int32 BuildingShellMatVersion = 13;
	static int32 AppliedBuildingShellMatVersion = 0;
	if (AppliedBuildingShellMatVersion != BuildingShellMatVersion)
	{
		AppliedBuildingShellMatVersion = BuildingShellMatVersion;
		ResetSurfaceMaterialCache(BuildingShellMaterialCache,1);
		BuildingShellMaterialBase = nullptr;
	}

	if (SurfaceId == 0 || !TextureResolver)
	{
		return nullptr;
	}

	FACEDatDecodedSurface Decoded;
	const bool bResolved = TextureResolver->ResolveSurface(SurfaceId, Decoded);
	if (bResolved && (Decoded.bFullyTransparent || Decoded.bAdditive))
	{
		// Windows / portal fills stay on the non-clip path (already holes or translucent).
		return GetOrCreateStaticMeshMaterial(SurfaceId, bWrapTexture);
	}
	if (bResolved && (Decoded.bClipMap || Decoded.bUsesAlpha))
	{
		return GetOrCreateOutdoorLitMaterial(SurfaceId, bWrapTexture);
	}

	if (auto* Cached=FindSurfaceMaterial(BuildingShellMaterialCache,1,CacheKey)) return Cached;

	UTexture2D* Tex = TextureResolver->GetOrCreateUTexture(SurfaceId, this, bWrapTexture);
	if (!Tex && bResolved && Decoded.bIsSolid && !Decoded.bFullyTransparent)
	{
		Tex = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
		if (Tex && Tex->GetPlatformData() && Tex->GetPlatformData()->Mips.Num() > 0)
		{
			Tex->CompressionSettings = TC_EditorIcon;
			Tex->SRGB = true;
			Tex->NeverStream = true;
			FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
			void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
			const FColor C = Decoded.SolidColor.ToFColor(true);
			FMemory::Memcpy(Data, &C, sizeof(FColor));
			Mip.BulkData.Unlock();
			Tex->UpdateResource();
			Tex->Rename(nullptr, this);
		}
		else
		{
			Tex = nullptr;
		}
	}
	if (!Tex)
	{
		return GetOrCreateStaticMeshMaterial(SurfaceId, bWrapTexture);
	}

	UMaterialInterface* Base = EnsureAceBuildingShellMaterialBase();
	if (!Base)
	{
		return GetOrCreateStaticMeshMaterial(SurfaceId, bWrapTexture);
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
	if (!MID)
	{
		return GetOrCreateStaticMeshMaterial(SurfaceId, bWrapTexture);
	}
	MID->SetTextureParameterValue(TEXT("ACETexture"), Tex);
	MID->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	MID->SetTextureParameterValue(TEXT("Texture"), Tex);
	MID->SetScalarParameterValue(TEXT("EmissiveStrength"), SceneryEmissiveScale);
	MID->SetVectorParameterValue(TEXT("WorldAmbientTint"),WorldAmbientTint);
	ApplyDistanceFogToMid(MID);
	BuildingShellMaterialCache.Add(CacheKey, MID);
	SurfaceMaterials.Add((uint64(1)<<32)|CacheKey, {MID,++SurfaceMaterialClock});
	return MID;
}

void UACEDatSubsystem::SetPortalLookOutLand(bool bEnable)
{
	if (bPortalLookOutLand == bEnable)
	{
		return;
	}
	bPortalLookOutLand = bEnable;
	UE_LOG(LogTemp, Verbose, TEXT("ACEDat: portal look-out land %s"),
		bEnable ? TEXT("ON (CustomStencil)") : TEXT("OFF (opaque outdoor)"));
}

bool UACEDatSubsystem::IsBuildingInteriorEnvCell(uint32 EnvCellId, float WorldScale)
{
	if ((EnvCellId & 0xFFFFu) < 0x0100u)
	{
		return false;
	}
	// WARNING: GetOrBuildBuildingInteriorFootprints may GetOrBuildEnvCellMesh and rehash
	// EnvCellMeshCache — callers must not hold FACEBuiltEnvCellMesh* across this call.
	const FBuildingInteriorMask& Mask =
		GetOrBuildBuildingInteriorFootprints(EnvCellId & 0xFFFF0000u, WorldScale);
	for (const FBuildingHoleMask& Bld : Mask.Buildings)
	{
		if (Bld.IndoorCellIds.Contains(EnvCellId))
		{
			return true;
		}
	}
	return false;
}

UMaterialInterface* UACEDatSubsystem::RemapLandMaterialForPortalLookOut(UMaterialInterface* Current, bool /*bLookOut*/)
{
	// All land now uses one depth-writing portal-mask shader. Its cached MID is
	// updated by SetLandLookOutClip. Cloning here disconnected live sections from
	// that cache and reintroduced the old AABB clip when crossing a doorway.
	return Current;
}
UMaterialInterface* UACEDatSubsystem::RemapLandMaterialWithDepthBias(UMaterialInterface* Current, float DepthBiasCm)
{
	if (!Current)
	{
		return nullptr;
	}
	UTexture* Tex = nullptr;
	if (UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Current))
	{
		Mid->GetTextureParameterValue(TEXT("ACETexture"), Tex);
		if (!Tex)
		{
			Mid->GetTextureParameterValue(TEXT("Texture"), Tex);
		}
		if (!Tex)
		{
			Mid->GetTextureParameterValue(TEXT("SlateUI"), Tex);
		}
	}
	UMaterialInterface* Base = EnsureAceLandMaterialBase();
	if (!Base)
	{
		return Current;
	}
	UMaterialInstanceDynamic* Out = UMaterialInstanceDynamic::Create(Base, this);
	if (!Out)
	{
		return Current;
	}
	if (Tex)
	{
		Out->SetTextureParameterValue(TEXT("ACETexture"), Tex);
		Out->SetTextureParameterValue(TEXT("Texture"), Tex);
		Out->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	}
	Out->SetScalarParameterValue(TEXT("LandDepthBias"), DepthBiasCm);
	ApplyDistanceFogToMid(Out);
	ApplyOutdoorPortalLandClipToMid(Out);
	ApplyLandInteriorClipToMid(Out);
	RegisterLandSky(Out);
	return Out;
}

void UACEDatSubsystem::ApplyOutdoorPortalLandClipToMid(UMaterialInstanceDynamic* Mid) const
{
    if (!Mid) return;
    const bool Shared = GetWorld() != nullptr;
    Mid->SetScalarParameterValue(TEXT("UseSharedPortal"), Shared ? 1.f : 0.f);
    if (!Shared)
    {
        Mid->SetScalarParameterValue(TEXT("LookOutEnable"), bLandLookOutClip ? (bLandLookInClip ? 2.f : 1.f) : 0.f);
        Mid->SetVectorParameterValue(TEXT("LookOutCam"), FLinearColor(LandLookOutCam));
        Mid->SetScalarParameterValue(TEXT("PortalViewCount"), LandPortalViewCount);
    }
    if (LandPortalViewTexture) Mid->SetTextureParameterValue(TEXT("PortalViews"), LandPortalViewTexture);
}

void UACEDatSubsystem::SetLandLookOutClip(bool bEnable, const FVector& CameraWorld,
    const TArray<ACEOutdoorPortalPlan::FAdmittedAperture>& Apertures,
    const TArray<ACEOutdoorPortalPlan::FAdmittedAperture>* LookInExits)
{
    FACEPortalViewMask Mask;
    if (bEnable) Mask = FACEPortalViewMask::Build(CameraWorld, Apertures);
    // Outside any visible doorway the camera is unused by the terrain shader.
    // HMD micro-movements must not dirty every terrain material's uniform buffer.
    const FVector ClipCamera = bEnable || CVarSkipUnusedPortalCamera.GetValueOnGameThread()==0 ? CameraWorld : FVector::ZeroVector;
    const bool bParametersChanged = bLandLookOutClip != bEnable
        || bLandLookInClip != (bEnable && LookInExits)
        || !LandLookOutCam.Equals(ClipCamera, 0.f);
    bLandLookInClip = bEnable && LookInExits;
    if (bLandLookInClip)
    {
        FACEPortalViewMask Exits = FACEPortalViewMask::Build(CameraWorld,*LookInExits);
        for (int32 Record = 0; Record < Exits.Records.Num();)
        {
            Exits.Records[Record].Y = 1; // Outdoor rays beyond a second door restore land.
            Record += 1 + static_cast<int32>(Exits.Records[Record].X);
        }
        Mask.Records.Append(Exits.Records);
        Mask.ViewCount += Exits.ViewCount;
    }
    const bool bChanged = Mask.Records.Num() != LandPortalViewRecords.Num()
        || (Mask.Records.Num() > 0 && FMemory::Memcmp(Mask.Records.GetData(), LandPortalViewRecords.GetData(),
            Mask.Records.Num() * sizeof(FVector4f)) != 0);
    bLandLookOutClip = bEnable;
    LandLookOutCam = ClipCamera;
    LandPortalViewCount = Mask.ViewCount;
    bool bRebindTexture = false;
    if (bChanged)
    {
        LandPortalViewRecords = MoveTemp(Mask.Records);
        constexpr int32 Width = FACEPortalViewMask::TextureWidth;
        const int32 Height = FMath::RoundUpToPowerOfTwo(FMath::Max(1, FMath::DivideAndRoundUp(LandPortalViewRecords.Num(), Width)));
        const bool bNewTexture = !LandPortalViewTexture || LandPortalViewTexture->GetSizeY() < Height;
        bRebindTexture = bNewTexture;
        if (bNewTexture)
        {
            LandPortalViewTexture = UTexture2D::CreateTransient(Width, Height, PF_A32B32G32R32F);
            if (!LandPortalViewTexture) return;
            LandPortalViewTexture->SRGB = false;
            LandPortalViewTexture->NeverStream = true;
            LandPortalViewTexture->Filter = TF_Nearest;
        }
        auto& Mip = LandPortalViewTexture->GetPlatformData()->Mips[0];
        void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memzero(Data, Mip.BulkData.GetBulkDataSize());
        FMemory::Memcpy(Data, LandPortalViewRecords.GetData(), LandPortalViewRecords.Num() * sizeof(FVector4f));
        Mip.BulkData.Unlock();
        if (bNewTexture) LandPortalViewTexture->UpdateResource();
        else if (LandPortalViewTexture->GetResource())
        {
            // Retain the texture resource while moving; update only the active rows.
            const int32 Bytes = Width * Height * sizeof(FVector4f);
            uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Bytes));
            FMemory::Memzero(Copy, Bytes);
            FMemory::Memcpy(Copy, LandPortalViewRecords.GetData(), LandPortalViewRecords.Num() * sizeof(FVector4f));
            auto* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
            LandPortalViewTexture->UpdateTextureRegions(0, 1, Region, Width * sizeof(FVector4f), sizeof(FVector4f), Copy,
                [](uint8* Pixels, const FUpdateTextureRegion2D* Regions) { FMemory::Free(Pixels); delete Regions; });
        }
    }
    if (auto* World = GetWorld(); World && (bChanged || bParametersChanged))
    {
        if (auto* Collection = RuntimePortalCollection())
        {
            auto* Instance = World->GetParameterCollectionInstance(Collection);
            Instance->SetScalarParameterValue(TEXT("LookOutEnable"), bLandLookOutClip ? (bLandLookInClip ? 2.f : 1.f) : 0.f);
            Instance->SetVectorParameterValue(TEXT("LookOutCam"), FLinearColor(LandLookOutCam));
            Instance->SetScalarParameterValue(TEXT("PortalViewCount"), LandPortalViewCount);
        }
    }
    // Texture content changes keep the same resource. Rebind cached materials
    // only if the packed portal list needs a larger texture allocation.
    if (bRebindTexture || (!GetWorld() && (bChanged || bParametersChanged)))
    {
        for (auto& Pair : LandMaterialCache) ApplyOutdoorPortalLandClipToMid(Pair.Value.Get());
        ApplyOutdoorPortalLandClipToMid(LandGpuMaterialInstance);
    }
}

void UACEDatSubsystem::SetOutdoorPortalLandClip(bool bEnable, const TArray<FVector4f>& WorldPlanes)
{
    if (!bEnable) SetLandLookOutClip(false, FVector::ZeroVector, {});
}

void UACEDatSubsystem::ApplyLandInteriorClipToMid(UMaterialInstanceDynamic* Mid) const
{
	if (!Mid)
	{
		return;
	}
	Mid->SetScalarParameterValue(TEXT("InteriorClipEnable"), bLandInteriorClip ? 1.f : 0.f);
	Mid->SetVectorParameterValue(TEXT("InteriorClipMin"), FLinearColor(LandInteriorClipMin));
	Mid->SetVectorParameterValue(TEXT("InteriorClipMax"), FLinearColor(LandInteriorClipMax));
}

void UACEDatSubsystem::SetLandInteriorClip(bool bEnable, const FVector& WorldMin, const FVector& WorldMax)
{
	const bool bOn = bEnable && WorldMin.X < WorldMax.X && WorldMin.Y < WorldMax.Y;
	if (bLandInteriorClip == bOn
		&& (!bOn || (LandInteriorClipMin.Equals(WorldMin, 2.f) && LandInteriorClipMax.Equals(WorldMax, 2.f))))
	{
		return;
	}
	bLandInteriorClip = bOn;
	LandInteriorClipMin = WorldMin;
	LandInteriorClipMax = WorldMax;
	for (auto& Pair : LandMaterialCache)
	{
		ApplyLandInteriorClipToMid(Pair.Value.Get());
	}
	ApplyLandInteriorClipToMid(LandGpuMaterialInstance);
}

UMaterialInterface* UACEDatSubsystem::EnsureAceUnlitMaskedMaterialBase()
{
	static constexpr int32 MaskedLitGraphVersion = 8;
	static int32 AppliedMaskedLitGraphVersion = 0;
	if (AppliedMaskedLitGraphVersion != MaskedLitGraphVersion)
	{
		AppliedMaskedLitGraphVersion = MaskedLitGraphVersion;
		MaskedMaterialBase = nullptr;
	}
	if (MaskedMaterialBase)
	{
		return MaskedMaterialBase;
	}

	MaskedMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEUnlitMasked_v8"), EACEAceMaterialKind::Masked,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, /*bWorldLit*/ false);
	if (MaskedMaterialBase)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: created runtime Unlit masked ACETexture material (ClipMap)"));
		return MaskedMaterialBase;
	}

	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceOutdoorLitMaterialBase()
{
	if (OutdoorLitMaterialBase)
	{
		return OutdoorLitMaterialBase;
	}
	OutdoorLitMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEOutdoorLit_v3"), EACEAceMaterialKind::Opaque,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, /*bWorldLit*/ true);
	if (OutdoorLitMaterialBase)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: created runtime DefaultLit outdoor scenery material"));
		return OutdoorLitMaterialBase;
	}
	return EnsureAceUnlitTexturedMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::EnsureAceOutdoorLitMaskedMaterialBase()
{
	if (OutdoorLitMaskedMaterialBase)
	{
		return OutdoorLitMaskedMaterialBase;
	}
	OutdoorLitMaskedMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEOutdoorLitMasked_v3"), EACEAceMaterialKind::Masked,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, /*bWorldLit*/ true);
	if (OutdoorLitMaskedMaterialBase)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: created runtime DefaultLit outdoor ClipMap material"));
		return OutdoorLitMaskedMaterialBase;
	}
	return EnsureAceUnlitMaskedMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::EnsureAceUnlitTranslucentMaterialBase()
{
	static constexpr int32 TransMatVersion = 11;
	static int32 AppliedTransMatVersion = 0;
	if (AppliedTransMatVersion != TransMatVersion)
	{
		AppliedTransMatVersion = TransMatVersion;
		TranslucentMaterialBase = nullptr;
		ResetSurfaceMaterialCache(ParticleMaterialCache,4);
	}
	if (TranslucentMaterialBase)
	{
		return TranslucentMaterialBase;
	}

	// The blend stage applies OpacityMul; multiplying emissive as well squares it.
	TranslucentMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEUnlitTranslucent_v11"), EACEAceMaterialKind::Translucent,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, /*bWorldLit*/ false,
        /*bOpacityTexAlphaOnly*/ false, /*bEmissiveTimesTexAlpha*/ false,
        /*bStaticVertexLighting*/ false, /*bExaminationLighting*/ false, /*bSurfaceShadow*/ true);
	if (TranslucentMaterialBase)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: created runtime Unlit translucent ACETexture material (overlays)"));
		return TranslucentMaterialBase;
	}

	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceUnlitAdditiveMaterialBase()
{
	static constexpr int32 AddMatVersion = 10;
	static int32 AppliedAddMatVersion = 0;
	if (AppliedAddMatVersion != AddMatVersion)
	{
		AppliedAddMatVersion = AddMatVersion;
		AdditiveMaterialBase = nullptr;
		ResetSurfaceMaterialCache(ParticleMaterialCache,4);
	}
	if (AdditiveMaterialBase)
	{
		return AdditiveMaterialBase;
	}

	AdditiveMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEUnlitAdditive_v10"), EACEAceMaterialKind::Additive,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false);
	if (AdditiveMaterialBase)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: created runtime Unlit additive ACETexture material (overlays)"));
		return AdditiveMaterialBase;
	}

	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceBatchedParticleMaterialBase(bool bAdditive)
{
	return CreateAceOverlayMaterial(bAdditive ? TEXT("M_ACEBatchedParticleAdditive_v3") : TEXT("M_ACEBatchedParticleTranslucent_v1"),
		bAdditive ? EACEAceMaterialKind::Additive : EACEAceMaterialKind::Translucent,
		false, false, false, false, false, false, bAdditive, false, false, false, false, false, bAdditive, true);
}

UMaterialInterface* UACEDatSubsystem::EnsureAceParticleTranslucentMaterialBase()
{
	if (ParticleTranslucentMaterialBase) return ParticleTranslucentMaterialBase;
	// Retail alpha blending attenuates the source once. Multiplying emissive by
	// OpacityMul as well squares the particle's fade and makes wand tears vanish.
	// Back-facing polygons are already supplied by the DAT mesh builder.
	ParticleTranslucentMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEUnlitParticleTranslucent_v2"), EACEAceMaterialKind::Translucent,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false);
	return ParticleTranslucentMaterialBase;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceParticleAdditiveMaterialBase()
{
	static constexpr int32 ParticleAddMatVersion = 9;
	static int32 AppliedParticleAddMatVersion = 0;
	if (AppliedParticleAddMatVersion != ParticleAddMatVersion)
	{
		AppliedParticleAddMatVersion = ParticleAddMatVersion;
		ParticleAdditiveMaterialBase = nullptr;
		ResetSurfaceMaterialCache(ParticleMaterialCache,4);
	}
	if (ParticleAdditiveMaterialBase)
	{
		return ParticleAdditiveMaterialBase;
	}

	// Apply authored translucency/texture alpha once in the framebuffer's color space.
	// The mesh builder emits the authored back face. A two-sided material would
	// render both copies from either view and double the additive contribution.
	ParticleAdditiveMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEUnlitParticleAdditive_v9"), EACEAceMaterialKind::Additive,
		/*bDisableFog*/ false, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, /*bWorldLit*/ false,
		/*bOpacityTexAlphaOnly*/ true, /*bEmissiveTimesTexAlpha*/ false,
        false, false, false, false, /*bRetailParticle*/ true);
	if (ParticleAdditiveMaterialBase)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: created runtime Unlit particle additive ACETexture material"));
		return ParticleAdditiveMaterialBase;
	}

	return EnsureAceUnlitAdditiveMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::EnsureAceSkyTranslucentMaterialBase()
{
	static constexpr int32 SkyTransMatVersion = 19;
	static int32 AppliedSkyTransMatVersion = 0;
	if (AppliedSkyTransMatVersion != SkyTransMatVersion)
	{
		AppliedSkyTransMatVersion = SkyTransMatVersion;
		SkyTranslucentMaterialBase = nullptr;
	}
	if (SkyTranslucentMaterialBase)
	{
		return SkyTranslucentMaterialBase;
	}

	// One-sided: two-sided let the far cube wall composite as a sliding color panel.
	SkyTranslucentMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACESkyTranslucent_v20"), EACEAceMaterialKind::Translucent,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (SkyTranslucentMaterialBase)
	{
		return SkyTranslucentMaterialBase;
	}

	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceSkyTranslucentWrapMaterialBase()
{
	static constexpr int32 SkyWrapMatVersion = 10;
	static int32 AppliedSkyWrapMatVersion = 0;
	if (AppliedSkyWrapMatVersion != SkyWrapMatVersion)
	{
		AppliedSkyWrapMatVersion = SkyWrapMatVersion;
		SkyTranslucentWrapMaterialBase = nullptr;
	}
	if (SkyTranslucentWrapMaterialBase)
	{
		return SkyTranslucentWrapMaterialBase;
	}

	SkyTranslucentWrapMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACESkyTranslucentWrap_v11"), EACEAceMaterialKind::Translucent,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ true,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (SkyTranslucentWrapMaterialBase)
	{
		return SkyTranslucentWrapMaterialBase;
	}

	return EnsureAceSkyTranslucentMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::EnsureAceSkyOpaqueMaterialBase()
{
	static constexpr int32 SkyOpqMatVersion = 8;
	static int32 AppliedSkyOpqMatVersion = 0;
	if (AppliedSkyOpqMatVersion != SkyOpqMatVersion)
	{
		AppliedSkyOpqMatVersion = SkyOpqMatVersion;
		SkyOpaqueMaterialBase = nullptr;
	}
	if (SkyOpaqueMaterialBase)
	{
		return SkyOpaqueMaterialBase;
	}

	// Solid faces still belong to the before-world sky pass.
	SkyOpaqueMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACESkyOpaque_v9"), EACEAceMaterialKind::Opaque,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (SkyOpaqueMaterialBase)
	{
		return SkyOpaqueMaterialBase;
	}

	UE_LOG(LogTemp, Warning, TEXT("ACESky: opaque day-dome material missing — translucent fallback"));
	return EnsureAceSkyTranslucentMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::EnsureAceSkyAdditiveMaterialBase()
{
	static constexpr int32 SkyAddMatVersion = 20;
	static int32 AppliedSkyAddMatVersion = 0;
	if (AppliedSkyAddMatVersion != SkyAddMatVersion)
	{
		AppliedSkyAddMatVersion = SkyAddMatVersion;
		SkyAdditiveMaterialBase = nullptr;
	}
	if (SkyAdditiveMaterialBase)
	{
		return SkyAdditiveMaterialBase;
	}

	SkyAdditiveMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACESkyAdditive_v21"), EACEAceMaterialKind::Additive,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ true,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (SkyAdditiveMaterialBase)
	{
		return SkyAdditiveMaterialBase;
	}

	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceSkyVertexColorMaterialBase()
{
	static constexpr int32 SkyVcMatVersion = 11;
	static int32 AppliedSkyVcMatVersion = 0;
	if (AppliedSkyVcMatVersion != SkyVcMatVersion)
	{
		AppliedSkyVcMatVersion = SkyVcMatVersion;
		SkyVertexColorMaterialBase = nullptr;
	}
	if (SkyVertexColorMaterialBase)
	{
		return SkyVertexColorMaterialBase;
	}

	// Preserve solid vertex RGB while compositing behind all world geometry.
	SkyVertexColorMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACESkyVertexColor_v12"), EACEAceMaterialKind::VertexColorOpaque,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (SkyVertexColorMaterialBase)
	{
		return SkyVertexColorMaterialBase;
	}

	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::EnsureAceSkyColorFillMaterialBase()
{
	static constexpr int32 SkyFillMatVersion = 8;
	static int32 AppliedSkyFillMatVersion = 0;
	if (AppliedSkyFillMatVersion != SkyFillMatVersion)
	{
		AppliedSkyFillMatVersion = SkyFillMatVersion;
		SkyColorFillMaterialBase = nullptr;
	}
	if (SkyColorFillMaterialBase)
	{
		return SkyColorFillMaterialBase;
	}

	// Two-sided so winding cannot hide the fill. Translucent so clouds still composite.
	// FillColor vector param (not PMC vertex A) — see CreateAceOverlayMaterial.
	SkyColorFillMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACESkyColorFill_v8"), EACEAceMaterialKind::VertexColorTranslucent,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ false, /*bForceUvWrap*/ false,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ true, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (SkyColorFillMaterialBase)
	{
		return SkyColorFillMaterialBase;
	}

	return EnsureAceSkyVertexColorMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::EnsureAceWeatherTranslucentMaterialBase()
{
	static constexpr int32 WxTransMatVersion = 14;
	static int32 AppliedWxTransMatVersion = 0;
	if (AppliedWxTransMatVersion != WxTransMatVersion)
	{
		AppliedWxTransMatVersion = WxTransMatVersion;
		WeatherTranslucentMaterialBase = nullptr;
	}
	if (WeatherTranslucentMaterialBase)
	{
		return WeatherTranslucentMaterialBase;
	}

	// One-sided: camera sits inside rain cylinders — two-sided faces z-fight as a
	// flickering white sheet. Use the same DAT lighting and opacity as the before-pass.
	// GameSky::Draw(after) composites the outdoor rain after DrawBlock, without depth testing.
	WeatherTranslucentMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEWeatherTranslucent_v14"), EACEAceMaterialKind::Translucent,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ true, /*bForceUvWrap*/ true,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (WeatherTranslucentMaterialBase)
	{
		return WeatherTranslucentMaterialBase;
	}

	return EnsureAceSkyTranslucentMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::EnsureAceWeatherAdditiveMaterialBase()
{
	static constexpr int32 WxAddMatVersion = 14;
	static int32 AppliedWxAddMatVersion = 0;
	if (AppliedWxAddMatVersion != WxAddMatVersion)
	{
		AppliedWxAddMatVersion = WxAddMatVersion;
		WeatherAdditiveMaterialBase = nullptr;
	}
	if (WeatherAdditiveMaterialBase)
	{
		return WeatherAdditiveMaterialBase;
	}

	WeatherAdditiveMaterialBase = CreateAceOverlayMaterial(
		TEXT("M_ACEWeatherAdditive_v14"), EACEAceMaterialKind::Additive,
		/*bDisableFog*/ true, /*bDisableDepthTest*/ true, /*bForceUvWrap*/ true,
		/*bEmissiveTimesOpacity*/ false, /*bTwoSided*/ false, false, false, false, false, false, false, /*bRetailSky*/ true);
	if (WeatherAdditiveMaterialBase)
	{
		return WeatherAdditiveMaterialBase;
	}

	return EnsureAceSkyAdditiveMaterialBase();
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateTexturedMaterial(uint32 SurfaceId, bool bWrapTexture)
{
	const uint32 CacheKey = FACEDatTextureResolver::WorldTextureKey(SurfaceId, bWrapTexture);
	if (SurfaceId == 0 || !TextureResolver)
	{
		return nullptr;
	}

	if (auto* Cached=FindSurfaceMaterial(TexturedMaterialCache,0,CacheKey)) return Cached;

	FACEDatDecodedSurface Decoded;
	const bool bResolved = TextureResolver->ResolveSurface(SurfaceId, Decoded);
	if (bResolved && IsDatPortalFillSurface(Decoded))
	{
		// Fully transparent ClipMap/solid portal anchors must not create MIDs that fall back
		// to unlit white when sampled.
		return nullptr;
	}

	UTexture2D* Tex = TextureResolver->GetOrCreateUTexture(SurfaceId, this, bWrapTexture);
	if (!Tex && bResolved && Decoded.bIsSolid && !IsDatPortalFillSurface(Decoded))
	{
		// Solid ColorValue furniture/props (and soft alpha solids) — synthesize a 1×1 swatch so
		// GetOrCreateSetupStaticMesh does not drop the section (gray/missing Stab scenery).
		Tex = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
		if (Tex && Tex->GetPlatformData() && Tex->GetPlatformData()->Mips.Num() > 0)
		{
			Tex->CompressionSettings = TC_EditorIcon;
			Tex->SRGB = true;
			Tex->NeverStream = true;
			FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
			void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
			const FColor C = Decoded.SolidColor.ToFColor(true);
			FMemory::Memcpy(Data, &C, sizeof(FColor));
			Mip.BulkData.Unlock();
			Tex->UpdateResource();
			Tex->Rename(nullptr, this);
		}
		else
		{
			Tex = nullptr;
		}
	}
	if (!Tex)
	{
		if (!bResolved || (!IsDatPortalFillSurface(Decoded) && !Decoded.bAdditive))
		{
			return GetVertexColorMaterial();
		}
		return nullptr;
	}

	UMaterialInterface* Base = EnsureAceUnlitTexturedMaterialBase();
	if (bResolved)
	{
		// Additive+ClipMap portal FX (Town Network 0x080004B9, luminous ground discs) must
		// stay Additive — ClipMap masked hard-edges and paints muddy solid slabs.
		if (Decoded.bAdditive)
		{
			Base = EnsureAceUnlitAdditiveMaterialBase();
		}
		else if (Decoded.bClipMap)
		{
			Base = EnsureAceUnlitMaskedMaterialBase();
		}
		else if (Decoded.bUsesAlpha)
		{
			// Many EnvCell floors set UsesAlpha but are visually opaque textured surfaces;
			// Translucent MIDs lose depth to outdoor LScape → grass through doorways.
			// Soft ColorValue glass + DAT SurfaceType.Translucent (Virindi body) stay soft.
			const bool bSoftGlass = !Decoded.bHasPixels && Decoded.SolidColor.A < 0.98f;
			// Mild DAT Translucency on Image walls (Yaraq stucco) must stay opaque so the
			// PMC can write shadow depth. Only real glass / high translucency goes soft.
			const bool bDatBodyTranslucent = bSoftGlass
				|| Decoded.Translucency > 0.35f
				|| (Decoded.bSurfaceTranslucent && Decoded.Translucency > 0.20f);
			if (bDatBodyTranslucent || (Decoded.bFullyTransparent && !Decoded.bClipMap && !Decoded.bAdditive))
			{
				Base = EnsureAceUnlitTranslucentMaterialBase();
			}
			// else keep opaque textured base so floors write depth
		}
		else if (Decoded.bFullyTransparent)
		{
			Base = EnsureAceUnlitTranslucentMaterialBase();
		}
	}
	if (!Base && Decoded.bClipMap)
	{
		return nullptr;
	}
	if (!Base)
	{
		Base = EnsureAceUnlitTexturedMaterialBase();
	}
	if (!Base)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
	if (!MID)
	{
		return nullptr;
	}
	MID->SetTextureParameterValue(TEXT("ACETexture"), Tex);
	MID->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	MID->SetTextureParameterValue(TEXT("Texture"), Tex);
	MID->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	MID->SetScalarParameterValue(TEXT("DiffuseStrength"), 1.f);
	MID->SetScalarParameterValue(TEXT("AuthoredLuminosity"), FMath::Clamp(Decoded.Luminosity,0.f,1.f));
	MID->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
	MID->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
	ApplyDistanceFogToMid(MID);
	TexturedMaterialCache.Add(CacheKey, MID);
	SurfaceMaterials.Add((uint64(0)<<32)|CacheKey, {MID,++SurfaceMaterialClock});
	return MID;
}

void UACEDatSubsystem::SetInteriorAmbient(const FLinearColor& Color)
{
	OutdoorInteriorAmbient = Color;
	// Interior illumination remains uniform on both sides of a doorway and
	// independent of the exterior sky's day/night ambient color.
	const FLinearColor Ambient = FLinearColor::White;
	if (InteriorAmbient.Equals(Ambient, .001f)) return;
	InteriorAmbient = Ambient;
	for (const auto& Pair : SurfaceMaterials)
	{
		if ((Pair.Key>>32)==3 && !(Pair.Key & 0x80000000u))
			if (auto* Mid=Pair.Value.Material.Get()) Mid->SetVectorParameterValue(TEXT("InteriorAmbient"), InteriorAmbient);
	}
}

void UACEDatSubsystem::SetInteriorUsesOutdoorAmbient(bool bUseOutdoor)
{
	if (bInteriorUsesOutdoorAmbient == bUseOutdoor) return;
	bInteriorUsesOutdoorAmbient = bUseOutdoor;
	SetInteriorAmbient(OutdoorInteriorAmbient);
}

// A little indirect light keeps unlit corners readable. Authored vertex light
// and local point lights supply the rest of the illumination in enclosed cells.
static constexpr float DungeonAmbient = .14f;

static float DungeonFill()
{
	return ACERuntimeOptions::UniformDungeonLighting() ? 1.f : DungeonAmbient;
}

static void ApplyDungeonSurfaceLighting(UMaterialInstanceDynamic* Material, float Luminosity)
{
	const float Fill = FMath::Max(DungeonFill(), Luminosity);
	Material->SetVectorParameterValue(TEXT("InteriorAmbient"), FLinearColor(Fill, Fill, Fill));
	// With full emissive fill, the DefaultLit portion of the surface must be zero.
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"), ACERuntimeOptions::UniformDungeonLighting() ? 1.f : Luminosity);
}

void UACEDatSubsystem::RefreshDungeonLighting()
{
	// The weak records include materials still in a loaded room after cache trimming.
	for (const auto& Pair : SurfaceMaterials)
		if ((Pair.Key >> 32) == 3 && (Pair.Key & 0x80000000u))
			if (auto* Mid=Pair.Value.Material.Get()) ApplyDungeonSurfaceLighting(Mid, Pair.Value.Luminosity);
	for (const auto& Pair : DungeonObjectMaterials)
		if (auto* Mid=Cast<UMaterialInstanceDynamic>(Pair.Value.Get()))
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), DungeonFill());
	for (const auto& Pair : WorldLightingInstances)
		if (Pair.Value == EObjectLightEnvironment::Dungeon)
			if (auto* Mid=Pair.Key.Get()) Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), DungeonFill());
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateEnvCellMaterial(uint32 SurfaceId, bool bWrapTexture, bool bEnclosed)
{
	const uint32 CacheKey = FACEDatTextureResolver::WorldTextureKey(SurfaceId, bWrapTexture) | (bEnclosed ? 0x80000000u : 0u);
	if (auto* Cached=FindSurfaceMaterial(EnvCellMaterialCache,3,CacheKey)) return Cached;
	if (!TextureResolver) return nullptr;
	FACEDatSurfaceRenderFlags Surface;
	if (!TextureResolver->ResolveSurfaceRenderFlags(SurfaceId, Surface) || Surface.bFullyTransparent) return nullptr;
	if (Surface.bAdditive || (Surface.bSurfaceTranslucent && Surface.Translucency > .2f && !Surface.bClipMap))
		return GetOrCreateTexturedMaterial(SurfaceId, bWrapTexture);
	auto* Source = Cast<UMaterialInstanceDynamic>(GetOrCreateOutdoorLitMaterial(SurfaceId, bWrapTexture));
	if (!Source) return nullptr;
	auto* Base = CreateAceOverlayMaterial(Surface.bClipMap ? TEXT("M_ACEEnvCellMasked_v3") : TEXT("M_ACEEnvCell_v3"),
		Surface.bClipMap ? EACEAceMaterialKind::Masked : EACEAceMaterialKind::Opaque,
		false, false, false, false, false, true, false, false, true);
	if (!Base) return nullptr;
	auto* MID = UMaterialInstanceDynamic::Create(Base, this);
	MID->SetTextureParameterValue(TEXT("ACETexture"), Source->K2_GetTextureParameterValue(TEXT("ACETexture")));
	const float Luminosity = FMath::Clamp(Surface.Luminosity,0.f,1.f);
	if (bEnclosed) ApplyDungeonSurfaceLighting(MID, Luminosity);
	else
	{
		MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Luminosity);
		MID->SetVectorParameterValue(TEXT("InteriorAmbient"), InteriorAmbient);
	}
	MID->SetScalarParameterValue(TEXT("DiffuseStrength"), FMath::Clamp(Surface.Diffuse,0.f,1.f));
	MID->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	// Indoor cells use their own lighting/fog state. Outdoor fog remains enabled
	// on landscape and scenery visible through their portals.
	MID->SetScalarParameterValue(TEXT("FogAmount"), 0.f);
	EnvCellMaterialCache.Add(CacheKey, MID);
	SurfaceMaterials.Add((uint64(3)<<32)|CacheKey, {MID,++SurfaceMaterialClock,Luminosity});
	return MID;
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateOutdoorLitMaterial(uint32 SurfaceId, bool bWrapTexture)
{
	const uint32 CacheKey = FACEDatTextureResolver::WorldTextureKey(SurfaceId, bWrapTexture);
	if (SurfaceId == 0 || !TextureResolver)
	{
		return nullptr;
	}

	if (auto* Cached=FindSurfaceMaterial(OutdoorLitMaterialCache,2,CacheKey)) return Cached;

	FACEDatDecodedSurface Decoded;
	const bool bResolved = TextureResolver->ResolveSurface(SurfaceId, Decoded);
	if (bResolved && IsDatPortalFillSurface(Decoded))
	{
		return nullptr;
	}

	UTexture2D* Tex = TextureResolver->GetOrCreateUTexture(SurfaceId, this, bWrapTexture);
	if (!Tex && bResolved && Decoded.bIsSolid && !IsDatPortalFillSurface(Decoded))
	{
		Tex = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
		if (Tex && Tex->GetPlatformData() && Tex->GetPlatformData()->Mips.Num() > 0)
		{
			Tex->CompressionSettings = TC_EditorIcon;
			Tex->SRGB = true;
			Tex->NeverStream = true;
			FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
			void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
			const FColor C = Decoded.SolidColor.ToFColor(true);
			FMemory::Memcpy(Data, &C, sizeof(FColor));
			Mip.BulkData.Unlock();
			Tex->UpdateResource();
			Tex->Rename(nullptr, this);
		}
		else
		{
			Tex = nullptr;
		}
	}
	if (!Tex)
	{
		return GetVertexColorMaterial();
	}

	UMaterialInterface* Base = EnsureAceOutdoorLitMaterialBase();
	if (bResolved)
	{
		if (Decoded.bAdditive)
		{
			Base = EnsureAceUnlitAdditiveMaterialBase();
		}
		else if (Decoded.bClipMap)
		{
			Base = EnsureAceOutdoorLitMaskedMaterialBase();
		}
		else if (Decoded.bFullyTransparent)
		{
			Base = EnsureAceUnlitTranslucentMaterialBase();
		}
		else if (Decoded.bUsesAlpha)
		{
			const bool bSoftGlass = !Decoded.bHasPixels && Decoded.SolidColor.A < 0.98f;
			const bool bDatBodyTranslucent = bSoftGlass
				|| Decoded.Translucency > 0.35f
				|| (Decoded.bSurfaceTranslucent && Decoded.Translucency > 0.20f);
			if (bDatBodyTranslucent)
			{
				Base = EnsureAceUnlitTranslucentMaterialBase();
			}
		}
	}
	if (!Base)
	{
		Base = EnsureAceOutdoorLitMaterialBase();
	}
	if (!Base)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
	if (!MID)
	{
		return nullptr;
	}
	MID->SetTextureParameterValue(TEXT("ACETexture"), Tex);
	MID->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	MID->SetTextureParameterValue(TEXT("Texture"), Tex);
	MID->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	MID->SetScalarParameterValue(TEXT("DiffuseStrength"), 1.f);
	MID->SetScalarParameterValue(TEXT("AuthoredLuminosity"), FMath::Clamp(Decoded.Luminosity,0.f,1.f));
	MID->SetScalarParameterValue(TEXT("EmissiveStrength"), SceneryEmissiveScale);
	MID->SetVectorParameterValue(TEXT("WorldAmbientTint"),WorldAmbientTint);
	MID->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
	ApplyDistanceFogToMid(MID);
	OutdoorLitMaterialCache.Add(CacheKey, MID);
	SurfaceMaterials.Add((uint64(2)<<32)|CacheKey, {MID,++SurfaceMaterialClock});
	return MID;
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateStaticMeshMaterial(uint32 SurfaceId, bool bWrapTexture)
{
	static constexpr int32 StaticMeshMatVersion = 10;
	static int32 AppliedStaticMeshMatVersion = 0;
	if (AppliedStaticMeshMatVersion != StaticMeshMatVersion)
	{
		AppliedStaticMeshMatVersion = StaticMeshMatVersion;
		StaticMeshMaterialCache.Reset();
		SetupStaticMeshCache.Reset();
		SetupStaticMeshSlotMaterials.Reset();
	}
	// Same live MID as PMC scenery. Runtime MIC + Set*EditorOnly never bound ACETexture
	// on HISM/SMC (black silhouettes, no distance fog).
	return GetOrCreateTexturedMaterial(SurfaceId, bWrapTexture);
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateParticleMaterial(uint32 SurfaceId)
{
	if (!RetailParticleView) RetailParticleView = CreateACERetailParticleView(GetWorld());
	if (SurfaceId == 0 || !TextureResolver)
	{
		return nullptr;
	}

	static constexpr int32 ParticleMatVersion = 20;
	static int32 AppliedParticleMatVersion = 0;
	if (AppliedParticleMatVersion != ParticleMatVersion)
	{
		AppliedParticleMatVersion = ParticleMatVersion;
		ResetSurfaceMaterialCache(ParticleMaterialCache,4);
	}

	if (auto* Cached=FindSurfaceMaterial(ParticleMaterialCache,4,SurfaceId)) return Cached;

	FACEDatDecodedSurface Decoded;
	const bool bResolved = TextureResolver->ResolveSurface(SurfaceId, Decoded);
	if (!bResolved || Decoded.bFullyTransparent)
	{
		return nullptr;
	}
	// Authored ColorValue swatches use the surface blend mode, just like image particles.
	const bool bSolidColorDrip = Decoded.bIsSolid && !Decoded.bClipMap && !Decoded.bFullyTransparent;

	UTexture2D* Tex = nullptr;
	if (bSolidColorDrip)
	{
		// GetOrCreateUTexture often yields a white 1×1 for ColorValue solids. Use the DAT color.
		Tex = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8);
		if (Tex && Tex->GetPlatformData() && Tex->GetPlatformData()->Mips.Num() > 0)
		{
			Tex->CompressionSettings = TC_EditorIcon;
			Tex->SRGB = true;
			Tex->NeverStream = true;
			FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
			void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
			FLinearColor Glow = Decoded.SolidColor;
			Glow.A = FMath::Clamp(Glow.A, 0.f, 1.f);
			const FColor C = Glow.ToFColor(true);
			FMemory::Memcpy(Data, &C, sizeof(FColor));
			Mip.BulkData.Unlock();
			Tex->UpdateResource();
			Tex->Rename(nullptr, this);
		}
		else
		{
			Tex = nullptr;
		}
	}
	else if (Decoded.bHasPixels && Decoded.Pixels.Num() > 0 && Decoded.Width > 0 && Decoded.Height > 0)
	{
		// Never GetOrCreateUTexture here — that path strips A when bUsesAlpha is false
		// (DXT5 / Image-only sprites with holes only in the pixel A channel).
		// The resolver handles ONE/ONE versus SRCALPHA/ONE using the raw
		// surface flags; bUsesAlpha also classifies additive materials.
        // Retail ImgTex caps at four levels; RenderDeviceD3D uses linear min/mag
        // with point mip selection. Extra 2x2/1x1 levels erase small star shapes.
		if (UTexture2D* SpriteTex = FACEDatTextureResolver::CreateTransientRgbaWithMips(
			Decoded.Width, Decoded.Height, Decoded.Pixels, /*bUsesAlpha*/ true,
			TA_Wrap, TA_Wrap, /*bPremultiplyAlpha*/ false, /*bDilateRgbIntoTransparent*/ false,
            /*MaxMipLevels*/ 4, /*Filter*/ TF_Bilinear))
		{
			SpriteTex->Rename(nullptr, this);
			Tex = SpriteTex;
		}
	}
	else
	{
		Tex = TextureResolver->GetOrCreateUTexture(SurfaceId, this);
	}
	if (!Tex)
	{
		return nullptr;
	}

	UMaterialInterface* Base = Decoded.bAdditive
		? EnsureAceParticleAdditiveMaterialBase()
		: EnsureAceParticleTranslucentMaterialBase();
	if (!Base)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
	if (!MID)
	{
		return nullptr;
	}
	MID->SetTextureParameterValue(TEXT("ACETexture"), Tex);
	MID->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	MID->SetTextureParameterValue(TEXT("Texture"), Tex);
	MID->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	MID->SetScalarParameterValue(TEXT("DiffuseStrength"), 1.f);
	// DAT Luminosity. Flooring additive to ≥1 washed torch flames and HW sprites to white.
	float Emis = Decoded.Luminosity > KINDA_SMALL_NUMBER ? Decoded.Luminosity : 0.85f;
	Emis = FMath::Clamp(Emis, 0.35f, 1.35f);
	if (Decoded.bAdditive || Decoded.Luminosity >= 0.75f)
	{
		Emis = FMath::Min(Emis, 1.15f);
	}
	MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Emis);
	MID->SetScalarParameterValue(TEXT("AuthoredLuminosity"), Decoded.Luminosity);
	MID->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
	ApplyDistanceFogToMid(MID);
	MID->SetScalarParameterValue(TEXT("FogAmount"), 0.f);
	ParticleMaterialCache.Add(SurfaceId, MID);
	SurfaceMaterials.Add((uint64(4)<<32)|SurfaceId, {MID,++SurfaceMaterialClock});
	return MID;
}

UMaterialInstanceDynamic* UACEDatSubsystem::CreateSkyParticleMaterial(UMaterialInterface* Source, UObject* Outer)
{
	if (!Source || !TextureResolver) return nullptr;
	for (const auto& Pair : SurfaceMaterials)
	{
		if ((Pair.Key>>32)!=4 || Pair.Value.Material.Get()!=Source) continue;
		FACEDatDecodedSurface Surface;
		if (!TextureResolver->ResolveSurface(uint32(Pair.Key),Surface)) return nullptr;
		auto* Texture=TextureResolver->GetOrCreateSkyUTexture(uint32(Pair.Key),this,false);
		if (!Texture) return nullptr;
		const bool Additive=Surface.bAdditive && !(Surface.bClipMap && Surface.bSurfaceTranslucent);
		auto* Mid=UMaterialInstanceDynamic::Create(Additive ? EnsureAceSkyAdditiveMaterialBase() : EnsureAceSkyTranslucentMaterialBase(),Outer);
		Mid->CopyMaterialUniformParameters(Source);
		Mid->SetTextureParameterValue(TEXT("ACETexture"),Texture);
		Mid->SetScalarParameterValue(TEXT("SkyCardEdgeFade"),Additive ? 1.f : 0.f);
		Mid->SetScalarParameterValue(TEXT("SkyAlphaTestReference"),Surface.bClipMap && !Surface.bSurfaceTranslucent ? Surface.AlphaTestReference : 0.f);
		return Mid;
	}
	return nullptr;
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateResolvedMaterial(
	uint32 SurfaceId, int32 PartIndex, const FACEObjDesc& Appearance, float ObjectTranslucency, uint8 WrapAxes, uint32 PartPaletteId)
{
	if (SurfaceId == 0 || !TextureResolver)
	{
		return nullptr;
	}

	const float ObjectOpacity = 1.f - FMath::Clamp(ObjectTranslucency, 0.f, 1.f);
	const bool bObjectUsesAlpha = ObjectOpacity < (1.f - KINDA_SMALL_NUMBER);
	const bool bSurfaceOverrides = Appearance.HasSurfaceOverrides(PartIndex);
	// Multipart objects must retain authored alpha even without an ObjDesc
	// override. The scenery material path deliberately has different depth
	// heuristics for floors; using it for a newly created corpse made the same
	// translucent body become opaque when its PhysicsDesc translucency was zero.

	uint64 Key = HashCombine(GetTypeHash(SurfaceId), GetTypeHash(PartIndex));
	Key = HashCombine(Key, GetTypeHash(PartPaletteId));
	Key = HashCombine(Key, GetTypeHash(WrapAxes));
	Key = HashCombine(Key, GetTypeHash(Appearance.PaletteBaseId));
	Key = HashCombine(Key, GetTypeHash(FMath::RoundToInt(ObjectOpacity * 255.f)));
	// v2: honor DAT SurfaceType.Translucent (Virindi) — bust stale opaque MIDs from v1 sign fix.
	Key = HashCombine(Key, GetTypeHash(2u));
	for (const FACEObjDescTextureChange& Change : Appearance.TextureChanges)
	{
		if (static_cast<int32>(Change.PartIndex) != PartIndex)
		{
			continue;
		}
		Key = HashCombine(Key, GetTypeHash(Change.OldTexture));
		Key = HashCombine(Key, GetTypeHash(Change.NewTexture));
	}
	for (const FACEObjDescSubPalette& Sp : Appearance.SubPalettes)
	{
		Key = HashCombine(Key, GetTypeHash(Sp.SubPaletteId));
		Key = HashCombine(Key, GetTypeHash(Sp.Offset));
		Key = HashCombine(Key, GetTypeHash(Sp.NumColors));
	}
	// Biology vs headwear depends on AnimPartChanges for this part — must be in the key.
	for (const FACEObjDescAnimPartChange& Ap : Appearance.AnimPartChanges)
	{
		if (static_cast<int32>(Ap.PartIndex) != PartIndex)
		{
			continue;
		}
		Key = HashCombine(Key, GetTypeHash(Ap.PartId));
	}

	ResolvedCacheLastUse.Add(Key, ++ResolvedCacheClock);
	if (const TObjectPtr<UMaterialInstanceDynamic>* Found = ResolvedMaterialCache.Find(Key))
	{
		if (*Found)
		{
			return Found->Get();
		}
	}
	if (auto* Material = LiveResolvedMaterials.FindRef(Key).Get())
	{
		ResolvedMaterialCache.Add(Key, Material);
		return Material;
	}

	FACEDatSurfaceRenderFlags SurfaceFlags;
	if (!bSurfaceOverrides && !bObjectUsesAlpha && WrapAxes == 0
		&& TextureResolver->ResolveSurfaceRenderFlags(SurfaceId, SurfaceFlags)
		&& !SurfaceFlags.bSurfaceTranslucent && SurfaceFlags.Translucency <= KINDA_SMALL_NUMBER
		&& !(SurfaceFlags.bIsSolid && SurfaceFlags.SolidAlpha < .98f))
	{
		auto* Opaque = GetOrCreateTexturedMaterial(SurfaceId);
		// Remember the classification too: resolving a cached surface copies its
		// pixel array, which must not repeat for every same-setup body refresh.
		if (auto* Cached = Cast<UMaterialInstanceDynamic>(Opaque))
		{
			ResolvedMaterialCache.Add(Key, Cached);
			LiveResolvedMaterials.Add(Key, Cached);
		}
		return Opaque; // Opaque models still share the existing material instance.
	}
	FACEDatDecodedSurface Decoded;
	const FACEObjDesc* AppearancePtr = bSurfaceOverrides ? &Appearance : nullptr;
	if (!TextureResolver->ResolveSurfaceWithAppearance(SurfaceId, PartIndex, AppearancePtr, Decoded, PartPaletteId))
	{
		return GetOrCreateTexturedMaterial(SurfaceId);
	}

	if (!Decoded.bHasPixels || Decoded.Width <= 0 || Decoded.Height <= 0)
	{
		if (Decoded.bIsSolid && !Decoded.bFullyTransparent)
		{
			// Solid surfaces need the same swatch as GetOrCreateTexturedMaterial,
			// including opaque surfaces with wrapped UVs or appearance overrides.
			// Otherwise lighting/examination conversion can sample the vertex-color
			// shader's unused white texture instead of the authored ColorValue.
			Decoded.Width = 1;
			Decoded.Height = 1;
			Decoded.bHasPixels = true;
			Decoded.Pixels = { Decoded.SolidColor.ToFColor(true) };
		}
		else
		{
			// Flat opaque fills can continue to use vertex color.
			return GetVertexColorMaterial();
		}
	}

	// CMaterial::SetTranslucencySimple replaces the object alpha. Keep it out of
	// the texture so subsequent Transparent hooks do not multiply it a second time.

	UTexture2D* Tex = nullptr;
	if (const TObjectPtr<UTexture2D>* TexFound = ResolvedTextureCache.Find(Key))
	{
		Tex = TexFound->Get();
	}
	if (!Tex) Tex = LiveResolvedTextures.FindRef(Key).Get();
	if (!Tex)
	{
		Tex = FACEDatTextureResolver::CreateTransientRgbaWithMips(
			Decoded.Width, Decoded.Height, Decoded.Pixels, Decoded.bUsesAlpha || bObjectUsesAlpha,
			(WrapAxes & 1) ? TA_Wrap : TA_Clamp, (WrapAxes & 2) ? TA_Wrap : TA_Clamp);
		if (!Tex)
		{
			return GetVertexColorMaterial();
		}
		Tex->Rename(nullptr, this);
	}
	ResolvedTextureCache.Add(Key, Tex);
	LiveResolvedTextures.Add(Key, Tex);

	UMaterialInterface* Base = EnsureAceUnlitTexturedMaterialBase();
	if (Decoded.bAdditive && !bObjectUsesAlpha)
	{
		Base = EnsureAceUnlitAdditiveMaterialBase();
	}
	else if (Decoded.bClipMap && !bObjectUsesAlpha)
	{
		Base = EnsureAceUnlitMaskedMaterialBase();
	}
	else if (Decoded.bUsesAlpha || bObjectUsesAlpha)
	{
		// Shop signs / EnvCell floors: textured UsesAlpha alone stays opaque.
		// Soft ColorValue glass, PhysicsDesc ObjectTranslucency, and DAT SurfaceType.Translucent
		// / Translucency (Claude Virindi body ≈0.4) use translucent so alpha+glow read correctly.
		const bool bSoftGlass = Decoded.bIsSolid && Decoded.SolidColor.A < 0.98f;
		const bool bDatBodyTranslucent = Decoded.bSurfaceTranslucent || Decoded.Translucency > KINDA_SMALL_NUMBER;
		if (bSoftGlass || bObjectUsesAlpha || bDatBodyTranslucent)
		{
			Base = EnsureAceUnlitTranslucentMaterialBase();
		}
		// else keep opaque textured base
	}
	if (!Base)
	{
		Base = EnsureAceUnlitTexturedMaterialBase();
	}
	if (!Base)
	{
		return GetVertexColorMaterial();
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
	if (!MID)
	{
		return GetVertexColorMaterial();
	}
	MID->SetTextureParameterValue(TEXT("ACETexture"), Tex);
	MID->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	MID->SetTextureParameterValue(TEXT("Texture"), Tex);
	MID->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	MID->SetScalarParameterValue(TEXT("DiffuseStrength"), 1.f);
	float Emis = 1.f;
	if (Decoded.Luminosity > KINDA_SMALL_NUMBER)
	{
		Emis = FMath::Clamp(1.f + Decoded.Luminosity * 2.f, 1.f, 4.f);
	}
	MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Emis);
	MID->SetScalarParameterValue(TEXT("AuthoredLuminosity"), Decoded.Luminosity);
	MID->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
	ApplyDistanceFogToMid(MID);
	ResolvedMaterialCache.Add(Key, MID);
	LiveResolvedMaterials.Add(Key, MID);
	MID->SetScalarParameterValue(TEXT("OpacityMul"), ObjectOpacity);
	return MID;
}

UMaterialInterface* UACEDatSubsystem::CreateExaminationMaterial(UMaterialInterface* Source, UObject* Outer)
{
	if (!Source) return nullptr;
	if (Source->GetMaterial() == GetVertexColorMaterial()->GetMaterial()) return Source;
	for (const auto& Entry : ExaminationMaterialBases)
		if (Source->GetMaterial()==Entry.Value) return Source;
	UTexture* Texture=nullptr;
	if (!Source->GetTextureParameterValue(TEXT("ACETexture"),Texture)||!Texture) return Source;
	const uint8 Blend=uint8(Source->GetBlendMode());
	if (Blend!=BLEND_Opaque && Blend!=BLEND_Masked)
	{
		// Emissive/transparent special-heritage surfaces keep their authored blend.
		float Fog=0.f;if(!Source->GetScalarParameterValue(TEXT("FogAmount"),Fog)||Fog==0.f)return Source;
		auto* Material=UMaterialInstanceDynamic::Create(Source->GetMaterial(),Outer);
		Material->CopyMaterialUniformParameters(Source);Material->SetScalarParameterValue(TEXT("FogAmount"),0.f);return Material;
	}
	const uint8 MaterialKey = Blend | (Source->IsTwoSided() ? 0x80 : 0);
	auto& Base=ExaminationMaterialBases.FindOrAdd(MaterialKey);
	if (!Base)
	{
		const EACEAceMaterialKind Kind=Blend==BLEND_Masked?EACEAceMaterialKind::Masked:Blend==BLEND_Additive?EACEAceMaterialKind::Additive:Blend==BLEND_Translucent?EACEAceMaterialKind::Translucent:EACEAceMaterialKind::Opaque;
		Base=CreateAceOverlayMaterial(*FString::Printf(TEXT("M_ACEExamination_v2_%u"),MaterialKey),Kind,true,false,false,false,Source->IsTwoSided(),false,false,false,false,true);
	}
	if (!Base) return Source;
	auto* Material=UMaterialInstanceDynamic::Create(Base,Outer);
	Material->CopyMaterialUniformParameters(Source);
	Material->SetScalarParameterValue(TEXT("FogAmount"),0.f);
	float Emission=1.f;Source->GetScalarParameterValue(TEXT("EmissiveStrength"),Emission);
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"),FMath::Max(0.f,(Emission-1.f)*.5f));
	Material->SetTextureParameterValue(TEXT("ACETexture"),Texture);
	return Material;
}

void UACEDatSubsystem::UpdateWorldObjectLighting(UMaterialInstanceDynamic* Material, bool bInterior, bool bEnclosed)
{
	if (!Material) return;
	const float Fill = bInterior ? (bEnclosed ? DungeonFill() : 1.f) : SceneryEmissiveScale;
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"), Fill);
	Material->SetVectorParameterValue(TEXT("WorldAmbientTint"), bInterior ? FLinearColor::White : WorldAmbientTint);
	WorldLightingInstances.Add(Material, !bInterior ? EObjectLightEnvironment::Outdoors :
		bEnclosed ? EObjectLightEnvironment::Dungeon : EObjectLightEnvironment::Interior);
}

UMaterialInterface* UACEDatSubsystem::EnsureFarSceneryMaterialBase(uint8 Kind)
{
	auto& Base = FarSceneryMaterialBases.FindOrAdd(Kind);
	if (Base) return Base;
	const FString Name = FString::Printf(TEXT("M_ACEFarScenery_v1_%u"), Kind);
#if WITH_EDITORONLY_DATA
	if (auto* Existing = FindObject<UMaterial>(GetTransientPackage(), *Name))
	{
		Base = Existing;
		return Base;
	}
	UMaterialInterface* Source = Kind == 2 ? EnsureAceBuildingShellMaterialBase()
		: Kind == 1 ? EnsureAceOutdoorLitMaskedMaterialBase() : EnsureAceOutdoorLitMaterialBase();
	if (Kind >= 3)
		Source = CreateAceOverlayMaterial(Kind == 4 ? TEXT("M_ACEEnvCellMasked_v3") : TEXT("M_ACEEnvCell_v3"),
			Kind == 4 ? EACEAceMaterialKind::Masked : EACEAceMaterialKind::Opaque,
			false, false, false, false, false, true, false, false, true);
	if (!Source) return nullptr;
	// Duplicate the proven graph: keep its lighting, world fog, UVs, doorway holes,
	// leaf alpha threshold and depth writing. Only far components use this parent.
	auto* Mat = DuplicateObject<UMaterial>(Source->GetMaterial(), GetTransientPackage(), *Name);
	if (!Mat) return nullptr;
	auto* EditorOnly = Mat->GetEditorOnlyData();
	FExpressionInput OriginalMask = EditorOnly->OpacityMask;
	if (!OriginalMask.Expression)
	{
		auto* Solid = NewObject<UMaterialExpressionConstant>(Mat);
		Solid->R = 1.f;
		EditorOnly->ExpressionCollection.AddExpression(Solid);
		OriginalMask.Expression = Solid;
	}
	auto* Cutoff = NewObject<UMaterialExpressionConstant>(Mat);
	Cutoff->R = Source->GetMaterial()->OpacityMaskClipValue;
	auto* FadeStart = NewObject<UMaterialExpressionConstant>(Mat);
	FadeStart->R = PersonalFarScenery::FadeStartFraction;
	auto* Position = NewObject<UMaterialExpressionWorldPosition>(Mat);
	Position->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	auto* Camera = NewObject<UMaterialExpressionCameraPositionWS>(Mat);
	auto* Limits = NewObject<UMaterialExpressionCollectionParameter>(Mat);
	Limits->Collection = RuntimeFarSceneryCollection();
	Limits->ParameterName = TEXT("FarSceneryLimits");
	Limits->ParameterId = Limits->Collection->GetParameterId(Limits->ParameterName);
	auto* SizeEnd = NewObject<UMaterialExpressionScalarParameter>(Mat);
	SizeEnd->ParameterName = TEXT("FarScenerySizeEnd");
	SizeEnd->DefaultValue = 0.f;
	SizeEnd->bUseCustomPrimitiveData = true;
	SizeEnd->PrimitiveDataIndex = PersonalFarScenery::SizeFadePrimitiveDataIndex;
	auto* Fade = NewObject<UMaterialExpressionCustom>(Mat);
	Fade->Description = TEXT("FarSceneryCoverage");
	Fade->OutputType = CMOT_Float1;
	Fade->Code = TEXT(
		// Clip the original leaf/door mask FIRST. Coverage must never resurrect a
		// filtered alpha fringe, the cause of earlier mobile foliage speckles.
		"clip(A - Cutoff);\n"
		"if (Limits.y < 0.5) return 1.0;\n"
		"float3 delta = W - C;\n"
		"float radiusFade = Limits.x > 0.0 ? 1.0 - smoothstep(Start * Limits.x, Limits.x, length(W.xy - Limits.zw)) : 1.0;\n"
		"float sizeFade = S > 0.0 ? 1.0 - smoothstep(Start * S, S, length(delta)) : 1.0;\n"
		"float coverage = min(radiusFade, sizeFade);\n"
		"if (coverage <= 0.001) return 0.0;\n"
		"if (coverage >= 0.999) return 1.0;\n"
		// Mobile's full/masked prepass clips depth without sample coverage and
		// bypasses the base-pass alpha-to-coverage blend state. Use the same
		// binary coverage in both passes there; positive fractional masks alone
		// would write every depth sample and remain opaque until the endpoint.
		"#if (FORWARD_SHADING && SUPPORTS_PIXEL_COVERAGE) || (SHADING_PATH_MOBILE && !EARLY_Z_PASS_ONLY_MATERIAL_MASKING)\n"
		"if (ResolvedView.NumSceneColorMSAASamples > 1) return coverage;\n"
		"#endif\n"
		// Non-MSAA uses UE's temporal blue noise, only within the narrow fade band.
		// MSAA instead resolves sample coverage without screen-space stippling.
		"return coverage > ViewScalarBlueNoise(uint2(Parameters.SvPosition.xy), View.StateFrameIndex) ? 1.0 : 0.0;\n");
	auto AddInput = [Fade](const TCHAR* Name, UMaterialExpression* Expression)
	{
		auto& Input = Fade->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Expression = Expression;
	};
	Fade->Inputs.Reset();
	AddInput(TEXT("A"), OriginalMask.Expression);
	Fade->Inputs.Last().Input = OriginalMask;
	AddInput(TEXT("Cutoff"), Cutoff);
	AddInput(TEXT("Start"), FadeStart);
	AddInput(TEXT("W"), Position);
	AddInput(TEXT("C"), Camera);
	AddInput(TEXT("Limits"), Limits);
	AddInput(TEXT("S"), SizeEnd);
	UMaterialExpression* AddedExpressions[] = { Cutoff, FadeStart, Position, Camera, Limits, SizeEnd, Fade };
	for (UMaterialExpression* Expression : AddedExpressions)
		EditorOnly->ExpressionCollection.AddExpression(Expression);
	EditorOnly->OpacityMask.Expression = Fade;
	EditorOnly->OpacityMask.OutputIndex = 0;
	Mat->BlendMode = BLEND_Masked;
	Mat->OpacityMaskClipValue = 0.001f;
	Mat->bUseAlphaToCoverage = true;
	Mat->DitherOpacityMask = false;
	Mat->PostEditChange();
	Base = Mat;
#else
	Base = LoadCookedAceMaterial(*Name);
#endif
	return Base;
}

UMaterialInterface* UACEDatSubsystem::GetFarSceneryMaterial(UMaterialInterface* Source, bool bPreserveLighting)
{
	if (!Source) return nullptr;
	auto& Cache = bPreserveLighting ? FarSceneryFixedMaterials : FarSceneryMaterials;
	if (const auto* Cached = Cache.Find(Source))
		if (auto* Existing = Cached->Get()) return Existing;
	UMaterial* Original = Source->GetMaterial();
	uint8 Kind;
	if (Original == EnsureAceBuildingShellMaterialBase()->GetMaterial()) Kind = 2;
	else if (Original == EnsureAceOutdoorLitMaskedMaterialBase()->GetMaterial()) Kind = 1;
	else if (Original == EnsureAceOutdoorLitMaterialBase()->GetMaterial()) Kind = 0;
	else if (Original->GetName() == TEXT("M_ACEEnvCell_v3")) Kind = 3;
	else if (Original->GetName() == TEXT("M_ACEEnvCellMasked_v3")) Kind = 4;
	else return Source; // Authored translucent/additive/portal surfaces retain their own blend.
	UMaterialInterface* Base = EnsureFarSceneryMaterialBase(Kind);
	if (!Base) return Source;
	auto* Material = UMaterialInstanceDynamic::Create(Base, this);
	Material->CopyMaterialUniformParameters(Source);
	Cache.Add(Source, Material);
	if (Kind < 3 && !bPreserveLighting)
	{
		ApplyDistanceFogToMid(Material);
		WorldLightingInstances.Add(Material, EObjectLightEnvironment::Outdoors);
	}
	return Material;
}

UMaterialInterface* UACEDatSubsystem::GetWorldObjectMaterial(UMaterialInterface* Source)
{
	if (!Source) return nullptr;
	// The vertex-color fallback exposes an unused default ACETexture. It is
	// not textured geometry and must retain its vertex-color shader.
	if (Source->GetMaterial() == GetVertexColorMaterial()->GetMaterial()) return Source;
	const EBlendMode Blend = Source->GetBlendMode();
	if (Blend != BLEND_Opaque && Blend != BLEND_Masked) return Source;
	float Emission = 1.f;
	Source->GetScalarParameterValue(TEXT("EmissiveStrength"), Emission);
	// DAT luminous and additive surfaces remain self illuminated.
	if (Emission > 1.01f) return Source;
	UTexture* Texture = nullptr;
	if (!Source->GetTextureParameterValue(TEXT("ACETexture"), Texture) || !Texture) return Source;
	if (const auto* Cached = WorldObjectMaterials.Find(Source)) if (auto* Existing = Cached->Get()) return Existing;
	auto* Base = Blend == BLEND_Masked ? EnsureAceOutdoorLitMaskedMaterialBase() : EnsureAceOutdoorLitMaterialBase();
	if (!Base) return Source;
	auto* Material = UMaterialInstanceDynamic::Create(Base, this);
	Material->CopyMaterialUniformParameters(Source);
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"), SceneryEmissiveScale);
	Material->SetVectorParameterValue(TEXT("WorldAmbientTint"), WorldAmbientTint);
	ApplyDistanceFogToMid(Material);
	WorldObjectMaterials.Add(Source, Material);
	return Material;
}

UMaterialInterface* UACEDatSubsystem::GetUniformInteriorMaterial(UMaterialInterface* Source)
{
	if (!Source) return nullptr;
	if (Source->GetMaterial() == GetVertexColorMaterial()->GetMaterial()) return Source;
	if (const auto* Cached = InteriorObjectMaterials.Find(Source)) if (auto* Existing = Cached->Get()) return Existing;
	const EBlendMode Blend = Source->GetBlendMode();
	if (Blend != BLEND_Opaque && Blend != BLEND_Masked) return Source;
	const uint8 MaterialKey = uint8(Blend) | (Source->IsTwoSided() ? 0x80 : 0);
	auto& Base = InteriorObjectMaterialBases.FindOrAdd(MaterialKey);
	if (!Base) Base = CreateAceOverlayMaterial(*FString::Printf(TEXT("M_ACEInteriorObject_%u"), MaterialKey),
		Blend == BLEND_Masked ? EACEAceMaterialKind::Masked : EACEAceMaterialKind::Opaque,
		false, false, false, false, Source->IsTwoSided());
	if (!Base) return Source;
	auto* Material = UMaterialInstanceDynamic::Create(Base, this);
	Material->CopyMaterialUniformParameters(Source);
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"), 1.f);
	ApplyDistanceFogToMid(Material);
	InteriorObjectMaterials.Add(Source, Material);
	return Material;
}

UMaterialInterface* UACEDatSubsystem::GetDungeonObjectMaterial(UMaterialInterface* Source)
{
	if (const auto* Cached=DungeonObjectMaterials.Find(Source))
		if (auto* Material=Cached->Get()) return Material;
	auto* Lit=GetWorldObjectMaterial(Source);
	if (!Lit || Lit==Source) return Source;
	auto* Material=UMaterialInstanceDynamic::Create(Lit->GetMaterial(),this);
	Material->CopyMaterialUniformParameters(Lit);
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"),DungeonFill());
	Material->SetVectorParameterValue(TEXT("WorldAmbientTint"),FLinearColor::White);
	Material->SetScalarParameterValue(TEXT("FogAmount"),0.f);
	DungeonObjectMaterials.Add(Source,Material);
	return Material;
}

UMaterialInterface* UACEDatSubsystem::GetOrCreateLandMaterial(uint32 PCode, const TArray<FColor>& Pixels, int32 Width, int32 Height,
	FACETerrainBlendCache::FBlendPtr SharedBlend)
{
	if (Width <= 0 || Height <= 0 || (SharedBlend ? !SharedBlend->IsValid() : Pixels.Num() < int64(Width) * Height))
	{
		return nullptr;
	}

	if (const auto* Found = LandMaterialCache.Find(PCode))
	{
		if (Found->IsValid())
		{
			return Found->Get();
		}
	}

	UTexture2D* Tex = nullptr;
	if (const auto* TexFound = LandTextureCache.Find(PCode))
	{
		Tex = TexFound->Get();
	}
	if (!Tex)
	{
		// TexMerge already uses the retail landscape resolution (1024 in the DAT).
		// The mobile object-texture cap must not halve the blended roads/terrain.
		// Upload the baked pixels directly as BGRA8, retaining the full mip chain.
		if (!SharedBlend)
		{
			auto Copy=MakeShared<FACETerrainBlend,ESPMode::ThreadSafe>();
			Copy->Width=Width; Copy->Height=Height; Copy->Pixels.Append(Pixels.GetData(),Width*Height);
			if (!Copy->PrepareUploadMips()) return nullptr;
			SharedBlend=Copy;
		}
		Tex = UACELandTextureMipProvider::CreateTexture(this,MoveTemp(SharedBlend));
		if (!Tex)
		{
			return nullptr;
		}
		Tex->Rename(nullptr, this);
		LandTextureCache.Add(PCode, Tex);
	}

	// Always opaque outdoor land. Look-out CustomStencil MIDs render black — never bake them.
	UMaterialInterface* LandBase = EnsureAceLandMaterialBase();
	if (!LandBase)
	{
		return nullptr;
	}

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(LandBase, this);
	if (!MID)
	{
		return nullptr;
	}
	MID->SetTextureParameterValue(TEXT("SlateUI"), Tex);
	MID->SetTextureParameterValue(TEXT("Texture"), Tex);
	MID->SetTextureParameterValue(TEXT("ACETexture"), Tex);
	MID->SetScalarParameterValue(TEXT("LandDepthBias"), 0.f);
	ApplyDistanceFogToMid(MID);
	ApplyOutdoorPortalLandClipToMid(MID);
	ApplyLandInteriorClipToMid(MID);
	LandMaterialCache.Add(PCode, MID);
	RegisterLandSky(MID);
	return MID;
}

uint64 UACEDatSubsystem::MakeScaleCacheKey(uint32 Id, float WorldScale)
{
	uint32 ScaleBits = 0;
	FMemory::Memcpy(&ScaleBits, &WorldScale, sizeof(ScaleBits));
	return (static_cast<uint64>(Id) << 32) ^ static_cast<uint64>(ScaleBits);
}

uint64 UACEDatSubsystem::MakeSetupCacheKey(uint32 Id, float WorldScale, int32 PlacementId)
{
	// PlacementId is part of the baked bind pose (Default vs Resting).
	// 0x200: PORT skip |Nz| < 0.12 (keep pedestal / town-hall trim).
	return MakeScaleCacheKey(Id, WorldScale)
		^ (static_cast<uint64>(static_cast<uint32>(PlacementId)) << 16)
		^ 0x200ull;
}

uint64 UACEDatSubsystem::MakeSetupStaticMeshCacheKey(uint32 Id, float WorldScale, int32 PlacementId, bool bEnableCollision, bool bParticleGfx, bool bStencilHoleClip, bool bOutdoorLit, bool bPhysicsCollisionOnly)
{
	return MakeSetupCacheKey(Id, WorldScale, PlacementId)
		^ (bEnableCollision ? 1ull : 0ull)
		^ (bParticleGfx ? 2ull : 0ull)
		^ (bStencilHoleClip ? 4ull : 0ull)
		^ 8ull // double-sided complex collision (walkable roofs)
		^ (bOutdoorLit ? 16ull : 0ull)
		^ (bPhysicsCollisionOnly ? 96ull : 0ull) // 32 + 64: physics-only + stair-riser strip
		^ 128ull; // PORT skip |Nz| 0.12
}

const FACEBuiltSetupMesh* UACEDatSubsystem::GetOrBuildSetupMesh(uint32 SetupId, float WorldScale, int32 PlacementId)
{
	return GetOrBuildSetupMeshShared(SetupId, WorldScale, PlacementId).Get();
}

TSharedPtr<const FACEBuiltSetupMesh> UACEDatSubsystem::GetOrBuildSetupMeshShared(uint32 SetupId, float WorldScale, int32 PlacementId)
{
	if (!EnsureLoaded() || !Builder || SetupId == 0)
	{
		return nullptr;
	}
	const uint64 CacheKey = MakeSetupCacheKey(SetupId, WorldScale, PlacementId);
	if (const auto* Existing = SetupMeshCache.Find(CacheKey))
	{
		return *Existing;
	}

	FACEBuiltSetupMesh Built;
	if (!Builder->BuildSetup(SetupId, WorldScale, Built, PlacementId))
	{
		return nullptr;
	}
	FailedSetupKeys.Remove(CacheKey);
	PendingSetupKeys.Remove(CacheKey);
	return SetupMeshCache.Add(CacheKey, MakeShared<FACEBuiltSetupMesh>(MoveTemp(Built)));
}

const FACEBuiltSetupMesh* UACEDatSubsystem::FindSetupMesh(uint32 SetupId, float WorldScale, int32 PlacementId) const
{
	if (SetupId == 0)
	{
		return nullptr;
	}
	return SetupMeshCache.FindRef(MakeSetupCacheKey(SetupId, WorldScale, PlacementId)).Get();
}

UACEDatSubsystem::EACESetupMeshStatus UACEDatSubsystem::RequestSetupMesh(uint32 SetupId, float WorldScale, int32 PlacementId)
{
	if (!EnsureLoaded() || !Builder || SetupId == 0)
	{
		return EACESetupMeshStatus::NotReady;
	}
	const uint64 CacheKey = MakeSetupCacheKey(SetupId, WorldScale, PlacementId);
	if (SetupMeshCache.Contains(CacheKey))
	{
		return EACESetupMeshStatus::Ready;
	}
	if (FailedSetupKeys.Contains(CacheKey))
	{
		return EACESetupMeshStatus::Failed;
	}
	if (PendingSetupKeys.Contains(CacheKey))
	{
		return EACESetupMeshStatus::Pending;
	}
	PendingSetupKeys.Add(CacheKey);
	PendingSetupBuilds.Add({ SetupId, WorldScale, CacheKey, PlacementId });
	PumpSetupBuildQueue();
	return EACESetupMeshStatus::Pending;
}

void UACEDatSubsystem::AllowSetupMeshRetry(uint32 SetupId, float WorldScale, int32 PlacementId)
{
	FailedSetupKeys.Remove(MakeSetupCacheKey(SetupId, WorldScale, PlacementId));
}

void UACEDatSubsystem::PumpSetupBuildQueue()
{
	if (!PortalDat || !Builder)
	{
		return;
	}
	constexpr int32 MaxConcurrent = 8; // was 4 — outdoor scenery ring queues hundreds of Setups
	while (PendingSetupBuilds.Num() > 0 && ActiveSetupBuilds.GetValue() < MaxConcurrent)
	{
		const FPendingScaledMeshBuild Job = PendingSetupBuilds[0];
		PendingSetupBuilds.RemoveAt(0);

		const int32 Generation = SetupBuildGeneration;
		FACEDatDatabase* PortalPtr = PortalDat.Get();
		FACEDatDatabase* HighResPtr = HighResDat.Get();
		TWeakObjectPtr<UACEDatSubsystem> WeakThis(this);
		ActiveSetupBuilds.Increment();
		FThreadSafeCounter* ActiveCounter = &ActiveSetupBuilds;

		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
			[WeakThis, Job, Generation, PortalPtr, HighResPtr, ActiveCounter]()
			{
				TSharedPtr<FACEBuiltSetupMesh> Built = MakeShared<FACEBuiltSetupMesh>();
				bool bOk = false;
				if (PortalPtr)
				{
					FACEDatTextureResolver WorkerTextures(PortalPtr, HighResPtr);
					FACESetupMeshBuilder WorkerBuilder(PortalPtr, HighResPtr, &WorkerTextures);
					bOk = WorkerBuilder.BuildSetup(Job.Id, Job.WorldScale, *Built, Job.PlacementId)
						&& Built->Parts.Num() > 0;
				}
				ActiveCounter->Decrement();
				AsyncTask(ENamedThreads::GameThread,
					[WeakThis, Job, Generation, bOk, Built]()
					{
						if (UACEDatSubsystem* Self = WeakThis.Get())
						{
							Self->OnSetupBuildComplete(Job.Id, Job.WorldScale, Job.CacheKey, Generation, bOk, Built);
						}
					});
			});
	}
}

void UACEDatSubsystem::OnSetupBuildComplete(uint32 SetupId, float WorldScale, uint64 CacheKey, int32 Generation,
	bool bOk, TSharedPtr<FACEBuiltSetupMesh> Mesh)
{
	if (Generation != SetupBuildGeneration)
	{
		PumpSetupBuildQueue();
		return;
	}
	PendingSetupKeys.Remove(CacheKey);
	// A synchronous consumer may have completed this same setup while the job ran.
	// Keep that immutable entry; replacing it can invalidate current consumers.
	if (SetupMeshCache.Contains(CacheKey))
	{
		PumpSetupBuildQueue();
		return;
	}
	if (bOk && Mesh)
	{
		SetupMeshCache.Add(CacheKey, MoveTemp(Mesh));
		FailedSetupKeys.Remove(CacheKey);
	}
	else
	{
		FailedSetupKeys.Add(CacheKey);
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: Setup 0x%08X mesh build failed (async)"), SetupId);
	}
	PumpSetupBuildQueue();
}

UStaticMesh* UACEDatSubsystem::GetOrCreateSetupStaticMesh(uint32 SetupId, float WorldScale, bool bEnableCollision, int32 PlacementId, bool bParticleGfx, bool bStencilHoleClip, bool bOutdoorLit, bool bPhysicsCollisionOnly)
{
	// Finalize cache format invalidation before taking pointers into SetupMeshCache.
	if (SetupId == 0 || !EnsureLoaded())
	{
		return nullptr;
	}
	const uint64 CacheKey = MakeSetupStaticMeshCacheKey(
		SetupId, WorldScale, PlacementId, bEnableCollision, bParticleGfx, bStencilHoleClip, bOutdoorLit,
		bPhysicsCollisionOnly);
	if (TObjectPtr<UStaticMesh>* Existing = SetupStaticMeshCache.Find(CacheKey))
	{
		return Existing->Get();
	}

	TSharedPtr<const FACEBuiltSetupMesh> Built = SetupMeshCache.FindRef(MakeSetupCacheKey(SetupId, WorldScale, PlacementId));
	if (!Built && bParticleGfx)
	{
		Built = GetOrBuildSetupMeshShared(SetupId, WorldScale, PlacementId);
	}
	if (!Built || Built->IsEmpty())
	{
		return nullptr;
	}

	struct FFlatSection
	{
		TArray<FVector> Verts;
		TArray<FVector> Norms;
		TArray<FVector2D> UVs;
		TArray<int32> Tris;
		UMaterialInterface* Mat = nullptr;
		bool bCollide = false;
		bool bCollisionOnly = false;
	};
	TArray<FFlatSection> Flat;

	for (const FACEBuiltSetupPart& Part : Built->Parts)
	{
		const FTransform& Bind = Part.BindTransform;
		for (const FACEBuiltMeshSection& Sec : Part.Sections)
		{
			if (Sec.IsEmpty())
			{
				continue;
			}
			if (bPhysicsCollisionOnly)
			{
				if (!Sec.bCollisionOnly)
				{
					continue;
				}
			}
			else if (Sec.bCollisionOnly)
			{
				// PhysicsBSP is collision-only. Putting it in the visual UStaticMesh (even
				// with a hidden material) shifted section slots so DAT wall MIDs landed on
				// the wrong GPU sections — black shells, textured door trim only. Draw mesh
				// already includes walls; doorway holes are DrawingPortalPolygonIds.
				continue;
			}
			if (!bPhysicsCollisionOnly && Sec.bFullyTransparent)
			{
				continue;
			}
			bool bSoftAlpha = Sec.bClipMap || Sec.bFullyTransparent;
			bool bAdditive = false;
			if (TextureResolver && Sec.SurfaceId != 0)
			{
				FACEDatSurfaceRenderFlags Decoded;
				if (TextureResolver->ResolveSurfaceRenderFlags(Sec.SurfaceId, Decoded))
				{
					if (IsDatPortalFillSurface(Decoded) && !Sec.bClipMap)
					{
						continue;
					}
					bSoftAlpha = bSoftAlpha || Decoded.bUsesAlpha || Decoded.bFullyTransparent || Decoded.bClipMap;
					bAdditive = Decoded.bAdditive;
				}
			}
			UMaterialInterface* Mat = nullptr;
			if (bPhysicsCollisionOnly)
			{
				Mat = EnsureInvisibleCollisionMaterial();
			}
			else
			{
				Mat = bParticleGfx
					? GetOrCreateParticleMaterial(Sec.SurfaceId)
					: (bStencilHoleClip
						? GetOrCreateBuildingShellMaterial(Sec.SurfaceId, Sec.bWrapTexture)
						: (bOutdoorLit
							? GetOrCreateOutdoorLitMaterial(Sec.SurfaceId, Sec.bWrapTexture)
							: GetOrCreateStaticMeshMaterial(Sec.SurfaceId, Sec.bWrapTexture)));
			}
			if (!Mat)
			{
				if (bParticleGfx || bAdditive || Sec.bFullyTransparent)
				{
					continue;
				}
				Mat = GetVertexColorMaterial();
			}
			if (!Mat)
			{
				continue;
			}

			FFlatSection& Out = Flat.AddDefaulted_GetRef();
			Out.Verts = Sec.Vertices;
			Out.Norms = Sec.Normals;
			Out.UVs = Sec.UVs;
			Out.Tris = Sec.Triangles;
			Out.Mat = Mat;
			// Collision is cooked separately from the authored physics faces.
			Out.bCollide = bEnableCollision && bPhysicsCollisionOnly;
			Out.bCollisionOnly = false;
			for (int32 i = 0; i < Out.Verts.Num(); ++i)
			{
				Out.Verts[i] = Bind.TransformPosition(Out.Verts[i]);
				if (Out.Norms.IsValidIndex(i))
				{
					Out.Norms[i] = Bind.TransformVectorNoScale(Out.Norms[i]).GetSafeNormal();
				}
			}


			if (bParticleGfx && Out.Verts.Num() > 0)
			{
				FBox Box(ForceInit);
				for (const FVector& V : Out.Verts)
				{
					Box += V;
				}
				const FVector Ext = Box.GetExtent();
				const float MaxHalf = Ext.GetMax();
				const float MinHalf = Ext.GetMin();
				const bool bCubic = MinHalf > MaxHalf * 0.45f;
				if (bCubic && MaxHalf > WorldScale * 50.f)
				{
					Flat.Pop();
					continue;
				}
			}
		}
	}
	if (Flat.Num() == 0)
	{
		return nullptr;
	}

	// When PhysicsPolygons exist, drop soft-alpha draw tris from the complex body by
	// excluding non-colliding sections from the collision-enabled mesh description.
	// Keep them in the visual mesh: BuildFromMeshDescriptions uses one desc, so instead
	// include all sections but mark collision-only materials as fully translucent after.
	FMeshDescription MeshDesc;
	FStaticMeshAttributes Attributes(MeshDesc);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> MaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(1);

	TArray<UMaterialInterface*> SlotMats;
	SlotMats.Reserve(Flat.Num());

	for (const FFlatSection& Sec : Flat)
	{
		const FPolygonGroupID GroupId = MeshDesc.CreatePolygonGroup();
		const FName SlotName(*FString::Printf(TEXT("ACE_%d"), SlotMats.Num()));
		MaterialSlotNames[GroupId] = SlotName;
		SlotMats.Add(Sec.Mat);

		TMap<int32, FVertexID> VertMap;
		VertMap.Reserve(Sec.Verts.Num());
		for (int32 Vi = 0; Vi < Sec.Verts.Num(); ++Vi)
		{
			const FVertexID Vid = MeshDesc.CreateVertex();
			Positions[Vid] = FVector3f(Sec.Verts[Vi]);
			VertMap.Add(Vi, Vid);
		}

		for (int32 Ti = 0; Ti + 2 < Sec.Tris.Num(); Ti += 3)
		{
			const int32 I0 = Sec.Tris[Ti];
			const int32 I1 = Sec.Tris[Ti + 1];
			const int32 I2 = Sec.Tris[Ti + 2];
			if (!VertMap.Contains(I0) || !VertMap.Contains(I1) || !VertMap.Contains(I2))
			{
				continue;
			}
			TArray<FVertexInstanceID, TInlineAllocator<3>> CornerIds;
			auto AddCorner = [&](int32 Idx)
			{
				const FVertexInstanceID Inst = MeshDesc.CreateVertexInstance(VertMap[Idx]);
				Normals[Inst] = Sec.Norms.IsValidIndex(Idx) ? FVector3f(Sec.Norms[Idx]) : FVector3f::UpVector;
				UVs.Set(Inst, 0, Sec.UVs.IsValidIndex(Idx) ? FVector2f(Sec.UVs[Idx]) : FVector2f::ZeroVector);
				CornerIds.Add(Inst);
			};
			AddCorner(I0);
			AddCorner(I1);
			AddCorner(I2);
			MeshDesc.CreatePolygon(GroupId, CornerIds);
		}
	}

	// Chaos only cooks new triangle bodies at runtime when their owner resolves
	// a game world. The transient package has no world; the DAT subsystem does.
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(this, NAME_None, RF_Transient | RF_DuplicateTransient);
	if (!StaticMesh)
	{
		return nullptr;
	}
	StaticMesh->bAllowCPUAccess = bEnableCollision && !bParticleGfx;

	{
		TArray<FStaticMaterial> StaticMats;
		StaticMats.Reserve(SlotMats.Num());
		for (int32 i = 0; i < SlotMats.Num(); ++i)
		{
			const FName SlotName(*FString::Printf(TEXT("ACE_%d"), i));
			StaticMats.Add(FStaticMaterial(SlotMats[i], SlotName));
			// Fast runtime builds skip the editor's UV-density derivation. DAT
			// textures are resident, but streaming queries still require valid data.
			StaticMats.Last().UVChannelData.bInitialized = true;
#if WITH_EDITORONLY_DATA
			StaticMats.Last().ImportedMaterialSlotName = SlotName;
#endif
		}
		StaticMesh->SetStaticMaterials(StaticMats);
	}

	TArray<const FMeshDescription*> Descs;
	Descs.Add(&MeshDesc);
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bBuildSimpleCollision = false;
	Params.bFastBuild = true;
	Params.bAllowCpuAccess = bEnableCollision && !bParticleGfx;
	Params.bMarkPackageDirty = false;
	// Editor builds require source models even for transient fast-built meshes.
	Params.bCommitMeshDescription = true;
	StaticMesh->BuildFromMeshDescriptions(Descs, Params);

	StaticMesh->CalculateExtendedBounds();
	if (StaticMesh->GetBounds().SphereRadius < 40.f)
	{
		StaticMesh->SetPositiveBoundsExtension(FVector(400.f));
		StaticMesh->SetNegativeBoundsExtension(FVector(400.f));
		StaticMesh->CalculateExtendedBounds();
	}

	// Materials were assigned before the fast build. UStaticMesh::SetMaterial is
	// an editor asset edit: repeating it here launches a derived-data rebuild per
	// slot, invalidating live resources and serializing uninitialized fast-build
	// bounds. Runtime components bind their overrides separately below.

	if (bEnableCollision && bPhysicsCollisionOnly)
    {
        StaticMesh->CreateBodySetup();
        if (UBodySetup* Body = StaticMesh->GetBodySetup())
        {
            Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
            Body->bDoubleSidedGeometry = true;
            Body->InvalidatePhysicsData();
            Body->CreatePhysicsMeshes();
        }
    }
    else if (bEnableCollision)
    {
        // UBodySetup retains its physics mesh as its outer/provider. Sharing the
        // cooked body keeps draw material slots and collision topology independent,
        // including for instanced scenery.
        UStaticMesh* PhysicsMesh = GetOrCreateSetupStaticMesh(SetupId, WorldScale, true,
            PlacementId, false, false, false, true);
        StaticMesh->SetBodySetup(PhysicsMesh ? PhysicsMesh->GetBodySetup() : nullptr);
        if (!PhysicsMesh)
        {
            // Retail falls back to authored Setup cylinders/spheres when the
            // parts have no PhysicsBSP. Trees commonly use this representation.
            // Cook once on the shared mesh, rather than one component per tree.
            TArray<FACEDatCollisionShape> Shapes;
            bool bSetupHasBSP = false;
            GetSetupCollisionShapes(SetupId, Shapes, bSetupHasBSP);
            if (!Shapes.IsEmpty())
            {
                StaticMesh->CreateBodySetup();
                UBodySetup* Body = StaticMesh->GetBodySetup();
                Body->CollisionTraceFlag = CTF_UseSimpleAsComplex;
                for (const auto& Shape : Shapes)
                {
                    if (!FMath::IsFinite(Shape.Radius) || Shape.Radius <= 0.f ||
                        !FMath::IsFinite(Shape.Height) || FVector(Shape.Origin).ContainsNaN()) continue;
                    const FVector Origin = FACEPosition::AceVectorToUnreal(FVector(Shape.Origin), WorldScale);
                    if (Shape.Height > 0.f)
                    {
                        FKConvexElem Cylinder;
                        for (int32 I = 0; I < 64; ++I)
                        {
                            const float Angle = 2.f * PI * I / 64.f;
                            const FVector P = Origin + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0) * Shape.Radius * WorldScale;
                            Cylinder.VertexData.Add(P);
                            Cylinder.VertexData.Add(P + FVector(0, 0, Shape.Height * WorldScale));
                        }
                        Cylinder.UpdateElemBox();
                        Body->AggGeom.ConvexElems.Add(MoveTemp(Cylinder));
                    }
                    else
                    {
                        FKSphereElem Sphere(Shape.Radius * WorldScale);
                        Sphere.Center = Origin;
                        Body->AggGeom.SphereElems.Add(Sphere);
                    }
                }
                Body->InvalidatePhysicsData();
                Body->CreatePhysicsMeshes();
            }
        }
    }

	SetupStaticMeshCache.Add(CacheKey, StaticMesh);
	{
		TArray<TObjectPtr<UMaterialInterface>> Stored;
		Stored.Reserve(SlotMats.Num());
		for (UMaterialInterface* Mat : SlotMats)
		{
			Stored.Add(Mat);
		}
		SetupStaticMeshSlotMaterials.Add(CacheKey, MoveTemp(Stored));
	}
	return StaticMesh;
}

void UACEDatSubsystem::BindSetupStaticMeshMaterials(UStaticMeshComponent* Comp, uint32 SetupId, float WorldScale, bool bEnableCollision, int32 PlacementId, bool bParticleGfx, bool bStencilHoleClip, bool bOutdoorLit, bool bPhysicsCollisionOnly)
{
	if (!Comp)
	{
		return;
	}
	const uint64 CacheKey = MakeSetupStaticMeshCacheKey(
		SetupId, WorldScale, PlacementId, bEnableCollision, bParticleGfx, bStencilHoleClip, bOutdoorLit,
		bPhysicsCollisionOnly);
	const TArray<TObjectPtr<UMaterialInterface>>* Slots = SetupStaticMeshSlotMaterials.Find(CacheKey);
	UStaticMesh* Mesh = Comp->GetStaticMesh();
	const int32 Num = Mesh ? Mesh->GetNumSections(0) : (Slots ? Slots->Num() : Comp->GetNumMaterials());
	for (int32 Si = 0; Si < Num; ++Si)
	{
		UMaterialInterface* Mat = nullptr;
		if (Slots && Slots->IsValidIndex(Si))
		{
			Mat = (*Slots)[Si].Get();
		}
		if (!Mat && Mesh)
		{
			Mat = Mesh->GetMaterial(Si);
		}
		if (Mat)
		{
			Comp->SetMaterial(Si, Mat);
		}
	}
	Comp->MarkRenderStateDirty();
}

UStaticMesh* UACEDatSubsystem::GetOrCreateParticleStaticMesh(uint32 GfxObjId, float WorldScale)
{
	return GetOrCreateSetupStaticMesh(GfxObjId, WorldScale, /*bEnableCollision*/ false, /*PlacementId*/ 0, /*bParticleGfx*/ true);
}

namespace
{
 bool SameAppearance(const FACEObjDesc& A, const FACEObjDesc& B)
 {
  if (A.PaletteBaseId != B.PaletteBaseId || A.SubPalettes.Num() != B.SubPalettes.Num()
   || A.TextureChanges.Num() != B.TextureChanges.Num() || A.AnimPartChanges.Num() != B.AnimPartChanges.Num()) return false;
  for (int32 I=0; I<A.SubPalettes.Num(); ++I)
   if (A.SubPalettes[I].SubPaletteId != B.SubPalettes[I].SubPaletteId || A.SubPalettes[I].Offset != B.SubPalettes[I].Offset
    || A.SubPalettes[I].NumColors != B.SubPalettes[I].NumColors) return false;
  for (int32 I=0; I<A.TextureChanges.Num(); ++I)
   if (A.TextureChanges[I].PartIndex != B.TextureChanges[I].PartIndex || A.TextureChanges[I].OldTexture != B.TextureChanges[I].OldTexture
    || A.TextureChanges[I].NewTexture != B.TextureChanges[I].NewTexture) return false;
  for (int32 I=0; I<A.AnimPartChanges.Num(); ++I)
   if (A.AnimPartChanges[I].PartIndex != B.AnimPartChanges[I].PartIndex || A.AnimPartChanges[I].PartId != B.AnimPartChanges[I].PartId) return false;
  return true;
 }
 SIZE_T AppearanceMeshBytes(const FACEBuiltSetupMesh& Mesh, const FACEObjDesc& Appearance)
 {
  SIZE_T Bytes=sizeof(Mesh)+Mesh.Parts.GetAllocatedSize()+Appearance.SubPalettes.GetAllocatedSize()
   +Appearance.TextureChanges.GetAllocatedSize()+Appearance.AnimPartChanges.GetAllocatedSize();
  for (const auto& Part : Mesh.Parts)
  {
   Bytes+=Part.Sections.GetAllocatedSize()+Part.Portals.GetAllocatedSize()+Part.DistanceLevels.GetAllocatedSize();
   for (const auto& Level : Part.DistanceLevels)
   {
    Bytes+=Level.Sections.GetAllocatedSize();
    for (const auto& Section : Level.Sections)
     Bytes+=Section.Vertices.GetAllocatedSize()+Section.Triangles.GetAllocatedSize()+Section.Normals.GetAllocatedSize()
      +Section.UVs.GetAllocatedSize()+Section.VertexColors.GetAllocatedSize();
   }
   for (const auto& Portal : Part.Portals) Bytes+=Portal.Vertices.GetAllocatedSize();
   for (const auto& Section : Part.Sections)
    Bytes+=Section.Vertices.GetAllocatedSize()+Section.Triangles.GetAllocatedSize()+Section.Normals.GetAllocatedSize()
     +Section.UVs.GetAllocatedSize()+Section.VertexColors.GetAllocatedSize();
  }
  return Bytes;
 }
}

TSharedPtr<const FACEBuiltSetupMesh> UACEDatSubsystem::GetOrBuildSetupAppearanceShared(uint32 SetupId,
 const FACEObjDesc& Appearance, float WorldScale, int32 PlacementId, bool bDistanceLevels)
{
 TRACE_CPUPROFILER_EVENT_SCOPE(ACE_BuildSetupAppearance);
 if (!EnsureLoaded() || !Builder || SetupId == 0) return nullptr;
 if (!bDistanceLevels && !Appearance.HasVisualOverrides()) return GetOrBuildSetupMeshShared(SetupId, WorldScale, PlacementId);
 // ObjDesc hashes are accelerators, not identity: compare every field on a hit.
 const uint64 Key=MakeSetupCacheKey(SetupId, WorldScale, PlacementId) ^ Appearance.GetContentHash()
  ^ (bDistanceLevels ? 0x9E3779B97F4A7C15ull : 0ull);
 if (auto* Entry=AppearanceMeshCache.Find(Key))
 {
  if (Entry->SetupId==SetupId && Entry->WorldScale==WorldScale && Entry->PlacementId==PlacementId
   && Entry->bDistanceLevels==bDistanceLevels && SameAppearance(Entry->Appearance,Appearance))
  {
   Entry->LastUse=++AppearanceMeshUse;
   return Entry->Mesh;
  }
  AppearanceMeshCacheBytes-=Entry->Bytes;
  AppearanceMeshCache.Remove(Key);
 }
 FACEBuiltSetupMesh Built;
 if (!Builder->BuildSetupWithAppearance(SetupId,Appearance,WorldScale,Built,PlacementId,bDistanceLevels)) return nullptr;
 auto Mesh=MakeShared<FACEBuiltSetupMesh>(MoveTemp(Built));
 const SIZE_T Bytes=AppearanceMeshBytes(*Mesh,Appearance)+sizeof(FAppearanceMeshEntry);
 constexpr SIZE_T Budget=PLATFORM_ANDROID ? 8*1024*1024 : 32*1024*1024;
 // Unusually large custom models still render; they simply bypass retention.
 if (Bytes>Budget) return Mesh;
 while (AppearanceMeshCache.Num()>=128 || AppearanceMeshCacheBytes+Bytes>Budget)
 {
  uint64 OldestKey=0, OldestUse=MAX_uint64;
  for (const auto& Pair : AppearanceMeshCache)
   if (Pair.Value.LastUse<OldestUse) { OldestUse=Pair.Value.LastUse;OldestKey=Pair.Key; }
  AppearanceMeshCacheBytes-=AppearanceMeshCache.FindChecked(OldestKey).Bytes;
  AppearanceMeshCache.Remove(OldestKey);
 }
 FAppearanceMeshEntry Entry;
 Entry.bDistanceLevels=bDistanceLevels;
 Entry.SetupId=SetupId;Entry.WorldScale=WorldScale;Entry.PlacementId=PlacementId;Entry.Appearance=Appearance;
 Entry.Mesh=Mesh;Entry.LastUse=++AppearanceMeshUse;Entry.Bytes=Bytes;
 AppearanceMeshCache.Add(Key,MoveTemp(Entry));AppearanceMeshCacheBytes+=Bytes;
 return Mesh;
}

bool UACEDatSubsystem::BuildSetupAppearance(uint32 SetupId, const FACEObjDesc& Appearance, float WorldScale, FACEBuiltSetupMesh& OutMesh, int32 PlacementId)
{
 const auto Mesh=GetOrBuildSetupAppearanceShared(SetupId,Appearance,WorldScale,PlacementId);
 OutMesh=Mesh ? *Mesh : FACEBuiltSetupMesh();
 return Mesh && Mesh->Parts.Num()>0;
}

bool UACEDatSubsystem::GetHoldingLocation(uint32 ParentSetupId, int32 ParentLocation, int32& OutPartIndex, FTransform& OutUnrealRelative, float WorldScale)
{
	OutPartIndex = 0;
	OutUnrealRelative = FTransform::Identity;
	if (!EnsureLoaded() || !Builder || ParentSetupId == 0 || ParentLocation == 0)
	{
		return false;
	}
	FTransform3f AceFrame;
	if (!Builder->GetHoldingLocation(ParentSetupId, ParentLocation, OutPartIndex, AceFrame))
	{
		return false;
	}
	const FQuat4f R = AceFrame.GetRotation();
	OutUnrealRelative = FTransform(
		FACEPosition::AceQuatToUnreal(R.W, FVector(R.X, R.Y, R.Z)),
		FACEPosition::AceVectorToUnreal(
			FVector(AceFrame.GetTranslation().X, AceFrame.GetTranslation().Y, AceFrame.GetTranslation().Z),
			WorldScale),
		FVector(1.f));
	return true;
}

bool UACEDatSubsystem::PrefetchPortalSpaceSetup(int32 SetupId, float WorldScale)
{
	if (!EnsureLoaded() || SetupId == 0)
	{
		return false;
	}
	const auto Mesh = GetOrBuildSetupMeshShared(static_cast<uint32>(SetupId), WorldScale);
	if (!Mesh || Mesh->IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: portal-space Setup 0x%08X prefetch failed"), SetupId);
		return false;
	}
	// Force surface MIDs so the tunnel is textured before the camera switches to it.
	for (const FACEBuiltSetupPart& Part : Mesh->Parts)
	{
		for (const FACEBuiltMeshSection& Sec : Part.Sections)
		{
			if (Sec.SurfaceId != 0 && !Sec.bFullyTransparent)
			{
				GetOrCreateTexturedMaterial(Sec.SurfaceId, Sec.bWrapTexture);
			}
		}
	}
	// Portalspace Animation 0x030005AC (120 part-frames, SoundTweaked 0x0A000316).
	if (MotionPlayer)
	{
		TArray<FTransform> Dummy;
		int32 DummyCount = 0;
		MotionPlayer->EvaluateAnimation(0x030005ACu, 0.f, Mesh->Parts.Num(), Dummy, WorldScale, DummyCount, true);
	}
	GetWave(0x0A000316u);
	UE_LOG(LogTemp, Log, TEXT("ACEDat: portal-space Setup 0x%08X prefetched (%d parts)"),
		SetupId, Mesh->Parts.Num());
	return true;
}

bool UACEDatSubsystem::ApplySetupToProceduralMesh(UProceduralMeshComponent* ProcMesh, int32 SetupId, float WorldScale, bool bEnableCollision, int32 PlacementId, bool bForceDrawCollision, bool bIndoorStairCollision, bool bOutdoorLit, bool bReverseFaces)
{
	return ApplySetupToProceduralMeshInternal(ProcMesh, SetupId, WorldScale, bEnableCollision, PlacementId, /*bParticleSafe*/ false, bForceDrawCollision, bIndoorStairCollision, bOutdoorLit, bReverseFaces);
}

bool UACEDatSubsystem::ApplyParticleGfxToProceduralMesh(UProceduralMeshComponent* ProcMesh, int32 GfxObjId, float WorldScale, bool bEnableCollision)
{
	return ApplySetupToProceduralMeshInternal(ProcMesh, GfxObjId, WorldScale, bEnableCollision, /*PlacementId*/ 0, /*bParticleSafe*/ true, false, false);
}

void UACEDatSubsystem::HideNonOpaqueProcMeshSections(UProceduralMeshComponent* ProcMesh)
{
	if (!IsValid(ProcMesh))
	{
		return;
	}
	const int32 Num = ProcMesh->GetNumSections();
	for (int32 i = 0; i < Num; ++i)
	{
		UMaterialInterface* Mat = ProcMesh->GetMaterial(i);
		if (!Mat)
		{
			continue;
		}
		const EBlendMode Blend = Mat->GetBlendMode();
		if (Blend == BLEND_Translucent || Blend == BLEND_Additive || Blend == BLEND_Modulate)
		{
			ProcMesh->SetMeshSectionVisible(i, false);
		}
	}
}

bool UACEDatSubsystem::ApplySetupToProceduralMeshInternal(UProceduralMeshComponent* ProcMesh, int32 SetupId, float WorldScale, bool bEnableCollision, int32 PlacementId, bool bParticleSafe, bool bForceDrawCollision, bool bIndoorStairCollision, bool bOutdoorLit, bool bReverseFaces)
{
	if (!IsValid(ProcMesh) || SetupId == 0)
	{
		return false;
	}

	const auto Mesh = GetOrBuildSetupMeshShared(static_cast<uint32>(SetupId), WorldScale, PlacementId);
	if (!Mesh || Mesh->IsEmpty())
	{
		return false;
	}

	// Particle pool / scenery can leave a component pending-kill or unregistered mid-cast.
	if (!IsValid(ProcMesh) || ProcMesh->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return false;
	}

	ProcMesh->ClearAllMeshSections();
	if (!IsValid(ProcMesh))
	{
		return false;
	}
	ProcMesh->bUseComplexAsSimpleCollision = bEnableCollision;
	// Keep cooking on the game thread. Async cooks use ThreadPool workers that do not inherit
	// FAppTime in UE 5.8 and fire ensures during scenery/particle mesh creation.
	ProcMesh->bUseAsyncCooking = false;
	int32 SectionIndex = 0;
	int32 CollisionSections = 0;
	int32 PhysicsCollisionSections = 0;
	TArray<UMaterialInterface*> SectionMaterials;

	auto TransformSection = [](const FACEBuiltMeshSection& Sec, const FTransform& Bind,
		TArray<FVector>& OutVerts, TArray<FVector>& OutNorms)
	{
		OutVerts = Sec.Vertices;
		OutNorms.SetNum(OutVerts.Num());
		for (int32 i = 0; i < OutVerts.Num(); ++i)
		{
			OutVerts[i] = Bind.TransformPosition(OutVerts[i]);
			const FVector LocalN = Sec.Normals.IsValidIndex(i) ? Sec.Normals[i] : FVector::UpVector;
			OutNorms[i] = Bind.TransformVectorNoScale(LocalN).GetSafeNormal();
		}
	};


	for (const FACEBuiltSetupPart& Part : Mesh->Parts)
	{
		const FTransform& Bind = Part.BindTransform;
		for (const FACEBuiltMeshSection& Sec : Part.Sections)
		{
			if (Sec.IsEmpty())
			{
				continue;
			}

			// Preserve the PhysicsBSP mesh, including stair risers, independently of rendering.
			if (Sec.bCollisionOnly)
			{
				if (!bEnableCollision)
				{
					continue;
				}
				TArray<FVector> Verts;
				TArray<FVector> Norms;
				TransformSection(Sec, Bind, Verts, Norms);
				TArray<int32> Tris = Sec.Triangles;

				if (Tris.Num() < 3)
				{
					continue;
				}
				ProcMesh->CreateMeshSection_LinearColor(
					SectionIndex,
					Verts,
					Tris,
					Norms,
					Sec.UVs,
					Sec.VertexColors,
					TArray<FProcMeshTangent>(),
					/*bCreateCollision*/ true);
				ProcMesh->SetMeshSectionVisible(SectionIndex, false);
				SectionMaterials.Add(nullptr);
				++CollisionSections;
				++PhysicsCollisionSections;
				++SectionIndex;
				continue;
			}

			if (Sec.bFullyTransparent && !Sec.bClipMap)
			{
				continue;
			}

			// Door/window portal planes are solid ColorValue + Translucency≈1 (magenta/black).
			// Never treat ClipMaps as doorway holes via color keying — that clips character blacks.
			if (TextureResolver && Sec.SurfaceId != 0 && !Sec.bClipMap)
			{
				FACEDatSurfaceRenderFlags Decoded;
				if (TextureResolver->ResolveSurfaceRenderFlags(Sec.SurfaceId, Decoded) && IsDatPortalFillSurface(Decoded))
				{
					continue;
				}
			}

			TArray<FVector> Verts;
			TArray<FVector> Norms;
			TransformSection(Sec, Bind, Verts, Norms);
			TArray<int32> DrawTris = Sec.Triangles;


			// Preserve authored mesh dimensions. Emitter scale is a multiplier, not
			// a target bounding-box size (normalizing cubes also shrank wand particles).
			if (bParticleSafe && Verts.Num() > 0)
			{
				FBox Box(ForceInit);
				for (const FVector& V : Verts)
				{
					Box += V;
				}
				const FVector Ext = Box.GetExtent();
				const float MaxHalf = Ext.GetMax();
				const float MinHalf = Ext.GetMin();
				const bool bCubic = MinHalf > MaxHalf * 0.45f;
				// ±1050 AC HW point-sprite cubes. Shrinking them still left white boxes; skip.
				if (bCubic && MaxHalf > WorldScale * 50.f)
				{
					continue;
				}
			}

			UMaterialInterface* Mat = nullptr;
			if (bParticleSafe)
			{
				Mat = GetOrCreateParticleMaterial(Sec.SurfaceId);
			}
			else if (bReverseFaces)
			{
				Mat = GetOrCreateBuildingShellMaterial(Sec.SurfaceId, Sec.bWrapTexture);
			}
			else if (bOutdoorLit)
			{
				Mat = GetOrCreateOutdoorLitMaterial(Sec.SurfaceId, Sec.bWrapTexture);
			}
			else
			{
				Mat = GetOrCreateTexturedMaterial(Sec.SurfaceId, Sec.bWrapTexture);
			}
			// Additive/alpha/ClipMap FX (Town Network swirls) must never fall back to opaque
			// VertexColorMaterial — that paints bright white discs under the particles.
			bool bSoftAlpha = Sec.bClipMap || Sec.bFullyTransparent;
			bool bAdditive = false;
			if (TextureResolver && Sec.SurfaceId != 0)
			{
				FACEDatSurfaceRenderFlags Decoded;
				if (TextureResolver->ResolveSurfaceRenderFlags(Sec.SurfaceId, Decoded))
				{
					bSoftAlpha = bSoftAlpha || Decoded.bUsesAlpha || Decoded.bFullyTransparent || Decoded.bClipMap;
					bAdditive = Decoded.bAdditive;
				}
			}
			if (!Mat && (bSoftAlpha || bAdditive || Sec.bFullyTransparent || Sec.bClipMap || bParticleSafe))
			{
				// Forced-draw collision (indoor stair Stabs): still emit so soft-alpha wood
				// treads collide. Fully transparent / additive / particles stay skipped.
				if (bForceDrawCollision && !Sec.bFullyTransparent && !bAdditive && !bParticleSafe)
				{
					Mat = GetVertexColorMaterial();
				}
				else
				{
					continue;
				}
			}
			if (!Mat)
			{
				if (bParticleSafe)
				{
					continue;
				}
				// Opaque scenery (fountains / furniture): show DAT-baked vertex colors rather than
				// dropping the section. Avoids silent holes when Surface→MID fails.
				Mat = GetVertexColorMaterial();
			}
			if (!Mat)
			{
				continue;
			}

			// Only the authored PhysicsBSP sections block movement.
			if (!IsValid(ProcMesh))
			{
				return false;
			}
			TArray<FLinearColor> Colors = Sec.VertexColors;
			if (Colors.Num() != Verts.Num())
			{
				Colors.SetNum(Verts.Num());
				for (FLinearColor& C : Colors)
				{
					C = FLinearColor::White;
				}
			}
			ProcMesh->CreateMeshSection_LinearColor(
				SectionIndex,
				Verts,
				DrawTris,
				Norms,
				Sec.UVs,
				Colors,
				TArray<FProcMeshTangent>(),
				/*bCreateCollision*/ false);
			SectionMaterials.Add(Mat);

			++SectionIndex;

		}
	}

	if (!IsValid(ProcMesh))
	{
		return false;
	}
	for (int32 i = 0; i < SectionMaterials.Num(); ++i)
	{
		// Null-check only — IsValid() asserts on GC'd MIDs (ParticleMaterialCache used to
		// lack UPROPERTY and could hand back InternalIndex=-1 after collection).
		if (UMaterialInterface* Mat = SectionMaterials[i])
		{
			ProcMesh->SetMaterial(i, Mat);
		}
	}
	if (bEnableCollision)
	{
		ProcMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		ProcMesh->SetCollisionObjectType(ECC_WorldStatic);
		ProcMesh->SetCollisionResponseToAllChannels(ECR_Block);
		// Architecture (buildings / stabs with forced draw collision) must retract the
		// spring-arm. Character weenie meshes keep Camera Ignore via ConfigurePartCollision.
		if (bForceDrawCollision)
		{
			ProcMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
		}
		else
		{
			ProcMesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		}
		ProcMesh->bUseComplexAsSimpleCollision = true;
		ProcMesh->RecreatePhysicsState();
		if (CollisionSections == 0 && SectionIndex > 0)
		{
			static int32 ZeroCollLogCount = 0;
			if (ZeroCollLogCount < 8)
			{
				++ZeroCollLogCount;
				UE_LOG(LogTemp, Verbose,
					TEXT("ACEDat: Setup 0x%08X forceDraw=%d drawable=%d ZERO collision (soft-alpha/empty PhysicsBSP)"),
					SetupId, bForceDrawCollision ? 1 : 0, SectionIndex);
			}
		}
	}
	else
	{
		ProcMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ProcMesh->SetCollisionObjectType(ECC_WorldStatic);
		ProcMesh->SetCollisionResponseToAllChannels(ECR_Block);
	}
	(void)PhysicsCollisionSections;
	return SectionIndex > 0;
}

bool UACEDatSubsystem::ApplySetupParts(const TArray<UProceduralMeshComponent*>& PartMeshes, int32 SetupId, const FACEObjDesc& Appearance, float WorldScale, TArray<FTransform>& OutBindTransforms, uint32& OutDefaultMotionTableId, int32 PlacementId, bool bEnableCollision, float ObjectTranslucency, const FACEBuiltSetupMesh* Prebuilt)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_ApplySetupParts);
	OutBindTransforms.Reset();
	OutDefaultMotionTableId = 0;

	TSharedPtr<const FACEBuiltSetupMesh> LocalBuilt;
	const FACEBuiltSetupMesh* BuiltPtr = Prebuilt;
	if (!BuiltPtr)
	{
		LocalBuilt = GetOrBuildSetupAppearanceShared(static_cast<uint32>(SetupId), Appearance, WorldScale, PlacementId);
		if (!LocalBuilt || LocalBuilt->Parts.Num() == 0)
		{
			return false;
		}
		BuiltPtr = LocalBuilt.Get();
	}
	else if (BuiltPtr->Parts.Num() == 0)
	{
		return false;
	}
	const FACEBuiltSetupMesh& Built = *BuiltPtr;

	OutDefaultMotionTableId = Built.DefaultMotionTableId;
	const int32 PartCount = FMath::Min(PartMeshes.Num(), Built.Parts.Num());
	OutBindTransforms.SetNum(PartCount);

	for (int32 P = 0; P < PartCount; ++P)
	{
		UProceduralMeshComponent* Proc = PartMeshes[P];
		const FACEBuiltSetupPart& Part = Built.Parts[P];
		OutBindTransforms[P] = Part.BindTransform;
		if (!Proc)
		{
			continue;
		}
		Proc->ClearAllMeshSections();
		Proc->SetViewFacing(Part.DrawMode, Part.SortCenter);
		Proc->ComponentTags.RemoveAll([](FName Tag) { return Tag.ToString().StartsWith(TEXT("ACEPhysicsSection_")); });
		Proc->bUseComplexAsSimpleCollision = bEnableCollision;
		Proc->bUseAsyncCooking = false;
		int32 SectionIndex = 0;
		TArray<UMaterialInterface*> SectionMaterials;
		TArray<FProcMeshDistanceLevel> DistanceLevels;

		for (int32 LevelIndex = 0; LevelIndex < FMath::Max(1, Part.DistanceLevels.Num()); ++LevelIndex)
		{
		const int32 FirstSection = SectionIndex;
		const auto& Sections = LevelIndex == 0 ? Part.Sections : Part.DistanceLevels[LevelIndex].Sections;
		for (const FACEBuiltMeshSection& Sec : Sections)
		{
			if (Sec.IsEmpty())
			{
				continue;
			}
			if (Sec.bCollisionOnly)
			{
				if (!bEnableCollision)
				{
					continue;
				}
				Proc->CreateMeshSection_LinearColor(
					SectionIndex,
					Sec.Vertices,
					Sec.Triangles,
					Sec.Normals,
					Sec.UVs,
					Sec.VertexColors,
					TArray<FProcMeshTangent>(),
					/*bCreateCollision*/ true);
				Proc->SetMeshSectionVisible(SectionIndex, false);
				Proc->ComponentTags.Add(FName(*FString::Printf(TEXT("ACEPhysicsSection_%d"), SectionIndex)));
				SectionMaterials.Add(nullptr);
				++SectionIndex;
				continue;
			}
			if (Sec.bFullyTransparent && !Sec.bClipMap)
			{
				continue;
			}
			// Classification needs four flags, not two copies of the complete
			// high-resolution pixel array for every part of every spawned body.
			FACEDatSurfaceRenderFlags SurfaceFlags;
			const bool bResolvedSurface = TextureResolver && Sec.SurfaceId != 0
				&& TextureResolver->ResolveSurfaceRenderFlags(Sec.SurfaceId, SurfaceFlags);
			if (!Sec.bClipMap && bResolvedSurface && SurfaceFlags.bFullyTransparent) continue;
			// DAT clothing can deliberately address another texture tile. Mukkir
			// Wings use U=0.89..1.95; clamping turns most of their surface black.
			// Only wrap axes that tile, so face patches retain their clamped edges.
			uint8 WrapAxes = 0;
			for (const FVector2D& UV : Sec.UVs)
			{
				if (UV.X < -KINDA_SMALL_NUMBER || UV.X > 1.0 + KINDA_SMALL_NUMBER) WrapAxes |= 1;
				if (UV.Y < -KINDA_SMALL_NUMBER || UV.Y > 1.0 + KINDA_SMALL_NUMBER) WrapAxes |= 2;
			}
			UMaterialInterface* Mat = GetOrCreateResolvedMaterial(
				Sec.SurfaceId, P, Appearance, ObjectTranslucency, WrapAxes, Part.OriginalPaletteId);
			bool bSoftAlpha = Sec.bClipMap || Sec.bFullyTransparent;
			bool bAdditive = false;
			if (bResolvedSurface)
			{
				bSoftAlpha = bSoftAlpha || SurfaceFlags.bUsesAlpha || SurfaceFlags.bFullyTransparent || SurfaceFlags.bClipMap;
				bAdditive = SurfaceFlags.bAdditive;
			}
			if (!Mat && (bSoftAlpha || bAdditive || Sec.bClipMap || Sec.bFullyTransparent))
			{
				continue;
			}
			// Opaque fallback — dropping the section hid head/armor polys when resolve failed.
			if (!Mat)
			{
				Mat = GetVertexColorMaterial();
			}
			if (!Mat)
			{
				continue;
			}
			// ConfigurePartCollision enables drawing sections only for visibility picking.
			// Blocking props and doors use the independent authored physics sections.
			const bool bSectionCollision = false;
			Proc->CreateMeshSection_LinearColor(
				SectionIndex,
				Sec.Vertices,
				Sec.Triangles,
				Sec.Normals,
				Sec.UVs,
				Sec.VertexColors,
				TArray<FProcMeshTangent>(),
				bSectionCollision);
			SectionMaterials.Add(Mat);
			++SectionIndex;
		}
		if (Part.DistanceLevels.IsValidIndex(LevelIndex))
		{
			const auto& L = Part.DistanceLevels[LevelIndex];
			DistanceLevels.Add({L.MinDistance, L.IdealDistance, L.MaxDistance, L.DrawMode, FirstSection, SectionIndex-FirstSection});
		}
		}
		Proc->SetDistanceLevels(MoveTemp(DistanceLevels), WorldScale);
		for (int32 i = 0; i < SectionMaterials.Num(); ++i)
		{
			if (SectionMaterials[i])
			{
				Proc->SetMaterial(i, SectionMaterials[i]);
			}
		}
		if (bEnableCollision && SectionIndex > 0)
		{
			Proc->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Proc->SetCollisionObjectType(ECC_WorldStatic);
			Proc->SetCollisionResponseToAllChannels(ECR_Block);
			Proc->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			Proc->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
			Proc->bUseComplexAsSimpleCollision = true;
			Proc->RecreatePhysicsState();
		}
		else
		{
			Proc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		Proc->SetRelativeTransform(Part.BindTransform);
		Proc->SetVisibility(SectionIndex > 0);
	}
	return PartCount > 0;
}

const FACEBuiltLandblockMesh* UACEDatSubsystem::GetOrBuildLandblockMesh(uint32 LandblockId, float WorldScale)
{
	if (!EnsureLoaded() || !LandBuilder)
	{
		return nullptr;
	}
	const uint32 Key = LandblockId & 0xFFFF0000;
	CancelledLandblockIds.Remove(Key); // A return trip can reuse a still-running bake.
	if (const TSharedPtr<FACEBuiltLandblockMesh>* Existing = LandblockCache.Find(Key))
	{
		// RegionDesc / ambient need Terrain[] — heights alone is not enough. Stale entries
		// that predate terrain capture would permanently starve CollectRegionScenery.
		if (*Existing && (*Existing)->bHasHeights && (*Existing)->bHasTerrain)
		{
			return Existing->Get();
		}
		++LandblockMeshRevision;
		LandblockCache.Remove(Key);
	}

	// Sync path (height sampling / first-time tools): try disk cache before a full rebuild.
	const uint64 Fingerprint = ComputeDatFingerprint();
	LandBuilder->SetCacheFingerprint(Fingerprint);
	const FString CachePath = ACELandblockMeshCache::MakeCacheFilePath(Key, WorldScale, Fingerprint);
	{
		TSharedPtr<FACEBuiltLandblockMesh> FromDisk = MakeShared<FACEBuiltLandblockMesh>();
		if (ACELandblockMeshCache::Load(CachePath, *FromDisk, WorldScale, Fingerprint)
			&& FromDisk->bHasHeights && FromDisk->bHasTerrain)
		{
			FromDisk->LandblockId = Key;
			if (LandBuilder)
			{
				LandBuilder->HydrateSectionBakes(*FromDisk, Fingerprint);
			}
			++LandblockMeshRevision;
			TSharedPtr<FACEBuiltLandblockMesh>& Slot = LandblockCache.Add(Key, MoveTemp(FromDisk));
			return Slot.Get();
		}
	}

	TSharedPtr<FACEBuiltLandblockMesh> Built = MakeShared<FACEBuiltLandblockMesh>();
	if (!LandBuilder->BuildLandblock(Key, WorldScale, *Built))
	{
		return nullptr;
	}
	LandBuilder->HydrateSectionBakes(*Built, Fingerprint);
	// Avoid racing the async worker's Save for the same path.
	if (!PendingLandblockIds.Contains(Key))
	{
		ACELandblockMeshCache::Save(CachePath, *Built, WorldScale, Fingerprint);
	}
	++LandblockMeshRevision;
	TSharedPtr<FACEBuiltLandblockMesh>& Slot = LandblockCache.Add(Key, MoveTemp(Built));
	return Slot.Get();
}

const FACEBuiltLandblockMesh* UACEDatSubsystem::FindLandblockMesh(uint32 LandblockId) const
{
	const uint32 Key = LandblockId & 0xFFFF0000;
	if (const TSharedPtr<FACEBuiltLandblockMesh>* Existing = LandblockCache.Find(Key))
	{
		if (*Existing && (*Existing)->bHasHeights && (*Existing)->bHasTerrain)
		{
			return Existing->Get();
		}
	}
	return nullptr;
}

bool UACEDatSubsystem::IsLandblockMeshInFlight(uint32 LandblockId) const
{
	return PendingLandblockIds.Contains(LandblockId & 0xFFFF0000u);
}

uint64 UACEDatSubsystem::ComputeDatFingerprint() const
{
	if (CachedDatFingerprint != 0)
	{
		return CachedDatFingerprint;
	}
	uint64 Fp = ACELandblockMeshCache::SchemaVersion;
	if (PortalDat)
	{
		Fp ^= PortalDat->GetSourceFingerprint();
	}
	if (CellDat)
	{
		Fp ^= CellDat->GetSourceFingerprint() * 0x9E3779B97F4A7C15ull;
	}
	if (HighResDat)
	{
		Fp ^= HighResDat->GetSourceFingerprint() * 0xC2B2AE3D27D4EB4Full;
	}
	const_cast<UACEDatSubsystem*>(this)->CachedDatFingerprint = Fp;
	return Fp;
}

UACEDatSubsystem::EACELandMeshStatus UACEDatSubsystem::RequestLandblockMesh(uint32 LandblockId, float WorldScale)
{
	if (!EnsureLoaded() || !LandBuilder)
	{
		return EACELandMeshStatus::NotReady;
	}

	const uint32 Key = LandblockId & 0xFFFF0000;
	CancelledLandblockIds.Remove(Key);
	if (FindLandblockMesh(Key))
	{
		return EACELandMeshStatus::Ready;
	}
	if (FailedLandblockMeshIds.Contains(Key))
	{
		return EACELandMeshStatus::Failed;
	}
	if (PendingLandblockIds.Contains(Key))
	{
		// Cell DAT may have been missing when this was first queued — Pump no-ops
		// until CellDat/LandBuilder exist, so retry the pump on every request.
		PumpLandblockBuildQueue();
		return EACELandMeshStatus::Pending;
	}

	PendingLandblockIds.Add(Key);
	PendingLandblockBuilds.Add({ Key, WorldScale });
	PumpLandblockBuildQueue();
	return EACELandMeshStatus::Pending;
}

void UACEDatSubsystem::CancelLandblockMeshRequestsOutside(const TSet<int32>& KeepLandblockIds)
{
	for (uint32 Id : PendingLandblockIds)
		if (!KeepLandblockIds.Contains(static_cast<int32>(Id))) CancelledLandblockIds.Add(Id);
	for (int32 i = PendingLandblockBuilds.Num() - 1; i >= 0; --i)
	{
		const uint32 Id = PendingLandblockBuilds[i].LandblockId;
		if (!KeepLandblockIds.Contains(static_cast<int32>(Id)))
		{
			PendingLandblockIds.Remove(Id);
			CancelledLandblockIds.Remove(Id); // Queued job has no completion to discard.
			PendingLandblockBuilds.RemoveAt(i);
		}
	}
}

void UACEDatSubsystem::EvictLandblockMeshesOutside(const TSet<int32>& KeepLandblockIds)
{
	TArray<uint32> ToRemove;
	ToRemove.Reserve(LandblockCache.Num());
	for (const auto& Pair : LandblockCache)
	{
		if (!Pair.Value.IsValid())
		{
			ToRemove.Add(Pair.Key);
			continue;
		}
		if (!KeepLandblockIds.Contains(static_cast<int32>(Pair.Key & 0xFFFF0000u)))
		{
			ToRemove.Add(Pair.Key);
		}
	}
	for (uint32 Key : ToRemove)
	{
		++LandblockMeshRevision;
		LandblockCache.Remove(Key);
		BuildingInteriorFootprints.Remove(Key & 0xFFFF0000u);
	}
	if (ToRemove.Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: evicted %d landblock mesh cache entries (keep=%d)"),
			ToRemove.Num(), KeepLandblockIds.Num());
	}
}

void UACEDatSubsystem::AllowLandblockMeshRetry(uint32 LandblockId)
{
	FailedLandblockMeshIds.Remove(LandblockId & 0xFFFF0000u);
}

void UACEDatSubsystem::PumpLandblockBuildQueue()
{
	if (!PortalDat || !CellDat || !LandBuilder)
	{
		return;
	}

	// WorldBuilder-style: several background landblock bakes in flight (disk cache + DAT).
	// Serializing to one job left the 5×5 ring waiting on a single worker.
	constexpr int32 MaxConcurrentLandblockBuilds = 2;
	while (PendingLandblockBuilds.Num() > 0
		&& ActiveLandblockBuilds.GetValue() < MaxConcurrentLandblockBuilds)
	{
		const FPendingLandblockBuild Job = PendingLandblockBuilds[0];
		PendingLandblockBuilds.RemoveAt(0);

		const int32 Generation = LandblockBuildGeneration;
		const uint64 Fingerprint = ComputeDatFingerprint();
		const auto BlendCache = LandBuilder->GetBlendCache();
		const FString CachePath = ACELandblockMeshCache::MakeCacheFilePath(Job.LandblockId, Job.WorldScale, Fingerprint);

		// Worker-owned builders: LandBuilder / TextureResolver caches are not thread-safe to share.
		FACEDatDatabase* PortalPtr = PortalDat.Get();
		FACEDatDatabase* HighResPtr = HighResDat.Get();
		FACEDatDatabase* CellPtr = CellDat.Get();
		// Always CPU bake for land until GPU material is re-enabled.
		const FACELandSurfaceAtlas* AtlasPtr = nullptr;
		TWeakObjectPtr<UACEDatSubsystem> WeakThis(this);

		ActiveLandblockBuilds.Increment();
		FThreadSafeCounter* ActiveCounter = &ActiveLandblockBuilds;
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
			[WeakThis, Job, Generation, Fingerprint, CachePath, PortalPtr, HighResPtr, CellPtr, AtlasPtr, ActiveCounter, BlendCache]()
			{
				TSharedPtr<FACEBuiltLandblockMesh> Built = MakeShared<FACEBuiltLandblockMesh>();
				bool bOk = false;

				// Prefer disk cache — pure I/O + decompress, then hydrate PCode bakes.
				if (ACELandblockMeshCache::Load(CachePath, *Built, Job.WorldScale, Fingerprint))
				{
					Built->LandblockId = Job.LandblockId;
					if (PortalPtr && CellPtr)
					{
						FACEDatTextureResolver WorkerTextures(PortalPtr, HighResPtr);
						FACELandblockMeshBuilder WorkerBuilder(PortalPtr, CellPtr, &WorkerTextures, BlendCache);
						WorkerBuilder.SetCacheFingerprint(Fingerprint);
						WorkerBuilder.SetLandSurfaceAtlas(AtlasPtr);
						WorkerBuilder.HydrateSectionBakes(*Built, Fingerprint);
					}
					bOk = Built->bHasHeights && Built->bHasTerrain && !Built->IsEmpty();
				}
				else if (PortalPtr && CellPtr)
				{
					FACEDatTextureResolver WorkerTextures(PortalPtr, HighResPtr);
					FACELandblockMeshBuilder WorkerBuilder(PortalPtr, CellPtr, &WorkerTextures, BlendCache);
					WorkerBuilder.SetCacheFingerprint(Fingerprint);
					WorkerBuilder.SetLandSurfaceAtlas(AtlasPtr);
					if (WorkerBuilder.BuildLandblock(Job.LandblockId, Job.WorldScale, *Built))
					{
						WorkerBuilder.HydrateSectionBakes(*Built, Fingerprint);
						bOk = true;
						ACELandblockMeshCache::Save(CachePath, *Built, Job.WorldScale, Fingerprint);
					}
				}

				ActiveCounter->Decrement();

				AsyncTask(ENamedThreads::GameThread,
					[WeakThis, Job, Generation, bOk, Built]()
					{
						if (UACEDatSubsystem* Self = WeakThis.Get())
						{
							Self->OnLandblockBuildComplete(Job.LandblockId, Job.WorldScale, Generation, bOk, Built);
						}
					});
			});
	}
}

void UACEDatSubsystem::OnLandblockBuildComplete(uint32 LandblockId, float WorldScale, int32 Generation,
	bool bOk, TSharedPtr<FACEBuiltLandblockMesh> Mesh)
{
	if (Generation != LandblockBuildGeneration)
	{
		PumpLandblockBuildQueue();
		return;
	}

	PendingLandblockIds.Remove(LandblockId);
	if (CancelledLandblockIds.Remove(LandblockId) > 0)
	{
		PumpLandblockBuildQueue();
		return;
	}
	if (bOk && Mesh && Mesh->bHasHeights && Mesh->bHasTerrain)
	{
		++LandblockMeshRevision;
		LandblockCache.FindOrAdd(LandblockId) = Mesh;
		FailedLandblockMeshIds.Remove(LandblockId);
		UE_LOG(LogTemp, Verbose, TEXT("ACEDat: landblock 0x%08X mesh ready (async)"), LandblockId);
	}
	else
	{
		FailedLandblockMeshIds.Add(LandblockId);
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: landblock 0x%08X mesh build failed (async)"), LandblockId);
	}

	PumpLandblockBuildQueue();
}

bool UACEDatSubsystem::ConsumeLandMeshReloadRequest(bool* bOutClearEnvCells)
{
	if (bOutClearEnvCells)
	{
		*bOutClearEnvCells = false;
	}
	// v105: stair Stab soft-alpha collision; sky no longer texture-thrashed on unpack.
	// Rebuild land + EnvCells + Setup. Do NOT ReleaseTextureObjects.
	static constexpr uint32 LandMeshFormatVersion = 121u;
	static uint32 AppliedLandMeshFormatVersion = 0u;
	if (AppliedLandMeshFormatVersion != LandMeshFormatVersion)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ACEDat: landmesh format %u → %u — land + EnvCell + Setup rebuild (keep shared textures)"),
			AppliedLandMeshFormatVersion, LandMeshFormatVersion);
		AppliedLandMeshFormatVersion = LandMeshFormatVersion;
		++LandblockBuildGeneration;
		++EnvCellBuildGeneration;
		++SetupBuildGeneration;
		PendingLandblockBuilds.Reset();
		PendingLandblockIds.Reset();
    CancelledLandblockIds.Reset();
		FailedLandblockMeshIds.Reset();
		PendingEnvCellBuilds.Reset();
		PendingEnvCellKeys.Reset();
    CancelledEnvCellKeys.Reset();
		FailedEnvCellKeys.Reset();
		PendingSetupBuilds.Reset();
		PendingSetupKeys.Reset();
		FailedSetupKeys.Reset();
		CachedDatFingerprint = 0;
		++LandblockMeshRevision;
		LandblockCache.Reset();
		LandblockInfoCache.Reset();
		DoorwayGeometryCache.Reset();
		SetupRuntimeMetadataCache.Reset();
		UncachedSetupRuntimeMetadata = FSetupRuntimeMetadata();
		EnvCellMeshCache.Reset();
		SetupMeshCache.Reset();
		ClearAppearanceMeshCache();
		BuildingInteriorFootprints.Reset();
		LandMaterialCache.Reset();
		LandMaterialBase = nullptr;
		LookOutLandMaterialCache.Reset();
		LandLookOutMaterialBase = nullptr;
		BuildingShellMaterialBase = nullptr;
		PortalStencilWriterMaterial = nullptr;
		PortalDepthApertureMaterial = nullptr;
		bLandLookOutClip = false;

		bPortalLookOutLand = false;
		for (auto& Pair : LandTextureCache)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->RemoveFromRoot();
			}
		}
		LandTextureCache.Reset();
		bLandMeshReloadRequested = false;
		if (bOutClearEnvCells)
		{
			*bOutClearEnvCells = true;
		}
		return true;
	}
	if (!bLandMeshReloadRequested)
	{
		return false;
	}
	bLandMeshReloadRequested = false;
	BuildingInteriorFootprints.Reset();
	return true;
}

bool UACEDatSubsystem::SampleOutdoorGroundZ(double UnrealX, double UnrealY, float WorldScale, float& OutUnrealZ, FVector* OutUnrealNormal)
{
	if (WorldScale <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const double GlobalX = -UnrealX / WorldScale;
	const double GlobalY = UnrealY / WorldScale;
	const int32 Lbx = FMath::FloorToInt(GlobalX / FACELandblockMeshBuilder::LandblockSize);
	const int32 Lby = FMath::FloorToInt(GlobalY / FACELandblockMeshBuilder::LandblockSize);
	if (Lbx < 0 || Lbx > 255 || Lby < 0 || Lby > 255)
	{
		return false;
	}

	const uint32 LandblockId = (static_cast<uint32>(Lbx) << 24) | (static_cast<uint32>(Lby) << 16);
	// Never sync-bake on the movement path — hitch if cache miss. Request async and skip.
	const FACEBuiltLandblockMesh* Mesh = FindLandblockMesh(LandblockId);
	if (!Mesh || !Mesh->bHasHeights || !Mesh->bHasTerrain)
	{
		RequestLandblockMesh(LandblockId, WorldScale);
		return false;
	}

	const float LocalX = GlobalX - static_cast<float>(Lbx) * FACELandblockMeshBuilder::LandblockSize;
	const float LocalY = GlobalY - static_cast<float>(Lby) * FACELandblockMeshBuilder::LandblockSize;
	float ZAc = 0.f;
	FVector NormalAc;
	if (!FACELandblockMeshBuilder::SampleHeightAc(*Mesh, LocalX, LocalY, ZAc, OutUnrealNormal ? &NormalAc : nullptr))
	{
		return false;
	}

	// Retail ValidateWalkable: contact settles waterDepth below the land plane.
	const float DepthAc = FACELandblockMeshBuilder::GetWaterDepthAc(*Mesh, LocalX, LocalY);
	OutUnrealZ = (ZAc - DepthAc) * WorldScale;
	if (OutUnrealNormal) *OutUnrealNormal = FACEPosition::AceVectorToUnreal(NormalAc, 1.f);
	return true;
}

float UACEDatSubsystem::GetOutdoorWaterDepthCm(double UnrealX, double UnrealY, float WorldScale)
{
	if (WorldScale <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}
	const double GlobalX = -UnrealX / WorldScale;
	const double GlobalY = UnrealY / WorldScale;
	const int32 Lbx = FMath::FloorToInt(GlobalX / FACELandblockMeshBuilder::LandblockSize);
	const int32 Lby = FMath::FloorToInt(GlobalY / FACELandblockMeshBuilder::LandblockSize);
	if (Lbx < 0 || Lbx > 255 || Lby < 0 || Lby > 255)
	{
		return 0.f;
	}
	const uint32 LandblockId = (static_cast<uint32>(Lbx) << 24) | (static_cast<uint32>(Lby) << 16);
	const FACEBuiltLandblockMesh* Mesh = FindLandblockMesh(LandblockId);
	if (!Mesh || !Mesh->bHasHeights || !Mesh->bHasTerrain)
	{
		RequestLandblockMesh(LandblockId, WorldScale);
		Mesh = FindLandblockMesh(LandblockId);
		if (!Mesh || !Mesh->bHasHeights || !Mesh->bHasTerrain)
		{
			return 0.f;
		}
	}
	const float LocalX = GlobalX - static_cast<float>(Lbx) * FACELandblockMeshBuilder::LandblockSize;
	const float LocalY = GlobalY - static_cast<float>(Lby) * FACELandblockMeshBuilder::LandblockSize;
	return FACELandblockMeshBuilder::GetWaterDepthAc(*Mesh, LocalX, LocalY) * WorldScale;
}

uint8 UACEDatSubsystem::GetOutdoorBlockWaterType(double UnrealX, double UnrealY, float WorldScale)
{
	if (WorldScale <= KINDA_SMALL_NUMBER)
	{
		return 0;
	}
	const double GlobalX = -UnrealX / WorldScale;
	const double GlobalY = UnrealY / WorldScale;
	const int32 Lbx = FMath::FloorToInt(GlobalX / FACELandblockMeshBuilder::LandblockSize);
	const int32 Lby = FMath::FloorToInt(GlobalY / FACELandblockMeshBuilder::LandblockSize);
	if (Lbx < 0 || Lbx > 255 || Lby < 0 || Lby > 255)
	{
		return 0;
	}
	const uint32 LandblockId = (static_cast<uint32>(Lbx) << 24) | (static_cast<uint32>(Lby) << 16);
	const FACEBuiltLandblockMesh* Mesh = FindLandblockMesh(LandblockId);
	if (!Mesh)
	{
		RequestLandblockMesh(LandblockId, WorldScale);
		Mesh = FindLandblockMesh(LandblockId);
	}
	return Mesh ? Mesh->BlockWaterType : 0;
}

uint8 UACEDatSubsystem::GetOutdoorCellWaterType(double UnrealX, double UnrealY, float WorldScale)
{
	if (WorldScale <= KINDA_SMALL_NUMBER)
	{
		return 0;
	}
	const double GlobalX = -UnrealX / WorldScale;
	const double GlobalY = UnrealY / WorldScale;
	const int32 Lbx = FMath::FloorToInt(GlobalX / FACELandblockMeshBuilder::LandblockSize);
	const int32 Lby = FMath::FloorToInt(GlobalY / FACELandblockMeshBuilder::LandblockSize);
	if (Lbx < 0 || Lbx > 255 || Lby < 0 || Lby > 255)
	{
		return 0;
	}
	const uint32 LandblockId = (static_cast<uint32>(Lbx) << 24) | (static_cast<uint32>(Lby) << 16);
	const FACEBuiltLandblockMesh* Mesh = FindLandblockMesh(LandblockId);
	if (!Mesh)
	{
		RequestLandblockMesh(LandblockId, WorldScale);
		Mesh = FindLandblockMesh(LandblockId);
	}
	if (!Mesh)
	{
		return 0;
	}
	const float LocalX = GlobalX - static_cast<float>(Lbx) * FACELandblockMeshBuilder::LandblockSize;
	const float LocalY = GlobalY - static_cast<float>(Lby) * FACELandblockMeshBuilder::LandblockSize;
	constexpr float CellSize = FACELandblockMeshBuilder::LandblockSize / 8.f;
	const int32 CellX = FMath::Clamp(FMath::FloorToInt(LocalX / CellSize), 0, 7);
	const int32 CellY = FMath::Clamp(FMath::FloorToInt(LocalY / CellSize), 0, 7);
	return Mesh->CellWaterType[CellX * 8 + CellY];
}

const UACEDatSubsystem::FBuildingInteriorMask& UACEDatSubsystem::GetOrBuildBuildingInteriorFootprints(
	uint32 LandblockId, float WorldScale)
{
	const uint32 Key = LandblockId & 0xFFFF0000u;
	if (const FBuildingInteriorMask* Existing = BuildingInteriorFootprints.Find(Key))
	{
		if (Existing->bComplete)
		{
			return *Existing;
		}
		BuildingInteriorFootprints.Remove(Key);
	}

	FBuildingInteriorMask& Out = BuildingInteriorFootprints.Add(Key);
	Out.bComplete = true;
	const auto Snapshot = GetLandblockInfo(Key);
	if (!Snapshot || Snapshot->Buildings.Num() == 0)
	{
		return Out;
	}
	const auto& Info = *Snapshot;

	auto AppendFloorTri = [](TArray<FInteriorHoleTri>& Dest, FVector2D A, FVector2D B, FVector2D C, float Z)
	{
		// No dilation — pad/shell hulls punched land outside the interior.
		FInteriorHoleTri Hole;
		Hole.A = A;
		Hole.B = B;
		Hole.C = C;
		Hole.Z = Z;
		Dest.Add(Hole);
	};

	auto AppendShellTri = [](TArray<FInteriorHoleTri>& Dest, FVector2D A, FVector2D B, FVector2D C)
	{
		FInteriorHoleTri Hole;
		Hole.A = A;
		Hole.B = B;
		Hole.C = C;
		Hole.Z = 0.f;
		Dest.Add(Hole);
	};

	// Ensure heightfield exists before grade tests.
	const FACEBuiltLandblockMesh* LandMesh = GetOrBuildLandblockMesh(Key, WorldScale);
	auto SampleTerrainZAt = [&](float UnrealX, float UnrealY, float& OutTerrainZ) -> bool
	{
		if (!LandMesh || !LandMesh->bHasHeights || WorldScale <= KINDA_SMALL_NUMBER)
		{
			return false;
		}
		const float LocalXAc = -UnrealX / WorldScale;
		const float LocalYAc = UnrealY / WorldScale;
		if (LocalXAc < -KINDA_SMALL_NUMBER
			|| LocalXAc > FACELandblockMeshBuilder::LandblockSize + KINDA_SMALL_NUMBER
			|| LocalYAc < -KINDA_SMALL_NUMBER
			|| LocalYAc > FACELandblockMeshBuilder::LandblockSize + KINDA_SMALL_NUMBER)
		{
			return false;
		}
		float TerrainZAc = 0.f;
		if (!FACELandblockMeshBuilder::SampleHeightAc(*LandMesh, LocalXAc, LocalYAc, TerrainZAc))
		{
			return false;
		}
		OutTerrainZ = TerrainZAc * WorldScale;
		return true;
	};

	auto CollectFloorTrisForCells = [&](const TSet<uint32>& CellIds, TArray<FInteriorHoleTri>& FloorOut,
		TArray<FInteriorHoleTri>& CeilingOut, float GradeZ)
	{
		// Ground-ish EnvCell floors only (not upper stories). Used for flora / land-collision ignore.
		constexpr float AboveGradeSlackCm = 150.f;
		constexpr float BelowGradeMaxCm = 1200.f;
		const float MaxKeepZ = GradeZ + AboveGradeSlackCm;
		const float MinKeepZ = GradeZ - BelowGradeMaxCm;

		struct FFloorCand
		{
			FVector2D A, B, C;
			float Z = 0.f;
		};

		for (uint32 CellId : CellIds)
		{
			const FACEBuiltEnvCellMesh* CellMesh = GetOrBuildEnvCellMesh(CellId, WorldScale);
			if (!CellMesh)
			{
				continue;
			}
			const FTransform ToLb = CellMesh->GetCellLocalToLandblock(WorldScale);

			TArray<FFloorCand> CellCands;

			auto ConsiderUpFace = [&](const FVector& W0, const FVector& W1, const FVector& W2, bool bAllowCeiling)
			{
				const FVector FaceN = FVector::CrossProduct(W1 - W0, W2 - W0).GetSafeNormal();
				const FVector Center = (W0 + W1 + W2) / 3.f;
				if (Center.Z > MaxKeepZ || Center.Z < MinKeepZ)
				{
					return;
				}
				if (FaceN.Z <= -0.20f)
				{
					if (bAllowCeiling)
					{
						AppendFloorTri(CeilingOut, FVector2D(W0.X, W0.Y), FVector2D(W1.X, W1.Y),
							FVector2D(W2.X, W2.Y), Center.Z);
					}
					return;
				}
				if (FaceN.Z < 0.20f)
				{
					return;
				}
				FFloorCand Cand;
				Cand.A = FVector2D(W0.X, W0.Y);
				Cand.B = FVector2D(W1.X, W1.Y);
				Cand.C = FVector2D(W2.X, W2.Y);
				Cand.Z = Center.Z;
				CellCands.Add(Cand);
			};

			// Draw floors + PhysicsBSP/collision floors — some shops only have solid floors
			// in CollisionSections (draw may be portal/transparent). Never Setup shells.
			auto WalkSections = [&](const TArray<FACEBuiltMeshSection>& Secs, bool bAllowCeiling)
			{
				for (const FACEBuiltMeshSection& Sec : Secs)
				{
					if (Sec.bFullyTransparent)
					{
						continue;
					}
					for (int32 T = 0; T + 2 < Sec.Triangles.Num(); T += 3)
					{
						const int32 I0 = Sec.Triangles[T];
						const int32 I1 = Sec.Triangles[T + 1];
						const int32 I2 = Sec.Triangles[T + 2];
						if (!Sec.Vertices.IsValidIndex(I0) || !Sec.Vertices.IsValidIndex(I1)
							|| !Sec.Vertices.IsValidIndex(I2))
						{
							continue;
						}
						ConsiderUpFace(
							ToLb.TransformPosition(Sec.Vertices[I0]),
							ToLb.TransformPosition(Sec.Vertices[I1]),
							ToLb.TransformPosition(Sec.Vertices[I2]),
							bAllowCeiling);
					}
				}
			};
			WalkSections(CellMesh->Sections, true);
			WalkSections(CellMesh->CollisionSections, false);

			for (const FFloorCand& Cand : CellCands)
			{
				AppendFloorTri(FloorOut, Cand.A, Cand.B, Cand.C, Cand.Z);
			}
			// Exact upward floor faces only — no convex-hull expand (bled past frames).
		}
	};

	int32 TotalFloor = 0;
	int32 TotalShell = 0;
	int32 MissingEnvCells = 0;

	for (int32 Bi = 0; Bi < Info.Buildings.Num(); ++Bi)
	{
		const FACEDatLandblockBuilding& Building = Info.Buildings[Bi];
		FBuildingHoleMask Bld;
		Bld.LandblockBuildingIndex = Bi;

		// Per-building EnvCell floors from that building's portal cells + connected rooms.
		TSet<uint32> CellIds;
		TArray<uint32> Queue;
		auto EnqueueIndoor = [&](uint16 ShortId)
		{
			if (ShortId == 0 || ShortId == 0xFFFFu || ShortId < 0x0100u)
			{
				return;
			}
			const uint32 CellId = Key | ShortId;
			if (!CellIds.Contains(CellId))
			{
				CellIds.Add(CellId);
				Queue.Add(CellId);
			}
		};
		for (uint16 ShortId : Building.PortalCellIds)
		{
			EnqueueIndoor(ShortId);
		}
		for (int32 Qi = 0; Qi < Queue.Num(); ++Qi)
		{
			const FACEBuiltEnvCellMesh* CellMesh = GetOrBuildEnvCellMesh(Queue[Qi], WorldScale);
			if (!CellMesh)
			{
				++MissingEnvCells;
				continue;
			}
			// Full indoor connectivity via CellPortals (VisibleCells alone misses back rooms).
			for (const FACEDatCellPortal& Portal : CellMesh->CellPortals)
			{
				EnqueueIndoor(Portal.OtherCellId);
			}
			for (uint16 Vc : CellMesh->VisibleCells)
			{
				EnqueueIndoor(Vc);
			}
		}

		const FVector BldLoc = FACEPosition::AceVectorToUnreal(
			FVector(Building.Origin.X, Building.Origin.Y, Building.Origin.Z), WorldScale);

		// Outdoor grade at the building — floors at/below this punch; upper stories do not.
		float GradeZ = BldLoc.Z;
		{
			float SampledGrade = GradeZ;
			if (SampleTerrainZAt(BldLoc.X, BldLoc.Y, SampledGrade))
			{
				GradeZ = SampledGrade;
			}
		}

		// Ground-band Setup hull from walkable faces only (not wall/roof verts — those
		// inflated the convex hull into plaza gaps between adjacent buildings).
		constexpr float GroundBandHalfCm = 180.f;
		constexpr float ShellPadCm = 0.f;
		TArray<FVector> SetupWorldVerts;
		TArray<TTuple<FVector, FVector, FVector>> SetupUpFaces;
		if (Building.ModelId != 0)
		{
			const FACEBuiltSetupMesh* Setup = GetOrBuildSetupMesh(Building.ModelId, WorldScale);
			if (Setup && !Setup->IsEmpty())
			{
				const FQuat BldQuat = FACEPosition::AceQuatToUnreal(
					Building.Orientation.W,
					FVector(Building.Orientation.X, Building.Orientation.Y, Building.Orientation.Z));
				const FTransform BuildingToLb(BldQuat, BldLoc);

				for (const FACEBuiltSetupPart& Part : Setup->Parts)
				{
					const FTransform ToLb = BuildingToLb * Part.BindTransform;
					for (const FACEBuiltMeshSection& Sec : Part.Sections)
					{
						if (Sec.bFullyTransparent)
						{
							continue;
						}
						for (const FVector& V : Sec.Vertices)
						{
							SetupWorldVerts.Add(ToLb.TransformPosition(V));
						}
						for (int32 T = 0; T + 2 < Sec.Triangles.Num(); T += 3)
						{
							const int32 I0 = Sec.Triangles[T];
							const int32 I1 = Sec.Triangles[T + 1];
							const int32 I2 = Sec.Triangles[T + 2];
							if (!Sec.Vertices.IsValidIndex(I0) || !Sec.Vertices.IsValidIndex(I1)
								|| !Sec.Vertices.IsValidIndex(I2))
							{
								continue;
							}
							const FVector W0 = ToLb.TransformPosition(Sec.Vertices[I0]);
							const FVector W1 = ToLb.TransformPosition(Sec.Vertices[I1]);
							const FVector W2 = ToLb.TransformPosition(Sec.Vertices[I2]);
							const FVector FaceN = FVector::CrossProduct(W1 - W0, W2 - W0).GetSafeNormal();
							if (FaceN.Z >= 0.30f)
							{
								SetupUpFaces.Emplace(W0, W1, W2);
							}
						}
					}
				}

				if (SetupWorldVerts.Num() >= 3)
				{
					const float BandCenterZ = BldLoc.Z;
					const float BandMin = BandCenterZ - GroundBandHalfCm;
					const float BandMax = BandCenterZ + GroundBandHalfCm;
					TArray<FVector2D> Points;
					for (const TTuple<FVector, FVector, FVector>& Face : SetupUpFaces)
					{
						const FVector& W0 = Face.Get<0>();
						const FVector& W1 = Face.Get<1>();
						const FVector& W2 = Face.Get<2>();
						const float Z = (W0.Z + W1.Z + W2.Z) / 3.f;
						if (Z >= BandMin && Z <= BandMax)
						{
							Points.Emplace(W0.X, W0.Y);
							Points.Emplace(W1.X, W1.Y);
							Points.Emplace(W2.X, W2.Y);
						}
					}
					if (Points.Num() < 3)
					{
						// Fallback: lowest Setup verts only (still tighter than ±400cm all verts).
						float MinZ = TNumericLimits<float>::Max();
						for (const FVector& W : SetupWorldVerts)
						{
							MinZ = FMath::Min(MinZ, W.Z);
						}
						const float FallbackMax = MinZ + 100.f;
						for (const FVector& W : SetupWorldVerts)
						{
							if (W.Z <= FallbackMax)
							{
								Points.Emplace(W.X, W.Y);
							}
						}
					}

					if (Points.Num() >= 3)
					{
						TArray<int32> HullIdx;
						ConvexHull2D::ComputeConvexHull(Points, HullIdx);
						TArray<FVector2D> Hull;
						FVector2D Centroid = FVector2D::ZeroVector;
						for (int32 Idx : HullIdx)
						{
							if (!Points.IsValidIndex(Idx))
							{
								continue;
							}
							Hull.Add(Points[Idx]);
							Centroid += Points[Idx];
						}
						if (Hull.Num() >= 3)
						{
							Centroid /= static_cast<float>(Hull.Num());
							for (FVector2D& V : Hull)
							{
								FVector2D D = V - Centroid;
								const float Len = D.Size();
								if (Len > KINDA_SMALL_NUMBER && !FMath::IsNearlyZero(ShellPadCm))
								{
									V = Centroid + D * ((Len + ShellPadCm) / Len);
								}
							}
							for (int32 i = 1; i + 1 < Hull.Num(); ++i)
							{
								AppendShellTri(Bld.ShellTris, Hull[0], Hull[i], Hull[i + 1]);
							}
						}
					}
				}
			}
		}

		// Do NOT claim EnvCells via shell AABB touch — convex shell between adjacent
		// buildings pulled plaza cells into flora/collision ignore. Floor tris come only
		// from this building's portal BFS (PortalCellIds → CellPortals / VisibleCells).

		CollectFloorTrisForCells(CellIds, Bld.FloorTris, Bld.CeilingTris, GradeZ);

		// FloorTris = flora skip + outdoor land-collision ignore under interiors.
		// Never CSG-punch LScape (banned). Plaza safety = grade band in CollectFloorTris.

		// Courtyard / open-roof: inset hull for flora skip only.
		// Always build when shell exists (rooms may have FloorTris while the open center does not).
		// Keep ~120cm outdoor ground at doorways for Use approach.
		if (Bld.ShellTris.Num() > 0)
		{
			FVector2D Centroid = FVector2D::ZeroVector;
			int32 Count = 0;
			for (const FInteriorHoleTri& Tri : Bld.ShellTris)
			{
				Centroid += Tri.A + Tri.B + Tri.C;
				Count += 3;
			}
			if (Count > 0)
			{
				Centroid /= static_cast<float>(Count);
				constexpr float DoorwayKeepCm = 120.f;
				auto Inset = [&](FVector2D V) -> FVector2D
				{
					FVector2D D = Centroid - V;
					const float Len = D.Size();
					if (Len > DoorwayKeepCm + KINDA_SMALL_NUMBER)
					{
						return V + D * (DoorwayKeepCm / Len);
					}
					return Centroid;
				};
				for (const FInteriorHoleTri& Tri : Bld.ShellTris)
				{
					const FVector2D A = Inset(Tri.A);
					const FVector2D B = Inset(Tri.B);
					const FVector2D C = Inset(Tri.C);
					const float Area2 = FMath::Abs((B.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (B.Y - A.Y));
					if (Area2 < 100.f)
					{
						continue;
					}
					AppendFloorTri(Bld.InsetShellTris, A, B, C, GradeZ);
				}
			}
		}

		// Keep cell→building mapping even with no punch geometry.
		Bld.IndoorCellIds = MoveTemp(CellIds);
		if (Bld.FloorTris.Num() == 0 && Bld.InsetShellTris.Num() == 0 && Bld.ShellTris.Num() == 0
			&& Bld.IndoorCellIds.Num() == 0)
		{
			continue;
		}

		TotalFloor += Bld.FloorTris.Num();
		TotalShell += Bld.ShellTris.Num();
		Out.Buildings.Add(MoveTemp(Bld));
	}

	// Buildings with portals but missing EnvCell meshes → rebuild after they arrive.
	Out.bComplete = (MissingEnvCells == 0);

	UE_LOG(LogTemp, Log,
		TEXT("ACEDat: Landblock 0x%08X terrain holes — %d buildings, %d floor tris, %d shell tris (complete=%d missingEnv=%d)"),
		Key, Out.Buildings.Num(), TotalFloor, TotalShell, Out.bComplete ? 1 : 0, MissingEnvCells);
	return Out;
}

void UACEDatSubsystem::InvalidateBuildingInteriorFootprints(uint32 LandblockId)
{
	BuildingInteriorFootprints.Remove(LandblockId & 0xFFFF0000u);
}

bool UACEDatSubsystem::PointInTriList(const FVector2D& Pt, const TArray<FInteriorHoleTri>& Tris)
{
	for (const FInteriorHoleTri& Tri : Tris)
	{
		if (PointInSingleTri(Pt, Tri))
		{
			return true;
		}
	}
	return false;
}

bool UACEDatSubsystem::PointInSingleTri(const FVector2D& Pt, const FInteriorHoleTri& Tri)
{
	// Barycentric with a small epsilon so shared land/floor edges don't flicker in/out.
	const FVector2D V0 = Tri.C - Tri.A;
	const FVector2D V1 = Tri.B - Tri.A;
	const FVector2D V2 = Pt - Tri.A;
	const float Dot00 = FVector2D::DotProduct(V0, V0);
	const float Dot01 = FVector2D::DotProduct(V0, V1);
	const float Dot02 = FVector2D::DotProduct(V0, V2);
	const float Dot11 = FVector2D::DotProduct(V1, V1);
	const float Dot12 = FVector2D::DotProduct(V1, V2);
	const float Denom = Dot00 * Dot11 - Dot01 * Dot01;
	if (FMath::Abs(Denom) < KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const float Inv = 1.f / Denom;
	const float U = (Dot11 * Dot02 - Dot01 * Dot12) * Inv;
	const float V = (Dot00 * Dot12 - Dot01 * Dot02) * Inv;
	constexpr float Eps = 1.e-3f;
	return (U >= -Eps) && (V >= -Eps) && (U + V <= 1.f + Eps);
}

bool UACEDatSubsystem::PointInInteriorHole(const FVector2D& Pt, float TerrainZ, const FBuildingInteriorMask& Mask)
{
	// Flora / land-collision ignore under EnvCell floors. Upper-story floors must not
	// suppress outdoor roofs. Never used to CSG-punch LScape.
	constexpr float AboveFloorSlackCm = 100.f;
	for (const FBuildingHoleMask& Bld : Mask.Buildings)
	{
		for (const FInteriorHoleTri& Tri : Bld.FloorTris)
		{
			if (Tri.Z > TerrainZ + AboveFloorSlackCm)
			{
				continue;
			}
			if (PointInSingleTri(Pt, Tri))
			{
				return true;
			}
		}
	}
	return false;
}

bool UACEDatSubsystem::IsUnderBuildingInterior(uint32 LandblockId, float LocalUnrealX, float LocalUnrealY, float WorldScale)
{
	const FBuildingInteriorMask& Mask = GetOrBuildBuildingInteriorFootprints(LandblockId, WorldScale);
	const FVector2D Pt(LocalUnrealX, LocalUnrealY);
	// Retail: land stays in the outdoor collision set; interiors use EnvCell PhysicsBSP.
	// Only suppress outdoor land under confirmed EnvCell floors (basement mouths / shop
	// decks). Full Setup ShellTris hull previously ignored plazas/courtyards where building
	// PhysicsBSP was incomplete → fall through visible grass.
	for (const FBuildingHoleMask& Bld : Mask.Buildings)
	{
		if (Bld.FloorTris.Num() > 0 && PointInTriList(Pt, Bld.FloorTris))
		{
			return true;
		}
	}
	return false;
}

bool UACEDatSubsystem::IsWorldXYUnderBuildingInterior(float UnrealX, float UnrealY, float WorldScale)
{
	if (WorldScale <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const float GlobalX = -UnrealX / WorldScale;
	const float GlobalY = UnrealY / WorldScale;
	const int32 Lbx = FMath::FloorToInt(GlobalX / FACELandblockMeshBuilder::LandblockSize);
	const int32 Lby = FMath::FloorToInt(GlobalY / FACELandblockMeshBuilder::LandblockSize);
	if (Lbx < 0 || Lbx > 255 || Lby < 0 || Lby > 255)
	{
		return false;
	}
	const uint32 LandblockId = (static_cast<uint32>(Lbx) << 24) | (static_cast<uint32>(Lby) << 16);
	const float Lx = GlobalX - static_cast<float>(Lbx) * FACELandblockMeshBuilder::LandblockSize;
	const float Ly = GlobalY - static_cast<float>(Lby) * FACELandblockMeshBuilder::LandblockSize;
	const FVector LocalUe = FACEPosition::AceVectorToUnreal(FVector(Lx, Ly, 0.f), WorldScale);
	return IsUnderBuildingInterior(LandblockId, LocalUe.X, LocalUe.Y, WorldScale);
}

int32 UACEDatSubsystem::FindBuildingInfoIndexForIndoorCell(uint32 LandblockId, uint32 CellId, float WorldScale)
{
	if (CellId == 0 || (CellId & 0x0000FFFFu) < 0x0100u)
	{
		return INDEX_NONE;
	}
	const uint16 ShortId = static_cast<uint16>(CellId & 0xFFFFu);
	const uint32 Key = LandblockId & 0xFFFF0000u;

	// Prefer LandblockInfo portals/stabs — does not depend on EnvCell mesh / footprint cache.
	if (const auto Snapshot = GetLandblockInfo(Key))
	{
		const auto& Info = *Snapshot;
		for (int32 Bi = 0; Bi < Info.Buildings.Num(); ++Bi)
		{
			const FACEDatLandblockBuilding& Building = Info.Buildings[Bi];
			for (uint16 PortalCell : Building.PortalCellIds)
			{
				if (PortalCell == ShortId)
				{
					return Bi;
				}
			}
			for (const FACEDatBuildingPortal& Portal : Building.Portals)
			{
				if (Portal.OtherCellId == ShortId)
				{
					return Bi;
				}
				for (uint16 Stab : Portal.StabCells)
				{
					if (Stab == ShortId)
					{
						return Bi;
					}
				}
			}
		}
	}

	const FBuildingInteriorMask& Mask = GetOrBuildBuildingInteriorFootprints(LandblockId, WorldScale);
	for (const FBuildingHoleMask& Bld : Mask.Buildings)
	{
		if (Bld.IndoorCellIds.Contains(CellId))
		{
			return Bld.LandblockBuildingIndex;
		}
	}
	return INDEX_NONE;
}

void UACEDatSubsystem::CollectBuildingShellHideIndices(uint32 LandblockId, uint32 PlayerCellId,
	const TSet<int32>& VisibleIndoorCellIds, float WorldScale, TArray<int32>& OutHideIndices)
{
	OutHideIndices.Reset();
	if ((PlayerCellId & 0xFFFFu) < 0x0100u)
	{
		return;
	}
	const uint32 Key = LandblockId & 0xFFFF0000u;
	const int32 Primary = FindBuildingInfoIndexForIndoorCell(Key, PlayerCellId, WorldScale);
	if (Primary != INDEX_NONE)
	{
		OutHideIndices.AddUnique(Primary);
	}
	// Do not fall back to every Setup shell on the landblock — that popped all of
	// Yaraq when the occupied cell was missing from a portal list.
	(void)VisibleIndoorCellIds;
}

void UACEDatSubsystem::CollectNearbyOutdoorBuildingShellHideIndices(uint32 LandblockId, const FVector& PlayerWorld,
	float WorldScale, TArray<int32>& OutHideIndices)
{
	if (WorldScale <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	const uint32 Key = LandblockId & 0xFFFF0000u;
	const int32 Lbx = static_cast<int32>((Key >> 24) & 0xFF);
	const int32 Lby = static_cast<int32>((Key >> 16) & 0xFF);
	const FVector Origin = FACEPosition::AceVectorToUnreal(
		FVector(Lbx * FACELandblockMeshBuilder::LandblockSize,
			Lby * FACELandblockMeshBuilder::LandblockSize, 0.f),
		WorldScale);
	const FVector2D Local(PlayerWorld.X - Origin.X, PlayerWorld.Y - Origin.Y);
	const FBuildingInteriorMask& Mask = GetOrBuildBuildingInteriorFootprints(Key, WorldScale);
	// Only hide a shell when the player is on that building's floor/shell, not when
	// walking 8 m down the street (that flickered every shop in Yaraq).
	for (const FBuildingHoleMask& Bld : Mask.Buildings)
	{
		if (Bld.LandblockBuildingIndex == INDEX_NONE)
		{
			continue;
		}
		if (PointInTriList(Local, Bld.FloorTris) || PointInTriList(Local, Bld.ShellTris))
		{
			OutHideIndices.AddUnique(Bld.LandblockBuildingIndex);
		}
	}
}

bool UACEDatSubsystem::TryGetBuildingLandClipBox(uint32 LandblockId, uint32 PlayerCellId, const FVector& PlayerWorld,
	float WorldScale, FVector& OutMin, FVector& OutMax)
{
	if (WorldScale <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const uint32 Key = LandblockId & 0xFFFF0000u;
	const int32 IndoorBi = ((PlayerCellId & 0xFFFFu) >= 0x0100u)
		? FindBuildingInfoIndexForIndoorCell(Key, PlayerCellId, WorldScale)
		: INDEX_NONE;
	const FBuildingInteriorMask& Mask = GetOrBuildBuildingInteriorFootprints(Key, WorldScale);
	if (Mask.Buildings.Num() == 0)
	{
		return false;
	}
	const int32 Lbx = static_cast<int32>((Key >> 24) & 0xFF);
	const int32 Lby = static_cast<int32>((Key >> 16) & 0xFF);
	const FVector Origin = FACEPosition::AceVectorToUnreal(
		FVector(Lbx * FACELandblockMeshBuilder::LandblockSize,
			Lby * FACELandblockMeshBuilder::LandblockSize, 0.f),
		WorldScale);
	const FVector2D Local(PlayerWorld.X - Origin.X, PlayerWorld.Y - Origin.Y);

	const FBuildingHoleMask* Found = nullptr;
	if (IndoorBi != INDEX_NONE)
	{
		for (const FBuildingHoleMask& Bld : Mask.Buildings)
		{
			if (Bld.LandblockBuildingIndex == IndoorBi)
			{
				Found = &Bld;
				break;
			}
		}
	}
	if (!Found)
	{
		for (const FBuildingHoleMask& Bld : Mask.Buildings)
		{
			if (PointInTriList(Local, Bld.FloorTris) || PointInTriList(Local, Bld.ShellTris)
				|| PointInTriList(Local, Bld.InsetShellTris))
			{
				Found = &Bld;
				break;
			}
		}
	}
	if (!Found)
	{
		return false;
	}

	FBox Box(ForceInit);
	auto AddTri = [&](const TArray<FInteriorHoleTri>& Tris)
	{
		for (const FInteriorHoleTri& T : Tris)
		{
			Box += FVector(Origin.X + T.A.X, Origin.Y + T.A.Y, Origin.Z + T.Z);
			Box += FVector(Origin.X + T.B.X, Origin.Y + T.B.Y, Origin.Z + T.Z);
			Box += FVector(Origin.X + T.C.X, Origin.Y + T.C.Y, Origin.Z + T.Z);
		}
	};
	AddTri(Found->FloorTris);
	if (Box.IsValid == 0)
	{
		AddTri(Found->InsetShellTris);
	}
	if (Box.IsValid == 0)
	{
		AddTri(Found->ShellTris);
	}
	if (Box.IsValid == 0)
	{
		for (uint32 CellId : Found->IndoorCellIds)
		{
			const FACEBuiltEnvCellMesh* Mesh = FindEnvCellMesh(CellId, WorldScale);
			if (!Mesh || !Mesh->bHasLocalBounds)
			{
				continue;
			}
			const FTransform CellToWorld = Mesh->GetCellLocalToLandblock(WorldScale)
				* FTransform(FQuat::Identity, Origin);
			Box += FBox(Mesh->LocalBoundsMin, Mesh->LocalBoundsMax).TransformBy(CellToWorld);
		}
	}
	if (Box.IsValid == 0)
	{
		return false;
	}
	OutMin = Box.Min;
	OutMax = Box.Max;
	return true;
}

namespace
{
	void AcePrepareLandSectionLighting(TArray<FVector>& Norms, TArray<int32>& Tris, TArray<FProcMeshTangent>& OutTangents)
	{
		FVector Sum = FVector::ZeroVector;
		for (const FVector& N : Norms)
		{
			Sum += N;
		}
		// DAT land winding already faces the camera (top of LScape). Vertex normals often
		// point down, which made DefaultLit N·L ≤ 0 (black ground). Flip normals only —
		// reversing triangles made front faces point down so land only drew from below.
		if (Sum.Z < 0.f)
		{
			for (FVector& N : Norms)
			{
				N = -N;
			}
		}
		(void)Tris;
		OutTangents.SetNum(Norms.Num());
		for (int32 i = 0; i < Norms.Num(); ++i)
		{
			const FVector N = Norms[i].GetSafeNormal();
			FVector T = FVector::CrossProduct(N, FVector(0.f, 0.f, 1.f));
			if (T.SizeSquared() < 0.05f)
			{
				T = FVector::CrossProduct(N, FVector(1.f, 0.f, 0.f));
			}
			OutTangents[i] = FProcMeshTangent(T.GetSafeNormal(), false);
		}
	}
}

bool UACEDatSubsystem::ApplyLandblockToProceduralMesh(UProceduralMeshComponent* ProcMesh, int32 LandblockId, float WorldScale, int32 PolySize, int32 TransDir)
{
	if (!IsValid(ProcMesh))
	{
		return false;
	}
	// Pin SharedPtr for the whole apply — Evict / FindOrAdd must not free section arrays mid-loop.
	TSharedPtr<FACEBuiltLandblockMesh> Pinned;
	{
		const uint32 Key = static_cast<uint32>(LandblockId) & 0xFFFF0000u;
		if (const TSharedPtr<FACEBuiltLandblockMesh>* Found = LandblockCache.Find(Key))
		{
			Pinned = *Found;
		}
	}
	if (!Pinned.IsValid() || !Pinned->bHasHeights || !Pinned->bHasTerrain || Pinned->IsEmpty())
	{
		RequestLandblockMesh(static_cast<uint32>(LandblockId), WorldScale);
		return false;
	}
	const FACEBuiltLandblockMesh& Mesh = *Pinned;
	// A complete block is only 128 triangles. Preserve its authored elevations
	// and diagonals at every distance: buildings and collision use those heights.
	// Retail's coarse rings visibly sink/float our independently streamed buildings.
	// Reuse the cached sections and textures instead of synchronously rebaking
	// them at ring changes. Texture mipmaps still reduce distant detail.

	ProcMesh->ClearAllMeshSections();
	// Large heightfield BodySetup off the game thread. Scenery/particle PMC cooks stay
	// sync (UE 5.8 ThreadPool FAppTime ensures).
	ProcMesh->bUseAsyncCooking = true;
	ProcMesh->bUseComplexAsSimpleCollision = true;

	int32 SectionIndex = 0;
	for (const FACEBuiltLandblockSection& Sec : Mesh.Sections)
	{
		if (Sec.IsEmpty() || Sec.Triangles.GetData() == nullptr || Sec.Vertices.GetData() == nullptr)
		{
			continue;
		}

		UMaterialInterface* Mat = GetVertexColorMaterial();
		if (Sec.bGpuTexMerge)
		{
			if (UMaterialInterface* GpuMat = GetOrCreateLandGpuMaterial())
			{
				Mat = GpuMat;
			}
			else
			{
				continue;
			}
		}
		else if (Sec.HasBakedTexture())
		{
			if (UMaterialInterface* LandMat = GetOrCreateLandMaterial(Sec.PCode, Sec.BakedPixels, Sec.BakeWidth, Sec.BakeHeight, Sec.SharedBake))
			{
				Mat = LandMat;
			}
		}
		if (!Mat)
		{
			continue;
		}

		TArray<FVector> KeepVerts;
		TArray<int32> KeepTris;
		TArray<FVector> KeepNorms;
		TArray<FVector2D> KeepUVs;
		TArray<FVector2D> KeepUV1, KeepUV2, KeepUV3;
		TArray<FLinearColor> KeepColors;
		TMap<int32, int32> Remap;

		auto EmitVert = [&](int32 SrcIdx) -> int32
		{
			if (const int32* Found = Remap.Find(SrcIdx))
			{
				return *Found;
			}
			const int32 Dst = KeepVerts.Num();
			KeepVerts.Add(Sec.Vertices[SrcIdx]);
			if (Sec.Normals.IsValidIndex(SrcIdx))
			{
				KeepNorms.Add(Sec.Normals[SrcIdx]);
			}
			else
			{
				KeepNorms.Add(FVector::UpVector);
			}
			if (Sec.UVs.IsValidIndex(SrcIdx))
			{
				KeepUVs.Add(Sec.UVs[SrcIdx]);
			}
			else
			{
				KeepUVs.Add(FVector2D::ZeroVector);
			}
			if (Sec.bGpuTexMerge && Sec.UV1.Num() == Sec.Vertices.Num())
			{
				KeepUV1.Add(Sec.UV1[SrcIdx]);
				KeepUV2.Add(Sec.UV2.IsValidIndex(SrcIdx) ? Sec.UV2[SrcIdx] : FVector2D::ZeroVector);
				KeepUV3.Add(Sec.UV3.IsValidIndex(SrcIdx) ? Sec.UV3[SrcIdx] : FVector2D::ZeroVector);
			}
			if (Sec.VertexColors.IsValidIndex(SrcIdx))
			{
				KeepColors.Add(Sec.VertexColors[SrcIdx]);
			}
			else
			{
				KeepColors.Add(FLinearColor::White);
			}
			Remap.Add(SrcIdx, Dst);
			return Dst;
		};

		for (int32 Ti = 0; Ti + 2 < Sec.Triangles.Num(); Ti += 3)
		{
			const int32 I0 = Sec.Triangles[Ti];
			const int32 I1 = Sec.Triangles[Ti + 1];
			const int32 I2 = Sec.Triangles[Ti + 2];
			if (!Sec.Vertices.IsValidIndex(I0) || !Sec.Vertices.IsValidIndex(I1)
				|| !Sec.Vertices.IsValidIndex(I2))
			{
				continue;
			}
			KeepTris.Add(EmitVert(I0));
			KeepTris.Add(EmitVert(I1));
			KeepTris.Add(EmitVert(I2));
		}

		if (KeepTris.Num() < 3 || !IsValid(ProcMesh))
		{
			continue;
		}

		TArray<FProcMeshTangent> KeepTangents;
		AcePrepareLandSectionLighting(KeepNorms, KeepTris, KeepTangents);

		// Full heightfield — do not omit building-footprint tris (that punched streets into voids).
		if (Sec.bGpuTexMerge && KeepUV1.Num() == KeepVerts.Num())
		{
			ProcMesh->CreateMeshSection_LinearColor(
				SectionIndex, KeepVerts, KeepTris, KeepNorms, KeepUVs, KeepUV1, KeepUV2, KeepUV3, KeepColors,
				KeepTangents, /*bCreateCollision*/ true);
		}
		else
		{
			ProcMesh->CreateMeshSection_LinearColor(
				SectionIndex, KeepVerts, KeepTris, KeepNorms, KeepUVs, KeepColors,
				KeepTangents, /*bCreateCollision*/ true);
		}
		ProcMesh->SetMaterial(SectionIndex, Mat);
		++SectionIndex;
	}

	if (!IsValid(ProcMesh))
	{
		return false;
	}
	ProcMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	ProcMesh->SetCollisionObjectType(ECC_WorldStatic);
	ProcMesh->SetCollisionResponseToAllChannels(ECR_Block);
	ProcMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	// Mouse pick uses ECC_Visibility on weenie capsules only — land must not occlude
	// underwater corpses / fish holes (re-apply used to reset this to Block).
	ProcMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	ProcMesh->SetCanEverAffectNavigation(false);
	ProcMesh->bUseComplexAsSimpleCollision = true;
	// Movable so landblocks can be relocated. Do not cast (CSM resolution).
	ProcMesh->SetMobility(EComponentMobility::Movable);
	ProcMesh->SetCastShadow(false);
	ProcMesh->bCastDynamicShadow = false;
	ProcMesh->bReceivesDecals = true;
	// Async cook completes on a worker — RecreatePhysicsState() would force sync.
	return SectionIndex > 0;
}

bool UACEDatSubsystem::ApplyLandblockChunkToProceduralMesh(UProceduralMeshComponent* ProcMesh,
	const TArray<const FACEBuiltLandblockMesh*>& Meshes, int32 ChunkOriginLandblockId, float WorldScale)
{
	if (!IsValid(ProcMesh) || Meshes.Num() == 0)
	{
		return false;
	}

	// Own copies — Mesh* args point into LandblockCache and can UAF if the map rehashes.
	TArray<FACEBuiltLandblockMesh> Owned;
	Owned.Reserve(Meshes.Num());
	for (const FACEBuiltLandblockMesh* Src : Meshes)
	{
		if (Src && !Src->IsEmpty())
		{
			Owned.Add(*Src);
		}
	}
	if (Owned.Num() == 0)
	{
		return false;
	}

	const int32 Ox = (ChunkOriginLandblockId >> 24) & 0xFF;
	const int32 Oy = (ChunkOriginLandblockId >> 16) & 0xFF;
	const FVector ChunkOriginAc(Ox * FACELandblockMeshBuilder::LandblockSize, Oy * FACELandblockMeshBuilder::LandblockSize, 0.f);

	ProcMesh->ClearAllMeshSections();
	ProcMesh->bUseAsyncCooking = true;
	ProcMesh->bUseComplexAsSimpleCollision = true;

	// Full heightfield draw + collision (no footprint omit — hull punch opened street voids).
	TArray<FVector> GpuVerts, GpuNorms;
	TArray<FVector2D> GpuUV0, GpuUV1, GpuUV2, GpuUV3;
	TArray<FLinearColor> GpuColors;
	TArray<int32> GpuTris;
	bool bAnyGpu = false;

	int32 SectionIndex = 0;
	for (const FACEBuiltLandblockMesh& Mesh : Owned)
	{
		const int32 Lbx = (Mesh.LandblockId >> 24) & 0xFF;
		const int32 Lby = (Mesh.LandblockId >> 16) & 0xFF;
		const FVector LbOriginUnreal = FACEPosition::AceVectorToUnreal(
			FVector(Lbx * FACELandblockMeshBuilder::LandblockSize, Lby * FACELandblockMeshBuilder::LandblockSize, 0.f) - ChunkOriginAc,
			WorldScale);

		for (const FACEBuiltLandblockSection& Sec : Mesh.Sections)
		{
			if (Sec.IsEmpty())
			{
				continue;
			}

			if (Sec.bGpuTexMerge && Sec.UV1.Num() == Sec.Vertices.Num())
			{
				if (!GetOrCreateLandGpuMaterial())
				{
					continue;
				}
				bAnyGpu = true;
				TMap<int32, int32> Remap;
				auto Emit = [&](int32 SrcIdx) -> int32
				{
					if (const int32* Found = Remap.Find(SrcIdx))
					{
						return *Found;
					}
					const int32 Dst = GpuVerts.Num();
					GpuVerts.Add(Sec.Vertices[SrcIdx] + LbOriginUnreal);
					GpuNorms.Add(Sec.Normals.IsValidIndex(SrcIdx) ? Sec.Normals[SrcIdx] : FVector::UpVector);
					GpuUV0.Add(Sec.UVs.IsValidIndex(SrcIdx) ? Sec.UVs[SrcIdx] : FVector2D::ZeroVector);
					GpuUV1.Add(Sec.UV1[SrcIdx]);
					GpuUV2.Add(Sec.UV2.IsValidIndex(SrcIdx) ? Sec.UV2[SrcIdx] : FVector2D::ZeroVector);
					GpuUV3.Add(Sec.UV3.IsValidIndex(SrcIdx) ? Sec.UV3[SrcIdx] : FVector2D::ZeroVector);
					GpuColors.Add(Sec.VertexColors.IsValidIndex(SrcIdx) ? Sec.VertexColors[SrcIdx] : FLinearColor::White);
					Remap.Add(SrcIdx, Dst);
					return Dst;
				};
				for (int32 Ti = 0; Ti + 2 < Sec.Triangles.Num(); Ti += 3)
				{
					const int32 I0 = Sec.Triangles[Ti];
					const int32 I1 = Sec.Triangles[Ti + 1];
					const int32 I2 = Sec.Triangles[Ti + 2];
					if (!Sec.Vertices.IsValidIndex(I0) || !Sec.Vertices.IsValidIndex(I1)
						|| !Sec.Vertices.IsValidIndex(I2))
					{
						continue;
					}
					GpuTris.Add(Emit(I0));
					GpuTris.Add(Emit(I1));
					GpuTris.Add(Emit(I2));
				}
				continue;
			}

			TArray<FVector> Verts;
			TArray<int32> Tris;
			TArray<FVector> Norms;
			TArray<FVector2D> UVs;
			TArray<FLinearColor> Colors;
			TMap<int32, int32> Remap;
			auto Emit = [&](int32 SrcIdx) -> int32
			{
				if (const int32* Found = Remap.Find(SrcIdx))
				{
					return *Found;
				}
				const int32 Dst = Verts.Num();
				Verts.Add(Sec.Vertices[SrcIdx] + LbOriginUnreal);
				Norms.Add(Sec.Normals.IsValidIndex(SrcIdx) ? Sec.Normals[SrcIdx] : FVector::UpVector);
				UVs.Add(Sec.UVs.IsValidIndex(SrcIdx) ? Sec.UVs[SrcIdx] : FVector2D::ZeroVector);
				Colors.Add(Sec.VertexColors.IsValidIndex(SrcIdx) ? Sec.VertexColors[SrcIdx] : FLinearColor::White);
				Remap.Add(SrcIdx, Dst);
				return Dst;
			};
			for (int32 Ti = 0; Ti + 2 < Sec.Triangles.Num(); Ti += 3)
			{
				const int32 I0 = Sec.Triangles[Ti];
				const int32 I1 = Sec.Triangles[Ti + 1];
				const int32 I2 = Sec.Triangles[Ti + 2];
				if (!Sec.Vertices.IsValidIndex(I0) || !Sec.Vertices.IsValidIndex(I1)
					|| !Sec.Vertices.IsValidIndex(I2))
				{
					continue;
				}
				Tris.Add(Emit(I0));
				Tris.Add(Emit(I1));
				Tris.Add(Emit(I2));
			}
			if (Tris.Num() < 3 || !IsValid(ProcMesh))
			{
				continue;
			}
			UMaterialInterface* Mat = GetVertexColorMaterial();
			if (Sec.HasBakedTexture())
			{
				if (UMaterialInterface* LandMat = GetOrCreateLandMaterial(Sec.PCode, Sec.BakedPixels, Sec.BakeWidth, Sec.BakeHeight, Sec.SharedBake))
				{
					Mat = LandMat;
				}
			}
			if (!Mat)
			{
				continue;
			}
			TArray<FProcMeshTangent> Tangents;
			AcePrepareLandSectionLighting(Norms, Tris, Tangents);
			ProcMesh->CreateMeshSection_LinearColor(
				SectionIndex, Verts, Tris, Norms, UVs, Colors,
				Tangents, /*bCreateCollision*/ true);
			ProcMesh->SetMaterial(SectionIndex, Mat);
			++SectionIndex;
		}
	}

	if (bAnyGpu && GpuVerts.Num() > 0 && IsValid(ProcMesh))
	{
		UMaterialInterface* Mat = GetOrCreateLandGpuMaterial();
		if (!Mat)
		{
			Mat = GetVertexColorMaterial();
		}
		if (Mat)
		{
			TArray<FProcMeshTangent> GpuTangents;
			AcePrepareLandSectionLighting(GpuNorms, GpuTris, GpuTangents);
			ProcMesh->CreateMeshSection_LinearColor(
				SectionIndex, GpuVerts, GpuTris, GpuNorms, GpuUV0, GpuUV1, GpuUV2, GpuUV3, GpuColors,
				GpuTangents, /*bCreateCollision*/ true);
			ProcMesh->SetMaterial(SectionIndex, Mat);
			++SectionIndex;
		}
	}

	ProcMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	ProcMesh->SetCollisionObjectType(ECC_WorldStatic);
	ProcMesh->SetCollisionResponseToAllChannels(ECR_Block);
	ProcMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	ProcMesh->SetCanEverAffectNavigation(false);
	ProcMesh->bUseComplexAsSimpleCollision = true;
	ProcMesh->SetMobility(EComponentMobility::Movable);
	ProcMesh->SetCastShadow(false);
	ProcMesh->bCastDynamicShadow = false;
	ProcMesh->bReceivesDecals = true;
	// Async cook — do not RecreatePhysicsState (would sync on GT).
	return SectionIndex > 0;
}

void UACEDatSubsystem::SetWorldEmissiveScale(float LandScale, float SceneryScale, const FLinearColor& AmbientTint)
{
	const float NewLand = FMath::Clamp(LandScale, 0.04f, 1.5f);
	const float NewScenery = SceneryScale < 0.f
		? FMath::Lerp(0.20f, 0.14f, FMath::Clamp((NewLand - 0.06f) / 0.10f, 0.f, 1.f))
		: FMath::Clamp(SceneryScale, 0.10f, 1.5f);
	if (FMath::IsNearlyEqual(WorldEmissiveScale, NewLand, 0.008f)
		&& FMath::IsNearlyEqual(SceneryEmissiveScale, NewScenery, 0.008f)
		&& WorldAmbientTint.Equals(AmbientTint,0.008f))
	{
		return;
	}
	WorldEmissiveScale = NewLand;
	SceneryEmissiveScale = NewScenery;
	WorldAmbientTint = AmbientTint;
	for (auto It=WorldLightingInstances.CreateIterator(); It; ++It)
	{
		if (auto* Material=It.Key().Get())
		{
			if (It.Value() != EObjectLightEnvironment::Outdoors) continue;
			Material->SetScalarParameterValue(TEXT("EmissiveStrength"), SceneryEmissiveScale);
			Material->SetVectorParameterValue(TEXT("WorldAmbientTint"), WorldAmbientTint);
		}
		else It.RemoveCurrent();
	}
	for (auto& Pair : WorldObjectMaterials)
	{
		if (auto* Mid = Pair.Value.Get())
		{
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), SceneryEmissiveScale);
			Mid->SetVectorParameterValue(TEXT("WorldAmbientTint"), WorldAmbientTint);
		}
	}
	for (auto It = LandSkyInstances.CreateIterator(); It; ++It)
	{
		if (auto* Mid = It->Get())
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"), WorldEmissiveScale);
		else
			It.RemoveCurrent();
	}
	if (LandGpuMaterialInstance)
	{
		LandGpuMaterialInstance->SetScalarParameterValue(TEXT("EmissiveStrength"), WorldEmissiveScale);
	}
	// Weak identities keep visible materials responsive to daylight after their
	// warm-cache reference is evicted; cache lifetime must not change lighting.
	for (const auto& Pair : SurfaceMaterials)
	{
		const uint32 Kind=uint32(Pair.Key>>32);
		if (Kind>2) continue;
		if (auto* Mid=Pair.Value.Material.Get())
		{
			Mid->SetScalarParameterValue(TEXT("EmissiveStrength"),Kind==0?1.f:SceneryEmissiveScale);
			if (Kind!=0) Mid->SetVectorParameterValue(TEXT("WorldAmbientTint"),WorldAmbientTint);
		}
	}
}

void UACEDatSubsystem::ApplyDistanceFogToMid(UMaterialInstanceDynamic* Mid) const
{
	ApplyDistanceFogToMaterialInstance(Mid);
}

void UACEDatSubsystem::ApplyDistanceFogToMaterial(UMaterialInterface* Inst) const
{
	ApplyDistanceFogToMaterialInstance(Inst);
}

void UACEDatSubsystem::ApplyDistanceFogToMaterialInstance(UMaterialInterface* Inst) const
{
	if (!Inst)
	{
		return;
	}
	UMaterialParameterCollection* Collection = nullptr;
	const bool bSharedFog = Inst->GetParameterCollectionParameterValue(TEXT("FogStart"), Collection)
		&& Collection == GetRuntimeFogCollection();
#if WITH_EDITOR
	if (UMaterialInstanceConstant* Mic = Cast<UMaterialInstanceConstant>(Inst))
	{
		if (bSharedFog)
		{
			Mic->SetScalarParameterValueEditorOnly(TEXT("FogAmount"), 1.f);
			return;
		}
		Mic->SetScalarParameterValueEditorOnly(TEXT("FogStart"), WorldFogStartCm);
		Mic->SetScalarParameterValueEditorOnly(TEXT("FogEnd"), WorldFogEndCm);
		Mic->SetScalarParameterValueEditorOnly(TEXT("FogAmount"), WorldFogAmount);
		Mic->SetVectorParameterValueEditorOnly(TEXT("FogColor"), WorldFogColor);
		return;
	}
#endif
	if (UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Inst))
	{
		if (bSharedFog)
		{
			Mid->SetScalarParameterValue(TEXT("FogAmount"), 1.f);
			return;
		}
		Mid->SetScalarParameterValue(TEXT("FogStart"), WorldFogStartCm);
		Mid->SetScalarParameterValue(TEXT("FogEnd"), WorldFogEndCm);
		Mid->SetScalarParameterValue(TEXT("FogAmount"), WorldFogAmount);
		Mid->SetVectorParameterValue(TEXT("FogColor"), WorldFogColor);
	}
}

void UACEDatSubsystem::ApplyWorldDistanceFogToLandscapes() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	static const FName OutdoorTerrainTag(TEXT("ACEOutdoorTerrain"));
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Landscape = *It;
		if (!Landscape || !Landscape->ActorHasTag(OutdoorTerrainTag))
		{
			continue;
		}
		TArray<ULandscapeComponent*> Components;
		Landscape->GetComponents(Components);
		if (World->IsGameWorld() && !Landscape->bUseDynamicMaterialInstance)
		{
			// Cooked constant instances are immutable. Preserve live weather/fog on
			// optional baked landscapes with the same MID path as DAT terrain.
			Landscape->bUseDynamicMaterialInstance = true;
			for (ULandscapeComponent* Component : Components)
			{
				if (!Component) continue;
				Component->MaterialInstancesDynamic.Reset();
				for (int32 Index = 0; Index < Component->GetMaterialInstanceCount(false); ++Index)
					Component->MaterialInstancesDynamic.Add(UMaterialInstanceDynamic::Create(Component->GetMaterialInstance(Index, false), Component));
				Component->MarkRenderStateDirty();
			}
		}
		for (ULandscapeComponent* LC : Components)
		{
			if (!LC)
			{
				continue;
			}
			const int32 NumMats = LC->GetMaterialInstanceCount();
			for (int32 MatIdx = 0; MatIdx < NumMats; ++MatIdx)
			{
				if (UMaterialInterface* Mat = LC->GetMaterialInstance(MatIdx))
				{
					ApplyDistanceFogToMaterialInstance(Mat);
				}
			}
		}
	}
}

void UACEDatSubsystem::SetWorldDistanceFog(float StartCm, float EndCm, const FLinearColor& Color, float Amount)
{
	const float NewStart = FMath::Max(0.f, StartCm);
	const float NewEnd = FMath::Max(NewStart + 1.f, EndCm);
	const float NewAmount = FMath::Clamp(Amount, 0.f, 1.f);
	if (FMath::IsNearlyEqual(WorldFogStartCm, NewStart, 8.f)
		&& FMath::IsNearlyEqual(WorldFogEndCm, NewEnd, 8.f)
		&& FMath::IsNearlyEqual(WorldFogAmount, NewAmount, 0.02f)
		&& WorldFogColor.Equals(Color, 0.0002f))
	{
		return;
	}
	WorldFogStartCm = NewStart;
	WorldFogEndCm = NewEnd;
	WorldFogAmount = NewAmount;
	WorldFogColor = Color;
	if (UWorld* World = GetWorld())
	{
		if (auto* Collection = GetRuntimeFogCollection())
		{
			if (auto* Instance = World->GetParameterCollectionInstance(Collection))
			{
				Instance->SetScalarParameterValue(TEXT("FogStart"), NewStart);
				Instance->SetScalarParameterValue(TEXT("FogEnd"), NewEnd);
				Instance->SetScalarParameterValue(TEXT("FogAmount"), NewAmount);
				Instance->SetVectorParameterValue(TEXT("FogColor"), Color);
			}
		}
	}
	// Optional landscapes baked with older shader parents retain their local
	// parameter path. DAT terrain/scenery and actors use the shared collection.
	ApplyWorldDistanceFogToLandscapes();
}

const FACEDatRegionSky* UACEDatSubsystem::GetRegionSkyInfo()
{
	if (!EnsureLoaded() || !PortalDat)
	{
		return nullptr;
	}
	if (!bRegionSkyLoaded)
	{
		TArray<uint8> RegionBlob;
		if (PortalDat->ReadFile(0x13000000u, RegionBlob))
		{
			FACEDatCursor Cur(RegionBlob);
			bRegionSkyLoaded = ACEDatUnpack::UnpackRegionSkyInfo(Cur, RegionSky);
		}
		if (!bRegionSkyLoaded)
		{
			UE_LOG(LogTemp, Warning, TEXT("ACEDat: RegionDesc SkyDesc unavailable"));
			return nullptr;
		}
		UE_LOG(LogTemp, Log, TEXT("ACEDat: SkyDesc loaded — %d day group(s), DayLength=%.0fs ZeroTOY=%.0f ZeroYear=%u DaysPerYear=%u"),
			RegionSky.DayGroups.Num(), RegionSky.DayLengthSeconds, RegionSky.ZeroTimeOfYear, RegionSky.ZeroYear, RegionSky.DaysPerYear);
		for (int32 g = 0; g < RegionSky.DayGroups.Num(); ++g)
		{
			const FACEDatSkyDayGroup& Dg = RegionSky.DayGroups[g];
			UE_LOG(LogTemp, Verbose, TEXT("ACEDat: DayGroup[%d] chance=%.3f name='%s' objs=%d"),
				g, Dg.ChanceOfOccur, *Dg.DayName, Dg.Objects.Num());
		}
	}
	return RegionSky.bValid ? &RegionSky : nullptr;
}

const FACEDatRegionSoundInfo* UACEDatSubsystem::GetRegionSoundInfo()
{
	if (!EnsureLoaded() || !PortalDat)
	{
		return nullptr;
	}
	if (!bRegionSoundLoaded)
	{
		TArray<uint8> RegionBlob;
		if (PortalDat->ReadFile(0x13000000u, RegionBlob))
		{
			FACEDatCursor Cur(RegionBlob);
			bRegionSoundLoaded = ACEDatUnpack::UnpackRegionSoundInfo(Cur, RegionSoundInfo);
		}
		if (!bRegionSoundLoaded)
		{
			UE_LOG(LogTemp, Warning, TEXT("ACEDat: RegionDesc SoundDesc unavailable"));
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("ACEDat: RegionDesc SoundDesc — %d ambient STB entries"),
				RegionSoundInfo.STBDescs.Num());
		}
	}
	return RegionSoundInfo.bValid ? &RegionSoundInfo : nullptr;
}

bool UACEDatSubsystem::TryResolveAmbientSTBForCell(uint32 CellId, const FACEDatAmbientSTBDesc*& OutStb)
{
	OutStb = nullptr;
	// Cell 0 = pre-enter-world. Indoor EnvCells have no terrain scene type.
	if ((CellId & 0xFFFFu) < 1 || (CellId & 0xFFFFu) > 64)
	{
		return false;
	}
	const FACEDatRegionSoundInfo* SoundInfo = GetRegionSoundInfo();
	if (!SoundInfo || !SoundInfo->bValid)
	{
		return false;
	}
	if (!bRegionSceneTablesLoaded)
	{
		TArray<uint8> RegionBlob;
		if (PortalDat && PortalDat->ReadFile(0x13000000u, RegionBlob))
		{
			FACEDatCursor Cur(RegionBlob);
			bRegionSceneTablesLoaded = ACEDatUnpack::UnpackRegionSceneTables(Cur, RegionSceneTables);
		}
	}
	if (!RegionSceneTables.bValid)
	{
		return false;
	}

	const uint32 LB = CellId & 0xFFFF0000u;
	const FACEBuiltLandblockMesh* Mesh = FindLandblockMesh(LB);
	if (!Mesh || !Mesh->bHasTerrain)
	{
		Mesh = GetOrBuildLandblockMesh(LB, 100.f);
	}
	if (!Mesh || !Mesh->bHasTerrain)
	{
		return false;
	}

	// Landcell IDs are one-based (1 + X*8 + Y), not zero-based vertices.
	const uint32 CellXY = (CellId & 0xFFFFu) - 1;
	const int32 CellX = static_cast<int32>(CellXY >> 3);
	const int32 CellY = static_cast<int32>(CellXY & 7);

	constexpr int32 VertexDim = 9;
	const int32 TerrainIdx = CellX * VertexDim + CellY;
	if (TerrainIdx < 0 || TerrainIdx >= 81)
	{
		return false;
	}
	const uint16 Terrain = Mesh->Terrain[TerrainIdx];
	const uint32 TerrainType = (Terrain >> 2) & 0x1F;
	const uint32 SceneTypeLocal = Terrain >> 11;
	if (!RegionSceneTables.TerrainSceneTypeIndices.IsValidIndex(static_cast<int32>(TerrainType)))
	{
		return false;
	}
	const TArray<uint32>& TypeScenes = RegionSceneTables.TerrainSceneTypeIndices[TerrainType];
	if (!TypeScenes.IsValidIndex(static_cast<int32>(SceneTypeLocal)))
	{
		return false;
	}
	const uint32 SceneInfoIdx = TypeScenes[SceneTypeLocal];
	if (!RegionSceneTables.SceneTypeStbIndices.IsValidIndex(static_cast<int32>(SceneInfoIdx)))
	{
		return false;
	}
	const uint32 StbIndex = RegionSceneTables.SceneTypeStbIndices[SceneInfoIdx];
	if (!SoundInfo->STBDescs.IsValidIndex(static_cast<int32>(StbIndex)))
	{
		return false;
	}
	OutStb = &SoundInfo->STBDescs[StbIndex];
	return OutStb->SoundTableId != 0 && OutStb->Sounds.Num() > 0;
}

bool UACEDatSubsystem::CollectRegionScenery(uint32 LandblockId, float WorldScale, TArray<FACEDatRegionSceneryItem>& OutItems)
{
	OutItems.Reset();
	if (!EnsureLoaded() || !PortalDat)
	{
		return false;
	}

	const uint64 Fingerprint = ComputeDatFingerprint();
	const uint32 LBKey = LandblockId & 0xFFFF0000u;
	const FString SceneryPath = ACESceneryTileCache::MakeCacheFilePath(LBKey, WorldScale, Fingerprint);
	if (ACESceneryTileCache::Load(SceneryPath, OutItems, WorldScale, Fingerprint))
	{
		return true;
	}

	if (!bRegionSceneTablesLoaded)
	{
		TArray<uint8> RegionBlob;
		if (PortalDat->ReadFile(0x13000000u, RegionBlob))
		{
			FACEDatCursor Cur(RegionBlob);
			bRegionSceneTablesLoaded = ACEDatUnpack::UnpackRegionSceneTables(Cur, RegionSceneTables);
		}
		if (!bRegionSceneTablesLoaded)
		{
			UE_LOG(LogTemp, Warning, TEXT("ACEDat: RegionDesc scene tables unavailable"));
			return false;
		}
	}

	// Prefer cache; if QueueScenery races ahead of Apply's cache install, sync-build once.
	const FACEBuiltLandblockMesh* Mesh = FindLandblockMesh(LandblockId);
	if (!Mesh || !Mesh->bHasTerrain || !Mesh->bHasHeights)
	{
		Mesh = GetOrBuildLandblockMesh(LandblockId, WorldScale);
	}
	if (!Mesh || !Mesh->bHasTerrain || !Mesh->bHasHeights)
	{
		// Drop any heights-only stale entry so RequestLandblockMesh actually rebuilds.
		++LandblockMeshRevision;
		LandblockCache.Remove(LandblockId & 0xFFFF0000u);
		RequestLandblockMesh(LandblockId, WorldScale);
		return false;
	}

	constexpr int32 VertexDim = FACELandblockMeshBuilder::VertexDim;
	constexpr int32 CellDim = FACELandblockMeshBuilder::CellDim;
	constexpr float CellSize = FACELandblockMeshBuilder::CellSize;
	constexpr float BlockLength = FACELandblockMeshBuilder::LandblockSize;
	const uint32 LB = LandblockId & 0xFFFF0000u;
	const uint32 BlockX = (LB >> 24) * 8u;
	const uint32 BlockY = ((LB >> 16) & 0xFFu) * 8u;

	// Retail CLandBlock::init_buildings assigns each building to the outdoor
	// sort cell containing its origin. get_land_scenes rejects that whole cell,
	// including open courtyards that have no shell/floor triangle below them.
	TSet<int32> BuildingCells;
	if (const auto BuildingInfo = GetLandblockInfo(LB))
		for (const auto& Building : BuildingInfo->Buildings)
		{
			const int32 X=FMath::FloorToInt(Building.Origin.X/CellSize);
			const int32 Y=FMath::FloorToInt(Building.Origin.Y/CellSize);
			if (X>=0 && X<CellDim && Y>=0 && Y<CellDim) BuildingCells.Add(X*CellDim+Y);
		}

	int32 CandCount = 0;
	int32 FreqSkip = 0;
	int32 BoundsOrRoadSkip = 0;
	int32 BuildingSkip = 0;
	int32 SlopeSkip = 0;
	int32 HeightSkip = 0;

	auto PseudoRand = [](uint32 A) -> float
	{
		return static_cast<float>(A) * 2.3283064e-10f;
	};

	auto GetRoadBits = [&](int32 Vx, int32 Vy) -> uint32
	{
		Vx = FMath::Clamp(Vx, 0, CellDim);
		Vy = FMath::Clamp(Vy, 0, CellDim);
		const int32 Idx = Vx * VertexDim + Vy;
		return (Idx >= 0 && Idx < 81) ? (static_cast<uint32>(Mesh->Terrain[Idx]) & 0x3u) : 0u;
	};

	// ACE Landblock.OnRoad — cull placements on road ribbons, not whole road cells.
	auto OnRoad = [&](float AcX, float AcY) -> bool
	{
		constexpr float RoadWidth = 5.f;
		constexpr float TileLength = CellSize; // 24
		const int32 X = FMath::FloorToInt(AcX / TileLength);
		const int32 Y = FMath::FloorToInt(AcY / TileLength);
		const float RMin = RoadWidth;
		const float RMax = TileLength - RoadWidth;
		const uint32 R0 = GetRoadBits(X, Y);
		const uint32 R1 = GetRoadBits(X, Y + 1);
		const uint32 R2 = GetRoadBits(X + 1, Y);
		const uint32 R3 = GetRoadBits(X + 1, Y + 1);
		if (R0 == 0 && R1 == 0 && R2 == 0 && R3 == 0)
		{
			return false;
		}
		const float Dx = AcX - X * TileLength;
		const float Dy = AcY - Y * TileLength;
		if (R0 > 0)
		{
			if (R1 > 0)
			{
				if (R2 > 0)
				{
					return (R3 > 0) ? true : (Dx < RMin || Dy < RMin);
				}
				return (R3 > 0) ? (Dx < RMin || Dy > RMax) : (Dx < RMin);
			}
			if (R2 > 0)
			{
				return (R3 > 0) ? (Dx > RMax || Dy < RMin) : (Dy < RMin);
			}
			return (R3 > 0) ? (FMath::Abs(Dx - Dy) < RMin) : (Dx + Dy < RMin);
		}
		if (R1 > 0)
		{
			if (R2 > 0)
			{
				return (R3 > 0) ? (Dx > RMax || Dy > RMax) : (FMath::Abs(Dx + Dy - TileLength) < RMin);
			}
			return (R3 > 0) ? (Dy > RMax) : (TileLength + Dx - Dy < RMin);
		}
		if (R2 > 0)
		{
			return (R3 > 0) ? (Dx > RMax) : (TileLength - Dx + Dy < RMin);
		}
		return (R3 > 0) && (TileLength * 2.f - Dx - Dy < RMin);
	};

	for (int32 CellX = 0; CellX < CellDim; ++CellX)
	{
		for (int32 CellY = 0; CellY < CellDim; ++CellY)
		{
			const int32 TerrainIdx = CellX * VertexDim + CellY;
			const uint16 Terrain = Mesh->Terrain[TerrainIdx];
			// Do NOT skip entire road cells — ACE still places scenery off the road ribbon.

			const uint32 TerrainType = (Terrain >> 2) & 0x1F;
			const uint32 SceneTypeLocal = Terrain >> 11;
			if (!RegionSceneTables.TerrainSceneTypeIndices.IsValidIndex(static_cast<int32>(TerrainType)))
			{
				continue;
			}
			const TArray<uint32>& TypeScenes = RegionSceneTables.TerrainSceneTypeIndices[TerrainType];
			if (!TypeScenes.IsValidIndex(static_cast<int32>(SceneTypeLocal)))
			{
				continue;
			}
			const uint32 SceneInfoIdx = TypeScenes[SceneTypeLocal];
			if (!RegionSceneTables.SceneTypes.IsValidIndex(static_cast<int32>(SceneInfoIdx)))
			{
				continue;
			}
			const TArray<uint32>& Scenes = RegionSceneTables.SceneTypes[SceneInfoIdx];
			if (Scenes.Num() == 0)
			{
				continue;
			}

			const uint32 GlobalCellX = static_cast<uint32>(CellX) + BlockX;
			const uint32 GlobalCellY = static_cast<uint32>(CellY) + BlockY;
			const uint32 CellMat = GlobalCellY * (712977289u * GlobalCellX + 1813693831u)
				- 1109124029u * GlobalCellX + 2139937281u;
			const float Offset = PseudoRand(CellMat);
			int32 SceneIdx = static_cast<int32>(Scenes.Num() * Offset);
			if (SceneIdx >= Scenes.Num())
			{
				SceneIdx = 0;
			}
			const uint32 SceneId = Scenes[SceneIdx];

			const FACEDatScene* Scene = SceneCache.Find(SceneId);
			if (!Scene)
			{
				TArray<uint8> SceneBlob;
				if (!PortalDat->ReadFile(SceneId, SceneBlob))
				{
					continue;
				}
				FACEDatCursor SceneCur(SceneBlob);
				FACEDatScene Parsed;
				if (!ACEDatUnpack::UnpackScene(SceneCur, Parsed))
				{
					continue;
				}
				Scene = &SceneCache.Add(SceneId, MoveTemp(Parsed));
			}

			const uint32 CellXMat = static_cast<uint32>(-1109124029) * GlobalCellX;
			const uint32 CellYMat = 1813693831u * GlobalCellY;
			const uint32 ComboMat = 1360117743u * GlobalCellX * GlobalCellY + 1888038839u;

			for (int32 J = 0; J < Scene->Objects.Num(); ++J)
			{
				const FACEDatSceneObjectDesc& Obj = Scene->Objects[J];
				if (Obj.WeenieObj != 0 || Obj.ObjId == 0)
				{
					continue;
				}
				++CandCount;
				const float Noise = PseudoRand(CellXMat + CellYMat - ComboMat * static_cast<uint32>(23399 + J));
				if (Noise >= Obj.Freq)
				{
					++FreqSkip;
					continue;
				}

				// ObjectDesc.Displace
				FVector Loc = Obj.Origin;
				if (Obj.DisplaceX > 0.f)
				{
					Loc.X = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 45773u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX) * Obj.DisplaceX + Obj.Origin.X;
				}
				if (Obj.DisplaceY > 0.f)
				{
					Loc.Y = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 72719u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX) * Obj.DisplaceY + Obj.Origin.Y;
				}
				const float Quadrant = PseudoRand(1813693831u * GlobalCellY
					- GlobalCellX * (1870387557u * GlobalCellY + 1109124029u) - 402451965u);
				FVector Displaced = Loc;
				if (Quadrant >= 0.75f) { Displaced = FVector(Loc.Y, -Loc.X, Loc.Z); }
				else if (Quadrant >= 0.5f) { Displaced = FVector(-Loc.X, -Loc.Y, Loc.Z); }
				else if (Quadrant >= 0.25f) { Displaced = FVector(-Loc.Y, Loc.X, Loc.Z); }

				const float Lx = static_cast<float>(CellX) * CellSize + Displaced.X;
				const float Ly = static_cast<float>(CellY) * CellSize + Displaced.Y;
				if (Lx < 0.f || Ly < 0.f || Lx >= BlockLength || Ly >= BlockLength || OnRoad(Lx, Ly))
				{
					++BoundsOrRoadSkip;
					continue;
				}

				if (BuildingCells.Contains(FMath::FloorToInt(Lx/CellSize)*CellDim+FMath::FloorToInt(Ly/CellSize)))
				{
					++BuildingSkip;
					continue;
				}

				float ZAc = 0.f;
				if (!FACELandblockMeshBuilder::SampleHeightAc(*Mesh, Lx, Ly, ZAc))
				{
					++HeightSkip;
					continue;
				}

				// Approximate slope from neighboring heights.
				float Zxp = ZAc, Zxm = ZAc, Zyp = ZAc, Zym = ZAc;
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, FMath::Min(Lx + 1.f, BlockLength - 0.01f), Ly, Zxp);
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, FMath::Max(Lx - 1.f, 0.f), Ly, Zxm);
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, Lx, FMath::Min(Ly + 1.f, BlockLength - 0.01f), Zyp);
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, Lx, FMath::Max(Ly - 1.f, 0.f), Zym);
				const FVector Normal = FVector(Zxm - Zxp, Zym - Zyp, 2.f).GetSafeNormal();
				if (Normal.Z < Obj.MinSlope || Normal.Z > Obj.MaxSlope)
				{
					++SlopeSkip;
					continue;
				}

				float Scale = Obj.MaxScale;
				if (!FMath::IsNearlyEqual(Obj.MinScale, Obj.MaxScale))
				{
					const float T = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 32593u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX);
					Scale = FMath::Pow(Obj.MaxScale / Obj.MinScale, T) * Obj.MinScale;
				}

				FQuat Orient = Obj.Orientation;
				if (Obj.Align != 0)
				{
					// ObjectDesc.ObjAlign — AC get_heading: (450 - atan2(y,x)°) % 360
					const FVector NegN = -Normal;
					const float AtanDeg = FMath::RadiansToDegrees(FMath::Atan2(NegN.Y, NegN.X));
					float HeadingDeg = FMath::Fmod(450.f - AtanDeg, 360.f);
					if (HeadingDeg < 0.f)
					{
						HeadingDeg += 360.f;
					}
					Orient = FQuat(FVector::UpVector, FMath::DegreesToRadians(HeadingDeg));
				}
				else if (Obj.MaxRotation > 0.f)
				{
					const float Degrees = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 63127u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX) * Obj.MaxRotation;
					Orient = FQuat(FVector::UpVector, FMath::DegreesToRadians(Degrees)) * Orient;
				}

				FACEDatRegionSceneryItem Item;
				Item.SetupId = Obj.ObjId;
				Item.OriginAc = FVector(Lx, Ly, ZAc + Displaced.Z);
				Item.Orientation = Orient;
				Item.Scale = Scale;
				OutItems.Add(Item);
			}
		}
	}

	UE_LOG(LogTemp, Log,
		TEXT("ACEDat: RegionDesc scenery landblock 0x%08X — %d objects (cand=%d freqSkip=%d bounds/road=%d bldg=%d height=%d slope=%d)"),
		LB, OutItems.Num(), CandCount, FreqSkip, BoundsOrRoadSkip, BuildingSkip, HeightSkip, SlopeSkip);
	ACESceneryTileCache::Save(SceneryPath, OutItems, WorldScale, Fingerprint);
	// true = tables + heights were usable (even when every candidate was culled).
	return true;
}

bool UACEDatSubsystem::LoadLandblockInfo(uint32 LandblockId, FACEDatLandblockInfo& OutInfo)
{
	OutInfo = FACEDatLandblockInfo();
	if (const auto Info = GetLandblockInfo(LandblockId))
	{
		OutInfo = *Info;
		return true;
	}
	return false;
}

TSharedPtr<const FACEDatLandblockInfo> UACEDatSubsystem::GetLandblockInfo(uint32 LandblockId)
{
	// EnsureLoaded can invalidate the cache after DAT replacement. Return an owned
	// immutable snapshot so nested mesh requests cannot invalidate a reader.
	if (!EnsureLoaded() || !CellDat)
	{
		return nullptr;
	}

	// LandblockInfo file id = (LB << 16) | 0xFFFE where LB is the high 16 bits of the cell.
	const uint32 LbKey = LandblockId & 0xFFFF0000u;
	if (const auto* Cached = LandblockInfoCache.Find(LbKey))
	{
		return *Cached;
	}

	const uint32 FileId = LbKey | 0x0000FFFEu;
	TArray<uint8> Blob;
	if (!CellDat->ReadFile(FileId, Blob))
	{
		return nullptr;
	}
	FACEDatCursor Cur(Blob);
	auto Parsed = MakeShared<FACEDatLandblockInfo>();
	if (!ACEDatUnpack::UnpackLandblockInfo(Cur, *Parsed))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: failed to unpack LandblockInfo 0x%08X"), FileId);
		return nullptr;
	}
	LandblockInfoCache.Add(LbKey, Parsed);
	return Parsed;
}

static TAutoConsoleVariable<int32> CVarACECacheDoorwayGeometry(
	TEXT("ace.Render.CacheDoorwayGeometry"), 1,
	TEXT("Reuse immutable building doorway geometry. Camera/frustum admission remains live."));

void UACEDatSubsystem::LoadBuildingDoorwayApertures(uint32 LandblockId, float WorldScale,
	TArray<ACEOutdoorPortalPlan::FAdmittedAperture>& OutApertures)
{
	OutApertures.Reset();
	if (const auto Geometry = GetBuildingDoorwayApertures(LandblockId, WorldScale))
		OutApertures = *Geometry;
}

TSharedPtr<const TArray<ACEOutdoorPortalPlan::FAdmittedAperture>> UACEDatSubsystem::GetBuildingDoorwayApertures(
	uint32 LandblockId, float WorldScale)
{
	// EnsureLoaded may invalidate DAT/mesh caches after a reload or format change.
	if (!EnsureLoaded() || !CellDat) return nullptr;
	const uint32 Key = LandblockId & 0xFFFF0000u;
	uint32 ScaleBits;
	FMemory::Memcpy(&ScaleBits, &WorldScale, sizeof(ScaleBits));
	const uint64 CacheKey = (uint64(Key) << 32) | ScaleBits;
	const bool bCache = CVarACECacheDoorwayGeometry.GetValueOnGameThread() != 0;
	if (bCache)
		if (auto* Cached = DoorwayGeometryCache.Find(CacheKey))
		{
			Cached->LastUse = ++DoorwayGeometryUse;
			return Cached->Apertures;
		}

	const auto Info = GetLandblockInfo(Key);
	if (!Info) return nullptr;
	auto Geometry = MakeShared<TArray<ACEOutdoorPortalPlan::FAdmittedAperture>>();
	auto& OutApertures = *Geometry;
	const FVector Origin = FACEPosition::AceVectorToUnreal(
		FVector(((Key >> 24) & 255) * 192.f, ((Key >> 16) & 255) * 192.f, 0), WorldScale);
	bool bComplete = true;
	for (const auto& Building : Info->Buildings)
	{
		const auto* Setup = FindSetupMesh(Building.ModelId, WorldScale);
		if (!Setup)
		{
			RequestSetupMesh(Building.ModelId, WorldScale);
			bComplete = false; // Never cache a partial result while streaming.
			continue;
		}
		const FTransform Frame(FACEPosition::AceQuatToUnreal(FQuat(Building.Orientation)),
			Origin + FACEPosition::AceVectorToUnreal(FVector(Building.Origin), WorldScale));
		for (const auto& Part : Setup->Parts) for (const auto& Poly : Part.Portals)
		{
			if (!Building.Portals.IsValidIndex(Poly.PortalIndex) || Poly.Vertices.Num() < 3) continue;
			const auto& Portal = Building.Portals[Poly.PortalIndex];
			if (Portal.OtherCellId < 0x100 || Portal.OtherCellId == 0xFFFF) continue;
			const FTransform Transform = Part.BindTransform * Frame;
			auto& A = OutApertures.AddDefaulted_GetRef();
			A.DestEnvCellId = Key | Portal.OtherCellId;
			A.OtherPortalId = Portal.OtherPortalId;
			// Retail PView uses the building PORT polygon and CBldPortal side.
			A.WorldNormal = Transform.TransformVectorNoScale(Poly.Normal).GetSafeNormal()
				* (Portal.IsPortalSide() ? -1.f : 1.f);
			for (const auto& V : Poly.Vertices) A.WorldVerts.Add(Transform.TransformPosition(V));
		}
	}
	if (bCache && bComplete)
	{
		// Only compact geometry is retained, never Setup meshes or streamed actors.
		// Bound roaming/teleport memory independently of the world residency set.
		if (DoorwayGeometryCache.Num() >= 256)
		{
			uint64 OldestKey = 0, OldestUse = MAX_uint64;
			for (const auto& Entry : DoorwayGeometryCache)
				if (Entry.Value.LastUse < OldestUse)
				{ OldestKey = Entry.Key; OldestUse = Entry.Value.LastUse; }
			DoorwayGeometryCache.Remove(OldestKey);
		}
		auto& Cached = DoorwayGeometryCache.Add(CacheKey);
		Cached.Apertures = Geometry;
		Cached.LastUse = ++DoorwayGeometryUse;
	}
	return Geometry;
}

bool UACEDatSubsystem::ResolveWorldEntryCell(const FACEPosition& Position, uint32& OutCellId, bool& bComplete)
{
	OutCellId = Position.CellId;
	bComplete = false;
	if (!Position.IsValid() || !EnsureLoaded() || !EnvCellBuilder) return false;
	const auto Info = GetLandblockInfo(Position.CellId);
	if (!Info || Info->NumCells == 0 || Info->NumCells > 0xFEFEu) return false;
	return EnvCellBuilder->ResolvePlacementCell(Position.CellId, Info->NumCells,
		Position.Location, OutCellId, bComplete);
}

bool UACEDatSubsystem::LoadEnvCell(uint32 EnvCellId, FACEDatEnvCell& OutCell)
{
	OutCell = FACEDatEnvCell();
	if ((EnvCellId & 0xFFFFu) < 0x0100u || EnvCellId == 0)
	{
		return false;
	}
	if (!EnsureLoaded() || !CellDat)
	{
		return false;
	}
	TArray<uint8> Blob;
	if (!CellDat->ReadFile(EnvCellId, Blob))
	{
		return false;
	}
	FACEDatCursor Cur(Blob);
	return ACEDatUnpack::UnpackEnvCell(Cur, OutCell);
}

const FACEBuiltEnvCellMesh* UACEDatSubsystem::GetOrBuildEnvCellMesh(uint32 EnvCellId, float WorldScale)
{
	// Outdoor landcells are never EnvCells — refuse early so a bad CellId can't trigger a
	// full Environment unpack on the game thread.
	if ((EnvCellId & 0xFFFFu) < 0x0100u)
	{
		return nullptr;
	}
	if (!EnsureLoaded() || !EnvCellBuilder || EnvCellId == 0)
	{
		return nullptr;
	}
	const uint64 CacheKey = MakeScaleCacheKey(EnvCellId, WorldScale);
	if (const FACEBuiltEnvCellMesh* Existing = EnvCellMeshCache.Find(CacheKey))
	{
		return Existing;
	}

	const uint64 Fingerprint = ComputeDatFingerprint();
	const FString CachePath = ACEEnvCellTileCache::MakeCacheFilePath(EnvCellId, WorldScale, Fingerprint);
	{
		FACEBuiltEnvCellMesh FromDisk;
		if (ACEEnvCellTileCache::Load(CachePath, FromDisk, WorldScale, Fingerprint)
			&& (!FromDisk.IsEmpty() || FromDisk.bPortalConnector || FromDisk.StaticObjects.Num() > 0))
		{
			FromDisk.EnvCellId = EnvCellId;
			FailedEnvCellKeys.Remove(CacheKey);
			PendingEnvCellKeys.Remove(CacheKey);
			return &EnvCellMeshCache.Add(CacheKey, MoveTemp(FromDisk));
		}
	}

	FACEBuiltEnvCellMesh Built;
	if (!EnvCellBuilder->BuildEnvCell(EnvCellId, WorldScale, Built))
	{
		return nullptr;
	}
	ACEEnvCellTileCache::Save(CachePath, Built, WorldScale, Fingerprint);
	FailedEnvCellKeys.Remove(CacheKey);
	PendingEnvCellKeys.Remove(CacheKey);
	return &EnvCellMeshCache.Add(CacheKey, MoveTemp(Built));
}

const FACEBuiltEnvCellMesh* UACEDatSubsystem::FindEnvCellMesh(uint32 EnvCellId, float WorldScale) const
{
	if ((EnvCellId & 0xFFFFu) < 0x0100u || EnvCellId == 0)
	{
		return nullptr;
	}
	return EnvCellMeshCache.Find(MakeScaleCacheKey(EnvCellId, WorldScale));
}

UACEDatSubsystem::EACEEnvCellMeshStatus UACEDatSubsystem::RequestEnvCellMesh(uint32 EnvCellId, float WorldScale)
{
	if ((EnvCellId & 0xFFFFu) < 0x0100u || EnvCellId == 0)
	{
		return EACEEnvCellMeshStatus::Failed;
	}
	if (!EnsureLoaded() || !EnvCellBuilder)
	{
		return EACEEnvCellMeshStatus::NotReady;
	}
	const uint64 CacheKey = MakeScaleCacheKey(EnvCellId, WorldScale);
	CancelledEnvCellKeys.Remove(CacheKey);
	if (EnvCellMeshCache.Contains(CacheKey))
	{
		return EACEEnvCellMeshStatus::Ready;
	}
	if (FailedEnvCellKeys.Contains(CacheKey))
	{
		return EACEEnvCellMeshStatus::Failed;
	}
	if (PendingEnvCellKeys.Contains(CacheKey))
	{
		return EACEEnvCellMeshStatus::Pending;
	}
	PendingEnvCellKeys.Add(CacheKey);
	PendingEnvCellBuilds.Add({ EnvCellId, WorldScale, CacheKey });
	PumpEnvCellBuildQueue();
	return EACEEnvCellMeshStatus::Pending;
}

void UACEDatSubsystem::CancelEnvCellMeshRequestsOutside(const TSet<int32>& KeepEnvCellIds)
{
	for (uint64 Key : PendingEnvCellKeys)
		if (!KeepEnvCellIds.Contains(static_cast<int32>(Key >> 32))) CancelledEnvCellKeys.Add(Key);
	for (int32 i = PendingEnvCellBuilds.Num() - 1; i >= 0; --i)
	{
		const uint32 Id = PendingEnvCellBuilds[i].Id;
		if (!KeepEnvCellIds.Contains(static_cast<int32>(Id)))
		{
			PendingEnvCellKeys.Remove(PendingEnvCellBuilds[i].CacheKey);
			CancelledEnvCellKeys.Remove(PendingEnvCellBuilds[i].CacheKey);
			PendingEnvCellBuilds.RemoveAt(i);
		}
	}
}

void UACEDatSubsystem::EvictEnvCellMeshesOutside(const TSet<int32>& KeepEnvCellIds, float WorldScale)
{
	TArray<uint64> ToRemove;
	ToRemove.Reserve(EnvCellMeshCache.Num());
	for (const auto& Pair : EnvCellMeshCache)
	{
		const uint32 IdFromKey = static_cast<uint32>(Pair.Key >> 32);
		if (!KeepEnvCellIds.Contains(static_cast<int32>(IdFromKey)))
		{
			ToRemove.Add(Pair.Key);
		}
	}
	for (uint64 Key : ToRemove)
	{
		EnvCellMeshCache.Remove(Key);
		FailedEnvCellKeys.Remove(Key);
	}
	if (ToRemove.Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: evicted %d EnvCell mesh cache entries (keep=%d)"),
			ToRemove.Num(), KeepEnvCellIds.Num());
	}
	(void)WorldScale;
}

void UACEDatSubsystem::TrimSetupMeshCache(int32 MaxEntries)
{
	if (MaxEntries < 64)
	{
		MaxEntries = 64;
	}
	const int32 Over = SetupMeshCache.Num() - MaxEntries;
	if (Over <= 0)
	{
		return;
	}
	TArray<uint64> Keys;
	SetupMeshCache.GetKeys(Keys);
	const int32 RemoveCount = FMath::Min(Over, Keys.Num());
	for (int32 i = 0; i < RemoveCount; ++i)
	{
		SetupMeshCache.Remove(Keys[i]);
		FailedSetupKeys.Remove(Keys[i]);
	}
	UE_LOG(LogTemp, Log, TEXT("ACEDat: trimmed Setup mesh cache by %d (now %d)"),
		RemoveCount, SetupMeshCache.Num());
}

void UACEDatSubsystem::TrimSetupStaticMeshCache(int32 MaxEntries)
{
	if (MaxEntries < 32)
	{
		MaxEntries = 32;
	}
	const int32 Over = SetupStaticMeshCache.Num() - MaxEntries;
	if (Over <= 0)
	{
		return;
	}
	TArray<uint64> Keys;
	SetupStaticMeshCache.GetKeys(Keys);
	const int32 RemoveCount = FMath::Min(Over, Keys.Num());
	for (int32 i = 0; i < RemoveCount; ++i)
	{
		if (UStaticMesh* Mesh = SetupStaticMeshCache.FindRef(Keys[i]).Get())
		{
			Mesh->RemoveFromRoot();
		}
		SetupStaticMeshCache.Remove(Keys[i]);
		SetupStaticMeshSlotMaterials.Remove(Keys[i]);
	}
	UE_LOG(LogTemp, Log, TEXT("ACEDat: trimmed Setup static-mesh cache by %d (now %d)"),
		RemoveCount, SetupStaticMeshCache.Num());
}

void UACEDatSubsystem::TrimResolvedTextureCache(int32 MaxEntries)
{
	MaxEntries = FMath::Max(64, MaxEntries);
	auto OldestKeys = [&](const auto& Cache)
	{
		TArray<uint64> Keys;
		if (Cache.Num() > MaxEntries)
		{
			Cache.GetKeys(Keys);
			Keys.Sort([&](uint64 A, uint64 B) { return ResolvedCacheLastUse.FindRef(A) < ResolvedCacheLastUse.FindRef(B); });
			Keys.SetNum(Cache.Num() - MaxEntries);
		}
		return Keys;
	};
	for (uint64 Key : OldestKeys(ResolvedTextureCache))
	{
		if (auto* Texture = ResolvedTextureCache.FindRef(Key).Get()) Texture->RemoveFromRoot();
		ResolvedTextureCache.Remove(Key);
	}
	// Material-only classifications need their own bound, even when the texture
	// cache is below its limit. Eviction must not forget live crowd resources.
	for (uint64 Key : OldestKeys(ResolvedMaterialCache)) ResolvedMaterialCache.Remove(Key);
	for (auto It=LiveResolvedTextures.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	for (auto It=LiveResolvedMaterials.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	for (auto It=ResolvedCacheLastUse.CreateIterator(); It; ++It)
		if (!ResolvedTextureCache.Contains(It.Key()) && !ResolvedMaterialCache.Contains(It.Key())) It.RemoveCurrent();
}

void UACEDatSubsystem::ResetSurfaceMaterialCache(TMap<uint32,TObjectPtr<UMaterialInstanceDynamic>>& Cache,uint32 Kind)
{
    Cache.Reset();
    for(auto It=SurfaceMaterials.CreateIterator();It;++It) if((It.Key()>>32)==Kind) It.RemoveCurrent();
}

UMaterialInstanceDynamic* UACEDatSubsystem::FindSurfaceMaterial(TMap<uint32,TObjectPtr<UMaterialInstanceDynamic>>& Cache,uint32 Kind,uint32 Key)
{
    const uint64 FullKey=(uint64(Kind)<<32)|Key;
    if(auto* Material=Cache.FindRef(Key).Get())
    {
        auto& Record=SurfaceMaterials.FindOrAdd(FullKey);
        Record.Material=Material;
        Record.LastUse=++SurfaceMaterialClock;
        return Material;
    }
    if(auto* Record=SurfaceMaterials.Find(FullKey))if(auto* Material=Record->Material.Get())
    {Record->LastUse=++SurfaceMaterialClock;Cache.Add(Key,Material);return Material;}
    return nullptr;
}

void UACEDatSubsystem::TrimTextureResolverCaches(int32 MaxTextures, int32 MaxDecodedSurfaces, int64 MaxBytes)
{
	for (auto It = LandMaterialCache.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	for (auto It = LandSkyInstances.CreateIterator(); It; ++It) if (!It->IsValid()) It.RemoveCurrent();
	for (auto It = LandTextureCache.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	// A converted material can outlive the source MID. Keep its weak record while
	// visible so daylight/fog updates still reach it after the source is collected.
	for (auto It = WorldObjectMaterials.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	for (auto It = InteriorObjectMaterials.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	for (auto It = DungeonObjectMaterials.CreateIterator(); It; ++It) if (!It.Value().IsValid()) It.RemoveCurrent();
	if (TextureResolver)
	{
		TextureResolver->TrimCaches(MaxTextures, MaxDecodedSurfaces, MaxBytes);
	}
	// These were independently unbounded strong maps. Touring new regions kept
    // old walls, scenery and particles (and all their GPU textures) alive forever.
    struct FCandidate {uint64 Key,LastUse;};TArray<FCandidate> Candidates;
    auto Collect=[&](const auto& Cache,uint32 Kind)
    {
        for(const auto& Pair:Cache)
        {
            const uint64 Key=(uint64(Kind)<<32)|Pair.Key;
            auto& Record=SurfaceMaterials.FindOrAdd(Key);
            Record.Material=Pair.Value;
            Candidates.Add({Key,Record.LastUse});
        }
    };
    Collect(TexturedMaterialCache,0);Collect(BuildingShellMaterialCache,1);
    Collect(OutdoorLitMaterialCache,2);Collect(EnvCellMaterialCache,3);Collect(ParticleMaterialCache,4);
    const int32 Over=Candidates.Num()-FMath::Max(64,MaxTextures);
    if(Over>0)
    {
        Candidates.Sort([](const auto& A,const auto& B){return A.LastUse<B.LastUse;});
        for(int32 I=0;I<Over;++I)
        {
            const uint32 Key=uint32(Candidates[I].Key);
            switch(Candidates[I].Key>>32)
            {
            case 0:TexturedMaterialCache.Remove(Key);break;
            case 1:BuildingShellMaterialCache.Remove(Key);break;
            case 2:OutdoorLitMaterialCache.Remove(Key);break;
            case 3:EnvCellMaterialCache.Remove(Key);break;
            case 4:ParticleMaterialCache.Remove(Key);break;
            }
        }
    }
    for(auto It=SurfaceMaterials.CreateIterator();It;++It)if(!It.Value().Material.IsValid())It.RemoveCurrent();
}

void UACEDatSubsystem::AllowEnvCellMeshRetry(uint32 EnvCellId, float WorldScale)
{
	FailedEnvCellKeys.Remove(MakeScaleCacheKey(EnvCellId, WorldScale));
}

void UACEDatSubsystem::PumpEnvCellBuildQueue()
{
	if (!CellDat || !PortalDat || !EnvCellBuilder)
	{
		return;
	}
	const int32 MaxConcurrent = ACEStreamingBudget::EnvCellWorkers();
	while (PendingEnvCellBuilds.Num() > 0 && ActiveEnvCellBuilds.GetValue() < MaxConcurrent)
	{
		const FPendingScaledMeshBuild Job = PendingEnvCellBuilds[0];
		PendingEnvCellBuilds.RemoveAt(0);

		const int32 Generation = EnvCellBuildGeneration;
		FACEDatDatabase* CellPtr = CellDat.Get();
		FACEDatDatabase* PortalPtr = PortalDat.Get();
		FACEDatDatabase* HighResPtr = HighResDat.Get();
		const uint64 Fingerprint = ComputeDatFingerprint();
		const FString CachePath = ACEEnvCellTileCache::MakeCacheFilePath(Job.Id, Job.WorldScale, Fingerprint);
		TWeakObjectPtr<UACEDatSubsystem> WeakThis(this);
		ActiveEnvCellBuilds.Increment();
		FThreadSafeCounter* ActiveCounter = &ActiveEnvCellBuilds;

		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
			[WeakThis, Job, Generation, CellPtr, PortalPtr, HighResPtr, ActiveCounter, Fingerprint, CachePath]()
			{
				TSharedPtr<FACEBuiltEnvCellMesh> Built = MakeShared<FACEBuiltEnvCellMesh>();
				bool bOk = false;
				if (ACEEnvCellTileCache::Load(CachePath, *Built, Job.WorldScale, Fingerprint))
				{
					Built->EnvCellId = Job.Id;
					bOk = !Built->IsEmpty() || Built->bPortalConnector || Built->StaticObjects.Num() > 0;
				}
				else if (CellPtr && PortalPtr)
				{
					FACEDatTextureResolver WorkerTextures(PortalPtr, HighResPtr);
					FACEEnvCellMeshBuilder WorkerBuilder(CellPtr, PortalPtr, &WorkerTextures);
					bOk = WorkerBuilder.BuildEnvCell(Job.Id, Job.WorldScale, *Built)
						&& (!Built->IsEmpty() || Built->bPortalConnector
							|| Built->StaticObjects.Num() > 0);
					if (bOk)
					{
						ACEEnvCellTileCache::Save(CachePath, *Built, Job.WorldScale, Fingerprint);
					}
				}
				ActiveCounter->Decrement();
				AsyncTask(ENamedThreads::GameThread,
					[WeakThis, Job, Generation, bOk, Built]()
					{
						if (UACEDatSubsystem* Self = WeakThis.Get())
						{
							Self->OnEnvCellBuildComplete(Job.Id, Job.WorldScale, Job.CacheKey, Generation, bOk, Built);
						}
					});
			});
	}
}

void UACEDatSubsystem::OnEnvCellBuildComplete(uint32 EnvCellId, float WorldScale, uint64 CacheKey, int32 Generation,
	bool bOk, TSharedPtr<FACEBuiltEnvCellMesh> Mesh)
{
	if (Generation != EnvCellBuildGeneration)
	{
		// The current visibility plan re-requests cells after a format reload. An old
		// completion must neither resurrect the departed area nor cancel its new job.
		PumpEnvCellBuildQueue();
		return;
	}
	PendingEnvCellKeys.Remove(CacheKey);
	if (CancelledEnvCellKeys.Remove(CacheKey) > 0)
	{
		PumpEnvCellBuildQueue();
		return;
	}
	if (bOk && Mesh)
	{
		EnvCellMeshCache.FindOrAdd(CacheKey) = MoveTemp(*Mesh);
		FailedEnvCellKeys.Remove(CacheKey);
	}
	else
	{
		FailedEnvCellKeys.Add(CacheKey);
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: EnvCell 0x%08X mesh build failed (async)"), EnvCellId);
	}
	PumpEnvCellBuildQueue();
}

bool UACEDatSubsystem::IsPointInsideEnvCell(uint32 EnvCellId, const FVector& UnrealWorldPos, float WorldScale, float PaddingCm)
{
	const FACEBuiltEnvCellMesh* Mesh = FindEnvCellMesh(EnvCellId, WorldScale);
	if (!Mesh)
	{
		RequestEnvCellMesh(EnvCellId, WorldScale);
		return false;
	}

	const uint32 Lbx = (EnvCellId >> 24) & 0xFF;
	const uint32 Lby = (EnvCellId >> 16) & 0xFF;
	const FVector LandblockOrigin = FACEPosition::AceVectorToUnreal(
		FVector(Lbx * 192.f, Lby * 192.f, 0.f), WorldScale);
	// UE FTransform: A*B applies A first, then B. Cell-local → cell Frame → +landblock
	// origin (matches AACEEnvCellActor: actor at LandblockOrigin, CellXform relative).
	const FTransform CellToWorld = Mesh->GetCellLocalToLandblock(WorldScale)
		* FTransform(FQuat::Identity, LandblockOrigin);
	const FVector LocalUE = CellToWorld.InverseTransformPosition(UnrealWorldPos);

	// Prefer CellStruct CellBSP (retail EnvCell.point_in_cell / BSPNode.point_inside_cell_bsp).
	// Planes are ACE cell-local RH; invert AceVectorToUnreal (negate X, unscale).
	if (Mesh->CellBspNodes.Num() > 0)
	{
		const float InvScale = (WorldScale > KINDA_SMALL_NUMBER) ? (1.f / WorldScale) : 1.f;
		const FVector AceLocal(-LocalUE.X * InvScale, LocalUE.Y * InvScale, LocalUE.Z * InvScale);
		// PhysicsGlobals.EPSILON — Front/Close → PosChild (or true); Behind → false.
		constexpr float Epsilon = 0.0002f;
		int32 Idx = 0;
		constexpr int32 MaxWalk = 4096;
		for (int32 Step = 0; Step < MaxWalk; ++Step)
		{
			if (!Mesh->CellBspNodes.IsValidIndex(Idx))
			{
				return false;
			}
			const FACEDatCellBspNode& Node = Mesh->CellBspNodes[Idx];
			if (Node.bLeaf)
			{
				return true;
			}
			const float Dist = Node.Plane.X * static_cast<float>(AceLocal.X)
				+ Node.Plane.Y * static_cast<float>(AceLocal.Y)
				+ Node.Plane.Z * static_cast<float>(AceLocal.Z)
				+ Node.Plane.W;
			if (Dist < -Epsilon)
			{
				return false; // Behind — NegChild is never walked for CellBSP
			}
			if (Node.PosChild == INDEX_NONE)
			{
				return true;
			}
			Idx = Node.PosChild;
		}
		return false;
	}

	// Fallback AABB when CellBSP is empty (rare / incomplete CellStruct).
	if (!Mesh->bHasLocalBounds)
	{
		return false;
	}
	const float Pad = FMath::Max(0.f, PaddingCm);
	const FVector PadVec(Pad, Pad, Pad);
	return FBox(Mesh->LocalBoundsMin - PadVec, Mesh->LocalBoundsMax + PadVec).IsInsideOrOn(LocalUE);
}

bool UACEDatSubsystem::ResolveContainingEnvCell(uint32 HintCellId, const FVector& UnrealWorldPos, float WorldScale, uint32& OutEnvCellId)
{
	OutEnvCellId = 0;
	if (!EnsureLoaded())
	{
		return false;
	}

	const uint32 LandblockKey = HintCellId & 0xFFFF0000u;
	const bool bIndoorHint = (HintCellId & 0xFFFFu) >= 0x0100u;

	auto TryCell = [&](uint32 FullId) -> bool
	{
		if ((FullId & 0xFFFFu) < 0x0100u)
		{
			return false;
		}
		if (IsPointInsideEnvCell(FullId, UnrealWorldPos, WorldScale))
		{
			OutEnvCellId = FullId;
			return true;
		}
		return false;
	};

	// BFS CellPortals + VisibleCells so basement / upper floors are found (shallow 1-hop missed them).
	TArray<uint32> Queue;
	TSet<uint32> Visited;
	auto Enqueue = [&](uint32 FullId)
	{
		if ((FullId & 0xFFFFu) < 0x0100u || Visited.Contains(FullId))
		{
			return;
		}
		Visited.Add(FullId);
		Queue.Add(FullId);
	};

	if (bIndoorHint)
	{
		Enqueue(HintCellId);
	}
	else
	{
		const auto Info = GetLandblockInfo(LandblockKey);
		if (!Info || Info->NumCells == 0)
		{
			return false;
		}
		for (const FACEDatLandblockBuilding& Building : Info->Buildings)
		{
			for (uint16 ShortId : Building.PortalCellIds)
			{
				Enqueue(LandblockKey | ShortId);
			}
		}
	}

	constexpr int32 MaxVisit = 24;
	for (int32 Qi = 0; Qi < Queue.Num() && Qi < MaxVisit; ++Qi)
	{
		const uint32 FullId = Queue[Qi];
		if (TryCell(FullId))
		{
			return true;
		}
		if (const FACEBuiltEnvCellMesh* Current = FindEnvCellMesh(FullId, WorldScale))
		{
			for (const FACEDatCellPortal& Portal : Current->CellPortals)
			{
				if (!Portal.IsOutsidePortal())
				{
					Enqueue(LandblockKey | Portal.OtherCellId);
				}
			}
			for (uint16 ShortId : Current->VisibleCells)
			{
				Enqueue(LandblockKey | ShortId);
			}
		}
		else
		{
			RequestEnvCellMesh(FullId, WorldScale);
		}
	}

	// Indoor hint with an outside portal and no containment → outdoor landcell.
	if (bIndoorHint)
	{
		if (const FACEBuiltEnvCellMesh* Current = FindEnvCellMesh(HintCellId, WorldScale))
		{
			for (const FACEDatCellPortal& Portal : Current->CellPortals)
			{
				if (Portal.IsOutsidePortal())
				{
					return false;
				}
			}
		}
	}
	return false;
}

bool UACEDatSubsystem::EvaluateIdleMotion(uint32 MotionTableId, float TimeSeconds, int32 NumParts, TArray<FTransform>& OutPartTransforms, float WorldScale, int32& OutAnimatedPartCount,
	const float* PreviousTimeSeconds, TArray<FACEDatAnimationHook>* OutCrossedHooks) const
{
	OutAnimatedPartCount = 0;
	if (!bPortalLoaded || !MotionPlayer || MotionTableId == 0)
	{
		return false;
	}
	if (MotionPlayer->GetMotionTableId() != MotionTableId || !MotionPlayer->IsReady())
	{
		if (!MotionPlayer->SetMotionTable(MotionTableId))
		{
			return false;
		}
	}
	return MotionPlayer->EvaluateIdle(TimeSeconds, NumParts, OutPartTransforms, WorldScale, OutAnimatedPartCount,
		PreviousTimeSeconds, OutCrossedHooks);
}

bool UACEDatSubsystem::GetMotionVelocity(uint32 Table, uint32 Command, uint32 Style, FVector& Out) const
{
    return bPortalLoaded && MotionPlayer && MotionPlayer->SetMotionTable(Table)
        && MotionPlayer->GetCycleVelocity(Command, Style, Out);
}

bool UACEDatSubsystem::BuildCycleRootTrack(uint32 Table, uint32 Command, uint32 Style, ACELandingMotion::FRootTrack& Out, bool Reverse) const
{
    return bPortalLoaded && MotionPlayer && MotionPlayer->SetMotionTable(Table)
        && MotionPlayer->BuildCycleRootTrack(Command, Style, Out, Reverse);
}

bool UACEDatSubsystem::GetMotionTurnRate(uint32 Table, uint32 Style, float& OutRadiansPerSecond) const
{
    FVector Omega;
    if (!bPortalLoaded || !MotionPlayer || !MotionPlayer->SetMotionTable(Table)
        || !MotionPlayer->GetModifierOmega(ACEMotion::TurnRight, Style, Omega)) return false;
    // Preserve the sign: AC's TurnRight omega is negative. The actor flips
    // that axis when converting to Unreal, just as it does packet quaternions.
    OutRadiansPerSecond = Omega.Z;
    return true;
}

bool UACEDatSubsystem::EvaluateMotionCommand(uint32 MotionTableId, uint32 MotionCommand, float TimeSeconds, int32 NumParts, TArray<FTransform>& OutPartTransforms, float WorldScale, int32& OutAnimatedPartCount,
	const float* PreviousTimeSeconds, TArray<FACEDatAnimationHook>* OutCrossedHooks, uint32 PreferredStyle, bool bLoop, bool* bOutFinished) const
{
	OutAnimatedPartCount = 0;
	if (bOutFinished)
	{
		*bOutFinished = false;
	}
	if (!bPortalLoaded || !MotionPlayer || MotionTableId == 0)
	{
		return false;
	}
	if (MotionPlayer->GetMotionTableId() != MotionTableId || !MotionPlayer->IsReady())
	{
		if (!MotionPlayer->SetMotionTable(MotionTableId))
		{
			return false;
		}
	}
	return MotionPlayer->EvaluateMotion(MotionCommand, TimeSeconds, NumParts, OutPartTransforms, WorldScale, OutAnimatedPartCount,
		PreviousTimeSeconds, OutCrossedHooks, PreferredStyle, bLoop, bOutFinished);
}

bool UACEDatSubsystem::BuildTransitionRootTrack(uint32 Table, uint32 From, uint32 To, uint32 Style, ACELandingMotion::FRootTrack& Out) const
{
	Out.Reset();
	return bPortalLoaded && MotionPlayer && Table != 0 && MotionPlayer->SetMotionTable(Table)
		&& MotionPlayer->BuildTransitionRootTrack(From, To, Style, Out);
}

bool UACEDatSubsystem::EvaluateMotionTransition(uint32 Table, uint32 From, uint32 To, float Time, int32 Parts,
	TArray<FTransform>& Out, float Scale, int32& Count, bool& Finished,
	const float* Previous, TArray<FACEDatAnimationHook>* Hooks, uint32 Style) const
{
	Count = 0; Finished = false;
	return bPortalLoaded && MotionPlayer && Table != 0 && MotionPlayer->SetMotionTable(Table)
		&& MotionPlayer->EvaluateTransition(From, To, Time, Parts, Out, Scale, Count, Finished, Previous, Hooks, Style);
}

bool UACEDatSubsystem::BuildWalkingLink(uint32 Table, int8 From, int8 To, uint32 Style, ACEWalkingMotion::FLink& Out) const
{
	return bPortalLoaded && MotionPlayer && Table != 0 && MotionPlayer->SetMotionTable(Table)
		&& MotionPlayer->BuildWalkingLink(From, To, Style, Out);
}

bool UACEDatSubsystem::EvaluateWalkingLink(const ACEWalkingMotion::FLink& Link, int32 Parts,
	TArray<FTransform>& Out, float Scale, int32& Count, TArray<FACEDatAnimationHook>& Hooks) const
{
	return bPortalLoaded && MotionPlayer && MotionPlayer->EvaluateWalkingLink(Link, Parts, Out, Scale, Count, Hooks);
}

bool UACEDatSubsystem::EvaluateMotionLink(uint32 MotionTableId, uint32 FromCommand, uint32 ToCommand, float TimeSeconds, int32 NumParts, TArray<FTransform>& OutPartTransforms, float WorldScale, int32& OutAnimatedPartCount, bool& bOutFinished,
	const float* PreviousTimeSeconds, TArray<FACEDatAnimationHook>* OutCrossedHooks, uint32 PreferredStyle) const
{
	OutAnimatedPartCount = 0;
	bOutFinished = false;
	if (!bPortalLoaded || !MotionPlayer || MotionTableId == 0)
	{
		return false;
	}
	if (MotionPlayer->GetMotionTableId() != MotionTableId || !MotionPlayer->IsReady())
	{
		if (!MotionPlayer->SetMotionTable(MotionTableId))
		{
			return false;
		}
	}
	return MotionPlayer->EvaluateLink(FromCommand, ToCommand, TimeSeconds, NumParts, OutPartTransforms, WorldScale, OutAnimatedPartCount, bOutFinished,
		PreviousTimeSeconds, OutCrossedHooks, PreferredStyle);
}

bool UACEDatSubsystem::EvaluateStateMotion(uint32 Table, uint32 From, uint32 To, float Time, bool bStartInCycle, int32 Parts,
	TArray<FTransform>& Out, float Scale, int32& Count, bool& bTransitionFinished,
	const float* Previous, TArray<FACEDatAnimationHook>* Hooks) const
{
	Count = 0; bTransitionFinished = false;
	return bPortalLoaded && MotionPlayer && Table != 0 && MotionPlayer->SetMotionTable(Table)
		&& MotionPlayer->EvaluateStateMotion(From, To, Time, bStartInCycle, Parts, Out, Scale, Count, bTransitionFinished, Previous, Hooks);
}

bool UACEDatSubsystem::EvaluateAnimationLoop(uint32 AnimationId, float TimeSeconds, int32 NumParts, TArray<FTransform>& OutPartTransforms, float WorldScale, int32& OutAnimatedPartCount,
	const float* PreviousTimeSeconds, TArray<FACEDatAnimationHook>* OutCrossedHooks, float Framerate, int32 LowFrame) const
{
	OutAnimatedPartCount = 0;
	if (!bPortalLoaded || !MotionPlayer || AnimationId == 0)
	{
		return false;
	}
	return MotionPlayer->EvaluateAnimation(AnimationId, TimeSeconds, NumParts, OutPartTransforms, WorldScale, OutAnimatedPartCount, true,
		PreviousTimeSeconds, OutCrossedHooks, Framerate, LowFrame);
}

static TAutoConsoleVariable<int32> CVarACECacheSetupMetadata(
	TEXT("ace.Dat.CacheSetupMetadata"), 1,
	TEXT("Cache immutable Setup movement, collision and script metadata; avoids repeated DAT unpacking."));

const UACEDatSubsystem::FSetupRuntimeMetadata* UACEDatSubsystem::FindSetupRuntimeMetadata(uint32 SetupId)
{
	if (!bPortalLoaded || !PortalDat || SetupId == 0) return nullptr;
	const bool bCache = CVarACECacheSetupMetadata.GetValueOnGameThread() != 0;
	if (bCache)
		if (auto* Cached = SetupRuntimeMetadataCache.Find(SetupId))
		{
			Cached->LastUse = ++SetupRuntimeMetadataUse;
			return Cached->bValid ? Cached : nullptr;
		}
	FSetupRuntimeMetadata Value;
	TArray<uint8> Blob;
	if (PortalDat->ReadFile(SetupId, Blob))
	{
		FACEDatCursor Cursor(Blob);
		FACEDatSetupModel Setup;
		if (ACEDatUnpack::UnpackSetupModel(Cursor, Setup))
		{
			Value.bValid = true;
			for (const auto& Point : Setup.Lights)
				if (Point.Value.Intensity > 0.f && Point.Value.Falloff > 0.f && (Point.Value.Color & 0xFFFFFFu))
				{
					Value.bHasPointLight = true;
					break;
				}
			Value.StepUp = Setup.StepUpHeight > 0.f ? Setup.StepUpHeight : .5f;
			Value.StepDown = Setup.StepDownHeight > 0.f ? Setup.StepDownHeight : .5f;
			Value.Height = Setup.Height; Value.Radius = Setup.Radius;
			Value.SelectionOrigin = Setup.SelectionSphereOrigin;
			Value.SelectionRadius = Setup.SelectionSphereRadius;
			Value.Animation = Setup.DefaultAnimation; Value.Script = Setup.DefaultScript;
			Value.ScriptTable = Setup.DefaultScriptTable; Value.SoundTable = Setup.DefaultSoundTable;
			Value.bPhysicsBSP = EnumHasAnyFlags(Setup.Flags, EACESetupFlags::HasPhysicsBSP);
			// Retail prefers cylinder-spheres when available, otherwise spheres.
			Value.MovementSpheres=Setup.Spheres;
			Value.Shapes = Setup.CylSpheres.IsEmpty() ? MoveTemp(Setup.Spheres) : MoveTemp(Setup.CylSpheres);
		}
	}
	if (!bCache)
	{
		UncachedSetupRuntimeMetadata = MoveTemp(Value);
		return UncachedSetupRuntimeMetadata.bValid ? &UncachedSetupRuntimeMetadata : nullptr;
	}
	if (SetupRuntimeMetadataCache.Num() >= 512)
	{
		uint32 OldestKey = 0; uint64 OldestUse = MAX_uint64;
		for (const auto& Entry : SetupRuntimeMetadataCache)
			if (Entry.Value.LastUse < OldestUse)
			{ OldestKey = Entry.Key; OldestUse = Entry.Value.LastUse; }
		SetupRuntimeMetadataCache.Remove(OldestKey);
	}
	Value.LastUse = ++SetupRuntimeMetadataUse;
	auto& Cached = SetupRuntimeMetadataCache.Add(SetupId, MoveTemp(Value));
	return Cached.bValid ? &Cached : nullptr;
}

bool UACEDatSubsystem::GetSetupCollisionShapes(uint32 SetupId, TArray<FACEDatCollisionShape>& OutShapes, bool& bHasPhysicsBSP)
{
	OutShapes.Reset(); bHasPhysicsBSP = false;
	const auto* Data = FindSetupRuntimeMetadata(SetupId);
	if (!Data) return false;
	OutShapes = Data->Shapes; bHasPhysicsBSP = Data->bPhysicsBSP;
	return true;
}

bool UACEDatSubsystem::GetSetupMovementSpheres(uint32 SetupId, TArray<FACEDatCollisionShape>& OutSpheres)
{
	OutSpheres.Reset();
	const auto* Data=FindSetupRuntimeMetadata(SetupId);
	if (!Data)return false;
	OutSpheres=Data->MovementSpheres;
	return true;
}

bool UACEDatSubsystem::TryGetSetupPhysics(uint32 SetupId, float& OutStepUpHeight, float& OutHeight, float& OutRadius, uint32& OutDefaultAnimationId, float* OutStepDownHeight,
	FVector3f* OutSelectionOriginAc, float* OutSelectionRadiusAc)
{
	OutStepUpHeight = .5f; OutHeight = 2.f; OutRadius = .5f; OutDefaultAnimationId = 0;
	if (OutStepDownHeight) *OutStepDownHeight = .5f;
	if (OutSelectionOriginAc) *OutSelectionOriginAc = FVector3f::ZeroVector;
	if (OutSelectionRadiusAc) *OutSelectionRadiusAc = 0.f;
	const auto* Data = FindSetupRuntimeMetadata(SetupId);
	if (!Data) return false;
	OutStepUpHeight = Data->StepUp; OutHeight = Data->Height; OutRadius = Data->Radius;
	OutDefaultAnimationId = Data->Animation;
	if (OutStepDownHeight) *OutStepDownHeight = Data->StepDown;
	if (OutSelectionOriginAc) *OutSelectionOriginAc = Data->SelectionOrigin;
	if (OutSelectionRadiusAc) *OutSelectionRadiusAc = Data->SelectionRadius;
	return true;
}

bool UACEDatSubsystem::TryGetSetupRuntimeMetadata(
	uint32 SetupId, uint32& OutDefaultScriptId, uint32& OutScriptTableId, uint32& OutSoundTableId, bool* OutHasPointLight)
{
	OutDefaultScriptId = OutScriptTableId = OutSoundTableId = 0;
	const auto* Data = FindSetupRuntimeMetadata(SetupId);
	if (OutHasPointLight) *OutHasPointLight = Data && Data->bHasPointLight;
	if (!Data) return false;
	OutDefaultScriptId = Data->Script;
	OutScriptTableId = Data->ScriptTable;
	OutSoundTableId = Data->SoundTable;
	return true;
}

template <typename T>
static const T* LoadPortalDatAsset(
	FACEDatDatabase* PortalDat,
	uint32 Id,
	TMap<uint32, T>& Cache,
	bool (*Unpack)(FACEDatCursor&, T&),
	const TCHAR* TypeName)
{
	if (!PortalDat || Id == 0)
	{
		return nullptr;
	}
	if (const T* Existing = Cache.Find(Id))
	{
		return Existing;
	}

	TArray<uint8> Blob;
	if (!PortalDat->ReadFile(Id, Blob))
	{
		return nullptr;
	}
	FACEDatCursor Cur(Blob);
	T Parsed;
	if (!Unpack(Cur, Parsed))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: failed to unpack %s 0x%08X at byte %d/%d"),
			TypeName, Id, Cur.Pos, Cur.Size);
		return nullptr;
	}
	return &Cache.Add(Id, MoveTemp(Parsed));
}

const FACEDatParticleEmitterInfo* UACEDatSubsystem::GetParticleEmitterInfo(uint32 Id)
{
	if (!EnsureLoaded())
	{
		return nullptr;
	}
	return LoadPortalDatAsset(PortalDat.Get(), Id, ParticleEmitterInfoCache,
		&ACEDatUnpack::UnpackParticleEmitterInfo, TEXT("ParticleEmitterInfo"));
}

bool UACEDatSubsystem::TryEstimateGfxLight(uint32 GfxObjId, FLinearColor& OutColor, float& OutLuminosity)
{
	OutColor = FLinearColor::White;
	OutLuminosity = 0.f;
	if (GfxObjId == 0 || !Builder || !TextureResolver)
	{
		return false;
	}
	if (const FGfxLightEstimate* Cached = GfxLightEstimateCache.Find(GfxObjId))
	{
		OutColor = Cached->Color;
		OutLuminosity = Cached->Luminosity;
		return Cached->bEstimated;
	}
	FACEDatGfxObj Gfx;
	if (!Builder->LoadGfxObj(GfxObjId, Gfx) || Gfx.Surfaces.Num() == 0)
	{
		return false;
	}
	FACEDatDecodedSurface Decoded;
	if (!TextureResolver->ResolveSurface(Gfx.Surfaces[0], Decoded))
	{
		return false;
	}
	// ResolveSurface uses the DAT's default palette, with no per-object overrides.
	// Cache only completed decodes; unavailable assets must remain retryable.
	const auto CacheEstimate = [this, GfxObjId, &OutColor, &OutLuminosity](bool bEstimated)
	{
		GfxLightEstimateCache.Add(GfxObjId, FGfxLightEstimate{OutColor, OutLuminosity, bEstimated});
		return bEstimated;
	};
	OutLuminosity = Decoded.Luminosity;
	if (Decoded.bIsSolid)
	{
		OutColor = Decoded.SolidColor;
	}
	else if (Decoded.bHasPixels && Decoded.Pixels.Num() > 0)
	{
		double R = 0.0, G = 0.0, B = 0.0, W = 0.0;
		for (const FColor& P : Decoded.Pixels)
		{
			const float A = static_cast<float>(P.A) / 255.f;
			const float L = static_cast<float>(FMath::Max3(P.R, P.G, P.B)) / 255.f;
			const float Wt = FMath::Max(A, L);
			if (Wt < 0.04f)
			{
				continue;
			}
			R += static_cast<double>(P.R) * Wt;
			G += static_cast<double>(P.G) * Wt;
			B += static_cast<double>(P.B) * Wt;
			W += Wt;
		}
		if (W <= KINDA_SMALL_NUMBER)
		{
			return CacheEstimate(false);
		}
		OutColor = FLinearColor(
			static_cast<float>(R / (W * 255.0)),
			static_cast<float>(G / (W * 255.0)),
			static_cast<float>(B / (W * 255.0)),
			1.f);
	}
	else
	{
		return CacheEstimate(false);
	}
	OutColor.A = 1.f;
	const float Lum = OutColor.GetLuminance();
	// Luminous means self-lit, not necessarily light-emitting. Black mesh
	// overlays (e.g. Quintessence Sickle) must never claim the source's light
	// ahead of its colored sparks merely because they are nearer the camera.
	if (Lum <= KINDA_SMALL_NUMBER || (Lum < 0.08f && OutLuminosity < 0.2f))
	{
		OutLuminosity = 0.f;
		return CacheEstimate(false);
	}
	if (Lum > 0.02f)
	{
		FLinearColor Hsv = OutColor.LinearRGBToHSV();
		Hsv.G = FMath::Clamp(Hsv.G * 1.35f, 0.f, 1.f);
		OutColor = Hsv.HSVToLinearRGB();
		OutColor.A = 1.f;
	}
	// Effect semantics belong to the caller: red fire/buffs can share this color.
	return CacheEstimate(true);
}

const FACEDatPhysicsScript* UACEDatSubsystem::GetPhysicsScript(uint32 Id)
{
	if (!EnsureLoaded())
	{
		return nullptr;
	}
	return LoadPortalDatAsset(PortalDat.Get(), Id, PhysicsScriptCache,
		&ACEDatUnpack::UnpackPhysicsScript, TEXT("PhysicsScript"));
}

const FACEDatPhysicsScriptTable* UACEDatSubsystem::GetPhysicsScriptTable(uint32 Id)
{
	if (!EnsureLoaded())
	{
		return nullptr;
	}
	return LoadPortalDatAsset(PortalDat.Get(), Id, PhysicsScriptTableCache,
		&ACEDatUnpack::UnpackPhysicsScriptTable, TEXT("PhysicsScriptTable"));
}

const FACEDatWave* UACEDatSubsystem::GetWave(uint32 Id)
{
	if (!EnsureLoaded())
	{
		return nullptr;
	}
	const FACEDatWave* Wave = LoadPortalDatAsset(PortalDat.Get(), Id, WaveCache,
		&ACEDatUnpack::UnpackWave, TEXT("Wave"));
	if (!Wave && HighResDat)
	{
		Wave = LoadPortalDatAsset(HighResDat.Get(), Id, WaveCache,
			&ACEDatUnpack::UnpackWave, TEXT("Wave"));
	}
	return Wave;
}

const FACEDatSoundTable* UACEDatSubsystem::GetSoundTable(uint32 Id)
{
	if (!EnsureLoaded())
	{
		return nullptr;
	}
	return LoadPortalDatAsset(PortalDat.Get(), Id, SoundTableCache,
		&ACEDatUnpack::UnpackSoundTable, TEXT("SoundTable"));
}

bool UACEDatSubsystem::EnsureSpellTableLoaded()
{
	if (bSpellTableLoaded)
	{
		return true;
	}
	if (!EnsureLoaded() || !PortalDat)
	{
		return false;
	}
	TArray<uint8> Blob;
	if (!PortalDat->ReadFile(0x0E00000Eu, Blob))
	{
		bSpellTableLoaded = true; // avoid retry spam
		return false;
	}
	FACEDatCursor Cur(Blob);
	bool bOk = true;
	Cur.ReadU32(bOk); // file Id
	const uint16 Count = Cur.ReadU16(bOk);
	Cur.ReadU16(bOk); // bucket size
	for (uint16 i = 0; i < Count && bOk; ++i)
	{
		const uint32 Key = Cur.ReadU32(bOk);
		FSpellInfoCacheEntry Entry;
		Entry.Name = Cur.ReadObfuscatedString(bOk);
		Cur.AlignBoundary();
		Entry.Description = Cur.ReadObfuscatedString(bOk);
		Cur.AlignBoundary();
		Entry.School = Cur.ReadU32(bOk);
		Entry.IconDid = Cur.ReadU32(bOk);
		Entry.Category = Cur.ReadU32(bOk); // Category
		Entry.Bitfield = Cur.ReadU32(bOk);
		Entry.BaseMana = Cur.ReadU32(bOk);
		Entry.BaseRange = Cur.ReadF32(bOk);
		Entry.RangeMod = Cur.ReadF32(bOk);
		Entry.Power = Cur.ReadU32(bOk);
		Cur.ReadF32(bOk);
		Entry.FormulaVersion = Cur.ReadU32(bOk);
		Cur.ReadF32(bOk);
		const uint32 MetaSpellType = Cur.ReadU32(bOk);
		Entry.bProjectile = MetaSpellType == 2 || MetaSpellType == 10 || MetaSpellType == 15;
		Cur.ReadU32(bOk);
		if (MetaSpellType == 1u || MetaSpellType == 12u) // Enchantment / FellowEnchantment
		{
			double Duration = 0.0;
			bOk = bOk && Cur.Read(Duration);
			Entry.Duration = Duration;
			Cur.ReadF32(bOk);
			Cur.ReadF32(bOk);
		}
		else if (MetaSpellType == 7u) // PortalSummon
		{
			double PortalLifetime = 0.0;
			bOk = bOk && Cur.Read(PortalLifetime);
		}
		uint32 Formula[8];
		for (auto& Component : Formula) Component = Cur.ReadU32(bOk);
		// CompositeSpellIcon uses the formula's first component, not spell
		// difficulty (item and special spells can have unrelated difficulty).
		auto FormulaHash = [](const FString& Text)
		{
			uint32 Hash = 0;
			// ReadObfuscatedString preserves each decoded DAT byte as a TCHAR.
			for (TCHAR C : Text)
			{
				Hash = (Hash << 4) + static_cast<int8>(static_cast<uint8>(C));
				const uint32 High = Hash & 0xF0000000u;
				if (High) Hash = (Hash ^ (High >> 24)) & 0x0FFFFFFFu;
			}
			return Hash;
		};
		const uint32 FormulaKey = FormulaHash(Entry.Name) % 0x12107680u + FormulaHash(Entry.Description) % 0xBEADCF45u;
		for (auto& Component : Formula)
			if (Component) { Component -= FormulaKey; if (Component > 198) Component &= 0xFF; }
		for (uint32 Component : Formula) { if (!Component) break; Entry.Formula.Add(Component); }
		if (Formula[0])
		{
			const uint32 Component = Formula[0];
			Entry.IconPowerLevel = Component <= 6 ? Component : Component == 110 ? 7 : Component == 112 ? 8 : Component == 192 ? 9 : Component == 193 ? 10 : 0;
		}
		// SpellFormula::Complete / GetTargetingType and SpellComponentTable's
		// talisman mapping, not the narrower server NonComponentTargetType.
		if (Formula[0] && Formula[1] && Formula[2] && Formula[3] && Formula[4])
		{
			int32 Last = 4;
			while (Last < 7 && Formula[Last + 1]) ++Last;
			const uint32 Talisman = Formula[Last];
			Entry.RetailTargetType = (Talisman >= 0x31 && Talisman <= 0x38)
				|| (Talisman >= 0x3C && Talisman <= 0x3E) || Talisman == 0xBE ? 16u
				: Talisman == 0x39 ? 560015u : Talisman == 0x3B ? 268500992u : 0u;
		}
		Cur.ReadU32(bOk); // CasterEffect
		Cur.ReadU32(bOk); // TargetEffect
		Cur.ReadU32(bOk); // FizzleEffect
		{
			double RecoveryInterval = 0.0;
			bOk = bOk && Cur.Read(RecoveryInterval);
		}
		Cur.ReadF32(bOk); // RecoveryAmount
		Entry.DisplayOrder = Cur.ReadU32(bOk); // DisplayOrder
		Entry.NonComponentTargetType = Cur.ReadU32(bOk);
		Cur.ReadU32(bOk); // ManaMod
		if (bOk)
		{
			SpellInfoCache.Add(Key, MoveTemp(Entry));
		}
	}
	bSpellTableLoaded = true;
	UE_LOG(LogTemp, Log, TEXT("ACEDat: SpellTable loaded (%d spells)"), SpellInfoCache.Num());
	return true;
}

bool UACEDatSubsystem::TryGetPluginSpellInfo(uint32 Id, uint32& School, uint32& Power, uint32& Category, uint32& Flags, double& Duration)
{
    if (!EnsureSpellTableLoaded()) return false;
    const auto* Entry = SpellInfoCache.Find(Id); if (!Entry) return false;
    School=Entry->School; Power=Entry->Power; Category=Entry->Category; Flags=Entry->Bitfield; Duration=Entry->Duration;
    // EnchantmentProjectile does not serialize its duration in portal.dat.
    // VT MySpell.Duration supplies these family durations for its effect tracker.
    if(Duration<=0){if(Category==636||Category==638)Duration=31.;else if(Category==637)Duration=16.;}
    return true;
}

bool UACEDatSubsystem::IsPluginSingleTargetOffensiveSpell(uint32 SpellId)
{
    if(!EnsureSpellTableLoaded())return false;
    const auto* E=SpellInfoCache.Find(SpellId);
    // VT excludes multi-target formulae (component 110), Corrosion, and untargeted
    // self-centred spells such as Tugak from per-monster failure accounting.
    return E && !(E->Bitfield&(4|8)) && E->RetailTargetType && E->Category!=638
        && !E->Formula.IsEmpty() && E->Formula[0]!=110;
}

bool UACEDatSubsystem::TryGetSpellInfo(uint32 SpellId, FString& OutName, uint32& OutIconDid)
{
	OutName.Reset();
	OutIconDid = 0;
	if (SpellId == 0)
	{
		return false;
	}
	EnsureSpellTableLoaded();
	if (const FSpellInfoCacheEntry* Found = SpellInfoCache.Find(SpellId))
	{
		OutName = Found->Name;
		OutIconDid = Found->IconDid;
		return !OutName.IsEmpty() || OutIconDid != 0;
	}
	return false;
}

bool UACEDatSubsystem::TryGetSpellDescription(uint32 SpellId, FString& OutDescription)
{
    OutDescription.Reset();
    EnsureSpellTableLoaded();
    if (const FSpellInfoCacheEntry* Found = SpellInfoCache.Find(SpellId))
    {
        OutDescription = Found->Description;
        return !OutDescription.IsEmpty();
    }
    return false;
}

bool UACEDatSubsystem::TryGetSpellExamination(uint32 SpellId, FString& OutDetails)
{
	OutDetails.Reset(); EnsureSpellTableLoaded();
	const auto* Spell = SpellInfoCache.Find(SpellId);
	if (!Spell) return false;
	static const TCHAR* Schools[] = {TEXT("Unknown"), TEXT("War Magic"), TEXT("Life Magic"), TEXT("Item Enchantment"), TEXT("Creature Enchantment"), TEXT("Void Magic")};
	OutDetails = FString::Printf(TEXT("%s\nDifficulty: %u\nBase mana: %u"), Schools[FMath::Min(Spell->School, 5u)], Spell->Power, Spell->BaseMana);
	if (Spell->Bitfield & 8u) OutDetails += TEXT("\nTarget: Self");
	else if (Spell->BaseRange > 0.f) OutDetails += FString::Printf(TEXT("\nRange: %.1f m + %.3f m per skill point"), Spell->BaseRange, Spell->RangeMod);
	if (Spell->Duration > 0.0) OutDetails += FString::Printf(TEXT("\nBase duration: %.0f seconds"), Spell->Duration);
	OutDetails += TEXT("\n\n") + Spell->Description;
	FString Account;
	const bool HasFocus = Spell->School < 32 && (GetPlayerSpellFocusMask() & (1u << Spell->School)) != 0;
	if(auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>())
		if(const auto Session=Client->GetSession())Account=Session->GetAccountName();
	const auto Formula=HasFocus?ACESpellFormula::WithFocus(Spell->Formula)
		:ACESpellFormula::Customize(Spell->Formula,Spell->FormulaVersion,Account);
	TArray<uint32> Order;TMap<uint32,int32> Counts;
	for(uint32 Component:Formula){if(!Counts.Contains(Component))Order.Add(Component);++Counts.FindOrAdd(Component);}
	TArray<FString> Names;
	for(uint32 Id:Order)if(const auto* Component=GetSpellComponents().FindByPredicate([&](const auto& C){return C.ComponentId==Id;}))
		Names.Add(Counts[Id]>1?FString::Printf(TEXT("%s (%d)"),*Component->Name,Counts[Id]):Component->Name);
	if(!Names.IsEmpty())OutDetails+=TEXT("\n\nIngredients: ")+FString::Join(Names,TEXT(", "));
	return true;
}

uint32 UACEDatSubsystem::GetPlayerSpellFocusMask() const
{
	const auto* GI = GetGameInstance();
	const auto* Client = GI ? GI->GetSubsystem<UACEClientSubsystem>() : nullptr;
	if (!Client) return 0;
	static const uint32 FocusWcid[] = {0,15271,15270,15269,15268,43173};
	static const uint32 FocusAug[] = {0,297,296,295,294,328};
	uint32 Mask = 0;
	const auto& Vitals = Client->GetPlayerVitalsView();
	const auto Packs = Client->GetPlayerPacks();
	for (uint32 School = 1; School < UE_ARRAY_COUNT(FocusWcid); ++School)
	{
		bool Focus = Vitals.StatQualityInts.FindRef(FocusAug[School]) > 0;
		// MagicPackIsOwned accepts the player's pack slots, not nested bags.
		for (const auto& Pack : Packs) if (uint32(Pack.WeenieClassId) == FocusWcid[School]) Focus = true;
		if (Focus) Mask |= 1u << School;
	}
	return Mask;
}

bool UACEDatSubsystem::TryGetPluginSpellComponents(uint32 SpellId, bool bHasFocus, TMap<FString, int32>& OutRequirements)
{
	OutRequirements.Reset(); EnsureSpellTableLoaded();
	const auto* Spell = SpellInfoCache.Find(SpellId); if (!Spell) return false;
	FString Account;
	if (auto* GI = GetGameInstance()) if (auto* Client = GI->GetSubsystem<UACEClientSubsystem>())
		if (const auto Session = Client->GetSession()) Account = Session->GetAccountName();
	const auto Formula = bHasFocus ? ACESpellFormula::WithFocus(Spell->Formula)
		: ACESpellFormula::Customize(Spell->Formula,Spell->FormulaVersion,Account);
	const auto& Components = GetSpellComponents();
	for (uint32 Id : Formula)
	{
		if (!Id) continue;
		const auto* Component = Components.FindByPredicate([&](const auto& C){return C.ComponentId == Id;});
		if (!Component) return false;
		++OutRequirements.FindOrAdd(Component->Name);
	}
	return true;
}

bool UACEDatSubsystem::TryGetPluginSpellScarabs(uint32 SpellId, TMap<FString, int32>& OutRequirements)
{
	OutRequirements.Reset();EnsureSpellTableLoaded();
	const auto* Spell=SpellInfoCache.Find(SpellId);if(!Spell)return false;
	const auto& Components=GetSpellComponents();
	for(uint32 Id:Spell->Formula)
	{
		if(!Id)continue;
		const auto* Component=Components.FindByPredicate([&](const auto& C){return C.ComponentId==Id;});
		if(!Component)return false;
		if(Component->Type==1)++OutRequirements.FindOrAdd(Component->Name);
	}
	return true;
}

bool UACEDatSubsystem::TryGetSpellDisplayOrder(uint32 SpellId, uint32& OutDisplayOrder)
{
	OutDisplayOrder = 0;
	if (SpellId == 0)
	{
		return false;
	}
	EnsureSpellTableLoaded();
	if (const FSpellInfoCacheEntry* Found = SpellInfoCache.Find(SpellId))
	{
		OutDisplayOrder = Found->DisplayOrder;
		return true;
	}
	return false;
}

bool UACEDatSubsystem::TryGetSpellSchoolAndLevel(uint32 SpellId, uint32& OutSchool, uint32& OutLevel)
{
	OutSchool = 0;
	OutLevel = 0;
	if (SpellId == 0)
	{
		return false;
	}
	EnsureSpellTableLoaded();
	if (const FSpellInfoCacheEntry* Found = SpellInfoCache.Find(SpellId))
	{
		OutSchool = Found->School;
		// CSpellBase::InqSpellLevelByRoughHeuristic uses the decoded formula's
		// power component. Difficulty is unrelated for many special/item spells.
		const uint32 PowerLevel = Found->IconPowerLevel;
		OutLevel = PowerLevel >= 9 ? PowerLevel - 2 : PowerLevel >= 7 ? PowerLevel - 1 : PowerLevel;
		return true;
	}
	return false;
}

uint32 UACEDatSubsystem::GetSpellIconPowerLevel(uint32 SpellId)
{
	EnsureSpellTableLoaded();
	const auto* Entry = SpellInfoCache.Find(SpellId);
	return Entry ? Entry->IconPowerLevel : 0;
}

bool UACEDatSubsystem::TryGetSpellTargeting(uint32 SpellId, uint32& OutBitfield, uint32& OutNonComponentTargetType)
{
	OutBitfield = 0;
	OutNonComponentTargetType = 0;
	if (SpellId == 0)
	{
		return false;
	}
	EnsureSpellTableLoaded();
	if (const FSpellInfoCacheEntry* Found = SpellInfoCache.Find(SpellId))
	{
		OutBitfield = Found->Bitfield;
		OutNonComponentTargetType = Found->NonComponentTargetType;
		return true;
	}
	return false;
}

bool UACEDatSubsystem::TryGetRetailSpellTargeting(uint32 SpellId, uint32& OutBitfield, uint32& OutTargetType, bool& OutProjectile)
{
	OutBitfield = OutTargetType = 0; OutProjectile = false;
	EnsureSpellTableLoaded();
	if (const auto* Spell = SpellInfoCache.Find(SpellId))
	{
		OutBitfield = Spell->Bitfield; OutTargetType = Spell->RetailTargetType; OutProjectile = Spell->bProjectile;
		return true;
	}
	return false;
}

bool UACEDatSubsystem::EnsureSpellComponentTableLoaded()
{
	if (bSpellComponentTableLoaded)
	{
		return SpellComponentCache.Num() > 0;
	}
	if (!EnsureLoaded() || !PortalDat)
	{
		return false;
	}
	bSpellComponentTableLoaded = true;

	// DualDidMapper 0x27000002: client enum (component id) → weenie class id.
	TMap<uint32, uint32> ComponentToWcid;
	TArray<uint8> MapBlob;
	if (PortalDat->ReadFile(0x27000002u, MapBlob))
	{
		FACEDatCursor Cur(MapBlob);
		bool bOk = true;
		Cur.ReadU32(bOk); // file Id
		Cur.ReadU8(bOk);  // ClientIDNumberingType
		const uint32 Count = Cur.ReadCompressedUInt32(bOk);
		for (uint32 i = 0; i < Count && bOk; ++i)
		{
			const uint32 ComponentId = Cur.ReadU32(bOk);
			const uint32 Wcid = Cur.ReadU32(bOk);
			if (bOk)
			{
				ComponentToWcid.Add(ComponentId, Wcid);
			}
		}
	}

	TArray<uint8> Blob;
	if (!PortalDat->ReadFile(0x0E00000Fu, Blob))
	{
		return false;
	}
	FACEDatCursor Cur(Blob);
	bool bOk = true;
	Cur.ReadU32(bOk); // file Id
	const uint16 NumComps = Cur.ReadU16(bOk);
	Cur.AlignBoundary();
	SpellComponentCache.Reserve(NumComps);
	for (uint16 i = 0; i < NumComps && bOk; ++i)
	{
		FACESpellComponentInfo Info;
		Info.ComponentId = Cur.ReadU32(bOk);
		Info.Name = Cur.ReadObfuscatedString(bOk);
		Cur.AlignBoundary();
		Info.Category = Cur.ReadU32(bOk);
		Info.IconDid = Cur.ReadU32(bOk);
		Info.Type = Cur.ReadU32(bOk);
		Cur.ReadU32(bOk); // Gesture
		Cur.ReadF32(bOk); // Time
		Cur.ReadObfuscatedString(bOk); // spell word Text
		Cur.AlignBoundary();
		Cur.ReadF32(bOk); // CDM
		if (!bOk)
		{
			break;
		}
		if (const uint32* Wcid = ComponentToWcid.Find(Info.ComponentId))
		{
			Info.Wcid = *Wcid;
		}
		SpellComponentCache.Add(MoveTemp(Info));
	}
	SpellComponentCache.Sort([](const FACESpellComponentInfo& A, const FACESpellComponentInfo& B)
	{
		if (A.Type != B.Type)
		{
			return A.Type < B.Type;
		}
		return A.Name < B.Name;
	});
	UE_LOG(LogTemp, Log, TEXT("ACEDat: SpellComponentTable loaded (%d components, %d mapped wcids)"),
		SpellComponentCache.Num(), ComponentToWcid.Num());
	return SpellComponentCache.Num() > 0;
}

const TArray<FACESpellComponentInfo>& UACEDatSubsystem::GetSpellComponents()
{
	EnsureSpellComponentTableLoaded();
	return SpellComponentCache;
}

bool UACEDatSubsystem::EnsureContractTableLoaded()
{
	if (bContractTableLoaded)
	{
		return ContractCache.Num() > 0;
	}
	if (!EnsureLoaded() || !PortalDat)
	{
		return false;
	}
	bContractTableLoaded = true;
	TArray<uint8> Blob;
	if (!PortalDat->ReadFile(0x0E00001Du, Blob))
	{
		return false;
	}
	FACEDatCursor Cur(Blob);
	bool bOk = true;
	Cur.ReadU32(bOk); // file Id
	const uint16 NumContracts = Cur.ReadU16(bOk);
	Cur.ReadU16(bOk); // table size
	auto ReadAlignedPString = [&Cur, &bOk]() -> FString
	{
		FString S = Cur.ReadPString(bOk);
		Cur.AlignBoundary();
		return S;
	};
	// Position = ObjCellID + Frame (origin + quaternion).
	auto ReadPosition = [&Cur](uint32& OutCell, FVector3f& OutOrigin) -> bool
	{
		bool bLocalOk = true;
		OutCell = Cur.ReadU32(bLocalOk);
		FQuat4f Rot;
		return bLocalOk && Cur.ReadFrame(OutOrigin, Rot);
	};
	for (uint16 i = 0; i < NumContracts && bOk; ++i)
	{
		const uint32 Key = Cur.ReadU32(bOk);
		FACEDatContractInfo Info;
		Cur.ReadU32(bOk); // Version
		Info.ContractId = Cur.ReadU32(bOk);
		Info.Name = ReadAlignedPString();
		Info.Description = ReadAlignedPString();
		Info.DescriptionProgress = ReadAlignedPString();
		Info.NameNPCStart = ReadAlignedPString();
		Info.NameNPCEnd = ReadAlignedPString();
		for (int32 Flag = 0; Flag < 5; ++Flag)
		{
			ReadAlignedPString(); // questflag stamped/started/finished/progress/timer
		}
		Info.QuestFlagRepeatTime = ReadAlignedPString();
		uint32 EndCell = 0;
		FVector3f EndOrigin = FVector3f::ZeroVector;
		bOk = bOk && ReadPosition(Info.CellNPCStart, Info.OriginNPCStart)
			&& ReadPosition(EndCell, EndOrigin)
			&& ReadPosition(Info.CellQuestArea, Info.OriginQuestArea);
		if (!bOk)
		{
			break;
		}
		if (Info.ContractId == 0)
		{
			Info.ContractId = Key;
		}
		ContractCache.Add(Key, MoveTemp(Info));
	}
	UE_LOG(LogTemp, Log, TEXT("ACEDat: ContractTable loaded (%d contracts)"), ContractCache.Num());
	return ContractCache.Num() > 0;
}

const FACEDatContractInfo* UACEDatSubsystem::GetContractInfo(uint32 ContractId)
{
	EnsureContractTableLoaded();
	return ContractCache.Find(ContractId);
}

bool UACEDatSubsystem::EnsureChatPoseTableLoaded()
{
	if (bChatPoseTableLoaded)
	{
		return ChatPoseCache.Num() > 0;
	}
	if (!EnsureLoaded() || !PortalDat)
	{
		return false;
	}
	TArray<uint8> Blob;
	if (!PortalDat->ReadFile(0x0E000007u, Blob))
	{
		bChatPoseTableLoaded = true;
		return false;
	}
	FACEDatCursor Cur(Blob);
	bool bOk = true;
	Cur.ReadU32(bOk); // file Id
	const uint16 PoseCount = Cur.ReadU16(bOk);
	Cur.ReadU16(bOk); // bucket
	TMap<FString, FString> PoseToCommand;
	for (uint16 i = 0; i < PoseCount && bOk; ++i)
	{
		const FString Key = Cur.ReadPString(bOk);
		Cur.AlignBoundary();
		const FString Value = Cur.ReadPString(bOk);
		Cur.AlignBoundary();
		if (bOk && !Key.IsEmpty())
		{
			PoseToCommand.Add(Key.ToLower(), Value);
		}
	}
	const uint16 EmoteCount = Cur.ReadU16(bOk);
	Cur.ReadU16(bOk);
	TMap<FString, TPair<FString, FString>> CommandToEmotes;
	for (uint16 i = 0; i < EmoteCount && bOk; ++i)
	{
		const FString Key = Cur.ReadPString(bOk);
		Cur.AlignBoundary();
		const FString MyEmote = Cur.ReadPString(bOk);
		Cur.AlignBoundary();
		const FString OtherEmote = Cur.ReadPString(bOk);
		Cur.AlignBoundary();
		if (bOk && !Key.IsEmpty())
		{
			CommandToEmotes.Add(Key.ToLower(), TPair<FString, FString>(MyEmote, OtherEmote));
		}
	}
	for (const TPair<FString, FString>& Pair : PoseToCommand)
	{
		FChatPoseEntry Entry;
		Entry.Command = Pair.Value;
		const FString CmdKey = Pair.Value.ToLower();
		if (const TPair<FString, FString>* Em = CommandToEmotes.Find(CmdKey))
		{
			Entry.MyEmote = Em->Key;
			Entry.OtherEmote = Em->Value;
		}
		ChatPoseCache.Add(Pair.Key, MoveTemp(Entry));
	}
	bChatPoseTableLoaded = true;
	UE_LOG(LogTemp, Log, TEXT("ACEDat: ChatPoseTable loaded (%d poses)"), ChatPoseCache.Num());
	return ChatPoseCache.Num() > 0;
}

bool UACEDatSubsystem::TryGetChatPose(const FString& PoseKey, FString& OutCommand, FString& OutMyEmote, FString& OutOtherEmote)
{
	OutCommand.Reset();
	OutMyEmote.Reset();
	OutOtherEmote.Reset();
	if (PoseKey.IsEmpty())
	{
		return false;
	}
	EnsureChatPoseTableLoaded();
	if (const FChatPoseEntry* Found = ChatPoseCache.Find(PoseKey.ToLower()))
	{
		OutCommand = Found->Command;
		OutMyEmote = Found->MyEmote;
		OutOtherEmote = Found->OtherEmote;
		return true;
	}
	return false;
}

void UACEDatSubsystem::GetChatPoseKeys(TArray<FString>& OutKeys)
{
	OutKeys.Reset();
	EnsureChatPoseTableLoaded();
	ChatPoseCache.GetKeys(OutKeys);
}

bool UACEDatSubsystem::EnsureSkillTableLoaded()
{
	if (!bSkillTableLoaded)
	{
		if (!EnsureLoaded() || !PortalDat)
		{
			return false;
		}
		TArray<uint8> Blob;
		if (!PortalDat->ReadFile(0x0E000004u, Blob))
		{
			bSkillTableLoaded = true; // avoid retry spam
			return false;
		}
		FACEDatCursor Cur(Blob);
		bool bOk = true;
		if (Cur.ReadU32(bOk) != 0x0E000004u || !bOk)
		{
			bSkillTableLoaded = true;
			return false;
		}
		const uint16 Count = Cur.ReadU16(bOk);
		Cur.ReadU16(bOk); // bucket size
		for (uint16 i = 0; i < Count && bOk; ++i)
		{
			const uint32 Key = Cur.ReadU32(bOk);
			FSpellInfoCacheEntry Entry;
			Entry.Description = Cur.ReadPString(bOk);
			Cur.AlignBoundary();
			Entry.Name = Cur.ReadPString(bOk);
			Cur.AlignBoundary();
			Entry.IconDid = Cur.ReadU32(bOk);
			Entry.TrainedCost = Cur.ReadU32(bOk); // TrainedCost
			Cur.ReadU32(bOk); // SpecializedCost
			Cur.ReadU32(bOk); // Category
			Cur.ReadU32(bOk); // ChargenUse
			Entry.MinLevel = Cur.ReadU32(bOk); // MinLevel (1 = usable untrained)
			Entry.FormulaW = Cur.ReadU32(bOk); Entry.FormulaX = Cur.ReadU32(bOk);
			Entry.FormulaY = Cur.ReadU32(bOk); Entry.FormulaZ = Cur.ReadU32(bOk);
			Entry.FormulaAttr1 = Cur.ReadU32(bOk); Entry.FormulaAttr2 = Cur.ReadU32(bOk);
			bOk = Cur.Skip(3 * 8) && bOk; // UpperBound / LowerBound / LearnMod doubles
			if (bOk)
			{
				SkillInfoCache.Add(Key, MoveTemp(Entry));
			}
		}
		bSkillTableLoaded = true;
		if (!bOk || SkillInfoCache.Num() != Count)
		{
			// A partial catalog cannot decide which server records are obsolete.
			SkillInfoCache.Reset();
			UE_LOG(LogTemp, Warning, TEXT("ACEDat: invalid SkillTable in %s"), *DatDirectory);
			return false;
		}
		UE_LOG(LogTemp, Log, TEXT("ACEDat: SkillTable loaded (%d skills)"), SkillInfoCache.Num());
	}
	return !SkillInfoCache.IsEmpty();
}

bool UACEDatSubsystem::TryGetSkillInfo(uint32 SkillId, FString& OutName, uint32& OutIconDid)
{
	OutName.Reset();
	OutIconDid = 0;
	if (!SkillId || !EnsureSkillTableLoaded()) return false;
	if (const FSpellInfoCacheEntry* Found = SkillInfoCache.Find(SkillId))
	{
		OutName = Found->Name;
		OutIconDid = Found->IconDid;
		return !OutName.IsEmpty() || OutIconDid != 0;
	}
	return false;
}

bool UACEDatSubsystem::TryGetSkillTooltip(uint32 SkillId, FString& OutText)
{
	OutText.Reset();
	FString Name;
	uint32 Icon = 0;
	if (!SkillId || !TryGetSkillInfo(SkillId, Name, Icon)) return false;
	const FSpellInfoCacheEntry* Skill = SkillInfoCache.Find(SkillId);
	if (!Skill) return false;
	// SkillSystem::InqSkillFormula uses full attribute names, weighted terms,
	// parentheses for a two-attribute sum, and omits a divisor of one.
	static const TCHAR* Attributes[] = {TEXT(""), TEXT("Strength"), TEXT("Endurance"),
		TEXT("Quickness"), TEXT("Coordination"), TEXT("Focus"), TEXT("Self")};
	TArray<FString, TInlineAllocator<2>> Terms;
	auto AddTerm = [&](uint32 Weight, uint32 Attribute)
	{
		if (!Weight || !Attribute || Attribute >= UE_ARRAY_COUNT(Attributes)) return;
		Terms.Add(Weight == 1 ? FString(Attributes[Attribute])
			: FString::Printf(TEXT("(%u x %s)"), Weight, Attributes[Attribute]));
	};
	AddTerm(Skill->FormulaX, Skill->FormulaAttr1);
	AddTerm(Skill->FormulaY, Skill->FormulaAttr2);
	if (!Terms.IsEmpty())
	{
		OutText = TEXT("( ");
		OutText += Terms.Num() == 2 ? TEXT("(") + Terms[0] + TEXT(" + ") + Terms[1] + TEXT(")") : Terms[0];
		if (Skill->FormulaZ != 1) OutText += FString::Printf(TEXT(" / %u"), Skill->FormulaZ);
		if (Skill->FormulaW) OutText += FString::Printf(TEXT("+%u"), Skill->FormulaW);
		OutText += TEXT(" )\n");
	}
	OutText += Skill->Description;
	return !OutText.IsEmpty();
}

bool UACEDatSubsystem::TryGetSkillTrainedCost(uint32 SkillId, int32& OutTrainedCost)
{
	OutTrainedCost = 0;
	FString Name;
	uint32 Icon = 0;
	if (!TryGetSkillInfo(SkillId, Name, Icon))
	{
		return false;
	}
	if (const FSpellInfoCacheEntry* Found = SkillInfoCache.Find(SkillId))
	{
		// Exact DAT cost — server rejects TrainSkill if creditsSpent != TrainedCost (incl. 0).
		OutTrainedCost = static_cast<int32>(Found->TrainedCost);
		return true;
	}
	return false;
}

bool UACEDatSubsystem::TryGetSkillMinLevel(uint32 SkillId, uint32& OutMinLevel)
{
	OutMinLevel = 0;
	FString Name;
	uint32 Icon = 0;
	if (!TryGetSkillInfo(SkillId, Name, Icon))
	{
		return false;
	}
	if (const FSpellInfoCacheEntry* Found = SkillInfoCache.Find(SkillId))
	{
		OutMinLevel = Found->MinLevel;
		return OutMinLevel != 0;
	}
	return false;
}

bool UACEDatSubsystem::EnsureXpTableLoaded()
{
	if (bXpTableLoaded)
	{
		return AttributeXpList.Num() > 0;
	}
	bXpTableLoaded = true;
	if (!EnsureLoaded() || !PortalDat)
	{
		return false;
	}
	TArray<uint8> Blob;
	if (!PortalDat->ReadFile(0x0E000018u, Blob))
	{
		return false;
	}
	FACEDatCursor Cur(Blob);
	bool bOk = true;
	Cur.ReadU32(bOk); // file Id
	const int32 AttributeCount = static_cast<int32>(Cur.ReadU32(bOk));
	const int32 VitalCount = static_cast<int32>(Cur.ReadU32(bOk));
	const int32 TrainedSkillCount = static_cast<int32>(Cur.ReadU32(bOk));
	const int32 SpecializedSkillCount = static_cast<int32>(Cur.ReadU32(bOk));
	const uint32 LevelCount = Cur.ReadU32(bOk);
	AttributeXpList.Reset();
	VitalXpList.Reset();
	TrainedSkillXpList.Reset();
	SpecializedSkillXpList.Reset();
	CharacterLevelXPList.Reset();
	AttributeXpList.Reserve(AttributeCount + 1);
	VitalXpList.Reserve(VitalCount + 1);
	TrainedSkillXpList.Reserve(TrainedSkillCount + 1);
	SpecializedSkillXpList.Reserve(SpecializedSkillCount + 1);
	CharacterLevelXPList.Reserve(static_cast<int32>(LevelCount) + 1);
	for (int32 i = 0; i <= AttributeCount && bOk; ++i)
	{
		AttributeXpList.Add(Cur.ReadU32(bOk));
	}
	for (int32 i = 0; i <= VitalCount && bOk; ++i)
	{
		VitalXpList.Add(Cur.ReadU32(bOk));
	}
	for (int32 i = 0; i <= TrainedSkillCount && bOk; ++i)
	{
		TrainedSkillXpList.Add(Cur.ReadU32(bOk));
	}
	for (int32 i = 0; i <= SpecializedSkillCount && bOk; ++i)
	{
		SpecializedSkillXpList.Add(Cur.ReadU32(bOk));
	}
	for (uint32 i = 0; i <= LevelCount && bOk; ++i)
	{
		uint64 Xp = 0;
		bOk = bOk && Cur.Read(Xp);
		CharacterLevelXPList.Add(Xp);
	}
	UE_LOG(LogTemp, Log, TEXT("ACEDat: XpTable loaded (attr=%d vital=%d trained=%d spec=%d levels=%d)"),
		AttributeXpList.Num(), VitalXpList.Num(), TrainedSkillXpList.Num(), SpecializedSkillXpList.Num(), CharacterLevelXPList.Num());
	return bOk && AttributeXpList.Num() > 0;
}

static bool XpToNextFromList(const TArray<uint32>& List, int32 CurrentXpSpent, int64& OutXpNeeded, int64* OutMaxSpendable, int32 RankCount)
{
	OutXpNeeded = 0;
	if (OutMaxSpendable) { *OutMaxSpendable = 0; }
	if (List.Num() < 2)
	{
		return false;
	}
	const int64 Spent = static_cast<uint32>(CurrentXpSpent);
	int32 NextIdx = INDEX_NONE;
	for (int32 i = 0; i < List.Num(); ++i)
	{
		if (static_cast<int64>(List[i]) > Spent)
		{
			NextIdx = i;
			break;
		}
	}
	if (NextIdx == INDEX_NONE)
	{
		return true; // already at max
	}
	// Retail Raise10Selection pays the cumulative cost of up to ten ranks.
	const int32 Target = FMath::Min(List.Num() - 1, NextIdx + FMath::Max(1, RankCount) - 1);
	OutXpNeeded = static_cast<int64>(List[Target]) - Spent;
	if (OutMaxSpendable)
	{
		// Attribute / skill XP caps exceed INT_MAX — must stay int64.
		*OutMaxSpendable = FMath::Max<int64>(0, static_cast<int64>(List.Last()) - Spent);
	}
	return true;
}

bool UACEDatSubsystem::TryGetAttributeXpToNextRank(int32 CurrentXpSpent, int64& OutXpNeeded, int64* OutMaxSpendable, int32 RankCount)
{
	if (!EnsureXpTableLoaded())
	{
		OutXpNeeded = 0;
		if (OutMaxSpendable) { *OutMaxSpendable = 0; }
		return false;
	}
	return XpToNextFromList(AttributeXpList, CurrentXpSpent, OutXpNeeded, OutMaxSpendable, RankCount);
}

bool UACEDatSubsystem::TryGetVitalXpToNextRank(int32 CurrentXpSpent, int64& OutXpNeeded, int64* OutMaxSpendable, int32 RankCount)
{
	if (!EnsureXpTableLoaded())
	{
		OutXpNeeded = 0;
		if (OutMaxSpendable) { *OutMaxSpendable = 0; }
		return false;
	}
	return XpToNextFromList(VitalXpList, CurrentXpSpent, OutXpNeeded, OutMaxSpendable, RankCount);
}

bool UACEDatSubsystem::TryGetSkillXpToNextRank(int32 AdvancementClass, int32 CurrentXpSpent, int64& OutXpNeeded, int64* OutMaxSpendable, int32 RankCount)
{
	if (!EnsureXpTableLoaded())
	{
		OutXpNeeded = 0;
		if (OutMaxSpendable) { *OutMaxSpendable = 0; }
		return false;
	}
	const TArray<uint32>& List = (AdvancementClass >= 3) ? SpecializedSkillXpList : TrainedSkillXpList;
	if (AdvancementClass < 2)
	{
		OutXpNeeded = 0;
		if (OutMaxSpendable) { *OutMaxSpendable = 0; }
		return false;
	}
	return XpToNextFromList(List, CurrentXpSpent, OutXpNeeded, OutMaxSpendable, RankCount);
}

bool UACEDatSubsystem::TryGetSkillRankProgress(int32 AdvancementClass, int32 Ranks, int32 CurrentXpSpent, float& OutProgress01)
{
	OutProgress01 = 0.f;
	if (AdvancementClass < 2 || !EnsureXpTableLoaded()) return false;
	const TArray<uint32>& List = AdvancementClass >= 3 ? SpecializedSkillXpList : TrainedSkillXpList;
	if (!List.IsValidIndex(Ranks)) return false;
	if (!List.IsValidIndex(Ranks + 1)) return true; // Retail shows no progress at the cap.
	const int64 Start = List[Ranks], End = List[Ranks + 1];
	if (End > Start)
		OutProgress01 = FMath::Clamp(float((double(uint32(CurrentXpSpent)) - Start) / double(End - Start)), 0.f, 1.f);
	return true;
}

bool UACEDatSubsystem::TryGetLevelExperienceCost(int32 Level, uint64& OutCost)
{
	OutCost = 0;
	if (!EnsureXpTableLoaded() || Level < 1 || Level >= CharacterLevelXPList.Num() - 1) return false;
	const uint64 Current = CharacterLevelXPList[Level], Next = CharacterLevelXPList[Level + 1];
	if (Next <= Current) return false;
	OutCost = Next - Current;
	return true;
}

bool UACEDatSubsystem::TryGetXpToNextLevel(int64 TotalExperience, int32 CurrentLevel, int64& OutXpNeeded, float* OutProgress01)
{
	OutXpNeeded = 0;
	if (OutProgress01) { *OutProgress01 = 0.f; }
	if (!EnsureXpTableLoaded() || CharacterLevelXPList.Num() < 2)
	{
		return false;
	}
	const int32 MaxLevel = CharacterLevelXPList.Num() - 1;
	const int32 Level = FMath::Clamp(CurrentLevel, 1, MaxLevel);
	if (Level >= MaxLevel)
	{
		if (OutProgress01) { *OutProgress01 = 1.f; }
		return true;
	}
	const int64 CurThresh = static_cast<int64>(CharacterLevelXPList[Level]);
	const int64 NextThresh = static_cast<int64>(CharacterLevelXPList[Level + 1]);
	OutXpNeeded = FMath::Max<int64>(0, NextThresh - TotalExperience);
	if (OutProgress01)
	{
		const int64 Span = FMath::Max<int64>(1, NextThresh - CurThresh);
		*OutProgress01 = FMath::Clamp(static_cast<float>(TotalExperience - CurThresh) / static_cast<float>(Span), 0.f, 1.f);
	}
	return true;
}
