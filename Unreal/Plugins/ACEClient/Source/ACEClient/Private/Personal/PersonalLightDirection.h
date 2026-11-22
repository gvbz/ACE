#pragma once

#include "CoreMinimal.h"
#include "Sky/ACELegionsSkyRenderer.h"

struct FACEDatSkyTimeOfDay;

/**
 * Personal smooth light direction with the imported AC2 sky. The client takes the light from the sun if it is
 * above the horizon, else the large moon, else the AC1 key direction, so the light turns in one step when a body
 * crosses the horizon and when the AC1 keys swap heading at midnight. Here the light turns smoothly, from the time
 * of day only: sharp AC1 heading turns are spread over NightTurn, and the light turns from the AC1 key direction
 * toward the moon, then toward the sun, by each body's weight above the horizon.
 * Directions are unit vectors toward the light, in Unreal coordinates (Z up, north +Y, east -X); the renderer's
 * Current.Direction is the opposite (the light's travel direction).
 */
namespace PersonalLightDirection
{
	/** Above this angle between two directions the light turns about the vertical axis (through the horizon). */
	inline constexpr float VerticalTurnDegrees = 90.f;
	/** A heading change of at least this much between two neighboring keys is a sharp turn. */
	inline constexpr float SharpTurnDegrees = 90.f;

	/** personal.LightDir.Smooth: 1 smooth, 0 the client's direction. */
	bool IsOn();
	/** personal.LightDir.HorizonFade, clamped to 5 to 45 degrees. */
	float HorizonFade();
	/** personal.LightDir.NightTurn, clamped to 0.01 to 0.25 of a day. */
	float NightTurn();

	/** A sharp AC1 heading turn spread over a window: the heading goes linearly from StartHeading to EndHeading. */
	struct FTurnWindow
	{
		double Start = 0., End = 0.;
		float StartHeading = 0.f, EndHeading = 0.f;
	};
	/**
	 * Every pair of neighboring keys (the last key of the day with the first of the next day too) whose heading
	 * changes by SharpTurnDegrees or more over a span shorter than NightTurn (by more than 0.0001 of a day) gives a
	 * window of NightTurn centered on that span. Start and End may lie outside 0 to 1.
	 */
	TArray<FTurnWindow> TurnWindows(const TArray<FACEDatSkyTimeOfDay>& Keys, float TurnLength);

	/** The client's AC1 key pitch and heading in degrees (BracketTimeOfDay and the lerp in UpdateImportedSky). */
	bool ClientKeyAngles(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction, float& PitchDeg, float& HeadingDeg);
	/** Toward the light from DAT pitch and heading, with the client's conversion (UpdateImportedSky). */
	FVector AnglesToDirection(float PitchDeg, float HeadingDeg);
	/** The AC1 key direction toward the light, with sharp heading turns spread over NightTurn. Zero without keys. */
	FVector KeyDirection(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction, float TurnLength);

	/** 0 if the body's opacity is 0; else smoothstep from 0 at the horizon to 1 at HorizonFade degrees up, times its opacity. */
	float Weight(const FACELegionsSkyFrame::FBody& Body, float FadeDegrees);
	/** From From toward To by T (0 to 1): along the shortest arc, or about the vertical axis when more than VerticalTurnDegrees apart. */
	FVector Turn(const FVector& From, const FVector& To, float T);
	/** From the key direction toward the moon by its weight, then toward the sun by its weight. */
	FVector Direction(const FVector& KeyDir, const FACELegionsSkyFrame::FBody& Moon, const FACELegionsSkyFrame::FBody& Sun, float FadeDegrees);

	/** Elevation above the horizon and compass bearing (0 north, 90 east) of a direction toward the light, in degrees. */
	void ElevationBearing(const FVector& Toward, float& ElevationDeg, float& BearingDeg);

	/**
	 * Called right after the renderer's Update (and the fog colors). With Smooth 1 sets Sky.Current.Direction to minus
	 * Direction(...); with 0 changes nothing. Fills personal.LightDir.Current either way. Game thread only.
	 */
	void Apply(FACELegionsSkyRenderer& Sky, const FACELegionsSkyFrame& Frame, const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction);
}
