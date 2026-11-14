#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalFarTerrain.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "Personal/PersonalFarTerrainMesh.h"
#include "ACERuntimeOptions.h"
#include "Dat/ACELandblockMeshBuilder.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace
{
	constexpr float FarShellScale = 100.f;
	constexpr int32 FarShellCells = FPersonalFarTerrainBake::CellsPerLandblock;

	int32 FarShellFloorDiv(int32 Value, int32 Step)
	{
		return (Value - (((Value % Step) + Step) % Step)) / Step;
	}

	FIntPoint FarShellBlockOfCell(int32 X, int32 Y)
	{
		return FIntPoint(FarShellFloorDiv(X, FarShellCells), FarShellFloorDiv(Y, FarShellCells));
	}

	TSet<FIntPoint> FarShellSquareHoles(FIntPoint Center, int32 Radius)
	{
		TSet<FIntPoint> Holes;
		for (int32 X = -Radius; X <= Radius; ++X)
			for (int32 Y = -Radius; Y <= Radius; ++Y) Holes.Add(Center + FIntPoint(X, Y));
		return Holes;
	}

	bool FarShellOnBorder(const FPersonalFarTerrainMesh::FLevel& Level, FIntPoint A, FIntPoint B)
	{
		return (A.X == B.X && (A.X == Level.Min.X || A.X == Level.Max.X))
			|| (A.Y == B.Y && (A.Y == Level.Min.Y || A.Y == Level.Max.Y));
	}

	int32 FarShellArea(const FPersonalFarTerrainMesh::FLevel& Level)
	{
		return (Level.Max.X - Level.Min.X) * (Level.Max.Y - Level.Min.Y);
	}

	/** Triangle indices the level boxes and the holes must give. */
	int32 FarShellExpectedIndices(const FPersonalFarTerrainMesh& Mesh, int32 HoleCount)
	{
		int32 Quads = -HoleCount * FarShellCells * FarShellCells;
		int32 Inner = 0;
		for (const auto& Level : Mesh.Levels)
		{
			Quads += (FarShellArea(Level) - Inner) / (Level.Step * Level.Step);
			Inner = FarShellArea(Level);
		}
		return Quads * 6;
	}

	/**
	 * Every open edge must be the outer edge, the edge of a hole, or a border between
	 * two spacing levels where the finer side lies exactly on the coarser edge.
	 */
	void FarShellCheckClosed(FAutomationTestBase& Test, const FString& Name, const FPersonalFarTerrainMesh& Mesh, const TSet<FIntPoint>& Holes)
	{
		TMap<FIntPoint, int32> VertexOfCorner;
		for (int32 I = 0; I < Mesh.Corners.Num(); ++I) VertexOfCorner.Add(Mesh.Corners[I], I);
		Test.TestEqual(Name + TEXT("one vertex per corner"), VertexOfCorner.Num(), Mesh.Vertices.Num());

		TMap<uint64, int32> EdgeUses;
		EdgeUses.Reserve(Mesh.Triangles.Num());
		for (int32 I = 0; I + 2 < Mesh.Triangles.Num(); I += 3)
		{
			for (int32 E = 0; E < 3; ++E)
			{
				const uint32 A = Mesh.Triangles[I + E], B = Mesh.Triangles[I + (E + 1) % 3];
				EdgeUses.FindOrAdd((uint64(FMath::Min(A, B)) << 32) | FMath::Max(A, B))++;
			}
		}
		int32 Overused = 0, Unexplained = 0, Cracks = 0;
		for (const auto& Pair : EdgeUses)
		{
			if (Pair.Value > 2) ++Overused;
			if (Pair.Value != 1) continue;
			const int32 IA = int32(Pair.Key >> 32), IB = int32(Pair.Key & 0xFFFFFFFFu);
			const FIntPoint A = Mesh.Corners[IA], B = Mesh.Corners[IB];
			if (FarShellOnBorder(Mesh.Levels.Last(), A, B)) continue;
			bool bExplained = false;
			for (int32 L = 0; L + 1 < Mesh.Levels.Num() && !bExplained; ++L)
			{
				if (!FarShellOnBorder(Mesh.Levels[L], A, B)) continue;
				bExplained = true;
				const int32 Length = FMath::Abs(A.X - B.X) + FMath::Abs(A.Y - B.Y);
				if (Length != Mesh.Levels[L + 1].Step) continue;
				// Coarse side: the finer level's middle vertex must sit on this edge.
				const int32* Middle = VertexOfCorner.Find(FIntPoint((A.X + B.X) / 2, (A.Y + B.Y) / 2));
				if (!Middle || !FMath::IsNearlyEqual(Mesh.Vertices[*Middle].Z, 0.5 * (Mesh.Vertices[IA].Z + Mesh.Vertices[IB].Z), 0.01)) ++Cracks;
			}
			if (bExplained) continue;
			// Edge of a hole: one cell long, with a loaded landblock on one side.
			if (A.X == B.X && FMath::Abs(A.Y - B.Y) == 1)
			{
				const int32 Y = FMath::Min(A.Y, B.Y);
				bExplained = Holes.Contains(FarShellBlockOfCell(A.X, Y)) || Holes.Contains(FarShellBlockOfCell(A.X - 1, Y));
			}
			else if (A.Y == B.Y && FMath::Abs(A.X - B.X) == 1)
			{
				const int32 X = FMath::Min(A.X, B.X);
				bExplained = Holes.Contains(FarShellBlockOfCell(X, A.Y)) || Holes.Contains(FarShellBlockOfCell(X, A.Y - 1));
			}
			if (!bExplained) ++Unexplained;
		}
		Test.TestEqual(Name + TEXT("no edge used by more than two triangles"), Overused, 0);
		Test.TestEqual(Name + TEXT("no open edge inside the shell"), Unexplained, 0);
		Test.TestEqual(Name + TEXT("no crack between spacing levels"), Cracks, 0);
		Test.TestEqual(Name + TEXT("triangles"), Mesh.Triangles.Num(), FarShellExpectedIndices(Mesh, Holes.Num()));
		int32 Down = 0;
		for (const FVector& Normal : Mesh.Normals) if (Normal.Z <= 0.) ++Down;
		Test.TestEqual(Name + TEXT("normals point up"), Down, 0);
		Test.TestTrue(Name + TEXT("arrays match"), Mesh.Normals.Num() == Mesh.Vertices.Num() && Mesh.UVs.Num() == Mesh.Vertices.Num());

		// Away from the borders between levels, the full-detail level has the heights
		// the client draws. This is what makes the join with loaded terrain exact.
		const auto& Fine = Mesh.Levels[0];
		int32 Wrong = 0;
		for (int32 I = 0; I < Mesh.Vertices.Num(); ++I)
		{
			const FIntPoint C = Mesh.Corners[I];
			const bool bInside = Mesh.Levels.Num() == 1
				? (C.X >= Fine.Min.X && C.X <= Fine.Max.X && C.Y >= Fine.Min.Y && C.Y <= Fine.Max.Y)
				: (C.X > Fine.Min.X && C.X < Fine.Max.X && C.Y > Fine.Min.Y && C.Y < Fine.Max.Y);
			if (!bInside) continue;
			const int32 X = FMath::Clamp(C.X, 0, FPersonalFarTerrainBake::Dim - 1), Y = FMath::Clamp(C.Y, 0, FPersonalFarTerrainBake::Dim - 1);
			const double Expected = 2. * ((X * 7 + Y * 13 + X * Y) % 50) * 100.;
			const FVector Position((Mesh.Center.X * FarShellCells - C.X) * 2400., (C.Y - Mesh.Center.Y * FarShellCells) * 2400., Expected);
			if (!Mesh.Vertices[I].Equals(Position, 0.01)) ++Wrong;
		}
		Test.TestEqual(Name + TEXT("full-detail vertices are the baked corners"), Wrong, 0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarTerrainShellTest, "ACE.Personal.FarTerrain.Shell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarTerrainShellTest::RunTest(const FString& Parameters)
{
	// Rough made-up terrain, so a vertex left off a coarser edge would show as a crack.
	// FarShellCheckClosed expects exactly this height formula.
	auto BakePtr = MakeUnique<FPersonalFarTerrainBake>();
	FPersonalFarTerrainBake& Bake = *BakePtr;
	Bake.Init();
	for (int32 I = 0; I < 256; ++I) Bake.HeightTable[I] = I * 2.f;
	for (int32 X = 0; X < FPersonalFarTerrainBake::Dim; ++X)
		for (int32 Y = 0; Y < FPersonalFarTerrainBake::Dim; ++Y)
			Bake.HeightIndex[X * FPersonalFarTerrainBake::Dim + Y] = uint8((X * 7 + Y * 13 + X * Y) % 50);
	Bake.BuildLevels();

	const FIntPoint Center(101, 77);
	const FIntPoint CenterCell = Center * FarShellCells;
	FPersonalFarTerrainMesh Mesh;

	PersonalFarTerrainMesh::Build(Bake, Center, 0, {}, 0, FarShellScale, Mesh);
	TestTrue(TEXT("Radius 0 is off"), Mesh.IsEmpty() && Mesh.Vertices.Num() == 0);
	PersonalFarTerrainMesh::Build(FPersonalFarTerrainBake(), Center, 48, {}, 0, FarShellScale, Mesh);
	TestTrue(TEXT("No bake, no mesh"), Mesh.IsEmpty());

	// One level: radius 8 around a loaded radius of 5.
	TSet<FIntPoint> Holes = FarShellSquareHoles(Center, 5);
	PersonalFarTerrainMesh::Build(Bake, Center, 8, Holes, 7, FarShellScale, Mesh);
	TestEqual(TEXT("Radius 8: one level"), Mesh.Levels.Num(), 1);
	TestEqual(TEXT("Radius 8: quads are one cell"), Mesh.Levels[0].Step, 1);
	TestEqual(TEXT("Radius 8: triangle indices"), Mesh.Triangles.Num(), (17 * 17 - 11 * 11) * 64 * 6);
	TestEqual(TEXT("Radius 8: vertices"), Mesh.Vertices.Num(), 137 * 137 - 87 * 87);
	FarShellCheckClosed(*this, TEXT("Radius 8: "), Mesh, Holes);
	int32 InHole = 0;
	for (const FIntPoint C : Mesh.Corners)
		if (FMath::Abs(C.X - CenterCell.X - 4) < 44 && FMath::Abs(C.Y - CenterCell.Y - 4) < 44) ++InHole;
	TestEqual(TEXT("Radius 8: nothing inside the loaded terrain"), InHole, 0);
	TestEqual(TEXT("Mesh origin is the center landblock's corner"),
		PersonalFarTerrainMesh::Origin(Center, FarShellScale), FVector(-101 * 19200., 77 * 19200., 0.));

	// The diagonal of every full-detail quad matches the loaded terrain's.
	{
		TSet<uint64> Edges;
		TMap<FIntPoint, int32> VertexOfCorner;
		for (int32 I = 0; I < Mesh.Corners.Num(); ++I) VertexOfCorner.Add(Mesh.Corners[I], I);
		for (int32 I = 0; I + 2 < Mesh.Triangles.Num(); I += 3)
			for (int32 E = 0; E < 3; ++E)
			{
				const uint32 A = Mesh.Triangles[I + E], B = Mesh.Triangles[I + (E + 1) % 3];
				Edges.Add((uint64(FMath::Min(A, B)) << 32) | FMath::Max(A, B));
			}
		int32 WrongDiagonals = 0, BothKinds = 0;
		const int32 Y = (Center.Y + 7) * FarShellCells + 3;
		for (int32 X = Mesh.Levels[0].Min.X; X < Mesh.Levels[0].Max.X; ++X)
		{
			const FIntPoint Block = FarShellBlockOfCell(X, Y);
			const bool bSplit = FACELandblockMeshBuilder::GetSplitDir((uint32(Block.X) << 24) | (uint32(Block.Y) << 16),
				X - Block.X * FarShellCells, Y - Block.Y * FarShellCells);
			BothKinds |= bSplit ? 1 : 2;
			const FIntPoint From = bSplit ? FIntPoint(X, Y + 1) : FIntPoint(X + 1, Y + 1);
			const FIntPoint To = bSplit ? FIntPoint(X + 1, Y) : FIntPoint(X, Y);
			const uint32 A = VertexOfCorner[From], B = VertexOfCorner[To];
			if (!Edges.Contains((uint64(FMath::Min(A, B)) << 32) | FMath::Max(A, B))) ++WrongDiagonals;
		}
		TestEqual(TEXT("Diagonals follow GetSplitDir"), WrongDiagonals, 0);
		TestEqual(TEXT("The checked row has both diagonals"), BothKinds, 3);
	}

	// Loaded terrain that is not a square: one more landblock loaded, one not yet.
	Holes.Add(Center + FIntPoint(6, 0));
	Holes.Remove(Center + FIntPoint(-5, -5));
	PersonalFarTerrainMesh::Build(Bake, Center, 8, Holes, 7, FarShellScale, Mesh);
	FarShellCheckClosed(*this, TEXT("Uneven hole: "), Mesh, Holes);

	// The default radius: four levels (24, 48, 96 and 192 m quads).
	Holes = FarShellSquareHoles(Center, 6);
	PersonalFarTerrainMesh::Build(Bake, Center, 48, Holes, 7, FarShellScale, Mesh);
	if (TestEqual(TEXT("Radius 48: four levels"), Mesh.Levels.Num(), 4))
	{
		for (int32 L = 0; L < 3; ++L)
		{
			const auto& Level = Mesh.Levels[L];
			const int32 Coarse = Level.Step * 2;
			TestEqual(FString::Printf(TEXT("Radius 48: level %d quad size"), L), Level.Step, 1 << L);
			TestTrue(FString::Printf(TEXT("Radius 48: level %d ends on the next level's grid"), L),
				Level.Min.X % Coarse == 0 && Level.Min.Y % Coarse == 0 && Level.Max.X % Coarse == 0 && Level.Max.Y % Coarse == 0);
			TestTrue(FString::Printf(TEXT("Radius 48: level %d reaches its rings"), L),
				Level.Min.X <= (Center.X - (8 << L)) * FarShellCells && Level.Max.X >= (Center.X + 1 + (8 << L)) * FarShellCells);
		}
		TestTrue(TEXT("Radius 48: reaches the radius"),
			Mesh.Levels[3].Min.X <= (Center.X - 48) * FarShellCells && Mesh.Levels[3].Max.Y >= (Center.Y + 49) * FarShellCells);
	}
	FarShellCheckClosed(*this, TEXT("Radius 48: "), Mesh, Holes);
	AddInfo(FString::Printf(TEXT("Radius 48: %d triangles, %d vertices"), Mesh.Triangles.Num() / 3, Mesh.Vertices.Num()));

	// The largest radius: five levels. Also the cost of one rebuild.
	const double Start = FPlatformTime::Seconds();
	PersonalFarTerrainMesh::Build(Bake, Center, 96, Holes, 7, FarShellScale, Mesh);
	const double Seconds = FPlatformTime::Seconds() - Start;
	TestEqual(TEXT("Radius 96: five levels"), Mesh.Levels.Num(), 5);
	FarShellCheckClosed(*this, TEXT("Radius 96: "), Mesh, Holes);
	AddInfo(FString::Printf(TEXT("Radius 96: %d triangles, %d vertices, built in %.2f ms"),
		Mesh.Triangles.Num() / 3, Mesh.Vertices.Num(), Seconds * 1000.));
	PersonalFarTerrainMesh::Build(Bake, Center, 500, Holes, 7, FarShellScale, Mesh);
	TestTrue(TEXT("Radius is limited to 96"), Mesh.Levels.Last().Max.X <= (Center.X + 1 + 96) * FarShellCells + 15);

	// A large loaded radius pushes the full-detail level out, so only it touches loaded terrain.
	Holes = FarShellSquareHoles(Center, 26);
	PersonalFarTerrainMesh::Build(Bake, Center, 96, Holes, 27, FarShellScale, Mesh);
	TestTrue(TEXT("Loaded radius 25: full detail clears the hole"), Mesh.Levels[0].Min.X <= (Center.X - 28) * FarShellCells);
	FarShellCheckClosed(*this, TEXT("Loaded radius 25: "), Mesh, Holes);

	// At the corner of the world the shell continues with the edge values.
	const FIntPoint Corner(0, 0);
	Holes = FarShellSquareHoles(Corner, 5);
	PersonalFarTerrainMesh::Build(Bake, Corner, 48, Holes, 7, FarShellScale, Mesh);
	FarShellCheckClosed(*this, TEXT("World corner: "), Mesh, Holes);
	int32 WrongOutside = 0;
	for (int32 I = 0; I < Mesh.Vertices.Num(); ++I)
	{
		const FIntPoint C = Mesh.Corners[I];
		if (C.X > -400 || C.Y > -400) continue;
		// The edge value of whichever level the vertex belongs to.
		bool bEdgeValue = false;
		for (int32 L = 0; L < FPersonalFarTerrainBake::LevelCount; ++L)
			bEdgeValue |= FMath::IsNearlyEqual(Mesh.Vertices[I].Z, Bake.HeightAt(L, 0, 0) * 100., 0.01);
		if (!bEdgeValue) ++WrongOutside;
	}
	TestEqual(TEXT("World corner: outside takes the edge height"), WrongOutside, 0);

	// The setting. A separate settings file: this test must not change player preferences.
	TGuardValue<FString> SettingsPath(GGameUserSettingsIni, FPaths::ProjectSavedDir() / TEXT("Automation/PersonalFarTerrainFixture.ini"));
	FConfigFile FixtureConfig; FixtureConfig.NoSave = false; FixtureConfig.bCanSaveAllSections = true;
	GConfig->SetFile(GGameUserSettingsIni, &FixtureConfig);
	using namespace PersonalFarTerrain;
	const ACERuntimeOptions::FOption* Row = nullptr;
	for (const ACERuntimeOptions::FOption& Option : ACERuntimeOptions::Values)
	{
		if (FCString::Strcmp(Option.Key, RadiusKey) == 0)
		{
			Row = &Option;
		}
	}
	if (!TestNotNull(TEXT("Radius option row"), Row))
	{
		return false;
	}
	// The slider's range is the row's range: the smallest radius is also the default.
	const int32 Smallest = FMath::RoundToInt(Row->Min);
	TestTrue(TEXT("Radius 0 cannot be chosen"), Smallest > 0);
	TestEqual(TEXT("The default is the smallest radius"), FMath::RoundToInt(Row->Default), Smallest);
	TestEqual(TEXT("The largest radius is what the mesh allows"), FMath::RoundToInt(Row->Max), PersonalFarTerrainMesh::MaxRadius);
	TestTrue(TEXT("Nothing saved: on"), IsOn());
	TestEqual(TEXT("Nothing saved: the smallest radius"), Radius(), Smallest);

	// What an older version saved. ACERuntimeOptions::Set would limit it, so write the file itself.
	auto SaveRadius = [](float Value) { GConfig->SetFloat(TEXT("ACE.Presentation"), RadiusKey, Value, GGameUserSettingsIni); };
	SaveRadius(0.f);
	TestFalse(TEXT("Saved 0: off, as before the check box"), IsOn());
	TestEqual(TEXT("Saved 0: the radius is still within the range"), Radius(), Smallest);
	for (const float Saved : { 1.f, 42.f, 43.f, 48.f, 96.f, 31.858f, 63.6f, 500.f })
	{
		SaveRadius(Saved);
		TestTrue(*FString::Printf(TEXT("Saved %g: on"), Saved), IsOn());
		TestEqual(*FString::Printf(TEXT("Saved %g: a whole number within the range"), Saved), Radius(),
			FMath::Clamp(FMath::RoundToInt(Saved), Smallest, PersonalFarTerrainMesh::MaxRadius));
	}

	// The slider cannot save a radius below its range, so it cannot turn the far terrain off.
	ACERuntimeOptions::Set(RadiusKey, 0.f);
	TestTrue(TEXT("Slider at its lowest: on"), IsOn());
	TestEqual(TEXT("Slider at its lowest: the smallest radius"), Radius(), Smallest);

	// The check box.
	ACERuntimeOptions::Set(OptionKey, 0.f);
	TestFalse(TEXT("Check box off: off"), IsOn());
	ACERuntimeOptions::Set(OptionKey, 1.f);
	TestTrue(TEXT("Check box on: on"), IsOn());
	SaveRadius(0.f);
	TestFalse(TEXT("Check box on, saved 0: off"), IsOn());
	return !HasAnyErrors();
}
#endif
