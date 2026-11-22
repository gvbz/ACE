#pragma once

#include "CoreMinimal.h"

struct FACELegionsSkyRenderer;

/**
 * Personal fog colors by weather: with the imported AC2 sky, recolors its fog through the sunrise and
 * sunset windows by the server's weather (the day group's name). Two color sets: the palettes (set A)
 * and the atmospheric tables (set B). The hue and saturation move toward the set's ramp; the AC2
 * brightness is kept, times a brightness factor per weather and window.
 * Colors in this namespace are display sRGB; hue in degrees (0 to 360), saturation and value 0 to 1.
 */
namespace PersonalFogColor
{
	inline constexpr const TCHAR* OptionKey = TEXT("PersonalFogColor");
	/** The Fog Color Saturation slider (0 to 1.5, default 0.35). */
	inline constexpr const TCHAR* SaturationOptionKey = TEXT("PersonalFogColorSaturation");

	/** Stage 2 limits at Day variation 1: hue shift in degrees, relative saturation change, brightness factor shift. */
	inline constexpr float MaxHueShift = 20.f;
	inline constexpr float MaxSatScale = .3f;
	inline constexpr float MaxBrightShift = .1f;

	/** personal.FogColor.Draw: -1 follows the Fog Colors by Weather check box, 0 off, 1 on. */
	bool IsOn();

	/** personal.FogColor.Saturation if 0 or more (a dashboard override), else the Fog Color Saturation option; 0 to 1.5. */
	float SaturationInUse(bool* bOverride = nullptr);

	/** Weather types, from the day group's name. */
	constexpr int32 Clear = 0, Sunny = 1, Cloudy = 2, Rainy = 3, WeatherCount = 4;
	/** Clear, Sunny, Cloudy, Rainy; empty for anything else. */
	const TCHAR* WeatherName(int32 Weather);
	/** 0 to 3 for Clear, Sunny, Cloudy, Rainy; -1 for any other day name. */
	int32 WeatherOf(const FString& DayName);

	/** Presets: 0 AC2 (no change), 1 to 4 the palettes, 5 Custom (hue and saturation from the console variables). */
	constexpr int32 PresetAC2 = 0, PresetCustom = 5, PresetCount = 6;
	/** AC2, dawn and dusk, Dawn to Dusk, Dusk and Dawn, From Dusk to Dawn, Custom. */
	const TCHAR* PresetName(int32 Preset);
	/** The colors of palettes 1 to 4, light to dark as listed; empty for 0, 5 and anything else. */
	TArrayView<const FColor> Palette(int32 Preset);

	/** Atmospheric tables 1 to 4 (set B): Clear sky, Overcast and rainy, Hazy, Foggy. */
	constexpr int32 TableCount = 4;
	const TCHAR* TableName(int32 Table);
	/** Columns: 0 horizon, 1 mid-sky, 2 zenith (table 4: distant horizon, mid-distance mist, near ground). */
	constexpr int32 ColumnCount = 3;
	const TCHAR* ColumnName(int32 Column);
	/** The table's own column (as listed) that holds Column; table 4's columns run the other way. */
	int32 TableColumn(int32 Table, int32 Column);
	/** Table 1 has six phases, tables 2 to 4 four; 0 for anything else. */
	int32 PhaseCount(int32 Table);
	/** The color of a phase (row, as listed) in the table's own column (as listed); black if out of range. */
	FColor TableColor(int32 Table, int32 Phase, int32 OwnColumn);

	struct FHSV
	{
		float Hue = 0.f;
		float Sat = 0.f;
		float Value = 0.f;
	};
	/** HSV of a display sRGB color. */
	FHSV ToHSV(FColor Color);
	/** From A toward B by T, the short way round the hue wheel; 0 to 360. */
	float LerpHue(float A, float B, float T);

	/** A window in day fractions; End may pass 1 (wraps into the next day). */
	struct FWindow
	{
		float Start = 0.f;
		float End = 0.f;
	};
	/** Sunrise from Twilight X to Y, sunset from Z to W, widened or narrowed by Scale around the middle. */
	FWindow Window(const FVector4f& Twilight, bool bSunrise, float Scale);
	/** 0 to 1 through the window, wrapping round midnight; -1 outside it. */
	float Position(float DayFraction, const FWindow& Window);
	/** The plateau: 0 outside the window, up to 1 over its first 20%, back to 0 over its last 20%. */
	float Weight(float Position);
	/**
	 * The storm rule: the weight is multiplied by 1 - Rain x StormFade (both clamped to 0 to 1). Overcast is not
	 * used: the overcast cloud layer is opaque through most of every Cloudy and Rainy day.
	 */
	float StormFactor(float Rain, float StormFade);

	/**
	 * A ramp through Stops, sorted by HSV value: brightest at Position 0 when bBrightFirst (sunset window),
	 * at Position 1 otherwise (sunrise window). Evenly spaced; the hue moves the short way round, saturation
	 * and value in a straight line. The order of Stops does not matter.
	 */
	FHSV Ramp(TArrayView<const FColor> Stops, float Position, bool bBrightFirst);
	/** The ramp of palette Preset (1 to 4) in a window: brightest at the start of sunset and the end of sunrise. */
	FHSV RampColor(int32 Preset, float Position, bool bSunrise);
	/**
	 * Set B: the table's phases placed by time through the window, in Column (0 horizon, 1 mid-sky, 2 zenith).
	 * Sunset: table 1 golden hour 0, civil twilight 0.4, nautical twilight 0.75, night 1; tables 2 to 4 dusk 0,
	 * twilight 0.5, night 1. Sunrise: table 1 night 0, blue hour 0.4, dawn 1; tables 2 to 4 night 0, dawn 1.
	 */
	FHSV TableRamp(int32 Table, int32 Column, float Position, bool bSunrise);

	/**
	 * Recolors a linear color (the renderer's) in display sRGB HSV: hue and saturation toward Hue and Sat by
	 * Amount. The value is kept times Lerp(1, Bright, Amount); with Value 0 or more it moves toward Value x Bright
	 * by Amount instead. Amount 0 returns Color as it is. Alpha is kept.
	 */
	FLinearColor Recolor(const FLinearColor& Color, float Hue, float Sat, float Bright, float Amount, float Value = -1.f);

	/** Stage 2: one window's offsets for a day, each -1 to 1. */
	struct FDayOffset
	{
		float Hue = 0.f;
		float Sat = 0.f;
		float Bright = 0.f;
	};
	struct FDayOffsets
	{
		FDayOffset Sunrise;
		FDayOffset Sunset;
	};
	/** The six offsets of a day, from a fixed integer hash of the day number (a different salt for each). */
	FDayOffsets DayOffsets(int32 DayNumber);
	/**
	 * The target moved by a day's offsets times Variation (0 to 1): the hue by up to MaxHueShift degrees (wrapped to
	 * 0 to 360), the saturation times 1 + offset x MaxSatScale. The value is not changed.
	 */
	FHSV VaryTarget(FHSV Target, const FDayOffset& Offset, float Variation);
	/** The brightness factor moved by up to MaxBrightShift times Variation, clamped to 0.5 to 1.5. */
	float VaryBright(float Bright, const FDayOffset& Offset, float Variation);

	/**
	 * Called by AACESkyDomeActor::UpdateImportedSky right after the renderer's Update: recolors its FogColor and
	 * SkyFogColor (FogColor alpha 1, SkyFogColor's alpha kept) and sets personal.FogColor.Current.
	 * Both take Column. With the atmospheric tables and SplitHaze 1 it also works out the horizon band's color
	 * (see BandColor): the AC2 FogColor recolored in HazeColumn.
	 * DayNumber is the server's day (BuiltDayNumber); the day-to-day offsets use it plus personal.FogColor.DayOffset.
	 * Overcast is only shown in Current; the amount uses Rain (StormFactor).
	 */
	void Apply(FACELegionsSkyRenderer& Sky, float DayFraction, const FVector4f& Twilight, const FString& DayName, int32 DayNumber,
		float Overcast, float Rain);

	/**
	 * Called by AACESkyDomeActor::UpdateFog for the horizon band's FillColor. Returns the band color of the last Apply
	 * when bImportedVisible and the feature, the atmospheric tables and SplitHaze 1 were in use there (alpha as
	 * FogColor's); otherwise FogColor unchanged.
	 */
	FLinearColor BandColor(const FLinearColor& FogColor, bool bImportedVisible);
}
