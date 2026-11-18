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
	for (const auto& Layer : Data.Layers)
		if (Layer.Setup == 0x04000023 || Layer.Setup == 0x040000b6)
			OrbCenters.Add(Layer.Setup,Layer.Center);
	const auto* Belt=Data.Layers.FindByPredicate([](const auto& L){return L.Setup==0x04000729;});
	if (!TestTrue(TEXT("Legions asteroid belt texture"),Belt && Belt->Sections[0].Texture==0x40000b81)) return false;
	const FVector BeltCenter=Belt->Center;
	auto Imported = MakeShared<ACELegionsSky::FBackdrop>(MoveTemp(Data));
	Owner->PendingSky = MakeFulfilledPromise<TSharedPtr<ACELegionsSky::FBackdrop>>(Imported).GetFuture();
	if (!TestTrue(TEXT("Build AC2 sky and haze outdoors"), Owner->UpdateImportedSky(0, true)))
		return false;
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
		const FVector StockBearing=(Native->Mesh->Bounds.Origin-Retail->GetActorLocation()).GetSafeNormal();
		TestTrue(TEXT("Artwork has AC1 azimuth and elevation"),StockBearing.Equals(Orb->GetComponentQuat().RotateVector(Center).GetSafeNormal(),.001));
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
			{TEXT("rain-day"),Storm,.5f,true}, {TEXT("rain-night"),Storm,.1f,true},
			{TEXT("ac1-dawn"),39755,.18f,false}})
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
				if (State.Day == Storm && Extended && State.Time == .5f)
					TestEqual(TEXT("Legions rain keeps the AC1 rainy-day terrain range"),
						Owner->DatSubsystem->GetWorldFogEndCm(),150000.f);
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
				else if (State.Imported && State.Day == ClearPreviewDay)
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
					Far->ShellMaterial->SetVectorParameterValue(TEXT("CloudMotion"),FLinearColor::Transparent);
					TArray<FColor> Unshadowed;
					if (!ReadView(Unshadowed)) return false;
					Far->ShellMaterial->SetVectorParameterValue(TEXT("CloudMotion"),Motion);
					int32 Shaded = 0;
					for (int32 I=View.Num()/2; I<View.Num(); ++I)
						Shaded += int32(Unshadowed[I].G)-View[I].G >= 3;
					AddInfo(FString::Printf(TEXT("Clouds darken %d distant terrain pixels at the summit"),Shaded));
					TestTrue(TEXT("Cloud shadows are visible beyond the local terrain"),Shaded > 100);
				}
			}
		}
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
	TArray<FColor> Clouded,Drifted,Clear;
	Owner->DatSubsystem->SetSkyAnimationTime(Session->GetPortalYearTicks());
	if (!TestTrue(TEXT("Capture terrain under imported clouds"),CaptureLand(Clouded))) return false;
	// Advance the sky clock while engine time is unchanged. Real time alone
	// must no longer move the terrain mask independently of the sky.
	Owner->DatSubsystem->SetSkyAnimationTime(Session->GetPortalYearTicks()+15.);
	if (!TestTrue(TEXT("Capture terrain after cloud drift"),CaptureLand(Drifted))) return false;
	Owner->DatSubsystem->SetLandSky(nullptr,FLinearColor::Transparent,Owner->ImportedSky->Current.AmbientColor);
	if (!TestTrue(TEXT("Capture identical terrain without clouds"),CaptureLand(Clear))) return false;
	int32 Darkened=0, Moved=0, MinDelta=255, MaxDelta=0;
	for (int32 I=0; I<Clear.Num(); ++I)
	{
		const int32 Delta=int32(Clear[I].R)-Clouded[I].R;
		MinDelta=FMath::Min(MinDelta,Delta); MaxDelta=FMath::Max(MaxDelta,Delta);
		Darkened+=Delta>2;
		Moved+=FMath::Abs(int32(Clouded[I].R)-Drifted[I].R)>1;
	}
	SaveView(Clouded,TEXT("terrain-clouded.png"));
	SaveView(Clear,TEXT("terrain-clear.png"));
	AddInfo(FString::Printf(TEXT("Cloud shadow capture: %d darkened, %d moved, delta %d to %d"),
		Darkened,Moved,MinDelta,MaxDelta));
	TestTrue(TEXT("The native mask casts spatially varying shadows on the ground"),
		Darkened>Clear.Num()/10 && MaxDelta-MinDelta>8);
	TestTrue(TEXT("Cloud shadows drift over stationary terrain"),Moved>Clear.Num()/20);
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
	for (auto* Part : MovingParts)
		if (Part->IsVisible())
			if (auto* Mid = Cast<UMaterialInstanceDynamic>(Part->GetMaterial(0)))
				if (Mid->K2_GetVectorParameterValue(TEXT("StageOps")).A > .5f) CloudPart = Part;
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
							const FVector Direction=(Slot.Mesh->Bounds.Origin-Retail->GetActorLocation()).GetSafeNormal();
							TestTrue(TEXT("Stock and imported orbits stay aligned"),Direction.Equals((*Part)->GetComponentQuat().RotateVector(Center).GetSafeNormal(),.001));
						}
					}
					else if (Gfx==0x01001F67 || Slot.bWeather || Slot.bAfterPass)
					{
						const auto& Actual=Owner->Slots[Slot.ObjectIndex];
						TestEqual(TEXT("Second moon and precipitation follow stock visibility"),
							Actual.Mesh && Actual.Mesh->IsVisible(),Slot.Mesh->IsVisible());
					}
				}
				TestEqual(TEXT("Legions cloud shadows follow the server cloud cover"),Owner->bImportedClouds,CloudCover);
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
