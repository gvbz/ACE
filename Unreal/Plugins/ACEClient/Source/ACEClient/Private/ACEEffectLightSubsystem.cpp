#include "ACEEffectLightSubsystem.h"
#include "ACEProfiling.h"
#include "ACERuntimeOptions.h"
#include "ACELandblockActor.h"
#include "ACEEnvCellActor.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEDatSubsystem.h"
#include "Engine/GameInstance.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/ActorComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

static TAutoConsoleVariable<int32> CVarEffectLightBudget(TEXT("ace.FXLightBudget"), 8,
	TEXT("Maximum simultaneous particle/script scene lights per desktop world (0 disables them, capped at 64)."));
static TAutoConsoleVariable<int32> CVarEffectLightBudgetMobile(TEXT("ace.FXLightBudgetMobile"), 4,
	TEXT("Maximum simultaneous particle/script scene lights per mobile world (0 disables them, capped at 64)."));
static TAutoConsoleVariable<int32> CVarDungeonFixtureMode(TEXT("ace.DungeonFixtureMode"), -1,
	TEXT("Full dungeon lighting: -1 follows shadow quality. For comparisons, 0 shares the effect budget, 1 lights all with two shadows, 2 shadows all."));
static TAutoConsoleVariable<float> CVarEffectLightMaxDistance(TEXT("ace.FXLightMaxDistance"), 3500.f,
	TEXT("Maximum scene-light distance in cm (from the player in dungeons, otherwise the camera). Fades over the outer 20 percent."));

namespace
{
	constexpr double SelectionInterval = .05;
	constexpr double ParticleSubmissionLifetime = .25;
	constexpr float OverlapIntensityBudget = 12.f;

	double OverlapWeight(const FACEEffectLightRequest& A, const FACEEffectLightRequest& B)
	{
		const double Range = double(A.Radius) + B.Radius;
		const double Margin = FMath::Max(1., .25 * FMath::Min(double(A.Radius), double(B.Radius)));
		const double DistSq = FVector::DistSquared(A.Position, B.Position);
		if (DistSq >= FMath::Square(Range + Margin)) return 0.;
		if (DistSq <= FMath::Square(Range)) return 1.;
		const double T = (Range + Margin - FMath::Sqrt(DistSq)) / Margin;
		return T*T*(3.-2.*T);
	}

	bool IsValidRequest(const FACEEffectLightRequest& Request)
	{
		return !Request.Position.ContainsNaN() && FMath::IsFinite(Request.Color.R)
			&& FMath::IsFinite(Request.Color.G) && FMath::IsFinite(Request.Color.B) && FMath::IsFinite(Request.Color.A)
			&& Request.Color.GetLuminance() > KINDA_SMALL_NUMBER
			&& FMath::IsFinite(Request.Intensity) && Request.Intensity > 0.f
			&& FMath::IsFinite(Request.Radius) && Request.Radius > 0.f;
	}

	float DistanceFade(const FVector& Position, const FVector* ViewPosition, float MaxDistance)
	{
		if (MaxDistance <= 0.f) return 0.f;
		if (!ViewPosition) return 1.f;
		const float Distance = FVector::Distance(Position, *ViewPosition);
		const float T = FMath::Clamp((MaxDistance - Distance) / (MaxDistance * .2f), 0.f, 1.f);
		return T * T * (3.f - 2.f * T);
	}
}

bool UACEEffectLightSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UACEEffectLightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(
		this, &UACEEffectLightSubsystem::OnWorldPostActorTick);
}

void UACEEffectLightSubsystem::OnWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	// Tickable subsystems run before TG_PostUpdateWork, where particle poses are
	// refreshed. Apply transforms and overlap limits only after those submissions.
	if (World == GetWorld() && (TickType == LEVELTICK_All || TickType == LEVELTICK_ViewportsOnly))
		RefreshLights(DeltaSeconds);
}

void UACEEffectLightSubsystem::Deinitialize()
{
	FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
	WorldPostActorTickHandle.Reset();
	for (UPointLightComponent* Light : LightPool)
		if (IsValid(Light)) Light->DestroyComponent();
	LightPool.Reset();
	Assignments.Reset();
	Sources.Reset();
	Super::Deinitialize();
}

UACEEffectLightSubsystem::FSourceState& UACEEffectLightSubsystem::FindOrAddSource(UActorComponent* Source)
{
	FSourceState& State = Sources.FindOrAdd(TWeakObjectPtr<UActorComponent>(Source));
	if (State.Order == 0)
	{
		State.Order = NextSourceOrder++;
		bSelectionDirty = true;
	}
	return State;
}

bool UACEEffectLightSubsystem::IsSourceUsable(UActorComponent* Source, const FACEBuiltEnvCellMesh* PlayerCell) const
{
	if (!IsValid(Source) || Source->IsBeingDestroyed() || Source->GetWorld() != GetWorld()) return false;
	const AActor* Owner = Source->GetOwner();
	if (!IsValid(Owner) || Owner->IsActorBeingDestroyed()) return false;
	const auto* Room = Cast<AACEEnvCellActor>(Owner);
	if (ACERuntimeOptions::UniformDungeonLighting())
	{
		const auto* FixtureRoom = Room ? Room : Cast<AACEEnvCellActor>(Owner->GetOwner());
		if (FixtureRoom && !FixtureRoom->bSeenOutside) return false;
	}
	// The camera can look through a doorway from a different cell than the player.
	if (!Owner->IsHidden()) return true;
	// A hidden fixture mount can still cast light onto a wall in the player's visible cells.
	if (!Room || !PlayerCell) return false;
	const uint32 Id = uint32(Room->EnvCellId);
	return Id == PlayerCell->EnvCellId || ((Id >> 16) == (PlayerCell->EnvCellId >> 16) &&
		PlayerCell->VisibleCells.Contains(uint16(Id)));
}

void UACEEffectLightSubsystem::SetCellLights(UActorComponent* Source, TArray<FACEEffectLightRequest> Fixtures)
{
	RemoveSource(Source);
	Fixtures.RemoveAll([](const auto& R){ return !IsValidRequest(R); });
	if (IsValid(Source) && Source->GetWorld()==GetWorld() && !Fixtures.IsEmpty())
		FindOrAddSource(Source).Fixtures=MoveTemp(Fixtures);
}

void UACEEffectLightSubsystem::SubmitParticles(UActorComponent* Source, TConstArrayView<FACEEffectLightRequest> Requests)
{
	if (!IsValid(Source) || Source->IsBeingDestroyed() || Source->GetWorld() != GetWorld()) return;
	if (Requests.IsEmpty()) { RemoveParticles(Source); return; }
	FSourceState& State = FindOrAddSource(Source);
	if (State.Particles.Num() != Requests.Num()) bSelectionDirty = true;
	else for (int32 I = 0; I < Requests.Num(); ++I)
		if (State.Particles[I].EmitterId != Requests[I].EmitterId) { bSelectionDirty = true; break; }
	State.Particles.Reset(Requests.Num());
	for (const FACEEffectLightRequest& Request : Requests)
		if (IsValidRequest(Request)) State.Particles.Add(Request);
	State.SubmittedAt = ClockSeconds;
	State.bSubmittedSinceRefresh = true;
	// Deleted slots must disappear before the next scheduled selection.
	for (int32 I = 0; I < Assignments.Num(); ++I)
		if (Assignments[I].Source.Get() == Source && !Assignments[I].bScript && !FindRequest(Assignments[I]))
		{
			ReleaseSlot(I);
			bSelectionDirty = true;
		}
}

void UACEEffectLightSubsystem::ReleaseSlot(int32 Index)
{
	if (LightPool.IsValidIndex(Index) && LightPool[Index]) LightPool[Index]->SetVisibility(false);
	if (Assignments.IsValidIndex(Index)) Assignments[Index] = {};
}

void UACEEffectLightSubsystem::ReleaseSourceSlots(UActorComponent* Source, bool bParticles, bool bScript, const uint32* EmitterId)
{
	for (int32 I = 0; I < Assignments.Num(); ++I)
	{
		const FLightKey& Key = Assignments[I];
		// Removing an emitter leaves the room's persistent fixtures alone.
		if (Key.bFixture && !(bParticles && bScript)) continue;
		if (Key.Source.Get() == Source && (Key.bScript ? bScript : bParticles)
			&& (!EmitterId || Key.EmitterId == *EmitterId))
		{
			ReleaseSlot(I);
			bSelectionDirty = true;
		}
	}
}

void UACEEffectLightSubsystem::RemoveParticles(UActorComponent* Source)
{
	const TWeakObjectPtr<UActorComponent> Key(Source);
	if (FSourceState* State = Sources.Find(Key))
	{
		if (State->Particles.IsEmpty()) return;
		ReleaseSourceSlots(Source, true, false);
		State->Particles.Reset();
		bSelectionDirty = true;
		if (!State->bScriptEnabled && State->Fixtures.IsEmpty()) Sources.Remove(Key);
	}
}

void UACEEffectLightSubsystem::RemoveEmitter(UActorComponent* Source, uint32 EmitterId)
{
	const TWeakObjectPtr<UActorComponent> Key(Source);
	if (FSourceState* State = Sources.Find(Key))
	{
		if (State->Particles.RemoveAll([EmitterId](const auto& Request) { return Request.EmitterId == EmitterId; }) == 0) return;
		ReleaseSourceSlots(Source, true, false, &EmitterId);
		bSelectionDirty = true;
		if (State->Particles.IsEmpty() && State->Fixtures.IsEmpty() && !State->bScriptEnabled) Sources.Remove(Key);
	}
}

void UACEEffectLightSubsystem::RemoveSource(UActorComponent* Source)
{
	if (Sources.Remove(TWeakObjectPtr<UActorComponent>(Source)) > 0)
	{
		ReleaseSourceSlots(Source, true, true);
		bSelectionDirty = true;
	}
}

void UACEEffectLightSubsystem::SetScriptLight(UActorComponent* Source, bool bEnabled, const FACEEffectLightRequest& Request)
{
	if (!IsValid(Source) || Source->IsBeingDestroyed() || Source->GetWorld() != GetWorld()) return;
	const TWeakObjectPtr<UActorComponent> Key(Source);
	if (!bEnabled || !IsValidRequest(Request))
	{
		if (FSourceState* State = Sources.Find(Key))
		{
			if (!State->bScriptEnabled) return;
			ReleaseSourceSlots(Source, false, true);
			State->bScriptEnabled = false;
			bSelectionDirty = true;
			if (State->Particles.IsEmpty() && State->Fixtures.IsEmpty()) Sources.Remove(Key);
		}
		return;
	}
	FSourceState& State = FindOrAddSource(Source);
	bSelectionDirty |= !State.bScriptEnabled || State.Script.bPortal != Request.bPortal;
	State.Script = Request;
	State.Script.EmitterId = 0;
	State.bScriptEnabled = true;
}

const FACEEffectLightRequest* UACEEffectLightSubsystem::FindRequest(const FLightKey& Key) const
{
	const FSourceState* State = Sources.Find(Key.Source);
	if (!State) return nullptr;
	if (Key.bFixture) return State->Fixtures.IsValidIndex(Key.EmitterId) ? &State->Fixtures[Key.EmitterId] : nullptr;
	if (Key.bScript) return State->bScriptEnabled ? &State->Script : nullptr;
	return State->Particles.FindByPredicate([&Key](const auto& Request) { return Request.EmitterId == Key.EmitterId; });
}

UPointLightComponent* UACEEffectLightSubsystem::FindParticleLight(const UActorComponent* Source, uint32 EmitterId) const
{
	const FLightKey Key{const_cast<UActorComponent*>(Source), EmitterId, false};
	for (int32 I = 0; I < Assignments.Num(); ++I)
		if (Assignments[I] == Key && LightPool[I] && LightPool[I]->IsVisible()) return LightPool[I];
	return nullptr;
}

UPointLightComponent* UACEEffectLightSubsystem::FindScriptLight(const UActorComponent* Source) const
{
	const FLightKey Key{const_cast<UActorComponent*>(Source), 0, true};
	for (int32 I = 0; I < Assignments.Num(); ++I)
		if (Assignments[I] == Key && LightPool[I] && LightPool[I]->IsVisible()) return LightPool[I];
	return nullptr;
}

bool UACEEffectLightSubsystem::IsParticleSelected(const UActorComponent* Source, uint32 EmitterId) const
{
	return FindParticleLight(Source, EmitterId) != nullptr;
}

void UACEEffectLightSubsystem::UpdateParticlePosition(UActorComponent* Source, uint32 EmitterId, const FVector& Position)
{
	if (Position.ContainsNaN()) return;
	if (FSourceState* State = Sources.Find(TWeakObjectPtr<UActorComponent>(Source)))
		if (FACEEffectLightRequest* Request = State->Particles.FindByPredicate(
			[EmitterId](const auto& Candidate) { return Candidate.EmitterId == EmitterId; })) Request->Position = Position;
	if (UPointLightComponent* Light = FindParticleLight(Source, EmitterId)) Light->SetWorldLocation(Position);
}

int32 UACEEffectLightSubsystem::GetVisibleLightCount(const UActorComponent* Source) const
{
	int32 Count = 0;
	for (int32 I = 0; I < LightPool.Num(); ++I)
		if (LightPool[I] && LightPool[I]->IsVisible() && LightPool[I]->Intensity > 0.f
			&& (!Source || Assignments[I].Source.Get() == Source)) ++Count;
	return Count;
}

bool UACEEffectLightSubsystem::IsPortalOccluded(const FChoice& Choice) const
{
	if (!Choice.Request.bPortal) return false;
	UWorld* World = GetWorld();
	if (!World) return false;
	const UActorComponent* Source = Choice.Key.Source.Get();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ACEPortalLightOcclude), false, Source ? Source->GetOwner() : nullptr);
	FHitResult Hit;
	const FVector Position = Choice.Request.Position;
	return World->LineTraceSingleByChannel(Hit, Position + FVector(0, 0, 900), Position + FVector(0, 0, 20),
		ECC_WorldStatic, Params) && Cast<AACELandblockActor>(Hit.GetActor()) && Hit.ImpactPoint.Z > Position.Z + 40.f;
}

UPointLightComponent* UACEEffectLightSubsystem::CreatePoolLight()
{
	UPointLightComponent* Light = NewObject<UPointLightComponent>(this);
	Light->SetVisibility(false);
	Light->SetIntensity(0.f);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetCastShadows(false);
	Light->SetUseInverseSquaredFalloff(false);
	Light->SetIntensityUnits(ELightUnits::Unitless);
	Light->SetLightFalloffExponent(2.f);
	Light->SetSourceRadius(36.f);
	Light->SetSoftSourceRadius(64.f);
	Light->SetSpecularScale(0.f);
	Light->SetAffectTranslucentLighting(false);
	Light->SetLightingChannels(true, true, false);
	Light->SetVolumetricScatteringIntensity(0.f);
	Light->RegisterComponentWithWorld(GetWorld());
	return Light;
}

void UACEEffectLightSubsystem::SelectLights(int32 Budget, const FVector* ViewPosition, float MaxDistance, const FACEBuiltEnvCellMesh* PlayerCell)
{
	TArray<FChoice> Candidates;
	const int32 FixtureMode = LastFixtureMode;
	int32 IndependentCandidateCount = 0;
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* LocalPawn = PC ? PC->GetPawn() : nullptr;
	for (const auto& Pair : Sources)
	{
		if (!IsSourceUsable(Pair.Key.Get(), PlayerCell)) continue;
		auto Add = [&](FACEEffectLightRequest Request, bool bScript, bool bFixture=false, uint32 Index=0)
		{
			if (bFixture) Request.Position = Pair.Key->GetOwner()->GetActorTransform().TransformPosition(Request.Position);
			if (!IsValidRequest(Request) || (!(bFixture && FixtureMode) && DistanceFade(Request.Position, ViewPosition, MaxDistance) <= 0.f)) return;
			FChoice Choice;
			Choice.Key = {Pair.Key, bFixture ? Index : Request.EmitterId, bScript, bFixture};
			Choice.Request = Request;
			Choice.Order = Pair.Value.Order;
			// Both carried weapons get first claim on the existing pool. Camera
			// offset or a nearby torch must not leave just the offhand unlit.
			Choice.bLocalHeld = LocalPawn && Pair.Key->GetOwner() != LocalPawn
				&& Pair.Key->GetOwner()->IsAttachedTo(LocalPawn);
			Choice.Rank = ViewPosition ? FVector::Distance(Request.Position, *ViewPosition) : 0.;
			if (Assignments.Contains(Choice.Key)) Choice.Rank *= .85;
			Candidates.Add(Choice);
			if (bFixture && FixtureMode) ++IndependentCandidateCount;
		};
		// An explicit authored light overrides the inferred material hint on the
		// same source, regardless of which hook happened to submit first.
		if (Pair.Value.bScriptEnabled) Add(Pair.Value.Script, true);
		else for (const auto& Request : Pair.Value.Particles) Add(Request, false);
		for (int32 I=0; I<Pair.Value.Fixtures.Num(); ++I) Add(Pair.Value.Fixtures[I], false, true, I);
	}
	Candidates.Sort([](const FChoice& A, const FChoice& B)
	{
		if (A.bLocalHeld != B.bLocalHeld) return A.bLocalHeld;
		if (A.Rank != B.Rank) return A.Rank < B.Rank;
		if (A.Order != B.Order) return A.Order < B.Order;
		if (A.Key.bScript != B.Key.bScript) return A.Key.bScript;
		return A.Key.EmitterId < B.Key.EmitterId;
	});
	TArray<FChoice, TInlineAllocator<64>> Chosen;
	TSet<TWeakObjectPtr<UActorComponent>> ChosenSources;
	int32 EffectCount=0, IndependentFixtures=0;
	for (const FChoice& Choice : Candidates)
	{
		if (EffectCount >= Budget && IndependentFixtures == IndependentCandidateCount) break;
		const bool bIndependent=Choice.Key.bFixture && FixtureMode;
		if (!bIndependent && EffectCount >= Budget) continue;
		if ((!Choice.Key.bFixture && ChosenSources.Contains(Choice.Key.Source)) || IsPortalOccluded(Choice)) continue;
		Chosen.Add(Choice);
		if (bIndependent) ++IndependentFixtures; else ++EffectCount;
		ChosenSources.Add(Choice.Key.Source);
	}
	for (int32 I = 0; I < Assignments.Num(); ++I)
		if (!Chosen.ContainsByPredicate([&](const FChoice& Choice) { return Choice.Key == Assignments[I]; })) ReleaseSlot(I);
	// A room transition or a lower budget can release slots in the middle of
	// the pool. Keep active light components stable when trimming empty slots.
	for (int32 I=Assignments.Num()-1; I>=0 && LightPool.Num()>Budget+IndependentFixtures; --I)
		if (!Assignments[I].Source.IsValid())
		{
			if (IsValid(LightPool[I])) LightPool[I]->DestroyComponent();
			LightPool.RemoveAtSwap(I); Assignments.RemoveAtSwap(I);
		}
	static const auto* ShadowQuality = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("sg.ShadowQuality"));
	const bool CastFixtureShadows = ACERuntimeOptions::DynamicLightingQuality()==2 &&
		ShadowQuality && ShadowQuality->GetValueOnGameThread() > 0;
	const bool AllFixtureShadows = FixtureMode==2 && GetWorld()->GetFeatureLevel() >= ERHIFeatureLevel::SM5;
	int32 ShadowsRemaining = CastFixtureShadows ? 2 : 0;
	for (const FChoice& Choice : Chosen)
	{
		int32 Slot = Assignments.Find(Choice.Key);
		if (Slot == INDEX_NONE)
		{
			Slot = Assignments.IndexOfByPredicate([](const FLightKey& Key) { return !Key.Source.IsValid(); });
			if (Slot == INDEX_NONE)
			{
				Slot = LightPool.Add(CreatePoolLight());
				Assignments.AddDefaulted();
			}
			Assignments[Slot] = Choice.Key;
		}
		const auto* Room = Cast<AACEEnvCellActor>(Choice.Key.Source->GetOwner()->GetOwner());
		const bool bIndoorFixture = Choice.Key.bFixture || (Room && !Room->bSeenOutside);
		const bool bIndependent=Choice.Key.bFixture && FixtureMode;
		const bool bShadow = CastFixtureShadows && bIndoorFixture && (ShadowsRemaining>0 || (bIndependent && AllFixtureShadows));
		if (bShadow && ShadowsRemaining>0) --ShadowsRemaining;
		LightPool[Slot]->SetCastShadows(bShadow);
		LightPool[Slot]->SetLightingChannels(!bIndoorFixture,true,false);
		if (bIndependent)
		{
			// The renderer culls the attenuation sphere for each actual view. A
			// torch around a corner can still light a visible patch of floor.
			// Normalize fixed lights against the admitted fixtures, independently
			// of facing and of transient spells entering the effect pool.
			const auto& R=Choice.Request;
			double Total=0.;
			for (const auto& Other:Chosen)
				if (Other.Key.bFixture) Total+=Other.Request.Intensity*OverlapWeight(R,Other.Request);
			UPointLightComponent* Light=LightPool[Slot];
			Light->SetWorldLocation(R.Position);
			Light->SetLightColor(R.Color);
			Light->SetAttenuationRadius(R.Radius);
			Light->SetIntensity(R.Intensity*FMath::Min(1.,OverlapIntensityBudget/FMath::Max(.001,Total)));
			Light->SetVisibility(true);
		}
	}
}

void UACEEffectLightSubsystem::UpdateSelectedLights(const FVector* ViewPosition, float MaxDistance, const FACEBuiltEnvCellMesh* PlayerCell)
{
	TArray<FACEEffectLightRequest, TInlineAllocator<64>> Requests;
	Requests.SetNum(Assignments.Num());
	TArray<bool, TInlineAllocator<64>> Active;
	Active.Init(false, Assignments.Num());
	for (int32 I = 0; I < Assignments.Num(); ++I)
	{
		const FLightKey& Key = Assignments[I];
		if (Key.bFixture && LastFixtureMode) continue; // Persistent mounts updated at selection time.
		const FACEEffectLightRequest* Request = FindRequest(Key);
		if (Request && IsSourceUsable(Key.Source.Get(), PlayerCell))
		{
			Requests[I] = *Request;
			if (Key.bFixture) Requests[I].Position = Key.Source->GetOwner()->GetActorTransform().TransformPosition(Request->Position);
			if (IsValidRequest(Requests[I]) && DistanceFade(Requests[I].Position, ViewPosition, MaxDistance) > 0.f)
			{
				Active[I] = true;
				continue;
			}
		}
		if (Key.Source.IsValid()) bSelectionDirty = true;
		ReleaseSlot(I);
	}
	for (int32 I = 0; I < Assignments.Num(); ++I)
	{
		if (!Active[I] || !LightPool[I]) continue;
		const FACEEffectLightRequest& Request = Requests[I];
		double OverlapIntensity = 0.;
		for (int32 J = 0; J < Assignments.Num(); ++J)
		{
			if (!Active[J]) continue;
			OverlapIntensity += Requests[J].Intensity * OverlapWeight(Request,Requests[J]);
		}
		const float Scale = float(FMath::Min(1., double(OverlapIntensityBudget) / FMath::Max(OverlapIntensity, .001)));
		UPointLightComponent* Light = LightPool[I];
		Light->SetWorldLocation(Request.Position);
		Light->SetLightColor(Request.Color);
		Light->SetAttenuationRadius(Request.Radius);
		Light->SetIntensity(Request.Intensity * Scale * DistanceFade(Request.Position, ViewPosition, MaxDistance));
		Light->SetVisibility(true);
	}
}

void UACEEffectLightSubsystem::RefreshLights(float DeltaSeconds)
{
	ACE_PROFILE_SCOPE(EffectLights);
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_EffectLightBudget);
	if (!GetWorld()) return;
	ClockSeconds += FMath::IsFinite(DeltaSeconds) ? FMath::Max(0.f, DeltaSeconds) : 0.f;
	for (auto It = Sources.CreateIterator(); It; ++It)
	{
		UActorComponent* Source = It.Key().Get();
		if (!IsValid(Source) || Source->IsBeingDestroyed() || Source->GetWorld() != GetWorld() || !IsValid(Source->GetOwner())
			|| Source->GetOwner()->IsActorBeingDestroyed())
		{
			for (int32 I = 0; I < Assignments.Num(); ++I)
				if (Assignments[I].Source == It.Key()) ReleaseSlot(I);
			It.RemoveCurrent();
			bSelectionDirty = true;
			continue;
		}
		FSourceState& State = It.Value();
		if (State.bSubmittedSinceRefresh)
		{
			// A long frame must not expire data submitted during that very frame.
			State.SubmittedAt = ClockSeconds;
			State.bSubmittedSinceRefresh = false;
		}
		if (!State.Particles.IsEmpty() && ClockSeconds - State.SubmittedAt > ParticleSubmissionLifetime)
		{
			State.Particles.Reset();
			ReleaseSourceSlots(Source, true, false);
		}
		if (State.bScriptEnabled) State.Script.Position = Source->GetOwner()->GetActorLocation();
		if (State.Particles.IsEmpty() && State.Fixtures.IsEmpty() && !State.bScriptEnabled) { It.RemoveCurrent(); bSelectionDirty = true; }
	}
	const bool bMobile = GetWorld()->GetFeatureLevel() <= ERHIFeatureLevel::ES3_1;
	const int32 LightingQuality = ACERuntimeOptions::DynamicLightingQuality();
	const int32 BudgetLimit = LightingQuality == 0 ? 0 : LightingQuality == 1 ? 2 : 64;
	const int32 Budget = FMath::Clamp(bMobile ? CVarEffectLightBudgetMobile.GetValueOnGameThread()
		: CVarEffectLightBudget.GetValueOnGameThread(), 0, BudgetLimit);
	int32 FixtureMode=0;
	if (LightingQuality==2 && !ACERuntimeOptions::UniformDungeonLighting())
	{
		static const auto* ShadowQuality=IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("sg.ShadowQuality"));
		const int32 Override=CVarDungeonFixtureMode.GetValueOnGameThread();
		FixtureMode=Override<0 ? (ShadowQuality && ShadowQuality->GetValueOnGameThread()>=3 ? 2 : 1) : FMath::Min(Override,2);
	}
	while (!FixtureMode && LightPool.Num() > Budget)
	{
		const int32 Index = LightPool.Num() - 1;
		ReleaseSlot(Index);
		if (IsValid(LightPool[Index])) LightPool[Index]->DestroyComponent();
		LightPool.RemoveAt(Index);
		Assignments.RemoveAt(Index);
		bSelectionDirty = true;
	}
	const float ConfiguredDistance = CVarEffectLightMaxDistance.GetValueOnGameThread();
	const float MaxDistance = FMath::Min(LightingQuality == 1 ? 1500.f : MAX_flt,
		FMath::IsFinite(ConfiguredDistance) ? FMath::Max(0.f, ConfiguredDistance) : 3500.f);
	if (Budget != LastBudget || MaxDistance != LastMaxDistance || FixtureMode != LastFixtureMode)
	{
		bSelectionDirty = true;
		LastBudget = Budget;
		LastMaxDistance = MaxDistance;
		LastFixtureMode = FixtureMode;
	}
	// Keep source lifetimes current so re-enabling works, but do not rank
	// candidates or allocate renderer lights while illumination is disabled.
	if (Budget == 0 && !FixtureMode) return;
	FVector ViewPosition;
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const auto* GI = GetWorld()->GetGameInstance();
	const auto* Client = GI ? GI->GetSubsystem<UACEClientSubsystem>() : nullptr;
	const uint32 CellId = Client ? Client->GetPlayerPosition().CellId : 0;
	const FACEBuiltEnvCellMesh* PlayerCell = nullptr;
	if (uint16(CellId) >= 0x100)
	{
		const auto* Controller = Cast<AACEPlayerController>(PC);
		if (const auto* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			PlayerCell = Dat->FindEnvCellMesh(CellId, Controller ? Controller->WorldScale : 100.f);
	}
	const FVector* View = nullptr;
	if (PlayerCell && !PlayerCell->CanSeeOutside() && PC && PC->GetPawn())
	{
		// Orbiting the camera in place must not move the distance fade,
		// reassign the two shadow maps, or displace a nearby torch.
		ViewPosition = PC->GetPawn()->GetActorLocation();
		View = &ViewPosition;
	}
	else if (PC && PC->PlayerCameraManager)
	{
		FRotator ViewRotation;
		PC->GetPlayerViewPoint(ViewPosition, ViewRotation);
		View = &ViewPosition;
	}
	if (bSelectionDirty || ClockSeconds >= NextSelectionAt)
	{
		bSelectionDirty = false;
		NextSelectionAt = ClockSeconds + SelectionInterval;
		SelectLights(Budget, View, MaxDistance, PlayerCell);
	}
	UpdateSelectedLights(View, MaxDistance, PlayerCell);
}
