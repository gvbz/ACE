#pragma once

#include "CoreMinimal.h"
#include "ACETypes.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACESetupMeshBuilder.h" // FACEBuiltMeshSection

struct FACEEnvCellPointLight
{
	FVector BlockPosition;
	FLinearColor Color;
	float Intensity;
	float Radius;
};

	/**
	 * Built indoor EnvCell — per-surface drawing sections in cell-local space, plus the cell's own
	 * placement Frame (within its owning landblock) and the pass-through data the presenter needs
	 * to sync neighboring cells (VisibleCells) and spawn scenery (StaticObjects).
	 *
	 * Draw is always FullCell (all CellStruct drawing polygons). Portals admit/order shells;
	 * they are never applied as GPU fragment clips on these sections.
	 */
struct FACEBuiltEnvCellMesh
{
	uint32 EnvCellId = 0;
	FVector3f Origin = FVector3f::ZeroVector;
	FQuat4f Orientation = FQuat4f::Identity;
	EACEEnvCellFlags Flags = static_cast<EACEEnvCellFlags>(0);
	TArray<FACEBuiltMeshSection> Sections;
	/** Physics-polygon collision mesh (cell-local). Prefer this over drawing sections for blocking. */
	TArray<FACEBuiltMeshSection> CollisionSections;
	TArray<FACEDatStab> StaticObjects;
	/** Lights owned by this cell. Position and radius are in Unreal units. */
	TArray<FACEEnvCellPointLight> PointLights;
	TArray<uint16> VisibleCells;
	TArray<FACEDatCellPortal> CellPortals;
	/**
	 * Per CellPortals[i] aperture polygon in cell-local Unreal space (RH→LH + WorldScale).
	 * Used for outdoor building-portal admission / depth apertures — never as EnvCell clip planes.
	 * Empty entry when the portal poly could not be resolved.
	 */
	TArray<TArray<FVector>> PortalApertureLocalVerts;
	/** Unit normal per portal aperture (cell-local Unreal); outdoor half-space is N·P+D > 0. */
	TArray<FVector> PortalApertureLocalNormals;
	TArray<float> PortalApertureLocalD;
	/**
	 * Outside-portal planes in cell-local Unreal space (after RH→LH + WorldScale).
	 * xyz = outward normal (outdoor half-space), w = D so N·P+D > 0 is outdoors.
	 */
	TArray<FVector4f> OutsidePortalPlanes;
	/**
	 * Cell-local draw mesh for CellPortal OtherCellId==0xFFFF polys — CustomDepth stencil
	 * writers for indoor look-out (retail PView landscape-through-outdoor-portals).
	 */
	FACEBuiltMeshSection OutsidePortalMesh;
	/** Transit cell: all draw polys are portals, no solid geometry — valid but not spawnable. */
	bool bPortalConnector = false;
	/** Cell-local AABB of drawable geometry (Unreal units after RH→LH + WorldScale). */
	FVector LocalBoundsMin = FVector::ZeroVector;
	FVector LocalBoundsMax = FVector::ZeroVector;
	bool bHasLocalBounds = false;
	/** CellStruct CellBSP for retail EnvCell.point_in_cell (ACE cell-local coords). Root = 0. */
	TArray<FACEDatCellBspNode> CellBspNodes;

	bool SeesOutside() const
	{
		return EnumHasAnyFlags(Flags, EACEEnvCellFlags::SeenOutside);
	}

	/** CellPortal OtherCellId 0xFFFF — transition to outdoor landcells. */
	bool HasOutsidePortal() const
	{
		for (const FACEDatCellPortal& Portal : CellPortals)
		{
			if (Portal.IsOutsidePortal())
			{
				return true;
			}
		}
		return false;
	}

	/** Retail indoor LScape peek: SeenOutside flag and/or an outside portal. */
	bool CanSeeOutside() const
	{
		return SeesOutside() || HasOutsidePortal();
	}

	bool IsEmpty() const
	{
		for (const FACEBuiltMeshSection& S : Sections)
		{
			if (!S.IsEmpty())
			{
				return false;
			}
		}
		for (const FACEBuiltMeshSection& S : CollisionSections)
		{
			if (!S.IsEmpty())
			{
				return false;
			}
		}
		if (!OutsidePortalMesh.IsEmpty())
		{
			return false;
		}
		return StaticObjects.Num() == 0;
	}

	/** World-space transform of this EnvCell relative to its landblock origin. */
	FTransform GetCellLocalToLandblock(float WorldScale) const
	{
		const FQuat CellQuat = FACEPosition::AceQuatToUnreal(
			Orientation.W, FVector(Orientation.X, Orientation.Y, Orientation.Z));
		const FVector CellLoc = FACEPosition::AceVectorToUnreal(
			FVector(Origin.X, Origin.Y, Origin.Z), WorldScale);
		return FTransform(CellQuat, CellLoc);
	}
};

/**
 * Builds drawable geometry for one indoor EnvCell (dungeon/building interior) by looking up its
 * CellStruct prefab block (Environment.Cells[CellStructure], found via EnvironmentId) and fanning
 * its drawing polygons — the same per-polygon UV/vertex-color approach as
 * FACESetupMeshBuilder::AppendGfxObjLocal, since CellStruct's vertex/polygon on-disk shapes are
 * shared with GfxObj. Surfaces are looked up directly in the EnvCell's own Surfaces list (already
 * full 0x08000000 ids), not the GfxObj's.
 */
class ACECLIENT_API FACEEnvCellMeshBuilder
{
public:
	FACEEnvCellMeshBuilder(FACEDatDatabase* InCellDat, FACEDatDatabase* InPortal, FACEDatTextureResolver* InTextures)
		: CellDat(InCellDat), Portal(InPortal), Textures(InTextures)
	{
	}

	bool BuildEnvCell(uint32 EnvCellId, float WorldScale, FACEBuiltEnvCellMesh& OutMesh);

private:
	bool LoadEnvironment(uint32 EnvironmentId, FACEDatEnvironment& Out);
	const FACEDatEnvironment* GetEnvironment(uint32 EnvironmentId);
	void BakeStaticLighting(const FACEDatEnvCell& Cell, float WorldScale, FACEBuiltEnvCellMesh& Mesh);
	void AppendCellStructLocal(
		const FACEDatCellStruct& CellStruct,
		const TArray<uint32>& Surfaces,
		const TArray<FACEDatCellPortal>& Portals,
		float WorldScale,
		TArray<FACEBuiltMeshSection>& OutSections,
		FBox& OutLocalBounds);
	void AppendCellStructPhysicsLocal(
		const FACEDatCellStruct& CellStruct,
		float WorldScale,
		TArray<FACEBuiltMeshSection>& OutCollisionSections,
		FBox& OutLocalBounds);

	FACEDatDatabase* CellDat = nullptr;
	TMap<uint32, TArray<FACEDatLightInfo>> SetupLightCache;
	FACEDatDatabase* Portal = nullptr;
	FACEDatTextureResolver* Textures = nullptr;
	TMap<uint32, FACEDatEnvironment> EnvironmentCache;
};
