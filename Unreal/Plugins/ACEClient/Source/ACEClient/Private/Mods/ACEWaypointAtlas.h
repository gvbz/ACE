#pragma once
#include "Mods/ACEPluginSubsystem.h"

namespace ACEWaypointAtlas
{
    struct FImportResult
    {
        TArray<FACEWaypointLandmark> Locations;
        int32 InvalidLocations=0,InvalidExits=0,RetiredLocations=0;
    };
    // Accept normalized GoArrow exports and raw Crossroads of Dereth atlases.
    // On failure the previous result is untouched; individual bad records are
    // reported rather than preventing a usable atlas from being imported.
    bool ParseLocations(const FString& Xml,FImportResult& Out,FString& Error);
    struct FRegion
    {
        FBox2D Bounds{ForceInit};
        TArray<FVector2D> Polygon;
    };
    const TArray<FRegion>& MapRegions();
    TArray<FACEWaypointLandmark> FindRoute(FVector2D Start,const FACEWaypointLandmark& Destination,
        TConstArrayView<FACEWaypointLandmark> Transports,TConstArrayView<FRegion> Regions,TFunctionRef<bool()> Cancelled);
}
