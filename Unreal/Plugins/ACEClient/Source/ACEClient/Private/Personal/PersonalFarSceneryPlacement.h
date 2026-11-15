#pragma once

#include "CoreMinimal.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatFileTypes.h"

struct FPersonalFarTerrainBake;

/**
 * Personal far scenery: where the RegionDesc scenery, the stabs and the buildings of a landblock go,
 * computed from the world bake and the DAT files without loading the landblock.
 * An own copy of the rules in UACEDatSubsystem::CollectRegionScenery; a test compares the two.
 */
namespace PersonalFarSceneryPlacement
{
	enum class EKind : uint8
	{
		Region,
		Stab,
		Building,
	};

	/**
	 * Object types are told apart by id and kind: the same id is another mesh as a stab (resting placement)
	 * or as a building (shell material). DAT ids use the low 26 bits, so the kind goes in the top two.
	 * A RegionDesc type's key is its id.
	 */
	inline uint32 TypeKey(uint32 Id, EKind Kind) { return (Id & 0x3FFFFFFFu) | (uint32(Kind) << 30); }
	inline uint32 TypeId(uint32 Key) { return Key & 0x3FFFFFFFu; }
	inline EKind TypeKind(uint32 Key) { return EKind(Key >> 30); }
	/** The placement the client builds the mesh with: stabs rest (UACEDatSubsystem::ACEPlacementResting). */
	inline int32 TypePlacement(uint32 Key) { return TypeKind(Key) == EKind::Stab ? 101 : 0; }

	struct FItem
	{
		uint32 SetupId = 0;
		int32 PlacementId = 0;
		/** AC units from the landblock's low corner. */
		FVector OriginAc = FVector::ZeroVector;
		FQuat Orientation = FQuat::Identity;
		float Scale = 1.f;
		EKind Kind = EKind::Region;
	};

	/** The DAT data the placement reads. Nothing changes after Open, so workers can share it. */
	struct FSource
	{
		/** Own handle: the landblock info files (buildings). Its reads are locked. */
		FACEDatDatabase Cell;
		FACEDatRegionSceneTables Tables;
		/** Every scene file the tables name, by id. */
		TMap<uint32, FACEDatScene> Scenes;
	};
	using FSourcePtr = TSharedPtr<const FSource, ESPMode::ThreadSafe>;

	/** Open the DAT files and read the scene tables and scenes. Slow; call off the game thread. Null on failure. */
	FSourcePtr Open(const FString& DatDirectory);

	/**
	 * The RegionDesc scenery of one landblock, in the order CollectRegionScenery gives it,
	 * then the stabs and the buildings of its info file, in the file's order.
	 * False when the landblock is not in the world or not in the DAT. Safe on any thread.
	 */
	bool Place(const FPersonalFarTerrainBake& Bake, const FSource& Source, uint32 LandblockId, TArray<FItem>& OutItems);
}
