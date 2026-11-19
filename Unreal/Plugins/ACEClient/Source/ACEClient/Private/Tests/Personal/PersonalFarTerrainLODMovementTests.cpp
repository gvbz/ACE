#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "Personal/PersonalFarTerrainMesh.h"
#include "Personal/PersonalFarScenery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarTerrainLODMovementTest, "ACE.Personal.FarTerrain.LODMovement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarTerrainLODMovementTest::RunTest(const FString& Parameters)
{
	// The existing shell fixture deliberately contains narrow ridges. Moving
	// one landblock previously changed a shared corner by 43.75m immediately.
	auto Bake = MakeUnique<FPersonalFarTerrainBake>();
	Bake->Init();
	for (int32 I = 0; I < 256; ++I) Bake->HeightTable[I] = I * 2.f;
	for (int32 X = 0; X < FPersonalFarTerrainBake::Dim; ++X)
		for (int32 Y = 0; Y < FPersonalFarTerrainBake::Dim; ++Y)
			Bake->HeightIndex[X * FPersonalFarTerrainBake::Dim + Y] = uint8((X * 7 + Y * 13 + X * Y) % 50);
	Bake->BuildLevels();

	constexpr float Scale = 100.f;
	const FIntPoint Center(101, 77);
	FPersonalFarTerrainMesh Before, After;
	const double Start = FPlatformTime::Seconds();
	PersonalFarTerrainMesh::Build(*Bake, Center, 96, {}, 7, Scale, Before);
	PersonalFarTerrainMesh::Build(*Bake, Center + FIntPoint(1, 0), 96, {}, 7, Scale, After);
	const double BuildMs = (FPlatformTime::Seconds() - Start) * 1000.;
	TMap<FIntPoint, double> Heights;
	for (int32 I = 0; I < Before.Corners.Num(); ++I) Heights.Add(Before.Corners[I], Before.Vertices[I].Z);
	double MaxChange = 0.;
	int32 Compared = 0;
	for (int32 I = 0; I < After.Corners.Num(); ++I)
	{
		if (const double* Old = Heights.Find(After.Corners[I]))
		{
			MaxChange = FMath::Max(MaxChange, FMath::Abs(*Old - After.Vertices[I].Z) / Scale);
			++Compared;
		}
	}
	TestTrue(TEXT("Recenter compares substantial shared terrain"), Compared > 50000);
	TestTrue(TEXT("A moving LOD boundary spreads ridge changes across its band"), MaxChange < 20.);
	TestTrue(TEXT("Morph band keeps the radius-96 mesh below 210k triangles"), Before.Triangles.Num() / 3 < 210000);
	TestEqual(TEXT("Outer coverage still starts at the requested radius"), Before.Levels.Last().Min.X, 32);
	TestEqual(TEXT("Outer coverage still ends on its original snapped grid"), Before.Levels.Last().Max.X, 1584);
	AddInfo(FString::Printf(TEXT("Adjacent-center LOD: max %.3fm (legacy fixture 43.750m), %d common vertices, %d triangles; two builds %.2fms"),
		MaxChange, Compared, Before.Triangles.Num() / 3, BuildMs));

	// Prepared objects can become visible over the OLD shell while its new
	// center is building. Protect R+1, including the leading edge after a step.
	const int32 NativeRing = PersonalFarScenery::NativeTerrainRing(16, 96);
	TestEqual(TEXT("Scenery prefetch adds one native terrain ring"), NativeRing, 17);
	TestEqual(TEXT("Native support cannot exceed the terrain shell"), PersonalFarScenery::NativeTerrainRing(16, 12), 12);
	TestEqual(TEXT("Native support respects maximum scenery placement"), PersonalFarScenery::NativeTerrainRing(24, 96), 24);
	TestEqual(TEXT("No terrain gives no native support"), PersonalFarScenery::NativeTerrainRing(16, 0), 0);
	PersonalFarTerrainMesh::Build(*Bake, Center, 96, {}, 7, Scale, After, nullptr, NativeRing);
	int32 NativeCorners = 0, ChangedNativeCorners = 0;
	const FIntPoint Low = (Center - FIntPoint(17, 17)) * 8;
	const FIntPoint High = (Center + FIntPoint(18, 18)) * 8;
	const FIntPoint NewVisibleLow = (Center + FIntPoint(1, 0) - FIntPoint(16, 16)) * 8;
	const FIntPoint NewVisibleHigh = (Center + FIntPoint(1, 0) + FIntPoint(17, 17)) * 8;
	int32 NewlyVisibleCorners = 0, ChangedVisibleCorners = 0;
	for (int32 I = 0; I < After.Corners.Num(); ++I)
	{
		const FIntPoint C = After.Corners[I];
		if (C.X < Low.X || C.X > High.X || C.Y < Low.Y || C.Y > High.Y) continue;
		++NativeCorners;
		const bool bNative = FMath::IsNearlyEqual(After.Vertices[I].Z, double(Bake->HeightAt(C.X, C.Y) * Scale), .01);
		if (!bNative) ++ChangedNativeCorners;
		if (C.X >= NewVisibleLow.X && C.X <= NewVisibleHigh.X && C.Y >= NewVisibleLow.Y && C.Y <= NewVisibleHigh.Y)
		{
			++NewlyVisibleCorners;
			if (!bNative) ++ChangedVisibleCorners;
		}
	}
	TestEqual(TEXT("Every prefetched scenery corner is retained"), NativeCorners, 281 * 281);
	TestEqual(TEXT("Native scenery support remains at original DAT height"), ChangedNativeCorners, 0);
	TestEqual(TEXT("Old shell covers the recentered visible scenery footprint"), NewlyVisibleCorners, 265 * 265);
	TestEqual(TEXT("Scenery revealed before shell recenter stays on native ground"), ChangedVisibleCorners, 0);
	return !HasAnyErrors();
}
#endif
