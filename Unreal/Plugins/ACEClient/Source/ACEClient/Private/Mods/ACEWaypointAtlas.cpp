#include "Mods/ACEWaypointAtlas.h"
#include "Interfaces/IPluginManager.h"
#include "XmlFile.h"
#include "Algo/Reverse.h"

namespace ACEWaypointAtlas
{
const TArray<FRegion>& MapRegions()
{
    static const TArray<FRegion> Regions=[]
    {
        TArray<FRegion> Result;
        const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
        if(!Plugin)return Result;
        FXmlFile Xml(Plugin->GetBaseDir()/TEXT("ClientMods/waypoint/MapRegions.xml"));
        if(!Xml.IsValid() || !Xml.GetRootNode())return Result;
        // Nested areas and rectangular islands take precedence over the enclosing polygon.
        for(const TCHAR* Tag:{TEXT("innerRect"),TEXT("innerPoly"),TEXT("rect"),TEXT("poly")})
        {
            for(const FXmlNode* Node:Xml.GetRootNode()->GetChildrenNodes())
            {
                if(Node->GetTag()!=Tag)continue;
                FRegion Region;
                for(const FXmlNode* Vertex:Node->GetChildrenNodes())
                {
                    FVector2D Point(FCString::Atod(*Vertex->GetAttribute(TEXT("EW"))),FCString::Atod(*Vertex->GetAttribute(TEXT("NS"))));
                    Region.Bounds+=Point;
                    Region.Polygon.Add(Point);
                }
                if(Region.Polygon.Num()<2)continue;
                if(Region.Polygon.Num()==2)Region.Polygon.Reset();
                Result.Add(MoveTemp(Region));
            }
        }
        return Result;
    }();
    return Regions;
}

static int32 RegionAt(const FVector2D Point,TConstArrayView<FRegion> Regions)
{
    for(int32 Index=0;Index<Regions.Num();++Index)
    {
        const FRegion& Region=Regions[Index];
        if(!Region.Bounds.IsInsideOrOn(Point))continue;
        const auto& Vertices=Region.Polygon;
        if(Vertices.IsEmpty())return Index;
        bool Inside=false;
        for(int32 I=0,J=Vertices.Num()-1;I<Vertices.Num();J=I++)
        {
            const auto& A=Vertices[I];const auto& B=Vertices[J];
            if((A.Y>Point.Y)!=(B.Y>Point.Y) && Point.X<(B.X-A.X)*(Point.Y-A.Y)/(B.Y-A.Y)+A.X)
                Inside=!Inside;
        }
        if(Inside)return Index;
    }
    return INDEX_NONE; // Mainland.
}

TArray<FACEWaypointLandmark> FindRoute(FVector2D Start,const FACEWaypointLandmark& Destination,
    TConstArrayView<FACEWaypointLandmark> Transports,TConstArrayView<FRegion> Regions,TFunctionRef<bool()> Cancelled)
{
    struct FStop
    {
        FVector2D Entrance,Exit;
        int32 EntranceRegion,ExitRegion;
        double Distance=TNumericLimits<double>::Max();
        int32 Hops=MAX_int32,Previous=INDEX_NONE;
        bool Visited=false;
    };
    TArray<FStop> Stops;
    auto Add=[&](FVector2D Entrance,FVector2D Exit)
    {
        Stops.Add({Entrance,Exit,RegionAt(Entrance,Regions),RegionAt(Exit,Regions)});
    };
    for(const auto& Link:Transports)
    {
        check(Link.TravelExit.IsSet());
        Add(Link.Coordinates,Link.TravelExit.GetValue());
    }
    const int32 Goal=Stops.Num();Add(Destination.Coordinates,Destination.Coordinates);
    const int32 Origin=Stops.Num();Add(Start,Start);
    int32 Current=Origin;
    Stops[Origin].Distance=0;Stops[Origin].Hops=0;
    // Dijkstra over directed transports. Generate walking connections on demand so a
    // dense atlas never allocates a portal-by-portal adjacency matrix.
    while(Current!=Goal)
    {
        if(Cancelled())return {};
        FStop& From=Stops[Current];From.Visited=true;
        int32 Next=INDEX_NONE;
        for(int32 I=0;I<=Goal;++I)
        {
            FStop& To=Stops[I];
            if(To.Visited)continue;
            if(From.ExitRegion==To.EntranceRegion)
            {
                const double Walk=FVector2D::Distance(From.Exit,To.Entrance);
                const double Total=From.Distance+Walk;
                const int32 Hops=From.Hops+(I!=Goal);
                if(Walk<50. && (Total<To.Distance || (Total==To.Distance && Hops<To.Hops)))
                {
                    To.Distance=Total;To.Hops=Hops;To.Previous=Current;
                }
            }
            if(To.Previous!=INDEX_NONE && (Next==INDEX_NONE || To.Distance<Stops[Next].Distance ||
               (To.Distance==Stops[Next].Distance && To.Hops<Stops[Next].Hops)))Next=I;
        }
        if(Next==INDEX_NONE)return {};
        Current=Next;
    }
    TArray<FACEWaypointLandmark> Steps{Destination};
    Steps[0].TravelExit.Reset(); // The destination is an arrival, even if it is a portal.
    for(int32 I=Stops[Goal].Previous;I!=Origin;I=Stops[I].Previous)Steps.Add(Transports[I]);
    Algo::Reverse(Steps);
    return Steps;
}
}
