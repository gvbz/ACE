#pragma once

#include "Mods/ACEPluginSight.h"
#include "ACEDatSubsystem.h"
#include "Dat/ACEEnvCellMeshBuilder.h"
#include "Engine/GameInstance.h"

// Route presentation/rejoin queries follow a continuous floor, not the chord
// between recorded heights. Actual movement remains in the player physics path.
// Cache loaded physics meshes once per bounded query batch. Visibility and
// collision residency must not change the shape of a route through adjacent cells.
class FACEPluginRouteGround
{
public:
    explicit FACEPluginRouteGround(UWorld& InWorld, int32 Budget = 6144)
        : World(InWorld), Remaining(Budget), Params(SCENE_QUERY_STAT(UCMRouteGround), true)
    {
        auto* Dat=World.GetGameInstance()?World.GetGameInstance()->GetSubsystem<UACEDatSubsystem>():nullptr;
        if (!Dat) return;
        for (TActorIterator<AACEEnvCellActor> It(&World); It; ++It)
            if (const auto* Built=Dat->FindEnvCellMesh(uint32(It->EnvCellId),It->WorldScale))
                for (const auto& Section:Built->CollisionSections)
                {
                    if (Section.IsEmpty() || (Section.bFullyTransparent&&!Section.bClipMap)) continue;
                    FSurface Surface;Surface.Section=&Section;
                    Surface.Transform=Built->GetCellLocalToLandblock(It->WorldScale)*It->GetActorTransform();
                    Surface.LocalBounds=FBox(Section.Vertices);
                    Surface.WorldBounds=Surface.LocalBounds.TransformBy(Surface.Transform);
                    Rooms.Add(Surface);
                }
    }

    bool Sample(const FVector& Near, FVector& Floor)
    {
        if (Remaining-- <= 0) return false;
        constexpr double Reach = 90.; // 50cm along a retail walkable slope + stair tolerance
        const FVector Start = Near + FVector(0,0,Reach), End = Near - FVector(0,0,Reach);
        double Best = Reach + .01;
        auto Accept = [&](const FVector& Point, const FVector& Normal)
        {
            const double Difference = FMath::Abs(Point.Z-Near.Z);
            if (Normal.Z >= .664174 && Difference < Best) { Best=Difference;Floor=Point; }
        };
        FHitResult Hit;
        if (World.LineTraceSingleByObjectType(Hit,Start,End,FCollisionObjectQueryParams(ECC_WorldStatic),Params))
            Accept(Hit.ImpactPoint,Hit.ImpactNormal);
        for (const auto& Surface : Rooms)
        {
            if (!FMath::LineBoxIntersection(Surface.WorldBounds,Start,End,End-Start)) continue;
            const FTransform& Transform=Surface.Transform;
            const FVector A=Transform.InverseTransformPosition(Start),B=Transform.InverseTransformPosition(End);
            if (!FMath::LineBoxIntersection(Surface.LocalBounds,A,B,B-A)) continue;
            const auto& V=Surface.Section->Vertices;const auto& I=Surface.Section->Triangles;
            for (int32 T=0;T+2<I.Num();T+=3)
            {
                if (!V.IsValidIndex(I[T])||!V.IsValidIndex(I[T+1])||!V.IsValidIndex(I[T+2])) continue;
                FVector Point,Normal;
                if (FMath::SegmentTriangleIntersection(A,B,V[I[T]],V[I[T+1]],V[I[T+2]],Point,Normal))
                    Accept(Transform.TransformPosition(Point),Transform.TransformVectorNoScale(Normal));
            }
        }
        return Best <= Reach;
    }

    bool Path(const FVector& From,const FVector& To,TArray<FVector>& Points)
    {
        Points.Reset();FVector Last;
        if (!Sample(From,Last)) return false;
        Points.Add(Last);
        const int32 Steps=FMath::Max(1,FMath::CeilToInt(FVector::Dist2D(From,To)/50.));
        if (Steps>800 || Steps>Remaining) return false;
        for (int32 I=1;I<=Steps;++I)
        {
            FVector Near=FMath::Lerp(From,To,double(I)/Steps);Near.Z=Last.Z;
            if (!Sample(Near,Last)) return false;
            Points.Add(Last);
        }
        // Never join a different floor solely because its XY matches.
        return FMath::Abs(Last.Z-To.Z)<=90.;
    }

    // Reserve a whole candidate before testing it. Exhausting a shared batch
    // budget halfway along a distant path does not mean that path is blocked.
    bool CanQueryPath(const FVector& From,const FVector& To) const
    {
        const int32 Steps=FMath::Max(1,FMath::CeilToInt(FVector::Dist2D(From,To)/50.));
        return Steps<=800 && Steps+1<=Remaining;
    }

    bool Reachable(const FVector& From,const FVector& To,const FACEPluginSightQuery& Sight,float Clearance=0.f)
    {
        TArray<FVector> Points;
        if (!Path(From,To,Points)) return false;
        for (int32 I=1;I<Points.Num();++I)
            if (!Sight.Clear(Points[I-1]+FVector(0,0,60),Points[I]+FVector(0,0,60),Clearance)) return false;
        return true;
    }
private:
    UWorld& World;
    int32 Remaining;
    FCollisionQueryParams Params;
    struct FSurface
    {
        const FACEBuiltMeshSection* Section=nullptr;
        FTransform Transform;
        FBox LocalBounds,WorldBounds;
    };
    TArray<FSurface,TInlineAllocator<64>> Rooms;
};
