#include "Personal/PersonalLightDirection.h"
#include "Dat/ACEDatFileTypes.h"
#include "ACETypes.h"
#include "HAL/IConsoleManager.h"

// Helpers in an anonymous namespace inside PersonalLightDirection: in a unity build they cannot clash with,
// or be hidden by, names of other files.
namespace PersonalLightDirection
{
namespace
{
	TAutoConsoleVariable<int32> CVarSmooth(TEXT("personal.LightDir.Smooth"), 1,
		TEXT("1 turns the light smoothly with the imported AC2 sky (sun and moon fade in above the horizon, the midnight turn is spread over NightTurn); 0 the client's direction."));
	TAutoConsoleVariable<float> CVarHorizonFade(TEXT("personal.LightDir.HorizonFade"), 20.f,
		TEXT("5 to 45 degrees: how high the sun or moon must be to give all of the light's direction (0 at the horizon)."));
	TAutoConsoleVariable<float> CVarNightTurn(TEXT("personal.LightDir.NightTurn"), .1f,
		TEXT("0.01 to 0.25 of a day: how long a sharp turn of the AC1 key heading takes (the midnight turn through south)."));
	TAutoConsoleVariable<FString> CVarCurrent(TEXT("personal.LightDir.Current"), TEXT("Not active: needs the imported AC2 sky, outdoors."),
		TEXT("Read only: the moon's and sun's weights and the light's elevation and bearing."), ECVF_ReadOnly);

	void SetCurrent(const FString& Text)
	{
		static FString Last;
		if (Text != Last)
		{
			Last = Text;
			CVarCurrent->Set(*Text, ECVF_SetByCode);
		}
	}

	/** A copy of AACESkyDomeActor::BracketTimeOfDay (ACESkyDomeActor.cpp, v163), which is protected. */
	bool Bracket(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction, int32& OutIdxA, int32& OutIdxB, float& OutAlpha)
	{
		OutIdxA = INDEX_NONE;
		OutIdxB = INDEX_NONE;
		OutAlpha = 0.f;
		if (Keys.Num() == 0)
		{
			return false;
		}
		for (int32 i = 0; i < Keys.Num(); ++i)
		{
			if (Keys[i].Begin <= DayFraction)
			{
				OutIdxA = i;
			}
		}
		if (OutIdxA == INDEX_NONE)
		{
			OutIdxA = Keys.Num() - 1;
			OutIdxB = 0;
			const float SpanStart = Keys[OutIdxA].Begin - 1.f;
			const float Span = FMath::Max(KINDA_SMALL_NUMBER, Keys[OutIdxB].Begin - SpanStart);
			OutAlpha = FMath::Clamp((DayFraction - SpanStart) / Span, 0.f, 1.f);
			return true;
		}
		OutIdxB = (OutIdxA + 1) % Keys.Num();
		const float SpanEnd = (OutIdxB == 0) ? Keys[OutIdxB].Begin + 1.f : Keys[OutIdxB].Begin;
		const float Span = FMath::Max(KINDA_SMALL_NUMBER, SpanEnd - Keys[OutIdxA].Begin);
		OutAlpha = FMath::Clamp((DayFraction - Keys[OutIdxA].Begin) / Span, 0.f, 1.f);
		return true;
	}

	double Fraction(double Time)
	{
		return Time - FMath::FloorToDouble(Time);
	}

	FVector FromElevationYaw(double Elevation, double Yaw)
	{
		const double C = FMath::Cos(Elevation);
		return FVector(C * FMath::Cos(Yaw), C * FMath::Sin(Yaw), FMath::Sin(Elevation));
	}
}

bool IsOn()
{
	return CVarSmooth.GetValueOnGameThread() != 0;
}

float HorizonFade()
{
	return FMath::Clamp(CVarHorizonFade.GetValueOnGameThread(), 5.f, 45.f);
}

float NightTurn()
{
	return FMath::Clamp(CVarNightTurn.GetValueOnGameThread(), .01f, .25f);
}

TArray<FTurnWindow> TurnWindows(const TArray<FACEDatSkyTimeOfDay>& Keys, float TurnLength)
{
	TArray<FTurnWindow> Windows;
	const int32 Count = Keys.Num();
	if (Count < 2)
	{
		return Windows;
	}
	for (int32 I = 0; I < Count; ++I)
	{
		const int32 J = (I + 1) % Count;
		const double SpanStart = Keys[I].Begin;
		const double SpanEnd = J == 0 ? Keys[J].Begin + 1. : Keys[J].Begin;
		const float Change = Keys[J].DirHeading - Keys[I].DirHeading;
		if (FMath::Abs(Change) < SharpTurnDegrees || SpanEnd - SpanStart >= TurnLength - 1e-4)
		{
			continue;
		}
		FTurnWindow W;
		const double Middle = (SpanStart + SpanEnd) * .5;
		W.Start = Middle - TurnLength * .5;
		W.End = Middle + TurnLength * .5;
		float Pitch;
		ClientKeyAngles(Keys, float(Fraction(W.Start)), Pitch, W.StartHeading);
		ClientKeyAngles(Keys, float(Fraction(W.End)), Pitch, W.EndHeading);
		Windows.Add(W);
	}
	return Windows;
}

bool ClientKeyAngles(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction, float& PitchDeg, float& HeadingDeg)
{
	int32 A, B;
	float Alpha;
	if (!Bracket(Keys, DayFraction, A, B, Alpha) || !Keys.IsValidIndex(A) || !Keys.IsValidIndex(B))
	{
		return false;
	}
	PitchDeg = FMath::Lerp(Keys[A].DirPitch, Keys[B].DirPitch, Alpha);
	HeadingDeg = FMath::Lerp(Keys[A].DirHeading, Keys[B].DirHeading, Alpha);
	return true;
}

FVector AnglesToDirection(float PitchDeg, float HeadingDeg)
{
	// As UpdateImportedSky (ACESkyDomeActor.cpp, v163), which stores the opposite as Frame.LightDirection.
	const float Pitch = FMath::DegreesToRadians(PitchDeg);
	const float Heading = FMath::DegreesToRadians(HeadingDeg);
	return FACEPosition::AceVectorToUnreal(FVector(FMath::Cos(Pitch) * FMath::Sin(Heading),
		FMath::Cos(Pitch) * FMath::Cos(Heading), FMath::Sin(Pitch)));
}

FVector KeyDirection(const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction, float TurnLength)
{
	float Pitch, Heading;
	if (!ClientKeyAngles(Keys, DayFraction, Pitch, Heading))
	{
		return FVector::ZeroVector;
	}
	for (const FTurnWindow& W : TurnWindows(Keys, TurnLength))
	{
		for (const double Shift : {0., 1., -1.})
		{
			const double Time = DayFraction + Shift;
			if (Time >= W.Start && Time <= W.End)
			{
				Heading = FMath::Lerp(W.StartHeading, W.EndHeading, float((Time - W.Start) / (W.End - W.Start)));
				return AnglesToDirection(Pitch, Heading);
			}
		}
	}
	return AnglesToDirection(Pitch, Heading);
}

float Weight(const FACELegionsSkyFrame::FBody& Body, float Fade)
{
	const FVector Toward = Body.Direction.GetSafeNormal();
	if (Body.Opacity <= 0.f || Toward.IsNearlyZero())
	{
		return 0.f;
	}
	const float Elevation = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float(Toward.Z), -1.f, 1.f)));
	return FMath::SmoothStep(0.f, FMath::Max(Fade, KINDA_SMALL_NUMBER), Elevation) * FMath::Clamp(Body.Opacity, 0.f, 1.f);
}

FVector Turn(const FVector& From, const FVector& To, float T)
{
	const FVector A = From.GetSafeNormal(), B = To.GetSafeNormal();
	if (T <= 0.f || B.IsNearlyZero())
	{
		return A;
	}
	if (T >= 1.f || A.IsNearlyZero())
	{
		return B;
	}
	const double Angle = FMath::Acos(FMath::Clamp(A | B, -1., 1.));
	if (Angle > FMath::DegreesToRadians(double(VerticalTurnDegrees)))
	{
		// About the vertical axis: elevation and bearing move evenly, the bearing the short way. The light stays
		// between the two heights, so it never passes high over the sky.
		const double ElevationA = FMath::Asin(FMath::Clamp(A.Z, -1., 1.)), ElevationB = FMath::Asin(FMath::Clamp(B.Z, -1., 1.));
		double YawA = FMath::Atan2(A.Y, A.X), YawB = FMath::Atan2(B.Y, B.X);
		if (A.Size2D() < 1e-6) YawA = YawB;
		if (B.Size2D() < 1e-6) YawB = YawA;
		const double Yaw = YawA + FMath::UnwindRadians(YawB - YawA) * T;
		return FromElevationYaw(FMath::Lerp(ElevationA, ElevationB, double(T)), Yaw);
	}
	if (Angle < 1e-6)
	{
		return A;
	}
	const double Sin = FMath::Sin(Angle);
	return ((A * FMath::Sin((1. - T) * Angle) + B * FMath::Sin(T * Angle)) / Sin).GetSafeNormal();
}

FVector Direction(const FVector& KeyDir, const FACELegionsSkyFrame::FBody& Moon, const FACELegionsSkyFrame::FBody& Sun, float Fade)
{
	FVector Toward = KeyDir.GetSafeNormal();
	Toward = Turn(Toward, Moon.Direction, Weight(Moon, Fade));
	return Turn(Toward, Sun.Direction, Weight(Sun, Fade));
}

void ElevationBearing(const FVector& Toward, float& ElevationDeg, float& BearingDeg)
{
	const FVector D = Toward.GetSafeNormal();
	ElevationDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float(D.Z), -1.f, 1.f)));
	// North is Unreal +Y, east is Unreal -X.
	BearingDeg = D.Size2D() < 1e-6 ? 0.f : float(FMath::RadiansToDegrees(FMath::Atan2(-D.X, D.Y)));
	if (BearingDeg < 0.f)
	{
		BearingDeg += 360.f;
	}
	if (BearingDeg >= 359.95f)
	{
		BearingDeg = 0.f;
	}
}

void Apply(FACELegionsSkyRenderer& Sky, const FACELegionsSkyFrame& Frame, const TArray<FACEDatSkyTimeOfDay>& Keys, float DayFraction)
{
	const float Fade = HorizonFade();
	const float MoonWeight = Weight(Frame.Moon, Fade), SunWeight = Weight(Frame.Sun, Fade);
	float Elevation, Bearing;
	if (!IsOn())
	{
		ElevationBearing(-Sky.Current.Direction, Elevation, Bearing);
		SetCurrent(FString::Printf(TEXT("Off (the client's direction). Light %.1f° up, bearing %.0f°."), Elevation, Bearing));
		return;
	}
	FVector Key = KeyDirection(Keys, DayFraction, NightTurn());
	if (Key.IsNearlyZero())
	{
		Key = -Frame.LightDirection;
	}
	const FVector Toward = Direction(Key, Frame.Moon, Frame.Sun, Fade);
	Sky.Current.Direction = -Toward;
	ElevationBearing(Toward, Elevation, Bearing);
	SetCurrent(FString::Printf(TEXT("On. Moon %.2f, sun %.2f. Light %.1f° up, bearing %.0f°."), MoonWeight, SunWeight, Elevation, Bearing));
}
}
