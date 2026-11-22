#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Algo/Reverse.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/IConsoleManager.h"
#include "Personal/PersonalFogColor.h"
#include "Personal/PersonalSliderLabel.h"
#include "Sky/ACELegionsSkyRenderer.h"
#include "ACERuntimeOptions.h"

// Fog colors by weather: option row, weather names, windows and weights, palette and table ramps, recolor, storm rule, Apply.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFogColorTest, "ACE.Personal.FogColor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFogColorTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFogColor;

	// The options are read from a settings file of the test's own, never the developer's.
	const FString PreviousIni = GGameUserSettingsIni;
	GGameUserSettingsIni = FPaths::ProjectSavedDir() / TEXT("Automation/FogColorPreferences.ini");
	FConfigFile Settings;
	Settings.NoSave = true;
	GConfig->SetFile(GGameUserSettingsIni, &Settings);
	GConfig->EmptySection(TEXT("ACE.Presentation"), GGameUserSettingsIni);
	ON_SCOPE_EXIT { GGameUserSettingsIni = PreviousIni; };

	// Every personal.FogColor variable: values and set-by priority are put back at the end.
	struct FSaved { IConsoleVariable* Variable; FString Value; uint32 SetBy; };
	TArray<FSaved> Saved;
	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda([&Saved](const TCHAR* Name, IConsoleObject* Object)
	{
		IConsoleVariable* Variable = Object->AsVariable();
		if (Variable && !Variable->TestFlags(ECVF_ReadOnly))
		{
			Saved.Add({Variable, Variable->GetString(), uint32(Variable->GetFlags()) & ECVF_SetByMask});
		}
	}), TEXT("personal.FogColor."));
	ON_SCOPE_EXIT
	{
		for (const FSaved& S : Saved)
		{
			S.Variable->Set(*S.Value, ECVF_SetByConsole);
			S.Variable->SetFlags(EConsoleVariableFlags((uint32(S.Variable->GetFlags()) & ~uint32(ECVF_SetByMask)) | S.SetBy));
		}
	};
	TestEqual(TEXT("49 settable variables"), Saved.Num(), 49);
	auto Var = [this](const TCHAR* Name) -> IConsoleVariable*
	{
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		TestNotNull(*FString::Printf(TEXT("Variable %s"), Name), V);
		return V;
	};
	auto SetVar = [&](const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* V = Var(Name))
		{
			V->Set(*FString::SanitizeFloat(Value), ECVF_SetByConsole);
		}
	};
	// Defaults of the plan (as chosen in game on 2026-11-22; Saturation -1 follows the option).
	struct FDefault { const TCHAR* Name; float Value; };
	TArray<FDefault> Defaults{
		{TEXT("personal.FogColor.Draw"), -1}, {TEXT("personal.FogColor.Strength"), 1}, {TEXT("personal.FogColor.Saturation"), -1},
		{TEXT("personal.FogColor.Window"), 1.5f}, {TEXT("personal.FogColor.StormFade"), .35f}, {TEXT("personal.FogColor.Weather"), -1}, {TEXT("personal.FogColor.Set"), 1},
		{TEXT("personal.FogColor.Column"), 0}, {TEXT("personal.FogColor.TableBrightness"), 0},
		{TEXT("personal.FogColor.SplitHaze"), 1}, {TEXT("personal.FogColor.HazeColumn"), 1},
		{TEXT("personal.FogColor.Variation"), .5f}, {TEXT("personal.FogColor.DayOffset"), 0},
		{TEXT("personal.FogColor.Clear.SunrisePalette"), 4}, {TEXT("personal.FogColor.Clear.SunsetPalette"), 1},
		{TEXT("personal.FogColor.Sunny.SunrisePalette"), 1}, {TEXT("personal.FogColor.Sunny.SunsetPalette"), 3},
		{TEXT("personal.FogColor.Cloudy.SunrisePalette"), 2}, {TEXT("personal.FogColor.Cloudy.SunsetPalette"), 2},
		{TEXT("personal.FogColor.Rainy.SunrisePalette"), 2}, {TEXT("personal.FogColor.Rainy.SunsetPalette"), 2},
		{TEXT("personal.FogColor.Clear.Table"), 1}, {TEXT("personal.FogColor.Sunny.Table"), 3},
		{TEXT("personal.FogColor.Cloudy.Table"), 4}, {TEXT("personal.FogColor.Rainy.Table"), 2},
		{TEXT("personal.FogColor.Rainy.SunriseHue"), 15}, {TEXT("personal.FogColor.Rainy.SunriseSat"), .45f},
		{TEXT("personal.FogColor.Rainy.SunsetHue"), 20}, {TEXT("personal.FogColor.Rainy.SunsetSat"), .7f}};
	// Sunrise and sunset brightness per weather.
	struct FBright { const TCHAR* Weather; float Sunrise; float Sunset; };
	const FBright Brights[] = {{TEXT("Clear"), .85f, .8f}, {TEXT("Sunny"), .9f, .85f}, {TEXT("Cloudy"), .85f, .8f}, {TEXT("Rainy"), .75f, .75f}};
	TArray<FString> BrightNames;
	for (const FBright& B : Brights)
	{
		BrightNames.Add(FString::Printf(TEXT("personal.FogColor.%s.SunriseBright"), B.Weather));
		BrightNames.Add(FString::Printf(TEXT("personal.FogColor.%s.SunsetBright"), B.Weather));
	}
	for (int32 I = 0; I < BrightNames.Num(); ++I)
	{
		Defaults.Add({*BrightNames[I], I % 2 == 0 ? Brights[I / 2].Sunrise : Brights[I / 2].Sunset});
	}
	for (const FDefault& D : Defaults)
	{
		if (IConsoleVariable* V = Var(D.Name))
		{
			TestEqual(*FString::Printf(TEXT("Default of %s"), D.Name), FCString::Atof(*V->GetDefaultValue()), D.Value, 1e-4f);
		}
	}
	IConsoleVariable* Current = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FogColor.Current"));
	TestTrue(TEXT("Current is a read-only variable"), Current && Current->TestFlags(ECVF_ReadOnly));
	// Start from the defaults, with the stage 1 values the checks below were written for: palettes, Saturation 1,
	// Window 1, Storm fade 1, no separate band, brightness 1, no day-to-day variation.
	for (const FSaved& S : Saved)
	{
		S.Variable->Set(*S.Variable->GetDefaultValue(), ECVF_SetByConsole);
	}
	SetVar(TEXT("personal.FogColor.Set"), 0);
	SetVar(TEXT("personal.FogColor.Saturation"), 1);
	SetVar(TEXT("personal.FogColor.Window"), 1);
	SetVar(TEXT("personal.FogColor.StormFade"), 1);
	SetVar(TEXT("personal.FogColor.SplitHaze"), 0);
	SetVar(TEXT("personal.FogColor.Variation"), 0);
	for (const FString& N : BrightNames)
	{
		SetVar(*N, 1);
	}

	// The option row: off by default.
	const ACERuntimeOptions::FOption* Row = nullptr;
	for (const ACERuntimeOptions::FOption& O : ACERuntimeOptions::Values)
	{
		if (FCString::Strcmp(O.Key, OptionKey) == 0)
		{
			Row = &O;
		}
	}
	if (TestNotNull(TEXT("Option row PersonalFogColor"), Row))
	{
		TestEqual(TEXT("Label"), FString(Row->Label), FString(TEXT("Fog Colors by Weather")));
		TestEqual(TEXT("Default 0"), Row->Default, 0.f);
		TestEqual(TEXT("Min 0"), Row->Min, 0.f);
		TestEqual(TEXT("Max 1"), Row->Max, 1.f);
	}
	TestFalse(TEXT("Nothing saved: off"), IsOn());
	ACERuntimeOptions::Set(OptionKey, 1.f);
	TestTrue(TEXT("Option on: on"), IsOn());
	SetVar(TEXT("personal.FogColor.Draw"), 0);
	TestFalse(TEXT("Draw 0: off"), IsOn());
	ACERuntimeOptions::Set(OptionKey, 0.f);
	SetVar(TEXT("personal.FogColor.Draw"), 1);
	TestTrue(TEXT("Draw 1: on with the option off"), IsOn());
	SetVar(TEXT("personal.FogColor.Draw"), -1);

	// Weather names.
	TestEqual(TEXT("Clear"), WeatherOf(TEXT("Clear")), Clear);
	TestEqual(TEXT("Sunny"), WeatherOf(TEXT("Sunny")), Sunny);
	TestEqual(TEXT("Cloudy"), WeatherOf(TEXT("Cloudy")), Cloudy);
	TestEqual(TEXT("Rainy"), WeatherOf(TEXT("Rainy")), Rainy);
	TestEqual(TEXT("Unknown name"), WeatherOf(TEXT("Snowy")), -1);
	TestEqual(TEXT("Empty name"), WeatherOf(TEXT("")), -1);
	TestEqual(TEXT("Preset names"), FString(PresetName(4)), FString(TEXT("From Dusk to Dawn")));

	// The four palettes and four tables, as given (hex codes from the plan).
	const TCHAR* PaletteHex[4][5] = {
		{TEXT("f8b196"), TEXT("f67280"), TEXT("c06c84"), TEXT("6c5b7b"), TEXT("355c7d")},
		{TEXT("f2f2b8"), TEXT("ccb7c0"), TEXT("9b6fa7"), TEXT("4f1b74"), TEXT("29005e")},
		{TEXT("bb6800"), TEXT("ae360d"), TEXT("ae0a1e"), TEXT("920420"), TEXT("661a34")},
		{TEXT("ffcdef"), TEXT("ff9eda"), TEXT("d08fd9"), TEXT("8f8bd5"), TEXT("5d44b7")}};
	for (int32 P = 1; P <= 4; ++P)
	{
		const TArrayView<const FColor> Colors = Palette(P);
		if (TestEqual(*FString::Printf(TEXT("Palette %d has five colors"), P), Colors.Num(), 5))
		{
			for (int32 I = 0; I < 5; ++I)
			{
				TestEqual(*FString::Printf(TEXT("Palette %d color %d"), P, I + 1), Colors[I], FColor::FromHex(PaletteHex[P - 1][I]));
			}
		}
	}
	TestEqual(TEXT("AC2 has no palette"), Palette(PresetAC2).Num(), 0);
	TestEqual(TEXT("Custom has no palette"), Palette(PresetCustom).Num(), 0);
	const TCHAR* TableHex[4][6][3] = {
		{{TEXT("F4A261"), TEXT("E76F51"), TEXT("2A4D69")}, {TEXT("D97E63"), TEXT("6C5B7B"), TEXT("1D2D44")}, {TEXT("8C4855"), TEXT("355C7D"), TEXT("0D1321")},
		 {TEXT("1B263B"), TEXT("0D1B2A"), TEXT("05080F")}, {TEXT("4E5978"), TEXT("2B3A67"), TEXT("101726")}, {TEXT("FAD6B8"), TEXT("E27E7E"), TEXT("3E5A8C")}},
		{{TEXT("8D99AE"), TEXT("5C677D"), TEXT("2B2D42")}, {TEXT("6B7080"), TEXT("434A59"), TEXT("1E222B")}, {TEXT("2A2E38"), TEXT("181B22"), TEXT("0A0C10")},
		 {TEXT("A3B1C6"), TEXT("738299"), TEXT("333C4D")}},
		{{TEXT("EB8126"), TEXT("AB5A74"), TEXT("3B2840")}, {TEXT("C85A53"), TEXT("7E5072"), TEXT("241929")}, {TEXT("382633"), TEXT("1F1924"), TEXT("0E0B12")},
		 {TEXT("E9B661"), TEXT("D9908D"), TEXT("48314C")}},
		{{TEXT("7A727A"), TEXT("A69BA3"), TEXT("D4C5C7")}, {TEXT("4A5260"), TEXT("6E788A"), TEXT("98A2B3")}, {TEXT("1C2129"), TEXT("2E3542"), TEXT("475163")},
		 {TEXT("8A94A6"), TEXT("B8C2D1"), TEXT("E6EBF2")}}};
	for (int32 T = 1; T <= 4; ++T)
	{
		TestEqual(*FString::Printf(TEXT("Table %d phases"), T), PhaseCount(T), T == 1 ? 6 : 4);
		for (int32 Phase = 0; Phase < PhaseCount(T); ++Phase)
		{
			for (int32 C = 0; C < 3; ++C)
			{
				TestEqual(*FString::Printf(TEXT("Table %d phase %d column %d"), T, Phase + 1, C + 1), TableColor(T, Phase, C), FColor::FromHex(TableHex[T - 1][Phase][C]));
			}
		}
	}

	// Windows and weights: 0 outside, 1 on the plateau; Window widens or narrows both.
	const FVector4f Twilight(.16f, .27f, .84f, .96f);
	auto WeightAt = [&](float Time, bool bSunrise, float Scale) { return Weight(Position(Time, Window(Twilight, bSunrise, Scale))); };
	TestEqual(TEXT("Sunrise middle: 1"), WeightAt(.215f, true, 1.f), 1.f);
	TestEqual(TEXT("Sunset middle: 1"), WeightAt(.90f, false, 1.f), 1.f);
	TestEqual(TEXT("Sunrise plateau edge (30%): 1"), WeightAt(.16f + .11f * .3f, true, 1.f), 1.f);
	TestEqual(TEXT("Window start: 0"), WeightAt(.84f, false, 1.f), 0.f);
	TestTrue(TEXT("Soft edge (10%): between 0 and 1"), WeightAt(.84f + .012f, false, 1.f) > 0.f && WeightAt(.84f + .012f, false, 1.f) < 1.f);
	for (const float Time : {.05f, .15f, .30f, .50f, .80f, .97f})
	{
		TestEqual(*FString::Printf(TEXT("Outside both windows at %.2f"), Time), WeightAt(Time, true, 1.f) + WeightAt(Time, false, 1.f), 0.f);
	}
	TestTrue(TEXT("Window 2: 0.30 is in the sunrise window"), WeightAt(.30f, true, 2.f) > 0.f);
	TestTrue(TEXT("Window 2: the sunset window passes midnight"), WeightAt(.005f, false, 2.f) > 0.f);
	TestEqual(TEXT("Window 0.5: 0.17 is outside"), WeightAt(.17f, true, .5f), 0.f);
	TestEqual(TEXT("Window 0.5: the middle is 1"), WeightAt(.90f, false, .5f), 1.f);

	// Ramp order: the brightest stop where the AC2 sky is brightest (start of sunset, end of sunrise).
	auto Same = [](const FHSV& A, const FHSV& B) { return FMath::Abs(A.Value - B.Value) < 1e-4f && FMath::Abs(A.Sat - B.Sat) < 1e-4f && FMath::Abs(A.Hue - B.Hue) < 1e-2f; };
	for (int32 P = 1; P <= 4; ++P)
	{
		FHSV Brightest, Darkest{0.f, 0.f, 2.f};
		for (const FColor& C : Palette(P))
		{
			const FHSV H = ToHSV(C);
			if (H.Value > Brightest.Value) Brightest = H;
			if (H.Value < Darkest.Value) Darkest = H;
		}
		TestTrue(*FString::Printf(TEXT("Palette %d: brightest at the start of sunset"), P), Same(RampColor(P, 0.f, false), Brightest));
		TestTrue(*FString::Printf(TEXT("Palette %d: darkest at the end of sunset"), P), Same(RampColor(P, 1.f, false), Darkest));
		TestTrue(*FString::Printf(TEXT("Palette %d: brightest at the end of sunrise"), P), Same(RampColor(P, 1.f, true), Brightest));
		TestTrue(*FString::Printf(TEXT("Palette %d: darkest at the start of sunrise"), P), Same(RampColor(P, 0.f, true), Darkest));
		TArray<FColor> Reversed(Palette(P).GetData(), Palette(P).Num());
		Algo::Reverse(Reversed);
		bool bSame = true;
		for (const float At : {0.f, .1f, .3f, .5f, .7f, .9f, 1.f})
		{
			bSame &= Same(Ramp(Reversed, At, true), Ramp(Palette(P), At, true)) && Same(Ramp(Reversed, At, false), Ramp(Palette(P), At, false));
		}
		TestTrue(*FString::Printf(TEXT("Palette %d: reversed gives the same ramp"), P), bSame);
	}

	// The short way round the hue wheel.
	TestEqual(TEXT("350 to 10, half way: 0"), FMath::Fmod(LerpHue(350.f, 10.f, .5f), 360.f), 0.f, .01f);
	TestEqual(TEXT("10 to 350, half way: 0"), FMath::Fmod(LerpHue(10.f, 350.f, .5f), 360.f), 0.f, .01f);
	TestEqual(TEXT("20 to 80, half way: 50"), LerpHue(20.f, 80.f, .5f), 50.f, .01f);
	{
		// Palette 1: peach (17 degrees) then coral (354 degrees) at the start of sunset; between them the hue stays red.
		const float Hue = RampColor(1, .125f, false).Hue;
		TestTrue(*FString::Printf(TEXT("Peach to coral goes through red (%.1f)"), Hue), Hue <= 17.5f || Hue >= 353.5f);
	}

	// Recolor: brightness kept at factor 1; nothing at amount 0; factor and target value.
	auto Display = [](const FLinearColor& C) { return ToHSV(C.ToFColorSRGB()); };
	const FLinearColor Sunset = FLinearColor::FromSRGBColor(FColor(196, 103, 56));
	FLinearColor Sunrise = FLinearColor::FromSRGBColor(FColor(148, 98, 82));
	Sunrise.A = .6f;
	for (const float Amount : {.3f, 1.f})
	{
		const FLinearColor R = Recolor(Sunset, 270.f, .6f, 1.f, Amount);
		TestEqual(*FString::Printf(TEXT("Brightness kept at amount %.1f"), Amount), Display(R).Value, Display(Sunset).Value, 1.5f / 255.f);
	}
	const FHSV Full = Display(Recolor(Sunset, 270.f, .6f, 1.f, 1.f));
	TestEqual(TEXT("Amount 1: target hue"), Full.Hue, 270.f, 1.5f);
	TestEqual(TEXT("Amount 1: target saturation"), Full.Sat, .6f, .01f);
	TestTrue(TEXT("Amount 0: unchanged"), Recolor(Sunset, 270.f, .6f, 1.5f, 0.f) == Sunset);
	TestEqual(TEXT("Alpha kept"), Recolor(Sunrise, 270.f, .6f, 1.f, 1.f).A, .6f);
	TestEqual(TEXT("Brightness factor 0.5"), Display(Recolor(Sunset, 270.f, .6f, .5f, 1.f)).Value, Display(Sunset).Value * .5f, 1.5f / 255.f);
	TestEqual(TEXT("Target value"), Display(Recolor(Sunset, 270.f, .6f, 1.f, 1.f, .2f)).Value, .2f, 1.5f / 255.f);
	TestEqual(TEXT("Black stays black"), Recolor(FLinearColor::Black, 270.f, .6f, 1.f, 1.f).GetLuminance(), 0.f, 1e-6f);

	// Falling rain removes the weight by Storm fade; overcast is not part of the rule.
	TestEqual(TEXT("No rain: full weight"), StormFactor(0.f, 1.f), 1.f);
	TestEqual(TEXT("Rain 1, Storm fade 1: none"), StormFactor(1.f, 1.f), 0.f);
	TestEqual(TEXT("Rain 1, Storm fade 0: full weight"), StormFactor(1.f, 0.f), 1.f);
	TestEqual(TEXT("Rain 0.5, Storm fade 1: half"), StormFactor(.5f, 1.f), .5f);
	TestEqual(TEXT("Rain 1, Storm fade 0.5: half"), StormFactor(1.f, .5f), .5f);
	TestEqual(TEXT("Clamped to 0 to 1"), StormFactor(2.f, 2.f), 0.f);

	// Set B: phases placed by time through both windows, in the chosen column.
	auto Is = [&](const FHSV& H, const TCHAR* Hex) { return Same(H, ToHSV(FColor::FromHex(Hex))); };
	TestTrue(TEXT("Table 1 sunset 0: golden hour"), Is(TableRamp(1, 0, 0.f, false), TEXT("F4A261")));
	TestTrue(TEXT("Table 1 sunset 0.4: civil twilight"), Is(TableRamp(1, 0, .4f, false), TEXT("D97E63")));
	TestTrue(TEXT("Table 1 sunset 0.75: nautical twilight"), Is(TableRamp(1, 0, .75f, false), TEXT("8C4855")));
	TestTrue(TEXT("Table 1 sunset 1: night"), Is(TableRamp(1, 0, 1.f, false), TEXT("1B263B")));
	TestTrue(TEXT("Table 1 sunrise 0: night"), Is(TableRamp(1, 0, 0.f, true), TEXT("1B263B")));
	TestTrue(TEXT("Table 1 sunrise 0.4: blue hour"), Is(TableRamp(1, 0, .4f, true), TEXT("4E5978")));
	TestTrue(TEXT("Table 1 sunrise 1: dawn"), Is(TableRamp(1, 0, 1.f, true), TEXT("FAD6B8")));
	TestTrue(TEXT("Table 2 sunset 0: dusk"), Is(TableRamp(2, 0, 0.f, false), TEXT("8D99AE")));
	TestTrue(TEXT("Table 2 sunset 0.5: twilight"), Is(TableRamp(2, 0, .5f, false), TEXT("6B7080")));
	TestTrue(TEXT("Table 3 sunset 1: night"), Is(TableRamp(3, 0, 1.f, false), TEXT("382633")));
	TestTrue(TEXT("Table 3 sunrise 0: night"), Is(TableRamp(3, 0, 0.f, true), TEXT("382633")));
	TestTrue(TEXT("Table 3 sunrise 1: dawn"), Is(TableRamp(3, 0, 1.f, true), TEXT("E9B661")));
	{
		const FHSV Mid = TableRamp(1, 0, .2f, false);
		const FHSV A = ToHSV(FColor::FromHex(TEXT("F4A261"))), B = ToHSV(FColor::FromHex(TEXT("D97E63")));
		TestTrue(TEXT("Between two phases the value is between theirs"), Mid.Value <= FMath::Max(A.Value, B.Value) + 1e-4f && Mid.Value >= FMath::Min(A.Value, B.Value) - 1e-4f);
	}
	TestTrue(TEXT("Column mid-sky"), Is(TableRamp(1, 1, 0.f, false), TEXT("E76F51")));
	TestTrue(TEXT("Column zenith"), Is(TableRamp(1, 2, 0.f, false), TEXT("2A4D69")));
	TestTrue(TEXT("Foggy horizon is the distant horizon"), Is(TableRamp(4, 0, 0.f, false), TEXT("D4C5C7")));
	TestTrue(TEXT("Foggy mid-sky is the mid-distance mist"), Is(TableRamp(4, 1, 0.f, false), TEXT("A69BA3")));
	TestTrue(TEXT("Foggy zenith is near ground"), Is(TableRamp(4, 2, 0.f, false), TEXT("7A727A")));

	// Apply: off, Draw 0, preset AC2 and an unknown weather leave the colors as they are.
	FACELegionsSkyRenderer Sky;
	auto Run = [&](float Time, const TCHAR* DayName, float Overcast = 0.f, float Rain = 0.f)
	{
		Sky.FogColor = FLinearColor(Sunset.R, Sunset.G, Sunset.B, 1.f);
		Sky.SkyFogColor = FLinearColor(Sunset.R, Sunset.G, Sunset.B, .35f);
		Apply(Sky, Time, Twilight, DayName, 77, Overcast, Rain);
		return Sky.FogColor == FLinearColor(Sunset.R, Sunset.G, Sunset.B, 1.f) && Sky.SkyFogColor == FLinearColor(Sunset.R, Sunset.G, Sunset.B, .35f);
	};
	TestTrue(TEXT("Option off: unchanged"), Run(.90f, TEXT("Clear")));
	TestTrue(TEXT("Current says off"), Current && Current->GetString().StartsWith(TEXT("Off")));
	ACERuntimeOptions::Set(OptionKey, 1.f);
	SetVar(TEXT("personal.FogColor.Draw"), 0);
	TestTrue(TEXT("Draw 0: unchanged"), Run(.90f, TEXT("Clear")));
	SetVar(TEXT("personal.FogColor.Draw"), -1);
	TestTrue(TEXT("Unknown weather: unchanged"), Run(.90f, TEXT("Snowy")));
	TestTrue(TEXT("Outside the windows: unchanged"), Run(.50f, TEXT("Clear")));
	TestTrue(TEXT("Rain 1, Storm fade 1: unchanged"), Run(.90f, TEXT("Rainy"), 0.f, 1.f));
	TestTrue(TEXT("Current shows amount 0 in full rain"), Current && Current->GetString().EndsWith(TEXT("; rain 1.00, overcast 0.00; amount 0.00.")));
	SetVar(TEXT("personal.FogColor.Clear.SunsetPalette"), PresetAC2);
	TestTrue(TEXT("Preset AC2: unchanged"), Run(.90f, TEXT("Clear")));
	SetVar(TEXT("personal.FogColor.Strength"), 0);
	SetVar(TEXT("personal.FogColor.Clear.SunsetPalette"), 1);
	TestTrue(TEXT("Strength 0: unchanged"), Run(.90f, TEXT("Clear")));
	SetVar(TEXT("personal.FogColor.Strength"), 1);

	// On: Clear at 0.90 (middle of sunset, palette 1): the middle stop's hue and saturation, the AC2 brightness.
	TestFalse(TEXT("Clear sunset: changed"), Run(.90f, TEXT("Clear")));
	{
		const FHSV Stop = RampColor(1, .5f, false);
		const FHSV Got = Display(Sky.FogColor);
		TestTrue(TEXT("The middle stop is mauve"), Is(Stop, TEXT("c06c84")));
		TestEqual(TEXT("Fog hue of the ramp"), Got.Hue, Stop.Hue, 1.5f);
		TestEqual(TEXT("Fog saturation of the ramp"), Got.Sat, Stop.Sat, .01f);
		TestEqual(TEXT("Fog brightness of AC2"), Got.Value, Display(Sunset).Value, 1.5f / 255.f);
		TestEqual(TEXT("FogColor alpha 1"), Sky.FogColor.A, 1.f);
		TestEqual(TEXT("SkyFogColor alpha kept"), Sky.SkyFogColor.A, .35f);
		TestEqual(TEXT("Sky fog recolored the same way"), Display(Sky.SkyFogColor).Hue, Got.Hue, 1.5f);
		TestTrue(TEXT("Current names the weather and palette"), Current && Current->GetString().Contains(TEXT("Clear (server)")) && Current->GetString().Contains(TEXT("sunset dawn and dusk")));
		TestTrue(TEXT("Current shows rain, overcast and the amount"), Current && Current->GetString().EndsWith(TEXT("; rain 0.00, overcast 0.00; amount 1.00.")));

		// Overcast 1 with rain 0 (Cloudy and Rainy days without rain): the full effect.
		const FLinearColor FullFog = Sky.FogColor;
		TestFalse(TEXT("Overcast 1, rain 0: changed"), Run(.90f, TEXT("Clear"), 1.f, 0.f));
		TestTrue(TEXT("Overcast 1, rain 0: the full effect"), Sky.FogColor.Equals(FullFog, 1e-5f));
		TestTrue(TEXT("Current shows overcast 1"), Current && Current->GetString().EndsWith(TEXT("; rain 0.00, overcast 1.00; amount 1.00.")));
		// Rain 1 with Storm fade 0: the full effect.
		SetVar(TEXT("personal.FogColor.StormFade"), 0);
		Run(.90f, TEXT("Clear"), 1.f, 1.f);
		TestTrue(TEXT("Rain 1, Storm fade 0: the full effect"), Sky.FogColor.Equals(FullFog, 1e-5f));
		TestTrue(TEXT("Current shows rain 1"), Current && Current->GetString().EndsWith(TEXT("; rain 1.00, overcast 1.00; amount 1.00.")));
		SetVar(TEXT("personal.FogColor.StormFade"), 1);
		// Rain 0.5 with Storm fade 1: half the amount, so hue and saturation half way from AC2 to the ramp.
		TestFalse(TEXT("Rain 0.5: changed"), Run(.90f, TEXT("Clear"), 0.f, .5f));
		const FHSV Half = Display(Sky.FogColor);
		const FHSV AC2 = Display(Sunset);
		TestEqual(TEXT("Rain 0.5: half the hue change"), Half.Hue, LerpHue(AC2.Hue, Got.Hue, .5f), 1.5f);
		TestEqual(TEXT("Rain 0.5: half the saturation change"), Half.Sat, FMath::Lerp(AC2.Sat, Got.Sat, .5f), .01f);
		TestTrue(TEXT("Current shows the half amount"), Current && Current->GetString().EndsWith(TEXT("; rain 0.50, overcast 0.00; amount 0.50.")));
	}
	// Saturation scales the palette saturation.
	SetVar(TEXT("personal.FogColor.Saturation"), .5f);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Saturation 0.5"), Display(Sky.FogColor).Sat, RampColor(1, .5f, false).Sat * .5f, .01f);
	SetVar(TEXT("personal.FogColor.Saturation"), 1);
	// Forced weather: Sunny's sunset palette 3 on a Clear day.
	SetVar(TEXT("personal.FogColor.Weather"), Sunny);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Forced Sunny uses palette 3"), Display(Sky.FogColor).Hue, RampColor(3, .5f, false).Hue, 1.5f);
	TestTrue(TEXT("Current says forced"), Current && Current->GetString().Contains(TEXT("Sunny (forced)")));
	SetVar(TEXT("personal.FogColor.Weather"), -1);
	// Custom: the hue and saturation of the variables.
	SetVar(TEXT("personal.FogColor.Clear.SunsetPalette"), PresetCustom);
	SetVar(TEXT("personal.FogColor.Clear.SunsetHue"), 200);
	SetVar(TEXT("personal.FogColor.Clear.SunsetSat"), .3f);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Custom hue"), Display(Sky.FogColor).Hue, 200.f, 1.5f);
	TestEqual(TEXT("Custom saturation"), Display(Sky.FogColor).Sat, .3f, .01f);
	// Brightness factor per weather and window.
	SetVar(TEXT("personal.FogColor.Clear.SunsetBright"), .5f);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Sunset brightness 0.5"), Display(Sky.FogColor).Value, Display(Sunset).Value * .5f, 1.5f / 255.f);
	SetVar(TEXT("personal.FogColor.Clear.SunsetBright"), 1);

	// Set B: the table's ramp; the brightness changes only with TableBrightness.
	SetVar(TEXT("personal.FogColor.Set"), 1);
	Run(.90f, TEXT("Clear"));
	const FHSV TableAt = TableRamp(1, 0, .5f, false);
	TestEqual(TEXT("Table hue"), Display(Sky.FogColor).Hue, TableAt.Hue, 1.5f);
	TestEqual(TEXT("Table: AC2 brightness kept"), Display(Sky.FogColor).Value, Display(Sunset).Value, 1.5f / 255.f);
	TestTrue(TEXT("Current names the table"), Current && Current->GetString().Contains(TEXT("Atmospheric tables, Horizon, Clear sky")));
	SetVar(TEXT("personal.FogColor.TableBrightness"), 1);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Table brightness"), Display(Sky.FogColor).Value, TableAt.Value, 1.5f / 255.f);
	SetVar(TEXT("personal.FogColor.Column"), 2);
	SetVar(TEXT("personal.FogColor.TableBrightness"), 0);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Zenith column hue"), Display(Sky.FogColor).Hue, TableRamp(1, 2, .5f, false).Hue, 1.5f);
	SetVar(TEXT("personal.FogColor.Column"), 0);

	// Separate horizon band. SplitHaze 0: the band is the fog color, whatever HazeColumn is.
	const FHSV Horizon = TableRamp(1, 0, .5f, false), MidSky = TableRamp(1, 1, .5f, false), Zenith = TableRamp(1, 2, .5f, false);
	auto Band = [&]() { return Display(BandColor(Sky.FogColor, true)); };
	SetVar(TEXT("personal.FogColor.HazeColumn"), 2);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("SplitHaze 0: fog takes the horizon hue"), Display(Sky.FogColor).Hue, Horizon.Hue, 1.5f);
	TestTrue(TEXT("SplitHaze 0: the band is the fog color"), BandColor(Sky.FogColor, true) == Sky.FogColor);
	TestFalse(TEXT("SplitHaze 0: Current names no band column"), Current && Current->GetString().Contains(TEXT("band")));
	// SplitHaze 1, table 1 at 0.90: fog and sky fog keep the horizon column, the band takes the mid-sky column.
	SetVar(TEXT("personal.FogColor.SplitHaze"), 1);
	SetVar(TEXT("personal.FogColor.HazeColumn"), 1);
	TestTrue(TEXT("Mid-sky and horizon differ in hue"), FMath::Abs(FMath::Fmod(MidSky.Hue - Horizon.Hue + 540.f, 360.f) - 180.f) > 10.f);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("SplitHaze 1: fog takes the horizon hue"), Display(Sky.FogColor).Hue, Horizon.Hue, 1.5f);
	TestEqual(TEXT("SplitHaze 1: sky fog takes the horizon hue"), Display(Sky.SkyFogColor).Hue, Horizon.Hue, 1.5f);
	TestEqual(TEXT("SplitHaze 1: FogColor alpha 1"), Sky.FogColor.A, 1.f);
	TestEqual(TEXT("SplitHaze 1: SkyFogColor alpha kept"), Sky.SkyFogColor.A, .35f);
	TestEqual(TEXT("Band takes the mid-sky hue"), Band().Hue, MidSky.Hue, 1.5f);
	TestEqual(TEXT("Band takes the mid-sky saturation"), Band().Sat, MidSky.Sat, .01f);
	TestEqual(TEXT("Band keeps the AC2 brightness"), Band().Value, Display(Sunset).Value, 1.5f / 255.f);
	TestEqual(TEXT("Band alpha is the fog color's"), BandColor(FLinearColor(.2f, .3f, .4f, .5f), true).A, .5f);
	TestTrue(TEXT("Band without the imported sky: the fog color"), BandColor(Sky.FogColor, false) == Sky.FogColor);
	TestTrue(TEXT("Current names the band column"), Current && Current->GetString().Contains(TEXT("Atmospheric tables, Horizon, Clear sky, band Mid-sky;")));
	// Table brightness: the band takes the mid-sky column's value, the fog the horizon column's.
	SetVar(TEXT("personal.FogColor.TableBrightness"), 1);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Table brightness: fog of the horizon column"), Display(Sky.FogColor).Value, Horizon.Value, 1.5f / 255.f);
	TestEqual(TEXT("Table brightness: band of the mid-sky column"), Band().Value, MidSky.Value, 1.5f / 255.f);
	TestTrue(TEXT("Current names the band column and table brightness"), Current && Current->GetString().Contains(TEXT("Clear sky, band Mid-sky, table brightness;")));
	SetVar(TEXT("personal.FogColor.TableBrightness"), 0);
	// HazeColumn 2: the zenith hue.
	SetVar(TEXT("personal.FogColor.HazeColumn"), 2);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("HazeColumn 2: band takes the zenith hue"), Band().Hue, Zenith.Hue, 1.5f);
	TestEqual(TEXT("HazeColumn 2: fog keeps the horizon hue"), Display(Sky.FogColor).Hue, Horizon.Hue, 1.5f);
	TestTrue(TEXT("Current names band Zenith"), Current && Current->GetString().Contains(TEXT("band Zenith")));
	// Table 4 (Cloudy) runs the other way: band column zenith is near ground, the fog's horizon is the distant horizon.
	// At 0.90 (sunset 0.5) both are the twilight row: near ground 4A5260, distant horizon 98A2B3.
	Run(.90f, TEXT("Cloudy"));
	TestEqual(TEXT("Table 4: band zenith is near ground"), Band().Sat, ToHSV(FColor::FromHex(TEXT("4A5260"))).Sat, .01f);
	TestEqual(TEXT("Table 4: fog horizon is the distant horizon"), Display(Sky.FogColor).Sat, ToHSV(FColor::FromHex(TEXT("98A2B3"))).Sat, .01f);
	SetVar(TEXT("personal.FogColor.HazeColumn"), 0);
	Run(.90f, TEXT("Cloudy"));
	TestEqual(TEXT("Table 4: band horizon is the distant horizon"), Band().Sat, ToHSV(FColor::FromHex(TEXT("98A2B3"))).Sat, .01f);
	// Out-of-range values are clamped: HazeColumn 9 is the zenith, SplitHaze 5 is on.
	SetVar(TEXT("personal.FogColor.HazeColumn"), 9);
	SetVar(TEXT("personal.FogColor.SplitHaze"), 5);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Clamped: band takes the zenith hue"), Band().Hue, Zenith.Hue, 1.5f);
	SetVar(TEXT("personal.FogColor.HazeColumn"), 1);
	SetVar(TEXT("personal.FogColor.SplitHaze"), 1);
	// Draw 0, the option off and an unknown weather: the band is the fog color.
	SetVar(TEXT("personal.FogColor.Draw"), 0);
	Run(.90f, TEXT("Clear"));
	TestTrue(TEXT("Draw 0: the band is the fog color"), BandColor(Sky.FogColor, true) == Sky.FogColor);
	SetVar(TEXT("personal.FogColor.Draw"), -1);
	ACERuntimeOptions::Set(OptionKey, 0.f);
	Run(.90f, TEXT("Clear"));
	TestTrue(TEXT("Option off: the band is the fog color"), BandColor(Sky.FogColor, true) == Sky.FogColor);
	ACERuntimeOptions::Set(OptionKey, 1.f);
	Run(.90f, TEXT("Snowy"));
	TestTrue(TEXT("Unknown weather: the band is the fog color"), BandColor(Sky.FogColor, true) == Sky.FogColor);
	// Set A ignores SplitHaze: the fog takes the palette's hue (Clear sunset: Custom, 200 degrees), the band follows it.
	SetVar(TEXT("personal.FogColor.Set"), 0);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("Set A: fog takes the palette hue"), Display(Sky.FogColor).Hue, 200.f, 1.5f);
	TestTrue(TEXT("Set A: the band is the fog color"), BandColor(Sky.FogColor, true) == Sky.FogColor);
	TestFalse(TEXT("Set A: Current names no band column"), Current && Current->GetString().Contains(TEXT("band")));

	// The Fog Color Saturation option: right after Fog Colors by Weather, 0.35, 0 to 1.5.
	{
		int32 FogAt = INDEX_NONE, SatAt = INDEX_NONE;
		for (int32 I = 0; I < UE_ARRAY_COUNT(ACERuntimeOptions::Values); ++I)
		{
			if (FCString::Strcmp(ACERuntimeOptions::Values[I].Key, OptionKey) == 0) FogAt = I;
			if (FCString::Strcmp(ACERuntimeOptions::Values[I].Key, SaturationOptionKey) == 0) SatAt = I;
		}
		if (TestTrue(TEXT("Option row PersonalFogColorSaturation"), SatAt != INDEX_NONE))
		{
			const ACERuntimeOptions::FOption& O = ACERuntimeOptions::Values[SatAt];
			TestEqual(TEXT("Saturation label"), FString(O.Label), FString(TEXT("Fog Color Saturation")));
			TestEqual(TEXT("Saturation default 0.35"), O.Default, .35f);
			TestEqual(TEXT("Saturation min 0"), O.Min, 0.f);
			TestEqual(TEXT("Saturation max 1.5"), O.Max, 1.5f);
			TestEqual(TEXT("Saturation row right after Fog Colors by Weather"), SatAt, FogAt + 1);
		}
	}
	// Saturation -1 follows the option; 0 or more overrides it.
	SetVar(TEXT("personal.FogColor.Set"), 1);
	SetVar(TEXT("personal.FogColor.Saturation"), -1);
	bool bOverride = true;
	TestEqual(TEXT("Nothing saved: the option's 0.35"), SaturationInUse(&bOverride), .35f, 1e-6f);
	TestFalse(TEXT("Following the option"), bOverride);
	ACERuntimeOptions::Set(SaturationOptionKey, .8f);
	TestEqual(TEXT("Option 0.8"), SaturationInUse(), .8f, 1e-6f);
	Run(.90f, TEXT("Clear"));
	TestTrue(TEXT("Current: saturation from the option"), Current && Current->GetString().Contains(TEXT("; saturation 0.80 (option).")));
	TestEqual(TEXT("Option 0.8 scales the table saturation"), Display(Sky.FogColor).Sat, TableRamp(1, 0, .5f, false).Sat * .8f, .01f);
	SetVar(TEXT("personal.FogColor.Saturation"), .45f);
	TestEqual(TEXT("Override 0.45"), SaturationInUse(&bOverride), .45f, 1e-6f);
	TestTrue(TEXT("Overridden"), bOverride);
	Run(.90f, TEXT("Clear"));
	TestTrue(TEXT("Current: saturation from the dashboard"), Current && Current->GetString().Contains(TEXT("; saturation 0.45 (dashboard).")));
	SetVar(TEXT("personal.FogColor.Saturation"), 0);
	TestEqual(TEXT("Override 0 is an override"), SaturationInUse(&bOverride), 0.f);
	TestTrue(TEXT("Override 0 overrides"), bOverride);
	ACERuntimeOptions::Set(SaturationOptionKey, .35f);
	SetVar(TEXT("personal.FogColor.Saturation"), 1);

	// Stage 2: the day's offsets.
	{
		const FDayOffsets A = DayOffsets(1234), B = DayOffsets(1234), C = DayOffsets(1235);
		auto Six = [](const FDayOffsets& O) { return TArray<float>{O.Sunrise.Hue, O.Sunrise.Sat, O.Sunrise.Bright, O.Sunset.Hue, O.Sunset.Sat, O.Sunset.Bright}; };
		TestTrue(TEXT("The same day gives the same offsets"), Six(A) == Six(B));
		int32 Different = 0;
		for (int32 I = 0; I < 6; ++I)
		{
			Different += FMath::Abs(Six(A)[I] - Six(C)[I]) > .01f ? 1 : 0;
		}
		TestTrue(TEXT("Neighboring days differ"), Different >= 4);
		// For the report: the six offsets of the test's day 77 and the three days on each side.
		for (int32 Day = 74; Day <= 80; ++Day)
		{
			const TArray<float> O = Six(DayOffsets(Day));
			AddInfo(FString::Printf(TEXT("DayOffsets %d: sunrise %+.3f %+.3f %+.3f, sunset %+.3f %+.3f %+.3f"), Day, O[0], O[1], O[2], O[3], O[4], O[5]));
		}
		double Sum[6] = {};
		bool bInRange = true, bNegative[6] = {}, bPositive[6] = {};
		for (int32 Day = 0; Day < 1000; ++Day)
		{
			const TArray<float> O = Six(DayOffsets(Day));
			for (int32 I = 0; I < 6; ++I)
			{
				bInRange &= O[I] >= -1.f && O[I] <= 1.f;
				Sum[I] += O[I];
				bNegative[I] |= O[I] < 0.f;
				bPositive[I] |= O[I] > 0.f;
			}
		}
		TestTrue(TEXT("Offsets within -1 to 1 over 1,000 days"), bInRange);
		for (int32 I = 0; I < 6; ++I)
		{
			TestTrue(*FString::Printf(TEXT("Offset %d: mean %.3f within 0.1"), I + 1, Sum[I] / 1000.), FMath::Abs(Sum[I] / 1000.) <= .1);
			TestTrue(*FString::Printf(TEXT("Offset %d: both signs"), I + 1), bNegative[I] && bPositive[I]);
		}
	}
	// Variation 0 leaves the target and brightness exactly as they are; at 1 the limits hold and the hue wraps.
	{
		const FHSV T{200.f, .5f, .4f};
		const FDayOffset O{.7f, -.6f, .9f};
		const FHSV Same0 = VaryTarget(T, O, 0.f);
		TestTrue(TEXT("Variation 0: target unchanged"), Same0.Hue == T.Hue && Same0.Sat == T.Sat && Same0.Value == T.Value);
		TestEqual(TEXT("Variation 0: brightness unchanged"), VaryBright(.9f, O, 0.f), .9f);
		TestEqual(TEXT("Hue wraps past 360"), VaryTarget({355.f, .5f, 0.f}, {1.f, 0.f, 0.f}, 1.f).Hue, 15.f, 1e-3f);
		TestEqual(TEXT("Hue wraps below 0"), VaryTarget({5.f, .5f, 0.f}, {-1.f, 0.f, 0.f}, 1.f).Hue, 345.f, 1e-3f);
		TestEqual(TEXT("Brightness clamped to 1.5"), VaryBright(1.45f, {0.f, 0.f, 1.f}, 1.f), 1.5f);
		TestEqual(TEXT("Brightness clamped to 0.5"), VaryBright(.55f, {0.f, 0.f, -1.f}, 1.f), .5f);
		TestEqual(TEXT("Brightness +0.05 at offset 0.5"), VaryBright(.9f, {0.f, 0.f, .5f}, 1.f), .95f, 1e-5f);
		bool bHue = true, bSat = true;
		for (int32 Day = 0; Day < 1000; ++Day)
		{
			const FDayOffsets D = DayOffsets(Day);
			for (const FDayOffset& W : {D.Sunrise, D.Sunset})
			{
				const FHSV V = VaryTarget({350.f, .5f, 0.f}, W, 1.f);
				bHue &= FMath::Abs(FMath::Fmod(V.Hue - 350.f + 540.f, 360.f) - 180.f) <= MaxHueShift + 1e-3f && V.Hue >= 0.f && V.Hue < 360.f;
				bSat &= V.Sat / .5f >= 1.f - MaxSatScale - 1e-5f && V.Sat / .5f <= 1.f + MaxSatScale + 1e-5f;
			}
		}
		TestTrue(TEXT("Variation 1: hue moves at most 20 degrees"), bHue);
		TestTrue(TEXT("Variation 1: saturation factor 0.7 to 1.3"), bSat);
	}
	// Apply: Variation 0 does not depend on the day; Variation 1 uses the day plus DayOffset, for fog and band alike.
	SetVar(TEXT("personal.FogColor.SplitHaze"), 1);
	SetVar(TEXT("personal.FogColor.HazeColumn"), 1);
	Run(.90f, TEXT("Clear"));
	const FLinearColor FogDay0 = Sky.FogColor, BandDay0 = BandColor(Sky.FogColor, true);
	SetVar(TEXT("personal.FogColor.DayOffset"), 3);
	Run(.90f, TEXT("Clear"));
	TestTrue(TEXT("Variation 0: the fog is the same on another day"), Sky.FogColor == FogDay0);
	TestTrue(TEXT("Variation 0: the band is the same on another day"), BandColor(Sky.FogColor, true) == BandDay0);
	TestTrue(TEXT("Current: day 80 (+3)"), Current && Current->GetString().Contains(TEXT("day 80 (+3).")));
	TestTrue(TEXT("Current: variation 0"), Current && Current->GetString().Contains(TEXT("Variation 0.00: sunset hue ")));
	SetVar(TEXT("personal.FogColor.Variation"), 1);
	Run(.90f, TEXT("Clear"));
	{
		const FDayOffset O = DayOffsets(80).Sunset;
		const FHSV Fog = VaryTarget(TableRamp(1, 0, .5f, false), O, 1.f);
		const FHSV BandTarget = VaryTarget(TableRamp(1, 1, .5f, false), O, 1.f);
		TestEqual(TEXT("Variation 1: fog hue of day 80"), Display(Sky.FogColor).Hue, Fog.Hue, 1.5f);
		TestEqual(TEXT("Variation 1: fog saturation of day 80"), Display(Sky.FogColor).Sat, FMath::Clamp(Fog.Sat, 0.f, 1.f), .01f);
		TestEqual(TEXT("Variation 1: fog brightness of day 80"), Display(Sky.FogColor).Value, Display(Sunset).Value * VaryBright(1.f, O, 1.f), 2.f / 255.f);
		TestEqual(TEXT("Variation 1: band hue of day 80"), Display(BandColor(Sky.FogColor, true)).Hue, BandTarget.Hue, 1.5f);
		const FString Expected = FString::Printf(TEXT("Variation 1.00: sunset hue %+.0f°, sat %+.0f%%, bright %+.2f."),
			O.Hue * MaxHueShift, O.Sat * MaxSatScale * 100.f, O.Bright * MaxBrightShift);
		TestTrue(*(TEXT("Current names the offsets: ") + Expected), Current && Current->GetString().Contains(Expected));
	}
	SetVar(TEXT("personal.FogColor.DayOffset"), 0);
	Run(.90f, TEXT("Clear"));
	TestEqual(TEXT("DayOffset 0: day 77's offsets"), Display(Sky.FogColor).Hue, VaryTarget(TableRamp(1, 0, .5f, false), DayOffsets(77).Sunset, 1.f).Hue, 1.5f);
	TestTrue(TEXT("Current: day 77 (+0)"), Current && Current->GetString().Contains(TEXT("day 77 (+0).")));
	Run(.50f, TEXT("Clear"));
	TestTrue(TEXT("Current: outside the windows"), Current && Current->GetString().Contains(TEXT("Variation 1.00: outside the windows.")));

	// The slider label: whole numbers as before; with decimals and no unit for the saturation.
	TestEqual(TEXT("Label, 0 decimals"), UPersonalSliderLabel::Format(6.2f, TEXT("landblocks")), FString(TEXT("6 landblocks")));
	TestEqual(TEXT("Label, 2 decimals"), UPersonalSliderLabel::Format(.3f, TEXT(""), 2), FString(TEXT("0.30")));
	TestEqual(TEXT("Label, 2 decimals with a unit"), UPersonalSliderLabel::Format(1.234f, TEXT("x"), 2), FString(TEXT("1.23 x")));
	return true;
}
#endif
