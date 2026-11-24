#pragma once

#include "ACEVisibleObjectPick.h"

// One context per decision, shared by the bounded monster/corpse lists. Use
// physical scenery, not selection capsules: doors intentionally ignore Camera.
class FACEPluginSightQuery
{
public:
    FACEPluginSightQuery(UWorld& InWorld, APawn* Self)
        : World(InWorld), Params(SCENE_QUERY_STAT(ACEPluginSight), true)
    {
        if (Self) Params.AddIgnoredActor(Self);
        for (TActorIterator<AACEWorldEntityActor> It(&World); It; ++It)
        {
            if (It->GetACEGuid()) Bodies.Add(It->GetACEGuid(), *It);
            if ((It->ItemType & ACEItemType::Creature) || It->IsCorpse() || It->IsAttachedToParent())
                Params.AddIgnoredActor(*It);
        }
        for (TActorIterator<AACEEnvCellActor> It(&World); It; ++It)
            if (It->CellMesh) Rooms.Add(It->CellMesh);
    }

    FVector Location(const FACEWorldObject& Object) const
    {
        // TargetManager reads the current physics frame, not the last F748.
        // A walking target may already have rounded a corner since that packet.
        if (const auto* Body = Bodies.Find(Object.Guid); Body && IsValid(*Body)) return (*Body)->GetActorLocation();
        return Object.Position.ToUnrealLocation();
    }

    bool Clear(const FVector& Start, const FVector& End, float Clearance = 0.f, bool TargetThroughWater = false) const
    {
        if (TargetThroughWater)
        {
            if (ACEWaterSight::BlockedFromWithinWater(World, Start, End, Clearance)) return false;
            FHitResult Hit;
            const auto Blocked = [&](const FCollisionQueryParams& Query)
            {
                return Clearance > 0.f
                    ? World.SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(Clearance), Query)
                    : World.LineTraceSingleByChannel(Hit, Start, End, ECC_Pawn, Query);
            };
            if (Blocked(Params))
            {
                FCollisionQueryParams Query(Params);
                do
                {
                    double BedDistance;
                    if (!ACEWaterSight::RefineTerrainHit(World, Hit, Start, End, Clearance, BedDistance)
                        || BedDistance <= FVector::Distance(Start, End)) return false;
                    Query.AddIgnoredComponent(Hit.GetComponent());
                } while (Blocked(Query));
            }
        }
        else if (Clearance > 0.f
            ? World.SweepTestByChannel(Start, End, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(Clearance), Params)
            : World.LineTraceTestByChannel(Start, End, ECC_Pawn, Params)) return false;
        // Loaded cells can have geometry before movement PhysicsBSP is active.
        // Camera/portal culling must not make their walls transparent to combat.
        double Distance = FVector::Distance(Start, End);
        for (auto* Mesh : Rooms)
        {
            if (!Mesh->IsRegistered() || !FMath::LineBoxIntersection(Mesh->Bounds.GetBox().ExpandBy(Clearance), Start, End, End-Start)) continue;
            if (Clearance <= 0.f)
            {
                if (ACEVisibleObjectPick::TraceMesh(*Mesh, Start, End, Distance, nullptr, true, true)) return false;
                continue;
            }
            // The same finite-width query must work before room collision is
            // cooked and while the room is culled. A center ray misses edges.
            const auto Transform = Mesh->GetComponentTransform();
            const FBox Path = FBox(Start, Start).ExpandBy(Clearance) + FBox(End, End).ExpandBy(Clearance);
            for (int32 SectionIndex = 0; SectionIndex < Mesh->GetNumSections(); ++SectionIndex)
            {
                const auto* Section = Mesh->GetProcMeshSection(SectionIndex);
                if (!Section || !Path.Intersect(Section->SectionLocalBox.TransformBy(Transform))) continue;
                const auto& V = Section->ProcVertexBuffer; const auto& I = Section->ProcIndexBuffer;
                for (int32 Index = 0; Index + 2 < I.Num(); Index += 3)
                {
                    if (!V.IsValidIndex(I[Index]) || !V.IsValidIndex(I[Index+1]) || !V.IsValidIndex(I[Index+2])) continue;
                    const FVector A = Transform.TransformPosition(V[I[Index]].Position);
                    const FVector B = Transform.TransformPosition(V[I[Index+1]].Position);
                    const FVector C = Transform.TransformPosition(V[I[Index+2]].Position);
                    FBox Bounds(ForceInit); Bounds += A; Bounds += B; Bounds += C;
                    if (!FMath::LineBoxIntersection(Bounds.ExpandBy(Clearance), Start, End, End-Start)) continue;
                    FVector Point, Normal;
                    if (FMath::SegmentTriangleIntersection(Start, End, A, B, C, Point, Normal)) return false;
                    const double RadiusSq = FMath::Square(double(Clearance));
                    if (FVector::DistSquared(Start, FMath::ClosestPointOnTriangleToPoint(Start, A, B, C)) <= RadiusSq
                        || FVector::DistSquared(End, FMath::ClosestPointOnTriangleToPoint(End, A, B, C)) <= RadiusSq) return false;
                    for (const auto& Edge : {TPair<FVector,FVector>(A,B), TPair<FVector,FVector>(B,C), TPair<FVector,FVector>(C,A)})
                    {
                        FMath::SegmentDistToSegmentSafe(Start, End, Edge.Key, Edge.Value, Point, Normal);
                        if (FVector::DistSquared(Point, Normal) <= RadiusSq) return false;
                    }
                }
            }
        }
        return true;
    }
private:
    UWorld& World;
    FCollisionQueryParams Params;
    TMap<int32, const AACEWorldEntityActor*> Bodies;
    TArray<UProceduralMeshComponent*, TInlineAllocator<64>> Rooms;
};
