#include "Personal/PersonalFogColor.h"
#include "Sky/ACELegionsSkyRenderer.h"
#include "ACERuntimeOptions.h"
#include "HAL/IConsoleManager.h"

// Helpers in an anonymous namespace inside PersonalFogColor: in a unity build they cannot clash with,
// or be hidden by, names of other files.
namespace PersonalFogColor
{
namespace
{
	TAutoConsoleVariable<int32> CVarDraw(TEXT("personal.FogColor.Draw"), -1,
		TEXT("-1 follows the Fog Colors by Weather check box, 0 off, 1 on. Needs the imported AC2 sky."));
	TAutoConsoleVariable<float> CVarStrength(TEXT("personal.FogColor.Strength"), 1.f,
		TEXT("0 to 1: how far the hue and saturation move toward the colors (1 all the way)."));
	TAutoConsoleVariable<float> CVarSaturation(TEXT("personal.FogColor.Saturation"), -1.f,
		TEXT("-1 follows the Fog Color Saturation option; 0 to 1.5 overrides it for this session. Scales the saturation of every palette, table and custom color."));
	TAutoConsoleVariable<float> CVarVariation(TEXT("personal.FogColor.Variation"), .5f,
		TEXT("0 to 1: how much the sunrise and sunset colors change from day to day (hue, saturation and brightness from the day number)."));
	TAutoConsoleVariable<int32> CVarDayOffset(TEXT("personal.FogColor.DayOffset"), 0,
		TEXT("Preview: the day used for the day-to-day change is the server's day number plus this (0 is today). Not saved."));
	TAutoConsoleVariable<float> CVarWindow(TEXT("personal.FogColor.Window"), 1.5f,
		TEXT("0.5 to 2: widens or narrows the sunrise and sunset windows around their middles."));
	TAutoConsoleVariable<float> CVarStormFade(TEXT("personal.FogColor.StormFade"), .35f,
		TEXT("0 to 1: how much falling rain removes the colors (1 all of them at full rain; 0 keeps them through rain)."));
	TAutoConsoleVariable<int32> CVarWeather(TEXT("personal.FogColor.Weather"), -1,
		TEXT("-1 follows the server's day group; 0 Clear, 1 Sunny, 2 Cloudy, 3 Rainy forces that weather's colors."));
	TAutoConsoleVariable<int32> CVarSet(TEXT("personal.FogColor.Set"), 1,
		TEXT("0 palettes (set A), 1 atmospheric tables (set B)."));
	TAutoConsoleVariable<int32> CVarColumn(TEXT("personal.FogColor.Column"), 0,
		TEXT("Atmospheric tables: 0 horizon, 1 mid-sky, 2 zenith (Foggy: distant horizon, mid-distance mist, near ground)."));
	TAutoConsoleVariable<int32> CVarTableBrightness(TEXT("personal.FogColor.TableBrightness"), 0,
		TEXT("Atmospheric tables: 1 also moves the brightness toward the table color; 0 keeps the AC2 brightness."));
	TAutoConsoleVariable<int32> CVarSplitHaze(TEXT("personal.FogColor.SplitHaze"), 1,
		TEXT("Atmospheric tables: 1 colors the horizon band from HazeColumn (the fog keeps Column); 0 the band follows the fog."));
	TAutoConsoleVariable<int32> CVarHazeColumn(TEXT("personal.FogColor.HazeColumn"), 1,
		TEXT("Atmospheric tables with SplitHaze 1: the horizon band's column, 0 horizon, 1 mid-sky, 2 zenith (Foggy: distant horizon, mid-distance mist, near ground)."));
	TAutoConsoleVariable<FString> CVarCurrent(TEXT("personal.FogColor.Current"), TEXT("Not active: needs the imported AC2 sky, outdoors."),
		TEXT("Read only: what the fog colors use now (weather, day, set, palettes or table, band column, saturation, day offsets, window weights, rain, overcast, amount)."), ECVF_ReadOnly);

	FString Name(const TCHAR* Weather, const TCHAR* Field)
	{
		return FString::Printf(TEXT("personal.FogColor.%s.%s"), Weather, Field);
	}

	/** The console variables of one weather type. */
	struct FWeatherVariables
	{
		TAutoConsoleVariable<int32> Table;
		TAutoConsoleVariable<int32> SunrisePalette;
		TAutoConsoleVariable<int32> SunsetPalette;
		TAutoConsoleVariable<float> SunriseHue;
		TAutoConsoleVariable<float> SunriseSat;
		TAutoConsoleVariable<float> SunsetHue;
		TAutoConsoleVariable<float> SunsetSat;
		TAutoConsoleVariable<float> SunriseBright;
		TAutoConsoleVariable<float> SunsetBright;

		FWeatherVariables(const TCHAR* Weather, int32 InTable, int32 InSunrise, int32 InSunset, float InSunriseBright, float InSunsetBright)
			: Table(*Name(Weather, TEXT("Table")), InTable,
				TEXT("Atmospheric table for this weather: 1 Clear sky, 2 Overcast and rainy, 3 Hazy, 4 Foggy."))
			, SunrisePalette(*Name(Weather, TEXT("SunrisePalette")), InSunrise,
				TEXT("Sunrise palette for this weather: 0 AC2, 1 dawn and dusk, 2 Dawn to Dusk, 3 Dusk and Dawn, 4 From Dusk to Dawn, 5 Custom."))
			, SunsetPalette(*Name(Weather, TEXT("SunsetPalette")), InSunset,
				TEXT("Sunset palette for this weather: 0 AC2, 1 dawn and dusk, 2 Dawn to Dusk, 3 Dusk and Dawn, 4 From Dusk to Dawn, 5 Custom."))
			, SunriseHue(*Name(Weather, TEXT("SunriseHue")), 15.f, TEXT("Custom sunrise hue, 0 to 360 degrees."))
			, SunriseSat(*Name(Weather, TEXT("SunriseSat")), .45f, TEXT("Custom sunrise saturation, 0 to 1."))
			, SunsetHue(*Name(Weather, TEXT("SunsetHue")), 20.f, TEXT("Custom sunset hue, 0 to 360 degrees."))
			, SunsetSat(*Name(Weather, TEXT("SunsetSat")), .70f, TEXT("Custom sunset saturation, 0 to 1."))
			, SunriseBright(*Name(Weather, TEXT("SunriseBright")), InSunriseBright, TEXT("Sunrise brightness factor for this weather, 0.5 to 1.5 (1 keeps the AC2 brightness)."))
			, SunsetBright(*Name(Weather, TEXT("SunsetBright")), InSunsetBright, TEXT("Sunset brightness factor for this weather, 0.5 to 1.5 (1 keeps the AC2 brightness)."))
		{
		}
	};

	// Starting setup: palettes Clear 4/1, Sunny 1/3, Cloudy 2/2, Rainy 2/2; tables Clear 1, Sunny 3, Cloudy 4, Rainy 2;
	// sunrise and sunset brightness Clear .85/.8, Sunny .9/.85, Cloudy .85/.8, Rainy .75/.75.
	FWeatherVariables Weathers[PersonalFogColor::WeatherCount] = {
		{TEXT("Clear"), 1, 4, 1, .85f, .8f},
		{TEXT("Sunny"), 3, 1, 3, .9f, .85f},
		{TEXT("Cloudy"), 4, 2, 2, .85f, .8f},
		{TEXT("Rainy"), 2, 2, 2, .75f, .75f}};

	constexpr FColor Hex(uint32 RGB)
	{
		return FColor(uint8(RGB >> 16), uint8(RGB >> 8), uint8(RGB), 255);
	}

	// Palettes 1 to 4, light to dark as listed (color-hex.com 1052412, 36761, 7319, 19576).
	const FColor Palettes[4][5] = {
		{Hex(0xf8b196), Hex(0xf67280), Hex(0xc06c84), Hex(0x6c5b7b), Hex(0x355c7d)},
		{Hex(0xf2f2b8), Hex(0xccb7c0), Hex(0x9b6fa7), Hex(0x4f1b74), Hex(0x29005e)},
		{Hex(0xbb6800), Hex(0xae360d), Hex(0xae0a1e), Hex(0x920420), Hex(0x661a34)},
		{Hex(0xffcdef), Hex(0xff9eda), Hex(0xd08fd9), Hex(0x8f8bd5), Hex(0x5d44b7)}};

	// Atmospheric tables, rows (phases) and columns as listed.
	const FColor Table1[6][3] = {
		{Hex(0xF4A261), Hex(0xE76F51), Hex(0x2A4D69)}, // golden hour (sunset)
		{Hex(0xD97E63), Hex(0x6C5B7B), Hex(0x1D2D44)}, // civil twilight (dusk)
		{Hex(0x8C4855), Hex(0x355C7D), Hex(0x0D1321)}, // nautical twilight
		{Hex(0x1B263B), Hex(0x0D1B2A), Hex(0x05080F)}, // astronomical night
		{Hex(0x4E5978), Hex(0x2B3A67), Hex(0x101726)}, // blue hour (pre-dawn)
		{Hex(0xFAD6B8), Hex(0xE27E7E), Hex(0x3E5A8C)}}; // dawn / sunrise
	// Tables 2 to 4: dusk, twilight, night, dawn.
	const FColor Tables[3][4][3] = {
		{{Hex(0x8D99AE), Hex(0x5C677D), Hex(0x2B2D42)}, {Hex(0x6B7080), Hex(0x434A59), Hex(0x1E222B)},
		 {Hex(0x2A2E38), Hex(0x181B22), Hex(0x0A0C10)}, {Hex(0xA3B1C6), Hex(0x738299), Hex(0x333C4D)}},
		{{Hex(0xEB8126), Hex(0xAB5A74), Hex(0x3B2840)}, {Hex(0xC85A53), Hex(0x7E5072), Hex(0x241929)},
		 {Hex(0x382633), Hex(0x1F1924), Hex(0x0E0B12)}, {Hex(0xE9B661), Hex(0xD9908D), Hex(0x48314C)}},
		// Foggy: near ground, mid-distance mist, distant horizon.
		{{Hex(0x7A727A), Hex(0xA69BA3), Hex(0xD4C5C7)}, {Hex(0x4A5260), Hex(0x6E788A), Hex(0x98A2B3)},
		 {Hex(0x1C2129), Hex(0x2E3542), Hex(0x475163)}, {Hex(0x8A94A6), Hex(0xB8C2D1), Hex(0xE6EBF2)}}};

	/** A phase and its place in the window. */
	struct FStop
	{
		int32 Phase;
		float At;
	};
	const FStop Table1Sunset[] = {{0, 0.f}, {1, .4f}, {2, .75f}, {3, 1.f}};
	const FStop Table1Sunrise[] = {{3, 0.f}, {4, .4f}, {5, 1.f}};
	const FStop OtherSunset[] = {{0, 0.f}, {1, .5f}, {2, 1.f}};
	const FStop OtherSunrise[] = {{2, 0.f}, {3, 1.f}};

	float Encode(float Linear)
	{
		Linear = FMath::Clamp(Linear, 0.f, 1.f);
		return Linear <= .0031308f ? Linear * 12.92f : 1.055f * FMath::Pow(Linear, 1.f / 2.4f) - .055f;
	}

	float Decode(float Display)
	{
		Display = FMath::Clamp(Display, 0.f, 1.f);
		return Display <= .04045f ? Display / 12.92f : FMath::Pow((Display + .055f) / 1.055f, 2.4f);
	}

	PersonalFogColor::FHSV Mix(const PersonalFogColor::FHSV& A, const PersonalFogColor::FHSV& B, float T)
	{
		return {PersonalFogColor::LerpHue(A.Hue, B.Hue, T), FMath::Lerp(A.Sat, B.Sat, T), FMath::Lerp(A.Value, B.Value, T)};
	}

	void SetCurrent(const FString& Text)
	{
		static FString Last;
		if (Text != Last)
		{
			Last = Text;
			CVarCurrent->Set(*Text, ECVF_SetByCode);
		}
	}

	// The horizon band's color from the last Apply (game thread only), and whether it is in use:
	// the feature on, the atmospheric tables and SplitHaze 1.
	bool bBandInUse = false;
	FLinearColor Band = FLinearColor::Black;

	/** A fixed 32-bit integer hash (lowbias32): the same on every client and run, unlike the engine's random state. */
	constexpr uint32 Hash(uint32 X)
	{
		X ^= X >> 16;
		X *= 0x7feb352du;
		X ^= X >> 15;
		X *= 0x846ca68bu;
		X ^= X >> 16;
		return X;
	}

	/** -1 to 1 for a day and a salt. */
	float Unit(int32 DayNumber, uint32 Salt)
	{
		const uint32 H = Hash(Hash(uint32(DayNumber)) ^ Hash(Salt * 0x9e3779b9u));
		return float(H >> 8) * (2.f / 16777215.f) - 1.f;
	}
}
}

bool PersonalFogColor::IsOn()
{
	const int32 Draw = CVarDraw.GetValueOnGameThread();
	return Draw < 0 ? ACERuntimeOptions::Get(OptionKey) > .5f : Draw > 0;
}

const TCHAR* PersonalFogColor::WeatherName(int32 Weather)
{
	static const TCHAR* Names[] = {TEXT("Clear"), TEXT("Sunny"), TEXT("Cloudy"), TEXT("Rainy")};
	return Weather >= 0 && Weather < WeatherCount ? Names[Weather] : TEXT("");
}

int32 PersonalFogColor::WeatherOf(const FString& DayName)
{
	for (int32 W = 0; W < WeatherCount; ++W)
	{
		if (DayName == WeatherName(W))
		{
			return W;
		}
	}
	return -1;
}

const TCHAR* PersonalFogColor::PresetName(int32 Preset)
{
	static const TCHAR* Names[] = {TEXT("AC2"), TEXT("dawn and dusk"), TEXT("Dawn to Dusk"), TEXT("Dusk and Dawn"), TEXT("From Dusk to Dawn"), TEXT("Custom")};
	return Preset >= 0 && Preset < PresetCount ? Names[Preset] : TEXT("");
}

TArrayView<const FColor> PersonalFogColor::Palette(int32 Preset)
{
	return Preset >= 1 && Preset <= 4 ? TArrayView<const FColor>(Palettes[Preset - 1], 5) : TArrayView<const FColor>();
}

const TCHAR* PersonalFogColor::TableName(int32 Table)
{
	static const TCHAR* Names[] = {TEXT("Clear sky"), TEXT("Overcast and rainy"), TEXT("Hazy"), TEXT("Foggy")};
	return Table >= 1 && Table <= TableCount ? Names[Table - 1] : TEXT("");
}

const TCHAR* PersonalFogColor::ColumnName(int32 Column)
{
	static const TCHAR* Names[] = {TEXT("Horizon"), TEXT("Mid-sky"), TEXT("Zenith")};
	return Column >= 0 && Column < ColumnCount ? Names[Column] : TEXT("");
}

int32 PersonalFogColor::TableColumn(int32 Table, int32 Column)
{
	Column = FMath::Clamp(Column, 0, ColumnCount - 1);
	return Table == 4 ? ColumnCount - 1 - Column : Column;
}

int32 PersonalFogColor::PhaseCount(int32 Table)
{
	return Table == 1 ? 6 : (Table >= 2 && Table <= TableCount ? 4 : 0);
}

FColor PersonalFogColor::TableColor(int32 Table, int32 Phase, int32 OwnColumn)
{
	if (Phase < 0 || Phase >= PhaseCount(Table) || OwnColumn < 0 || OwnColumn >= ColumnCount)
	{
		return FColor::Black;
	}
	return Table == 1 ? Table1[Phase][OwnColumn] : Tables[Table - 2][Phase][OwnColumn];
}

PersonalFogColor::FHSV PersonalFogColor::ToHSV(FColor Color)
{
	const FLinearColor HSV = FLinearColor(Color.R / 255.f, Color.G / 255.f, Color.B / 255.f).LinearRGBToHSV();
	return {HSV.R, HSV.G, HSV.B};
}

float PersonalFogColor::LerpHue(float A, float B, float T)
{
	const float Delta = FMath::Fmod(B - A + 540.f, 360.f) - 180.f;
	float Hue = FMath::Fmod(A + Delta * T, 360.f);
	return Hue < 0.f ? Hue + 360.f : Hue;
}

PersonalFogColor::FWindow PersonalFogColor::Window(const FVector4f& Twilight, bool bSunrise, float Scale)
{
	const float Start = bSunrise ? Twilight.X : Twilight.Z;
	const float End = bSunrise ? Twilight.Y : Twilight.W;
	const float Middle = (Start + End) * .5f;
	const float Half = (End - Start) * .5f * FMath::Clamp(Scale, .5f, 2.f);
	return {Middle - Half, Middle + Half};
}

float PersonalFogColor::Position(float DayFraction, const FWindow& W)
{
	const float Length = W.End - W.Start;
	if (Length <= 0.f)
	{
		return -1.f;
	}
	for (const float Offset : {0.f, 1.f, -1.f})
	{
		const float P = (DayFraction + Offset - W.Start) / Length;
		if (P >= 0.f && P <= 1.f)
		{
			return P;
		}
	}
	return -1.f;
}

float PersonalFogColor::Weight(float P)
{
	if (P < 0.f || P > 1.f)
	{
		return 0.f;
	}
	return FMath::SmoothStep(0.f, .2f, P) * (1.f - FMath::SmoothStep(.8f, 1.f, P));
}

float PersonalFogColor::StormFactor(float Rain, float StormFade)
{
	return 1.f - FMath::Clamp(Rain, 0.f, 1.f) * FMath::Clamp(StormFade, 0.f, 1.f);
}

PersonalFogColor::FHSV PersonalFogColor::Ramp(TArrayView<const FColor> Stops, float P, bool bBrightFirst)
{
	if (Stops.IsEmpty())
	{
		return {};
	}
	TArray<TPair<FColor, FHSV>, TInlineAllocator<8>> Sorted;
	for (const FColor& C : Stops)
	{
		Sorted.Add({C, ToHSV(C)});
	}
	// Brightest first; equal values paler first (as the palettes are listed), then by hue and the color itself,
	// so any listed order gives the same ramp.
	Sorted.Sort([](const TPair<FColor, FHSV>& A, const TPair<FColor, FHSV>& B)
	{
		if (A.Value.Value != B.Value.Value) return A.Value.Value > B.Value.Value;
		if (A.Value.Sat != B.Value.Sat) return A.Value.Sat < B.Value.Sat;
		if (A.Value.Hue != B.Value.Hue) return A.Value.Hue > B.Value.Hue;
		return A.Key.DWColor() > B.Key.DWColor();
	});
	if (Sorted.Num() == 1)
	{
		return Sorted[0].Value;
	}
	P = FMath::Clamp(bBrightFirst ? P : 1.f - P, 0.f, 1.f);
	const float At = P * (Sorted.Num() - 1);
	const int32 I = FMath::Min(FMath::FloorToInt32(At), Sorted.Num() - 2);
	return Mix(Sorted[I].Value, Sorted[I + 1].Value, At - I);
}

PersonalFogColor::FHSV PersonalFogColor::RampColor(int32 Preset, float P, bool bSunrise)
{
	return Ramp(Palette(Preset), P, !bSunrise);
}

PersonalFogColor::FHSV PersonalFogColor::TableRamp(int32 Table, int32 Column, float P, bool bSunrise)
{
	if (PhaseCount(Table) == 0)
	{
		return {};
	}
	TArrayView<const FStop> Stops = Table == 1
		? (bSunrise ? TArrayView<const FStop>(Table1Sunrise) : TArrayView<const FStop>(Table1Sunset))
		: (bSunrise ? TArrayView<const FStop>(OtherSunrise) : TArrayView<const FStop>(OtherSunset));
	const int32 Own = TableColumn(Table, Column);
	P = FMath::Clamp(P, 0.f, 1.f);
	int32 I = 0;
	while (I + 2 < Stops.Num() && P > Stops[I + 1].At)
	{
		++I;
	}
	const FStop& A = Stops[I];
	const FStop& B = Stops[I + 1];
	const float T = FMath::Clamp((P - A.At) / FMath::Max(B.At - A.At, KINDA_SMALL_NUMBER), 0.f, 1.f);
	return Mix(ToHSV(TableColor(Table, A.Phase, Own)), ToHSV(TableColor(Table, B.Phase, Own)), T);
}

FLinearColor PersonalFogColor::Recolor(const FLinearColor& Color, float Hue, float Sat, float Bright, float Amount, float Value)
{
	Amount = FMath::Clamp(Amount, 0.f, 1.f);
	if (Amount <= 0.f)
	{
		return Color;
	}
	const FLinearColor HSV = FLinearColor(Encode(Color.R), Encode(Color.G), Encode(Color.B)).LinearRGBToHSV();
	// A gray has no hue of its own: take the target's.
	const float NewHue = HSV.G < .02f ? Hue : LerpHue(HSV.R, Hue, Amount);
	const float NewSat = FMath::Lerp(HSV.G, FMath::Clamp(Sat, 0.f, 1.f), Amount);
	const float NewValue = Value >= 0.f ? FMath::Lerp(HSV.B, Value * Bright, Amount) : HSV.B * FMath::Lerp(1.f, Bright, Amount);
	const FLinearColor Display = FLinearColor(NewHue, NewSat, FMath::Clamp(NewValue, 0.f, 1.f)).HSVToLinearRGB();
	return FLinearColor(Decode(Display.R), Decode(Display.G), Decode(Display.B), Color.A);
}

float PersonalFogColor::SaturationInUse(bool* bOverride)
{
	const float Override = CVarSaturation.GetValueOnGameThread();
	if (bOverride)
	{
		*bOverride = Override >= 0.f;
	}
	return FMath::Clamp(Override >= 0.f ? Override : ACERuntimeOptions::Get(SaturationOptionKey), 0.f, 1.5f);
}

PersonalFogColor::FDayOffsets PersonalFogColor::DayOffsets(int32 DayNumber)
{
	FDayOffsets O;
	O.Sunrise = {Unit(DayNumber, 1), Unit(DayNumber, 2), Unit(DayNumber, 3)};
	O.Sunset = {Unit(DayNumber, 4), Unit(DayNumber, 5), Unit(DayNumber, 6)};
	return O;
}

PersonalFogColor::FHSV PersonalFogColor::VaryTarget(FHSV Target, const FDayOffset& Offset, float Variation)
{
	Variation = FMath::Clamp(Variation, 0.f, 1.f);
	float Hue = FMath::Fmod(Target.Hue + Offset.Hue * MaxHueShift * Variation, 360.f);
	Target.Hue = Hue < 0.f ? Hue + 360.f : Hue;
	Target.Sat *= 1.f + Offset.Sat * MaxSatScale * Variation;
	return Target;
}

float PersonalFogColor::VaryBright(float Bright, const FDayOffset& Offset, float Variation)
{
	return FMath::Clamp(Bright + Offset.Bright * MaxBrightShift * FMath::Clamp(Variation, 0.f, 1.f), .5f, 1.5f);
}

void PersonalFogColor::Apply(FACELegionsSkyRenderer& Sky, float DayFraction, const FVector4f& Twilight, const FString& DayName, int32 DayNumber,
	float Overcast, float Rain)
{
	bBandInUse = false;
	if (!IsOn())
	{
		SetCurrent(CVarDraw.GetValueOnGameThread() == 0 ? TEXT("Off (Fog colors set to Off here).") : TEXT("Off (Fog Colors by Weather is unchecked)."));
		return;
	}
	const int32 Forced = CVarWeather.GetValueOnGameThread();
	const bool bForced = Forced >= 0 && Forced < WeatherCount;
	const int32 Weather = bForced ? Forced : WeatherOf(DayName);
	if (Weather < 0)
	{
		SetCurrent(FString::Printf(TEXT("On. Day %d, '%s': not Clear, Sunny, Cloudy or Rainy, so the AC2 colors stay."), DayNumber, *DayName));
		return;
	}
	const FWeatherVariables& Vars = Weathers[Weather];
	const bool bTables = CVarSet.GetValueOnGameThread() == 1;
	const int32 Column = FMath::Clamp(CVarColumn.GetValueOnGameThread(), 0, ColumnCount - 1);
	const bool bSplitHaze = bTables && FMath::Clamp(CVarSplitHaze.GetValueOnGameThread(), 0, 1) == 1;
	const int32 HazeColumn = FMath::Clamp(CVarHazeColumn.GetValueOnGameThread(), 0, ColumnCount - 1);
	const int32 Table = FMath::Clamp(Vars.Table.GetValueOnGameThread(), 1, TableCount);
	const bool bTableBrightness = CVarTableBrightness.GetValueOnGameThread() != 0;
	const float Strength = FMath::Clamp(CVarStrength.GetValueOnGameThread(), 0.f, 1.f);
	bool bSaturationOverride = false;
	const float Saturation = SaturationInUse(&bSaturationOverride);
	const float Scale = FMath::Clamp(CVarWindow.GetValueOnGameThread(), .5f, 2.f);
	const float Storm = StormFactor(Rain, CVarStormFade.GetValueOnGameThread());
	// Stage 2: the day's offsets, the same for the land fog and the horizon band and for both color sets.
	const float Variation = FMath::Clamp(CVarVariation.GetValueOnGameThread(), 0.f, 1.f);
	const int32 DayOffset = CVarDayOffset.GetValueOnGameThread();
	const int32 Day = DayNumber + DayOffset;
	const FDayOffsets Offsets = DayOffsets(Day);

	float Weights[2] = {0.f, 0.f};
	int32 Presets[2] = {0, 0};
	float InUse = 0.f;
	// The horizon band starts from the AC2 FogColor as Update set it, before the recolor below.
	FLinearColor BandNow = Sky.FogColor;
	for (const bool bSunrise : {true, false})
	{
		const int32 Index = bSunrise ? 0 : 1;
		const float P = Position(DayFraction, Window(Twilight, bSunrise, Scale));
		Weights[Index] = Weight(P);
		const int32 Preset = FMath::Clamp((bSunrise ? Vars.SunrisePalette : Vars.SunsetPalette).GetValueOnGameThread(), 0, PresetCount - 1);
		Presets[Index] = Preset;
		const float Amount = Weights[Index] * Strength * Storm;
		if (Amount <= 0.f || (!bTables && Preset == PresetAC2))
		{
			continue;
		}
		InUse = FMath::Max(InUse, Amount);
		FHSV Target;
		if (bTables)
		{
			Target = TableRamp(Table, Column, P, bSunrise);
		}
		else if (Preset == PresetCustom)
		{
			Target.Hue = FMath::Fmod(FMath::Fmod((bSunrise ? Vars.SunriseHue : Vars.SunsetHue).GetValueOnGameThread(), 360.f) + 360.f, 360.f);
			Target.Sat = FMath::Clamp((bSunrise ? Vars.SunriseSat : Vars.SunsetSat).GetValueOnGameThread(), 0.f, 1.f);
		}
		else
		{
			Target = RampColor(Preset, P, bSunrise);
		}
		const FDayOffset& Offset = bSunrise ? Offsets.Sunrise : Offsets.Sunset;
		Target = VaryTarget(Target, Offset, Variation);
		const float Bright = VaryBright(FMath::Clamp((bSunrise ? Vars.SunriseBright : Vars.SunsetBright).GetValueOnGameThread(), .5f, 1.5f), Offset, Variation);
		const float Sat = FMath::Clamp(Target.Sat * Saturation, 0.f, 1.f);
		const float Value = bTables && bTableBrightness ? Target.Value : -1.f;
		Sky.FogColor = Recolor(Sky.FogColor, Target.Hue, Sat, Bright, Amount, Value);
		Sky.FogColor.A = 1.f;
		Sky.SkyFogColor = Recolor(Sky.SkyFogColor, Target.Hue, Sat, Bright, Amount, Value);
		if (bSplitHaze)
		{
			// The horizon band: same table, phases, weight, amount, brightness factor and table brightness rule, in HazeColumn.
			const FHSV BandTarget = VaryTarget(TableRamp(Table, HazeColumn, P, bSunrise), Offset, Variation);
			const float BandValue = bTableBrightness ? BandTarget.Value : -1.f;
			BandNow = Recolor(BandNow, BandTarget.Hue, FMath::Clamp(BandTarget.Sat * Saturation, 0.f, 1.f), Bright, Amount, BandValue);
		}
	}
	if (bSplitHaze)
	{
		bBandInUse = true;
		Band = BandNow;
	}

	const FString Colors = bTables
		? FString::Printf(TEXT("Atmospheric tables, %s, %s%s%s"), ColumnName(Column), TableName(Table),
			bSplitHaze ? *FString::Printf(TEXT(", band %s"), ColumnName(HazeColumn)) : TEXT(""),
			bTableBrightness ? TEXT(", table brightness") : TEXT(""))
		: FString::Printf(TEXT("Palettes: sunrise %s, sunset %s"), PresetName(Presets[0]), PresetName(Presets[1]));
	// The offsets in use for the active window (the one with the larger weight).
	FString Variety = TEXT("outside the windows");
	if (Weights[0] > 0.f || Weights[1] > 0.f)
	{
		const bool bSunrise = Weights[0] >= Weights[1];
		const FDayOffset& O = bSunrise ? Offsets.Sunrise : Offsets.Sunset;
		Variety = FString::Printf(TEXT("%s hue %+.0f°, sat %+.0f%%, bright %+.2f"), bSunrise ? TEXT("sunrise") : TEXT("sunset"),
			O.Hue * MaxHueShift * Variation, O.Sat * MaxSatScale * Variation * 100.f, O.Bright * MaxBrightShift * Variation);
	}
	SetCurrent(FString::Printf(TEXT("On. %s (%s), day %d (%+d). %s; saturation %.2f (%s). Variation %.2f: %s. Weights: sunrise %.2f, sunset %.2f; rain %.2f, overcast %.2f; amount %.2f."),
		WeatherName(Weather), bForced ? TEXT("forced") : TEXT("server"), Day, DayOffset, *Colors, Saturation,
		bSaturationOverride ? TEXT("dashboard") : TEXT("option"), Variation, *Variety, Weights[0], Weights[1],
		FMath::Clamp(Rain, 0.f, 1.f), FMath::Clamp(Overcast, 0.f, 1.f), InUse));
}

FLinearColor PersonalFogColor::BandColor(const FLinearColor& FogColor, bool bImportedVisible)
{
	if (!bImportedVisible || !bBandInUse)
	{
		return FogColor;
	}
	FLinearColor Color = Band;
	Color.A = FogColor.A;
	return Color;
}
