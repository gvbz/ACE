#pragma once

#include "ACEWorldBakeEdEngine.generated.h"

DEFINE_LOG_CATEGORY_STATIC(LogACEWorldBake_Editor, Log, All);

struct FACEWorldBakeEdImportPayload
{
	FString FileName;

	FString Payload;
};

UCLASS()
class UACEWorldBakeEdEngine : public UUnrealEdEngine
{
	GENERATED_UCLASS_BODY()

	void ImportT3D(const FString& FileName, const FString& T3DPayLoad, bool FocusImportedActor, const FString& LevelNameToActivate);

	void ImportT3DToBlueprint(const FString& FileName, const FString& T3DPayLoad, const FName AssetName);

	void ClearImportQueue();

	void ClearFocusQueue();

	void StartFocusImportActors();

	bool LoadAndMakeLevelStreamingActive(ULevelStreaming* LevelStreaming);

	bool HasPendingImports() const;

	bool HasPendingBlueprintImports() const;

	bool HasPendingT3DImports() const;

	void ApplyPieLoginMapRedirect();

	void PumpBlueprintImportQueueUntilIdle(double MaxSeconds);

	void PumpT3DImportQueueUntilIdle(double MaxSeconds);

	void PumpImportQueueUntilIdle(double MaxSeconds);

	virtual void Tick(float DeltaSeconds, bool bIdleMode) override;
	virtual float GetMaxTickRate(float DeltaTime, bool bAllowFrameRateSmoothing = true) const override;
	virtual bool ShouldThrottleCPUUsage() const override;

private:
	bool ProcessOneBlueprintImport();

	bool ProcessOneT3DImport(bool bDeferWorldCompositionRescan);

	void PumpEditorTick(bool bIdleMode);
	typedef TMap<FName, FACEWorldBakeEdImportPayload> TImportToBlueprintMap;
	TImportToBlueprintMap T3DImportToBlueprintQueue;
	typedef TMap<FString, TArray<FACEWorldBakeEdImportPayload>> TImportMap;
	TImportMap T3DImportQueue;
	TArray<AActor*> FocusQueue;
	float FocusTime;
	bool FocusImportedActors;
};
