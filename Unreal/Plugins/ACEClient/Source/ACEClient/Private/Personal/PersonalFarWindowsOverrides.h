#pragma once

#include "CoreMinimal.h"
#include "Personal/PersonalFarWindows.h"

/**
 * Personal far windows: overrides for openings whose size cannot tell whether they hold a door.
 * The client ships a list (plugin Data/Personal/FarWindowsOverrides.txt), read first; Saved/Personal/FarWindowsOverrides.txt adds to it
 * and wins, read once at start; personal.FarWindows.Mark changes and writes it. A line "rule" undoes a shipped line.
 * An opening is named by its building type and its order in the model ([0], [1], ... as the survey numbers them).
 * A type line applies to every building of that type; a line for one building (landblock and building number) wins over it.
 *
 *   0x010014C3 1 open              type 0x010014C3, opening [1]: no door, far rooms
 *   7E65 0 0x010014C3 1 door       landblock 7E65, building 0 (of that type), opening [1]: a door
 */
namespace PersonalFarWindowsOverrides
{
	enum class EOverride : uint8
	{
		/** No override: the rule decides ("rule" in the file). */
		None,
		Door,
		Open
	};

	struct FOverrides
	{
		/** Building type << 32 | opening order. */
		TMap<uint64, EOverride> Types;
		/** Landblock (high 16 bits of its key) << 48 | building number << 32 | opening order; the type is checked too. */
		TMap<uint64, TPair<uint32, EOverride>> Buildings;
	};

	inline uint64 TypeKey(uint32 ModelId, int32 Order)
	{
		return (uint64(ModelId) << 32) | uint32(Order);
	}
	inline uint64 BuildingKey(uint32 LandblockKey, int32 Building, int32 Order)
	{
		return (uint64(LandblockKey >> 16) << 48) | (uint64(uint16(Building)) << 32) | uint32(Order);
	}

	/** Lines as in the file; unknown lines and lines starting with # are skipped. */
	void Parse(const FString& Text, FOverrides& Out);
	FString Write(const FOverrides& Overrides);

	EOverride Find(const FOverrides& Overrides, uint32 ModelId, uint32 LandblockKey, int32 Building, int32 Order);
	/** Kinds: in model order (PersonalFarSceneryMeshes::OpeningKinds). Door makes a door; open makes a door an open entrance. */
	void Apply(const FOverrides& Overrides, uint32 ModelId, uint32 LandblockKey, int32 Building, TArray<PersonalFarWindows::EKind>& Kinds);

	/** The overrides of the running client, read from the file on first use. */
	const FOverrides& Get();
	/**
	 * Set (None: remove) the type lines of some openings, and write the file. The file is read first, so lines taken out
	 * of it by hand while the client runs stay out. False when the file cannot be written.
	 */
	bool SetForType(uint32 ModelId, TConstArrayView<int32> Orders, EOverride Override);
	/** The Saved file (marks). */
	FString FilePath();
	/** The list shipped with the client, in the plugin. */
	FString ShippedPath();
	/** Lines of Over replace the same lines of InOut, and are added. */
	void Merge(const FOverrides& Over, FOverrides& InOut);
	const TCHAR* Name(EOverride Override);
}
