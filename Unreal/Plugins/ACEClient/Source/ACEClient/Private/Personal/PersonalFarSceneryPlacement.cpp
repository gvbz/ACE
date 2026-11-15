#include "Personal/PersonalFarSceneryPlacement.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "Dat/ACELandblockMeshBuilder.h"

namespace PersonalFarSceneryPlacement
{
FSourcePtr Open(const FString& DatDirectory)
{
	const double Start = FPlatformTime::Seconds();
	auto Source = MakeShared<FSource, ESPMode::ThreadSafe>();
	FACEDatDatabase Portal;
	if (!Portal.Open(DatDirectory / TEXT("client_portal.dat")) || !Source->Cell.Open(DatDirectory / TEXT("client_cell_1.dat")))
	{
		UE_LOG(LogTemp, Warning, TEXT("PersonalFarScenery: DAT files not opened in '%s'; far scenery stays off"), *DatDirectory);
		return nullptr;
	}
	TArray<uint8> Bytes;
	if (Portal.ReadFile(FACELandblockMeshBuilder::RegionFileId, Bytes))
	{
		FACEDatCursor Cursor(Bytes);
		ACEDatUnpack::UnpackRegionSceneTables(Cursor, Source->Tables);
	}
	if (!Source->Tables.bValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("PersonalFarScenery: no scene tables in the region file; far scenery stays off"));
		return nullptr;
	}
	for (const TArray<uint32>& SceneIds : Source->Tables.SceneTypes)
	{
		for (const uint32 SceneId : SceneIds)
		{
			if (Source->Scenes.Contains(SceneId) || !Portal.ReadFile(SceneId, Bytes))
			{
				continue;
			}
			FACEDatCursor Cursor(Bytes);
			FACEDatScene Scene;
			if (ACEDatUnpack::UnpackScene(Cursor, Scene))
			{
				Source->Scenes.Add(SceneId, MoveTemp(Scene));
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("PersonalFarScenery: placement data ready, %d scenes (%.2fs)"), Source->Scenes.Num(), FPlatformTime::Seconds() - Start);
	return Source;
}

bool Place(const FPersonalFarTerrainBake& Bake, const FSource& Source, uint32 LandblockId, TArray<FItem>& OutItems)
{
	OutItems.Reset();
	constexpr int32 VertexDim = FACELandblockMeshBuilder::VertexDim;
	constexpr int32 CellDim = FACELandblockMeshBuilder::CellDim;
	constexpr float CellSize = FACELandblockMeshBuilder::CellSize;
	constexpr float BlockLength = FACELandblockMeshBuilder::LandblockSize;
	const uint32 LB = LandblockId & 0xFFFF0000u;
	const int32 LandblockX = int32(LB >> 24);
	const int32 LandblockY = int32((LB >> 16) & 0xFFu);
	if (!Bake.IsValid() || !Source.Tables.bValid || LandblockX >= FPersonalFarTerrainBake::LandblockDim || LandblockY >= FPersonalFarTerrainBake::LandblockDim
		|| !Source.Cell.Contains(LB | 0xFFFFu))
	{
		return false;
	}

	// The landblock's heights and terrain words in the client's own form, so heights are
	// sampled by the client's own code (FACELandblockMeshBuilder::SampleHeightAc).
	FACEBuiltLandblockMesh MeshData;
	MeshData.LandblockId = LB;
	for (int32 VX = 0; VX < VertexDim; ++VX)
	{
		for (int32 VY = 0; VY < VertexDim; ++VY)
		{
			const int32 X = LandblockX * CellDim + VX, Y = LandblockY * CellDim + VY;
			MeshData.HeightsAc[VX * VertexDim + VY] = Bake.HeightAt(X, Y);
			MeshData.Terrain[VX * VertexDim + VY] = Bake.TerrainWordAt(X, Y);
		}
	}
	MeshData.bHasHeights = true;
	MeshData.bHasTerrain = true;
	const FACEBuiltLandblockMesh* Mesh = &MeshData;

	// Cells with a building get no scenery.
	TSet<int32> BuildingCells;
	FACEDatLandblockInfo Info;
	{
		TArray<uint8> Bytes;
		if (Source.Cell.ReadFile(LB | 0xFFFEu, Bytes))
		{
			FACEDatCursor Cursor(Bytes);
			if (ACEDatUnpack::UnpackLandblockInfo(Cursor, Info))
			{
				for (const FACEDatLandblockBuilding& Building : Info.Buildings)
				{
					const int32 X = FMath::FloorToInt(Building.Origin.X / CellSize);
					const int32 Y = FMath::FloorToInt(Building.Origin.Y / CellSize);
					if (X >= 0 && X < CellDim && Y >= 0 && Y < CellDim) BuildingCells.Add(X * CellDim + Y);
				}
			}
		}
	}

	// From here on: the rules of UACEDatSubsystem::CollectRegionScenery, kept line for line
	// (same types, same order of operations), so both give the same numbers.
	const uint32 BlockX = (LB >> 24) * 8u;
	const uint32 BlockY = ((LB >> 16) & 0xFFu) * 8u;

	auto PseudoRand = [](uint32 A) -> float
	{
		return static_cast<float>(A) * 2.3283064e-10f;
	};

	auto GetRoadBits = [&](int32 Vx, int32 Vy) -> uint32
	{
		Vx = FMath::Clamp(Vx, 0, CellDim);
		Vy = FMath::Clamp(Vy, 0, CellDim);
		const int32 Idx = Vx * VertexDim + Vy;
		return (Idx >= 0 && Idx < 81) ? (static_cast<uint32>(Mesh->Terrain[Idx]) & 0x3u) : 0u;
	};

	auto OnRoad = [&](float AcX, float AcY) -> bool
	{
		constexpr float RoadWidth = 5.f;
		constexpr float TileLength = CellSize; // 24
		const int32 X = FMath::FloorToInt(AcX / TileLength);
		const int32 Y = FMath::FloorToInt(AcY / TileLength);
		const float RMin = RoadWidth;
		const float RMax = TileLength - RoadWidth;
		const uint32 R0 = GetRoadBits(X, Y);
		const uint32 R1 = GetRoadBits(X, Y + 1);
		const uint32 R2 = GetRoadBits(X + 1, Y);
		const uint32 R3 = GetRoadBits(X + 1, Y + 1);
		if (R0 == 0 && R1 == 0 && R2 == 0 && R3 == 0)
		{
			return false;
		}
		const float Dx = AcX - X * TileLength;
		const float Dy = AcY - Y * TileLength;
		if (R0 > 0)
		{
			if (R1 > 0)
			{
				if (R2 > 0)
				{
					return (R3 > 0) ? true : (Dx < RMin || Dy < RMin);
				}
				return (R3 > 0) ? (Dx < RMin || Dy > RMax) : (Dx < RMin);
			}
			if (R2 > 0)
			{
				return (R3 > 0) ? (Dx > RMax || Dy < RMin) : (Dy < RMin);
			}
			return (R3 > 0) ? (FMath::Abs(Dx - Dy) < RMin) : (Dx + Dy < RMin);
		}
		if (R1 > 0)
		{
			if (R2 > 0)
			{
				return (R3 > 0) ? (Dx > RMax || Dy > RMax) : (FMath::Abs(Dx + Dy - TileLength) < RMin);
			}
			return (R3 > 0) ? (Dy > RMax) : (TileLength + Dx - Dy < RMin);
		}
		if (R2 > 0)
		{
			return (R3 > 0) ? (Dx > RMax) : (TileLength - Dx + Dy < RMin);
		}
		return (R3 > 0) && (TileLength * 2.f - Dx - Dy < RMin);
	};

	for (int32 CellX = 0; CellX < CellDim; ++CellX)
	{
		for (int32 CellY = 0; CellY < CellDim; ++CellY)
		{
			const int32 TerrainIdx = CellX * VertexDim + CellY;
			const uint16 Terrain = Mesh->Terrain[TerrainIdx];

			const uint32 TerrainType = (Terrain >> 2) & 0x1F;
			const uint32 SceneTypeLocal = Terrain >> 11;
			if (!Source.Tables.TerrainSceneTypeIndices.IsValidIndex(static_cast<int32>(TerrainType)))
			{
				continue;
			}
			const TArray<uint32>& TypeScenes = Source.Tables.TerrainSceneTypeIndices[TerrainType];
			if (!TypeScenes.IsValidIndex(static_cast<int32>(SceneTypeLocal)))
			{
				continue;
			}
			const uint32 SceneInfoIdx = TypeScenes[SceneTypeLocal];
			if (!Source.Tables.SceneTypes.IsValidIndex(static_cast<int32>(SceneInfoIdx)))
			{
				continue;
			}
			const TArray<uint32>& Scenes = Source.Tables.SceneTypes[SceneInfoIdx];
			if (Scenes.Num() == 0)
			{
				continue;
			}

			const uint32 GlobalCellX = static_cast<uint32>(CellX) + BlockX;
			const uint32 GlobalCellY = static_cast<uint32>(CellY) + BlockY;
			const uint32 CellMat = GlobalCellY * (712977289u * GlobalCellX + 1813693831u)
				- 1109124029u * GlobalCellX + 2139937281u;
			const float Offset = PseudoRand(CellMat);
			int32 SceneIdx = static_cast<int32>(Scenes.Num() * Offset);
			if (SceneIdx >= Scenes.Num())
			{
				SceneIdx = 0;
			}
			const uint32 SceneId = Scenes[SceneIdx];

			const FACEDatScene* Scene = Source.Scenes.Find(SceneId);
			if (!Scene)
			{
				continue;
			}

			const uint32 CellXMat = static_cast<uint32>(-1109124029) * GlobalCellX;
			const uint32 CellYMat = 1813693831u * GlobalCellY;
			const uint32 ComboMat = 1360117743u * GlobalCellX * GlobalCellY + 1888038839u;

			for (int32 J = 0; J < Scene->Objects.Num(); ++J)
			{
				const FACEDatSceneObjectDesc& Obj = Scene->Objects[J];
				if (Obj.WeenieObj != 0 || Obj.ObjId == 0)
				{
					continue;
				}
				const float Noise = PseudoRand(CellXMat + CellYMat - ComboMat * static_cast<uint32>(23399 + J));
				if (Noise >= Obj.Freq)
				{
					continue;
				}

				// ObjectDesc.Displace
				FVector Loc = Obj.Origin;
				if (Obj.DisplaceX > 0.f)
				{
					Loc.X = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 45773u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX) * Obj.DisplaceX + Obj.Origin.X;
				}
				if (Obj.DisplaceY > 0.f)
				{
					Loc.Y = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 72719u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX) * Obj.DisplaceY + Obj.Origin.Y;
				}
				const float Quadrant = PseudoRand(1813693831u * GlobalCellY
					- GlobalCellX * (1870387557u * GlobalCellY + 1109124029u) - 402451965u);
				FVector Displaced = Loc;
				if (Quadrant >= 0.75f) { Displaced = FVector(Loc.Y, -Loc.X, Loc.Z); }
				else if (Quadrant >= 0.5f) { Displaced = FVector(-Loc.X, -Loc.Y, Loc.Z); }
				else if (Quadrant >= 0.25f) { Displaced = FVector(-Loc.Y, Loc.X, Loc.Z); }

				const float Lx = static_cast<float>(CellX) * CellSize + Displaced.X;
				const float Ly = static_cast<float>(CellY) * CellSize + Displaced.Y;
				if (Lx < 0.f || Ly < 0.f || Lx >= BlockLength || Ly >= BlockLength || OnRoad(Lx, Ly))
				{
					continue;
				}

				if (BuildingCells.Contains(FMath::FloorToInt(Lx / CellSize) * CellDim + FMath::FloorToInt(Ly / CellSize)))
				{
					continue;
				}

				float ZAc = 0.f;
				if (!FACELandblockMeshBuilder::SampleHeightAc(*Mesh, Lx, Ly, ZAc))
				{
					continue;
				}

				// Approximate slope from neighboring heights.
				float Zxp = ZAc, Zxm = ZAc, Zyp = ZAc, Zym = ZAc;
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, FMath::Min(Lx + 1.f, BlockLength - 0.01f), Ly, Zxp);
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, FMath::Max(Lx - 1.f, 0.f), Ly, Zxm);
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, Lx, FMath::Min(Ly + 1.f, BlockLength - 0.01f), Zyp);
				FACELandblockMeshBuilder::SampleHeightAc(*Mesh, Lx, FMath::Max(Ly - 1.f, 0.f), Zym);
				const FVector Normal = FVector(Zxm - Zxp, Zym - Zyp, 2.f).GetSafeNormal();
				if (Normal.Z < Obj.MinSlope || Normal.Z > Obj.MaxSlope)
				{
					continue;
				}

				float Scale = Obj.MaxScale;
				if (!FMath::IsNearlyEqual(Obj.MinScale, Obj.MaxScale))
				{
					const float T = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 32593u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX);
					Scale = FMath::Pow(Obj.MaxScale / Obj.MinScale, T) * Obj.MinScale;
				}

				FQuat Orient = Obj.Orientation;
				if (Obj.Align != 0)
				{
					// ObjectDesc.ObjAlign: AC get_heading, (450 - atan2(y, x) in degrees) % 360
					const FVector NegN = -Normal;
					const float AtanDeg = FMath::RadiansToDegrees(FMath::Atan2(NegN.Y, NegN.X));
					float HeadingDeg = FMath::Fmod(450.f - AtanDeg, 360.f);
					if (HeadingDeg < 0.f)
					{
						HeadingDeg += 360.f;
					}
					Orient = FQuat(FVector::UpVector, FMath::DegreesToRadians(HeadingDeg));
				}
				else if (Obj.MaxRotation > 0.f)
				{
					const float Degrees = PseudoRand(1813693831u * GlobalCellY
						- (static_cast<uint32>(J) + 63127u) * (1360117743u * GlobalCellY * GlobalCellX + 1888038839u)
						- 1109124029u * GlobalCellX) * Obj.MaxRotation;
					Orient = FQuat(FVector::UpVector, FMath::DegreesToRadians(Degrees)) * Orient;
				}

				FItem Item;
				Item.SetupId = Obj.ObjId;
				Item.OriginAc = FVector(Lx, Ly, ZAc + Displaced.Z);
				Item.Orientation = Orient;
				Item.Scale = Scale;
				OutItems.Add(Item);
			}
		}
	}

	// The stabs and buildings of the info file, as AACELandblockActor::QueueScenery takes them.
	for (const FACEDatLandblockStab& Stab : Info.Objects)
	{
		if (Stab.Id == 0)
		{
			continue;
		}
		FItem Item;
		Item.SetupId = Stab.Id;
		Item.PlacementId = TypePlacement(TypeKey(Stab.Id, EKind::Stab));
		Item.OriginAc = FVector(Stab.Origin);
		Item.Orientation = FQuat(Stab.Orientation);
		Item.Kind = EKind::Stab;
		OutItems.Add(Item);
	}
	for (const FACEDatLandblockBuilding& Building : Info.Buildings)
	{
		if (Building.ModelId == 0)
		{
			continue;
		}
		FItem Item;
		Item.SetupId = Building.ModelId;
		Item.OriginAc = FVector(Building.Origin);
		Item.Orientation = FQuat(Building.Orientation);
		Item.Kind = EKind::Building;
		OutItems.Add(Item);
	}
	return true;
}
}
