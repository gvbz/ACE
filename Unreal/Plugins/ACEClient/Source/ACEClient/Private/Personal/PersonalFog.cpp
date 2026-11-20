#include "Personal/PersonalFog.h"
#include "Personal/PersonalFarTerrainDetail.h"
#include "Personal/PersonalFarTerrainSubsystem.h"
#include "Personal/PersonalFarWindows.h"
#include "Personal/PersonalSkyHorizon.h"
#include "ACEClientSubsystem.h"
#include "ACERuntimeOptions.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	/** The forecast test of UpdateFog's far terrain branch. */
	bool IsClearForecast(const FString& DayName)
	{
		return DayName == TEXT("Clear") || DayName == TEXT("Sunny");
	}

	/** The night amount the far rooms use: PersonalFarWindows::NightAmount by the client's day fraction (0.5, day, without a client). */
	float NightAmountFor(const UPersonalFarTerrainSubsystem* Shell)
	{
		const UWorld* World = Shell ? Shell->GetWorld() : nullptr;
		const UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
		const UACEClientSubsystem* Client = Game ? Game->GetSubsystem<UACEClientSubsystem>() : nullptr;
		return PersonalFarWindows::NightAmount(Client ? Client->GetGameDayFraction(0.5f) : 0.5f);
	}
}

int32 PersonalFog::MaxLandblocks()
{
	// The option row limits the value to 5 to 43.
	return FMath::RoundToInt32(ACERuntimeOptions::Get(OptionKey));
}

float PersonalFog::LimitEnd(float EndMeters, float CoverageMeters, int32 MaxLandblocks)
{
	float End = FMath::Min(EndMeters, CoverageMeters);
	if (MaxLandblocks < NoLimit)
	{
		End = FMath::Min(End, MaxLandblocks * BlockMeters);
	}
	return End;
}

float PersonalFog::LimitFogEnd(float EndMeters, int32 FarRadius, const UPersonalFarTerrainSubsystem* Shell, const FVector& CameraLocation, float WorldScale)
{
	if (FarRadius <= 0 || !Shell)
	{
		return EndMeters;
	}
	// The same coverage as UpdateFog's far terrain branch.
	const float Coverage = Shell->FogCoverageDistanceCm(CameraLocation) / FMath::Max(WorldScale, 1.f);
	return LimitEnd(EndMeters, Coverage, MaxLandblocks());
}

float PersonalFog::StartCm(float ClientStartCm, float EndCm, bool bClearForecast, float NightAmount, float StartClearLandblocks, float WorldScale)
{
	const float Day = bClearForecast && StartClearLandblocks > 0.f ? StartClearLandblocks * BlockMeters * WorldScale : ClientStartCm;
	const float Start = FMath::Lerp(Day, ClientStartCm, FMath::Clamp(NightAmount, 0.f, 1.f));
	return FMath::Clamp(Start, 0.f, FMath::Max(0.f, EndCm - 1.f));
}

float PersonalFog::FogStartCm(float ClientStartCm, float EndCm, int32 FarRadius, const FString& DayName, const UPersonalFarTerrainSubsystem* Shell,
	float WorldScale)
{
	if (FarRadius <= 0 || !IsLimited())
	{
		return ClientStartCm;
	}
	return StartCm(ClientStartCm, EndCm, IsClearForecast(DayName), NightAmountFor(Shell), LimitedStartClearLandblocks, WorldScale);
}

float PersonalFog::FogAmount(int32 FarRadius)
{
	return FarRadius > 0 && IsLimited() ? LimitedFogAmount : 1.f;
}

float PersonalFog::HorizonOpacity()
{
	return PersonalSkyHorizon::IsEnabled() ? HorizonOpacityWithFarHorizon : 1.f;
}

int32 PersonalFog::TextureRings(int32 MaxLandblocks)
{
	return FMath::Clamp(MaxLandblocks + 1, 1, PersonalFarTerrainDetail::MeshRings);
}

FIntRect PersonalFog::TextureCells(FIntPoint TextureCenter, int32 Rings)
{
	FIntRect Cells = PersonalFarTerrainDetail::MeshCells(TextureCenter);
	const int32 Cut = (PersonalFarTerrainDetail::MeshRings - FMath::Clamp(Rings, 1, PersonalFarTerrainDetail::MeshRings)) * 8;
	Cells.Min += FIntPoint(Cut, Cut);
	Cells.Max -= FIntPoint(Cut, Cut);
	return Cells;
}
