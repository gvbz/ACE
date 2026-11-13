#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"

struct FACEDatCellLandblock;

/**
 * Personal far terrain: the height and terrain type of every 24 m cell corner in the
 * world, read once from the cell DAT and cached on disk.
 */
struct FPersonalFarTerrainBake
{
	static constexpr int32 LandblockDim = 255;
	static constexpr int32 CellsPerLandblock = 8;
	/** Cell corners per axis: 255 landblocks of 8 cells, plus the last corner. */
	static constexpr int32 Dim = LandblockDim * CellsPerLandblock + 1;
	/** Height levels. Level N has one sample every 2^N cells; levels above 0 are smoothed. */
	static constexpr int32 LevelCount = 5;
	/** Side of the square color texture. Larger than Dim; the rest repeats the edge. */
	static constexpr int32 ColorTextureSize = 2048;
	/** Terrain code of a road corner. Codes below it are terrain types. */
	static constexpr uint8 RoadCode = 32;

	/** Per corner, index X * Dim + Y: index into HeightTable, and terrain code. */
	TArray<uint8> HeightIndex;
	TArray<uint8> TerrainCode;
	/** Per corner: the low 7 bits of the DAT terrain word (road in bits 0-1, terrain type in bits 2-6). */
	TArray<uint8> TerrainRaw;
	/** Heights in AC units. */
	float HeightTable[256] = {};
	/** One color (sRGB) per terrain code. */
	FColor Palette[RoadCode + 1];
	/** Smoothed heights for levels 1 and up. Not cached; BuildLevels fills them. */
	TArray<float> Levels[LevelCount - 1];
	/** Pixels for the color texture. The owner may empty this once the texture exists. */
	TArray<FColor> ColorPixels;

	FPersonalFarTerrainBake();
	void Init();
	bool IsValid() const { return HeightIndex.Num() == Dim * Dim && TerrainCode.Num() == Dim * Dim && TerrainRaw.Num() == Dim * Dim; }
	bool HasLevels() const { return Levels[LevelCount - 2].Num() > 0; }

	/** X and Y in cells from the world's low corner. Outside the world the edge value is used. */
	float HeightAt(int32 X, int32 Y) const { return HeightTable[HeightIndex[Index(X, Y)]]; }
	float HeightAt(int32 Level, int32 X, int32 Y) const;
	FColor ColorAt(int32 X, int32 Y) const { return Palette[TerrainCode[Index(X, Y)]]; }
	/** Terrain palette code of the cell whose low corner is (X, Y): the texture loaded terrain draws there. */
	uint32 PalCodeAt(int32 X, int32 Y) const;
	/** Terrain code for the low 7 bits of a DAT terrain word. */
	static uint8 CodeOfRaw(uint8 Raw) { return (Raw & 0x3) != 0 ? RoadCode : uint8((Raw >> 2) & 0x1F); }

	/** Store the 9x9 corners of one landblock. */
	void AddLandblock(int32 BlockX, int32 BlockY, const FACEDatCellLandblock& Land);
	/** Build the smoothed height levels. Needs a filled bake. */
	void BuildLevels();
	void BuildColorPixels();

	bool Save(const FString& Path, uint64 Key) const;
	/** False when the file is missing, damaged, or was made from other DAT files. */
	bool Load(const FString& Path, uint64 Key);

private:
	static int32 Index(int32 X, int32 Y)
	{
		return FMath::Clamp(X, 0, Dim - 1) * Dim + FMath::Clamp(Y, 0, Dim - 1);
	}
};

namespace PersonalFarTerrainBake
{
	using FBakePtr = TSharedPtr<FPersonalFarTerrainBake, ESPMode::ThreadSafe>;

	/** Changes when the DAT files change (path, size, time). 0 when a file is missing. */
	uint64 SourceKey(const FString& DatDirectory);

	FString CachePath(uint64 Key);

	/** Read the whole world from the DAT files. Slow; call off the game thread. */
	bool Build(const FString& DatDirectory, FPersonalFarTerrainBake& Out);

	/** Use the cache file when it matches the DAT files, otherwise build and save. */
	bool LoadOrBuild(const FString& DatDirectory, FPersonalFarTerrainBake& Out);

	/** LoadOrBuild, height levels and color pixels on a worker thread. Null when the bake failed. */
	TFuture<FBakePtr> LoadOrBuildAsync(const FString& DatDirectory);
}
