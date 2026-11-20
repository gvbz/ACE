#pragma once

#include "CoreMinimal.h"

class UPersonalFarTerrainSubsystem;

/**
 * Personal fog distance: the Max Fog Distance option and the fog values used while it limits the fog.
 * The fog rules act only while the far terrain is drawn.
 * Distances are in meters (AC units); one landblock is 192 m.
 */
namespace PersonalFog
{
	inline constexpr const TCHAR* OptionKey = TEXT("PersonalMaxFogDistance");
	/** The top of the slider: no limit, so the fog is as the client sets it at every Far Terrain Radius. */
	constexpr int32 NoLimit = 43;
	constexpr float BlockMeters = 192.f;

	/** While Max Fog Distance is below 43: the fog start in Clear and Sunny weather by day, in landblocks. */
	constexpr float LimitedStartClearLandblocks = 3.f;
	/** While Max Fog Distance is below 43: the most fog, 0 to 1. */
	constexpr float LimitedFogAmount = .73f;
	/** While Far Terrain Horizon is on: the horizon band's opacity, 0 to 1. */
	constexpr float HorizonOpacityWithFarHorizon = .73f;

	/** The Max Fog Distance option in landblocks, a whole number (5 to 43, default 11). */
	int32 MaxLandblocks();
	/** True while Max Fog Distance limits the fog (below 43). */
	inline bool IsLimited() { return MaxLandblocks() < NoLimit; }

	/**
	 * The fog end with the far terrain drawn: the end the client set, at most the far terrain's coverage
	 * from the camera and, below 43, at most MaxLandblocks x 192, in every weather and at every time. No state, for tests.
	 */
	float LimitEnd(float EndMeters, float CoverageMeters, int32 MaxLandblocks);

	/**
	 * Called by AACESkyDomeActor::UpdateFog just before its fog clamp. Returns EndMeters as it is when
	 * the far terrain is not drawn (FarRadius 0); otherwise LimitEnd with the option and the shell's coverage.
	 */
	float LimitFogEnd(float EndMeters, int32 FarRadius, const UPersonalFarTerrainSubsystem* Shell, const FVector& CameraLocation, float WorldScale);

	/**
	 * The fog start in cm. No state, for tests. In a Clear or Sunny forecast with StartClearLandblocks above 0 the
	 * day value is that distance, else ClientStartCm; at night it is ClientStartCm. Blended by NightAmount (0 day,
	 * 1 night), then always below the fog end (EndCm - 1).
	 */
	float StartCm(float ClientStartCm, float EndCm, bool bClearForecast, float NightAmount, float StartClearLandblocks, float WorldScale);

	/**
	 * The start UpdateFog sends: ClientStartCm as it is with the far terrain off (FarRadius 0) or at 43. Otherwise
	 * StartCm with LimitedStartClearLandblocks, the forecast and the night amount the far rooms use.
	 */
	float FogStartCm(float ClientStartCm, float EndCm, int32 FarRadius, const FString& DayName, const UPersonalFarTerrainSubsystem* Shell,
		float WorldScale);
	/** The amount UpdateFog sends: LimitedFogAmount while the far terrain is drawn and the fog is limited, else 1. */
	float FogAmount(int32 FarRadius);
	/** The horizon band's OpacityMul UpdateFog sets: HorizonOpacityWithFarHorizon while Far Terrain Horizon is on, else 1. */
	float HorizonOpacity();

	/** Rings of landblocks around the texture center the shell draws with the detail texture: Max + 1, at most MeshRings. */
	int32 TextureRings(int32 MaxLandblocks);
	inline int32 TextureRings() { return TextureRings(MaxLandblocks()); }

	/** The detail texture's mesh cells (PersonalFarTerrainDetail::MeshCells) cut down to Rings rings. */
	FIntRect TextureCells(FIntPoint TextureCenter, int32 Rings);
}
