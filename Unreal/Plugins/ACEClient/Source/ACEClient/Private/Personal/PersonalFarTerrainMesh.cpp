#include "Personal/PersonalFarTerrainMesh.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "ACERuntimeOptions.h"
#include "ACETypes.h"
#include "Dat/ACELandblockMeshBuilder.h"

namespace
{
	int32 GridMod(int32 Value, int32 Step) { return ((Value % Step) + Step) % Step; }
	int32 GridFloor(int32 Value, int32 Step) { return Value - GridMod(Value, Step); }
	int32 GridCeil(int32 Value, int32 Step) { return -GridFloor(-Value, Step); }

	constexpr int32 FarCells = FPersonalFarTerrainBake::CellsPerLandblock;

	/** Builds the levels from the inside out. Vertices on a border between two levels are shared. */
	struct FFarTerrainShellBuilder
	{
		struct FGrid
		{
			FPersonalFarTerrainMesh::FLevel Level;
			bool bLast = false;
			int32 CountY = 0;
			TArray<int32> VertexIndex;
		};

		const FPersonalFarTerrainBake& Bake;
		FPersonalFarTerrainMesh& Out;
		FIntPoint CenterCell;
		float WorldScale;
		TArray<FGrid> Grids;

		int32 Vertex(int32 K, int32 X, int32 Y)
		{
			if (K > 0)
			{
				const FPersonalFarTerrainMesh::FLevel& Inner = Grids[K - 1].Level;
				if (X >= Inner.Min.X && X <= Inner.Max.X && Y >= Inner.Min.Y && Y <= Inner.Max.Y)
				{
					return Vertex(K - 1, X, Y);
				}
			}
			FGrid& Grid = Grids[K];
			const FPersonalFarTerrainMesh::FLevel& Level = Grid.Level;
			const int32 Step = Level.Step;
			int32& Slot = Grid.VertexIndex[((X - Level.Min.X) / Step) * Grid.CountY + (Y - Level.Min.Y) / Step];
			if (Slot >= 0)
			{
				return Slot;
			}
			float Height = Bake.HeightAt(K, X, Y);
			const bool bOnX = X == Level.Min.X || X == Level.Max.X;
			const bool bOnY = Y == Level.Min.Y || Y == Level.Max.Y;
			if (!Grid.bLast && (bOnX || bOnY))
			{
				// The border belongs to the coarser level outside it. A vertex that
				// level does not have sits on the straight edge between its neighbors.
				const int32 Coarse = Step * 2;
				if (bOnX && GridMod(Y, Coarse) != 0)
				{
					Height = 0.5f * (Bake.HeightAt(K + 1, X, Y - Step) + Bake.HeightAt(K + 1, X, Y + Step));
				}
				else if (bOnY && GridMod(X, Coarse) != 0)
				{
					Height = 0.5f * (Bake.HeightAt(K + 1, X - Step, Y) + Bake.HeightAt(K + 1, X + Step, Y));
				}
				else
				{
					Height = Bake.HeightAt(K + 1, X, Y);
				}
			}
			const float Run = 2.f * Step * PersonalFarTerrainMesh::CellSizeAc;
			const float SlopeX = (Bake.HeightAt(K, X + Step, Y) - Bake.HeightAt(K, X - Step, Y)) / Run;
			const float SlopeY = (Bake.HeightAt(K, X, Y + Step) - Bake.HeightAt(K, X, Y - Step)) / Run;
			Slot = Out.Vertices.Add(FACEPosition::AceVectorToUnreal(FVector(
				(X - CenterCell.X) * PersonalFarTerrainMesh::CellSizeAc,
				(Y - CenterCell.Y) * PersonalFarTerrainMesh::CellSizeAc, Height), WorldScale));
			// AC normal (-SlopeX, -SlopeY, 1); AceVectorToUnreal flips X.
			Out.Normals.Add(FVector(SlopeX, -SlopeY, 1.f).GetSafeNormal());
			Out.UVs.Add(FVector2D((X + 0.5f) / FPersonalFarTerrainBake::ColorTextureSize, (Y + 0.5f) / FPersonalFarTerrainBake::ColorTextureSize));
			Out.Corners.Add(FIntPoint(X, Y));
			return Slot;
		}
	};
}

namespace PersonalFarTerrainMesh
{
FVector Origin(FIntPoint Center, float WorldScale)
{
	return FACEPosition::AceVectorToUnreal(FVector(Center.X * double(LandblockSizeAc), Center.Y * double(LandblockSizeAc), 0.), WorldScale);
}

int32 RadiusSetting()
{
	return FMath::Clamp(FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("PersonalFarTerrainRadius"))), 0, MaxRadius);
}

void Build(const FPersonalFarTerrainBake& Bake, FIntPoint Center, int32 Radius,
	const TSet<FIntPoint>& Holes, int32 HoleRadius, float WorldScale, FPersonalFarTerrainMesh& Out)
{
	Out = FPersonalFarTerrainMesh();
	Out.Center = Center;
	Radius = FMath::Min(Radius, MaxRadius);
	if (Radius <= 0 || !Bake.IsValid())
	{
		return;
	}

	FFarTerrainShellBuilder Builder{Bake, Out, Center * FarCells, WorldScale};
	// The full-detail level always reaches past the holes, so only it borders loaded terrain.
	const int32 FineLimit = FMath::Max(FineRings, HoleRadius + 1);
	int32 VertexCount = 0;
	for (int32 K = 0; K < FPersonalFarTerrainBake::LevelCount; ++K)
	{
		const int32 Step = 1 << K;
		const int32 Limit = FineLimit << K;
		FFarTerrainShellBuilder::FGrid Grid;
		Grid.bLast = Radius <= Limit || K == FPersonalFarTerrainBake::LevelCount - 1;
		const int32 Half = Grid.bLast ? Radius : Limit;
		// A level that has a coarser one outside it ends on that coarser grid.
		const int32 Snap = Grid.bLast ? Step : Step * 2;
		Grid.Level.Step = Step;
		Grid.Level.Min = FIntPoint(GridFloor((Center.X - Half) * FarCells, Snap), GridFloor((Center.Y - Half) * FarCells, Snap));
		Grid.Level.Max = FIntPoint(GridCeil((Center.X + 1 + Half) * FarCells, Snap), GridCeil((Center.Y + 1 + Half) * FarCells, Snap));
		Grid.CountY = (Grid.Level.Max.Y - Grid.Level.Min.Y) / Step + 1;
		const int32 Count = ((Grid.Level.Max.X - Grid.Level.Min.X) / Step + 1) * Grid.CountY;
		Grid.VertexIndex.Init(INDEX_NONE, Count);
		VertexCount += Count;
		const bool bLast = Grid.bLast;
		Builder.Grids.Add(MoveTemp(Grid));
		if (bLast)
		{
			break;
		}
	}
	Out.Vertices.Reserve(VertexCount);
	Out.Normals.Reserve(VertexCount);
	Out.UVs.Reserve(VertexCount);
	Out.Corners.Reserve(VertexCount);
	Out.Triangles.Reserve(VertexCount * 6);

	for (int32 K = 0; K < Builder.Grids.Num(); ++K)
	{
		const FPersonalFarTerrainMesh::FLevel Level = Builder.Grids[K].Level;
		const FPersonalFarTerrainMesh::FLevel* Inner = K > 0 ? &Builder.Grids[K - 1].Level : nullptr;
		const int32 Step = Level.Step;
		for (int32 X = Level.Min.X; X < Level.Max.X; X += Step)
		{
			for (int32 Y = Level.Min.Y; Y < Level.Max.Y; Y += Step)
			{
				if (Inner && X >= Inner->Min.X && X < Inner->Max.X && Y >= Inner->Min.Y && Y < Inner->Max.Y)
				{
					continue;
				}
				bool bSplit = true;
				if (K == 0)
				{
					const FIntPoint Block(GridFloor(X, FarCells) / FarCells, GridFloor(Y, FarCells) / FarCells);
					if (Holes.Contains(Block))
					{
						continue;
					}
					// Full-detail quads use the diagonal the loaded terrain uses.
					if (Block.X >= 0 && Block.Y >= 0 && Block.X < FPersonalFarTerrainBake::LandblockDim && Block.Y < FPersonalFarTerrainBake::LandblockDim)
					{
						bSplit = FACELandblockMeshBuilder::GetSplitDir((uint32(Block.X) << 24) | (uint32(Block.Y) << 16),
							X - Block.X * FarCells, Y - Block.Y * FarCells);
					}
				}
				const int32 LL = Builder.Vertex(K, X, Y);
				const int32 LR = Builder.Vertex(K, X + Step, Y);
				const int32 TL = Builder.Vertex(K, X, Y + Step);
				const int32 TR = Builder.Vertex(K, X + Step, Y + Step);
				if (bSplit)
				{
					Out.Triangles.Append({TL, LL, LR, TL, LR, TR});
				}
				else
				{
					Out.Triangles.Append({TR, LL, LR, TR, TL, LL});
				}
			}
		}
		Out.Levels.Add(Level);
	}
}
}
