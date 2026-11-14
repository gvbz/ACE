#pragma once

#include "CoreMinimal.h"

/**
 * Personal sky horizon. The stars layer of the retail sky is the upper half of a cube,
 * open at the bottom, so it ends at eye level. Below eye level the day dome is not
 * covered and shows as a bright strip between the far terrain and the night sky.
 * This continues the walls of the stars layer straight down.
 */
namespace PersonalSkyHorizon
{
	/** GfxObj of the stars layer. */
	inline constexpr uint32 StarsGfxId = 0x010015EFu;

	/** Option row PersonalSkyHorizon. */
	inline constexpr const TCHAR* OptionKey = TEXT("PersonalSkyHorizon");

	/**
	 * Adds one quad below every rim edge: an edge at height 0 that belongs to one face only,
	 * with that face above it. The quad goes down by the half-width of the mesh and faces
	 * the same way as the face above. Texture coordinates continue at the same scale;
	 * normals and colors are copied from the rim. Existing entries are not changed.
	 * Returns the number of triangles added.
	 */
	int32 ExtendRim(TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector>& Normals,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors);

	/** For one section of a sky mesh: extends the stars layer when the option and the far terrain are on. */
	void ExtendSection(uint32 GfxId, TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector>& Normals,
		TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors);

	/** Builds the sky meshes again, so a changed option shows at once. */
	void RebuildSky();
}
