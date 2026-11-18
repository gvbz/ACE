#pragma once

#include "CoreMinimal.h"
#include "Personal/PersonalFarScenery.h"
#include "Personal/PersonalFarWindows.h"

/**
 * Personal far rooms: the rooms behind every opening of a building that is not a door, drawn where the client
 * draws none (it loads rooms within one landblock only). The rules; no state.
 */
namespace PersonalFarRooms
{
	/** The client draws rooms within this many landblocks of the player's (UACETerrainPresenterComponent::FullDetailRadius). */
	inline constexpr int32 ClientRoomRing = 1;

	/**
	 * Far rooms are drawn. Console variable personal.FarWindows.Rooms: -1 follows the feature (personal.FarWindows.Draw),
	 * 0 off, 1 on. Far terrain objects are needed either way.
	 */
	bool IsOn();
	/** Console variable personal.FarWindows.RoomsBudgetMs: game thread time per update for making far room meshes. */
	float BudgetMs();

	/** Every opening that is not a door shows the rooms behind it: windows, open entrances, floor and roof openings, openings over 12 m2. */
	inline bool GetsRooms(PersonalFarWindows::EKind Kind)
	{
		return Kind != PersonalFarWindows::EKind::Door;
	}

	/** True for the short id of a room (the low word of a cell id); 0xFFFF is "outside". */
	inline bool IsRoom(uint16 ShortId)
	{
		return ShortId >= 0x100 && ShortId != 0xFFFF;
	}

	/**
	 * The rooms an opening shows: the room behind it and every room of its portal's stab list, the list the client
	 * loads close up (AddLandblockGrabVisible). Added to InOutRooms without repeats, as full cell ids.
	 */
	inline void AddRooms(uint32 LandblockKey, uint16 OtherCellId, TConstArrayView<uint16> StabCells, TArray<uint32>& InOutRooms)
	{
		if (!IsRoom(OtherCellId))
		{
			return;
		}
		InOutRooms.AddUnique((LandblockKey & 0xFFFF0000u) | OtherCellId);
		for (const uint16 Stab : StabCells)
		{
			if (IsRoom(Stab)) InOutRooms.AddUnique((LandblockKey & 0xFFFF0000u) | Stab);
		}
	}

	/** Far rooms are made out to the ring where far scenery draws buildings. */
	inline bool IsWanted(FIntPoint Block, FIntPoint Center, int32 OuterRing)
	{
		return PersonalFarScenery::Ring(Block, Center) <= FMath::Min(OuterRing, PersonalFarScenery::InteriorRings)
			&& Block.X >= 0 && Block.Y >= 0 && Block.X < PersonalFarScenery::WorldLandblocks && Block.Y < PersonalFarScenery::WorldLandblocks;
	}

	/**
	 * The far rooms of a building are shown past the client's own rooms, and within them for as long as the client
	 * does not draw a room behind one of the building's far-room openings. A far room is in the same place as the
	 * client's and looks the same, so both may be drawn. Hidden for the building the player is in.
	 */
	inline bool ShowRooms(int32 Ring, bool bARoomIsNotDrawn, bool bPlayerInside)
	{
		return !bPlayerInside && (Ring > ClientRoomRing || bARoomIsNotDrawn);
	}

	/**
	 * Day fog. The client's rooms take no fog (FogAmount 0), their walls do, so far rooms by day are darker than the
	 * fogged walls around them. A far room takes FogAmount = day x ramp: no fog at night, so lights show from far away;
	 * by day none out to Near meters, rising smoothly to all of it at Far, so it still equals the client's room
	 * at the handoff. Night: 0 to 1 (PersonalFarWindows::NightAmount, the stars curve).
	 */
	inline float RoomFog(float Night, float Distance, float Near, float Far)
	{
		const float Ramp = Far > Near ? FMath::SmoothStep(Near, Far, Distance) : (Distance >= Near ? 1.f : 0.f);
		return (1.f - FMath::Clamp(Night, 0.f, 1.f)) * Ramp;
	}
	/** The fog amount is set in steps of this size, so a material changes only when one can see it. */
	inline constexpr float FogStepSize = 0.04f;
	inline int32 FogStep(float Amount)
	{
		return FMath::RoundToInt32(FMath::Clamp(Amount, 0.f, 1.f) / FogStepSize);
	}
	/**
	 * Console variable personal.FarWindows.RoomLift, in centimeters (default 3): far rooms stand this much higher than the
	 * client's rooms. A far room's floor lies in the plane of the ground around its building, so from far away the two
	 * fight for the pixels seen through a doorway (recordings of 2026-11-15, 7d). Applied live, no rebuild.
	 */
	float RoomLiftCm();
	/** The lift as an offset in Unreal units, straight up (a building stands upright, so its own up is the world's). */
	inline FVector LiftOffset(float WorldScale)
	{
		return FVector(0.f, 0.f, RoomLiftCm() * WorldScale / 100.f);
	}
	/** Console variable personal.FarWindows.RoomsAlways: far rooms shown in rings 0 and 1 whatever the client draws. */
	bool RoomsAlways();
	/** Console variable personal.FarWindows.LogFlicker: log every change of far-room visibility near the camera. */
	bool LogFlicker();
	/** Console variables personal.FarWindows.RoomFogNear and .RoomFogFar (meters). The day fog is always on. */
	float RoomFogNear();
	float RoomFogFar();

	/** Mixes one more value into a sharing key. Two buildings share a far-room mesh when their keys are equal. */
	inline uint32 MixKey(uint32 Key, uint32 Value)
	{
		return HashCombineFast(Key, Value);
	}
}
