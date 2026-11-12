#include "Personal/PersonalFarTerrainBake.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACELandblockMeshBuilder.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr uint32 FarTerrainCacheMagic = 0x54464150; // "PAFT"
	constexpr uint32 FarTerrainCacheVersion = 2;
	constexpr int32 BakeVertexDim = FACELandblockMeshBuilder::VertexDim;

	/** Average color of a baked terrain texture. Every eighth pixel is enough. */
	bool AverageBakedColor(FACELandblockMeshBuilder& Builder, uint32 PalCode, FColor& Out)
	{
		TArray<FColor> Pixels;
		int32 Width = 0, Height = 0;
		if (!Builder.BakePCode(PalCode, Pixels, Width, Height) || Pixels.Num() != Width * Height || Pixels.Num() == 0)
		{
			return false;
		}
		constexpr int32 Step = 8;
		FLinearColor Sum(0, 0, 0, 0);
		int32 Count = 0;
		for (int32 Y = 0; Y < Height; Y += Step)
		{
			for (int32 X = 0; X < Width; X += Step)
			{
				Sum += FLinearColor(Pixels[Y * Width + X]);
				++Count;
			}
		}
		Sum /= float(Count);
		Sum.A = 1.f;
		Out = Sum.ToFColor(true);
		return true;
	}

	void HashDatFile(const FString& Path, uint32& Hash, bool& bOk)
	{
		const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
		if (!Stat.bIsValid || Stat.FileSize <= 0)
		{
			bOk = false;
			return;
		}
		Hash = HashCombine(Hash, GetTypeHash(Path.ToLower()));
		Hash = HashCombine(Hash, GetTypeHash(Stat.FileSize));
		Hash = HashCombine(Hash, GetTypeHash(Stat.ModificationTime.ToUnixTimestamp()));
	}

	int32 FarTerrainLevelDim(int32 Level) { return ((FPersonalFarTerrainBake::Dim - 1) >> Level) + 1; }
}

FPersonalFarTerrainBake::FPersonalFarTerrainBake()
{
	for (FColor& Color : Palette) Color = FColor(128, 128, 128);
	Palette[RoadCode] = FColor(115, 97, 71);
}

void FPersonalFarTerrainBake::Init()
{
	HeightIndex.Init(0, Dim * Dim);
	TerrainCode.Init(0, Dim * Dim);
	for (TArray<float>& Level : Levels) Level.Reset();
	ColorPixels.Reset();
}

float FPersonalFarTerrainBake::HeightAt(int32 Level, int32 X, int32 Y) const
{
	if (Level <= 0 || !HasLevels())
	{
		return HeightAt(X, Y);
	}
	Level = FMath::Min(Level, LevelCount - 1);
	const int32 Size = FarTerrainLevelDim(Level);
	const int32 LX = FMath::Clamp(X, 0, Dim - 1) >> Level;
	const int32 LY = FMath::Clamp(Y, 0, Dim - 1) >> Level;
	return Levels[Level - 1][LX * Size + LY];
}

void FPersonalFarTerrainBake::AddLandblock(int32 BlockX, int32 BlockY, const FACEDatCellLandblock& Land)
{
	if (!IsValid() || BlockX < 0 || BlockY < 0 || BlockX >= LandblockDim || BlockY >= LandblockDim)
	{
		return;
	}
	// Same vertex order as FACELandblockMeshBuilder::BuildLandblock. Neighbors share edge corners.
	for (int32 VX = 0; VX < BakeVertexDim; ++VX)
	{
		for (int32 VY = 0; VY < BakeVertexDim; ++VY)
		{
			const int32 Vertex = VX * BakeVertexDim + VY;
			const int32 Corner = (BlockX * CellsPerLandblock + VX) * Dim + BlockY * CellsPerLandblock + VY;
			const uint16 Terrain = Land.Terrain[Vertex];
			HeightIndex[Corner] = Land.Height[Vertex];
			TerrainCode[Corner] = (Terrain & 0x3) != 0 ? RoadCode : uint8((Terrain >> 2) & 0x1F);
		}
	}
}

void FPersonalFarTerrainBake::BuildLevels()
{
	if (!IsValid())
	{
		return;
	}
	// Each level averages the one below it (1-2-1 in both directions), so far
	// ridges become smooth lines, not one sharp sample every few hundred meters.
	static const float Weights[3] = {0.25f, 0.5f, 0.25f};
	for (int32 Level = 1; Level < LevelCount; ++Level)
	{
		const int32 Size = FarTerrainLevelDim(Level);
		const int32 Below = FarTerrainLevelDim(Level - 1);
		const TArray<float>* Source = Level > 1 ? &Levels[Level - 2] : nullptr;
		TArray<float>& Out = Levels[Level - 1];
		Out.SetNumUninitialized(Size * Size);
		for (int32 X = 0; X < Size; ++X)
		{
			for (int32 Y = 0; Y < Size; ++Y)
			{
				float Sum = 0.f;
				for (int32 A = -1; A <= 1; ++A)
				{
					const int32 SX = FMath::Clamp(2 * X + A, 0, Below - 1);
					for (int32 B = -1; B <= 1; ++B)
					{
						const int32 SY = FMath::Clamp(2 * Y + B, 0, Below - 1);
						const float Height = Source ? (*Source)[SX * Below + SY] : HeightTable[HeightIndex[SX * Dim + SY]];
						Sum += Weights[A + 1] * Weights[B + 1] * Height;
					}
				}
				Out[X * Size + Y] = Sum;
			}
		}
	}
}

void FPersonalFarTerrainBake::BuildColorPixels()
{
	if (!IsValid())
	{
		return;
	}
	ColorPixels.SetNumUninitialized(ColorTextureSize * ColorTextureSize);
	for (int32 Y = 0; Y < ColorTextureSize; ++Y)
	{
		for (int32 X = 0; X < ColorTextureSize; ++X)
		{
			ColorPixels[Y * ColorTextureSize + X] = ColorAt(X, Y);
		}
	}
}

bool FPersonalFarTerrainBake::Save(const FString& Path, uint64 Key) const
{
	if (!IsValid())
	{
		return false;
	}
	TArray<uint8> Bytes;
	Bytes.Reserve(2 * Dim * Dim + 2048);
	FMemoryWriter Writer(Bytes);
	uint32 Magic = FarTerrainCacheMagic, Version = FarTerrainCacheVersion, Size = Dim;
	Writer << Magic << Version << Key << Size;
	Writer.Serialize(const_cast<float*>(HeightTable), sizeof(HeightTable));
	Writer.Serialize(const_cast<FColor*>(Palette), sizeof(Palette));
	Writer.Serialize(const_cast<uint8*>(HeightIndex.GetData()), HeightIndex.Num());
	Writer.Serialize(const_cast<uint8*>(TerrainCode.GetData()), TerrainCode.Num());
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

bool FPersonalFarTerrainBake::Load(const FString& Path, uint64 Key)
{
	constexpr int64 HeaderBytes = 3 * sizeof(uint32) + sizeof(uint64) + sizeof(HeightTable) + sizeof(Palette);
	constexpr int64 DataBytes = int64(Dim) * Dim * 2;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent) || Bytes.Num() != HeaderBytes + DataBytes)
	{
		return false;
	}
	FMemoryReader Reader(Bytes);
	uint32 Magic = 0, Version = 0, Size = 0;
	uint64 StoredKey = 0;
	Reader << Magic << Version << StoredKey << Size;
	if (Magic != FarTerrainCacheMagic || Version != FarTerrainCacheVersion || StoredKey != Key || Size != Dim)
	{
		return false;
	}
	Init();
	Reader.Serialize(HeightTable, sizeof(HeightTable));
	Reader.Serialize(Palette, sizeof(Palette));
	Reader.Serialize(HeightIndex.GetData(), HeightIndex.Num());
	Reader.Serialize(TerrainCode.GetData(), TerrainCode.Num());
	for (uint8& Code : TerrainCode) Code = FMath::Min(Code, RoadCode);
	return !Reader.IsError();
}

namespace PersonalFarTerrainBake
{
uint64 SourceKey(const FString& DatDirectory)
{
	uint32 Hash = FarTerrainCacheVersion;
	bool bOk = true;
	HashDatFile(DatDirectory / TEXT("client_cell_1.dat"), Hash, bOk);
	const uint32 CellHash = Hash;
	HashDatFile(DatDirectory / TEXT("client_portal.dat"), Hash, bOk);
	const uint64 Key = (uint64(CellHash) << 32) | Hash;
	return bOk ? FMath::Max<uint64>(Key, 1) : 0;
}

FString CachePath(uint64 Key)
{
	return FPaths::ProjectSavedDir() / TEXT("Personal") / FString::Printf(TEXT("FarTerrain_%016llX.bin"), Key);
}

bool Build(const FString& DatDirectory, FPersonalFarTerrainBake& Out)
{
	const double Start = FPlatformTime::Seconds();
	// Own handles: the game's cell DAT is not public, and its reads share one lock
	// with terrain streaming.
	FACEDatDatabase Portal, Cell;
	if (!Portal.Open(DatDirectory / TEXT("client_portal.dat")) || !Cell.Open(DatDirectory / TEXT("client_cell_1.dat")))
	{
		return false;
	}
	const double Opened = FPlatformTime::Seconds();

	Out.Init();
	{
		TArray<float> Table;
		TArray<uint8> Bytes;
		if (!Portal.ReadFile(FACELandblockMeshBuilder::RegionFileId, Bytes))
		{
			return false;
		}
		FACEDatCursor Cursor(Bytes);
		if (!ACEDatUnpack::UnpackRegionLandHeightTable(Cursor, Table) || Table.Num() != 256)
		{
			return false;
		}
		FMemory::Memcpy(Out.HeightTable, Table.GetData(), sizeof(Out.HeightTable));
	}

	int32 TypesFound = 0;
	{
		FACEDatTextureResolver Textures(&Portal, nullptr);
		FACELandblockMeshBuilder Builder(&Portal, &Cell, &Textures);
		if (!Builder.EnsureTexMerge())
		{
			return false;
		}
		for (int32 Type = 0; Type < FPersonalFarTerrainBake::RoadCode; ++Type)
		{
			if (AverageBakedColor(Builder, FACELandblockMeshBuilder::GetPalCode(0, 0, 0, 0, Type, Type, Type, Type), Out.Palette[Type]))
			{
				++TypesFound;
			}
		}
		AverageBakedColor(Builder, FACELandblockMeshBuilder::GetPalCode(1, 1, 1, 1, 0, 0, 0, 0), Out.Palette[FPersonalFarTerrainBake::RoadCode]);
	}
	if (TypesFound == 0)
	{
		return false;
	}
	const double Colored = FPlatformTime::Seconds();

	// A hole in the DAT stays at height index 0 and terrain type 0.
	int32 Missing = 0;
	TArray<uint8> Bytes;
	FACEDatCellLandblock Land;
	for (int32 X = 0; X < FPersonalFarTerrainBake::LandblockDim; ++X)
	{
		for (int32 Y = 0; Y < FPersonalFarTerrainBake::LandblockDim; ++Y)
		{
			const uint32 FileId = (uint32(X) << 24) | (uint32(Y) << 16) | 0xFFFFu;
			if (Cell.ReadFile(FileId, Bytes))
			{
				FACEDatCursor Cursor(Bytes);
				if (ACEDatUnpack::UnpackCellLandblock(Cursor, Land))
				{
					Out.AddLandblock(X, Y, Land);
					continue;
				}
			}
			++Missing;
		}
	}
	const int32 Total = FPersonalFarTerrainBake::LandblockDim * FPersonalFarTerrainBake::LandblockDim;
	if (Missing > Total / 2)
	{
		return false;
	}
	const double Done = FPlatformTime::Seconds();
	UE_LOG(LogTemp, Log, TEXT("PersonalFarTerrain: bake %.2fs (open %.2fs, colors %.2fs for %d types, landblocks %.2fs, %d missing)"),
		Done - Start, Opened - Start, Colored - Opened, TypesFound, Done - Colored, Missing);
	return true;
}

bool LoadOrBuild(const FString& DatDirectory, FPersonalFarTerrainBake& Out)
{
	const uint64 Key = SourceKey(DatDirectory);
	if (Key != 0)
	{
		const FString Path = CachePath(Key);
		if (Out.Load(Path, Key))
		{
			return true;
		}
		if (Build(DatDirectory, Out))
		{
			// A failed save only means the next start builds again.
			Out.Save(Path, Key);
			return true;
		}
	}
	UE_LOG(LogTemp, Warning, TEXT("PersonalFarTerrain: world bake failed for '%s'; far terrain stays off"), *DatDirectory);
	return false;
}

TFuture<FBakePtr> LoadOrBuildAsync(const FString& DatDirectory)
{
	return Async(EAsyncExecution::ThreadPool, [DatDirectory]() -> FBakePtr
	{
		auto Bake = MakeShared<FPersonalFarTerrainBake, ESPMode::ThreadSafe>();
		if (!LoadOrBuild(DatDirectory, *Bake))
		{
			return FBakePtr();
		}
		const double Start = FPlatformTime::Seconds();
		Bake->BuildLevels();
		Bake->BuildColorPixels();
		UE_LOG(LogTemp, Log, TEXT("PersonalFarTerrain: world data ready (height levels and color map %.2fs)"), FPlatformTime::Seconds() - Start);
		return Bake;
	});
}
}
