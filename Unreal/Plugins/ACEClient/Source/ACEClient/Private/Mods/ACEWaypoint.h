#pragma once
#include "CoreMinimal.h"
#include "ACETypes.h"
class FJsonObject;

namespace ACEWaypoint
{
    struct FLink { int32 Begin=0, End=0; FVector2D Coordinates; };
    struct FRoutePoint { FACEPosition Position; int32 Index=0; bool Connect=false,Current=false; };
    // Map-only route geometry: no world actors, ground traces or distance culling.
    TArray<FRoutePoint> MapRoute(const TSharedPtr<FJsonObject>& Route,const FACEPosition& Player,bool Dungeon,int32 Current);
    // Coordinates are signed E/W (X), N/S (Y), in the retail map's 240m units.
    TArray<FLink> FindCoordinates(const FString& Text);
    bool Parse(const FString& Text, FVector2D& Out);
    FString Format(FVector2D Coordinates);
    FString MarkerLabel(const FString& Name);
    struct FMapLabelCandidate
    {
        int32 Index;
        FVector2D Point,Size;
        FString Key;
        int32 Priority=0;
    };
    struct FMapLabel { int32 Index; FVector2D Origin,Size; };
    // Screen-local layout: marker icons remain independent of label admission.
    TArray<FMapLabel> LayoutLabels(TArray<FMapLabelCandidate> Candidates,FVector2D ViewSize,const TArray<FBox2D>& Reserved);
    struct FMapClusterPoint { int32 Index; FVector2D Point; FString Key; };
    struct FMapCluster { FVector2D Point; TArray<int32> Indices; };
    // Point is a fixed screen-local anchor; every member stays within Radius.
    // Invalid positions are ignored. A nonpositive/nonfinite radius keeps singletons.
    TArray<FMapCluster> ClusterMarkers(TArray<FMapClusterPoint> Points,double Radius=18.);
    FVector2D Coordinates(const FACEPosition& Position);
    double RelativeBearing(FVector2D From, FVector2D To, FVector2D Forward);
    inline double NorthBearing(FVector2D Forward)
    {return RelativeBearing(FVector2D::ZeroVector,FVector2D(0,1),Forward);}
    // Screen basis: north-up by default, or the normalized facing vector up.
    inline FVector2D ToMap(FVector2D Delta,FVector2D Up)
    {return {Delta.X*Up.Y-Delta.Y*Up.X,-FVector2D::DotProduct(Delta,Up)};}
    inline FVector2D FromMap(FVector2D Delta,FVector2D Up)
    {return {Delta.X*Up.Y-Delta.Y*Up.X,-Delta.X*Up.X-Delta.Y*Up.Y};}
}
