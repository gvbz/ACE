#pragma once

#include "CoreMinimal.h"

struct FPersonalFarTerrainBake;

/** Mesh arrays for the far terrain shell, relative to the low corner of the center landblock. */
struct FPersonalFarTerrainMesh
{
	/** One spacing level: quads of Step cells between the previous level's box and this box. */
	struct FLevel
	{
		/** Quad size in 24 m cells: 1, 2, 4, 8 or 16. */
		int32 Step = 1;
		/** Corner range of the box, in cells from the world's low corner. */
		FIntPoint Min = FIntPoint::ZeroValue;
		FIntPoint Max = FIntPoint::ZeroValue;
	};

	/** Center landblock. */
	FIntPoint Center = FIntPoint::ZeroValue;
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	/** Quads drawn with the detail texture, and every vertex's coordinate in it. */
	TArray<int32> DetailTriangles;
	TArray<FVector2D> DetailUVs;
	/** World cell corner of each vertex. */
	TArray<FIntPoint> Corners;
	TArray<FLevel> Levels;

	bool IsEmpty() const { return Triangles.Num() == 0 && DetailTriangles.Num() == 0; }
};

namespace PersonalFarTerrainMesh
{
	constexpr int32 MaxRadius = 96;
	/**
	 * Landblock rings drawn at full detail (24 m quads, like loaded terrain). Each
	 * following level has quads twice as large and reaches twice as far.
	 */
	constexpr int32 FineRings = 8;
	constexpr float LandblockSizeAc = 192.f;
	constexpr float CellSizeAc = 24.f;

	/**
	 * Build the shell around the Center landblock out to Radius landblocks.
	 * Holes are landblocks the client already draws. They must lie within HoleRadius of Center.
	 * Next to a hole the shell has the same heights and diagonals as loaded terrain.
	 * Quads inside DetailCells (cells, max exclusive) go to DetailTriangles; without it all go to Triangles.
	 */
	void Build(const FPersonalFarTerrainBake& Bake, FIntPoint Center, int32 Radius,
		const TSet<FIntPoint>& Holes, int32 HoleRadius, float WorldScale, FPersonalFarTerrainMesh& Out,
		const FIntRect* DetailCells = nullptr);

	/** World position of the mesh origin for a center landblock. */
	FVector Origin(FIntPoint Center, float WorldScale);
}
