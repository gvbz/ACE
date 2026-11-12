#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACELandblockMeshBuilder.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarTerrainBakeTest, "ACE.Personal.FarTerrain.Bake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarTerrainBakeTest::RunTest(const FString& Parameters)
{
	constexpr int32 Dim = FPersonalFarTerrainBake::Dim;
	// Made-up landblock: vertex N has height index N and terrain type N % 32.
	FACEDatCellLandblock Land;
	for (int32 I = 0; I < 81; ++I)
	{
		Land.Height[I] = uint8(I);
		Land.Terrain[I] = uint16((I % 32) << 2);
	}
	Land.Terrain[10] = uint16((5 << 2) | 1);

	auto BakePtr = MakeUnique<FPersonalFarTerrainBake>();
	FPersonalFarTerrainBake& Bake = *BakePtr;
	TestFalse(TEXT("Empty bake is not valid"), Bake.IsValid());
	Bake.Init();
	TestTrue(TEXT("Initialized bake is valid"), Bake.IsValid());
	for (int32 I = 0; I < 256; ++I) Bake.HeightTable[I] = I * 2.f;
	for (int32 I = 0; I < FPersonalFarTerrainBake::RoadCode; ++I) Bake.Palette[I] = FColor(I, 100, 0);
	Bake.Palette[FPersonalFarTerrainBake::RoadCode] = FColor(255, 0, 255);

	Bake.AddLandblock(3, 4, Land);
	int32 WrongHeights = 0, WrongColors = 0;
	for (int32 VX = 0; VX < 9; ++VX)
		for (int32 VY = 0; VY < 9; ++VY)
		{
			const int32 Vertex = VX * 9 + VY;
			if (Bake.HeightAt(24 + VX, 32 + VY) != Vertex * 2.f) ++WrongHeights;
			const FColor Expected = Vertex == 10 ? Bake.Palette[FPersonalFarTerrainBake::RoadCode] : Bake.Palette[Vertex % 32];
			if (Bake.ColorAt(24 + VX, 32 + VY) != Expected) ++WrongColors;
		}
	TestEqual(TEXT("All 81 corner heights of a landblock"), WrongHeights, 0);
	TestEqual(TEXT("All 81 corner colors, with the road"), WrongColors, 0);
	TestEqual(TEXT("Neighbors are untouched"), Bake.HeightAt(33, 32), 0.f);

	Bake.AddLandblock(254, 254, Land);
	TestEqual(TEXT("Last landblock reaches the last corner"), Bake.HeightAt(Dim - 1, Dim - 1), 160.f);
	TestEqual(TEXT("Outside the world clamps to the edge, high side"), Bake.HeightAt(Dim + 500, Dim + 9), 160.f);
	TestEqual(TEXT("Outside the world clamps to the edge, low side"), Bake.HeightAt(-9, Dim - 1), Bake.HeightAt(0, Dim - 1));
	TestEqual(TEXT("Outside color clamps too"), Bake.ColorAt(Dim + 30, Dim - 1), Bake.ColorAt(Dim - 1, Dim - 1));
	Bake.AddLandblock(255, 0, Land);
	Bake.AddLandblock(-1, 0, Land);
	TestEqual(TEXT("Landblocks outside the world are ignored"), Bake.HeightAt(Dim - 1, 0), 0.f);

	// Smoothed levels.
	TestFalse(TEXT("No levels before BuildLevels"), Bake.HasLevels());
	TestEqual(TEXT("Without levels every level reads the full-detail height"), Bake.HeightAt(3, 28, 36), Bake.HeightAt(28, 36));
	Bake.BuildLevels();
	TestTrue(TEXT("Levels are built"), Bake.HasLevels());
	TestEqual(TEXT("Level 0 is the full-detail height"), Bake.HeightAt(0, 28, 36), Bake.HeightAt(28, 36));
	TestEqual(TEXT("Flat ground stays flat at every level"), Bake.HeightAt(4, 800, 800), 0.f);
	// Level 1 at cell (28, 36) is the 1-2-1 average of the 3x3 corners around it.
	float Expected = 0.f;
	static const float Weights[3] = {0.25f, 0.5f, 0.25f};
	for (int32 A = -1; A <= 1; ++A)
		for (int32 B = -1; B <= 1; ++B) Expected += Weights[A + 1] * Weights[B + 1] * Bake.HeightAt(28 + A, 36 + B);
	TestEqual(TEXT("Level 1 averages its neighbors"), Bake.HeightAt(1, 28, 36), Expected, 0.001f);
	TestEqual(TEXT("Levels clamp outside the world"), Bake.HeightAt(2, Dim + 80, Dim + 80), Bake.HeightAt(2, Dim - 1, Dim - 1));

	Bake.BuildColorPixels();
	constexpr int32 Size = FPersonalFarTerrainBake::ColorTextureSize;
	if (TestEqual(TEXT("Color map size"), Bake.ColorPixels.Num(), Size * Size))
	{
		TestEqual(TEXT("Color map row is Y, column is X"), Bake.ColorPixels[33 * Size + 25], Bake.ColorAt(25, 33));
		TestEqual(TEXT("Color map repeats the edge past the world"), Bake.ColorPixels[(Size - 1) * Size + Size - 1], Bake.ColorAt(Dim - 1, Dim - 1));
	}

	// Cache file.
	const FString Path = FPaths::ProjectSavedDir() / TEXT("Automation/PersonalFarTerrainFixture.bin");
	IFileManager::Get().Delete(*Path);
	auto LoadedPtr = MakeUnique<FPersonalFarTerrainBake>();
	FPersonalFarTerrainBake& Loaded = *LoadedPtr;
	TestFalse(TEXT("Missing cache is rejected"), Loaded.Load(Path, 42));
	TestTrue(TEXT("Cache saves"), Bake.Save(Path, 42));
	TestTrue(TEXT("Cache loads with the same key"), Loaded.Load(Path, 42));
	TestTrue(TEXT("Loaded heights match"), Loaded.HeightIndex == Bake.HeightIndex
		&& FMemory::Memcmp(Loaded.HeightTable, Bake.HeightTable, sizeof(Bake.HeightTable)) == 0);
	TestTrue(TEXT("Loaded terrain and colors match"), Loaded.TerrainCode == Bake.TerrainCode
		&& FMemory::Memcmp(Loaded.Palette, Bake.Palette, sizeof(Bake.Palette)) == 0);
	TestFalse(TEXT("Cache from other DAT files is rejected"), Loaded.Load(Path, 43));
	TArray<uint8> Bytes;
	FFileHelper::LoadFileToArray(Bytes, *Path);
	Bytes.SetNum(Bytes.Num() - 100);
	FFileHelper::SaveArrayToFile(Bytes, *Path);
	TestFalse(TEXT("Truncated cache is rejected"), Loaded.Load(Path, 42));
	IFileManager::Get().Delete(*Path);
	TestFalse(TEXT("Unsaved empty bake"), FPersonalFarTerrainBake().Save(Path, 42));

	TestEqual(TEXT("No key without DAT files"), PersonalFarTerrainBake::SourceKey(FPaths::ProjectSavedDir() / TEXT("Automation/NoSuchDatFolder")), uint64(0));

	// Real DAT files: the bake must agree with the terrain the client draws.
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	const uint64 Key = PersonalFarTerrainBake::SourceKey(DatDirectory);
	if (Key == 0)
	{
		AddInfo(TEXT("DAT files not found; skipped the comparison with real terrain"));
		return !HasAnyErrors();
	}
	TestEqual(TEXT("Key is stable"), PersonalFarTerrainBake::SourceKey(DatDirectory), Key);
	FPersonalFarTerrainBake& World = Loaded;
	double Start = FPlatformTime::Seconds();
	if (!TestTrue(TEXT("World bake builds"), PersonalFarTerrainBake::Build(DatDirectory, World)))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("World bake: %.2fs"), FPlatformTime::Seconds() - Start));
	Start = FPlatformTime::Seconds();
	World.BuildLevels();
	World.BuildColorPixels();
	AddInfo(FString::Printf(TEXT("Height levels and color map: %.2fs"), FPlatformTime::Seconds() - Start));

	FACEDatDatabase Portal, Cell;
	if (!Portal.Open(DatDirectory / TEXT("client_portal.dat")) || !Cell.Open(DatDirectory / TEXT("client_cell_1.dat")))
	{
		return false;
	}
	FACELandblockMeshBuilder Builder(&Portal, &Cell, nullptr);
	TSet<uint8> Codes;
	for (uint8 Code : World.TerrainCode) Codes.Add(Code);
	TSet<uint32> DistinctColors;
	for (uint8 Code : Codes) DistinctColors.Add(World.Palette[Code].ToPackedARGB());
	TestTrue(TEXT("Terrain types have different colors"), DistinctColors.Num() >= 8);
	TestTrue(TEXT("The world has roads"), Codes.Contains(FPersonalFarTerrainBake::RoadCode));
	float MinHeight = MAX_flt, MaxHeight = -MAX_flt;
	for (uint8 Index : World.HeightIndex) { MinHeight = FMath::Min(MinHeight, World.HeightTable[Index]); MaxHeight = FMath::Max(MaxHeight, World.HeightTable[Index]); }
	TestTrue(TEXT("The world has mountains"), MaxHeight - MinHeight > 100.f);
	AddInfo(FString::Printf(TEXT("World bake: heights %.1f to %.1f, %d terrain codes, %d colors"), MinHeight, MaxHeight, Codes.Num(), DistinctColors.Num()));

	for (const FIntPoint Block : {FIntPoint(0xA9, 0xB4), FIntPoint(0xDA, 0x55), FIntPoint(0x7E, 0x65), FIntPoint(0x2B, 0x12),
		FIntPoint(0, 0), FIntPoint(254, 254), FIntPoint(254, 100), FIntPoint(100, 254)})
	{
		FACEBuiltLandblockMesh Mesh;
		const uint32 Id = (uint32(Block.X) << 24) | (uint32(Block.Y) << 16);
		if (!TestTrue(FString::Printf(TEXT("Landblock %08X builds"), Id), Builder.BuildLandblock(Id, 100.f, Mesh) && Mesh.bHasHeights))
		{
			continue;
		}
		int32 Wrong = 0;
		for (int32 VX = 0; VX < 9; ++VX)
			for (int32 VY = 0; VY < 9; ++VY)
				if (World.HeightAt(Block.X * 8 + VX, Block.Y * 8 + VY) != Mesh.HeightsAc[VX * 9 + VY]) ++Wrong;
		TestEqual(FString::Printf(TEXT("Landblock %08X: all 81 heights match the client's terrain"), Id), Wrong, 0);
	}
	return !HasAnyErrors();
}
#endif
