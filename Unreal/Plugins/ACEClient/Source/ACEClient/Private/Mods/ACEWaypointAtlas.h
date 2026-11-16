#pragma once
#include "Mods/ACEPluginSubsystem.h"

namespace ACEWaypointAtlas
{
    struct FRegion
    {
        FBox2D Bounds{ForceInit};
        TArray<FVector2D> Polygon;
    };
    const TArray<FRegion>& MapRegions();
    TArray<FACEWaypointLandmark> FindRoute(FVector2D Start,const FACEWaypointLandmark& Destination,
        TConstArrayView<FACEWaypointLandmark> Transports,TConstArrayView<FRegion> Regions,TFunctionRef<bool()> Cancelled);
}
