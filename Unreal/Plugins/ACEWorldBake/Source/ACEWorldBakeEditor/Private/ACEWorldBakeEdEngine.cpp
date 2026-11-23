#include "ACEWorldBakeEdEngine.h"
#include "ACEGameEngine.h"
#include "ACERuntimeOptions.h"
#include "Misc/ScopeExit.h"
#include "ACEWorldBakeEditor.h"
#include "ACEWorldBakeEditorInternal.h"
#include "ACEWorldBakeWorldBuilder.h"
#include "Editor/UnrealEdEngine.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Selection.h"
#include "Engine/World.h"
#include "Engine/WorldComposition.h"
#include "EditorLevelUtils.h"
#include "EditorLevelLibrary.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelUtils.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Engine/LevelStreaming.h"
#include "Containers/Set.h"
#include "ACEPlaySessionRedirect.h"
#include "PlayInEditorDataTypes.h"
#include "LandscapeProxy.h"
#include "LandscapeComponent.h"
#include "EngineUtils.h"
#include "LevelEditor.h"
#include "IAssetViewport.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Settings/LevelEditorMiscSettings.h"

namespace ACEWorldBakeEditorWcStreaming
{
	static bool bEditorWcStreamingPausedForPie = false;
	static bool bSavedEditorWorldShouldTick = true;
	static FString GEditorMapPathBeforeWcPieRedirect;
	static TMap<FName, bool> SavedDisableDistanceStreaming;
	static TArray<TWeakObjectPtr<ALandscapeProxy>> HiddenEditorLandscapesDuringPie;

	static bool IsLandscapeTileAssetName(const FString& AssetName)
	{
		TArray<FString> Parts;
		AssetName.ParseIntoArray(Parts, TEXT("_"));
		return Parts.Num() == 3 && Parts[0] == TEXT("Tile") && Parts[1].StartsWith(TEXT("x")) && Parts[2].StartsWith(TEXT("y"));
	}

	static void ForEachLandscapeStreamer(UWorld* EditorWorld, TFunctionRef<void(ULevelStreaming*)> Func)
	{
		if (!EditorWorld || !EditorWorld->WorldComposition)
		{
			return;
		}
		TSet<ULevelStreaming*> Seen;
		for (ULevelStreaming* SL : EditorWorld->WorldComposition->TilesStreaming)
		{
			if (SL && !Seen.Contains(SL))
			{
				Seen.Add(SL);
				Func(SL);
			}
		}
		for (ULevelStreaming* SL : EditorWorld->GetStreamingLevels())
		{
			if (!SL || Seen.Contains(SL))
			{
				continue;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(SL->GetWorldAssetPackageName());
			if (!IsLandscapeTileAssetName(AssetName))
			{
				continue;
			}
			Seen.Add(SL);
			Func(SL);
		}
	}

	static void HideEditorLandscapesDuringPie(UWorld* EditorWorld);
	static void MaintainEditorLandscapeHiddenState(UWorld* EditorWorld);

	static bool IsEditorWcPausedForPie()
	{
		return bEditorWcStreamingPausedForPie;
	}

	static void SwapEditorToLoginMapForWcPie(const FString& LoginMapPackagePath)
	{
		if (!GEditor)
		{
			return;
		}
		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (!EditorWorld || !EditorWorld->GetOutermost())
		{
			return;
		}
		const FString CurrentMap = EditorWorld->GetOutermost()->GetName();
		if (CurrentMap == LoginMapPackagePath)
		{
			return;
		}
		GEditorMapPathBeforeWcPieRedirect = CurrentMap;
		UE_LOG(LogACEWorldBake_Editor, Warning,
			TEXT("ACEWorldBake: swapping editor map '%s' -> '%s' so WC terrain is not rendered during login PIE"),
			*CurrentMap, *LoginMapPackagePath);
		if (!FEditorFileUtils::LoadMap(LoginMapPackagePath, false, true))
		{
			GEditorMapPathBeforeWcPieRedirect.Empty();
			UE_LOG(LogACEWorldBake_Editor, Error, TEXT("ACEWorldBake: failed to swap editor to login map for PIE"));
		}
	}

	static void RestoreEditorMapAfterWcPie()
	{
		if (GEditorMapPathBeforeWcPieRedirect.IsEmpty())
		{
			return;
		}
		const FString RestoreMap = GEditorMapPathBeforeWcPieRedirect;
		GEditorMapPathBeforeWcPieRedirect.Empty();
		UE_LOG(LogACEWorldBake_Editor, Warning, TEXT("ACEWorldBake: restoring editor map '%s' after PIE"), *RestoreMap);
		if (!FEditorFileUtils::LoadMap(RestoreMap, false, true))
		{
			UE_LOG(LogACEWorldBake_Editor, Error, TEXT("ACEWorldBake: failed to restore editor map '%s' after PIE"), *RestoreMap);
		}
	}

	static void HideLandscapeActor(ALandscapeProxy* Landscape, int32& NumHidden)
	{
		if (!Landscape)
		{
			return;
		}
		for (const TWeakObjectPtr<ALandscapeProxy>& WeakLandscape : HiddenEditorLandscapesDuringPie)
		{
			if (WeakLandscape.Get() == Landscape)
			{
				return;
			}
		}
		HiddenEditorLandscapesDuringPie.Add(Landscape);
		Landscape->SetIsTemporarilyHiddenInEditor(true);
		Landscape->SetActorHiddenInGame(true);
		Landscape->SetActorEnableCollision(false);
		TArray<ULandscapeComponent*> Components;
		Landscape->GetComponents(Components);
		for (ULandscapeComponent* LC : Components)
		{
			if (LC)
			{
				LC->SetVisibility(false, true);
				LC->SetHiddenInGame(true);
				LC->SetComponentTickEnabled(false);
			}
		}
		Landscape->MarkComponentsRenderStateDirty();
		++NumHidden;
	}

	static void HideLandscapesInLevel(ULevel* Level, int32& NumHidden)
	{
		if (!Level)
		{
			return;
		}
		for (AActor* Actor : Level->Actors)
		{
			if (ALandscapeProxy* Landscape = Cast<ALandscapeProxy>(Actor))
			{
				HideLandscapeActor(Landscape, NumHidden);
			}
		}
	}

	static void PauseEditorWorldCompositionStreaming(UWorld* EditorWorld)
	{
		if (!EditorWorld || !EditorWorld->WorldComposition || bEditorWcStreamingPausedForPie)
		{
			return;
		}

		bSavedEditorWorldShouldTick = EditorWorld->ShouldTick();
		EditorWorld->SetShouldTick(false);

		SavedDisableDistanceStreaming.Reset();
		int32 NumStreamers = 0;
		ForEachLandscapeStreamer(EditorWorld, [&NumStreamers](ULevelStreaming* SL)
		{
			const FName Pkg = SL->GetWorldAssetPackageFName();
			if (!SavedDisableDistanceStreaming.Contains(Pkg))
			{
				SavedDisableDistanceStreaming.Add(Pkg, SL->bDisableDistanceStreaming);
			}
			SL->bDisableDistanceStreaming = true;
			SL->bShouldBlockOnLoad = false;
			SL->bShouldBlockOnUnload = false;
			++NumStreamers;
		});

		bEditorWcStreamingPausedForPie = true;
		UE_LOG(LogACEWorldBake_Editor, Warning, TEXT("ACEWorldBake: paused editor WC distance streaming during PIE (%d streamers, flags only)"),
			NumStreamers);

		HideEditorLandscapesDuringPie(EditorWorld);
	}

	static void MaintainEditorWcStreamingFlags(UWorld* EditorWorld)
	{
		if (!bEditorWcStreamingPausedForPie || !EditorWorld || !EditorWorld->WorldComposition)
		{
			return;
		}
		ForEachLandscapeStreamer(EditorWorld, [](ULevelStreaming* SL)
		{
			SL->bDisableDistanceStreaming = true;
			SL->bShouldBlockOnLoad = false;
			SL->bShouldBlockOnUnload = false;
		});
	}

	static void HideEditorLandscapesDuringPie(UWorld* EditorWorld)
	{
		if (!EditorWorld)
		{
			return;
		}
		HiddenEditorLandscapesDuringPie.Reset();
		int32 NumHidden = 0;
		HideLandscapesInLevel(EditorWorld->PersistentLevel, NumHidden);
		for (ULevel* Level : EditorWorld->GetLevels())
		{
			HideLandscapesInLevel(Level, NumHidden);
		}
		for (ULevelStreaming* SL : EditorWorld->GetStreamingLevels())
		{
			if (SL && SL->GetLoadedLevel())
			{
				HideLandscapesInLevel(SL->GetLoadedLevel(), NumHidden);
			}
		}
		if (NumHidden > 0)
		{
			UE_LOG(LogACEWorldBake_Editor, Warning, TEXT("ACEWorldBake: hid %d editor landscape(s) during PIE (keeps render thread off WC tiles)"),
				NumHidden);
		}
	}

	static void MaintainEditorLandscapeHiddenState(UWorld* EditorWorld)
	{
		if (!EditorWorld || !bEditorWcStreamingPausedForPie)
		{
			return;
		}
		for (TWeakObjectPtr<ALandscapeProxy>& WeakLandscape : HiddenEditorLandscapesDuringPie)
		{
			if (ALandscapeProxy* Landscape = WeakLandscape.Get())
			{
				if (!Landscape->IsTemporarilyHiddenInEditor())
				{
					Landscape->SetIsTemporarilyHiddenInEditor(true);
					Landscape->SetActorHiddenInGame(true);
					Landscape->SetActorEnableCollision(false);
					TArray<ULandscapeComponent*> Components;
					Landscape->GetComponents(Components);
					for (ULandscapeComponent* LC : Components)
					{
						if (LC)
						{
							LC->SetVisibility(false, true);
							LC->SetHiddenInGame(true);
						}
					}
				}
			}
		}
	}

	static void RestoreEditorLandscapesAfterPie()
	{
		for (TWeakObjectPtr<ALandscapeProxy>& WeakLandscape : HiddenEditorLandscapesDuringPie)
		{
			if (ALandscapeProxy* Landscape = WeakLandscape.Get())
			{
				Landscape->SetIsTemporarilyHiddenInEditor(false);
				Landscape->SetActorHiddenInGame(false);
				Landscape->SetActorEnableCollision(true);
				TArray<ULandscapeComponent*> Components;
				Landscape->GetComponents(Components);
				for (ULandscapeComponent* LC : Components)
				{
					if (LC)
					{
						LC->SetHiddenInGame(false);
						LC->SetVisibility(true, true);
					}
				}
			}
		}
		HiddenEditorLandscapesDuringPie.Reset();
	}

	static void RestoreEditorWorldCompositionStreaming(UWorld* EditorWorld)
	{
		if (!bEditorWcStreamingPausedForPie)
		{
			return;
		}

		bEditorWcStreamingPausedForPie = false;

		if (EditorWorld)
		{
			EditorWorld->SetShouldTick(bSavedEditorWorldShouldTick);
		}

		if (!EditorWorld || !EditorWorld->WorldComposition)
		{
			SavedDisableDistanceStreaming.Reset();
			return;
		}

		ForEachLandscapeStreamer(EditorWorld, [](ULevelStreaming* SL)
		{
			const FName Pkg = SL->GetWorldAssetPackageFName();
			if (const bool* Saved = SavedDisableDistanceStreaming.Find(Pkg))
			{
				SL->bDisableDistanceStreaming = *Saved;
			}
		});
		SavedDisableDistanceStreaming.Reset();
		RestoreEditorLandscapesAfterPie();
		UE_LOG(LogACEWorldBake_Editor, Warning, TEXT("ACEWorldBake: restored editor WC distance streaming after PIE"));
	}

	static void RegisterPieStreamingDelegates()
	{
		static bool bRegistered = false;
		if (bRegistered)
		{
			return;
		}
		bRegistered = true;
		FEditorDelegates::BeginPIE.AddLambda([](const bool /*bIsSimulating*/)
		{
			if (GEditor)
			{
				PauseEditorWorldCompositionStreaming(GEditor->GetEditorWorldContext().World());
			}
		});
		FEditorDelegates::EndPIE.AddLambda([](const bool /*bIsSimulating*/)
		{
			ACEPlaySessionRedirect::GPendingWcTravelMapAfterLogin.Empty();
			ACEPlaySessionRedirect::GPendingEnteredWorldAfterTravel.Reset();
			if (GEditor)
			{
				RestoreEditorMapAfterWcPie();
				RestoreEditorWorldCompositionStreaming(GEditor->GetEditorWorldContext().World());
			}
		});
	}
}

UACEWorldBakeEdEngine::UACEWorldBakeEdEngine(const class FObjectInitializer& PCIP)
: Super(PCIP)
, FocusTime(0.0f)
, FocusImportedActors(false)
{
	ACEWorldBakeEditorWcStreaming::RegisterPieStreamingDelegates();
}

void UACEWorldBakeEdEngine::ImportT3D(const FString& FileName, const FString& T3DPayLoad, bool FocusImportedActor, const FString& LevelNameToActivate)
{
	const int32 AtIndex = T3DImportQueue.FindOrAdd(LevelNameToActivate).Insert({ FileName, T3DPayLoad }, 0);

	if (0 == AtIndex)
	{
		T3DImportQueue.KeySort(TLess<FString>());
	}

	FocusImportedActors = FocusImportedActor;
}

void UACEWorldBakeEdEngine::ImportT3DToBlueprint(const FString& FileName, const FString& T3DPayLoad, const FName AssetName)
{
	T3DImportToBlueprintQueue.Add(AssetName, { FileName, T3DPayLoad });
}

void UACEWorldBakeEdEngine::ClearImportQueue()
{
	T3DImportQueue.Empty();
}

void UACEWorldBakeEdEngine::ClearFocusQueue()
{
	FocusQueue.Empty();
	FocusImportedActors = false;
}

void UACEWorldBakeEdEngine::StartFocusImportActors()
{
	FocusImportedActors = true;
}

bool UACEWorldBakeEdEngine::LoadAndMakeLevelStreamingActive(ULevelStreaming* LevelStreaming)
{
	UWorld* const EditorWorldFromContext = GetEditorWorldContext().World();

	if (!EditorWorldFromContext || !LevelStreaming)
	{
		return false;
	}

	ULevelStreamingDynamic* AsKismetLevelStreaming = Cast<ULevelStreamingDynamic>(LevelStreaming);
	if (!AsKismetLevelStreaming)
	{
		return false;
	}

	AsKismetLevelStreaming->SetShouldBeLoaded(true);
	AsKismetLevelStreaming->SetShouldBeVisible(true);
	AsKismetLevelStreaming->bShouldBlockOnLoad = true;
	AsKismetLevelStreaming->bInitiallyLoaded = true;
	AsKismetLevelStreaming->bInitiallyVisible = true;

	if (!EditorWorldFromContext->GetStreamingLevels().Contains(LevelStreaming))
	{
		EditorWorldFromContext->AddStreamingLevel(AsKismetLevelStreaming);
	}

	if (EditorWorldFromContext->WorldComposition && !EditorWorldFromContext->WorldComposition->TilesStreaming.Contains(LevelStreaming))
	{
		EditorWorldFromContext->WorldComposition->TilesStreaming.Add(LevelStreaming);
	}

	EditorWorldFromContext->FlushLevelStreaming();
	AsKismetLevelStreaming->bShouldBlockOnLoad = false;

	ULevel* LoadedLevel = LevelStreaming->GetLoadedLevel();
	if (!LoadedLevel)
	{
		UE_LOG_ACEWORLDBAKEED(Warning, TEXT("Failed to load streaming level %s"), *LevelStreaming->GetWorldAssetPackageName());
		return false;
	}

	if (EditorWorldFromContext->GetCurrentLevel() != LoadedLevel)
	{
		EditorWorldFromContext->SetCurrentLevel(LoadedLevel);
		FEditorDelegates::NewCurrentLevel.Broadcast();
	}

	return true;
}

void UACEWorldBakeEdEngine::PumpEditorTick(bool bIdleMode)
{
	if (!FApp::IsUnattended())
	{
		Super::Tick(0.f, bIdleMode);

		// Main loop already ticks Slate during PIE; doing it again here deadlocks the editor.
		if (FSlateApplication::IsInitialized() && !(GEditor && GEditor->PlayWorld))
		{
			FSlateApplication::Get().Tick();
		}
	}
}

bool UACEWorldBakeEdEngine::ProcessOneBlueprintImport()
{
	UWorld* const EditorWorldFromContext = GetEditorWorldContext().World();
	if (!EditorWorldFromContext || T3DImportToBlueprintQueue.Num() == 0)
	{
		return false;
	}

	auto T3DImportToBlueprintIter = T3DImportToBlueprintQueue.CreateIterator();
	const FACEWorldBakeEdImportPayload& ImportPayload = T3DImportToBlueprintIter.Value();
	const FName ImportedAssetName = T3DImportToBlueprintIter.Key();

	if (ImportPayload.Payload.IsEmpty())
	{
		UE_LOG_ACEWORLDBAKEED(Warning, TEXT("ImportToBlueprint %s skipped, empty payload"), *ImportPayload.FileName);
		T3DImportToBlueprintQueue.Remove(ImportedAssetName);
		return true;
	}

	UE_LOG_ACEWORLDBAKEED(Log, TEXT("ImportToBlueprint %s"), *ImportPayload.FileName);

	TArray<AActor*> PastedActors;
	if (!ACEWorldBakeImportT3DText(EditorWorldFromContext, ImportPayload.Payload, PastedActors))
	{
		UE_LOG_ACEWORLDBAKEED(Warning, TEXT("ImportToBlueprint %s failed"), *ImportPayload.FileName);
		T3DImportToBlueprintQueue.Remove(ImportedAssetName);
		return true;
	}

	const FString ImportedAssetNameString = ImportedAssetName.ToString();
	AActor* ImportedActor = nullptr;
	for (AActor* Actor : PastedActors)
	{
		if (ImportedAssetNameString.Contains(Actor->GetName()))
		{
			ImportedActor = Actor;
			break;
		}
	}

	if (!ImportedActor && PastedActors.Num() > 0)
	{
		ImportedActor = PastedActors[0];
	}

	if (!ImportedActor)
	{
		UE_LOG_ACEWORLDBAKEED(Warning, TEXT("ImportToBlueprint %s produced no actors"), *ImportPayload.FileName);
		T3DImportToBlueprintQueue.Remove(ImportedAssetName);
		return true;
	}

	FKismetEditorUtilities::FCreateBlueprintFromActorParams CreateParams;
	CreateParams.bDeferCompilation = true;
	CreateParams.bKeepMobility = false;
	CreateParams.bOpenBlueprint = false;
	CreateParams.bReplaceActor = false;
	if (UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprintFromActor(ImportedAssetNameString, ImportedActor, CreateParams))
	{
		NewBlueprint->MarkPackageDirty();
	}
	else
	{
		UE_LOG_ACEWORLDBAKEED(Warning, TEXT("ImportToBlueprint %s CreateBlueprintFromActor failed"), *ImportPayload.FileName);
	}

	T3DImportToBlueprintQueue.Remove(ImportedAssetName);

	if (ImportedActor->GetWorld())
	{
		ImportedActor->GetWorld()->DestroyActor(ImportedActor, false, false);
	}

	return true;
}

bool UACEWorldBakeEdEngine::ProcessOneT3DImport(bool bDeferWorldCompositionRescan)
{
	UWorld* const EditorWorldFromContext = GetEditorWorldContext().World();
	if (!EditorWorldFromContext || T3DImportQueue.Num() == 0)
	{
		return false;
	}

	auto Iter = T3DImportQueue.CreateIterator();
	while (Iter)
	{
		const FString& LevelName = Iter.Key();
		TArray<FACEWorldBakeEdImportPayload>& Payloads = Iter.Value();

		if (Payloads.Num() == 0)
		{
			UE_LOG_ACEWORLDBAKEED(Warning, TEXT("Removing stale empty T3D import queue for %s"), *LevelName);
			T3DImportQueue.Remove(Iter.Key());
			return true;
		}

		const bool LoadAndActivateLevel = LevelName.Len() > 0;
		bool LoadedAndActive = !LoadAndActivateLevel;
		ULevelStreaming* LevelStreaming = nullptr;

		if (LoadAndActivateLevel)
		{
			if (EditorWorldFromContext->WorldComposition)
			{
				const FString PackageNameToLoad = ACEWorldBakeMap::SubLevelContentRoot / LevelName;

				if (!ACEWorldBakeEnsureLevelFileExists(PackageNameToLoad))
				{
					UE_LOG_ACEWORLDBAKEED(Warning, TEXT("Failed to create sub-level package %s — dropping queued imports"), *PackageNameToLoad);
					T3DImportQueue.Remove(Iter.Key());
					return true;
				}

				LevelStreaming = FLevelUtils::FindStreamingLevel(EditorWorldFromContext, *PackageNameToLoad);
				if (LevelStreaming)
				{
					LoadedAndActive = LoadAndMakeLevelStreamingActive(LevelStreaming);
				}
				else
				{
					LevelStreaming = UEditorLevelUtils::AddLevelToWorld(
						EditorWorldFromContext,
						*PackageNameToLoad,
						ULevelStreamingDynamic::StaticClass());

					if (LevelStreaming)
					{
						if (EditorWorldFromContext->WorldComposition)
						{
							EditorWorldFromContext->WorldComposition->Rescan();
						}

						LoadedAndActive = LoadAndMakeLevelStreamingActive(LevelStreaming);

						if (!FApp::IsUnattended())
						{
							FEditorDelegates::RefreshAllBrowsers.Broadcast();
							FEditorDelegates::RefreshLevelBrowser.Broadcast();
						}

						if (LevelStreaming->GetLoadedLevel() && !LevelStreaming->GetLoadedLevel()->LevelBoundsActor.IsValid())
						{
							FActorSpawnParameters SpawnParameters;
							SpawnParameters.OverrideLevel = LevelStreaming->GetLoadedLevel();
							ALevelBounds* NewLevelBounds = EditorWorldFromContext->SpawnActor<ALevelBounds>(SpawnParameters);
							LevelStreaming->GetLoadedLevel()->LevelBoundsActor = NewLevelBounds;
						}
					}
				}
			}

			if (!LoadedAndActive)
			{
				UE_LOG_ACEWORLDBAKEED(Warning, TEXT("Failed to activate level for %s — dropping queued imports"), *LevelName);
				T3DImportQueue.Remove(Iter.Key());
				return true;
			}
		}

		const FACEWorldBakeEdImportPayload ImportPayload = Payloads.Pop();

		UE_LOG_ACEWORLDBAKEED(Log, TEXT("Import %s"), *ImportPayload.FileName);

		TArray<AActor*> PastedActors;
		if (ACEWorldBakeImportT3DText(EditorWorldFromContext, ImportPayload.Payload, PastedActors))
		{
			if (FocusImportedActors && PastedActors.Num() > 0)
			{
				if (AActor* PastedActor = PastedActors[0])
				{
					if (PastedActor->GetComponents().Num() > 2)
					{
						FocusQueue.Insert(PastedActor, 0);
					}
				}
			}
		}
		else
		{
			UE_LOG_ACEWORLDBAKEED(Warning, TEXT("Import %s failed — dropping payload"), *ImportPayload.FileName);
		}

		if (Payloads.Num() == 0)
		{
			if (LoadAndActivateLevel)
			{
				ULevel* LevelToSave = nullptr;
				if (LevelStreaming && LevelStreaming->GetLoadedLevel())
				{
					LevelToSave = LevelStreaming->GetLoadedLevel();
				}
				else if (EditorWorldFromContext->GetCurrentLevel())
				{
					LevelToSave = EditorWorldFromContext->GetCurrentLevel();
				}

				if (LevelToSave)
				{
					const FString LevelPackageName = LevelToSave->GetOutermost()->GetName();
					const FString LevelFilePath = FPackageName::LongPackageNameToFilename(
						LevelPackageName, FPackageName::GetMapPackageExtension());

					if (!FEditorFileUtils::SaveLevel(LevelToSave, LevelFilePath))
					{
						UE_LOG_ACEWORLDBAKEED(Warning, TEXT("Failed to save level %s after T3D import — continuing"), *LevelPackageName);
					}
				}
			}

			if (!bDeferWorldCompositionRescan && EditorWorldFromContext->WorldComposition)
			{
				EditorWorldFromContext->WorldComposition->Rescan();
			}

			T3DImportQueue.Remove(Iter.Key());
		}

		return true;
	}

	return false;
}

void UACEWorldBakeEdEngine::ApplyPieLoginMapRedirect()
{
	if (!PlaySessionRequest.IsSet())
	{
		return;
	}

	FRequestPlaySessionParams& Params = PlaySessionRequest.GetValue();
	if (!Params.GlobalMapOverride.IsEmpty())
	{
		return;
	}

	ACEPlaySessionRedirect::GPendingWcTravelMapAfterLogin.Empty();
	ACEPlaySessionRedirect::GPendingEnteredWorldAfterTravel.Reset();

	if (UWorld* ActiveWorld = GetEditorWorldContext().World())
	{
		const FString CurrentMap = ActiveWorld->GetOutermost()
			? ActiveWorld->GetOutermost()->GetName()
			: ActiveWorld->GetPathName();
		const bool bOnWcMap = ActiveWorld->WorldComposition
			&& ActiveWorld->WorldComposition->GetTilesList().Num() > 0;
		const bool bOnLoginMap = CurrentMap == ACEPlaySessionRedirect::LoginMapPath
			|| CurrentMap.Contains(TEXT("Template_Default"));

		if (bOnWcMap)
		{
			ACEPlaySessionRedirect::GPendingWcTravelMapAfterLogin = CurrentMap;
			Params.GlobalMapOverride = ACEPlaySessionRedirect::LoginMapPath;

			// Remove Dereth WC from the editor render path — login PIE runs on Template_Default only.
			ACEWorldBakeEditorWcStreaming::SwapEditorToLoginMapForWcPie(ACEPlaySessionRedirect::LoginMapPath);
			ActiveWorld = GetEditorWorldContext().World();

			if (ActiveWorld)
			{
				ACEWorldBakeEditorWcStreaming::PauseEditorWorldCompositionStreaming(ActiveWorld);
			}

			if (Params.EditorPlaySettings)
			{
				Params.EditorPlaySettings->LastExecutedPlayModeType = PlayMode_InViewPort;
			}

			// Repeat-last-play uses New Editor Window without DestinationSlateViewport — force viewport PIE.
			if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
			{
				FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
				TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
				if (ActiveViewport.IsValid()
					&& FSlateApplication::IsInitialized()
					&& FSlateApplication::Get().FindWidgetWindow(ActiveViewport->AsWidget()).IsValid())
				{
					Params.DestinationSlateViewport = ActiveViewport;
					UE_LOG(LogACEWorldBake_Editor, Warning,
						TEXT("ACEWorldBake: WC PIE redirect — play in active level viewport (not New Editor Window)"));
				}
				else
				{
					UE_LOG(LogACEWorldBake_Editor, Warning,
						TEXT("ACEWorldBake: WC PIE redirect — no visible level viewport; PIE may open a floating window"));
				}
			}

			UE_LOG(LogACEWorldBake_Editor, Warning,
				TEXT("ACEWorldBake: PIE from WC map '%s' — login on '%s', travel to world on Enter World"),
				*CurrentMap, *ACEPlaySessionRedirect::LoginMapPath);
		}
		else if (bOnLoginMap && FPackageName::DoesPackageExist(ACEPlaySessionRedirect::WorldMapPath))
		{
			// Editor opened on the lightweight login map — still travel to baked Dereth after login.
			ACEPlaySessionRedirect::GPendingWcTravelMapAfterLogin = ACEPlaySessionRedirect::WorldMapPath;
			UE_LOG(LogACEWorldBake_Editor, Warning,
				TEXT("ACEWorldBake: PIE from login map '%s' — will ClientTravel to WC world '%s' on Enter World"),
				*CurrentMap, *ACEPlaySessionRedirect::WorldMapPath);
		}
	}
}

float UACEWorldBakeEdEngine::GetMaxTickRate(float DeltaTime, bool bAllowFrameRateSmoothing) const
{
	return ACEFrontendFrameRate::GetLimit(*this,Super::GetMaxTickRate(DeltaTime,bAllowFrameRateSmoothing),true);
}

bool UACEWorldBakeEdEngine::ShouldThrottleCPUUsage() const
{
	// The editor's background throttle otherwise reduces preview UCM to 3 Hz.
	return !ACEBackgroundActivity::HasClientWorld(*this)&&Super::ShouldThrottleCPUUsage();
}

void UACEWorldBakeEdEngine::Tick(float DeltaSeconds, bool bIdleMode)
{
	// EditorEngine has an extra background audio mute independent of FApp.
	// Borrow it for the play session only; do not save over editor preferences.
	auto* AudioSettings=GetMutableDefault<ULevelEditorMiscSettings>();
	const bool SavedBackgroundAudio=AudioSettings->bAllowBackgroundAudio;
	if(ACEBackgroundActivity::HasClientWorld(*this))
		AudioSettings->bAllowBackgroundAudio=ACERuntimeOptions::Get(TEXT("ActiveSoundOnly"))<=.5f;
	ON_SCOPE_EXIT {AudioSettings->bAllowBackgroundAudio=SavedBackgroundAudio;};
	ApplyPieLoginMapRedirect();

	if (GEditor && GEditor->PlayWorld)
	{
		UWorld* WcEditorWorld = GetEditorWorldContext().World();
		ACEWorldBakeEditorWcStreaming::MaintainEditorWcStreamingFlags(WcEditorWorld);
		ACEWorldBakeEditorWcStreaming::MaintainEditorLandscapeHiddenState(WcEditorWorld);
		Super::Tick(DeltaSeconds, bIdleMode);
		return;
	}

	if (!ACEWorldBakeEditorWcStreaming::IsEditorWcPausedForPie())
	{
		ACEWorldBakeEditorWcStreaming::RestoreEditorWorldCompositionStreaming(GetEditorWorldContext().World());
	}

	// WC login redirect / queued play session: use real delta so PIE startup frame can complete (PumpEditorTick uses 0).
	// Headless automation still needs the editor tick to drain asset compilation
	// and become ready for tests. PumpEditorTick intentionally skips unattended runs.
	if (FApp::IsUnattended() || ACEWorldBakeEditorWcStreaming::IsEditorWcPausedForPie() || PlaySessionRequest.IsSet())
	{
		Super::Tick(DeltaSeconds, bIdleMode);
	}
	else
	{
		PumpEditorTick(bIdleMode);
	}

	if (ProcessOneBlueprintImport())
	{
		return;
	}

	ProcessOneT3DImport(false);

	if (FocusQueue.Num() > 0)
	{
		FocusTime += DeltaSeconds;
		if (1.0f < FocusTime)
		{
			MoveViewportCamerasToActor(*FocusQueue.Pop(), true);
			FocusTime = 0.0f;
		}
	}
}

bool UACEWorldBakeEdEngine::HasPendingBlueprintImports() const
{
	return T3DImportToBlueprintQueue.Num() > 0;
}

bool UACEWorldBakeEdEngine::HasPendingT3DImports() const
{
	return T3DImportQueue.Num() > 0;
}

bool UACEWorldBakeEdEngine::HasPendingImports() const
{
	return HasPendingBlueprintImports() || HasPendingT3DImports();
}

void UACEWorldBakeEdEngine::PumpBlueprintImportQueueUntilIdle(double MaxSeconds)
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 ProcessedCount = 0;

	while (HasPendingBlueprintImports())
	{
		ProcessOneBlueprintImport();
		PumpEditorTick(true);

		++ProcessedCount;
		if (ProcessedCount % 256 == 0)
		{
			UE_LOG_ACEWORLDBAKEED(Log, TEXT("Setup blueprint import progress: %d remaining"), T3DImportToBlueprintQueue.Num());
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		if (MaxSeconds > 0.0 && (FPlatformTime::Seconds() - StartSeconds) >= MaxSeconds)
		{
			break;
		}
	}

	if (ProcessedCount > 0)
	{
		UE_LOG_ACEWORLDBAKEED(Log, TEXT("Setup blueprint import queue drained (%d remaining)"), T3DImportToBlueprintQueue.Num());
	}
}

void UACEWorldBakeEdEngine::PumpT3DImportQueueUntilIdle(double MaxSeconds)
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 ProcessedCount = 0;
	int32 QueueCountAtLastProgress = T3DImportQueue.Num();

	while (HasPendingT3DImports())
	{
		if (!ProcessOneT3DImport(true))
		{
			break;
		}

		PumpEditorTick(true);

		++ProcessedCount;
		if (ProcessedCount % 256 == 0)
		{
			const int32 QueueCount = T3DImportQueue.Num();
			UE_LOG_ACEWORLDBAKEED(Log, TEXT("Landblock T3D import progress: %d level queues remaining"), QueueCount);

			if (QueueCount < QueueCountAtLastProgress)
			{
				ACEWorldBakeSaveDirtyContentPackages();
				QueueCountAtLastProgress = QueueCount;
			}
			else if (QueueCount > 0)
			{
				UE_LOG_ACEWORLDBAKEED(Error, TEXT("Landblock T3D import stalled at %d level queues — aborting pump"), QueueCount);
				break;
			}

			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		if (MaxSeconds > 0.0 && (FPlatformTime::Seconds() - StartSeconds) >= MaxSeconds)
		{
			break;
		}
	}

	if (ProcessedCount > 0)
	{
		UE_LOG_ACEWORLDBAKEED(Log, TEXT("Landblock T3D import queue drained (%d level queues remaining)"), T3DImportQueue.Num());
	}
}

void UACEWorldBakeEdEngine::PumpImportQueueUntilIdle(double MaxSeconds)
{
	PumpBlueprintImportQueueUntilIdle(MaxSeconds);
	PumpT3DImportQueueUntilIdle(MaxSeconds);
}
