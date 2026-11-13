#include "Personal/PersonalFarTerrainDetail.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "ACERuntimeOptions.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACELandblockMeshBuilder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr uint32 FarDetailCacheMagic = 0x44464150; // "PAFD"
	constexpr uint32 FarDetailCacheVersion = 1;
	constexpr int32 FarDetailBlockCells = FPersonalFarTerrainBake::CellsPerLandblock;
	/** Pixels across one landblock at mip 0. */
	constexpr int32 FarDetailBlockPixels = FarDetailBlockCells * PersonalFarTerrainDetail::CellPixels;

	int32 FarDetailMod(int32 Value, int32 Step) { return ((Value % Step) + Step) % Step; }

	/** DAT handles of its own, like the world bake: the game's are not public. */
	struct FFarDetailDatSource
	{
		FString Directory;
		FACEDatDatabase Portal, Cell;
		TUniquePtr<FACEDatTextureResolver> Textures;
		TUniquePtr<FACELandblockMeshBuilder> Builder;
		bool bTried = false;

		bool Bake(uint32 PalCode, TArray<FColor>& OutPixels, int32& OutSize)
		{
			if (!bTried)
			{
				bTried = true;
				if (Portal.Open(Directory / TEXT("client_portal.dat")) && Cell.Open(Directory / TEXT("client_cell_1.dat")))
				{
					Textures = MakeUnique<FACEDatTextureResolver>(&Portal, nullptr);
					Builder = MakeUnique<FACELandblockMeshBuilder>(&Portal, &Cell, Textures.Get());
					if (!Builder->EnsureTexMerge())
					{
						Builder.Reset();
					}
				}
			}
			int32 Height = 0;
			return Builder && Builder->BakePCode(PalCode, OutPixels, OutSize, Height) && Height == OutSize;
		}
	};
}

namespace PersonalFarTerrainDetail
{
	bool Enabled()
	{
		return ACERuntimeOptions::Get(TEXT("PersonalFarTerrainTextures")) > 0.5f;
	}

	FIntRect MeshCells(FIntPoint TextureCenter)
	{
		return FIntRect((TextureCenter - FIntPoint(MeshRings, MeshRings)) * FarDetailBlockCells,
			(TextureCenter + FIntPoint(MeshRings + 1, MeshRings + 1)) * FarDetailBlockCells);
	}

	bool Covers(FIntPoint TextureCenter, FIntPoint MeshCenter)
	{
		return FMath::Abs(TextureCenter.X - MeshCenter.X) <= 1 && FMath::Abs(TextureCenter.Y - MeshCenter.Y) <= 1;
	}

	FPersonalFarTerrainDetail::FBakeTile MakeDatTileSource(const FString& DatDirectory)
	{
		TSharedRef<FFarDetailDatSource, ESPMode::ThreadSafe> Source = MakeShared<FFarDetailDatSource, ESPMode::ThreadSafe>();
		Source->Directory = DatDirectory;
		return [Source](uint32 PalCode, TArray<FColor>& OutPixels, int32& OutSize)
		{
			return Source->Bake(PalCode, OutPixels, OutSize);
		};
	}
}

const TArray<FColor>& FPersonalFarTerrainDetail::Tile(const FPersonalFarTerrainBake& Bake, int32 CellX, int32 CellY, const FBakeTile& BakeTile)
{
	using namespace PersonalFarTerrainDetail;
	const uint32 PalCode = Bake.PalCodeAt(CellX, CellY);
	if (const TArray<FColor>* Found = Tiles.Find(PalCode))
	{
		return *Found;
	}
	TArray<FColor> Pixels;
	int32 Size = 0;
	const bool bBaked = BakeTile && BakeTile(PalCode, Pixels, Size) && Size >= CellPixels
		&& FMath::IsPowerOfTwo(Size) && Pixels.Num() == Size * Size;
	if (!bBaked)
	{
		// No texture for this code: the flat color the far terrain has without detail.
		Size = CellPixels;
		Pixels.Init(Bake.ColorAt(CellX, CellY), Size * Size);
	}
	// The same filter as the mips of loaded terrain.
	TArray<FColor> Smaller;
	while (Size > CellPixels)
	{
		FACEDatTextureResolver::BuildOpaqueMip(Pixels, Size, Size, Smaller);
		Pixels = MoveTemp(Smaller);
		Size /= 2;
	}
	TArray<FColor>& Out = Tiles.Add(PalCode);
	Out.Reserve(TilePixels);
	for (int32 Mip = 0; Mip < CellMips; ++Mip)
	{
		// Loaded terrain maps the texture with U = 1 - x and V = 1 - y across the cell
		// (BuildLandblock, CornerU and CornerV). Here the column is x and the row is y.
		for (int32 Row = 0; Row < Size; ++Row)
		{
			for (int32 Column = 0; Column < Size; ++Column)
			{
				FColor Pixel = Pixels[(Size - 1 - Row) * Size + (Size - 1 - Column)];
				Pixel.A = 255;
				Out.Add(Pixel);
			}
		}
		if (Size > 1)
		{
			FACEDatTextureResolver::BuildOpaqueMip(Pixels, Size, Size, Smaller);
			Pixels = MoveTemp(Smaller);
			Size /= 2;
		}
	}
	++Unsaved;
	return Out;
}

void FPersonalFarTerrainDetail::Update(const FPersonalFarTerrainBake& Bake, FIntPoint Center, const FBakeTile& BakeTile, TArray<FUpload>& Out)
{
	using namespace PersonalFarTerrainDetail;
	Out.Reset();
	if (!Bake.IsValid())
	{
		return;
	}
	if (SlotBlock.Num() == 0)
	{
		SlotBlock.Init(FIntPoint(MIN_int32, MIN_int32), BlocksAcross * BlocksAcross);
		LastCellMip.Init(FColor::Black, CellsAcross * CellsAcross);
	}

	const FIntPoint First = Center - FIntPoint(TextureRings, TextureRings);
	TArray<FIntPoint> Changed;
	for (int32 X = 0; X < BlocksAcross; ++X)
	{
		for (int32 Y = 0; Y < BlocksAcross; ++Y)
		{
			const FIntPoint Block = First + FIntPoint(X, Y);
			if (SlotBlock[FarDetailMod(Block.X, BlocksAcross) * BlocksAcross + FarDetailMod(Block.Y, BlocksAcross)] != Block)
			{
				Changed.Add(Block);
			}
		}
	}
	if (Changed.Num() == 0)
	{
		return;
	}
	// A large change is sent as whole mips, so everything is written again.
	const bool bWhole = Changed.Num() > BlocksAcross * BlocksAcross / 4;
	if (bWhole)
	{
		Changed.Reset();
		for (int32 X = 0; X < BlocksAcross; ++X)
		{
			for (int32 Y = 0; Y < BlocksAcross; ++Y) Changed.Add(First + FIntPoint(X, Y));
		}
		Out.SetNum(CellMips);
		for (int32 Mip = 0; Mip < CellMips; ++Mip)
		{
			FUpload& Upload = Out[Mip];
			Upload.Mip = Mip;
			Upload.Width = Upload.Height = TextureSize >> Mip;
			Upload.Pixels.SetNumUninitialized(Upload.Width * Upload.Height);
		}
	}
	else
	{
		Out.Reserve(Changed.Num() * CellMips + MipCount - CellMips);
	}

	for (const FIntPoint Block : Changed)
	{
		const FIntPoint Slot(FarDetailMod(Block.X, BlocksAcross), FarDetailMod(Block.Y, BlocksAcross));
		SlotBlock[Slot.X * BlocksAcross + Slot.Y] = Block;
		// Where this landblock's pixels go, per cell mip.
		FColor* Target[CellMips];
		int32 Stride[CellMips];
		for (int32 Mip = 0; Mip < CellMips; ++Mip)
		{
			const int32 BlockPixels = FarDetailBlockPixels >> Mip;
			if (bWhole)
			{
				Stride[Mip] = Out[Mip].Width;
				Target[Mip] = Out[Mip].Pixels.GetData() + Slot.Y * BlockPixels * Stride[Mip] + Slot.X * BlockPixels;
			}
			else
			{
				FUpload& Upload = Out.AddDefaulted_GetRef();
				Upload.Mip = Mip;
				Upload.X = Slot.X * BlockPixels;
				Upload.Y = Slot.Y * BlockPixels;
				Upload.Width = Upload.Height = BlockPixels;
				Upload.Pixels.SetNumUninitialized(BlockPixels * BlockPixels);
				Stride[Mip] = BlockPixels;
				Target[Mip] = Upload.Pixels.GetData();
			}
		}
		for (int32 CX = 0; CX < FarDetailBlockCells; ++CX)
		{
			for (int32 CY = 0; CY < FarDetailBlockCells; ++CY)
			{
				const FColor* Source = Tile(Bake, Block.X * FarDetailBlockCells + CX, Block.Y * FarDetailBlockCells + CY, BakeTile).GetData();
				for (int32 Mip = 0; Mip < CellMips; ++Mip)
				{
					const int32 Size = CellPixels >> Mip;
					for (int32 Row = 0; Row < Size; ++Row)
					{
						FMemory::Memcpy(Target[Mip] + (CY * Size + Row) * Stride[Mip] + CX * Size, Source + Row * Size, Size * sizeof(FColor));
					}
					Source += Size * Size;
				}
				LastCellMip[(Slot.Y * FarDetailBlockCells + CY) * CellsAcross + Slot.X * FarDetailBlockCells + CX] = Source[-1];
			}
		}
	}

	// The mips smaller than one pixel per cell mix cells: make them again from that mip.
	TArray<FColor> Pixels = LastCellMip, Smaller;
	int32 Size = CellsAcross;
	for (int32 Mip = CellMips; Mip < MipCount && Size > 1; ++Mip)
	{
		FACEDatTextureResolver::BuildOpaqueMip(Pixels, Size, Size, Smaller);
		Pixels = MoveTemp(Smaller);
		Size /= 2;
		FUpload& Upload = Out.AddDefaulted_GetRef();
		Upload.Mip = Mip;
		Upload.Width = Upload.Height = Size;
		Upload.Pixels = Pixels;
	}
}

FString FPersonalFarTerrainDetail::TileCachePath(uint64 Key)
{
	return FPaths::ProjectSavedDir() / TEXT("Personal") / FString::Printf(TEXT("FarTerrainTiles_%016llX.bin"), Key);
}

bool FPersonalFarTerrainDetail::SaveTiles(const FString& Path, uint64 Key)
{
	TArray<uint8> Bytes;
	Bytes.Reserve(32 + Tiles.Num() * (sizeof(uint32) + TilePixels * sizeof(FColor)));
	FMemoryWriter Writer(Bytes);
	uint32 Magic = FarDetailCacheMagic, Version = FarDetailCacheVersion, Pixels = TilePixels, Count = Tiles.Num();
	Writer << Magic << Version << Key << Pixels << Count;
	for (const auto& Pair : Tiles)
	{
		uint32 Code = Pair.Key;
		Writer << Code;
		Writer.Serialize(const_cast<FColor*>(Pair.Value.GetData()), TilePixels * sizeof(FColor));
	}
	if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
	{
		return false;
	}
	Unsaved = 0;
	return true;
}

bool FPersonalFarTerrainDetail::LoadTiles(const FString& Path, uint64 Key)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		return false;
	}
	FMemoryReader Reader(Bytes);
	uint32 Magic = 0, Version = 0, Pixels = 0, Count = 0;
	uint64 StoredKey = 0;
	Reader << Magic << Version << StoredKey << Pixels << Count;
	constexpr int64 EntryBytes = sizeof(uint32) + TilePixels * sizeof(FColor);
	if (Reader.IsError() || Magic != FarDetailCacheMagic || Version != FarDetailCacheVersion || StoredKey != Key
		|| Pixels != TilePixels || Bytes.Num() - Reader.Tell() != int64(Count) * EntryBytes)
	{
		return false;
	}
	for (uint32 I = 0; I < Count; ++I)
	{
		uint32 Code = 0;
		Reader << Code;
		TArray<FColor>& Tile = Tiles.FindOrAdd(Code);
		Tile.SetNumUninitialized(TilePixels);
		Reader.Serialize(Tile.GetData(), TilePixels * sizeof(FColor));
	}
	Unsaved = 0;
	return !Reader.IsError();
}
