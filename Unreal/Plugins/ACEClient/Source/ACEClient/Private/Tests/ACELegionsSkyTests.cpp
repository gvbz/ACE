#if WITH_DEV_AUTOMATION_TESTS
#include "Sky/ACELegionsSkyRenderer.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACERuntimeOptions.h"
#include "ACESession.h"
#include "ACELandblockActor.h"
#include "Personal/PersonalFarScenerySubsystem.h"
#include "Personal/PersonalFarTerrainMesh.h"
#include "Personal/PersonalFarTerrainSubsystem.h"
#include "ACESkyDomeActor.h"
#include "ACECameraRetail.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/DirectionalLight.h"
#include "Components/LightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UI/ACEVideoSettingsWidget.h"
#include "Components/EditableTextBox.h"
#include "Components/PanelWidget.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ProceduralMeshComponent.h"
#include "RenderingThread.h"
#if WITH_EDITOR
#include "ShaderCompiler.h"
#include "StaticMeshCompiler.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELegionsSkyTest, "ACE.Graphics.LegionsSkyImport",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext |
                                     EAutomationTestFlags::EngineFilter)
bool FACELegionsSkyTest::RunTest(const FString&)
{
	const float FogLimitSaved=ACERuntimeOptions::Get(TEXT("PersonalMaxFogDistance")); ACERuntimeOptions::Set(TEXT("PersonalMaxFogDistance"),43.f); ON_SCOPE_EXIT { ACERuntimeOptions::Set(TEXT("PersonalMaxFogDistance"),FogLimitSaved); }; // personal: fog-distance
	const float SkyHorizonSaved=ACERuntimeOptions::Get(TEXT("PersonalSkyHorizon")); ACERuntimeOptions::Set(TEXT("PersonalSkyHorizon"),0.f); ON_SCOPE_EXIT { ACERuntimeOptions::Set(TEXT("PersonalSkyHorizon"),SkyHorizonSaved); }; // personal: fog-distance
	const FString Output = FPaths::ProjectSavedDir() / TEXT("Automation/LegionsSky");
	IFileManager::Get().MakeDirectory(*Output, true);
	const FString Cache = Output / TEXT("Test.sky");
	FString Error;
	ACELegionsSky::FBackdrop Data;
	const FString Broken = Output / TEXT("Truncated.sky");
	FFileHelper::SaveStringToFile(TEXT("truncated"), *Broken);
	TestFalse(TEXT("Invalid cache falls back"), ACELegionsSky::Load(Broken, Data, Error));
	TestFalse(TEXT("Missing AC2 data is rejected"), ACELegionsSky::Import(Output, Cache, Error));
	FString Legacy;
	if (FParse::Value(FCommandLine::Get(),TEXT("AC2SkyLegacyCache="),Legacy))
	{
		ACELegionsSky::FBackdrop Old;
		TestTrue(TEXT("Previous import keeps its artwork and light keys until reimport"),
			ACELegionsSky::Load(Legacy,Old,Error) && Old.CloudMask==0 && Old.Images.Num()==9 &&
			Old.Keys.Num()==13 && FMath::IsNearlyEqual(Old.Keys[0].Direct,.65f));
	}
	FString PreviousCache;
	if (FParse::Value(FCommandLine::Get(),TEXT("AC2SkyCloudCache="),PreviousCache))
	{
		ACELegionsSky::FBackdrop Old;
		TestTrue(TEXT("Cloud-shadow imports remain usable before adding weather"),
			ACELegionsSky::Load(PreviousCache,Old,Error) && Old.CloudMask!=0 && Old.RainDays.IsEmpty() && Old.Layers.Num()==8);
	}
	FString Source;
	if (!FParse::Value(FCommandLine::Get(), TEXT("AC2SkyTestDat="), Source))
	{
		AddInfo(TEXT("Pass -AC2SkyTestDat=<Legions folder> for the local artwork and render checks."));
		return true;
	}
	if (!TestTrue(TEXT("Import player-owned Legions DATs"), ACELegionsSky::Import(Source, Cache, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("Failed reimport preserves cache"), ACELegionsSky::Import(Output, Cache, Error));
	if (!TestTrue(TEXT("Reload using only the saved cache"), ACELegionsSky::Load(Cache, Data, Error)))
		return false;
	TestEqual(TEXT("Both rain days referenced by the Legions region"), Data.RainDays.Num(), 2);
	for (const auto& Day : Data.RainDays)
	{
		TestEqual(TEXT("Peak rain is fully overcast"),Day.Keys[Day.Wet].Haze.A,1.f);
		TestTrue(TEXT("Approaching rain still reveals the sky"),Day.Keys[Day.Cloudy].Haze.A < 1.f && Day.Keys[Day.Cloudy].Haze.A > .5f);
	}
	TestEqual(TEXT("Outdoor sky layers"), Data.Layers.Num(), 8);
	TestEqual(TEXT("Native time keys"), Data.Keys.Num(), 13);
	TestEqual(TEXT("Sky textures and terrain shadow map"), Data.Images.Num(), 10);
	TestTrue(TEXT("SurfaceDesc supplies the original moving terrain mask"),Data.Images.Contains(0x400003a9) &&
		Data.CloudMask == 0x400003a9 && Data.CloudMotion.Equals(FLinearColor(.001f,-.006f,-.002f,0),.000001f));
	int64 NativeBytes=0, ExpandedBytes=0;
	for (const auto& Pair : Data.Images)
	{
		TestTrue(TEXT("Original texture mip chain is retained"), Pair.Value.Mips.Num() > 1);
		for (int32 I=0; I<Pair.Value.Mips.Num(); ++I)
		{
			NativeBytes+=Pair.Value.Mips[I].Num();
			ExpandedBytes+=int64(FMath::Max(1,Pair.Value.Width>>I))*FMath::Max(1,Pair.Value.Height>>I)*4;
		}
	}
	AddInfo(FString::Printf(TEXT("Sky and cloud mask mip payload %.2f MiB (BGRA equivalent %.2f MiB)"),
		double(NativeBytes)/1048576.,double(ExpandedBytes)/1048576.));
	bool bIndependentCloudMotion = false;
	for (const auto& Layer : Data.Layers)
		for (const auto& Section : Layer.Sections)
			bIndependentCloudMotion |= Section.StageOps.A > 0 && !Section.Velocity[0].IsNearlyZero() &&
			                           !Section.Velocity[1].IsNearlyZero() &&
			                           Section.Velocity[0] != Section.Velocity[1];
	TestTrue(TEXT("Both cloud texture stages use the DAT motion"), bIndependentCloudMotion);
	TArray<uint8> Tampered;
	if (!TestTrue(TEXT("Read cache for integrity test"), FFileHelper::LoadFileToArray(Tampered, *Cache)) ||
	    Tampered.Num() < 16)
		return false;
	Tampered[12] ^= 1;
	FFileHelper::SaveArrayToFile(Tampered, *Broken);
	ACELegionsSky::FBackdrop Rejected;
	TestFalse(TEXT("Reject modified cache"), ACELegionsSky::Load(Broken, Rejected, Error));
	const FString Installed = Output / TEXT("Quest/Legions.sky");
	const FString Incoming = ACELegionsSky::StagedPath(Installed);
	auto& Files = IFileManager::Get();
	Files.MakeDirectory(*FPaths::GetPath(Incoming), true);
	auto Transfer = [&](const FString& Path)
	{
		return TestTrue(TEXT("Stage player file"), Files.Copy(*Incoming, *Path) == COPY_OK);
	};
	if (!Transfer(Source / TEXT("portal.dat")) ||
		!TestTrue(TEXT("Import Quest sky from base archive alone"), ACELegionsSky::ImportStaged(Installed, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("Release space used by transferred DAT"), Files.FileExists(*Incoming));
	if (Transfer(Cache))
		TestTrue(TEXT("Accept preimported desktop cache"), ACELegionsSky::ImportStaged(Installed, Error));
	if (Transfer(Broken))
		TestFalse(TEXT("Reject damaged replacement"), ACELegionsSky::ImportStaged(Installed, Error));
	TArray<uint8> Before, After;
	TestTrue(TEXT("Failed import preserves working sky byte-for-byte"),
		FFileHelper::LoadFileToArray(Before, *Cache) && FFileHelper::LoadFileToArray(After, *Installed) && Before == After);
	if (!Legacy.IsEmpty() && Transfer(Legacy))
		TestTrue(TEXT("Accept legacy desktop sky on Quest"), ACELegionsSky::ImportStaged(Installed, Error));
	if (GUsingNullRHI)
		return true;
	const auto Init = UWorld::InitializationValues()
	                      .AllowAudioPlayback(false)
	                      .RequiresHitProxies(false)
	                      .CreateNavigation(false)
	                      .CreateAISystem(false)
	                      .ShouldSimulatePhysics(false);
	auto* World =
	    UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	auto* GI = NewObject<UGameInstance>(GEngine);
	GI->InitializeStandalone();
	auto* InitialWorld = GI->GetWorld();
	GI->GetWorldContext()->SetCurrentWorld(World);
	World->SetGameInstance(GI);
	InitialWorld->DestroyWorld(false);
	ON_SCOPE_EXIT
	{
		GI->Shutdown();
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	auto* Owner = World->SpawnActor<AACESkyDomeActor>();
	auto* Settings=CreateWidget<UACEVideoSettingsWidget>(World,UACEVideoSettingsWidget::StaticClass());
	if (!TestNotNull(TEXT("Graphics options"),Settings)) return false;
	const auto SettingsSlate=Settings->TakeWidget();
	auto* Path=Cast<UEditableTextBox>(Settings->GetWidgetFromName(TEXT("LegionsDatPath")));
	if (!TestNotNull(TEXT("Editable DAT path"),Path)) return false;
#if PLATFORM_WINDOWS || PLATFORM_LINUX
	TestNotNull(TEXT("Desktop file picker"),Settings->GetWidgetFromName(TEXT("BrowseLegionsDat")));
#endif
	auto* ImportRow=Path->GetParent()->GetParent();
	FWidgetRenderer WidgetRenderer(true,true);
	for (int32 Scale : {1,2})
	{
		const FVector2D Size(272*Scale,22*Scale);
		auto* RowTarget=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
		WidgetRenderer.DrawWidget(RowTarget,ImportRow->TakeWidget(),float(Scale),Size,0.f);
		FlushRenderingCommands();
		TArray<FColor> Pixels;
		if (RowTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels))
		{
			TArray64<uint8> PNG;
			FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
			FFileHelper::SaveArrayToFile(PNG,*(Output/FString::Printf(TEXT("import-%dx.png"),Scale)));
		}
	}
	Owner->DatSubsystem = GI->GetSubsystem<UACEDatSubsystem>();
	FString RetailDir=TEXT("C:/Turbine/Asheron's Call");
	FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),RetailDir);
	if (!TestTrue(TEXT("Load AC1 sky schedules"),Owner->DatSubsystem->LoadDatDirectory(RetailDir))) return false;
	Owner->ClientSubsystem=GI->GetSubsystem<UACEClientSubsystem>();
	auto Session=Owner->ClientSubsystem->GetSession();
	auto SetClock=[&](int32 Day,double Phase)
	{
		Session->ApplyServerTime(Day*7620.0-3600.0+Phase*7620.0,FPlatformTime::Seconds(),TEXT("TimeSync"));
	};
	SetClock(39755,0);
	if (!TestTrue(TEXT("Build server-selected clear day"),Owner->TryBuild())) return false;
	TestEqual(TEXT("Clear day fixture"),Owner->ActiveGroup.DayName,FString(TEXT("Clear")));
	auto* Retail=World->SpawnActor<AACESkyDomeActor>();
	Retail->DatSubsystem=Owner->DatSubsystem;
	Retail->ClientSubsystem=Owner->ClientSubsystem;
	Retail->bDriveWorldLighting=false;
	Retail->bDriveWorldFog=false;
	Retail->SetActorHiddenInGame(true);
	if (!TestTrue(TEXT("Build stock reference sky"),Retail->TryBuild())) return false;
	auto StockSlot=[](AACESkyDomeActor* Sky,uint32 Id)
	{
		return Sky->Slots.FindByPredicate([Id](const auto& Slot){ return Slot.Def.DefaultGfxObjectId==Id; });
	};
	Owner->ImportedRevision = ACELegionsSky::Revision();
	TMap<uint32,FVector> OrbCenters;
	TMap<uint32,FVector> OrbBodyCenters;
	for (const auto& Layer : Data.Layers)
		if (Layer.Setup == 0x04000023 || Layer.Setup == 0x040000b6)
		{
			OrbCenters.Add(Layer.Setup,Layer.Center);
			FBox BodyBounds(ForceInit);
			for (const auto& Section : Layer.Sections)
				if (!Section.Additive)
					for (const int32 Index : Section.Indices) BodyBounds+=Layer.Vertices[Index];
			OrbBodyCenters.Add(Layer.Setup,BodyBounds.IsValid ? BodyBounds.GetCenter() : Layer.Center);
		}
	const auto* Belt=Data.Layers.FindByPredicate([](const auto& L){return L.Setup==0x04000729;});
	if (!TestTrue(TEXT("Legions asteroid belt texture"),Belt && Belt->Sections[0].Texture==0x40000b81)) return false;
	const FVector BeltCenter=Belt->Center;
	const auto AuthoredFogKeys = Data.Keys;
	auto Imported = MakeShared<ACELegionsSky::FBackdrop>(MoveTemp(Data));
	Owner->PendingSky = MakeFulfilledPromise<TSharedPtr<ACELegionsSky::FBackdrop>>(Imported).GetFuture();
	if (!TestTrue(TEXT("Build AC2 sky and haze outdoors"), Owner->UpdateImportedSky(0, true)))
		return false;
	auto RenderedCloudOpacity = [](AActor* Actor)
	{
		float Opacity = 0.f;
		TInlineComponentArray<UProceduralMeshComponent*> Parts(Actor);
		for (const auto* Part : Parts)
			if (Part->IsVisible())
			{
				bool CloudSheet=false;
				for (int32 Section=0; Section<Part->GetNumMaterials(); ++Section)
					if (auto* Mid = Cast<UMaterialInstanceDynamic>(Part->GetMaterial(Section)))
					{
						FLinearColor Stages;
						CloudSheet|=Mid->GetVectorParameterValue(FMaterialParameterInfo(TEXT("StageOps")),Stages)&&Stages.A>.5f;
					}
				if (!CloudSheet) continue;
				for (int32 Section=0; Section<Part->GetNumMaterials(); ++Section)
					if (auto* Mid = Cast<UMaterialInstanceDynamic>(Part->GetMaterial(Section)))
				{
					FLinearColor Stages;
					if (Mid->GetVectorParameterValue(FMaterialParameterInfo(TEXT("StageOps")),Stages))
					{
						const float MaterialAlpha=Mid->K2_GetVectorParameterValue(TEXT("MaterialFactor")).A;
						Opacity = FMath::Max(Opacity,FMath::Clamp(Mid->K2_GetVectorParameterValue(TEXT("SkyTint")).A,0.f,1.f)*FMath::Clamp(MaterialAlpha*(Stages.A>.5f?2.f:1.f),0.f,1.f));
					}
				}
			}
		return Opacity;
	};
	{
		// Keep the imported art but control the sheet lifetime and authored alpha.
		// AC1 may still schedule clouds while the selected AC2 sheet is hidden.
		ACELegionsSky::FBackdrop Fixture;
		if (!ACELegionsSky::Load(Cache,Fixture,Error)) return false;
		auto* Cloud = Fixture.Layers.FindByPredicate([](const auto& L){ return L.Setup == 0x04000089; });
		if (!TestNotNull(TEXT("Imported cloud sheet exists for visibility regression"),Cloud)) return false;
		const uint32 CloudName = Cloud->Name;
		Cloud->Begin = .2f; Cloud->End = .8f;
		for (auto& Section : Cloud->Sections) Section.Tint.A = 1.f;
		const auto BaseKey = Fixture.Keys[0];
		Fixture.Keys.Reset();
		for (const auto TimeAlpha : {FVector2D(0,0),FVector2D(.25,0),FVector2D(.5,1),FVector2D(.75,1),FVector2D(1,1)})
		{
			auto Key = BaseKey; Key.Time = TimeAlpha.X;
			Key.Objects.FindOrAdd(CloudName) = {0,0,FLinearColor(1,1,1,TimeAlpha.Y)};
			Fixture.Keys.Add(MoveTemp(Key));
		}
		auto TransparentMaterialFixture = Fixture;
		for (auto& Layer : TransparentMaterialFixture.Layers)
			if (Layer.Setup == 0x04000089)
				for (auto& Section : Layer.Sections) Section.Tint.A = 0.f;
		auto* ProbeActor = World->SpawnActor<AActor>(); ProbeActor->SetActorHiddenInGame(true);
		FACELegionsSkyRenderer Probe;
		ON_SCOPE_EXIT { Probe.Clear(); ProbeActor->Destroy(); };
		if (!TestTrue(TEXT("Build cloud visibility fixture from native artwork"),Probe.Build(*ProbeActor,MoveTemp(Fixture)))) return false;
		FACELegionsSkyFrame Frame; Frame.CloudOpacity = 1.f;
		Probe.Update(.5f,Frame);
		TestEqual(TEXT("Opaque visible cloud sheet supplies full shadow opacity"),Probe.CloudShadowOpacity,1.f);
		TestEqual(TEXT("Shadow opacity matches the material alpha actually drawn"),Probe.CloudShadowOpacity,RenderedCloudOpacity(ProbeActor));
		Probe.Update(.9f,Frame);
		TestEqual(TEXT("Clouds outside their native lifetime leave no ground shadow despite AC1 cloud cover"),Probe.CloudShadowOpacity,0.f);
		TestEqual(TEXT("The out-of-lifetime cloud sheet really is hidden"),RenderedCloudOpacity(ProbeActor),0.f);
		Probe.Update(.25f,Frame);
		TestEqual(TEXT("Authored transparent cloud keys leave no ground shadow despite AC1 cloud cover"),Probe.CloudShadowOpacity,0.f);
		Probe.Update(.5f,Frame);
		Frame.CloudOpacity = .25f; Probe.Update(.5f,Frame);
		TestEqual(TEXT("Fading cloud artwork proportionally fades its ground shadow"),Probe.CloudShadowOpacity,.25f);
		TestEqual(TEXT("Fading shadow opacity matches the drawn cloud material"),Probe.CloudShadowOpacity,RenderedCloudOpacity(ProbeActor));
		Frame.CloudOpacity = 0.f; Probe.Update(.5f,Frame);
		TestEqual(TEXT("Clearing weather removes the previous cloud shadow"),Probe.CloudShadowOpacity,0.f);
		if (!TestTrue(TEXT("Build cloud sheet with transparent native materials"),Probe.Build(*ProbeActor,MoveTemp(TransparentMaterialFixture)))) return false;
		Frame.CloudOpacity = 1.f; Probe.Update(.5f,Frame);
		TestEqual(TEXT("Transparent material sections cannot cast ground shadows even with opaque cloud keys"),Probe.CloudShadowOpacity,0.f);
		TestEqual(TEXT("Transparent native cloud materials contribute no drawn alpha"),RenderedCloudOpacity(ProbeActor),0.f);
	}
	// CDayDesc::GetWorldFog returns the fog record, independently of the
	// dome/ramp/cloud object tints. In particular the transparent grey noon
	// overlay must not borrow a pink cloud or blue dome hue for world geometry.
	FACELegionsSkyFrame FogFrame;
	FogFrame.bClearForecast = true;
	FogFrame.CloudOpacity = 1.f;
	for (const auto& Key : AuthoredFogKeys)
	{
		const auto Haze = Owner->ImportedSky->Update(Key.Time,FogFrame);
		auto Expected = FLinearColor::FromSRGBColor(Key.Haze.ToFColor(false));
		Expected.A = 1.f;
		TestTrue(TEXT("World fog retains the authored AC2 RGB at every clear-day key"),
			Owner->ImportedSky->FogColor.Equals(Expected,.001f));
		TestEqual(TEXT("Sky overlay retains its independently authored opacity"),Haze.A,Key.Haze.A);
	}
	FogFrame.Overcast = FogFrame.Rain = 1.f;
	for (const float Phase : {0.f,.1f,.5f,.9f})
	{
		FogFrame.WeatherFogColor = FLinearColor::Red;
		const auto Haze = Owner->ImportedSky->Update(Phase,FogFrame);
		const auto Authored = Owner->ImportedSky->FogColor;
		const auto AuthoredSky = Owner->ImportedSky->SkyFogColor;
		FogFrame.WeatherFogColor = FLinearColor::Green;
		Owner->ImportedSky->Update(Phase,FogFrame);
		TestTrue(TEXT("AC1 weather tint cannot recolor imported world fog or its sky overlay"),
			Owner->ImportedSky->FogColor.Equals(Authored,.001f) &&
			Owner->ImportedSky->SkyFogColor.Equals(AuthoredSky,.001f));
		auto ExpectedStorm = FLinearColor::FromSRGBColor(Haze.ToFColor(false));
		ExpectedStorm.A = 1.f;
		TestTrue(TEXT("Storm clouds have their own light; fog remains the authored storm result"),
			Authored.Equals(ExpectedStorm,.001f));
	}
	Owner->UpdateSky(0,0,true);
	auto* Camera = NewObject<USceneCaptureComponent2D>(Owner);
	auto* Target = NewObject<UTextureRenderTarget2D>(Owner);
	Target->ClearColor = FLinearColor::Black;
	Target->InitCustomFormat(512, 320, PF_B8G8R8A8, false);
	Target->UpdateResourceImmediate(true);
	Camera->TextureTarget = Target;
	Camera->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Camera->bCaptureEveryFrame = false;
	Camera->bCaptureOnMovement = false;
	Camera->SetWorldRotation(FRotator(25, 0, 0));
	Camera->ShowFlags.SetEyeAdaptation(false);
	Camera->ShowFlags.SetTonemapper(false);
	Camera->ShowFlags.SetAtmosphere(false);
	Camera->RegisterComponent();
#if WITH_EDITOR
	if (GShaderCompilingManager)
		GShaderCompilingManager->FinishAllCompilation();
#endif
	FlushRenderingCommands();
	if (FParse::Param(FCommandLine::Get(), TEXT("AC2SkyPerf")))
	{
		Target->ResizeTarget(1920, 1080);
		for (const float Start : {0.f, .5f})
		{
			TArray<double> UpdateUs, FrameMs;
			for (int32 I = 0; I < 90; ++I)
			{
				World->Tick(LEVELTICK_TimeOnly,1.f/60.f);
				const double Begin = FPlatformTime::Seconds();
				Owner->UpdateImportedSky(Start + I / (60.f * 3600.f), true);
				const double Updated = FPlatformTime::Seconds();
				World->SendAllEndOfFrameUpdates();
				Camera->CaptureScene();
				ENQUEUE_RENDER_COMMAND(ACEWaitForSkyCapture)([](FRHICommandListImmediate& Cmd){ Cmd.SubmitAndBlockUntilGPUIdle(); });
				FlushRenderingCommands();
				if (I >= 30)
				{
					UpdateUs.Add((Updated - Begin) * 1.e6);
					FrameMs.Add((FPlatformTime::Seconds() - Begin) * 1.e3);
				}
			}
			UpdateUs.Sort();
			FrameMs.Sort();
			AddInfo(FString::Printf(TEXT("Sky benchmark t=%.1f 1080p median update %.2f us, synchronized capture %.3f ms (60 samples)"),
				Start, UpdateUs[30], FrameMs[30]));
		}
		Target->ResizeTarget(512, 320);
	}
	TArray<FColor> Night;
	for (const float Time : {0.f, .5f})
	{
		Owner->UpdateSky(Time,0,true);
		World->SendAllEndOfFrameUpdates();
		Camera->CaptureScene();
		FlushRenderingCommands();
		TArray<FColor> Pixels;
		if (!TestTrue(TEXT("Read rendered sky"), Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels)))
			return false;
		int32 Colored = 0, Changed = 0;
		for (int32 I = 0; I < Pixels.Num(); ++I)
		{
			auto& C = Pixels[I];
			C.A = 255;
			Colored += FMath::Max3(C.R, C.G, C.B) > 12;
			if (Night.IsValidIndex(I))
				Changed += FMath::Abs(int32(Night[I].B) - C.B) + FMath::Abs(int32(Night[I].R) - C.R) > 16;
		}
		TestTrue(TEXT("Sky artwork visible"), Colored > Pixels.Num() / 100);
		if (Time == 0)
			Night = Pixels;
		else
			TestTrue(TEXT("Day and night look different"), Changed > Pixels.Num() / 4);
		TArray64<uint8> PNG;
		FImageUtils::PNGCompressImageArray(512, 320, Pixels, PNG);
		FFileHelper::SaveArrayToFile(PNG, *(Output / (Time == 0 ? TEXT("night.png") : TEXT("day.png"))));
	}
	FACEDatSkyTimeOfDay Stock{};
	Stock.DirPitch=90; Stock.DirColor=Stock.AmbColor=0xffffffff;
	// The default third-person orbit only looks up 20 degrees. Verify the actual
	// composed pixels in that view, including clouds, at a normal world position.
	FACEPosition Yaraq;
	Yaraq.CellId=0x7D64000D;
	Yaraq.Location=FVector(35.68,102.89,12.01);
	FVector Viewer=Yaraq.ToUnrealLocation(100) + FVector(0,0,180);
	TArray<AACELandblockActor*> Coast;
	if (FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),RetailDir))
	{
		for (int32 X=-1; X<=1; ++X)
			for (int32 Y=-1; Y<=1; ++Y)
			{
				const int32 Id=((0x7D+X)<<24) | ((0x64+Y)<<16);
				Owner->DatSubsystem->GetOrBuildLandblockMesh(Id,100);
				auto* Block=World->SpawnActor<AACELandblockActor>();
				if (!TestTrue(TEXT("Build coastal terrain"),Block && Block->LoadLandblock(Id,100,false))) return false;
				Coast.Add(Block);
			}
#if WITH_EDITOR
		if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
#endif
		float Ground=0;
		if (!TestTrue(TEXT("Camera is above the coastal terrain"),Owner->DatSubsystem->SampleOutdoorGroundZ(Viewer.X,Viewer.Y,100,Ground))) return false;
		Viewer.Z=Ground+180;
	}
	Owner->SetActorLocation(Viewer);
	Camera->SetWorldLocation(Viewer);
	Camera->ShowFlags.SetTonemapper(true);
	Camera->FOVAngle=ACECameraRetail::HorizontalFovDegrees(16.f/9.f);
	// Match the viewport's UNORM backbuffer. FinalColorLDR already includes
	// display gamma; an sRGB render target would encode the sky a second time.
	Target->TargetGamma = 2.2f;
	Target->InitCustomFormat(640,360,PF_B8G8R8A8,true);
	Target->UpdateResourceImmediate(true);
	auto ReadView=[&](TArray<FColor>& Pixels)
	{
		World->SendAllEndOfFrameUpdates();
		Camera->CaptureScene();
		FlushRenderingCommands();
		return Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
	};
	auto SaveView=[&](TArray<FColor>& Pixels,const TCHAR* Name)
	{
		for (auto& Pixel : Pixels) Pixel.A=255;
		TArray64<uint8> PNG;
		FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,Pixels,PNG);
		FFileHelper::SaveArrayToFile(PNG,*(Output / Name));
	};
	// Compare final pixels at a fixed cloud position. The belt should read at a
	// normal coastal viewing angle, and the sun when looking toward its AC1 bearing.
	// The local ACE clear-weather preview selects the first Clear group (8).
	SetClock(39762,0);
	Owner->ClearSky(true);
	if (!Owner->TryBuild() || !TestEqual(TEXT("Local preview weather"),Owner->ActiveGroup.DayName,FString(TEXT("Clear")))) return false;
	TInlineComponentArray<UProceduralMeshComponent*> SkyParts(Owner);
	auto FindArtwork = [&](FVector LocalCenter) -> UProceduralMeshComponent*
	{
		for (auto* Part : SkyParts)
			if (auto* Section = Part->GetProcMeshSection(0); Section && Section->SectionLocalBox.GetCenter().Equals(LocalCenter,2.))
				return Part;
		return nullptr;
	};
	auto* ClearBelt = FindArtwork(BeltCenter);
	auto* ClearSun = FindArtwork(OrbCenters.FindChecked(0x04000023u));
	if (!TestNotNull(TEXT("Clear sky belt"),ClearBelt) || !TestNotNull(TEXT("Clear sky sun"),ClearSun)) return false;
	for (const float Phase : {.5f,.856f})
	{
		Owner->UpdateSky(Phase,0,true);
		for (auto* Artwork : {ClearBelt,ClearSun})
		{
			const bool IsSun = Artwork == ClearSun;
			const auto Bearing = (Artwork->Bounds.Origin - Viewer).Rotation();
			if (IsSun && !TestTrue(TEXT("Clear-day sun is above the horizon"),Artwork->IsVisible() && Bearing.Pitch >= 0))
				return false;
			int32 Strongest = -1;
			TArray<FColor> Best;
			for (int32 View = 0; View < (IsSun ? 1 : 4); ++View)
			{
				Camera->SetWorldRotation(IsSun ? Bearing : FRotator(12,90 * View,0));
				TArray<FColor> With, Without;
				if (!ReadView(With)) return false;
				Camera->HideComponent(Artwork);
				if (!ReadView(Without)) return false;
				Camera->HiddenComponents.Reset();
				int32 VisiblePixels = 0;
				for (int32 I = 0; I < With.Num(); ++I)
					if (FMath::Abs(int32(With[I].R) - Without[I].R) >= 12)
						++VisiblePixels;
				if (VisiblePixels > Strongest)
				{
					Strongest = VisiblePixels;
					Best = MoveTemp(With);
				}
			}
			const FString Name = FString::Printf(TEXT("clear-%s-%s"),Phase == .5f ? TEXT("noon") : TEXT("late"),IsSun ? TEXT("sun") : TEXT("belt"));
			AddInfo(FString::Printf(TEXT("%s: %d pixels with at least 12 levels of red contrast"),*Name,Strongest));
			TestTrue(Name,Strongest > (IsSun ? 400 : 1500));
			SaveView(Best,*(Name + TEXT(".png")));
		}
	}
	// Frame the setting sun off-center to judge the belt through the cloud field.
	const float SunYaw = (ClearSun->Bounds.Origin - Viewer).Rotation().Yaw;
	Camera->SetWorldRotation(FRotator(12, SunYaw + 25, 0));
	TArray<FColor> LateCoast;
	if (!ReadView(LateCoast)) return false;
	SaveView(LateCoast,TEXT("clear-late-coast.png"));
	SetClock(39755,0);
	Owner->ClearSky(true);
	if (!Owner->TryBuild()) return false;
	for (const float Time : {.32f,.73f,.04f})
	{
		const bool bSun=Time>.2f;
		Owner->UpdateSky(Time,0,true);
		Retail->UpdateSky(Time,0,false);
		Owner->UpdateWorldLighting(Stock,Stock,0,Time);
		Owner->UpdateFog(Stock,Stock,0);
		const FVector Center=OrbCenters.FindChecked(bSun ? 0x04000023u : 0x040000b6u);
		TInlineComponentArray<UProceduralMeshComponent*> Parts(Owner);
		UProceduralMeshComponent* Orb=nullptr;
		for (auto* Part : Parts)
			if (const auto* S=Part->GetProcMeshSection(0); S && S->SectionLocalBox.GetCenter().Equals(Center,2.))
				Orb=Part;
		if (!TestNotNull(TEXT("Celestial artwork has its own drawn mesh"),Orb)) return false;
		const auto* Native=StockSlot(Retail,bSun ? 0x01001348u : 0x01001F6Au);
		if (!Native || !TestNotNull(TEXT("Native AC1 celestial card"),Native->Mesh)) return false;
		const FVector StockBearing=Native->Mesh->GetComponentQuat().RotateVector(Native->LocalBodyDirection).GetSafeNormal();
		const FVector BodyCenter=OrbBodyCenters.FindChecked(bSun ? 0x04000023u : 0x040000b6u);
		TestTrue(TEXT("Artwork has AC1 azimuth and elevation"),StockBearing.Equals(
			(Orb->GetComponentTransform().TransformPosition(BodyCenter)-Owner->GetActorLocation()).GetSafeNormal(),.001));
		const FRotator Direction=StockBearing.Rotation();
		AddInfo(FString::Printf(TEXT("Dereth %.3f %s pitch %.1f yaw %.1f visible %d"),Time,
			bSun ? TEXT("sun") : TEXT("moon"),Direction.Pitch,Direction.Yaw,Orb->IsVisible()));
		TestTrue(TEXT("Orb is above the horizon and reachable in third person"),
			Orb->IsVisible() && Direction.Pitch>0 && Direction.Pitch<45);
		Camera->SetWorldRotation(FRotator(FMath::Clamp(Direction.Pitch-15.,0.,20.),Direction.Yaw,0));
		int32 MostVisible=0;
		TArray<FColor> BestView;
		// A passing opaque cloud can legitimately cover the sun. Keep all layers
		// enabled and compare the final image at several points in their drift.
		for (int32 Sample=0; Sample<(bSun ? 4 : 1); ++Sample)
		{
			if (Sample>0)
				for (int32 Frame=0; Frame<64; ++Frame) World->Tick(LEVELTICK_TimeOnly,.25f);
			TArray<FColor> With,Without;
			if (!ReadView(With)) return false;
			Camera->HideComponent(Orb);
			if (!ReadView(Without)) return false;
			Camera->HiddenComponents.Reset();
			int32 Changed=0;
			for (int32 I=0; I<With.Num(); ++I)
			{
				const auto& A=With[I]; const auto& B=Without[I];
				if (FMath::Max3(FMath::Abs(A.R-B.R),FMath::Abs(A.G-B.G),FMath::Abs(A.B-B.B))>8) ++Changed;
			}
			AddInfo(FString::Printf(TEXT("Orb contributes %d pixels at cloud sample %d"),Changed,Sample));
			if (Sample==0 || Changed>MostVisible)
			{
				MostVisible=Changed;
				BestView=MoveTemp(With);
			}
		}
		TestTrue(TEXT("Celestial disc is distinguishable in the composed sky"),MostVisible>100);
		const TCHAR* FileName=!bSun ? TEXT("retail-moon.png") : Time<.5f ? TEXT("morning-sun.png") : TEXT("afternoon-sun.png");
		SaveView(BestView,FileName);
		if (!bSun)
			TestTrue(TEXT("The AC1 moon rises against the dark blue Legions night"),
				Owner->ImportedSky->FogColor.B > Owner->ImportedSky->FogColor.R && Owner->ImportedSky->FogColor.GetLuminance()<.02f);
	}
	// Bearing alone cannot detect a flipped moon or a correctly centered but
	// oversized disc. Measure the body separately from its larger glow section,
	// then capture the actual artwork and both atmospheric occluders.
	{
		auto AngularBounds = [&](UProceduralMeshComponent* Mesh,int32 Section,const TCHAR* Label)
		{
			FBox2D Bounds(ForceInit);
			const auto* Geometry=Mesh ? Mesh->GetProcMeshSection(Section) : nullptr;
			if (!Geometry) return Bounds;
			const auto& Transform=Mesh->GetComponentTransform();
			const FVector Origin=Mesh->GetOwner()->GetActorLocation();
			// Imported sections share one vertex buffer; only referenced indices
			// belong to this section. The glow must not inflate the body's size.
			FBox SectionBounds(ForceInit);
			for (const uint32 Index : Geometry->ProcIndexBuffer) SectionBounds+=Geometry->ProcVertexBuffer[Index].Position;
			const FVector Center=Transform.TransformPosition(SectionBounds.GetCenter())-Origin;
			const FQuat View=Center.Rotation().Quaternion();
			for (const uint32 Index : Geometry->ProcIndexBuffer)
			{
				const auto& Vertex=Geometry->ProcVertexBuffer[Index];
				const FVector P=View.UnrotateVector(Transform.TransformPosition(Vertex.Position)-Origin);
				Bounds+=FVector2D(FMath::RadiansToDegrees(FMath::Atan2(P.Y,P.X)),FMath::RadiansToDegrees(FMath::Atan2(P.Z,P.X)));
			}
			FVector U=FVector::ZeroVector,V=FVector::ZeroVector;
			for (int32 Index=0; Index+2<Geometry->ProcIndexBuffer.Num(); Index+=3)
			{
				const auto& A=Geometry->ProcVertexBuffer[Geometry->ProcIndexBuffer[Index]];
				const auto& B=Geometry->ProcVertexBuffer[Geometry->ProcIndexBuffer[Index+1]];
				const auto& C=Geometry->ProcVertexBuffer[Geometry->ProcIndexBuffer[Index+2]];
				const FVector2D AB=B.UV0-A.UV0,AC=C.UV0-A.UV0;
				const double Det=AB.X*AC.Y-AB.Y*AC.X;
				if (FMath::Abs(Det)<1.e-8) continue;
				U=View.UnrotateVector(Transform.TransformVector(((B.Position-A.Position)*AC.Y-(C.Position-A.Position)*AB.Y)/Det)).GetSafeNormal();
				V=View.UnrotateVector(Transform.TransformVector(((C.Position-A.Position)*AB.X-(B.Position-A.Position)*AC.X)/Det)).GetSafeNormal();
				break;
			}
			// The retail card rolls in its orbit. A world-up screen box grows as
			// a square rotates, although the disc itself has not changed size.
			// Compare the body spans along its projected texture axes instead;
			// this also avoids equating a square corner with the moon's radius.
			const FVector Right=FVector(0.,U.Y,U.Z).GetSafeNormal();
			const FVector Up=FVector::CrossProduct(FVector::ForwardVector,Right);
			FBox2D TextureBounds(ForceInit);
			if (!Right.IsNearlyZero())
				for (const uint32 Index : Geometry->ProcIndexBuffer)
				{
					const FVector P=View.UnrotateVector(Transform.TransformPosition(Geometry->ProcVertexBuffer[Index].Position)-Origin);
					TextureBounds+=FVector2D(FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(P,Right),P.X)),
						FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(P,Up),P.X)));
				}
			AddInfo(FString::Printf(TEXT("Moon geometry %s section %d: center %s screen width %.4f height %.4f texture width %.4f height %.4f view-U %s view-V %s priority %d"),
				Label,Section,*Center.ToString(),Bounds.GetSize().X,Bounds.GetSize().Y,TextureBounds.GetSize().X,TextureBounds.GetSize().Y,
				*U.ToString(),*V.ToString(),Mesh->TranslucencySortPriority));
			return TextureBounds;
		};
		Owner->UpdateSky(.04f,0,true);
		Retail->UpdateSky(.04f,0,false);
		TInlineComponentArray<UProceduralMeshComponent*> Parts(Owner);
		auto* MoonPtr=Parts.FindByPredicate([&](auto* Part)
		{
			const auto* Section=Part->GetProcMeshSection(0);
			return Section && Section->SectionLocalBox.GetCenter().Equals(OrbCenters.FindChecked(0x040000b6u),2.);
		});
		if (!TestNotNull(TEXT("Imported primary moon for orientation and occlusion checks"),MoonPtr)) return false;
		auto* Moon=*MoonPtr;
		const auto* Primary=StockSlot(Retail,0x01001F6Au);
		const auto* Secondary=StockSlot(Owner,0x01001F67u);
		if (!Primary || !Secondary || !TestNotNull(TEXT("Retail primary moon for angular size reference"),Primary->Mesh) ||
			!TestNotNull(TEXT("Retained second moon for atmospheric occlusion"),Secondary->Mesh)) return false;
		const FBox2D ImportedSize=AngularBounds(Moon,0,TEXT("imported primary body"));
		AngularBounds(Moon,1,TEXT("imported primary glow"));
		const FBox2D RetailSize=AngularBounds(Primary->Mesh,0,TEXT("retail primary"));
		AngularBounds(Secondary->Mesh,0,TEXT("retail secondary"));
		TestTrue(TEXT("Imported primary moon body matches the corresponding retail angular diameter"),
			ImportedSize.bIsValid && RetailSize.bIsValid &&
			FMath::IsNearlyEqual(ImportedSize.GetSize().GetMax(),RetailSize.GetSize().GetMax(),.25));

		const auto PreviousMode=Camera->PrimitiveRenderMode;
		const auto PreviousRotation=Camera->GetComponentRotation();
		Camera->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		ON_SCOPE_EXIT
		{
			Camera->ShowOnlyComponents.Reset();
			Camera->HiddenComponents.Reset();
			Camera->PrimitiveRenderMode=PreviousMode;
			Camera->SetWorldRotation(PreviousRotation);
			Owner->UpdateSky(.04f,0,true);
		};
		Camera->ShowOnlyComponent(Moon);
		Camera->SetWorldRotation((Moon->GetComponentTransform().TransformPosition(OrbBodyCenters.FindChecked(0x040000b6u))-Viewer).Rotation());
		const bool GlowVisible=Moon->GetProcMeshSection(1) && Moon->GetProcMeshSection(1)->bSectionVisible;
		Moon->SetMeshSectionVisible(1,false);
		TArray<FColor> Disc,Empty;
		if (!ReadView(Disc)) return false;
		Moon->SetVisibility(false);
		if (!ReadView(Empty)) return false;
		Moon->SetVisibility(true);
		Moon->SetMeshSectionVisible(1,GlowVisible);
		double Light=0.,HorizontalMoment=0.;
		int32 LitPixels=0;
		for (int32 I=0; I<Disc.Num(); ++I)
		{
			const int32 Weight=FMath::Max(0,int32(Disc[I].R)+Disc[I].G+Disc[I].B-Empty[I].R-Empty[I].G-Empty[I].B);
			Light+=Weight;
			HorizontalMoment+=(I%Target->SizeX-(Target->SizeX-1)*.5)*Weight;
			LitPixels+=Weight>12;
		}
		const double CrescentOffset=Light>0. ? HorizontalMoment/Light : 0.;
		AddInfo(FString::Printf(TEXT("Isolated primary moon: %d lit pixels, bright-side centroid %.3f pixels right of center"),LitPixels,CrescentOffset));
		TestTrue(TEXT("Primary moon body is visible without its glow"),LitPixels>100);
		TestTrue(TEXT("Imported moon keeps its native bright crescent on the right"),CrescentOffset>.5);
		SaveView(Disc,TEXT("moon-primary-isolated.png"));

		auto* CloudPtr=Parts.FindByPredicate([](auto* Part)
		{
			for (int32 Section=0; Section<Part->GetNumMaterials(); ++Section)
				if (auto* Mid=Cast<UMaterialInstanceDynamic>(Part->GetMaterial(Section)))
				{
					FLinearColor Stages;
					if (Mid->GetVectorParameterValue(FMaterialParameterInfo(TEXT("StageOps")),Stages) && Stages.A>.5f) return true;
				}
			return false;
		});
		if (!TestNotNull(TEXT("Imported cloud sheet for moon occlusion"),CloudPtr) ||
			!TestNotNull(TEXT("Imported sky fog overlay for moon occlusion"),Owner->ColorFillMesh.Get()) ||
			!TestNotNull(TEXT("Imported sky fog material for moon occlusion"),Owner->ColorFillMid.Get())) return false;
		auto* Cloud=*CloudPtr;
		struct FCloudMaterialSnapshot
		{
			UMaterialInstanceDynamic* Material;
			FLinearColor Factor,Tint,Stages;
		};
		TArray<FCloudMaterialSnapshot> CloudMaterials;
		for (int32 Section=0; Section<Cloud->GetNumMaterials(); ++Section)
			if (auto* Mid=Cast<UMaterialInstanceDynamic>(Cloud->GetMaterial(Section)))
				CloudMaterials.Add({Mid,Mid->K2_GetVectorParameterValue(TEXT("MaterialFactor")),
					Mid->K2_GetVectorParameterValue(TEXT("SkyTint")),Mid->K2_GetVectorParameterValue(TEXT("StageOps"))});
		const float FogOpacity=Owner->ColorFillMid->K2_GetScalarParameterValue(TEXT("OpacityMul"));
		const bool FogVisible=Owner->ColorFillMesh->IsVisible(),CloudVisible=Cloud->IsVisible();
		ON_SCOPE_EXIT
		{
			for (const auto& Saved : CloudMaterials)
			{
				Saved.Material->SetVectorParameterValue(TEXT("MaterialFactor"),Saved.Factor);
				Saved.Material->SetVectorParameterValue(TEXT("SkyTint"),Saved.Tint);
				Saved.Material->SetVectorParameterValue(TEXT("StageOps"),Saved.Stages);
			}
			Owner->ColorFillMid->SetScalarParameterValue(TEXT("OpacityMul"),FogOpacity);
			Owner->ColorFillMesh->SetVisibility(FogVisible);
			Cloud->SetVisibility(CloudVisible);
		};
		for (int32 BodyIndex=0; BodyIndex<2; ++BodyIndex)
		{
			auto* Body=BodyIndex==0 ? Moon : Secondary->Mesh;
			bool FoundVisible=false;
			for (const float Phase : {.04f,.1f,.17f,.215f,.93f,.999f})
			{
				Owner->UpdateSky(Phase,0,true);
				if (Body->IsVisible() && (Body->Bounds.Origin-Viewer).GetSafeNormal().Z>.1)
				{
					FoundVisible=true;
					break;
				}
			}
			if (!TestTrue(TEXT("Each moon has a visible above-horizon phase for occlusion"),FoundVisible)) return false;
			Camera->SetWorldRotation((Body->Bounds.Origin-Viewer).Rotation());
			Camera->ShowOnlyComponents.Reset();
			Camera->ShowOnlyComponent(Body);
			TArray<FColor> Alone;
			if (!ReadView(Alone)) return false;
			int32 BodyPixels=0;
			for (const FColor& Pixel : Alone) BodyPixels+=FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>8;
			TestTrue(TEXT("Moon is visible before adding its atmospheric occluder"),BodyPixels>100);
			for (int32 Layer=0; Layer<2; ++Layer)
			{
				auto* Occluder=Layer==0 ? Owner->ColorFillMesh.Get() : Cloud;
				Occluder->SetVisibility(true);
				Owner->ColorFillMid->SetScalarParameterValue(TEXT("OpacityMul"),1.f);
				for (const auto& Saved : CloudMaterials)
				{
					auto Factor=Saved.Factor, Tint=Saved.Tint,Stages=Saved.Stages;
					Factor.A=Tint.A=1.f;
					Stages.G=Stages.A=0.f; // opaque coverage, keeping the real cloud geometry/material
					Saved.Material->SetVectorParameterValue(TEXT("MaterialFactor"),Factor);
					Saved.Material->SetVectorParameterValue(TEXT("SkyTint"),Tint);
					Saved.Material->SetVectorParameterValue(TEXT("StageOps"),Stages);
				}
				Camera->ShowOnlyComponents.Reset();
				Camera->ShowOnlyComponent(Body);
				Camera->ShowOnlyComponent(Occluder);
				TArray<FColor> With,Without;
				if (!ReadView(With)) return false;
				Body->SetVisibility(false);
				if (!ReadView(Without)) return false;
				Body->SetVisibility(true);
				int32 Changed=0;
				for (int32 I=0; I<With.Num(); ++I)
					Changed+=FMath::Max3(FMath::Abs(int32(With[I].R)-Without[I].R),FMath::Abs(int32(With[I].G)-Without[I].G),
						FMath::Abs(int32(With[I].B)-Without[I].B))>1;
				AddInfo(FString::Printf(TEXT("Moon %d behind opaque %s: %d changed pixels"),BodyIndex+1,Layer==0 ? TEXT("sky fog") : TEXT("clouds"),Changed));
				TestEqual(Layer==0 ? TEXT("Both moons disappear behind opaque sky fog") : TEXT("Both moons disappear behind opaque clouds"),Changed,0);
				SaveView(With,*FString::Printf(TEXT("moon-%d-opaque-%s.png"),BodyIndex+1,Layer==0 ? TEXT("fog") : TEXT("cloud")));
			}
		}
	}
	// Same coast, camera and server phase. Switch the forecast without advancing
	// the GPU clock, then capture both renderers during the same rain event.
	{
		Camera->SetWorldRotation(FRotator(5,110,0));
		TArray<FColor> Dry,Wet,StockWet;
		Owner->UpdateSky(.5f,0,true);
		if (!ReadView(Dry)) return false;
		SaveView(Dry,TEXT("coast-legions-clear.png"));
		const auto* Region=Owner->DatSubsystem->GetRegionSkyInfo();
		int32 StormDay=39755;
		while (StormDay<40255 && Owner->PickDayGroup(*Region,StormDay*7620.0-3600.0+3810.0)!=3) ++StormDay;
		if (!TestTrue(TEXT("Find daytime storm fixture"),StormDay<40255)) return false;
		SetClock(StormDay,.5f);
		Owner->ClearSky(true);
		if (!Owner->TryBuild()) return false;
		// The belt is a separate celestial layer, including nights with no
		// active AC1 cloud deck. Measure its contribution through the haze.
		TInlineComponentArray<UProceduralMeshComponent*> Parts(Owner);
		auto* BeltMesh=Parts.FindByPredicate([&](auto* Part) {
			const auto* Section=Part->GetProcMeshSection(0);
			return Section && Section->SectionLocalBox.GetCenter().Equals(BeltCenter,2.);
		});
		if (!TestNotNull(TEXT("Asteroid belt mesh"),BeltMesh)) return false;
		for (float Phase : {0.f,.008f,.1f})
		{
			Owner->UpdateSky(Phase,0,true);
			int32 MostVisible=0;
			for (float Yaw : {0.f,90.f,180.f,270.f})
			{
				Camera->SetWorldRotation(FRotator(15,Yaw,0));
				TArray<FColor> With,Without;
				if (!ReadView(With)) return false;
				Camera->HideComponent(*BeltMesh);
				if (!ReadView(Without)) return false;
				Camera->HiddenComponents.Reset();
				int32 Changed=0;
				for (int32 I=0; I<With.Num(); ++I)
					if (FMath::Abs(With[I].R-Without[I].R)>3) ++Changed;
				if (Changed>MostVisible)
				{
					MostVisible=Changed;
					if (Phase==.008f) SaveView(With,TEXT("coast-legions-passing-rain.png"));
				}
			}
			AddInfo(FString::Printf(TEXT("Belt at storm clock %.3f changes %d pixels"),Phase,MostVisible));
			TestTrue(TEXT("Belt shows through clear and partial cover, disappears under opaque rain haze"),
				Phase==.1f ? MostVisible==0 : MostVisible>100);
		}
		for (const float Phase : {.1f,.5f})
		{
			Owner->UpdateSky(Phase,0,true);
			Camera->SetWorldRotation(FRotator(13,110,0));
			auto** Cloud = Parts.FindByPredicate([](const auto* P){ return P->TranslucencySortPriority == -2399; });
			if (!TestTrue(TEXT("Rain has a foreground cloud sheet"),Cloud && (*Cloud)->IsVisible())) return false;
			TArray<FColor> With, Without;
			if (!ReadView(With)) return false;
			Camera->HideComponent(*Cloud);
			if (!ReadView(Without)) return false;
			Camera->HiddenComponents.Reset();
			int32 ChangedCloud = 0;
			for (int32 I=0; I<With.Num()/2; ++I)
				ChangedCloud += FMath::Abs(int32(With[I].B)-Without[I].B) >= 4;
			AddInfo(FString::Printf(TEXT("Rain clouds at %.2f change %d upper-sky pixels"),Phase,ChangedCloud));
			TestTrue(TEXT("Rain clouds render through peak haze"),ChangedCloud > With.Num()/30);
			if (Phase == .1f)
			{
				int32 Highlights = 0;
				for (int32 I=0; I<With.Num()/2; ++I)
					Highlights += With[I].B >= 28 && int32(With[I].B)-Without[I].B >= 8;
				TestTrue(TEXT("Night rain has readable cloud highlights"),Highlights > With.Num()/100);
			}
			SaveView(With,Phase == .1f ? TEXT("storm-clouds-night.png") : TEXT("storm-clouds-day.png"));
		}
		Camera->SetWorldRotation(FRotator(5,110,0));
		Owner->UpdateSky(.5f,0,true);
		if (!ReadView(Wet)) return false;
		SaveView(Wet,TEXT("coast-legions-storm.png"));
		auto* Rain=StockSlot(Owner,0x01004C44);
		TestTrue(TEXT("Server storm draws the outdoor after-pass"),Rain && Rain->Mesh && Rain->Mesh->IsVisible());
		int32 Changed=0;
		for (int32 I=0; I<Wet.Num()/2; ++I)
			if (FMath::Abs(Dry[I].R-Wet[I].R)+FMath::Abs(Dry[I].B-Wet[I].B)>10) ++Changed;
		TestTrue(TEXT("Rain changes the rendered Legions sky"),Changed>Wet.Num()/5);
		Owner->UpdateSky(.5f,0,false);
		if (!ReadView(StockWet)) return false;
		SaveView(StockWet,TEXT("coast-stock-storm.png"));
		Changed=0;
		for (int32 I=0; I<Wet.Num()/2; ++I)
			if (FMath::Abs(StockWet[I].R-Wet[I].R)+FMath::Abs(StockWet[I].B-Wet[I].B)>10) ++Changed;
		TestTrue(TEXT("Imported storm has its own visible sky"),Changed>Wet.Num()/5);
		// Toggle back through the normal actor path, injecting the test's cache.
		auto Reloaded=MakeShared<ACELegionsSky::FBackdrop>();
		if (!ACELegionsSky::Load(Cache,*Reloaded,Error)) return false;
		Owner->ImportedRevision=ACELegionsSky::Revision();
		Owner->PendingSky=MakeFulfilledPromise<TSharedPtr<ACELegionsSky::FBackdrop>>(Reloaded).GetFuture();
		SetClock(39755,0);
		Owner->ClearSky(true);
		if (!Owner->TryBuild()) return false;
		Owner->UpdateSky(.04f,0,true);
	}
	for (auto* Block : Coast) Block->Destroy();

	// Look over the streamed edge at the exact reported summit and facing.
	if (!RetailDir.IsEmpty())
	{
		FACEPosition Summit;
		Summit.CellId = 0xD5990036;
		Summit.Location = FVector(144.610855,120.631470,374.005005);
		Summit.RotationW = .573960f;
		Summit.RotationXYZ.Z = .818883f;
		const FVector Eye = Summit.ToUnrealLocation(100) + FVector(0,0,180);
		Owner->SetActorLocation(Eye);
		Camera->SetWorldLocationAndRotation(Eye, FRotator(-2,Summit.GetAceForwardVector().Rotation().Yaw,0));
		TSet<FIntPoint> Holes;
		const FIntPoint Center(0xd5,0x99);
		Coast.Reset();
		constexpr int32 LocalRadius = 5;
		for (int32 X=-LocalRadius; X<=LocalRadius; ++X)
			for (int32 Y=-LocalRadius; Y<=LocalRadius; ++Y)
			{
				const FIntPoint Block = Center + FIntPoint(X,Y);
				const uint32 Id = (uint32(Block.X)<<24) | (uint32(Block.Y)<<16);
				auto* Actor = World->SpawnActor<AACELandblockActor>();
				Owner->DatSubsystem->GetOrBuildLandblockMesh(Id,100);
				if (!Actor || !Actor->LoadLandblock(Id,100,false)) return false;
				Coast.Add(Actor);
				Holes.Add(Block);
			}
		auto* Far = World->GetSubsystem<UPersonalFarTerrainSubsystem>();
		if (!TestNotNull(TEXT("Summit far terrain subsystem"),Far)) return false;
		Far->Bake = PersonalFarTerrainBake::LoadOrBuildAsync(RetailDir).Get();
		if (!TestTrue(TEXT("Summit shell material"), Far->EnsureMesh(*Owner->DatSubsystem))) return false;
		auto Landscape = MakeShared<FPersonalFarTerrainMesh,ESPMode::ThreadSafe>();
		constexpr int32 FarRadius = 96;
		const int32 SceneryRings = FParse::Param(FCommandLine::Get(),TEXT("CompareFarScenery")) ? PersonalFarScenery::LastRing : 0;
		PersonalFarTerrainMesh::Build(*Far->Bake,Center,FarRadius,Holes,LocalRadius,100,*Landscape,nullptr,SceneryRings);
		Far->ApplyMesh(Landscape,FarRadius,MoveTemp(Holes),100,{});
		const auto* Region = Owner->DatSubsystem->GetRegionSkyInfo();
		int32 Storm = 39755;
		while (Storm < 40255 && Owner->PickDayGroup(*Region,Storm*7620.0-3600.0+3810.0) != 3) ++Storm;
		constexpr int32 ClearPreviewDay = 40557;
		struct FSummitView { const TCHAR* Name; int32 Day; float Time; bool Imported; };
		for (const FSummitView& State : {FSummitView{TEXT("clear"),ClearPreviewDay,.5f,true},
			{TEXT("clear-dawn"),ClearPreviewDay,.22f,true}, {TEXT("clear-dusk"),ClearPreviewDay,.90f,true},
			{TEXT("clear-night"),ClearPreviewDay,.04f,true},
			{TEXT("rain-day"),Storm,.5f,true}, {TEXT("rain-night"),Storm,.1f,true},
			{TEXT("ac1-rain-day"),Storm,.5f,false}, {TEXT("ac1-rain-night"),Storm,.1f,false},
			{TEXT("ac1-dawn"),39755,.18f,false}, {TEXT("ac1-noon"),ClearPreviewDay,.5f,false},
			{TEXT("ac1-dusk"),ClearPreviewDay,.90f,false}, {TEXT("ac1-night"),ClearPreviewDay,.04f,false}})
		{
			SetClock(State.Day,State.Time);
			Owner->ClearSky(true);
			if (!Owner->TryBuild()) return false;
			for (const bool Extended : {false,true})
			{
				Far->SetShown(Extended);
				Owner->FogClampLandblocks = float(Extended ? FarRadius : LocalRadius);
				Owner->UpdateSky(State.Time,0,State.Imported);
				TestTrue(TEXT("Summit fog reaches full opacity before the terrain ends"),
					Owner->DatSubsystem->GetWorldFogEndCm() <= Owner->FogClampLandblocks * 19200.f);
				if (Extended)
				{
					TestTrue(TEXT("Fog is limited by actual built coverage from the summit camera"),
						Owner->DatSubsystem->GetWorldFogEndCm() <= Far->FogCoverageDistanceCm(Eye));
					if (State.Imported)
						TestTrue(TEXT("Imported palette retains at least its own authored visibility inside available coverage"),
							Owner->DatSubsystem->GetWorldFogEndCm() + 1.f >= FMath::Min(
								Owner->ImportedSky->FogMax * 100.f,Far->FogCoverageDistanceCm(Eye)));
				}
				if (State.Day == Storm && Extended && State.Time == .5f)
					TestEqual(TEXT("Legions rain keeps the AC1 rainy-day terrain range"),
						Owner->DatSubsystem->GetWorldFogEndCm(),150000.f);
				if (State.Imported && State.Day == ClearPreviewDay && Extended && State.Time == .9f)
					TestEqual(TEXT("Clear dusk retains extended visibility instead of an orange short-range wall"),
						Owner->DatSubsystem->GetWorldFogEndCm(),600000.f);
				TArray<FColor> View;
				if (!ReadView(View)) return false;
				SaveView(View,*FString::Printf(TEXT("summit-%s-%s.png"),State.Name,Extended ? TEXT("far") : TEXT("near")));
				if (!Extended)
				{
					// Add real terrain beyond the clipped edge without changing the fog.
					// Fully extinguished ground must match the lower sky it replaces.
					Far->SetShown(true);
					TArray<FColor> WithGround;
					if (!ReadView(WithGround)) return false;
					int32 EdgePixels = 0;
					for (int32 I=View.Num()/2; I<View.Num(); ++I)
						EdgePixels += FMath::Max3(FMath::Abs(View[I].R-WithGround[I].R),
							FMath::Abs(View[I].G-WithGround[I].G),FMath::Abs(View[I].B-WithGround[I].B)) >= 6;
					AddInfo(FString::Printf(TEXT("Summit %s edge: %d lower-view pixels differ by six levels"),State.Name,EdgePixels));
					TestTrue(TEXT("The exhausted terrain range blends into the horizon"),EdgePixels < View.Num()/100);
				}
				else if (State.Imported && State.Day == ClearPreviewDay && State.Time == .5f)
				{
					TestEqual(TEXT("Summit preview selected the reported weather"), Owner->ActiveGroup.DayName, FString(TEXT("Clear")));
					// Sample the unobstructed upper sky, above the fogged horizon.
					double SkySum = 0.;
					const int32 SkyPixels = Target->SizeX * (Target->SizeY / 4);
					for (int32 I = 0; I < SkyPixels; ++I)
						SkySum += .2126 * View[I].R + .7152 * View[I].G + .0722 * View[I].B;
					AddInfo(FString::Printf(TEXT("Clear noon summit upper sky: %.2f / 255"), SkySum / SkyPixels));
					TestTrue(TEXT("Clear noon retains the brightness of the imported daytime sky"), SkySum / SkyPixels >= 133.);
					TestTrue(TEXT("Far terrain receives the imported shadow texture"),
						Far->ShellMaterial->K2_GetTextureParameterValue(TEXT("CloudMask")) == Owner->ImportedSky->CloudMask.Get());
					if (!TestTrue(TEXT("Far detail atlas material exists"),Far->EnsureDetailTexture())) return false;
					TestTrue(TEXT("Newly streamed far detail receives the same shadow mask and ambient light"),
						Far->DetailMaterial->K2_GetTextureParameterValue(TEXT("CloudMask")) == Owner->ImportedSky->CloudMask.Get() &&
						Far->DetailMaterial->K2_GetVectorParameterValue(TEXT("LandSkyTint")) == Far->ShellMaterial->K2_GetVectorParameterValue(TEXT("LandSkyTint")) &&
						Far->DetailMaterial->K2_GetScalarParameterValue(TEXT("EmissiveStrength")) == Far->ShellMaterial->K2_GetScalarParameterValue(TEXT("EmissiveStrength")));
					const FLinearColor Motion = Far->ShellMaterial->K2_GetVectorParameterValue(TEXT("CloudMotion"));
					TestTrue(TEXT("Far terrain shadow strength matches the cloud artwork"),FMath::IsNearlyEqual(Motion.A,RenderedCloudOpacity(Owner),1.f/255.f));
					TestEqual(TEXT("Newly streamed far detail inherits the current shadow strength"),
						Far->DetailMaterial->K2_GetVectorParameterValue(TEXT("CloudMotion")).A,Motion.A);
					Far->ShellMaterial->SetVectorParameterValue(TEXT("CloudMotion"),FLinearColor::Transparent);
					TArray<FColor> Unshadowed;
					if (!ReadView(Unshadowed)) return false;
					Far->ShellMaterial->SetVectorParameterValue(TEXT("CloudMotion"),Motion);
					int32 Shaded = 0;
					for (int32 I=View.Num()/2; I<View.Num(); ++I)
						Shaded += int32(Unshadowed[I].G)-View[I].G >= 3;
					AddInfo(FString::Printf(TEXT("Clouds darken %d distant terrain pixels at the summit"),Shaded));
					if (Motion.A > .1f) TestTrue(TEXT("Visible clouds cast shadows beyond the local terrain"),Shaded > 100);
					else if (Motion.A == 0.f) TestEqual(TEXT("Clear sky leaves distant terrain unshadowed"),Shaded,0);
					// Hold the camera, clock, holes and material fixed while the far
					// shell catches up to an adjacent player landblock. This isolates
					// geometry LOD movement from a sky turn or changed weather.
					const TSet<FIntPoint> StableHoles = Far->BuiltHoles;
					auto Recentered = MakeShared<FPersonalFarTerrainMesh,ESPMode::ThreadSafe>();
					PersonalFarTerrainMesh::Build(*Far->Bake,Center + FIntPoint(1,0),FarRadius,
						StableHoles,LocalRadius,100,*Recentered,nullptr,SceneryRings);
					Far->ApplyMesh(Recentered,FarRadius,TSet<FIntPoint>(StableHoles),100,{});
					Far->SetShown(true);
					TArray<FColor> ShiftedView;
					if (!ReadView(ShiftedView)) return false;
					SaveView(ShiftedView,TEXT("summit-clear-recenter.png"));
					int32 RecenterPixels = 0;
					for (int32 I=0; I<View.Num(); ++I)
						RecenterPixels += FMath::Max3(FMath::Abs(View[I].R-ShiftedView[I].R),
							FMath::Abs(View[I].G-ShiftedView[I].G),FMath::Abs(View[I].B-ShiftedView[I].B)) >= 6;
					AddInfo(FString::Printf(TEXT("Fixed-camera terrain recenter: %d / %d pixels change by six levels"),RecenterPixels,View.Num()));
					Far->ApplyMesh(Landscape,FarRadius,TSet<FIntPoint>(StableHoles),100,{});
					Far->SetShown(true);
				}
			}
		}
		// Force an extinction interval beyond the shell, then move the camera
		// while retaining the old mesh as an asynchronous rebuild would. The
		// shell's old center must never be mistaken for the new viewer center.
		Far->SetShown(true);
		Owner->FogClampLandblocks = 0.f;
		FACEDatSkyTimeOfDay LongRange;
		LongRange.MaxWorldFog = 100000.f;
		LongRange.WorldFogColor = 0xFF8090A0;
		for (const FVector Offset : {FVector::ZeroVector,FVector(30000,0,0),FVector(-30000,25000,0)})
		{
			Owner->SetActorLocation(Eye + Offset);
			Owner->UpdateFog(LongRange,LongRange,0.f);
			TestTrue(TEXT("Delayed recenter and camera boom cannot expose the shell cutoff"),
				FMath::IsNearlyEqual(Owner->DatSubsystem->GetWorldFogEndCm(),
					Far->FogCoverageDistanceCm(Eye + Offset),1.f));
		}
		Owner->SetActorLocation(Eye);
		// Reproduce the user's 96-block summit view with actual streamed
		// scenery. Hold the sky, shell geometry, projection and clock fixed
		// while measuring the extra silhouettes beyond the old radius.
		if (FParse::Param(FCommandLine::Get(),TEXT("CompareFarScenery")))
		{
			auto* Scenery = World->GetSubsystem<UPersonalFarScenerySubsystem>();
			auto* Radius = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.Radius"));
			const int32 SavedRadius = Radius->GetInt();
			ON_SCOPE_EXIT { Radius->SetWithCurrentPriority(SavedRadius); Scenery->ClearAll(); };
			auto ScenerySky = MakeShared<ACELegionsSky::FBackdrop>();
			if (!ACELegionsSky::Load(Cache,*ScenerySky,Error)) return false;
			Owner->ResetImportedSky();
			Owner->ImportedRevision=ACELegionsSky::Revision();
			Owner->PendingSky=MakeFulfilledPromise<TSharedPtr<ACELegionsSky::FBackdrop>>(ScenerySky).GetFuture();
			SetClock(ClearPreviewDay,.5);
			Owner->ClearSky(true);
			if (!Owner->TryBuild()) return false;
			Far->SetShown(true);
			Camera->SetWorldRotation(FRotator(-7,Summit.GetAceForwardVector().Rotation().Yaw,0));
			Owner->FogClampLandblocks=FarRadius;
			Owner->UpdateSky(.5f,0,true);
			TArray<FColor> PreviousView;
			int32 PreviousInstances=0;
			for (int32 Rings : {8,PersonalFarScenery::LastRing})
			{
				Radius->SetWithCurrentPriority(Rings);
				const double Start=FPlatformTime::Seconds();
				do
				{
					++GFrameCounter;
					Scenery->Stream(*Owner->DatSubsystem,Far->Bake,Center,100,Owner->DatSubsystem->GetWorldFogEndCm(),
						[Center](FIntPoint P){ return PersonalFarScenery::Ring(P,Center)<=LocalRadius; });
					FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
					FPlatformProcess::SleepNoStats(.001f);
				} while (!Scenery->bComplete && FPlatformTime::Seconds()-Start<180.);
				if (!TestTrue(TEXT("Summit scenery finished streaming"),Scenery->bComplete)) return false;
				const auto Stats=Scenery->GetStats();
#if WITH_EDITOR
				FStaticMeshCompilingManager::Get().FinishAllCompilation();
				if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
#endif
				TArray<FColor> View;
				if (!ReadView(View)) return false;
				SaveView(View,*FString::Printf(TEXT("summit-objects-%d.png"),Rings));
				AddInfo(FString::Printf(TEXT("Summit radius %d: %d blocks, %d instances, %d components, %.2f s"),
					Rings,Stats.InSet,Stats.Instances,Stats.Components,FPlatformTime::Seconds()-Start));
				if (!PreviousView.IsEmpty())
				{
					int32 Changed=0;
					for (int32 I=View.Num()/2; I<View.Num(); ++I)
						Changed+=FMath::Abs(int32(View[I].G)-PreviousView[I].G)>5;
					TestTrue(TEXT("More distant objects reach the renderer"),Stats.Instances>PreviousInstances && Changed>5);
					AddInfo(FString::Printf(TEXT("Extended summit scenery changes %d lower-view pixels"),Changed));
				}
				PreviousInstances=Stats.Instances;
				PreviousView=MoveTemp(View);
			}
			// With visibility exhausted in front of the valley, the silhouettes
			// must disappear into the same fog color as the ground behind them.
			Owner->DatSubsystem->SetWorldDistanceFog(0.f,100.f,Owner->ImportedSky->FogColor,1.f);
			TArray<FColor> FogWithObjects, FogWithoutObjects;
			if (!ReadView(FogWithObjects)) return false;
			Scenery->ClearAll();
			if (!ReadView(FogWithoutObjects)) return false;
			int32 VisibleThroughFog = 0;
			for (int32 I=FogWithObjects.Num()/2; I<FogWithObjects.Num(); ++I)
				VisibleThroughFog += FMath::Abs(int32(FogWithObjects[I].G)-FogWithoutObjects[I].G)>5;
			AddInfo(FString::Printf(TEXT("Fully fogged summit scenery: %d lower-view pixels differ"),VisibleThroughFog));
			TestTrue(TEXT("Distant scenery fades into the ground fog"),VisibleThroughFog < 100);
			SaveView(FogWithObjects,TEXT("summit-objects-full-fog.png"));
		}
		Far->SetShown(false);
		Owner->FogClampLandblocks = 0.f;
		for (auto* Block : Coast) Block->Destroy();
		Owner->ResetImportedSky();
		auto Reloaded = MakeShared<ACELegionsSky::FBackdrop>();
		if (!ACELegionsSky::Load(Cache,*Reloaded,Error)) return false;
		Owner->ImportedRevision = ACELegionsSky::Revision();
		Owner->PendingSky = MakeFulfilledPromise<TSharedPtr<ACELegionsSky::FBackdrop>>(Reloaded).GetFuture();
		Owner->UpdateSky(.04f,0,true);
	}
	Camera->ShowFlags.SetTonemapper(false);
	Owner->SetActorLocation(FVector::ZeroVector);
	Camera->SetWorldLocation(FVector::ZeroVector);
	Target->TargetGamma = 0.f;
	Target->InitCustomFormat(512,320,PF_B8G8R8A8,false);
	Target->UpdateResourceImmediate(true);
	FVector MorningDirection;
	for (const float Time : {.30f,.8f})
	{
		Owner->UpdateImportedSky(Time,true);
		Owner->UpdateWorldLighting(Stock,Stock,0,Time);
		if (!TestNotNull(TEXT("Directional world light"),Owner->SunLightActor.Get())) return false;
		const auto Direction=Owner->SunLightActor->GetActorForwardVector();
		TestTrue(TEXT("World shadows follow the imported light"),Direction.Equals(Owner->ImportedSky->Current.Direction,.003));
		TestTrue(TEXT("Light travels down at an angle"),Direction.Z<0 && Direction.Size2D()>.1);
		TestTrue(TEXT("Direct intensity uses the AC2 key"),FMath::IsNearlyEqual(Owner->SunLightActor->GetLightComponent()->Intensity,2.6f));
		if (Time<.5f) MorningDirection=Direction;
		else TestTrue(TEXT("Shadows move between morning and evening"),!Direction.Equals(MorningDirection,.15));
	}
	// Exercise the material used by both existing and newly streamed land tiles.
	// Keep sun, camera and base texture fixed so pixel differences isolate the mask.
	UStaticMesh* Shapes[] = {
		LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane")),
		LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube"))};
	if (!TestNotNull(TEXT("Ground mesh"),Shapes[0]) || !TestNotNull(TEXT("Caster mesh"),Shapes[1])) return false;
#if WITH_EDITOR
	// Mobile preview can still be compiling the fixture meshes during the synchronous captures.
	FStaticMeshCompilingManager::Get().FinishCompilation(Shapes);
#endif
	auto* Floor=NewObject<UStaticMeshComponent>(Owner);
	Floor->SetStaticMesh(Shapes[0]);
	auto* Land=Owner->DatSubsystem->GetOrCreateLandMaterial(100,{FColor(160,160,160)},1,1);
	if (!TestNotNull(TEXT("Terrain material"),Land)) return false;
	Floor->SetMaterial(0,Land);
	Floor->SetWorldScale3D(FVector(2000,2000,1));
	Floor->RegisterComponent();
	// The overhead probe moves independently of the player-centered sky. Capture
	// only its receiver and caster so backdrop geometry cannot cover the fixture.
	Camera->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Camera->ShowOnlyComponent(Floor);
	auto WaitForProbeMesh = [World](UStaticMeshComponent* Mesh)
	{
		// Cooked builds can defer the scene proxy until shader pipeline precaching
		// completes. A synchronous capture does not pump its game-thread callback.
		const double Deadline = FPlatformTime::Seconds() + 30.;
		do
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			World->SendAllEndOfFrameUpdates();
			FlushRenderingCommands();
			if (Mesh->SceneProxy && !Mesh->IsPSOPrecaching()) return true;
			FPlatformProcess::SleepNoStats(.001f);
		} while (FPlatformTime::Seconds() < Deadline);
		return false;
	};
	if (!TestTrue(TEXT("Cloud-shadow receiver is ready for capture"), WaitForProbeMesh(Floor))) return false;
	Camera->SetWorldLocationAndRotation(FVector(0,0,40000),FRotator(-90,0,0));
	Owner->DatSubsystem->SetWorldDistanceFog(1.e7f,2.e7f,FLinearColor::Black,0);
#if WITH_EDITOR
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
#endif
	auto CaptureLand=[&](TArray<FColor>& Pixels)
	{
		World->SendAllEndOfFrameUpdates();
		Camera->CaptureScene();
		FlushRenderingCommands();
		return Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
	};
	TArray<FColor> Clouded,Drifted,Faded,ZeroStrength,Clear;
	FLinearColor ShadowMotion = Owner->ImportedSky->CloudMotion;
	ShadowMotion.R /= FMath::Max(1.f,Owner->WorldScale);
	ShadowMotion.A = 1.f;
	// Use an explicit opaque cloud mask for this material probe; the native
	// cloud sheet may legitimately be asleep at the last orbit-test phase.
	Owner->DatSubsystem->SetLandSky(Owner->ImportedSky->CloudMask.Get(),ShadowMotion,Owner->ImportedSky->Current.AmbientColor);
	Owner->DatSubsystem->SetSkyAnimationTime(Session->GetPortalYearTicks());
	if (!TestTrue(TEXT("Capture terrain under imported clouds"),CaptureLand(Clouded))) return false;
	// Advance the sky clock while engine time is unchanged. Real time alone
	// must no longer move the terrain mask independently of the sky.
	Owner->DatSubsystem->SetSkyAnimationTime(Session->GetPortalYearTicks()+15.);
	if (!TestTrue(TEXT("Capture terrain after cloud drift"),CaptureLand(Drifted))) return false;
	Owner->DatSubsystem->SetSkyAnimationTime(Session->GetPortalYearTicks());
	ShadowMotion.A = .25f;
	Owner->DatSubsystem->SetLandSky(Owner->ImportedSky->CloudMask.Get(),ShadowMotion,Owner->ImportedSky->Current.AmbientColor);
	if (!TestTrue(TEXT("Capture terrain under fading clouds"),CaptureLand(Faded))) return false;
	ShadowMotion.A = 0.f;
	Owner->DatSubsystem->SetLandSky(Owner->ImportedSky->CloudMask.Get(),ShadowMotion,Owner->ImportedSky->Current.AmbientColor);
	if (!TestTrue(TEXT("Capture terrain after cloud opacity reaches zero"),CaptureLand(ZeroStrength))) return false;
	auto* NewLand = Owner->DatSubsystem->GetOrCreateLandMaterial(101,{FColor(160,160,160)},1,1);
	if (!TestNotNull(TEXT("New terrain material created after clouds clear"),NewLand)) return false;
	TestEqual(TEXT("Newly streamed terrain inherits zero cloud opacity"),CastChecked<UMaterialInstanceDynamic>(NewLand)->K2_GetVectorParameterValue(TEXT("CloudMotion")).A,0.f);
	Owner->DatSubsystem->SetLandSky(nullptr,FLinearColor::Transparent,Owner->ImportedSky->Current.AmbientColor);
	if (!TestTrue(TEXT("Capture identical terrain without clouds"),CaptureLand(Clear))) return false;
	int32 Darkened=0, Moved=0, MinDelta=255, MaxDelta=0, ZeroChanged=0, Intermediate=0;
	int64 FullDarkening=0, FadedDarkening=0;
	for (int32 I=0; I<Clear.Num(); ++I)
	{
		const int32 Delta=int32(Clear[I].R)-Clouded[I].R;
		MinDelta=FMath::Min(MinDelta,Delta); MaxDelta=FMath::Max(MaxDelta,Delta);
		Darkened+=Delta>2;
		Moved+=FMath::Abs(int32(Clouded[I].R)-Drifted[I].R)>1;
		ZeroChanged+=FMath::Max3(FMath::Abs(int32(Clear[I].R)-ZeroStrength[I].R),
			FMath::Abs(int32(Clear[I].G)-ZeroStrength[I].G),FMath::Abs(int32(Clear[I].B)-ZeroStrength[I].B))>1;
		const int32 FadedDelta=int32(Clear[I].R)-Faded[I].R;
		FullDarkening+=FMath::Max(0,Delta); FadedDarkening+=FMath::Max(0,FadedDelta);
		Intermediate+=Delta>8 && FadedDelta>0 && FadedDelta<Delta-2;
	}
	SaveView(Clouded,TEXT("terrain-clouded.png"));
	SaveView(Clear,TEXT("terrain-clear.png"));
	SaveView(Faded,TEXT("terrain-cloud-fade.png"));
	SaveView(ZeroStrength,TEXT("terrain-cloud-zero.png"));
	AddInfo(FString::Printf(TEXT("Cloud shadow capture: %d darkened, %d moved, delta %d to %d"),
		Darkened,Moved,MinDelta,MaxDelta));
	TestTrue(TEXT("The native mask casts spatially varying shadows on the ground"),
		Darkened>Clear.Num()/10 && MaxDelta-MinDelta>8);
	TestTrue(TEXT("Cloud shadows drift over stationary terrain"),Moved>Clear.Num()/20);
	TestEqual(TEXT("A retained mask with zero cloud opacity renders exactly like a clear sky"),ZeroChanged,0);
	TestTrue(TEXT("Partially transparent clouds render weaker ground shadows than opaque clouds"),
		Intermediate>Clear.Num()/10 && FadedDarkening>FullDarkening/20 && FadedDarkening<FullDarkening/2);
	// Under the strongly colored sunset key, the bounded fog should change
	// distant terrain while leaving surface contrast visible.
	Owner->UpdateImportedSky(.90f,true);
	Owner->UpdateWorldLighting(Stock,Stock,0,.90f);
	Owner->UpdateFog(Stock,Stock,0);
	const float SunsetEnd = Owner->DatSubsystem->GetWorldFogEndCm();
	TestTrue(TEXT("The imported weather sets a world-space visibility distance"),SunsetEnd > 1000.f);
	const FLinearColor FogBefore=Owner->ImportedSky->FogColor;
	TestTrue(TEXT("Exercise a warm sunset fog"),FogBefore.R > 3.f*FogBefore.B);
	Camera->SetWorldLocationAndRotation(FVector(0,0,SunsetEnd * .6f),FRotator(-90,0,0));
	TArray<FColor> Hazy,Unfogged;
	if (!CaptureLand(Hazy)) return false;
	Owner->DatSubsystem->SetWorldDistanceFog(0.f,SunsetEnd,FogBefore,0.f);
	if (!CaptureLand(Unfogged)) return false;
	Owner->UpdateFog(Stock,Stock,0);
	int32 FoggedPixels=0,MinLand=255,MaxLand=0;
	for (int32 I=0; I<Hazy.Num(); ++I)
	{
		FoggedPixels+=FMath::Abs(Hazy[I].R-Unfogged[I].R)>2;
		MinLand=FMath::Min(MinLand,int32(Hazy[I].G));
		MaxLand=FMath::Max(MaxLand,int32(Hazy[I].G));
	}
	TestTrue(TEXT("Sunset fog reaches the distant landscape"),FoggedPixels>Hazy.Num()/4);
	TestTrue(TEXT("Distant terrain retains cloud contrast under the haze"),MaxLand-MinLand>4);
	// A player-height caster on level ground must cast a visible shadow that
	// changes direction with the orb. Isolate its shadow with paired captures.
	auto* Caster=NewObject<UStaticMeshComponent>(Owner);
	Caster->SetStaticMesh(Shapes[1]);
	Caster->SetWorldLocation(FVector(0,0,100));
	Caster->SetWorldScale3D(FVector(.5,.5,2));
	Caster->RegisterComponent();
	Camera->ShowOnlyComponent(Caster);
	if (!TestTrue(TEXT("Directional shadow caster is ready for capture"), WaitForProbeMesh(Caster))) return false;
	Camera->SetWorldLocationAndRotation(FVector(0,0,900),FRotator(-90,0,0));
	FVector2D FirstCenter;
	for (const float Time : {.30f,.8f})
	{
		Owner->UpdateImportedSky(Time,true);
		Owner->UpdateWorldLighting(Stock,Stock,0,Time);
		Caster->SetCastShadow(true);
		TArray<FColor> With,Without;
		if (!CaptureLand(With)) return false;
		Caster->SetCastShadow(false);
		if (!CaptureLand(Without)) return false;
		FVector2D Sum=FVector2D::ZeroVector;
		int32 Count=0;
		for (int32 I=0; I<With.Num(); ++I)
		{
			if (int32(Without[I].R)-With[I].R<=5) continue;
			Sum+=FVector2D(I%Target->SizeX,I/Target->SizeX); ++Count;
		}
		if (!TestTrue(TEXT("Caster produces a ground shadow"),Count>20)) return false;
		const auto Center=Sum/Count;
		if (Time<.5f) FirstCenter=Center;
		else TestTrue(TEXT("Rendered shadow moves across the ground"),(Center-FirstCenter).Size()>8);
	}
	Owner->DatSubsystem->SetIndoorEnvironment(true);
	TestFalse(TEXT("Enclosed interiors keep their AC1 sky handling"), Owner->UpdateImportedSky(.5f, true));
	Owner->UpdateFog(Stock,Stock,0);
	TestEqual(TEXT("Indoor switch restores AC1 visibility"),Owner->DatSubsystem->GetWorldFogEndCm(),1.f);
	Owner->DatSubsystem->SetIndoorEnvironment(false);
	TestTrue(TEXT("Returning outdoors restores the AC2 sky"), Owner->UpdateImportedSky(.5f, true));
	Owner->UpdateFog(Stock,Stock,0);
	TestTrue(TEXT("Legions world fog resumes outdoors"),Owner->DatSubsystem->GetWorldFogEndCm() > 1000.f);
	// Verify both imported texture stages travel in AC1's world direction.
	SetClock(39762,.5);
	Owner->ClearSky(true); Owner->TryBuild();
	Retail->ClearSky(); Retail->TryBuild();
	Owner->UpdateSky(.5f,0,true); Retail->UpdateSky(.5f,0,false);
	const AACESkyDomeActor::FSkySlot* Wind = nullptr;
	float WindOpacity = -1.f;
	for (const auto& Slot : Retail->Slots)
		if (!Slot.LocalCloudTravel.IsNearlyZero() && Slot.Mesh && Slot.Mesh->IsVisible())
		{
			float Opacity = Slot.Mids[0]->K2_GetScalarParameterValue(TEXT("OpacityMul"));
			if (Opacity > WindOpacity) { Wind = &Slot; WindOpacity = Opacity; }
		}
	if (!TestNotNull(TEXT("AC1 cloud supplies world wind"),Wind)) return false;
	const FVector WindDirection = Wind->Mesh->GetComponentQuat().RotateVector(Wind->LocalCloudTravel).GetSafeNormal2D();
	TInlineComponentArray<UProceduralMeshComponent*> MovingParts(Owner);
	UProceduralMeshComponent* CloudPart = nullptr;
	int32 MovingCloudParts = 0;
	for (auto* Part : MovingParts)
		if (Part->IsVisible())
			if (auto* Mid = Cast<UMaterialInstanceDynamic>(Part->GetMaterial(0)))
			{
				FLinearColor Stages;
				// K2's missing-parameter fallback has alpha 1, so it also matched
				// stationary classic/fill materials after rebuilding the sky.
				if (Mid->GetVectorParameterValue(FMaterialParameterInfo(TEXT("StageOps")),Stages) && Stages.A > .5f)
				{
					CloudPart = Part;
					++MovingCloudParts;
				}
			}
	TestEqual(TEXT("Exactly one active imported two-stage cloud sheet"),MovingCloudParts,1);
	if (!TestNotNull(TEXT("Imported two-stage cloud sheet is visible"),CloudPart)) return false;
	auto* CloudMid = CastChecked<UMaterialInstanceDynamic>(CloudPart->GetMaterial(0));
	ACESkyMotion::FUVBasis Basis;
	const auto* Section = CloudPart->GetProcMeshSection(0);
	TArray<FVector> Positions; TArray<FVector2D> Coordinates; TArray<int32> Indices;
	for (const auto& Vertex : Section->ProcVertexBuffer) { Positions.Add(Vertex.Position); Coordinates.Add(Vertex.UV0); }
	for (uint32 Index : Section->ProcIndexBuffer) Indices.Add(Index);
	Basis.Consider(Positions,Coordinates,Indices);
	const auto StartOffset = CloudMid->K2_GetVectorParameterValue(TEXT("UVOffset"));
	SetClock(39762,.5+1./7620.); Owner->UpdateSky(.5f,0,true);
	const auto EndOffset = CloudMid->K2_GetVectorParameterValue(TEXT("UVOffset"));
	auto Delta = [](float A,float B) { double D=B-A; return D-FMath::RoundToDouble(D); };
	for (const FVector2D Motion : {FVector2D(Delta(StartOffset.R,EndOffset.R),Delta(StartOffset.G,EndOffset.G)),
		FVector2D(Delta(StartOffset.B,EndOffset.B),Delta(StartOffset.A,EndOffset.A))})
	{
		const FVector Travel = CloudPart->GetComponentQuat().RotateVector(Basis.Travel(Motion)).GetSafeNormal2D();
		AddInfo(FString::Printf(TEXT("Imported cloud motion: delta=(%.9f,%.9f) travel=%s wind=%s dot=%.6f"),
			Motion.X,Motion.Y,*Travel.ToString(),*WindDirection.ToString(),FVector::DotProduct(Travel,WindDirection)));
		TestTrue(TEXT("Imported cloud stages move with AC1's clouds"),FVector::DotProduct(Travel,WindDirection)>.999);
	}
	const auto MaskMotion=Owner->ImportedSky->CloudMotion;
	TestTrue(TEXT("Terrain cloud shadows travel with the clouds"),
		FVector::DotProduct(FVector(-MaskMotion.G,-MaskMotion.B,0).GetSafeNormal2D(),WindDirection)>.999);
	// Image evidence: a large sky-time step with no engine-time step must
	// visibly animate the composed imported sky, not just its status output.
	Floor->SetVisibility(false); Caster->SetVisibility(false);
	Camera->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
	Camera->ClearShowOnlyComponents();
	Camera->SetWorldLocation(Owner->GetActorLocation());
	Camera->SetWorldRotation(FRotator(65,0,0));
	TArray<FColor> MotionBefore,MotionAfter;
	ReadView(MotionBefore);
	SetClock(39762,.5+64./7620.); Owner->UpdateSky(.5f,0,true);
	ReadView(MotionAfter);
	int32 MotionPixels=0;
	for (int32 I=0; I<MotionBefore.Num(); ++I)
		MotionPixels += FMath::Abs(int32(MotionBefore[I].R)-MotionAfter[I].R)>3;
	SaveView(MotionBefore,TEXT("timelapse-before.png")); SaveView(MotionAfter,TEXT("timelapse-64x-after.png"));
	AddInfo(FString::Printf(TEXT("64x sky animation changes %d pixels with engine time fixed"),MotionPixels));
	TestTrue(TEXT("Timelapse visibly moves the imported sky"),MotionPixels>MotionBefore.Num()/100);
	{
		const float SavedOption=ACERuntimeOptions::Get(TEXT("LegionsSky"));
		ACERuntimeOptions::Set(TEXT("LegionsSky"),1);
		ACERuntimeOptions::Apply();
		ON_SCOPE_EXIT { ACERuntimeOptions::Set(TEXT("LegionsSky"),SavedOption); ACERuntimeOptions::Apply(); };
		TArray<int32> Days{39755,40551};
		const auto* Region=Owner->DatSubsystem->GetRegionSkyInfo();
		TSet<int32> Covered;
		for (int32 Day=39755; Day<40255 && Covered.Num()<Region->DayGroups.Num(); ++Day)
		{
			const int32 Group=Owner->PickDayGroup(*Region,Day*7620.0-3600.0+3810.0);
			if (!Covered.Contains(Group)) Days.AddUnique(Day);
			Covered.Add(Group);
		}
		TestEqual(TEXT("Exercise every retail forecast"),Covered.Num(),Region->DayGroups.Num());
		Days.Add(39755);
		for (const int32 Day : Days)
		{
			SetClock(Day,0);
			Retail->ClearSky();
			if (!Retail->TryBuild()) return false;
			// Traverse midnight, both moon tracks, daybreak, sunset, and the
			// reported rainy phase. Repeat the clear day after a backward sync.
			for (const float Fraction : {.001f,.035f,.10f,.17f,.215f,.24f,.50f,.856f,.93f,.999f})
			{
				SetClock(Day,Fraction);
				Owner->Tick(.51f);
				const float Time=Owner->LastImportedTime;
				TestTrue(TEXT("Sky sampled the synchronized server phase"),FMath::IsNearlyEqual(Time,Fraction,.001f));
				Retail->UpdateSky(Time,0,false);
				TestEqual(TEXT("Imported and stock sky select the same weather group"),Owner->ActiveGroup.DayName,Retail->ActiveGroup.DayName);
				bool CloudCover=false;
				for (const auto& Slot : Retail->Slots)
				{
					const uint32 Gfx=Slot.Def.DefaultGfxObjectId;
					if (!Slot.Mesh) continue;
					if (Slot.Mesh->IsVisible() && (Gfx==0x01004C44 || Gfx==0x01004C42 ||
						(!Slot.bWeather && !Slot.bAfterPass && (!FMath::IsNearlyZero(Slot.Def.TexVelocityX) || !FMath::IsNearlyZero(Slot.Def.TexVelocityY)))))
						CloudCover=true;
					if (!Slot.bWeather && !Slot.bAfterPass && (Slot.Def.TexVelocityX!=0.f || Slot.Def.TexVelocityY!=0.f))
					{
						const auto& Actual=Owner->Slots[Slot.ObjectIndex];
						TestFalse(TEXT("Stock cloud deck is replaced by Legions weather"),Actual.Mesh && Actual.Mesh->IsVisible());
					}
					if (Gfx==0x01001348 || Gfx==0x01001F6A)
					{
						const FVector Center=OrbCenters.FindChecked(Gfx==0x01001348 ? 0x04000023u : 0x040000b6u);
						TInlineComponentArray<UProceduralMeshComponent*> Parts(Owner);
						auto* Part=Parts.FindByPredicate([&](auto* M)
						{ const auto* S=M->GetProcMeshSection(0); return S && S->SectionLocalBox.GetCenter().Equals(Center,2.); });
						if (!TestNotNull(TEXT("Imported orb still present after weather rollover"),Part)) return false;
						TestEqual(TEXT("Server time controls orb rise/set"),(*Part)->IsVisible(),Slot.Mesh->IsVisible());
						if (Slot.Mesh->IsVisible())
						{
							const FVector Direction=Slot.Mesh->GetComponentQuat().RotateVector(Slot.LocalBodyDirection).GetSafeNormal();
							const FVector BodyCenter=OrbBodyCenters.FindChecked(Gfx==0x01001348 ? 0x04000023u : 0x040000b6u);
							TestTrue(TEXT("Stock and imported orbits stay aligned"),Direction.Equals(
								((*Part)->GetComponentTransform().TransformPosition(BodyCenter)-Owner->GetActorLocation()).GetSafeNormal(),.001));
						}
					}
					else if (Gfx==0x01001F67 || Slot.bWeather || Slot.bAfterPass)
					{
						const auto& Actual=Owner->Slots[Slot.ObjectIndex];
						TestEqual(TEXT("Second moon and precipitation follow stock visibility"),
							Actual.Mesh && Actual.Mesh->IsVisible(),Slot.Mesh->IsVisible());
					}
				}
				const float DrawnCloudOpacity=RenderedCloudOpacity(Owner);
				TestEqual(TEXT("Ground shadow presence follows the imported clouds actually drawn"),Owner->bImportedClouds,DrawnCloudOpacity>KINDA_SMALL_NUMBER);
				TestEqual(TEXT("Renderer shadow strength follows the visible cloud material alpha"),Owner->ImportedSky->CloudShadowOpacity,DrawnCloudOpacity);
				TestTrue(TEXT("Weather without cloud cover cannot retain imported ground shadows"),CloudCover||!Owner->bImportedClouds);
				TestTrue(TEXT("Weather and time transitions update existing terrain shadow strength"),FMath::IsNearlyEqual(CastChecked<UMaterialInstanceDynamic>(Land)->K2_GetVectorParameterValue(TEXT("CloudMotion")).A,DrawnCloudOpacity,1.f/255.f));
			}
		}
	}
	TestFalse(TEXT("Turning the option off restores AC1"), Owner->UpdateImportedSky(.5f, false));
	TestNull(TEXT("Switching to AC1 removes the imported fog"),Owner->FindComponentByClass<UExponentialHeightFogComponent>());
	Owner->UpdateWorldLighting(Stock,Stock,0,.5f);
	TestTrue(TEXT("Stock light direction is restored"),Owner->SunLightActor->GetActorForwardVector().Equals(FVector::DownVector,.003));
	TInlineComponentArray<UProceduralMeshComponent*> Remaining(Owner);
	TestTrue(TEXT("Stock sky backdrop survives the switch"),Remaining.Contains(Owner->ColorFillMesh.Get()));
	// AC1 can also keep pooled rain and lightning meshes after a storm.
	// Check the actual imported materials on every remaining component.
	const auto* ImportedAlpha=FACELegionsSkyRenderer::MaterialParent(false);
	const auto* ImportedAdd=FACELegionsSkyRenderer::MaterialParent(true);
	for (const auto* Mesh : Remaining)
		for (int32 I=0; I<Mesh->GetNumMaterials(); ++I)
			if (const auto* Mid=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(I)))
				TestTrue(TEXT("No Legions artwork remains after disabling"),Mid->Parent!=ImportedAlpha && Mid->Parent!=ImportedAdd);
	return true;
}
#endif
