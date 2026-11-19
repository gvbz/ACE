#include "Mods/ACEWaypoint.h"
#include "Internationalization/Regex.h"
#include "Dom/JsonObject.h"

namespace ACEWaypoint
{
TArray<FRoutePoint> MapRoute(const TSharedPtr<FJsonObject>& Route,const FACEPosition& Player,bool Dungeon,int32 Current)
{
    TArray<FRoutePoint> Result;const TArray<TSharedPtr<FJsonValue>>* Points=nullptr;
    if(!Route||!Route->TryGetArrayField(TEXT("route"),Points))return Result;
    bool Have=false,FirstVisible=false;FRoutePoint First;
    // Bound work for imported/custom profiles. Never close a truncated loop.
    const int32 Count=FMath::Min(Points->Num(),8192);
    for(int32 I=0;I<Count;++I)
    {
        const TSharedPtr<FJsonObject>* O=nullptr;double Cell=0,X=0,Y=0,Z=0;
        if(!(*Points)[I]||!(*Points)[I]->TryGetObject(O)||!O->IsValid()||
           !(*O)->TryGetNumberField(TEXT("cell"),Cell)||!(*O)->TryGetNumberField(TEXT("x"),X)||
           !(*O)->TryGetNumberField(TEXT("y"),Y)||!(*O)->TryGetNumberField(TEXT("z"),Z)||
           !FMath::IsFinite(Cell)||Cell<1||Cell>MAX_uint32||Cell!=FMath::FloorToDouble(Cell)||
           !FMath::IsFinite(X)||!FMath::IsFinite(Y)||!FMath::IsFinite(Z))
        {Have=false;continue;}
        FRoutePoint Point;Point.Position.CellId=int32(uint32(Cell));Point.Position.Location=FVector(X,Y,Z);Point.Index=I+1;Point.Current=Point.Index==Current;
        bool Legacy=false;(*O)->TryGetBoolField(TEXT("legacy"),Legacy);
        if(Legacy&&(uint32(Player.CellId)&0xffff)>=0x100)
        {
            const uint32 From=uint32(Point.Position.CellId)>>16,To=uint32(Player.CellId)>>16;
            Point.Position.Location.X+=(int32(From>>8)-int32(To>>8))*192;
            Point.Position.Location.Y+=(int32(From&255)-int32(To&255))*192;
            Point.Position.CellId=Player.CellId;
        }
        const bool Interior=(uint32(Point.Position.CellId)&0xffff)>=0x100;
        const bool Visible=Dungeon?(Interior&&(uint32(Point.Position.CellId)>>16)==(uint32(Player.CellId)>>16)):!Interior;
        Point.Connect=Have&&Visible;
        if(I==0){First=Point;FirstVisible=Visible;}
        if(Visible)Result.Add(Point);
        FString Kind;(*O)->TryGetStringField(TEXT("kind"),Kind);
        Have=Visible&&Kind!=TEXT("portal")&&Kind!=TEXT("recall");
    }
    bool Loop=false,Reverse=false;Route->TryGetBoolField(TEXT("loop_route"),Loop);Route->TryGetBoolField(TEXT("reverse_route"),Reverse);
    if(Loop&&!Reverse&&Count==Points->Num()&&Count>1&&Have&&FirstVisible)
    {First.Connect=true;First.Current=false;Result.Add(First);}
    return Result;
}
FString MarkerLabel(const FString& Name)
{
    return Name.StartsWith(TEXT("Portal to "),ESearchCase::IgnoreCase) ? Name.Mid(10) : Name;
}
TArray<FMapLabel> LayoutLabels(TArray<FMapLabelCandidate> Candidates,FVector2D ViewSize,const TArray<FBox2D>& Reserved)
{
    TArray<FMapLabel> Result;
    if(ViewSize.ContainsNaN()||ViewSize.X<=4||ViewSize.Y<=4)return Result;
    // Stable priorities avoid refresh/iteration-order flicker. Hover wins, then
    // towns and portals; equal names use position rather than container order.
    Candidates.Sort([](const FMapLabelCandidate& A,const FMapLabelCandidate& B)
    {
        if(A.Priority!=B.Priority)return A.Priority>B.Priority;
        const int32 Name=A.Key.Compare(B.Key,ESearchCase::CaseSensitive);
        if(Name!=0)return Name<0;
        if(A.Point.X!=B.Point.X)return A.Point.X<B.Point.X;
        if(A.Point.Y!=B.Point.Y)return A.Point.Y<B.Point.Y;
        return A.Index<B.Index;
    });
    TArray<FBox2D> Occupied=Reserved;
    for(const auto& Candidate:Candidates)
    {
        const auto P=Candidate.Point,S=Candidate.Size;
        if(P.ContainsNaN()||S.ContainsNaN()||S.X<=0||S.Y<=0||P.X<0||P.Y<0||P.X>ViewSize.X||P.Y>ViewSize.Y)continue;
        if(S.X>ViewSize.X-4||S.Y>ViewSize.Y-4)continue;
        // Keep text near its marker. Never clamp many unrelated labels onto
        // the same edge of the map or move them arbitrarily far from the icon.
        const FVector2D Origins[]={P+FVector2D(16,-S.Y*.5),P-FVector2D(S.X+16,S.Y*.5),
            P+FVector2D(-S.X*.5,16),P-FVector2D(S.X*.5,S.Y+16)};
        for(const auto& Origin:Origins)
        {
            const FBox2D Box(Origin,Origin+S);
            if(Box.Min.X<2||Box.Min.Y<2||Box.Max.X>ViewSize.X-2||Box.Max.Y>ViewSize.Y-2)continue;
            const FBox2D Padded(Box.Min-FVector2D(2,2),Box.Max+FVector2D(2,2));
            if(Occupied.ContainsByPredicate([&](const FBox2D& Other){return Padded.Intersect(Other);}))continue;
            Result.Add({Candidate.Index,Origin,S});Occupied.Add(Box);break;
        }
    }
    return Result;
}
TArray<FMapCluster> ClusterMarkers(TArray<FMapClusterPoint> Points,double Radius)
{
    TArray<FMapCluster> Result;
    // Remove invalid values before sorting so the comparator remains a strict
    // ordering even when a caller cannot project a marker into screen space.
    Points.RemoveAll([](const FMapClusterPoint& P){return P.Index<0||P.Point.ContainsNaN();});
    Points.Sort([](const FMapClusterPoint& A,const FMapClusterPoint& B)
    {
        const int32 Name=A.Key.Compare(B.Key,ESearchCase::CaseSensitive);
        if(Name!=0)return Name<0;
        if(A.Point.X!=B.Point.X)return A.Point.X<B.Point.X;
        if(A.Point.Y!=B.Point.Y)return A.Point.Y<B.Point.Y;
        return A.Index<B.Index;
    });
    if(!FMath::IsFinite(Radius)||Radius<=0)
    {
        for(const auto& P:Points)Result.Add({P.Point,{P.Index}});
        return Result;
    }
    // Search only neighboring anchor buckets. Anchors are mutually farther
    // apart than Radius, so dense atlases cannot create an unbounded scan in
    // a bucket. Appending a member never moves an anchor or grows its reach.
    TMap<FIntPoint,TArray<int32>> Grid;
    const double RadiusSquared=Radius*Radius;
    auto Cell=[Radius](FVector2D P)
    {
        // Leave a cell of headroom for the neighbor offsets. Real callers use
        // viewport coordinates, but avoid integer overflow for extreme input.
        return FIntPoint(FMath::FloorToInt(FMath::Clamp(P.X/Radius,double(MIN_int32)+1,double(MAX_int32)-1)),
            FMath::FloorToInt(FMath::Clamp(P.Y/Radius,double(MIN_int32)+1,double(MAX_int32)-1)));
    };
    for(const auto& P:Points)
    {
        const FIntPoint Bucket=Cell(P.Point);
        int32 Best=INDEX_NONE;double Nearest=RadiusSquared;
        for(int32 X=-1;X<=1;++X)for(int32 Y=-1;Y<=1;++Y)
            if(const auto* Nearby=Grid.Find(Bucket+FIntPoint(X,Y)))for(int32 Index:*Nearby)
            {
                const double Distance=(P.Point-Result[Index].Point).SizeSquared();
                if(Distance<=RadiusSquared&&(Best==INDEX_NONE||Distance<Nearest||(Distance==Nearest&&Index<Best)))
                {Best=Index;Nearest=Distance;}
            }
        if(Best==INDEX_NONE)
        {
            Best=Result.Add({P.Point,{}});
            Grid.FindOrAdd(Bucket).Add(Best);
        }
        Result[Best].Indices.Add(P.Index);
    }
    return Result;
}
TArray<FLink> FindCoordinates(const FString& Text)
{
    static const FRegexPattern Pattern(TEXT("(?i)(?<![\\w.+-])([0-9]{1,3}(?:\\.[0-9]{1,6})?)\\s*([NS])\\s*[,; ]\\s*([0-9]{1,3}(?:\\.[0-9]{1,6})?)\\s*([EW])(?![\\w])"));
    TArray<FLink> Result;
    FRegexMatcher Match(Pattern,Text.Left(16384));
    while(Result.Num()<64 && Match.FindNext())
    {
        double NS=FCString::Atod(*Match.GetCaptureGroup(1)),EW=FCString::Atod(*Match.GetCaptureGroup(3));
        if(NS>102 || EW>102)continue;
        if(Match.GetCaptureGroup(2).Equals(TEXT("S"),ESearchCase::IgnoreCase))NS=-NS;
        if(Match.GetCaptureGroup(4).Equals(TEXT("W"),ESearchCase::IgnoreCase))EW=-EW;
        Result.Add({Match.GetMatchBeginning(),Match.GetMatchEnding(),FVector2D(EW,NS)});
    }
    return Result;
}
bool Parse(const FString& Text,FVector2D& Out)
{
    const FString Trimmed=Text.TrimStartAndEnd();const auto Links=FindCoordinates(Trimmed);
    if(Links.Num()!=1 || Links[0].Begin!=0 || Links[0].End!=Trimmed.Len())return false;
    Out=Links[0].Coordinates;return true;
}
FString Format(FVector2D P)
{
    return FString::Printf(TEXT("%.2f%s, %.2f%s"),FMath::Abs(P.Y),P.Y<0?TEXT("S"):TEXT("N"),FMath::Abs(P.X),P.X<0?TEXT("W"):TEXT("E"));
}
FVector2D Coordinates(const FACEPosition& P)
{
    const uint32 Cell=uint32(P.CellId);
    return FVector2D((((Cell>>24)&255)*192.+P.Location.X)/240.-101.95,(((Cell>>16)&255)*192.+P.Location.Y)/240.-101.95);
}
double RelativeBearing(FVector2D From,FVector2D To,FVector2D Forward)
{
    const auto D=To-From;
    return FMath::Atan2(Forward.Y*D.X-Forward.X*D.Y,FVector2D::DotProduct(Forward,D));
}
}
