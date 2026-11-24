#pragma once

#include "CoreMinimal.h"

class UWorld;
struct FHitResult;

namespace ACEWaterSight
{
    bool BlockedFromWithinWater(UWorld& World, const FVector& Start, const FVector& End, float Radius);

    // Returns true when a hit on a water-textured heightfield was refined.
    // SolidDistance is the nearest physical bed/bank in that component, or MAX_dbl.
    bool RefineTerrainHit(UWorld& World, const FHitResult& Hit, const FVector& Start,
        const FVector& End, float Radius, double& SolidDistance);
}
