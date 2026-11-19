#pragma once

#include "CoreMinimal.h"

/** Personal far scenery: the rules for which landblocks and objects are drawn. No state; lengths in any one unit. */
namespace PersonalFarScenery
{
	/** The client draws its own scenery within this many landblocks of the player's. */
	inline constexpr int32 NearRing = 2;
	/** The shell keeps the selected scenery rings at native terrain resolution. */
	inline constexpr int32 LastRing = 24;
	/** Interior room meshes and window panels are only useful in the nearer rings. */
	inline constexpr int32 InteriorRings = 8;
	inline constexpr int32 WorldLandblocks = 255;
	/** CPD 0/1 are already used for depth bias/selection by the ordinary object graph. */
	inline constexpr int32 SizeFadePrimitiveDataIndex = 2;
	inline constexpr float FadeStartFraction = 0.8f;

	/** Prepare an invisible outer ring before the player crosses a landblock boundary. */
	inline int32 PlacementRing(int32 VisibleRing)
	{
		return FMath::Clamp(VisibleRing + 1, NearRing, LastRing);
	}

	/** Keep prefetched scenery on native heights while a recentered shell is still building. */
	inline int32 NativeTerrainRing(int32 VisibleRing, int32 TerrainRing)
	{
		return FMath::Min(PlacementRing(VisibleRing), FMath::Max(TerrainRing, 0));
	}

	/** A prefetched ring covers one-block recenter latency; a cell covers the camera boom. */
	inline float FadeRadius(int32 OuterRing, float BlockSize)
	{
		const float Rings = FMath::Max(float(OuterRing < LastRing ? OuterRing : LastRing - 1) - .125f, 0.f);
		return Rings * FMath::Max(BlockSize, 0.f);
	}

	/** Coverage is zero before hardware culling can remove any still-visible surface. */
	inline float CullDistance(float FadeEnd, float BoundsRadius)
	{
		return FadeEnd + FMath::Max(BoundsRadius, 1.f);
	}

	/** Matches the material's smooth distance transition; zero end disables that limit. */
	inline float FadeAmount(float Distance, float End)
	{
		return End > 0.f ? 1.f - FMath::SmoothStep(End * FadeStartFraction, End, Distance) : 1.f;
	}

	/** Check box "Far Terrain Objects". */
	inline constexpr const TCHAR* OptionKey = TEXT("PersonalFarScenery");
	/** Slider "Far Terrain Objects Radius", in landblocks. */
	inline constexpr const TCHAR* RadiusKey = TEXT("PersonalFarSceneryRadius");
	/** Check box "Hide Far Terrain with Fog". */
	inline constexpr const TCHAR* FogKey = TEXT("PersonalFarSceneryFog");

	/**
	 * Far objects are drawn: the check box and the far terrain are on.
	 * Console variable personal.FarScenery.Draw: -1 the check box, 0 off, 1 on. The far terrain is needed either way.
	 */
	bool IsOn();
	/** Outermost ring with far objects. Console variable personal.FarScenery.Radius: 0 the slider, else that ring. */
	int32 Radius();
	/** No far objects past the fog end. Console variable personal.FarScenery.FogRule: -1 the check box, 0 no, 1 yes. */
	bool HideWithFog();

	/** Console variable personal.FarScenery.SizeFactor: draw distance per unit of object height. */
	float SizeFactor();

	/** Landblocks between Block and Center, the larger of the two directions. */
	inline int32 Ring(FIntPoint Block, FIntPoint Center)
	{
		return FMath::Max(FMath::Abs(Block.X - Center.X), FMath::Abs(Block.Y - Center.Y));
	}

	/** How far away an object of this height and scale is still drawn. At factor 400 it is then about 4 pixels tall. */
	inline float DrawDistance(float Height, float Scale, float Factor)
	{
		return Height * Scale * Factor;
	}

	/** The closest a landblock in this ring can be to the player. */
	inline float NearEdge(int32 InRing, float BlockSize)
	{
		return FMath::Max(InRing - 1, 0) * BlockSize;
	}

	/** False for an object type that cannot be seen anywhere in a landblock of this ring: it is not created there. */
	inline bool InReach(float Distance, int32 InRing, float BlockSize)
	{
		return Distance >= NearEdge(InRing, BlockSize);
	}

	/** The outermost ring whose near edge is inside the fog end, at most LastRing. Past the fog end everything is the fog color. */
	inline int32 FogRing(float FogEnd, float BlockSize)
	{
		return FMath::Clamp(1 + FMath::FloorToInt32(FMath::Max(FogEnd, 0.f) / BlockSize), 0, LastRing);
	}

	/** Retain existing blocks one ring beyond the build limit; this does not request new placement. */
	inline int32 RetentionRing(int32 BuildRing)
	{
		return FMath::Clamp(BuildRing + 1, 0, LastRing);
	}

	/**
	 * True for a landblock of the world that has far scenery while the player is in Center.
	 * Ring NearRing is in the set too: prepared and hidden, ready for when the client's own scenery leaves it.
	 */
	inline bool IsWanted(FIntPoint Block, FIntPoint Center, int32 OuterRing)
	{
		const int32 Rings = Ring(Block, Center);
		return Rings >= NearRing && Rings <= FMath::Min(OuterRing, LastRing)
			&& Block.X >= 0 && Block.Y >= 0 && Block.X < WorldLandblocks && Block.Y < WorldLandblocks;
	}

	/** Handoff: the far objects of a landblock are shown unless the client's own scenery is there, complete and shown. */
	inline bool ShowFar(int32 InRing, bool bNearShown)
	{
		return InRing > NearRing || !bNearShown;
	}
}
