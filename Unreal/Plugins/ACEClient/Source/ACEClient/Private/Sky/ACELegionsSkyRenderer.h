#pragma once

#include "Sky/ACELegionsSky.h"
#include "Sky/ACESkyMotion.h"
#include "UObject/StrongObjectPtr.h"

class UProceduralMeshComponent;
class UMaterialInterface;

// The art comes from Legions. Visibility, bearings and weather come from
// the active AC1 RegionDesc day group at the server's portal-year time.
struct FACELegionsSkyFrame
{
	struct FBody
	{
		FVector Direction = FVector::ZeroVector;
		float Opacity = 0.f;
		double TanHalfAngle = 0.;
		float OrbitPhase = 0.f;
	};
	FBody Sun, Moon;
	double Seconds = 0.;
	FVector CloudDirection = FVector::ZeroVector;
	FVector LightDirection = FVector::DownVector;
	bool bClearForecast = false;
	float CloudOpacity = 0.f;
	float Overcast = 0.f;
	float Rain = 0.f;
	float WeatherFogMin = 0.f, WeatherFogMax = 0.f, WeatherDirect = 0.f;
	FLinearColor WeatherFogColor = FLinearColor::Black;
};

struct FACELegionsSkyRenderer
{
	static UMaterialInterface* MaterialParent(bool bAdditive);
	bool Build(AActor& Owner, ACELegionsSky::FBackdrop&& Imported);
	FLinearColor Update(float PaletteTime, const FACELegionsSkyFrame& Frame);
	void SetVisible(bool Visible);
	bool HasRainDays() const { return !Sky.RainDays.IsEmpty(); }
	void Clear();
	struct FLighting
	{
		FVector Direction = FVector::DownVector;
		FLinearColor DirectColor = FLinearColor::White, AmbientColor = FLinearColor::White;
		float Direct = 0, Ambient = 0;
		float SunIntensity() const { return FMath::Max(0.f, Direct) * 4.f; }
	};
	FLighting Current, Noon;
	float FogMin = 0, FogMax = 0;
	float ClearDaylight = 0.f;
	// RGB comes from the day fog key; sky overlay opacity is a separate field.
	FLinearColor SkyFogColor = FLinearColor::Black;
	// World extinction uses FogMin/FogMax, never the sky overlay's alpha.
	FLinearColor FogColor = FLinearColor::Black;
	TStrongObjectPtr<UTexture2D> CloudMask;
	FLinearColor CloudMotion = FLinearColor::Transparent;
	// Ground attenuation follows the sheet actually drawn, including its native
	// visibility interval, authored alpha and weather/clear-day fade.
	float CloudShadowOpacity = 0.f;

  private:
	ACELegionsSky::FBackdrop Sky;
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> Meshes;
	FLinearColor Haze = FLinearColor::Transparent;
	TArray<ACESkyMotion::FUVBasis> CloudBases;
	ACESkyMotion::FBodyShape MoonShape;
};
