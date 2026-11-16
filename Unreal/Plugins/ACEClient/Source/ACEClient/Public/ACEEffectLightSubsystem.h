#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ACEEffectLightSubsystem.generated.h"

class UActorComponent;
class UPointLightComponent;
struct FACEBuiltEnvCellMesh;

/** A scene-light hint. Particle materials and simulation remain owned by their source. */
struct FACEEffectLightRequest
{
	uint32 EmitterId = 0;
	FVector Position = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::White;
	float Intensity = 10.f;
	float Radius = 720.f;
	bool bPortal = false;
};

/** One bounded scene-light pool per world, shared by particle and authored script lights. */
UCLASS()
class ACECLIENT_API UACEEffectLightSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	void SubmitParticles(UActorComponent* Source, TConstArrayView<FACEEffectLightRequest> Requests);
	void RemoveParticles(UActorComponent* Source);
	void RemoveEmitter(UActorComponent* Source, uint32 EmitterId);
	void RemoveSource(UActorComponent* Source);
	void SetScriptLight(UActorComponent* Source, bool bEnabled, const FACEEffectLightRequest& Request);
	/** DAT fixture positions are in the owning room actor's landblock-local UE frame. */
	void SetCellLights(UActorComponent* Source, TArray<FACEEffectLightRequest> Fixtures);
	bool IsParticleSelected(const UActorComponent* Source, uint32 EmitterId) const;
	void UpdateParticlePosition(UActorComponent* Source, uint32 EmitterId, const FVector& Position);
	UPointLightComponent* FindParticleLight(const UActorComponent* Source, uint32 EmitterId) const;
	UPointLightComponent* FindScriptLight(const UActorComponent* Source) const;
	int32 GetVisibleLightCount(const UActorComponent* Source = nullptr) const;
	int32 GetAllocatedLightCount() const { return LightPool.Num(); }
	/** Public for deterministic fixtures; gameplay refreshes after actor/component ticks. */
	void RefreshLights(float DeltaSeconds);

private:
	struct FSourceState
	{
		TArray<FACEEffectLightRequest> Particles;
		TArray<FACEEffectLightRequest> Fixtures;
		FACEEffectLightRequest Script;
		double SubmittedAt = 0;
		uint64 Order = 0;
		bool bScriptEnabled = false;
		bool bSubmittedSinceRefresh = false;
	};
	struct FLightKey
	{
		TWeakObjectPtr<UActorComponent> Source;
		uint32 EmitterId = 0;
		bool bScript = false;
		bool bFixture = false;
		bool operator==(const FLightKey& Other) const
		{
			return Source == Other.Source && EmitterId == Other.EmitterId && bScript == Other.bScript && bFixture == Other.bFixture;
		}
	};
	struct FChoice
	{
		FLightKey Key;
		FACEEffectLightRequest Request;
		double Rank = 0;
		uint64 Order = 0;
		bool bLocalHeld = false;
	};

	FSourceState& FindOrAddSource(UActorComponent* Source);
	bool IsSourceUsable(UActorComponent* Source, const FACEBuiltEnvCellMesh* PlayerCell) const;
	bool IsPortalOccluded(const FChoice& Choice) const;
	const FACEEffectLightRequest* FindRequest(const FLightKey& Key) const;
	void ReleaseSlot(int32 Index);
	void ReleaseSourceSlots(UActorComponent* Source, bool bParticles, bool bScript, const uint32* EmitterId = nullptr);
	void SelectLights(int32 Budget, const FVector* ViewPosition, float MaxDistance, const FACEBuiltEnvCellMesh* PlayerCell);
	void UpdateSelectedLights(const FVector* ViewPosition, float MaxDistance, const FACEBuiltEnvCellMesh* PlayerCell);
	UPointLightComponent* CreatePoolLight();
	void OnWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);

	TMap<TWeakObjectPtr<UActorComponent>, FSourceState> Sources;
	UPROPERTY(Transient) TArray<TObjectPtr<UPointLightComponent>> LightPool;
	TArray<FLightKey> Assignments;
	FDelegateHandle WorldPostActorTickHandle;
	double ClockSeconds = 0;
	double NextSelectionAt = 0;
	uint64 NextSourceOrder = 1;
	int32 LastBudget = -1;
	int32 LastFixtureMode = -1;
	float LastMaxDistance = -1.f;
	bool bSelectionDirty = true;
};
