#pragma once

#include "CoreMinimal.h"

struct FPersonalFarTerrainBake;

/**
 * Personal far terrain: one texture around the player made from the same per-cell
 * terrain textures the loaded terrain draws (FACELandblockMeshBuilder::BakePCode),
 * scaled down. The near part of the far terrain shell is drawn with it.
 *
 * The texture wraps: world cell (X, Y) is always at pixel (X * CellPixels, Y * CellPixels)
 * modulo the texture size. So mesh UVs do not depend on where the player is, and a move
 * of one landblock only rewrites the landblocks that came into range.
 */
namespace PersonalFarTerrainDetail
{
	constexpr int32 TextureSize = 4096;
	/** Texture pixels per 24 m cell. */
	constexpr int32 CellPixels = 16;
	/** Mips that stay inside one cell: 16, 8, 4, 2 and 1 pixels. */
	constexpr int32 CellMips = 5;
	/** Cells and landblocks across the texture. */
	constexpr int32 CellsAcross = TextureSize / CellPixels;
	constexpr int32 BlocksAcross = CellsAcross / 8;
	/** The texture holds landblocks Center - TextureRings to Center + TextureRings - 1. */
	constexpr int32 TextureRings = BlocksAcross / 2;
	/**
	 * The mesh uses the texture for Center - MeshRings to Center + MeshRings. Two rings
	 * smaller, so a texture and a mesh made one landblock apart still fit together.
	 */
	constexpr int32 MeshRings = TextureRings - 2;
	constexpr int32 MipCount = 13;

	/** The Far Terrain Textures setting. */
	bool Enabled();

	/** Cells (min inclusive, max exclusive) a mesh may draw with a texture made for TextureCenter. */
	FIntRect MeshCells(FIntPoint TextureCenter);

	/** True when a texture made for TextureCenter has everything a mesh around MeshCenter uses. */
	bool Covers(FIntPoint TextureCenter, FIntPoint MeshCenter);

	/** Texture coordinate of a cell corner. Not wrapped: it must interpolate across a quad. */
	inline FVector2D CornerUV(int32 X, int32 Y) { return FVector2D(X / float(CellsAcross), Y / float(CellsAcross)); }
}

class FPersonalFarTerrainDetail
{
public:
	/** One rectangle of one mip to copy to the texture. */
	struct FUpload
	{
		int32 Mip = 0;
		int32 X = 0, Y = 0, Width = 0, Height = 0;
		TArray<FColor> Pixels;
	};

	/** Gives the square texture loaded terrain draws for a terrain palette code (BakePCode). */
	using FBakeTile = TFunction<bool(uint32 PalCode, TArray<FColor>& OutPixels, int32& OutSize)>;

	/**
	 * Bring the texture content to Center. Out gets every part that changed.
	 * Call from one thread at a time; the caller must apply every upload it is given.
	 */
	void Update(const FPersonalFarTerrainBake& Bake, FIntPoint Center, const FBakeTile& BakeTile, TArray<FUpload>& Out);

	int32 TileCount() const { return Tiles.Num(); }
	/** Tiles made since the last save or load. */
	int32 UnsavedTiles() const { return Unsaved; }
	bool SaveTiles(const FString& Path, uint64 Key);
	bool LoadTiles(const FString& Path, uint64 Key);

	static FString TileCachePath(uint64 Key);
	/** Pixels of one tile: the five cell mips one after another. */
	static constexpr int32 TilePixels = 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1;

private:
	const TArray<FColor>& Tile(const FPersonalFarTerrainBake& Bake, int32 CellX, int32 CellY, const FBakeTile& BakeTile);

	/** Per palette code: the cell mips, already turned the way the texture needs them. */
	TMap<uint32, TArray<FColor>> Tiles;
	int32 Unsaved = 0;
	/** Which landblock each slot of the texture holds. */
	TArray<FIntPoint> SlotBlock;
	/** The 1-pixel-per-cell mip. The smaller mips are made from it. */
	TArray<FColor> LastCellMip;
};

namespace PersonalFarTerrainDetail
{
	/** Tile source that reads the DAT files in the folder. It opens them on first use; use it from one thread at a time. */
	FPersonalFarTerrainDetail::FBakeTile MakeDatTileSource(const FString& DatDirectory);
}
