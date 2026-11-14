#pragma once

#include "CoreMinimal.h"

/** Personal far terrain: the two settings every user of the far terrain reads. */
namespace PersonalFarTerrain
{
	/** Option row PersonalFarTerrain: the Far Terrain check box. */
	inline constexpr const TCHAR* OptionKey = TEXT("PersonalFarTerrain");

	/** Option row PersonalFarTerrainRadius: the Far Terrain Radius slider. Its row holds the range. */
	inline constexpr const TCHAR* RadiusKey = TEXT("PersonalFarTerrainRadius");

	/**
	 * The check box is on, and the saved radius is not 0. Before the check box existed,
	 * radius 0 was how the far terrain was turned off; a saved 0 still means off.
	 */
	bool IsOn();

	/** The radius in landblocks, a whole number within the slider's range. */
	int32 Radius();
}
