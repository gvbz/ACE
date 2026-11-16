#pragma once

#include "CoreMinimal.h"
#include "Personal/PersonalFarScenery.h"

/** Personal far windows: the kinds of opening of a building, and the rules for the door panels. No state. */
namespace PersonalFarWindows
{
	/** Openings larger than this, in square meters, are of their own kind (a far room either way). */
	inline constexpr float MaxArea = 12.f;

	/** What an opening of a building is taken for. The order is the order of a panel mesh's sections and materials. */
	enum class EKind : uint8
	{
		Window,
		Door,
		/** An open entrance: a shop front, a tent entrance, a tall opening that starts higher up. */
		Entrance,
		/** Lying flat: a floor or roof opening. */
		Flat,
		/** Over 12 m2. */
		None
	};
	/** The kinds a panel mesh has sections for; an opening over 12 m2 is drawn in the open-entrance section. */
	inline constexpr int32 KindCount = 4;

	/** An opening in its building's own space, in meters. */
	struct FOpening
	{
		float Area = 0.f;
		/** Largest distance between two corners, seen from above. */
		float Width = 0.f;
		float Height = 0.f;
		/** Height of its lowest point. */
		float Bottom = 0.f;
		bool bFlat = false;
	};

	/**
	 * A door is an opening in a wall of a door's size, at ground level or on an upper floor. From the door tests at
	 * Shoushi and Yaraq (2026-11-15): nearly all doors are 2.5 m tall, Yaraq's arched doors 2.8 m; single doors 1.5 to 2.3 m
	 * wide, double doors 3.0 and 4.0 m. A door starts within MaxStart of the building's lowest opening or at least
	 * MinUpperStart above it; an opening between (0.5 to 2.5 m up) is a window, never a door.
	 * The sizes are given to 0.1 m, so they are compared with a little over half of that (Slack): 2.3 m wide takes up to 2.36 m.
	 */
	struct FDoorRule
	{
		float MinHeight = 2.2f;
		float MaxHeight = 2.8f;
		float SingleMinWidth = 1.1f;
		float SingleMaxWidth = 2.3f;
		/** Double doors: these widths, give or take DoubleTolerance. */
		float DoubleWidths[2] = {3.f, 4.f};
		float DoubleTolerance = 0.1f;
		float Slack = 0.06f;
		float MaxStart = 0.5f;
		float MinUpperStart = 2.5f;
	};
	inline bool IsDoorSize(float Width, float Height, const FDoorRule& Door = FDoorRule())
	{
		const float S = Door.Slack;
		if (Height < Door.MinHeight - S || Height > Door.MaxHeight + S) return false;
		if (Width >= Door.SingleMinWidth - S && Width <= Door.SingleMaxWidth + S) return true;
		for (const float Double : Door.DoubleWidths)
		{
			if (FMath::Abs(Width - Double) <= Door.DoubleTolerance + S) return true;
		}
		return false;
	}
	/** Start: height of the opening's lowest point above the building's lowest opening. */
	inline bool IsDoorStart(float Start, const FDoorRule& Door = FDoorRule())
	{
		return Start < Door.MaxStart || Start >= Door.MinUpperStart - Door.Slack;
	}

	/** A window is under this tall and starts at least this far above the building's lowest opening. */
	inline constexpr float WindowMaxHeight = 2.1f;
	inline constexpr float WindowMinStart = 0.5f;

	/** The lowest point of the building's openings in walls up to MaxArea; of all its openings when there is none. */
	inline float LowestOpening(TConstArrayView<FOpening> Openings)
	{
		float Lowest = MAX_flt, Any = MAX_flt;
		for (const FOpening& Opening : Openings)
		{
			Any = FMath::Min(Any, Opening.Bottom);
			if (!Opening.bFlat && Opening.Area <= MaxArea) Lowest = FMath::Min(Lowest, Opening.Bottom);
		}
		return Lowest < MAX_flt ? Lowest : (Any < MAX_flt ? Any : 0.f);
	}

	inline EKind KindOf(const FOpening& Opening, float Lowest, const FDoorRule& Door = FDoorRule())
	{
		if (Opening.Area > MaxArea) return EKind::None;
		if (Opening.bFlat) return EKind::Flat;
		const float Start = Opening.Bottom - Lowest;
		if (IsDoorSize(Opening.Width, Opening.Height, Door) && IsDoorStart(Start, Door)) return EKind::Door;
		if (Opening.Height < WindowMaxHeight && Start >= WindowMinStart) return EKind::Window;
		return EKind::Entrance;
	}

	/** The test colors of personal.FarWindows.Kinds 1: doors red, every opening that gets a far room green. */
	inline FColor TestColor(EKind Kind)
	{
		return Kind == EKind::Door ? FColor(220, 0, 0) : FColor(0, 200, 0);
	}

	/** The check box Far Terrain Rooms (option row PersonalFarWindows). */
	inline constexpr const TCHAR* OptionKey = TEXT("PersonalFarWindows");
	/**
	 * Far terrain windows are on: door panels, and far rooms unless personal.FarWindows.Rooms says otherwise.
	 * Console variable personal.FarWindows.Draw: -1 follows the check box, 0 off, 1 on. Far terrain objects are needed either way.
	 */
	bool IsOn();
	/** Console variable personal.FarWindows.Kinds: panels in the test colors in every opening, near and far. */
	bool ShowKinds();
	/** Console variable personal.FarWindows.LogRooms: log when the room behind a door panel is first shown and when its mesh is first drawn. */
	bool LogRooms();

	/** Door panels and far rooms are made out to this ring: where far scenery draws buildings. */
	inline int32 OuterRing(float FogEndCm, float WorldScale)
	{
		const int32 Fog = PersonalFarScenery::FogRing(FogEndCm, 192.f * WorldScale);
		return FMath::Min(PersonalFarScenery::HideWithFog() ? Fog : PersonalFarScenery::LastRing, PersonalFarScenery::Radius());
	}
	/** Landblocks with door panels while the player is in Center. */
	inline bool IsPrepared(FIntPoint Block, FIntPoint Center, int32 InOuterRing)
	{
		return PersonalFarScenery::Ring(Block, Center) <= FMath::Min(InOuterRing, PersonalFarScenery::LastRing)
			&& Block.X >= 0 && Block.Y >= 0 && Block.X < PersonalFarScenery::WorldLandblocks && Block.Y < PersonalFarScenery::WorldLandblocks;
	}

	/**
	 * A door panel is shown until the door has taken over: the player is within DoorDistance (the server sends the door
	 * object by distance) and the client draws the room behind it. Hidden for the building the player is in.
	 * In test mode every panel is shown, so each opening keeps its color while walking up.
	 */
	inline bool ShowPanel(bool bTest, bool bDoorTakenOver, bool bPlayerInside)
	{
		return bTest || (!bPlayerInside && !bDoorTakenOver);
	}
	/** Console variable personal.FarWindows.DoorDistance: a door panel stays until the player is within this many meters of it. */
	float DoorDistance();
	/** True when the player is near enough to a door for its panel to go. Once in reach, it stays so until 10% further away. */
	inline bool DoorInReach(float Distance, float Reach, bool bWasInReach)
	{
		return Distance <= Reach * (bWasInReach ? 1.1f : 1.f);
	}

	// Color. Panels carry their own color: no sun, no shadow, so near and far look the same.

	/**
	 * How much it is night, 0 to 1, by the day fraction the sky uses. Follows the stars layer:
	 * 0 from 0.27 to 0.84, rising to 1 at 0.96, 1 until 0.16, falling to 0 at 0.27.
	 */
	inline float NightAmount(float DayFraction)
	{
		const float Fraction = DayFraction - FMath::FloorToFloat(DayFraction);
		if (Fraction <= 0.16f || Fraction >= 0.96f) return 1.f;
		if (Fraction < 0.27f) return (0.27f - Fraction) / (0.27f - 0.16f);
		if (Fraction <= 0.84f) return 0.f;
		return (Fraction - 0.84f) / (0.96f - 0.84f);
	}
	/** Doors keep this share of their color at night, so they do not stand out against a dark wall. */
	inline constexpr float NightDim = 0.25f;
	inline float DimLevel(float Night)
	{
		return FMath::Lerp(1.f, NightDim, FMath::Clamp(Night, 0.f, 1.f));
	}
	inline FColor Scaled(FColor Color, float Level)
	{
		const float L = FMath::Clamp(Level, 0.f, 1.f);
		return FColor(uint8(FMath::RoundToInt32(Color.R * L)), uint8(FMath::RoundToInt32(Color.G * L)), uint8(FMath::RoundToInt32(Color.B * L)), 255);
	}
	/** Console variable personal.FarWindows.DoorColor: "R,G,B", 0 to 255, the color of door panels by day. */
	FColor DoorColor();
}
