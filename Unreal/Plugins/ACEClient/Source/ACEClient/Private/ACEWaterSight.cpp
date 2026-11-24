#include "ACEWaterSight.h"
#include "ACEDatSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACELandblockActor.h"
#include "ACETerrainChunkActor.h"
#include "Dat/ACELandblockMeshBuilder.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
bool IntersectBed(UACEDatSubsystem& Dat, const FVector& Start, const FVector& Delta,
    float Radius, float Scale, double Enter, double Exit, double& SolidDistance)
{
    // Depth is constant over each retail half-cell. Height is planar on each
    // side of the cell diagonal. Partition at both possible diagonal families.
    TArray<double,TInlineAllocator<32>> Cuts{Enter, Exit};
    const double Cell = FACELandblockMeshBuilder::CellSize * Scale;
    auto AddCrossings = [&](double Origin, double Direction, double Spacing)
    {
        if (FMath::IsNearlyZero(Direction)) return;
        const double A = Origin + Enter * Direction, B = Origin + Exit * Direction;
        const int32 Last = FMath::CeilToInt(FMath::Max(A, B) / Spacing);
        for (int32 I = FMath::FloorToInt(FMath::Min(A, B) / Spacing) + 1; I < Last; ++I)
            Cuts.Add((I * Spacing - Origin) / Direction);
    };
    AddCrossings(Start.X, Delta.X, Cell / 2);
    AddCrossings(Start.Y, Delta.Y, Cell / 2);
    AddCrossings(Start.X + Start.Y, Delta.X + Delta.Y, Cell);
    AddCrossings(Start.X - Start.Y, Delta.X - Delta.Y, Cell);
    Cuts.Sort();
    SolidDistance = MAX_dbl;
    for (int32 I = 1; I < Cuts.Num(); ++I)
    {
        const double A = Cuts[I-1], B = Cuts[I];
        if (B - A <= UE_DOUBLE_SMALL_NUMBER) continue;
        FVector Floor = Start + Delta * ((A + B) / 2), Normal;
        float Z;
        if (!Dat.SampleOutdoorGroundZ(Floor.X, Floor.Y, Scale, Z, &Normal)) return false;
        Floor.Z = Z;
        const double From = FVector::DotProduct(Start + A*Delta - Floor, Normal) - Radius;
        const double To = FVector::DotProduct(Start + B*Delta - Floor, Normal) - Radius;
        if (From <= 0 || To <= 0)
        {
            const double T = From <= 0 ? A : FMath::Lerp(A, B, From / (From - To));
            SolidDistance = T * Delta.Size();
            break;
        }
    }
    return true;
}
}

bool ACEWaterSight::BlockedFromWithinWater(UWorld& World, const FVector& Start, const FVector& End, float Radius)
{
    auto* GI = World.GetGameInstance();
    auto* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
    if (!Dat) return false;
    constexpr float Scale = 100.f; // network positions and combat query coordinates are in cm
    const double BlockSize = FACELandblockMeshBuilder::LandblockSize * Scale;
    const int32 X=FMath::FloorToInt(-Start.X/BlockSize), Y=FMath::FloorToInt(Start.Y/BlockSize);
    if (X<0 || X>255 || Y<0 || Y>255) return false;
    const auto* Block = Dat->FindLandblockMesh((uint32(X)<<24) | (uint32(Y)<<16));
    if (!Block || !Block->BlockWaterType) return false;
    // Indoor rooms can share XY coordinates with the outdoor water table.
    const auto* Client = GI->GetSubsystem<UACEClientSubsystem>();
    if (Client && (Client->GetPlayerPosition().CellId & 0xffff) >= 0x100) return false;
    const float Depth = Dat->GetOutdoorWaterDepthCm(Start.X,Start.Y,Scale);
    float Bed;
    if (Depth<=0 || !Dat->SampleOutdoorGroundZ(Start.X,Start.Y,Scale,Bed)
        || Start.Z < Bed || Start.Z > Bed+Depth+Radius) return false;
    // A horizontal ray starting inside water may never cross the rendered
    // sheet. The bottom can still rise into its path before the next surface hit.
    double Distance;
    return !IntersectBed(*Dat,Start,End-Start,Radius,Scale,0,1,Distance) || Distance <= FVector::Distance(Start,End);
}

bool ACEWaterSight::RefineTerrainHit(UWorld& World, const FHitResult& Hit, const FVector& Start,
    const FVector& End, float Radius, double& SolidDistance)
{
    const auto* Component = Hit.GetComponent();
    static const FName TerrainTag(TEXT("ACEOutdoorTerrain"));
    if (!Component || !(Component->ComponentHasTag(TerrainTag) || (Hit.GetActor() && Hit.GetActor()->ActorHasTag(TerrainTag)))) return false;
    auto* GI = World.GetGameInstance();
    auto* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
    float Scale = 100.f;
    if (auto* Block = Cast<AACELandblockActor>(Hit.GetActor())) Scale = Block->WorldScale;
    else if (auto* Chunk = Cast<AACETerrainChunkActor>(Hit.GetActor())) Scale = Chunk->WorldScale;
    if (!Dat || Dat->GetOutdoorWaterDepthCm(Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Scale) <= 0) return false;

    // The same mesh can hold water, a dry island and the opposite bank. Intersect
    // the physical bed along this component's entire XY span before ignoring it.
    const FVector Delta = End - Start;
    const FBox Box = Component->Bounds.GetBox();
    double Enter = 0, Exit = 1;
    for (int Axis = 0; Axis < 2; ++Axis)
    {
        if (FMath::IsNearlyZero(Delta[Axis]))
        {
            if (Start[Axis] < Box.Min[Axis] || Start[Axis] > Box.Max[Axis]) return false;
            continue;
        }
        double A = (Box.Min[Axis] - Start[Axis]) / Delta[Axis];
        double B = (Box.Max[Axis] - Start[Axis]) / Delta[Axis];
        if (A > B) Swap(A, B);
        Enter = FMath::Max(Enter, A);
        Exit = FMath::Min(Exit, B);
    }
    if (Exit <= Enter) return false;
    return IntersectBed(*Dat,Start,Delta,Radius,Scale,Enter,Exit,SolidDistance);
}
