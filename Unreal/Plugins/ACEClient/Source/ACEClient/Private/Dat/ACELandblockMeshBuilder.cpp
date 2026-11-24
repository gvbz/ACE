#include "Dat/ACELandblockMeshBuilder.h"
#include "Dat/ACELandSurfaceAtlas.h"
#include "Dat/ACEDatCursor.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACEPCodeTileCache.h"
#include "ACETypes.h"

namespace
{
	uint8 GetTerrainType(uint16 RawTerrain) { return static_cast<uint8>((RawTerrain >> 2) & 0x1F); }
	uint8 GetRoadType(uint16 RawTerrain) { return static_cast<uint8>(RawTerrain & 0x3); }

	const FLinearColor& GetTerrainTypeColor(uint8 TerrainType)
	{
		static const FLinearColor Colors[32] =
		{
			FLinearColor(0.42f, 0.40f, 0.38f),
			FLinearColor(0.20f, 0.45f, 0.12f),
			FLinearColor(0.75f, 0.85f, 0.90f),
			FLinearColor(0.12f, 0.50f, 0.10f),
			FLinearColor(0.30f, 0.35f, 0.18f),
			FLinearColor(0.28f, 0.19f, 0.12f),
			FLinearColor(0.07f, 0.07f, 0.08f),
			FLinearColor(0.40f, 0.30f, 0.20f),
			FLinearColor(0.35f, 0.32f, 0.18f),
			FLinearColor(0.35f, 0.42f, 0.15f),
			FLinearColor(0.80f, 0.70f, 0.40f),
			FLinearColor(0.65f, 0.62f, 0.55f),
			FLinearColor(0.55f, 0.50f, 0.42f),
			FLinearColor(0.50f, 0.45f, 0.38f),
			FLinearColor(0.40f, 0.40f, 0.36f),
			FLinearColor(0.92f, 0.93f, 0.95f),
			FLinearColor(0.15f, 0.35f, 0.55f),
			FLinearColor(0.15f, 0.40f, 0.45f),
			FLinearColor(0.10f, 0.45f, 0.55f),
			FLinearColor(0.12f, 0.42f, 0.50f),
			FLinearColor(0.05f, 0.15f, 0.35f),
			FLinearColor(0.20f, 0.25f, 0.12f),
			FLinearColor(0.18f, 0.38f, 0.55f),
			FLinearColor(0.20f, 0.30f, 0.15f),
			FLinearColor(0.45f, 0.28f, 0.20f),
			FLinearColor(0.30f, 0.10f, 0.08f),
			FLinearColor(0.45f, 0.15f, 0.05f),
			FLinearColor(0.55f, 0.75f, 0.85f),
			FLinearColor(0.25f, 0.40f, 0.20f),
			FLinearColor(0.15f, 0.28f, 0.14f),
			FLinearColor(0.30f, 0.25f, 0.30f),
			FLinearColor(0.38f, 0.34f, 0.30f),
		};
		return Colors[TerrainType & 0x1F];
	}
}

bool FACELandblockMeshBuilder::EnsureHeightTable()
{
	if (bHeightTableReady)
	{
		return LandHeightTable.Num() == 256;
	}
	if (!Portal)
	{
		return false;
	}
	TArray<uint8> Blob;
	if (!Portal->ReadFile(RegionFileId, Blob))
	{
		UE_LOG(LogTemp, Error, TEXT("ACEDat: missing RegionDesc 0x%08X"), RegionFileId);
		return false;
	}
	FACEDatCursor Cur(Blob);
	if (!ACEDatUnpack::UnpackRegionLandHeightTable(Cur, LandHeightTable) || LandHeightTable.Num() != 256)
	{
		UE_LOG(LogTemp, Error, TEXT("ACEDat: failed to unpack LandHeightTable"));
		return false;
	}
	bHeightTableReady = true;
	return true;
}

bool FACELandblockMeshBuilder::EnsureTexMerge()
{
	if (bTexMergeReady)
	{
		return TexMerge.TerrainDesc.Num() > 0;
	}
	if (!Portal)
	{
		return false;
	}
	TArray<uint8> Blob;
	if (!Portal->ReadFile(RegionFileId, Blob))
	{
		return false;
	}
	FACEDatCursor Cur(Blob);
	if (!ACEDatUnpack::UnpackRegionTexMerge(Cur, TexMerge))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: TexMerge unpack failed — terrain will use approx colors"));
		return false;
	}
	bTexMergeReady = TexMerge.TerrainDesc.Num() > 0;
	return bTexMergeReady;
}

bool FACELandblockMeshBuilder::GetSplitDir(uint32 LandblockId, int32 CellX, int32 CellY)
{
	const int32 LandblockX = static_cast<int32>((LandblockId >> 24) & 0xFF);
	const int32 LandblockY = static_cast<int32>((LandblockId >> 16) & 0xFF);
	const int32 X = LandblockX * 8 + CellX;
	const int32 Y = LandblockY * 8 + CellY;
	const uint32 Dw = static_cast<uint32>(X) * static_cast<uint32>(Y) * 0x0CCAC033u
		- static_cast<uint32>(X) * 0x421BE3BDu
		+ static_cast<uint32>(Y) * 0x6C1AC587u
		- 0x519B8F25u;
	return (Dw & 0x80000000u) == 0;
}

void FACELandblockMeshBuilder::GetBlockOrient(int32 DX, int32 DY, int32& OutPolySize, int32& OutTransDir)
{
	// LScape stores land_blocks[iy + ix * width]: ix is world X, iy is world Y.
	// Its direction enum is therefore relative to (v6=DX, v5=DY).
	Swap(DX, DY);
	const int32 Cheb = FMath::Max(FMath::Abs(DX), FMath::Abs(DY));
	int32 Size = 1;
	if (Cheb > 1)
	{
		if (Cheb > 2)
		{
			if (Cheb > 4)
			{
				OutPolySize = 8;
				OutTransDir = 0;
				return;
			}
			Size = 4;
		}
		else
		{
			Size = 2;
		}
	}
	OutPolySize = Size;
	if (DY == Size)
	{
		if (DX == Size)
		{
			OutTransDir = 7;
		}
		else
		{
			OutTransDir = (DX != -Size) ? 3 : 8;
		}
	}
	else if (DY == -Size)
	{
		if (DX == Size)
		{
			OutTransDir = 5;
		}
		else
		{
			OutTransDir = 2 * (DX == -Size) + 4;
		}
	}
	else if (DX == Size)
	{
		OutTransDir = 1;
	}
	else
	{
		OutTransDir = (DX != -Size) ? 0 : 2;
	}
}

bool FACELandblockMeshBuilder::BuildLodSections(const FACEBuiltLandblockMesh& Full, int32 PolySize, int32 TransDir,
	float WorldScale, TArray<FACEBuiltLandblockSection>& OutSections)
{
	OutSections.Reset();
	if (!Full.bHasHeights || !Full.bHasTerrain || PolySize < 1)
	{
		return false;
	}
	const int32 PS = PolySize;
	if (PS != 1 && PS != 2 && PS != 4 && PS != 8)
	{
		return false;
	}
	const int32 SideCell = CellDim / PS;
	if (SideCell < 1)
	{
		return false;
	}
	const int32 SideVert = SideCell + 1;
	const uint32 LandblockId = Full.LandblockId;

	TArray<float> Z;
	Z.SetNum(SideVert * SideVert);
	for (int32 VX = 0; VX < SideVert; ++VX)
	{
		for (int32 VY = 0; VY < SideVert; ++VY)
		{
			const int32 FX = VX * PS;
			const int32 FY = VY * PS;
			Z[VX * SideVert + VY] = Full.HeightsAc[FX * VertexDim + FY];
		}
	}

	// CLandBlockStruct::TransAdjust: match the coarser ring at the outer edge.
	if (TransDir != 0 && SideCell > 1 && SideCell <= CellDim)
	{
		auto At = [&Z, SideVert](int32 X, int32 Y) -> float&
		{
			return Z[X * SideVert + Y];
		};
		if (TransDir == 1 || TransDir == 5 || TransDir == 7)
		{
			for (int32 I = 1; I < SideCell; I += 2)
			{
				At(I, SideCell) = 0.5f * (At(I - 1, SideCell) + At(I + 1, SideCell));
			}
		}
		if (TransDir == 4 || TransDir == 5 || TransDir == 6)
		{
			for (int32 I = 1; I < SideCell; I += 2)
			{
				At(0, I) = 0.5f * (At(0, I - 1) + At(0, I + 1));
			}
		}
		if (TransDir == 2 || TransDir == 6 || TransDir == 8)
		{
			for (int32 I = 1; I < SideCell; I += 2)
			{
				At(I, 0) = 0.5f * (At(I - 1, 0) + At(I + 1, 0));
			}
		}
		if (TransDir == 3 || TransDir == 7 || TransDir == 8)
		{
			for (int32 I = 1; I < SideCell; I += 2)
			{
				At(SideCell, I) = 0.5f * (At(SideCell, I - 1) + At(SideCell, I + 1));
			}
		}
		// Retail's poly-size-two inner-edge correction uses the omitted fine
		// samples. Without it steep fine terrain exposes a gap to the coarse ring.
		if (SideCell == 4 && TransDir >= 1 && TransDir <= 4)
		{
			for (int32 I = 1; I < SideCell; I += 2)
			{
				const bool bAlongX = TransDir <= 2;
				const int32 Edge = (TransDir == 2 || TransDir == 4) ? CellDim : 0;
				auto Height = [&](int32 Offset)
				{
					return Full.HeightsAc[bAlongX ? (2*I+Offset)*VertexDim+Edge : Edge*VertexDim+2*I+Offset];
				};
				float& Value = bAlongX ? At(I, Edge/PS) : At(Edge/PS, I);
				Value = FMath::Min3(Value, 2*Height(-1)-Height(-2), 2*Height(1)-Height(2));
			}
		}
	}

    if (PS == 1)
    {
        // Preserve full-resolution texture coordinates (including the GPU merge
        // attributes) while applying retail's transition on the outer 3x3 edge.
        OutSections = Full.Sections;
        for (auto& Section : OutSections)
            for (FVector& V : Section.Vertices)
            {
                const int32 X = FMath::Clamp(FMath::RoundToInt(-V.X/(CellSize*WorldScale)),0,CellDim);
                const int32 Y = FMath::Clamp(FMath::RoundToInt(V.Y/(CellSize*WorldScale)),0,CellDim);
                V.Z = Z[X*SideVert+Y]*WorldScale;
            }
        return OutSections.Num()>0;
    }

	TArray<FVector> GridVerts;
	TArray<FVector> GridNormals;
	GridVerts.SetNum(SideVert * SideVert);
	GridNormals.SetNum(SideVert * SideVert);
	for (int32 VX = 0; VX < SideVert; ++VX)
	{
		for (int32 VY = 0; VY < SideVert; ++VY)
		{
			const float Zac = Z[VX * SideVert + VY];
			GridVerts[VX * SideVert + VY] = FACEPosition::AceVectorToUnreal(
				FVector(VX * PS * CellSize, VY * PS * CellSize, Zac), WorldScale);
			GridNormals[VX * SideVert + VY] = FVector::UpVector;
		}
	}

	TArray<int32> GridTris;
	for (int32 CX = 0; CX < SideCell; ++CX)
	{
		for (int32 CY = 0; CY < SideCell; ++CY)
		{
			const int32 LL = CX * SideVert + CY;
			const int32 LR = (CX + 1) * SideVert + CY;
			const int32 TL = CX * SideVert + (CY + 1);
			const int32 TR = (CX + 1) * SideVert + (CY + 1);
			if (GetSplitDir(LandblockId, CX * PS, CY * PS))
			{
				GridTris.Append({TL, LL, LR, TL, LR, TR});
			}
			else
			{
				GridTris.Append({TR, LL, LR, TR, TL, LL});
			}
		}
	}
	TArray<FVector> Accumulators;
	Accumulators.SetNumZeroed(GridVerts.Num());
	for (int32 i = 0; i + 2 < GridTris.Num(); i += 3)
	{
		const FVector N = FVector::CrossProduct(
			GridVerts[GridTris[i + 1]] - GridVerts[GridTris[i]],
			GridVerts[GridTris[i + 2]] - GridVerts[GridTris[i]]).GetSafeNormal();
		Accumulators[GridTris[i]] += N;
		Accumulators[GridTris[i + 1]] += N;
		Accumulators[GridTris[i + 2]] += N;
	}
	for (int32 i = 0; i < GridNormals.Num(); ++i)
	{
		GridNormals[i] = Accumulators[i].GetSafeNormal();
	}

	TMap<uint32, int32> PCodeToSection;
	TMap<uint32, const FACEBuiltLandblockSection*> FullByPCode;
	for (const FACEBuiltLandblockSection& Sec : Full.Sections)
	{
		if (!Sec.bGpuTexMerge && Sec.HasBakedTexture())
		{
			FullByPCode.FindOrAdd(Sec.PCode) = &Sec;
		}
	}

	const bool bHaveTex = EnsureTexMerge() && Textures != nullptr;
	for (int32 CX = 0; CX < SideCell; ++CX)
	{
		for (int32 CY = 0; CY < SideCell; ++CY)
		{
			const int32 FX = CX * PS;
			const int32 FY = CY * PS;
			const uint16 Tr1 = Full.Terrain[FX * VertexDim + FY];
			const uint16 Tr2 = Full.Terrain[(FX + PS) * VertexDim + FY];
			const uint16 Tr3 = Full.Terrain[(FX + PS) * VertexDim + (FY + PS)];
			const uint16 Tr4 = Full.Terrain[FX * VertexDim + (FY + PS)];
			const uint32 PCode = GetPalCode(
				GetRoadType(Tr1), GetRoadType(Tr2), GetRoadType(Tr3), GetRoadType(Tr4),
				GetTerrainType(Tr1), GetTerrainType(Tr2), GetTerrainType(Tr3), GetTerrainType(Tr4));

			int32* SectionIdx = PCodeToSection.Find(PCode);
			if (!SectionIdx)
			{
				FACEBuiltLandblockSection Sec;
				Sec.PCode = PCode;
				if (const FACEBuiltLandblockSection* const* Found = FullByPCode.Find(PCode))
				{
					Sec.SharedBake = (*Found)->SharedBake;
					Sec.BakedPixels = (*Found)->BakedPixels;
					Sec.BakeWidth = (*Found)->BakeWidth;
					Sec.BakeHeight = (*Found)->BakeHeight;
				}
				else if (bHaveTex)
				{
					FillSectionBake(Sec);
				}
				const int32 NewIdx = OutSections.Add(MoveTemp(Sec));
				PCodeToSection.Add(PCode, NewIdx);
				SectionIdx = PCodeToSection.Find(PCode);
			}

			FACEBuiltLandblockSection& Section = OutSections[*SectionIdx];
			const int32 LL = CX * SideVert + CY;
			const int32 LR = (CX + 1) * SideVert + CY;
			const int32 TL = CX * SideVert + (CY + 1);
			const int32 TR = (CX + 1) * SideVert + (CY + 1);
			const int32 Base = Section.Vertices.Num();
			const int32 CornerGi[4] = { LL, LR, TR, TL };
			const float CornerU[4] = { 1.f, 0.f, 0.f, 1.f };
			const float CornerV[4] = { 1.f, 1.f, 0.f, 0.f };
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				const int32 Gi = CornerGi[Corner];
				Section.Vertices.Add(GridVerts[Gi]);
				Section.Normals.Add(GridNormals[Gi]);
				Section.UVs.Add(FVector2D(CornerU[Corner], CornerV[Corner]));
				Section.VertexColors.Add(FLinearColor::White);
			}
			if (GetSplitDir(LandblockId, CX * PS, CY * PS))
			{
				Section.Triangles.Append({Base + 3, Base + 0, Base + 1, Base + 3, Base + 1, Base + 2});
			}
			else
			{
				Section.Triangles.Append({Base + 2, Base + 0, Base + 1, Base + 2, Base + 3, Base + 0});
			}
		}
	}

	return OutSections.Num() > 0;
}

FLinearColor FACELandblockMeshBuilder::GetWaterDepthsAc(uint32 PCode)
{
	uint32 Types[4];
	GetTerrainCodes(PCode, Types);
	bool Wet[4];
	int32 WetCount = 0;
	for (int32 I = 0; I < 4; ++I)
	{
		Wet[I] = Types[I] >= 0x10 && Types[I] <= 0x14;
		WetCount += int32(Wet[I]);
	}
	if (!WetCount) return FLinearColor::Transparent;
	if (WetCount == 4) return FLinearColor(.9f,.9f,.9f,.9f);
	return FLinearColor(Wet[0] ? .45f : .1f, Wet[1] ? .45f : .1f,
		Wet[2] ? .45f : .1f, Wet[3] ? .45f : .1f);
}

float FACELandblockMeshBuilder::GetWaterDepthAc(const FACEBuiltLandblockMesh& Mesh, float LocalX, float LocalY)
{
	LocalX = FMath::Clamp(LocalX, 0.f, LandblockSize - KINDA_SMALL_NUMBER);
	LocalY = FMath::Clamp(LocalY, 0.f, LandblockSize - KINDA_SMALL_NUMBER);
	const int32 X = FMath::Min(FMath::FloorToInt(LocalX / CellSize), CellDim - 1);
	const int32 Y = FMath::Min(FMath::FloorToInt(LocalY / CellSize), CellDim - 1);
	if (Mesh.bHasCellWater)
	{
		const uint8 Type = Mesh.CellWaterType[X * CellDim + Y];
		if (Type == 0) return 0.f;
		if (Type == 2) return .9f;
	}
	if (!Mesh.bHasTerrain) return 0.f;
	const int32 SW = X * VertexDim + Y, SE = SW + VertexDim;
	const uint32 Code = GetPalCode(0,0,0,0, GetTerrainType(Mesh.Terrain[SW]), GetTerrainType(Mesh.Terrain[SE]),
		GetTerrainType(Mesh.Terrain[SE+1]), GetTerrainType(Mesh.Terrain[SW+1]));
	const FLinearColor Depths = GetWaterDepthsAc(Code);
	const bool East = LocalX - X * CellSize >= CellSize * .5f;
	const bool North = LocalY - Y * CellSize >= CellSize * .5f;
	return North ? (East ? Depths.B : Depths.A) : (East ? Depths.G : Depths.R);
}

bool FACELandblockMeshBuilder::SampleHeightAc(const FACEBuiltLandblockMesh& Mesh, float LocalX, float LocalY, float& OutZAc, FVector* OutNormalAc)
{
	if (!Mesh.bHasHeights)
	{
		return false;
	}

	LocalX = FMath::Clamp(LocalX, 0.f, LandblockSize - KINDA_SMALL_NUMBER);
	LocalY = FMath::Clamp(LocalY, 0.f, LandblockSize - KINDA_SMALL_NUMBER);

	const int32 CX = FMath::Clamp(FMath::FloorToInt(LocalX / CellSize), 0, CellDim - 1);
	const int32 CY = FMath::Clamp(FMath::FloorToInt(LocalY / CellSize), 0, CellDim - 1);
	const float U = (LocalX / CellSize) - static_cast<float>(CX);
	const float V = (LocalY / CellSize) - static_cast<float>(CY);

	auto VertZ = [&](int32 X, int32 Y) -> float
	{
		return Mesh.HeightsAc[X * VertexDim + Y];
	};

	const FVector P0(0.f, 0.f, VertZ(CX, CY));           // LL
	const FVector P1(1.f, 0.f, VertZ(CX + 1, CY));       // LR
	const FVector P2(1.f, 1.f, VertZ(CX + 1, CY + 1));   // TR
	const FVector P3(0.f, 1.f, VertZ(CX, CY + 1));       // TL

	FVector A, B, C;
	if (GetSplitDir(Mesh.LandblockId, CX, CY))
	{
		// Diagonal TL–LR: tris (TL,LL,LR) and (TL,LR,TR)
		if (U + V <= 1.f)
		{
			A = P3; B = P0; C = P1;
		}
		else
		{
			A = P3; B = P1; C = P2;
		}
	}
	else
	{
		// Diagonal TR–LL: tris (TR,LL,LR) and (TR,TL,LL)
		if (V <= U)
		{
			A = P2; B = P0; C = P1;
		}
		else
		{
			A = P2; B = P3; C = P0;
		}
	}

	const FVector N = FVector::CrossProduct(B - A, C - A);
	if (FMath::Abs(N.Z) <= KINDA_SMALL_NUMBER)
	{
		OutZAc = A.Z;
		if (OutNormalAc) *OutNormalAc = FVector::UpVector;
		return true;
	}
	if (OutNormalAc)
	{
		// U/V are normalized cell coordinates; convert their derivatives back
		// to AC distance before testing slope against the walkable threshold.
		*OutNormalAc = FVector(N.X / CellSize, N.Y / CellSize, N.Z).GetSafeNormal();
		if (OutNormalAc->Z < 0) *OutNormalAc *= -1;
	}
	// Plane through A: N·(P-A)=0 → Z from U,V in cell space
	OutZAc = A.Z - (N.X * (U - A.X) + N.Y * (V - A.Y)) / N.Z;
	return true;
}

uint32 FACELandblockMeshBuilder::GetPalCode(int32 R1, int32 R2, int32 R3, int32 R4, int32 T1, int32 T2, int32 T3, int32 T4)
{
	const uint32 TerrainBits = (static_cast<uint32>(T1) << 15) | (static_cast<uint32>(T2) << 10)
		| (static_cast<uint32>(T3) << 5) | static_cast<uint32>(T4);
	const uint32 RoadBits = (static_cast<uint32>(R1) << 26) | (static_cast<uint32>(R2) << 24)
		| (static_cast<uint32>(R3) << 22) | (static_cast<uint32>(R4) << 20);
	const uint32 SizeBits = 1u << 28;
	return SizeBits | RoadBits | TerrainBits;
}

bool FACELandblockMeshBuilder::LoadTerrainSurface(uint32 TerrainType, FACEDatDecodedSurface& Out, uint32& OutTiling)
{
	OutTiling = 1;
	if (const FACEDatDecodedSurface* Cached = TerrainSurfCache.Find(TerrainType))
	{
		Out = *Cached;
		if (const uint32* Tile = TerrainTilingCache.Find(TerrainType))
		{
			OutTiling = *Tile;
		}
		return Out.bHasPixels;
	}

	const FACEDatTerrainTex* Tex = nullptr;
	for (const FACEDatTMTerrainDesc& Desc : TexMerge.TerrainDesc)
	{
		if (Desc.TerrainType == TerrainType)
		{
			Tex = &Desc.TerrainTex;
			break;
		}
	}
	if (!Tex && TexMerge.TerrainDesc.Num() > 0)
	{
		Tex = &TexMerge.TerrainDesc[0].TerrainTex;
	}
	if (!Tex || !Textures || Tex->TexGID == 0)
	{
		return false;
	}

	OutTiling = FMath::Max(1u, Tex->TexTiling);
	if (!Textures->DecodeSurfaceTexture(Tex->TexGID, Out))
	{
		return false;
	}
	TerrainSurfCache.Add(TerrainType, Out);
	TerrainTilingCache.Add(TerrainType, OutTiling);
	return true;
}

FColor FACELandblockMeshBuilder::SampleTiled(const FACEDatDecodedSurface& Surf, float U, float V, uint32 Tiling) const
{
	const float Tile = static_cast<float>(FMath::Max(1u, Tiling));
	return Surf.SampleUV(U * Tile, V * Tile);
}

void FACELandblockMeshBuilder::RotateUV(float U, float V, int32 Rot, float& OutU, float& OutV)
{
	// ImgTex.MergeTexture step mapping for LandDefs.Rotation (ACE UV space, U=0 west).
	// Mesh UVs are U-mirrored to match AceVectorToUnreal(-X); bake flips U on write
	// so these rotations stay in ACE space.
	switch (Rot & 3)
	{
	case 1: OutU = 1.f - V; OutV = U; break;           // Rot90
	case 2: OutU = 1.f - U; OutV = 1.f - V; break;     // Rot180
	case 3: OutU = V; OutV = 1.f - U; break;           // Rot270
	default: OutU = U; OutV = V; break;
	}
}

FColor FACELandblockMeshBuilder::SampleAlpha(const FACEDatDecodedSurface& Alpha, float U, float V, int32 Rot) const
{
	float RU, RV;
	RotateUV(U, V, Rot, RU, RV);
	return Alpha.SampleUV(RU, RV);
}

void FACELandblockMeshBuilder::MergeOverlay(TArray<FColor>& Pixels, int32 Size, const FACEDatDecodedSurface& Overlay, uint32 OverlayTiling, const FACEDatDecodedSurface& Alpha, int32 Rot) const
{
	if (!Overlay.bHasPixels || !Alpha.bHasPixels || Size <= 0)
	{
		return;
	}
	for (int32 Y = 0; Y < Size; ++Y)
	{
		// Texture row 0 = V=0 = north (ACE LandUVs NW/NE).
		const float V = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Size);
		for (int32 X = 0; X < Size; ++X)
		{
			// Sample in ACE UV (U=0 west). Write column mirrored so the bake matches
			// mesh UVs that are U-mirrored for AceVectorToUnreal(-X).
			const float U = (static_cast<float>(X) + 0.5f) / static_cast<float>(Size);
			const FColor A = SampleAlpha(Alpha, U, V, Rot);
			// ImgTex::MergeTexture blends the encoded RGB bytes, not linear-light
			// colors. Linearizing first brightens road/grass transition bands.
			// CUSTOM_LSCAPE_ALPHA stores the retained base weight in R; retail
			// promotes weights above 128 by one for its 256-denominator blend.
			if (A.R == 255)
			{
				continue;
			}
			const int32 Idx = Y * Size + (Size - 1 - X);
			const FColor Base = Pixels[Idx];
			const FColor Over = SampleTiled(Overlay, U, V, OverlayTiling);
			const uint32 BaseWeight = A.R + (A.R > 128 ? 1u : 0u);
			const uint32 OverlayWeight = 256u - BaseWeight;
			Pixels[Idx] = FColor(
				static_cast<uint8>((BaseWeight * Base.R + OverlayWeight * Over.R) >> 8),
				static_cast<uint8>((BaseWeight * Base.G + OverlayWeight * Over.G) >> 8),
				static_cast<uint8>((BaseWeight * Base.B + OverlayWeight * Over.B) >> 8), 255);
		}
	}
}

void FACELandblockMeshBuilder::GetTerrainCodes(uint32 PCode, uint32 OutTypes[4])
{
	OutTypes[0] = (PCode >> 15) & 0x1F; // SW
	OutTypes[1] = (PCode >> 10) & 0x1F; // SE
	OutTypes[2] = (PCode >> 5) & 0x1F;  // NE
	OutTypes[3] = PCode & 0x1F;         // NW
}

bool FACELandblockMeshBuilder::GetTerrainLayers(uint32 PCode, uint32 OutTerrainTypes[4], uint32 OutTCodes[3], int32& OutLayerCount)
{
	// Port of ACE.Server Physics TexMerge.GetTerrain / BuildTCodes.
	OutLayerCount = 0;
	OutTCodes[0] = OutTCodes[1] = OutTCodes[2] = 0;
	uint32 Types[4];
	GetTerrainCodes(PCode, Types);

	int32 MatchI = -1;
	for (int32 i = 0; i < 4 && MatchI < 0; ++i)
	{
		for (int32 j = i + 1; j < 4; ++j)
		{
			if (Types[i] == Types[j])
			{
				MatchI = i;
				break;
			}
		}
	}

	if (MatchI < 0)
	{
		// All four corners unique — base = SW, overlays at SE/NE/NW corners (tcodes 2,4,8).
		OutTerrainTypes[0] = Types[0];
		OutTerrainTypes[1] = Types[1];
		OutTerrainTypes[2] = Types[2];
		OutTerrainTypes[3] = Types[3];
		OutTCodes[0] = 2;
		OutTCodes[1] = 4;
		OutTCodes[2] = 8;
		OutLayerCount = 4;
		return true;
	}

	const uint32 T1 = Types[MatchI];
	OutTerrainTypes[0] = T1;
	OutTerrainTypes[1] = T1;
	OutTerrainTypes[2] = T1;
	OutLayerCount = 1;

	uint32 T2 = 0;
	bool bHaveT2 = false;
	for (int32 k = 0; k < 4; ++k)
	{
		if (Types[k] == T1)
		{
			continue;
		}
		if (!bHaveT2)
		{
			OutTCodes[0] = 1u << k;
			T2 = Types[k];
			OutTerrainTypes[1] = T2;
			bHaveT2 = true;
			OutLayerCount = 2;
		}
		else
		{
			if (Types[k] == T2 && OutTCodes[0] == (1u << (k - 1)))
			{
				OutTCodes[0] += (1u << k);
			}
			else
			{
				OutTerrainTypes[2] = Types[k];
				OutTCodes[1] = 1u << k;
				OutLayerCount = 3;
			}
			break;
		}
	}
	return true;
}

void FACELandblockMeshBuilder::GetRoadCodes(uint32 PCode, uint32 OutRCodes[2], bool& bAllRoad) const
{
	// Port of ACE.Server Physics TexMerge.GetRoadCode.
	OutRCodes[0] = OutRCodes[1] = 0;
	bAllRoad = false;
	uint32 Mask = 0;
	if ((PCode & 0xC000000) != 0) Mask |= 1;  // SW (r1 / corner bit 0)
	if ((PCode & 0x3000000) != 0) Mask |= 2;  // SE (r2)
	if ((PCode & 0xC00000) != 0) Mask |= 4;   // NE (r3)
	if ((PCode & 0x300000) != 0) Mask |= 8;   // NW (r4)

	switch (Mask)
	{
	case 0xF: bAllRoad = true; break;
	case 0xE: OutRCodes[0] = 6; OutRCodes[1] = 12; break;
	case 0xD: OutRCodes[0] = 9; OutRCodes[1] = 12; break;
	case 0xB: OutRCodes[0] = 9; OutRCodes[1] = 3; break;
	case 0x7: OutRCodes[0] = 3; OutRCodes[1] = 6; break;
	case 0x0: break;
	default: OutRCodes[0] = Mask; break;
	}
}

bool FACELandblockMeshBuilder::FindTerrainAlpha(uint32 PCode, uint32 TCode, FACEDatDecodedSurface& OutAlpha, int32& OutRot, uint32* OutTexGID)
{
	OutRot = 0;
	if (OutTexGID)
	{
		*OutTexGID = 0;
	}
	const bool bCorner = (TCode == 1 || TCode == 2 || TCode == 4 || TCode == 8);
	const TArray<FACEDatTerrainAlphaMap>& Maps = bCorner ? TexMerge.CornerTerrainMaps : TexMerge.SideTerrainMaps;
	const int32 Num = Maps.Num();
	if (Num <= 0 || !Textures)
	{
		return false;
	}

	int32 Prng = static_cast<int32>(FMath::FloorToDouble((1379576222.0 * static_cast<double>(PCode) - 1372186442.0) * 2.3283064e-10 * Num));
	if (Prng < 0 || Prng >= Num)
	{
		Prng = 0;
	}

	const FACEDatTerrainAlphaMap& Alpha = Maps[Prng];
	uint32 AlphaCode = Alpha.TCode;
	for (int32 i = 0; i < 4; ++i)
	{
		if (AlphaCode == TCode)
		{
			OutRot = i;
			if (OutTexGID)
			{
				*OutTexGID = Alpha.TexGID;
				return Alpha.TexGID != 0;
			}
			return Alpha.TexGID != 0 && Textures->DecodeSurfaceTexture(Alpha.TexGID, OutAlpha);
		}
		AlphaCode *= 2;
		if (AlphaCode >= 16)
		{
			AlphaCode -= 15;
		}
	}
	return false;
}

bool FACELandblockMeshBuilder::FindRoadAlpha(uint32 PCode, uint32 RCode, FACEDatDecodedSurface& OutAlpha, int32& OutRot, uint32* OutTexGID)
{
	OutRot = 0;
	if (OutTexGID)
	{
		*OutTexGID = 0;
	}
	const int32 Num = TexMerge.RoadMaps.Num();
	if (Num <= 0 || !Textures)
	{
		return false;
	}

	int32 Prng = static_cast<int32>(FMath::FloorToDouble((1379576222.0 * static_cast<double>(PCode) - 1372186442.0) * 2.3283064e-10 * Num));
	if (Prng < 0)
	{
		Prng = 0;
	}

	for (int32 i = 0; i < Num; ++i)
	{
		const int32 Idx = (i + Prng) % Num;
		const FACEDatRoadAlphaMap& Alpha = TexMerge.RoadMaps[Idx];
		uint32 AlphaCode = Alpha.RCode;
		for (int32 j = 0; j < 4; ++j)
		{
			if (AlphaCode == RCode)
			{
				OutRot = j;
				if (OutTexGID)
				{
					*OutTexGID = Alpha.RoadTexGID;
					return Alpha.RoadTexGID != 0;
				}
				return Alpha.RoadTexGID != 0 && Textures->DecodeSurfaceTexture(Alpha.RoadTexGID, OutAlpha);
			}
			AlphaCode *= 2;
			if (AlphaCode >= 16)
			{
				AlphaCode -= 15;
			}
		}
	}
	return false;
}

bool FACELandblockMeshBuilder::ResolveGpuBlend(uint32 PCode, FACELandGpuBlend& Out)
{
	Out = FACELandGpuBlend();
	if (!LandAtlas || !LandAtlas->IsReady())
	{
		return false;
	}

	uint32 TerrainTypes[4] = {};
	uint32 TCodes[3] = {};
	int32 LayerCount = 0;
	if (!GetTerrainLayers(PCode, TerrainTypes, TCodes, LayerCount))
	{
		return false;
	}

	Out.BaseLayer = LandAtlas->FindTerrainLayerByType(TerrainTypes[0]);
	if (Out.BaseLayer < 0)
	{
		return false;
	}

	Out.OverlayCount = 0;
	for (int32 i = 0; i < LayerCount && Out.OverlayCount < 3; ++i)
	{
		const uint32 OverlayType = TerrainTypes[FMath::Min(i + 1, 3)];
		const int32 TexLayer = LandAtlas->FindTerrainLayerByType(OverlayType);
		FACEDatDecodedSurface Dummy;
		int32 Rot = 0;
		uint32 AlphaGid = 0;
		if (TexLayer < 0 || !FindTerrainAlpha(PCode, TCodes[i], Dummy, Rot, &AlphaGid))
		{
			continue;
		}
		Out.OverlayLayers[Out.OverlayCount] = TexLayer;
		Out.OverlayAlphaLayers[Out.OverlayCount] = LandAtlas->FindAlphaLayer(AlphaGid);
		Out.OverlayRots[Out.OverlayCount] = Rot;
		++Out.OverlayCount;
	}

	uint32 RCodes[2] = {};
	bool bAllRoad = false;
	GetRoadCodes(PCode, RCodes, bAllRoad);
	Out.RoadCount = 0;
	Out.RoadLayer = LandAtlas->FindTerrainLayerByType(RoadTerrainType);
	if (Out.RoadLayer >= 0 && (bAllRoad || RCodes[0] != 0 || RCodes[1] != 0))
	{
		for (int32 i = 0; i < 2 && Out.RoadCount < 2; ++i)
		{
			if (!bAllRoad && RCodes[i] == 0)
			{
				continue;
			}
			FACEDatDecodedSurface Dummy;
			int32 Rot = 0;
			uint32 AlphaGid = 0;
			if (!FindRoadAlpha(PCode, bAllRoad ? 1u : RCodes[i], Dummy, Rot, &AlphaGid))
			{
				continue;
			}
			Out.RoadAlphaLayers[Out.RoadCount] = LandAtlas->FindAlphaLayer(AlphaGid);
			Out.RoadRots[Out.RoadCount] = Rot;
			++Out.RoadCount;
		}
	}
	return true;
}

bool FACELandblockMeshBuilder::BakePCode(uint32 PCode, TArray<FColor>& OutPixels, int32& OutW, int32& OutH)
{
	OutW = 0;
	OutH = 0;
	OutPixels.Reset();

	const int32 Size = (TexMerge.BaseTexSize > 0) ? static_cast<int32>(TexMerge.BaseTexSize) : BakeSize;
	if (Size <= 0)
	{
		return false;
	}

	uint32 TerrainTypes[4] = {};
	uint32 TCodes[3] = {};
	int32 LayerCount = 0;
	GetTerrainLayers(PCode, TerrainTypes, TCodes, LayerCount);

	uint32 RCodes[2] = {};
	bool bAllRoad = false;
	GetRoadCodes(PCode, RCodes, bAllRoad);

	FACEDatDecodedSurface RoadSurf;
	uint32 RoadTile = 1;
	const bool bHaveRoadSurf = LoadTerrainSurface(RoadTerrainType, RoadSurf, RoadTile);

	FACEDatDecodedSurface BaseSurf;
	uint32 BaseTile = 1;
	if (bAllRoad)
	{
		if (!bHaveRoadSurf)
		{
			return false;
		}
		BaseSurf = RoadSurf;
		BaseTile = RoadTile;
	}
	else
	{
		if (!LoadTerrainSurface(TerrainTypes[0], BaseSurf, BaseTile))
		{
			return false;
		}
	}

	OutW = Size;
	OutH = Size;
	OutPixels.SetNumUninitialized(Size * Size);
	for (int32 Y = 0; Y < Size; ++Y)
	{
		const float V = (static_cast<float>(Y) + 0.5f) / static_cast<float>(Size);
		for (int32 X = 0; X < Size; ++X)
		{
			const float U = (static_cast<float>(X) + 0.5f) / static_cast<float>(Size);
			// U-flip write to match mesh LandUVs mirrored for AceVectorToUnreal(-X).
			OutPixels[Y * Size + (Size - 1 - X)] = SampleTiled(BaseSurf, U, V, BaseTile);
		}
	}

	if (!bAllRoad)
	{
		for (int32 i = 0; i < 3; ++i)
		{
			if (TCodes[i] == 0)
			{
				break;
			}
			FACEDatDecodedSurface Overlay;
			uint32 OverlayTile = 1;
			const uint32 OverlayType = TerrainTypes[FMath::Min(i + 1, 3)];
			if (!LoadTerrainSurface(OverlayType, Overlay, OverlayTile))
			{
				continue;
			}
			FACEDatDecodedSurface Alpha;
			int32 Rot = 0;
			if (!FindTerrainAlpha(PCode, TCodes[i], Alpha, Rot))
			{
				continue;
			}
			MergeOverlay(OutPixels, Size, Overlay, OverlayTile, Alpha, Rot);
		}

		if (bHaveRoadSurf)
		{
			for (int32 i = 0; i < 2; ++i)
			{
				if (RCodes[i] == 0)
				{
					break;
				}
				FACEDatDecodedSurface Alpha;
				int32 Rot = 0;
				if (!FindRoadAlpha(PCode, RCodes[i], Alpha, Rot))
				{
					continue;
				}
				MergeOverlay(OutPixels, Size, RoadSurf, RoadTile, Alpha, Rot);
			}
		}
	}

	return true;
}

bool FACELandblockMeshBuilder::FillSectionBake(FACEBuiltLandblockSection& Section)
{
    Section.SharedBake = BlendCache->GetOrBuild(Section.PCode, CacheFingerprint, [&]() -> FACETerrainBlendCache::FBlendPtr
    {
        auto Blend = MakeShared<FACETerrainBlend, ESPMode::ThreadSafe>();
        if (!(CacheFingerprint != 0 && ACEPCodeTileCache::LoadPrepared(Section.PCode, CacheFingerprint, *Blend)))
        {
            if (!BakePCode(Section.PCode, Blend->Pixels, Blend->Width, Blend->Height)) return nullptr;
            if (!Blend->PrepareUploadMips()) return nullptr;
            if (CacheFingerprint != 0)
                ACEPCodeTileCache::SavePrepared(Section.PCode, CacheFingerprint, *Blend);
        }
        return Blend;
    });
    if (!Section.SharedBake) return false;
    Section.BakeWidth = Section.SharedBake->Width;
    Section.BakeHeight = Section.SharedBake->Height;
    Section.BakedPixels.Empty();
    return true;
}

void FACELandblockMeshBuilder::HydrateSectionBakes(FACEBuiltLandblockMesh& Mesh, uint64 DatFingerprint)
{
    CacheFingerprint = DatFingerprint;
    EnsureTexMerge();
    for (FACEBuiltLandblockSection& Sec : Mesh.Sections)
    {
        if (Sec.bGpuTexMerge || Sec.HasBakedTexture()) continue;
        FillSectionBake(Sec);
    }
}

bool FACELandblockMeshBuilder::BuildLandblock(uint32 LandblockId, float WorldScale, FACEBuiltLandblockMesh& OutMesh)
{
	OutMesh = FACEBuiltLandblockMesh();
	OutMesh.LandblockId = LandblockId;
	if (!Cell || !EnsureHeightTable())
	{
		return false;
	}
	const bool bHaveTex = EnsureTexMerge() && Textures != nullptr;

	const uint32 FileId = (LandblockId & 0xFFFF0000) | 0xFFFF;
	TArray<uint8> Blob;
	if (!Cell->ReadFile(FileId, Blob))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: missing CellLandblock 0x%08X"), FileId);
		return false;
	}

	FACEDatCursor Cur(Blob);
	FACEDatCellLandblock LB;
	if (!ACEDatUnpack::UnpackCellLandblock(Cur, LB))
	{
		return false;
	}

	TArray<FVector> GridVerts;
	TArray<FVector> GridNormals;
	TArray<FLinearColor> GridColors;
	GridVerts.Reserve(VertexDim * VertexDim);
	GridNormals.Reserve(VertexDim * VertexDim);
	GridColors.Reserve(VertexDim * VertexDim);

	for (int32 X = 0; X < VertexDim; ++X)
	{
		for (int32 Y = 0; Y < VertexDim; ++Y)
		{
			const uint8 HeightIdx = LB.Height[X * VertexDim + Y];
			const float Z = LandHeightTable.IsValidIndex(HeightIdx) ? LandHeightTable[HeightIdx] : 0.f;
			OutMesh.HeightsAc[X * VertexDim + Y] = Z;
			GridVerts.Add(FACEPosition::AceVectorToUnreal(FVector(X * CellSize, Y * CellSize, Z), WorldScale));
			GridNormals.Add(FVector::UpVector);

			const uint16 RawTerrain = LB.Terrain[X * VertexDim + Y];
			OutMesh.Terrain[X * VertexDim + Y] = RawTerrain;
			FLinearColor Color = GetTerrainTypeColor(GetTerrainType(RawTerrain));
			if (GetRoadType(RawTerrain) != 0)
			{
				Color = FLinearColor::LerpUsingHSV(Color, FLinearColor(0.45f, 0.38f, 0.28f), 0.7f);
			}
			const float T = FMath::Clamp(Z / 100.f, 0.f, 1.f);
			Color *= (0.85f + 0.3f * T);
			Color.A = 1.f;
			GridColors.Add(Color);
		}
	}
	OutMesh.bHasHeights = true;
	OutMesh.bHasTerrain = true;

	// Retail LandblockStruct.CalcCellWater → LandCell.WaterType per 24×24 cell,
	// then LandblockStruct.water_type aggregate (EntirelyWater only if every cell is fully wet).
	{
		static const uint8 SurfChar[32] = {
			0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0,
			1, 1, 1, 1, 1, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0
		};
		bool bBlockHasWater = false;
		bool bBlockAllWater = true;
		for (int32 CX = 0; CX < CellDim; ++CX)
		{
			for (int32 CY = 0; CY < CellDim; ++CY)
			{
				bool bHas = false;
				bool bFull = true;
				for (int32 VX = CX * VertexPerCell; VX <= VertexPerCell * (CX + 1); ++VX)
				{
					for (int32 VY = CY * VertexPerCell; VY <= VertexPerCell * (CY + 1); ++VY)
					{
						const uint8 SurfIdx = static_cast<uint8>(
							(OutMesh.Terrain[VX * VertexDim + VY] >> 2) & 0x1F);
						if (SurfChar[SurfIdx] != 0)
						{
							bHas = true;
						}
						else
						{
							bFull = false;
						}
					}
				}
				uint8 WT = 0;
				if (bHas)
				{
					bBlockHasWater = true;
					WT = bFull ? 2 : 1;
					if (!bFull)
					{
						bBlockAllWater = false;
					}
				}
				else
				{
					bBlockAllWater = false;
				}
				OutMesh.CellWaterType[CX * CellDim + CY] = WT;
			}
		}
		OutMesh.bHasCellWater = true;
		OutMesh.BlockWaterType = bBlockHasWater ? (bBlockAllWater ? 2 : 1) : 0;
	}

	auto VertIndex = [](int32 X, int32 Y) { return X * VertexDim + Y; };

	// Smooth normals over the shared grid first.
	TArray<int32> GridTris;
	for (int32 CX = 0; CX < CellDim; ++CX)
	{
		for (int32 CY = 0; CY < CellDim; ++CY)
		{
			const int32 LL = VertIndex(CX, CY);
			const int32 LR = VertIndex(CX + 1, CY);
			const int32 TL = VertIndex(CX, CY + 1);
			const int32 TR = VertIndex(CX + 1, CY + 1);
			if (GetSplitDir(LandblockId, CX, CY))
			{
				GridTris.Append({TL, LL, LR, TL, LR, TR});
			}
			else
			{
				GridTris.Append({TR, LL, LR, TR, TL, LL});
			}
		}
	}
	TArray<FVector> Accumulators;
	Accumulators.SetNumZeroed(GridVerts.Num());
	for (int32 i = 0; i + 2 < GridTris.Num(); i += 3)
	{
		const FVector N = FVector::CrossProduct(
			GridVerts[GridTris[i + 1]] - GridVerts[GridTris[i]],
			GridVerts[GridTris[i + 2]] - GridVerts[GridTris[i]]).GetSafeNormal();
		Accumulators[GridTris[i]] += N;
		Accumulators[GridTris[i + 1]] += N;
		Accumulators[GridTris[i + 2]] += N;
	}
	for (int32 i = 0; i < GridNormals.Num(); ++i)
	{
		GridNormals[i] = Accumulators[i].GetSafeNormal();
	}

	TMap<uint32, int32> PCodeToSection;
	const bool bGpu = LandAtlas && LandAtlas->IsReady();

	// GPU path: one drawable section for the whole landblock (one draw / one material).
	int32 GpuSectionIdx = INDEX_NONE;
	if (bGpu)
	{
		FACEBuiltLandblockSection Sec;
		Sec.bGpuTexMerge = true;
		Sec.PCode = 0;
		GpuSectionIdx = OutMesh.Sections.Add(MoveTemp(Sec));
	}

	auto PackLayer = [](int32 Layer) -> float
	{
		return Layer >= 0 ? (static_cast<float>(Layer) + 0.5f) : -1.f;
	};

	for (int32 CX = 0; CX < CellDim; ++CX)
	{
		for (int32 CY = 0; CY < CellDim; ++CY)
		{
			const int32 I = VertIndex(CX, CY);
			const int32 J = VertIndex(CX + 1, CY);
			const uint16 Tr1 = LB.Terrain[I];
			const uint16 Tr2 = LB.Terrain[J];
			const uint16 Tr3 = LB.Terrain[J + 1];
			const uint16 Tr4 = LB.Terrain[I + 1];
			const uint32 PCode = GetPalCode(
				GetRoadType(Tr1), GetRoadType(Tr2), GetRoadType(Tr3), GetRoadType(Tr4),
				GetTerrainType(Tr1), GetTerrainType(Tr2), GetTerrainType(Tr3), GetTerrainType(Tr4));

			FACELandGpuBlend GpuBlend;
			const bool bCellGpu = bGpu && ResolveGpuBlend(PCode, GpuBlend);

			int32 SectionIdxValue = GpuSectionIdx;
			if (!bCellGpu)
			{
				int32* SectionIdx = PCodeToSection.Find(PCode);
				if (!SectionIdx)
				{
					FACEBuiltLandblockSection Sec;
					Sec.PCode = PCode;
					if (bHaveTex)
					{
						FillSectionBake(Sec);
					}
					const int32 NewIdx = OutMesh.Sections.Add(MoveTemp(Sec));
					PCodeToSection.Add(PCode, NewIdx);
					SectionIdx = PCodeToSection.Find(PCode);
				}
				SectionIdxValue = *SectionIdx;
			}

			FACEBuiltLandblockSection& Section = OutMesh.Sections[SectionIdxValue];

			const int32 LL = VertIndex(CX, CY);
			const int32 LR = VertIndex(CX + 1, CY);
			const int32 TL = VertIndex(CX, CY + 1);
			const int32 TR = VertIndex(CX + 1, CY + 1);
			const int32 Base = Section.Vertices.Num();

			// Corner UVs in ACE space (SW,SE,NE,NW) — mesh U is mirrored for Unreal -X.
			const float CornerU[4] = { 1.f, 0.f, 0.f, 1.f };
			const float CornerV[4] = { 1.f, 1.f, 0.f, 0.f };
			const int32 CornerGi[4] = { LL, LR, TR, TL };

			auto AddVert = [&](int32 Corner)
			{
				const int32 Gi = CornerGi[Corner];
				const float U = CornerU[Corner];
				const float V = CornerV[Corner];
				Section.Vertices.Add(GridVerts[Gi]);
				Section.Normals.Add(GridNormals[Gi]);
				Section.UVs.Add(FVector2D(U, V));
				if (bCellGpu)
				{
					float A0U = U, A0V = V;
					if (GpuBlend.OverlayCount > 0)
					{
						RotateUV(U, V, GpuBlend.OverlayRots[0], A0U, A0V);
					}
					Section.UV1.Add(FVector2D(PackLayer(GpuBlend.BaseLayer), PackLayer(GpuBlend.OverlayCount > 0 ? GpuBlend.OverlayLayers[0] : -1)));
					Section.UV2.Add(FVector2D(A0U, A0V));
					Section.UV3.Add(FVector2D(
						PackLayer(GpuBlend.OverlayCount > 0 ? GpuBlend.OverlayAlphaLayers[0] : -1),
						PackLayer(GpuBlend.RoadCount > 0 ? GpuBlend.RoadLayer : -1)));
					float R0U = U, R0V = V;
					if (GpuBlend.RoadCount > 0)
					{
						RotateUV(U, V, GpuBlend.RoadRots[0], R0U, R0V);
					}
					Section.VertexColors.Add(FLinearColor(
						PackLayer(GpuBlend.OverlayCount > 1 ? GpuBlend.OverlayLayers[1] : -1),
						PackLayer(GpuBlend.OverlayCount > 1 ? GpuBlend.OverlayAlphaLayers[1] : -1),
						R0U,
						R0V));
				}
				else
				{
					Section.VertexColors.Add(GridColors[Gi]);
				}
			};
			AddVert(0);
			AddVert(1);
			AddVert(2);
			AddVert(3);

			if (GetSplitDir(LandblockId, CX, CY))
			{
				Section.Triangles.Append({Base + 3, Base + 0, Base + 1, Base + 3, Base + 1, Base + 2});
			}
			else
			{
				Section.Triangles.Append({Base + 2, Base + 0, Base + 1, Base + 2, Base + 3, Base + 0});
			}
		}
	}

	// Drop empty GPU placeholder if every cell fell back to CPU sections.
	if (bGpu && GpuSectionIdx != INDEX_NONE && OutMesh.Sections[GpuSectionIdx].IsEmpty())
	{
		OutMesh.Sections.RemoveAt(GpuSectionIdx);
	}

	return !OutMesh.IsEmpty();
}
