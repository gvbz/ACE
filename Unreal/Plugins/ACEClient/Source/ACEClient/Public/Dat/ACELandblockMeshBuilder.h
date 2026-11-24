#pragma once

#include "CoreMinimal.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACETerrainBlendCache.h"

class FACEDatTextureResolver;

struct FACEBuiltLandblockSection
{
	/** Land cell palette/tex-merge code (GetPalCode). Shared across cells with identical corners. */
	uint32 PCode = 0;
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	/** GPU TexMerge: UV1=(baseLayer,o0Tex), UV2=(o0AlphaUV), UV3=(o0AlphaLayer,roadTex). */
	TArray<FVector2D> UV1;
	TArray<FVector2D> UV2;
	TArray<FVector2D> UV3;
	TArray<FLinearColor> VertexColors;
	/** Baked TexMerge RGBA for this PCode (CPU path / cache fallback). */
	TArray<FColor> BakedPixels;
	FACETerrainBlendCache::FBlendPtr SharedBake;
	int32 BakeWidth = 0;
	int32 BakeHeight = 0;
	/** When true, draw with land Texture2DArray material (no BakedPixels required). */
	bool bGpuTexMerge = false;

	bool IsEmpty() const { return Vertices.Num() == 0 || Triangles.Num() < 3; }
	// Explicit diagnostic/export copy. Runtime uploads use SharedBake directly.
	TArray<FColor> GetBakedPixels() const { return SharedBake ? SharedBake->CopyBasePixels() : BakedPixels; }
	bool HasBakedTexture() const { return BakeWidth > 0 && BakeHeight > 0 && (SharedBake ? SharedBake->IsValid() : BakedPixels.Num() == int64(BakeWidth) * BakeHeight); }
};

struct FACEBuiltLandblockMesh
{
	uint32 LandblockId = 0;
	TArray<FACEBuiltLandblockSection> Sections;

	/** 9×9 AC-unit heights (LandHeightTable lookup) matching the visual/collision verts. */
	float HeightsAc[81] = {};
	bool bHasHeights = false;
	/** Raw CellLandblock terrain codes (roads + terrain type + scene type). */
	uint16 Terrain[81] = {};
	bool bHasTerrain = false;
	/**
	 * Per outdoor cell (8×8): 0=NotWater, 1=PartiallyWater, 2=EntirelyWater
	 * (retail LandCell.WaterType from LandblockStruct.CalcCellWater).
	 */
	uint8 CellWaterType[64] = {};
	bool bHasCellWater = false;
	/**
	 * LandblockStruct.water_type aggregate (retail CLandBlockStruct::CalcWater).
	 * EntirelyWater (2) → CLandCell::find_env_collisions Collided for walkers
	 * (open-ocean landblocks only). Cell EntirelyWater on a PartiallyWater block
	 * is walkable at 0.9 AC depth.
	 */
	uint8 BlockWaterType = 0;

	bool IsEmpty() const
	{
		for (const FACEBuiltLandblockSection& S : Sections)
		{
			if (!S.IsEmpty())
			{
				return false;
			}
		}
		return true;
	}
};

/** Outdoor landblock heightfield (9×9) using RegionDesc LandHeightTable + TexMerge blends. */
class ACECLIENT_API FACELandblockMeshBuilder
{
	friend class FACELandscapeTextureFidelityTest;
public:
	static constexpr int32 CellDim = 8;
	static constexpr int32 VertexDim = 9;
	/** Retail LandDefs.VertexPerCell — 4 corners per 24×24 cell, not a 3×3 neighborhood. */
	static constexpr int32 VertexPerCell = 1;
	static constexpr float CellSize = 24.f;
	static constexpr float LandblockSize = 192.f;
	static constexpr uint32 RegionFileId = 0x13000000;
	static constexpr int32 BakeSize = 128;
	static constexpr uint32 RoadTerrainType = 0x20;

	FACELandblockMeshBuilder(FACEDatDatabase* InPortal, FACEDatDatabase* InCell, FACEDatTextureResolver* InTextures,
		TSharedPtr<FACETerrainBlendCache, ESPMode::ThreadSafe> InBlendCache = nullptr)
		: Portal(InPortal), Cell(InCell), Textures(InTextures),
		  BlendCache(InBlendCache ? InBlendCache : MakeShared<FACETerrainBlendCache, ESPMode::ThreadSafe>())
	{
	}

	bool EnsureHeightTable();
	void SetCacheFingerprint(uint64 Fingerprint) { CacheFingerprint = Fingerprint; }
	TSharedPtr<FACETerrainBlendCache, ESPMode::ThreadSafe> GetBlendCache() const { return BlendCache; }
	bool EnsureTexMerge();
	bool BuildLandblock(uint32 LandblockId, float WorldScale, FACEBuiltLandblockMesh& OutMesh);

	/**
	 * Optional GPU atlas — when set and ready, BuildLandblock skips BakePCode and emits
	 * a single section with Texture2DArray layer indices packed into UV1–3 / VertexColor.
	 */
	void SetLandSurfaceAtlas(const class FACELandSurfaceAtlas* InAtlas) { LandAtlas = InAtlas; }

	/** CPU TexMerge bake for one terrain palette code (also used to hydrate disk tiles). */
	bool BakePCode(uint32 PCode, TArray<FColor>& OutPixels, int32& OutW, int32& OutH);
	/** Fill missing BakedPixels on CPU sections after a geometry-only disk load. */
	void HydrateSectionBakes(FACEBuiltLandblockMesh& Mesh, uint64 DatFingerprint);

	static bool GetSplitDir(uint32 LandblockId, int32 CellX, int32 CellY);
	static uint32 GetPalCode(int32 R1, int32 R2, int32 R3, int32 R4, int32 T1, int32 T2, int32 T3, int32 T4);

	/**
	 * Retail LScape::get_block_orient. DX/DY are landblock offsets from the viewer block.
	 * OutPolySize is 1/2/4/8 (side_cell_count = 8/poly). OutTransDir is LandDefs::Direction.
	 */
	static void GetBlockOrient(int32 DX, int32 DY, int32& OutPolySize, int32& OutTransDir);

	/**
	 * Subsample the cached 9×9 heightfield into a coarser draw mesh (ConstructVertices +
	 * TransAdjust). HeightsAc / Terrain on Full stay 9×9 for sampling.
	 */
	bool BuildLodSections(const FACEBuiltLandblockMesh& Full, int32 PolySize, int32 TransDir,
		float WorldScale, TArray<FACEBuiltLandblockSection>& OutSections);

	/** Plane-sample the 9×9 heightfield at local landblock XY (AC units, 0..192). */
	static bool SampleHeightAc(const FACEBuiltLandblockMesh& Mesh, float LocalX, float LocalY, float& OutZAc, FVector* OutNormalAc = nullptr);

	/**
	 * Retail CObjCell::get_water_depth / CLandBlockStruct::calc_water_depth (ACE LandblockStruct).
	 * Discrete sink into the land plane: 0 / 0.1 / 0.45 / 0.9 AC — not palette/blue darkness.
	 * Local XY in landblock space (AC units, 0..192).
	 */
	static float GetWaterDepthAc(const FACEBuiltLandblockMesh& Mesh, float LocalX, float LocalY);
	/** Water depths for the SW, SE, NE and NW quarters of a TexMerge cell. */
	static FLinearColor GetWaterDepthsAc(uint32 PCode);

private:
	bool FillSectionBake(FACEBuiltLandblockSection& Section);
	bool LoadTerrainSurface(uint32 TerrainType, FACEDatDecodedSurface& Out, uint32& OutTiling);
	FColor SampleTiled(const FACEDatDecodedSurface& Surf, float U, float V, uint32 Tiling) const;
	static void RotateUV(float U, float V, int32 Rot, float& OutU, float& OutV);
	bool FindTerrainAlpha(uint32 PCode, uint32 TCode, FACEDatDecodedSurface& OutAlpha, int32& OutRot, uint32* OutTexGID = nullptr);
	bool FindRoadAlpha(uint32 PCode, uint32 RCode, FACEDatDecodedSurface& OutAlpha, int32& OutRot, uint32* OutTexGID = nullptr);
	static void GetTerrainCodes(uint32 PCode, uint32 OutTypes[4]);
	bool GetTerrainLayers(uint32 PCode, uint32 OutTerrainTypes[4], uint32 OutTCodes[3], int32& OutLayerCount);
	void GetRoadCodes(uint32 PCode, uint32 OutRCodes[2], bool& bAllRoad) const;
	bool ResolveGpuBlend(uint32 PCode, struct FACELandGpuBlend& Out);
	FColor SampleAlpha(const FACEDatDecodedSurface& Alpha, float U, float V, int32 Rot) const;
	void MergeOverlay(TArray<FColor>& Pixels, int32 Size, const FACEDatDecodedSurface& Overlay, uint32 OverlayTiling, const FACEDatDecodedSurface& Alpha, int32 Rot) const;

	FACEDatDatabase* Portal = nullptr;
	FACEDatDatabase* Cell = nullptr;
	FACEDatTextureResolver* Textures = nullptr;
	const class FACELandSurfaceAtlas* LandAtlas = nullptr;
	TArray<float> LandHeightTable;
	bool bHeightTableReady = false;
	FACEDatTexMerge TexMerge;
	bool bTexMergeReady = false;
	TMap<uint32, FACEDatDecodedSurface> TerrainSurfCache;
	TMap<uint32, uint32> TerrainTilingCache;
	TSharedPtr<FACETerrainBlendCache, ESPMode::ThreadSafe> BlendCache;
	uint64 CacheFingerprint = 0;
};
