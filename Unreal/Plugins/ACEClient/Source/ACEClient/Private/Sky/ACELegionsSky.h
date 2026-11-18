#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"

namespace ACELegionsSky
{
struct FSection
{
	TArray<int32> Indices;
	uint32 Texture = 0;
	bool Additive = false;
	FLinearColor Tint = FLinearColor::White;
	// Native texture-stage operations: RGB gain, use texture alpha, signed add, double alpha.
	FLinearColor StageOps = FLinearColor(1, 0, 0, 0);
	FVector2D Offset[2]{FVector2D::ZeroVector, FVector2D::ZeroVector};
	FVector2D Velocity[2]{FVector2D::ZeroVector, FVector2D::ZeroVector};
};
struct FLayer
{
	uint32 Name = 0;
	uint32 Setup = 0;
	FVector Center = FVector::ZeroVector;
	float Begin = 0, End = 1, BeginPitch = 0, EndPitch = 0, BeginHeading = 0, EndHeading = 0;
	bool West = false;
	TArray<FVector> Vertices;
	TArray<FVector2D> UVs;
	TArray<FSection> Sections;
};
struct FKey
{
	float Time = 0, Rain = 0;
	float FogMin = 0, FogMax = 0, Direct = 0, Heading = 0, Pitch = 0, Ambient = 0;
	FLinearColor DirectColor, AmbientColor;
	FLinearColor Haze = FLinearColor::Black;
	struct FOverride
	{
		float Pitch, Heading;
		FLinearColor Tint;
	};
	TMap<uint32, FOverride> Objects;
};
struct FImage
{
	int32 Width = 0, Height = 0;
	EPixelFormat Format = PF_B8G8R8A8;
	TArray<TArray<uint8>> Mips;
};
// Wettest key and preceding cloud bank in each rain schedule referenced by SkyDesc.
struct FRainDay
{
	TArray<FKey> Keys;
	int32 Cloudy = 0, Wet = 0;
};
struct FBackdrop
{
	uint32 CloudMask = 0;
	FLinearColor CloudMotion = FLinearColor::Transparent;
	TArray<FLayer> Layers;
	TArray<FRainDay> RainDays;
	TArray<FKey> Keys;
	TMap<uint32, FImage> Images;
};
FString CachePath();
FString StagedPath(const FString& Destination);
bool ImportStaged(const FString& Destination, FString& Error);
bool HasCache(bool* NeedsWeather = nullptr);
int32 Revision();
bool Import(const FString& DirectoryOrFile, const FString& Destination, FString& Error);
bool Load(const FString& Filename, FBackdrop& Sky, FString& Error);
}
