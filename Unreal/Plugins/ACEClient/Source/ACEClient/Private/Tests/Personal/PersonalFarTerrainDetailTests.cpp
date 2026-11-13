#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "Personal/PersonalFarTerrainDetail.h"
#include "Personal/PersonalFarTerrainMesh.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACELandblockMeshBuilder.h"
#include "ACERuntimeOptions.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace
{
	using FFarDetailUpload = FPersonalFarTerrainDetail::FUpload;
	constexpr int32 FarDetailTestSize = PersonalFarTerrainDetail::TextureSize;

	int32 FarDetailTestMod(int32 Value, int32 Step) { return ((Value % Step) + Step) % Step; }

	/** The upload of one mip that holds texture pixel (X, Y) of that mip. */
	const FColor* FarDetailTestPixel(const TArray<FFarDetailUpload>& Uploads, int32 Mip, int32 X, int32 Y)
	{
		for (const FFarDetailUpload& Upload : Uploads)
		{
			if (Upload.Mip == Mip && X >= Upload.X && X < Upload.X + Upload.Width && Y >= Upload.Y && Y < Upload.Y + Upload.Height)
			{
				return &Upload.Pixels[(Y - Upload.Y) * Upload.Width + X - Upload.X];
			}
		}
		return nullptr;
	}

	/** Every pixel of a world cell at mip 0 must be the fake tile, turned the way loaded terrain maps it. */
	int32 FarDetailTestWrongPixels(const TArray<FFarDetailUpload>& Uploads, const FPersonalFarTerrainBake& Bake, int32 CellX, int32 CellY)
	{
		constexpr int32 P = PersonalFarTerrainDetail::CellPixels;
		const uint8 Blue = uint8(Bake.PalCodeAt(CellX, CellY) & 0xFF);
		int32 Wrong = 0;
		for (int32 I = 0; I < P; ++I)
		{
			for (int32 J = 0; J < P; ++J)
			{
				const FColor* Pixel = FarDetailTestPixel(Uploads, 0,
					FarDetailTestMod(CellX * P + I, FarDetailTestSize), FarDetailTestMod(CellY * P + J, FarDetailTestSize));
				// Texture column I is x, so tile column 15 - I (U = 1 - x). The same for rows.
				if (!Pixel || *Pixel != FColor(uint8(16 * (P - 1 - I) + 4), uint8(16 * (P - 1 - J) + 4), Blue, 255)) ++Wrong;
			}
		}
		return Wrong;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarTerrainDetailTest, "ACE.Personal.FarTerrain.Detail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarTerrainDetailTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFarTerrainDetail;
	constexpr int32 Dim = FPersonalFarTerrainBake::Dim;

	// The palette code of a cell: corners in the order loaded terrain uses.
	FACEDatCellLandblock Land;
	for (int32 I = 0; I < 81; ++I)
	{
		Land.Height[I] = 0;
		Land.Terrain[I] = uint16(((I % 32) << 2) | 0x800);
	}
	Land.Terrain[10] = uint16((5 << 2) | 1);
	auto BakePtr = MakeUnique<FPersonalFarTerrainBake>();
	FPersonalFarTerrainBake& Bake = *BakePtr;
	Bake.Init();
	Bake.AddLandblock(100, 60, Land);
	TestTrue(TEXT("Palette code: four corners with a road"), Bake.PalCodeAt(800, 480) ==
		FACELandblockMeshBuilder::GetPalCode(0, 0, 1, 0, 0, 9, 5, 1));
	TestTrue(TEXT("Palette code: plain cell"), Bake.PalCodeAt(803, 482) ==
		FACELandblockMeshBuilder::GetPalCode(0, 0, 0, 0, 29 % 32, 38 % 32, 39 % 32, 30 % 32));
	TestTrue(TEXT("Road corners still take the road color"), Bake.TerrainCode[(800 + 1) * Dim + 480 + 1] == FPersonalFarTerrainBake::RoadCode);
	TestTrue(TEXT("Outside the world the edge cell repeats"), Bake.PalCodeAt(-50, 480) == Bake.PalCodeAt(-1, 480));

	TestEqual(TEXT("32 landblocks across"), BlocksAcross, 32);
	const FIntPoint Center(100, 60);
	TestTrue(TEXT("Mesh cells"), MeshCells(Center) == FIntRect(FIntPoint(86, 46) * 8, FIntPoint(115, 75) * 8));
	TestTrue(TEXT("A texture one landblock away covers the mesh"), Covers(Center + FIntPoint(1, -1), Center));
	TestFalse(TEXT("A texture two landblocks away does not"), Covers(Center + FIntPoint(2, 0), Center));

	// A fake 32-pixel texture per code: red is the column, green the row, blue the code.
	int32 Bakes = 0;
	const FPersonalFarTerrainDetail::FBakeTile Source = [&Bakes](uint32 PalCode, TArray<FColor>& OutPixels, int32& OutSize)
	{
		++Bakes;
		OutSize = 32;
		OutPixels.SetNumUninitialized(32 * 32);
		for (int32 Y = 0; Y < 32; ++Y)
			for (int32 X = 0; X < 32; ++X) OutPixels[Y * 32 + X] = FColor(uint8(X * 8), uint8(Y * 8), uint8(PalCode & 0xFF), 0);
		return true;
	};

	FPersonalFarTerrainDetail Detail;
	TArray<FFarDetailUpload> Uploads;
	Detail.Update(Bake, Center, Source, Uploads);
	if (!TestEqual(TEXT("First update: every mip, whole"), Uploads.Num(), MipCount))
	{
		return false;
	}
	for (int32 Mip = 0; Mip < MipCount; ++Mip)
	{
		const FFarDetailUpload& Upload = Uploads[Mip];
		TestTrue(FString::Printf(TEXT("First update: mip %d is whole"), Mip), Upload.Mip == Mip && Upload.X == 0 && Upload.Y == 0
			&& Upload.Width == FarDetailTestSize >> Mip && Upload.Height == Upload.Width && Upload.Pixels.Num() == Upload.Width * Upload.Width);
	}
	TestEqual(TEXT("One tile per palette code"), Bakes, Detail.TileCount());
	TestTrue(TEXT("The made-up landblock has many codes"), Detail.TileCount() > 40);
	TestEqual(TEXT("Cell with a road: pixels and orientation"), FarDetailTestWrongPixels(Uploads, Bake, 800, 480), 0);
	TestEqual(TEXT("Plain cell: pixels and orientation"), FarDetailTestWrongPixels(Uploads, Bake, 803, 482), 0);
	TestEqual(TEXT("Lowest landblock in range"), FarDetailTestWrongPixels(Uploads, Bake, 84 * 8, 44 * 8), 0);
	TestEqual(TEXT("Highest landblock in range"), FarDetailTestWrongPixels(Uploads, Bake, 115 * 8 + 7, 75 * 8 + 7), 0);
	// Mip 1 halves the cell: the average of two tile columns and rows.
	if (const FColor* Pixel = FarDetailTestPixel(Uploads, 1, FarDetailTestMod(803 * 8 + 2, FarDetailTestSize / 2), FarDetailTestMod(482 * 8 + 5, FarDetailTestSize / 2)))
	{
		TestEqual(TEXT("Mip 1 of a cell"), *Pixel, FColor(uint8(32 * (7 - 2) + 12), uint8(32 * (7 - 5) + 12), uint8(Bake.PalCodeAt(803, 482) & 0xFF), 255));
	}
	// Mip 4 is one pixel per cell.
	if (const FColor* Pixel = FarDetailTestPixel(Uploads, 4, FarDetailTestMod(803, CellsAcross), FarDetailTestMod(482, CellsAcross)))
	{
		TestTrue(TEXT("Mip 4 of a cell: blue is the code"), Pixel->B == uint8(Bake.PalCodeAt(803, 482) & 0xFF));
	}

	Detail.Update(Bake, Center, Source, Uploads);
	TestEqual(TEXT("Same center: nothing to do"), Uploads.Num(), 0);

	// One landblock east: one column of landblocks comes in and takes the slots of the column that left.
	const int32 BakesBefore = Bakes;
	Detail.Update(Bake, Center + FIntPoint(1, 0), Source, Uploads);
	TestEqual(TEXT("One step: 32 landblocks in 5 mips, and the small mips"), Uploads.Num(), 32 * CellMips + MipCount - CellMips);
	TestEqual(TEXT("One step: no new tiles for known codes"), Bakes, BakesBefore);
	TestEqual(TEXT("One step: the new column is written"), FarDetailTestWrongPixels(Uploads, Bake, 116 * 8 + 3, 60 * 8 + 3), 0);
	TestTrue(TEXT("One step: the rest is not sent again"), FarDetailTestPixel(Uploads, 0, FarDetailTestMod(803 * 16, FarDetailTestSize), FarDetailTestMod(482 * 16, FarDetailTestSize)) == nullptr);
	if (Uploads.Num() > 0)
	{
		TestTrue(TEXT("One step: parts are one landblock"), Uploads[0].Mip == 0 && Uploads[0].Width == 128 && Uploads[0].X % 128 == 0 && Uploads[0].Pixels.Num() == 128 * 128);
	}

	Detail.Update(Bake, FIntPoint(10, 200), Source, Uploads);
	TestEqual(TEXT("Far jump: every mip, whole"), Uploads.Num(), MipCount);
	TestEqual(TEXT("Far jump: pixels"), FarDetailTestWrongPixels(Uploads, Bake, 10 * 8, 200 * 8), 0);

	// A source that fails gives the flat color, not a hole.
	Bake.Palette[0] = FColor(10, 20, 30);
	FPersonalFarTerrainDetail Flat;
	Flat.Update(Bake, FIntPoint(200, 200), [](uint32, TArray<FColor>&, int32&) { return false; }, Uploads);
	if (const FColor* Pixel = FarDetailTestPixel(Uploads, 0, 5, 5))
	{
		TestEqual(TEXT("Failed bake: flat color"), *Pixel, FColor(10, 20, 30, 255));
	}

	// Tile cache file.
	const FString Path = FPaths::ProjectSavedDir() / TEXT("Automation/PersonalFarTerrainTilesFixture.bin");
	IFileManager::Get().Delete(*Path);
	TestTrue(TEXT("New tiles are unsaved"), Detail.UnsavedTiles() > 0);
	TestTrue(TEXT("Tiles save"), Detail.SaveTiles(Path, 42));
	TestEqual(TEXT("Saved tiles are not unsaved"), Detail.UnsavedTiles(), 0);
	FPersonalFarTerrainDetail Loaded;
	TestFalse(TEXT("Tiles from other DAT files are rejected"), Loaded.LoadTiles(Path, 43));
	TestTrue(TEXT("Tiles load"), Loaded.LoadTiles(Path, 42));
	TestEqual(TEXT("All tiles loaded"), Loaded.TileCount(), Detail.TileCount());
	Bakes = 0;
	Loaded.Update(Bake, Center, Source, Uploads);
	TestEqual(TEXT("Loaded tiles are used, none is baked again"), Bakes, 0);
	TestEqual(TEXT("Loaded tiles give the same pixels"), FarDetailTestWrongPixels(Uploads, Bake, 800, 480), 0);
	IFileManager::Get().Delete(*Path);

	// The mesh: quads inside the detail cells go to the detail part, nothing is lost or doubled.
	{
		Bake.BuildLevels();
		TSet<FIntPoint> Holes;
		for (int32 X = -5; X <= 5; ++X)
			for (int32 Y = -5; Y <= 5; ++Y) Holes.Add(Center + FIntPoint(X, Y));
		FPersonalFarTerrainMesh Plain, Split;
		PersonalFarTerrainMesh::Build(Bake, Center, 48, Holes, 7, 100.f, Plain);
		const FIntRect Cells = MeshCells(Center + FIntPoint(1, 0));
		PersonalFarTerrainMesh::Build(Bake, Center, 48, Holes, 7, 100.f, Split, &Cells);
		TestEqual(TEXT("Mesh without detail cells has no detail part"), Plain.DetailTriangles.Num(), 0);
		TestEqual(TEXT("Split mesh: same vertices"), Split.Vertices.Num(), Plain.Vertices.Num());
		TestEqual(TEXT("Split mesh: same triangles in all"), Split.Triangles.Num() + Split.DetailTriangles.Num(), Plain.Triangles.Num());
		TestTrue(TEXT("Split mesh: both parts have triangles"), Split.Triangles.Num() > 0 && Split.DetailTriangles.Num() > 0);
		int32 Outside = 0, Inside = 0, WrongUV = 0;
		for (int32 Index : Split.DetailTriangles)
		{
			const FIntPoint C = Split.Corners[Index];
			if (C.X < Cells.Min.X || C.X > Cells.Max.X || C.Y < Cells.Min.Y || C.Y > Cells.Max.Y) ++Outside;
		}
		for (int32 I = 0; I + 2 < Split.Triangles.Num(); I += 3)
		{
			bool bAllInside = true;
			for (int32 K = 0; K < 3; ++K)
			{
				const FIntPoint C = Split.Corners[Split.Triangles[I + K]];
				bAllInside &= C.X >= Cells.Min.X && C.X <= Cells.Max.X && C.Y >= Cells.Min.Y && C.Y <= Cells.Max.Y;
			}
			if (bAllInside) ++Inside;
		}
		for (int32 I = 0; I < Split.Vertices.Num(); ++I)
			if (Split.DetailUVs[I] != CornerUV(Split.Corners[I].X, Split.Corners[I].Y)) ++WrongUV;
		TestEqual(TEXT("Split mesh: detail part stays inside the detail cells"), Outside, 0);
		TestEqual(TEXT("Split mesh: nothing inside the detail cells is left flat"), Inside, 0);
		TestTrue(TEXT("Split mesh: one detail coordinate per vertex"), Split.DetailUVs.Num() == Split.Vertices.Num() && WrongUV == 0);
		TestTrue(TEXT("Detail coordinate: 256 cells across the texture"), CornerUV(256, 512) == FVector2D(1., 2.));
	}

	// The setting. A separate settings file: this test must not change player preferences.
	{
		TGuardValue<FString> SettingsPath(GGameUserSettingsIni, FPaths::ProjectSavedDir() / TEXT("Automation/PersonalFarTerrainDetailFixture.ini"));
		FConfigFile FixtureConfig; FixtureConfig.NoSave = false; FixtureConfig.bCanSaveAllSections = true;
		GConfig->SetFile(GGameUserSettingsIni, &FixtureConfig);
		TestTrue(TEXT("Far Terrain Textures is on by default"), Enabled());
		ACERuntimeOptions::Set(TEXT("PersonalFarTerrainTextures"), 0.f);
		TestFalse(TEXT("Far Terrain Textures off"), Enabled());
		ACERuntimeOptions::Set(TEXT("PersonalFarTerrainTextures"), 1.f);
		TestTrue(TEXT("Far Terrain Textures on again"), Enabled());
	}

	// Real DAT files: the same textures as the terrain the client draws, and what they cost.
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (PersonalFarTerrainBake::SourceKey(DatDirectory) == 0)
	{
		AddInfo(TEXT("DAT files not found; skipped the comparison with real terrain"));
		return !HasAnyErrors();
	}
	FPersonalFarTerrainBake& World = Bake;
	if (!TestTrue(TEXT("World bake builds"), PersonalFarTerrainBake::Build(DatDirectory, World)))
	{
		return false;
	}
	FACEDatDatabase Portal, Cell;
	if (!Portal.Open(DatDirectory / TEXT("client_portal.dat")) || !Cell.Open(DatDirectory / TEXT("client_cell_1.dat")))
	{
		return false;
	}
	FACELandblockMeshBuilder Builder(&Portal, &Cell, nullptr);
	for (const FIntPoint Block : {FIntPoint(0xA9, 0xB4), FIntPoint(0xDA, 0x55), FIntPoint(0x7E, 0x65), FIntPoint(0x8A, 0x0E), FIntPoint(254, 100)})
	{
		FACEBuiltLandblockMesh Mesh;
		const uint32 Id = (uint32(Block.X) << 24) | (uint32(Block.Y) << 16);
		if (!TestTrue(FString::Printf(TEXT("Landblock %08X builds"), Id), Builder.BuildLandblock(Id, 100.f, Mesh)))
		{
			continue;
		}
		TSet<uint32> Drawn, Baked;
		for (const FACEBuiltLandblockSection& Section : Mesh.Sections) Drawn.Add(Section.PCode);
		for (int32 CX = 0; CX < 8; ++CX)
			for (int32 CY = 0; CY < 8; ++CY) Baked.Add(World.PalCodeAt(Block.X * 8 + CX, Block.Y * 8 + CY));
		TestTrue(FString::Printf(TEXT("Landblock %08X: the same palette codes as the client's terrain (%d)"), Id, Drawn.Num()),
			Drawn.Num() == Baked.Num() && Drawn.Includes(Baked));
	}

	// Mizuda, the baseline spot: a first texture with no tile cache, then one step.
	FPersonalFarTerrainDetail Real;
	const FPersonalFarTerrainDetail::FBakeTile Dat = MakeDatTileSource(DatDirectory);
	const FIntPoint Mizuda(0x8A, 0x0E);
	double Start = FPlatformTime::Seconds();
	Real.Update(World, Mizuda, Dat, Uploads);
	const double First = FPlatformTime::Seconds() - Start;
	TestEqual(TEXT("Real first update: every mip"), Uploads.Num(), MipCount);
	TArray<FColor> Pixels;
	int32 TileSize = 0;
	TestTrue(TEXT("Real terrain texture bakes"), Dat(World.PalCodeAt(Mizuda.X * 8, Mizuda.Y * 8 + 60), Pixels, TileSize) && TileSize >= CellPixels);
	TSet<uint32> Colors;
	if (Uploads.Num() == MipCount)
	{
		for (int32 I = 0; I < Uploads[4].Pixels.Num(); I += 7) Colors.Add(Uploads[4].Pixels[I].ToPackedARGB());
	}
	TestTrue(TEXT("Real texture has many colors"), Colors.Num() > 50);
	Start = FPlatformTime::Seconds();
	Real.Update(World, Mizuda + FIntPoint(1, 0), Dat, Uploads);
	const double Step = FPlatformTime::Seconds() - Start;
	AddInfo(FString::Printf(TEXT("Detail texture at Mizuda: first %.2fs for %d tiles of %d px source; one step %.1f ms, %d parts, %d tiles"),
		First, Real.TileCount(), TileSize, Step * 1000., Uploads.Num(), Real.TileCount()));
	return !HasAnyErrors();
}
#endif
