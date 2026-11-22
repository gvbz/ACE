#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/IConsoleManager.h"
#include "Engine/GameInstance.h"
#include "ACEDatSubsystem.h"
#include "ACETypes.h"
#include "Dat/ACEDatFileTypes.h"
#include "Personal/PersonalLightDirection.h"
#include "Sky/ACELegionsSkyRenderer.h"

namespace PersonalLightDirectionTest
{
namespace
{
	using FBody = FACELegionsSkyFrame::FBody;

	/**
	 * The large moon and the sun as the measurement found them (report 2026-11-22_LightJump_P1, the same in all 20 day
	 * groups): the client's sun and moon code (ResolveCelestialState, the slots) is protected, so they are built here.
	 * Moon: slot 0.00 to 0.23 (opacity 1 inside), angle -20° to 190°, turning about the horizontal north axis at 75.4°
	 * from it: rises at bearing 75.4°, highest 75.4° up in the north at 0.1205, sets at 284.6°.
	 * Sun: slot 0.16 to 0.94, angle -23° to 203° through the east, overhead and the west: rises at 0.2395, sets at 0.8605.
	 * Unreal coordinates: east is -X, north +Y.
	 */
	FBody MoonAt(double T)
	{
		FBody Body;
		if (T > 0. && T < .23)
		{
			const double Angle = FMath::DegreesToRadians(-20. + 210. * T / .23);
			const double Tilt = FMath::DegreesToRadians(75.4);
			Body.Direction = FVector(-FMath::Sin(Tilt) * FMath::Cos(Angle), FMath::Cos(Tilt), FMath::Sin(Tilt) * FMath::Sin(Angle));
			Body.Opacity = 1.f;
		}
		return Body;
	}

	FBody SunAt(double T)
	{
		FBody Body;
		if (T > .16 && T < .94)
		{
			const double Angle = FMath::DegreesToRadians(-23. + 226. * (T - .16) / .78);
			Body.Direction = FVector(-FMath::Cos(Angle), 0., FMath::Sin(Angle));
			Body.Opacity = 1.f;
		}
		return Body;
	}

	/** The client's AC1 key direction toward the light: a copy of BracketTimeOfDay and lines 1009 to 1020 of UpdateImportedSky (ACESkyDomeActor.cpp, v163). */
	FVector ClientKeyToward(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction)
	{
		int32 A = INDEX_NONE, B = INDEX_NONE;
		float Alpha = 0.f;
		for (int32 i = 0; i < Keys.Num(); ++i)
		{
			if (Keys[i].Begin <= DayFraction)
			{
				A = i;
			}
		}
		if (A == INDEX_NONE)
		{
			A = Keys.Num() - 1;
			B = 0;
			const float SpanStart = Keys[A].Begin - 1.f;
			const float Span = FMath::Max(KINDA_SMALL_NUMBER, Keys[B].Begin - SpanStart);
			Alpha = FMath::Clamp((DayFraction - SpanStart) / Span, 0.f, 1.f);
		}
		else
		{
			B = (A + 1) % Keys.Num();
			const float SpanEnd = (B == 0) ? Keys[B].Begin + 1.f : Keys[B].Begin;
			const float Span = FMath::Max(KINDA_SMALL_NUMBER, SpanEnd - Keys[A].Begin);
			Alpha = FMath::Clamp((DayFraction - Keys[A].Begin) / Span, 0.f, 1.f);
		}
		const auto& Before = Keys[A];
		const auto& After = Keys[B];
		const float Pitch = FMath::DegreesToRadians(FMath::Lerp(Before.DirPitch, After.DirPitch, Alpha));
		const float Heading = FMath::DegreesToRadians(FMath::Lerp(Before.DirHeading, After.DirHeading, Alpha));
		const FVector LightDirection = -FACEPosition::AceVectorToUnreal(FVector(FMath::Cos(Pitch) * FMath::Sin(Heading),
			FMath::Cos(Pitch) * FMath::Cos(Heading), FMath::Sin(Pitch)));
		return -LightDirection;
	}

	FACELegionsSkyFrame FrameAt(const TArray<FACEDatSkyTimeOfDay>& Keys, float T)
	{
		FACELegionsSkyFrame Frame;
		Frame.Sun = SunAt(T);
		Frame.Moon = MoonAt(T);
		Frame.LightDirection = -ClientKeyToward(Keys, T);
		return Frame;
	}

	/** The client's light toward: the source rule of FACELegionsSkyRenderer::Update (ACELegionsSkyRenderer.cpp lines 247 and 363 to 368, v163). */
	FVector ClientToward(const FACELegionsSkyFrame& Frame)
	{
		FVector Direction = Frame.LightDirection;
		for (const FBody* Body : {&Frame.Sun, &Frame.Moon})
		{
			if (Body->Opacity > 0.f && Body->Direction.Z > 0.)
			{
				Direction = -Body->Direction;
				break;
			}
		}
		return -Direction;
	}

	/** The light toward after Apply, starting from the client's direction as Update leaves it. */
	FVector AppliedToward(const TArray<FACEDatSkyTimeOfDay>& Keys, float T)
	{
		const FACELegionsSkyFrame Frame = FrameAt(Keys, T);
		FACELegionsSkyRenderer Sky;
		Sky.Current.Direction = -ClientToward(Frame);
		PersonalLightDirection::Apply(Sky, Frame, Keys, T);
		return -Sky.Current.Direction;
	}

	double AngleDeg(const FVector& A, const FVector& B)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(A.GetSafeNormal() | B.GetSafeNormal(), -1., 1.)));
	}

	float ElevationOf(const FVector& Toward)
	{
		float Elevation, Bearing;
		PersonalLightDirection::ElevationBearing(Toward, Elevation, Bearing);
		return Elevation;
	}

	float BearingOf(const FVector& Toward)
	{
		float Elevation, Bearing;
		PersonalLightDirection::ElevationBearing(Toward, Elevation, Bearing);
		return Bearing;
	}

	/** Toward a point at the given elevation and bearing (degrees). */
	FVector Toward(double ElevationDeg, double BearingDeg)
	{
		const double E = FMath::DegreesToRadians(ElevationDeg), B = FMath::DegreesToRadians(BearingDeg);
		return FVector(-FMath::Cos(E) * FMath::Sin(B), FMath::Cos(E) * FMath::Cos(B), FMath::Sin(E));
	}

	constexpr int32 Steps = 2000; // every 0.0005 of a day
}
}

// Smooth light direction with the AC2 sky: console variables, weights, turns, the night turn, and every 0.0005 of a day
// in every day group of the DAT files.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalLightDirectionTest, "ACE.Personal.LightDirection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalLightDirectionTest::RunTest(const FString& Parameters)
{
	using namespace PersonalLightDirectionTest;
	using namespace PersonalLightDirection;

	// A settings file of the test's own, never the developer's.
	const FString PreviousIni = GGameUserSettingsIni;
	GGameUserSettingsIni = FPaths::ProjectSavedDir() / TEXT("Automation/LightDirectionPreferences.ini");
	FConfigFile Settings;
	Settings.NoSave = true;
	GConfig->SetFile(GGameUserSettingsIni, &Settings);
	GConfig->EmptySection(TEXT("ACE.Presentation"), GGameUserSettingsIni);
	ON_SCOPE_EXIT { GGameUserSettingsIni = PreviousIni; };

	// The personal.LightDir variables: values and set-by priority are put back at the end.
	struct FSaved { IConsoleVariable* Variable; FString Value; uint32 SetBy; };
	TArray<FSaved> Saved;
	for (const TCHAR* Name : {TEXT("personal.LightDir.Smooth"), TEXT("personal.LightDir.HorizonFade"), TEXT("personal.LightDir.NightTurn"),
		TEXT("personal.LightDir.Current")})
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!TestNotNull(*FString::Printf(TEXT("Variable %s"), Name), Variable))
		{
			return false;
		}
		Saved.Add({Variable, Variable->GetString(), uint32(Variable->GetFlags()) & ECVF_SetByMask});
	}
	ON_SCOPE_EXIT
	{
		for (const FSaved& S : Saved)
		{
			S.Variable->Set(*S.Value, S.Variable->TestFlags(ECVF_ReadOnly) ? ECVF_SetByCode : ECVF_SetByConsole);
			S.Variable->SetFlags(EConsoleVariableFlags((uint32(S.Variable->GetFlags()) & ~uint32(ECVF_SetByMask)) | S.SetBy));
		}
	};
	IConsoleVariable* Smooth = Saved[0].Variable;
	IConsoleVariable* Fade = Saved[1].Variable;
	IConsoleVariable* Night = Saved[2].Variable;
	IConsoleVariable* Current = Saved[3].Variable;
	auto Defaults = [&]()
	{
		for (IConsoleVariable* V : {Smooth, Fade, Night})
		{
			V->Set(*V->GetDefaultValue(), ECVF_SetByConsole);
		}
	};
	Defaults();

	// Console variables: defaults and clamps.
	TestEqual(TEXT("Smooth defaults to 1"), Smooth->GetInt(), 1);
	TestEqual(TEXT("HorizonFade defaults to 20"), Fade->GetFloat(), 20.f);
	TestEqual(TEXT("NightTurn defaults to 0.1"), Night->GetFloat(), .1f, 1e-6f);
	TestTrue(TEXT("Current is read only"), Current->TestFlags(ECVF_ReadOnly));
	TestTrue(TEXT("Smooth is on"), IsOn());
	Fade->Set(TEXT("2"), ECVF_SetByConsole);
	TestEqual(TEXT("HorizonFade clamped to 5"), HorizonFade(), 5.f);
	Fade->Set(TEXT("60"), ECVF_SetByConsole);
	TestEqual(TEXT("HorizonFade clamped to 45"), HorizonFade(), 45.f);
	Night->Set(TEXT("0.001"), ECVF_SetByConsole);
	TestEqual(TEXT("NightTurn clamped to 0.01"), NightTurn(), .01f);
	Night->Set(TEXT("0.5"), ECVF_SetByConsole);
	TestEqual(TEXT("NightTurn clamped to 0.25"), NightTurn(), .25f);
	Defaults();

	// Weight: 0 without opacity, smoothstep from the horizon to HorizonFade, times the opacity.
	{
		FBody Body;
		Body.Direction = Toward(30., 90.);
		TestEqual(TEXT("Weight 0 at opacity 0"), Weight(Body, 20.f), 0.f);
		Body.Opacity = 1.f;
		TestEqual(TEXT("Weight 1 above HorizonFade"), Weight(Body, 20.f), 1.f);
		Body.Direction = Toward(10., 90.);
		TestEqual(TEXT("Weight 0.5 halfway up"), Weight(Body, 20.f), .5f, 1e-4f);
		Body.Opacity = .5f;
		TestEqual(TEXT("Weight times the opacity"), Weight(Body, 20.f), .25f, 1e-4f);
		Body.Opacity = 1.f;
		Body.Direction = Toward(-1., 90.);
		TestEqual(TEXT("Weight 0 below the horizon"), Weight(Body, 20.f), 0.f);
		Body.Direction = Toward(5., 90.);
		TestEqual(TEXT("Weight smoothstep at 5 of 20 degrees"), Weight(Body, 20.f), .15625f, 1e-4f);
	}

	// Turn: shortest arc for directions up to 90° apart; about the vertical axis beyond, never high over the sky.
	{
		const FVector A = Toward(10., 90.), B = Toward(30., 120.);
		const FVector Half = Turn(A, B, .5f);
		TestEqual(TEXT("Shortest arc: halfway is half the angle"), AngleDeg(A, Half), AngleDeg(A, B) * .5, 1e-3);
		TestEqual(TEXT("Shortest arc: on the arc"), AngleDeg(A, Half) + AngleDeg(Half, B), AngleDeg(A, B), 1e-3);
		TestTrue(TEXT("Turn 0 is the start"), Turn(A, B, 0.f).Equals(A, 1e-9));
		TestTrue(TEXT("Turn 1 is the end"), Turn(A, B, 1.f).Equals(B, 1e-9));
		// The moonset case: the moon low in the WNW, the key in the east, 160° apart.
		const FVector Moon = Toward(10., 285.), Key = Toward(11.6, 90.);
		float HighestWide = -90.f;
		for (int32 I = 0; I <= 20; ++I)
		{
			HighestWide = FMath::Max(HighestWide, ElevationOf(Turn(Key, Moon, I / 20.f)));
		}
		TestTrue(TEXT("Wide turn stays between the two heights"), HighestWide <= 11.6f + 1e-3f);
		TestEqual(TEXT("Wide turn halfway: bearing the short way, through the north"), BearingOf(Turn(Key, Moon, .5f)), 7.5f, .01f);
		TestEqual(TEXT("Wide turn halfway: height halfway"), ElevationOf(Turn(Key, Moon, .5f)), 10.8f, .01f);
		const FVector Opposite = Toward(5., 270.);
		TestTrue(TEXT("Opposite directions turn through the horizon"), ElevationOf(Turn(Toward(5., 90.), Opposite, .5f)) < 5.01f);
	}

	// The DAT files: the AC1 keys of every day group.
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (!FPaths::FileExists(DatDirectory / TEXT("client_portal.dat")))
	{
		AddWarning(TEXT("DAT files not found; skipped the checks over the day groups"));
		return !HasAnyErrors();
	}
	UGameInstance* GameInstance = NewObject<UGameInstance>();
	UACEDatSubsystem* Dat = NewObject<UACEDatSubsystem>(GameInstance);
	if (!TestTrue(TEXT("DAT files load"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}
	const FACEDatRegionSky* Sky = Dat->GetRegionSkyInfo();
	if (!TestTrue(TEXT("Sky description loads"), Sky && Sky->bValid && Sky->DayGroups.Num() > 0))
	{
		return false;
	}

	double WorstSmooth = 0., WorstClient = 0.;
	int32 Groups = 0;
	for (int32 G = 0; G < Sky->DayGroups.Num(); ++G)
	{
		const FACEDatSkyDayGroup& Group = Sky->DayGroups[G];
		const TArray<FACEDatSkyTimeOfDay>& Keys = Group.TimesOfDay;
		if (Keys.Num() == 0)
		{
			continue;
		}
		++Groups;
		const FString Name = FString::Printf(TEXT("group %d %s"), G, *Group.DayName);

		// The night turn windows; two of them must not overlap.
		const TArray<FTurnWindow> Windows = TurnWindows(Keys, .1f);
		FString List;
		for (const FTurnWindow& W : Windows)
		{
			List += FString::Printf(TEXT(" %.4f to %.4f (%.0f° to %.0f°)"), W.Start, W.End, W.StartHeading, W.EndHeading);
		}
		AddInfo(FString::Printf(TEXT("%s: %d keys, turn windows:%s"), *Name, Keys.Num(), *List));
		for (int32 I = 0; I < Windows.Num(); ++I)
		{
			for (int32 J = I + 1; J < Windows.Num(); ++J)
			{
				for (const double Shift : {-1., 0., 1.})
				{
					if (Windows[I].Start < Windows[J].End + Shift && Windows[J].Start + Shift < Windows[I].End)
					{
						AddWarning(FString::Printf(TEXT("%s: turn windows %d and %d overlap"), *Name, I, J));
					}
				}
			}
		}
		TestTrue(*FString::Printf(TEXT("%s: a turn window across midnight"), *Name), Windows.ContainsByPredicate([](const FTurnWindow& W)
		{
			return W.Start < 1. && W.End > 1.;
		}));

		// Every 0.0005 of a day: the turn per step, smoothed and the client's.
		double MaxSmooth = 0., MaxClient = 0.;
		double AtSmooth = 0., AtClient = 0.;
		FVector LastSmooth, LastClient;
		bool bExact = true, bApplied = true, bKeyExact = true;
		for (int32 I = 0; I <= Steps; ++I)
		{
			const float T = float(I) / Steps;
			const FACELegionsSkyFrame Frame = FrameAt(Keys, T);
			const FVector Client = ClientToward(Frame);
			const FVector Smoothed = AppliedToward(Keys, T);
			bApplied &= Smoothed == Direction(KeyDirection(Keys, T, .1f), Frame.Moon, Frame.Sun, 20.f);
			// A NightTurn of 0.001 or less spreads nothing: the client's AC1 direction exactly.
			const FVector Key = ClientKeyToward(Keys, T);
			bKeyExact &= KeyDirection(Keys, T, .001f) == Key && KeyDirection(Keys, T, .0005f) == Key;
			if (I > 0)
			{
				const double StepSmooth = AngleDeg(LastSmooth, Smoothed), StepClient = AngleDeg(LastClient, Client);
				if (StepSmooth > MaxSmooth)
				{
					MaxSmooth = StepSmooth;
					AtSmooth = T;
				}
				if (StepClient > MaxClient)
				{
					MaxClient = StepClient;
					AtClient = T;
				}
			}
			LastSmooth = Smoothed;
			LastClient = Client;

			// Smooth 0: Apply leaves the client's direction exactly.
			Smooth->Set(TEXT("0"), ECVF_SetByConsole);
			{
				FACELegionsSkyRenderer Renderer;
				Renderer.Current.Direction = -Client;
				Apply(Renderer, Frame, Keys, T);
				bExact &= Renderer.Current.Direction == -Client;
			}
			Smooth->Set(TEXT("1"), ECVF_SetByConsole);
		}
		AddInfo(FString::Printf(TEXT("%s: largest turn per 0.0005 day %.2f° at %.4f (client %.2f° at %.4f)"), *Name, MaxSmooth, AtSmooth, MaxClient, AtClient));
		TestTrue(*FString::Printf(TEXT("%s: at most 6° per step (%.2f° at %.4f)"), *Name, MaxSmooth, AtSmooth), MaxSmooth <= 6.);
		TestTrue(*FString::Printf(TEXT("%s: Smooth 0 leaves the client's direction exactly"), *Name), bExact);
		TestTrue(*FString::Printf(TEXT("%s: Apply sets minus Direction(...)"), *Name), bApplied);
		TestTrue(*FString::Printf(TEXT("%s: KeyDirection with NightTurn 0.001 or less is the client's AC1 direction"), *Name), bKeyExact);
		WorstSmooth = FMath::Max(WorstSmooth, MaxSmooth);
		WorstClient = FMath::Max(WorstClient, MaxClient);

		// Midnight: halfway through the night turn the light is in the south, as high as the keys give it.
		for (const FTurnWindow& W : Windows)
		{
			if (W.Start < 1. && W.End > 1.)
			{
				const double Middle = (W.Start + W.End) * .5;
				const float T = float(Middle - FMath::FloorToDouble(Middle));
				const FVector Light = AppliedToward(Keys, T);
				const float KeyElevation = ElevationOf(ClientKeyToward(Keys, T));
				AddInfo(FString::Printf(TEXT("%s: halfway through the night turn (%.4f) the light is %.2f° up at bearing %.1f°"), *Name, T, ElevationOf(Light), BearingOf(Light)));
				TestEqual(*FString::Printf(TEXT("%s: night turn through the south"), *Name), BearingOf(Light), 180.f, 5.f);
				TestEqual(*FString::Printf(TEXT("%s: night turn at the keys' height"), *Name), ElevationOf(Light), KeyElevation, .01f);
				TestEqual(*FString::Printf(TEXT("%s: the keys' height at midnight is 0.9°"), *Name), KeyElevation, .9f, .05f);
			}
		}

		// Moonset: from the moon's last moment at full weight to the AC1 key, the light stays below 30° up.
		{
			int32 Full = INDEX_NONE, Key = INDEX_NONE;
			for (int32 I = int32(.12 * Steps); I <= int32(.23 * Steps); ++I)
			{
				const FBody Moon = MoonAt(double(I) / Steps);
				const float W = Weight(Moon, 20.f);
				if (W >= 1.f)
				{
					Full = I;
				}
				else if (W <= 0.f && Full != INDEX_NONE && Key == INDEX_NONE)
				{
					Key = I;
				}
			}
			if (TestTrue(*FString::Printf(TEXT("%s: moonset found"), *Name), Full != INDEX_NONE && Key != INDEX_NONE))
			{
				float Highest = -90.f;
				for (int32 I = Full; I <= Key; ++I)
				{
					Highest = FMath::Max(Highest, ElevationOf(AppliedToward(Keys, float(I) / Steps)));
				}
				AddInfo(FString::Printf(TEXT("%s: moonset %.4f to %.4f, highest light %.1f°"), *Name, double(Full) / Steps, double(Key) / Steps, Highest));
				TestTrue(*FString::Printf(TEXT("%s: moonset light below 30° up (%.1f°)"), *Name, Highest), Highest < 30.f);
				TestTrue(*FString::Printf(TEXT("%s: after moonset the light is the AC1 key"), *Name),
					AngleDeg(AppliedToward(Keys, float(Key) / Steps), ClientKeyToward(Keys, float(Key) / Steps)) < .01);
			}
		}

		// Away from the handoffs the light is the client's.
		for (const float T : {.12f, .5f})
		{
			const double Off = AngleDeg(AppliedToward(Keys, T), ClientToward(FrameAt(Keys, T)));
			TestTrue(*FString::Printf(TEXT("%s: at %.2f the light is the client's (%.4f°)"), *Name, T, Off), Off < .1);
		}
	}
	TestTrue(TEXT("Day groups found"), Groups > 0);
	AddInfo(FString::Printf(TEXT("All %d groups: largest turn per 0.0005 day %.2f° (client %.2f°)"), Groups, WorstSmooth, WorstClient));

	// Current: weights, elevation and bearing; off names the client's direction.
	{
		const TArray<FACEDatSkyTimeOfDay>& Keys = Sky->DayGroups[0].TimesOfDay;
		AppliedToward(Keys, .12f);
		const FString On = Current->GetString();
		AddInfo(TEXT("Current at 0.12: ") + On);
		TestTrue(TEXT("Current names the weights"), On.StartsWith(TEXT("On. Moon 1.00, sun 0.00. Light ")));
		TestTrue(TEXT("Current names the elevation and bearing"), On.Contains(TEXT("° up, bearing ")) && On.EndsWith(TEXT("°.")));
		Smooth->Set(TEXT("0"), ECVF_SetByConsole);
		AppliedToward(Keys, .12f);
		TestTrue(TEXT("Current when off"), Current->GetString().StartsWith(TEXT("Off (the client's direction). Light ")));
		Smooth->Set(TEXT("1"), ECVF_SetByConsole);
	}
	return !HasAnyErrors();
}
#endif
