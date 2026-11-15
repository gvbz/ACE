#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalFarSceneryMeshes.h"
#include "Personal/PersonalFarSceneryPlacement.h"
#include "Personal/PersonalFarScenerySubsystem.h"
#include "Personal/PersonalFarTerrainBake.h"
#include "Personal/PersonalFarTerrain.h"
#include "Personal/PersonalSliderLabel.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "ACEDatSubsystem.h"
#include "ACERuntimeOptions.h"
#include "ACETypes.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dat/ACEDatFileTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarSceneryPlacementTest, "ACE.Personal.FarScenery.Placement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarSceneryPlacementTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFarSceneryPlacement;

	// Without data nothing is placed.
	{
		auto Empty = MakeUnique<FPersonalFarTerrainBake>();
		FSource NoSource;
		TArray<FItem> Items;
		Items.AddDefaulted();
		TestFalse(TEXT("No placement without a bake and scene tables"), Place(*Empty, NoSource, 0xC1620000u, Items));
		TestEqual(TEXT("A failed placement returns no items"), Items.Num(), 0);
	}

	// Real DAT files: the placement must equal what the client places near the player.
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (PersonalFarTerrainBake::SourceKey(DatDirectory) == 0)
	{
		AddInfo(TEXT("DAT files not found; skipped the comparison with the client's scenery"));
		return !HasAnyErrors();
	}
	auto BakePtr = MakeUnique<FPersonalFarTerrainBake>();
	FPersonalFarTerrainBake& Bake = *BakePtr;
	if (!TestTrue(TEXT("World bake loads"), PersonalFarTerrainBake::LoadOrBuild(DatDirectory, Bake)))
	{
		return false;
	}
	const FSourcePtr Source = Open(DatDirectory);
	if (!TestTrue(TEXT("Placement data opens"), Source.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("The region has scenes"), Source->Scenes.Num() > 10);
	AddInfo(FString::Printf(TEXT("%d scenes, %d files in the cell DAT index"), Source->Scenes.Num(), Source->Cell.GetFileCount()));

	// The bake against each landblock's own file, world wide. The bake keeps one value per
	// shared edge corner (the higher landblock's); the client uses each landblock's own.
	{
		int32 HeightDiffs = 0, RoadOrTypeDiffs = 0, Landblocks = 0;
		TArray<uint8> Bytes;
		FACEDatCellLandblock Land;
		for (int32 X = 0; X < FPersonalFarTerrainBake::LandblockDim; ++X)
		{
			for (int32 Y = 0; Y < FPersonalFarTerrainBake::LandblockDim; ++Y)
			{
				if (!Source->Cell.ReadFile((uint32(X) << 24) | (uint32(Y) << 16) | 0xFFFFu, Bytes))
				{
					continue;
				}
				FACEDatCursor Cursor(Bytes);
				if (!ACEDatUnpack::UnpackCellLandblock(Cursor, Land))
				{
					continue;
				}
				++Landblocks;
				bool bHeight = false, bTerrain = false;
				for (int32 VX = 0; VX < 9; ++VX)
				{
					for (int32 VY = 0; VY < 9; ++VY)
					{
						const int32 V = VX * 9 + VY;
						bHeight |= Bake.HeightIndex[(X * 8 + VX) * FPersonalFarTerrainBake::Dim + Y * 8 + VY] != Land.Height[V];
						// Scene type is read at a cell's low corner only, which is always the landblock's own.
						const bool bOwn = VX < 8 && VY < 8;
						const uint16 Mask = bOwn ? 0xF87F : 0x007F;
						bTerrain |= (Bake.TerrainWordAt(X * 8 + VX, Y * 8 + VY) & Mask) != (Land.Terrain[V] & Mask);
					}
				}
				HeightDiffs += bHeight ? 1 : 0;
				RoadOrTypeDiffs += bTerrain ? 1 : 0;
			}
		}
		AddInfo(FString::Printf(TEXT("Bake against %d landblock files: %d differ in a height, %d in a road or terrain type (shared edges)"),
			Landblocks, HeightDiffs, RoadOrTypeDiffs));
		TestEqual(TEXT("Every landblock's heights are in the bake, edges included"), HeightDiffs, 0);
	}

	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
	TStrongObjectPtr<UACEDatSubsystem> Owner(NewObject<UACEDatSubsystem>(Instance.Get()));
	UACEDatSubsystem* Dat = Owner.Get();
	if (!TestTrue(TEXT("The client's DAT subsystem loads"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}

	// Sawato, Hoshino and Mizuda with the landblocks around them, two towns (buildings),
	// and the steepest landblock within 8 of Sawato and Hoshino.
	TArray<FIntPoint> Blocks;
	for (const FIntPoint Center : {FIntPoint(0xC1, 0x62), FIntPoint(0x49, 0xE4), FIntPoint(0x8A, 0x0E)})
	{
		for (int32 DX = -2; DX <= 2; ++DX)
			for (int32 DY = -2; DY <= 2; ++DY) Blocks.Add(Center + FIntPoint(DX, DY));
	}
	Blocks.Add(FIntPoint(0xA9, 0xB4));
	Blocks.Add(FIntPoint(0xDA, 0x55));
	{
		FIntPoint Steepest(0, 0);
		float SteepestRange = -1.f;
		for (const FIntPoint Center : {FIntPoint(0xC1, 0x62), FIntPoint(0x49, 0xE4)})
		{
			for (int32 DX = -8; DX <= 8; ++DX)
				for (int32 DY = -8; DY <= 8; ++DY)
				{
					const FIntPoint Block = Center + FIntPoint(DX, DY);
					float Low = MAX_flt, High = -MAX_flt;
					for (int32 VX = 0; VX < 9; ++VX)
						for (int32 VY = 0; VY < 9; ++VY)
						{
							const float Height = Bake.HeightAt(Block.X * 8 + VX, Block.Y * 8 + VY);
							Low = FMath::Min(Low, Height);
							High = FMath::Max(High, Height);
						}
					if (High - Low > SteepestRange)
					{
						SteepestRange = High - Low;
						Steepest = Block;
					}
				}
		}
		Blocks.Add(Steepest);
		AddInfo(FString::Printf(TEXT("Steepest landblock tested: %02X%02X, %.0f m from low to high"), Steepest.X, Steepest.Y, SteepestRange));
	}

	int32 Compared = 0, Total = 0, WithBuildings = 0, WrongBlocks = 0, Stabs = 0, Buildings = 0;
	double MaxOriginDiff = 0., MaxQuatDiff = 0.;
	for (const FIntPoint Block : Blocks)
	{
		const uint32 Id = (uint32(Block.X) << 24) | (uint32(Block.Y) << 16);
		TArray<FACEDatRegionSceneryItem> Theirs;
		if (!Dat->CollectRegionScenery(Id, 100.f, Theirs))
		{
			AddInfo(FString::Printf(TEXT("Landblock %08X: the client has no scenery data; skipped"), Id));
			continue;
		}
		TArray<FItem> All;
		if (!TestTrue(FString::Printf(TEXT("Landblock %08X is placed"), Id), Place(Bake, *Source, Id, All)))
		{
			continue;
		}
		// RegionDesc scenery first, then the stabs, then the buildings.
		TArray<FItem> Mine, MyStabs, MyBuildings;
		bool bOrdered = true;
		for (const FItem& Item : All)
		{
			bOrdered = bOrdered && (Item.Kind == EKind::Region ? MyStabs.Num() + MyBuildings.Num() == 0 : (Item.Kind == EKind::Stab ? MyBuildings.Num() == 0 : true));
			(Item.Kind == EKind::Region ? Mine : (Item.Kind == EKind::Stab ? MyStabs : MyBuildings)).Add(Item);
		}
		TestTrue(FString::Printf(TEXT("Landblock %08X: scenery, then stabs, then buildings"), Id), bOrdered);
		FACEDatLandblockInfo Info;
		Dat->LoadLandblockInfo(Id, Info);
		if (Info.Buildings.Num() > 0)
		{
			++WithBuildings;
		}
		// Stabs and buildings equal the info file the client reads, as AACELandblockActor::QueueScenery takes them.
		{
			int32 At = 0;
			bool bSameStabs = true;
			for (const FACEDatLandblockStab& Stab : Info.Objects)
			{
				if (Stab.Id == 0) continue;
				bSameStabs = bSameStabs && MyStabs.IsValidIndex(At) && MyStabs[At].SetupId == Stab.Id && MyStabs[At].PlacementId == UACEDatSubsystem::ACEPlacementResting
					&& FVector3f(MyStabs[At].OriginAc) == Stab.Origin && FQuat4f(MyStabs[At].Orientation) == Stab.Orientation && MyStabs[At].Scale == 1.f;
				++At;
			}
			TestTrue(FString::Printf(TEXT("Landblock %08X: stabs equal the info file"), Id), bSameStabs && At == MyStabs.Num());
			At = 0;
			bool bSameBuildings = true;
			for (const FACEDatLandblockBuilding& Building : Info.Buildings)
			{
				if (Building.ModelId == 0) continue;
				bSameBuildings = bSameBuildings && MyBuildings.IsValidIndex(At) && MyBuildings[At].SetupId == Building.ModelId && MyBuildings[At].PlacementId == 0
					&& FVector3f(MyBuildings[At].OriginAc) == Building.Origin && FQuat4f(MyBuildings[At].Orientation) == Building.Orientation && MyBuildings[At].Scale == 1.f;
				++At;
			}
			TestTrue(FString::Printf(TEXT("Landblock %08X: buildings equal the info file"), Id), bSameBuildings && At == MyBuildings.Num());
			Stabs += MyStabs.Num();
			Buildings += MyBuildings.Num();
		}
		++Compared;
		Total += Mine.Num();
		bool bSame = Mine.Num() == Theirs.Num();
		for (int32 I = 0; bSame && I < Mine.Num(); ++I)
		{
			// The client keeps positions and rotations as floats (its scenery cache and spawn list).
			const double OriginDiff = (FVector3f(Mine[I].OriginAc) - FVector3f(Theirs[I].OriginAc)).GetAbsMax();
			const FQuat4f A(Mine[I].Orientation), B(Theirs[I].Orientation);
			const double QuatDiff = FMath::Max(FMath::Max(FMath::Abs(A.X - B.X), FMath::Abs(A.Y - B.Y)), FMath::Max(FMath::Abs(A.Z - B.Z), FMath::Abs(A.W - B.W)));
			MaxOriginDiff = FMath::Max(MaxOriginDiff, OriginDiff);
			MaxQuatDiff = FMath::Max(MaxQuatDiff, QuatDiff);
			bSame = Mine[I].SetupId == Theirs[I].SetupId && Mine[I].PlacementId == 0 && Mine[I].Kind == EKind::Region
				&& OriginDiff <= 1.e-4 && QuatDiff <= 1.e-5 && Mine[I].Scale == Theirs[I].Scale;
		}
		if (!bSame)
		{
			++WrongBlocks;
			AddError(FString::Printf(TEXT("Landblock %08X: placement differs from CollectRegionScenery (%d objects against %d)"), Id, Mine.Num(), Theirs.Num()));
		}
	}
	TestEqual(TEXT("Every landblock equals CollectRegionScenery: Setup, position, rotation, scale"), WrongBlocks, 0);
	TestTrue(TEXT("Most landblocks were compared"), Compared >= Blocks.Num() - 4);
	TestTrue(TEXT("The landblocks have scenery"), Total > 2000);
	TestTrue(TEXT("Landblocks with buildings were compared"), WithBuildings >= 2);
	TestTrue(TEXT("Stabs and buildings were compared"), Stabs > 0 && Buildings >= 2);
	AddInfo(FString::Printf(TEXT("%d landblocks compared (%d with buildings), %d objects, %d stabs, %d buildings; largest difference: position %.6f, rotation %.7f"),
		Compared, WithBuildings, Total, Stabs, Buildings, MaxOriginDiff, MaxQuatDiff));

	// The type key keeps id and kind apart.
	TestEqual(TEXT("A RegionDesc type's key is its id"), TypeKey(0x02000258u, EKind::Region), 0x02000258u);
	TestTrue(TEXT("Stab and building keys differ from the id and give it back"),
		TypeKey(0x02000258u, EKind::Stab) != 0x02000258u && TypeId(TypeKey(0x02000258u, EKind::Stab)) == 0x02000258u
		&& TypeKind(TypeKey(0x01000ABCu, EKind::Building)) == EKind::Building && TypeId(TypeKey(0x01000ABCu, EKind::Building)) == 0x01000ABCu);
	TestTrue(TEXT("Stabs rest, the others use placement 0"), TypePlacement(TypeKey(1, EKind::Stab)) == UACEDatSubsystem::ACEPlacementResting
		&& TypePlacement(TypeKey(1, EKind::Region)) == 0 && TypePlacement(TypeKey(1, EKind::Building)) == 0);

	// Not in the world, or not in the DAT.
	{
		TArray<FItem> Items;
		TestFalse(TEXT("Landblock column 255 is outside the world"), Place(Bake, *Source, 0xFF620000u, Items));
		TestFalse(TEXT("Landblock row 255 is outside the world"), Place(Bake, *Source, 0xC1FF0000u, Items));
	}

	// Cost: the 17 x 17 landblocks around Sawato, one after another.
	{
		const double Start = FPlatformTime::Seconds();
		int32 Count = 0, Objects = 0;
		TArray<FItem> Items;
		for (int32 DX = -8; DX <= 8; ++DX)
			for (int32 DY = -8; DY <= 8; ++DY)
			{
				if (Place(Bake, *Source, (uint32(0xC1 + DX) << 24) | (uint32(0x62 + DY) << 16), Items))
				{
					++Count;
					Objects += Items.Num();
				}
			}
		const double Seconds = FPlatformTime::Seconds() - Start;
		AddInfo(FString::Printf(TEXT("Placement of %d landblocks around Sawato: %d objects in %.1f ms (%.3f ms per landblock)"),
			Count, Objects, Seconds * 1000., Count > 0 ? Seconds * 1000. / Count : 0.));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarSceneryRulesTest, "ACE.Personal.FarScenery.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarSceneryRulesTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFarScenery;
	const FIntPoint Center(0xC1, 0x62);
	constexpr float Block = 192.f;

	// Rings and the wanted set.
	TestEqual(TEXT("Ring is the larger of the two directions"), Ring(Center + FIntPoint(-3, 7), Center), 7);
	TestFalse(TEXT("The player's landblock is the client's"), IsWanted(Center, Center, LastRing));
	TestFalse(TEXT("Ring 1 is the client's"), IsWanted(Center + FIntPoint(1, -1), Center, LastRing));
	TestTrue(TEXT("Ring 2 is in the set, prepared for the handoff"), IsWanted(Center + FIntPoint(2, -2), Center, LastRing));
	TestTrue(TEXT("Ring 8 gets far scenery, corners too"), IsWanted(Center + FIntPoint(-8, 8), Center, LastRing));
	TestFalse(TEXT("Ring 9 is past the radius"), IsWanted(Center + FIntPoint(0, 9), Center, LastRing));
	TestFalse(TEXT("Ring 9 stays out with a larger outer ring"), IsWanted(Center + FIntPoint(0, 9), Center, 20));
	TestFalse(TEXT("Ring 5 is out when the outer ring is 4"), IsWanted(Center + FIntPoint(5, 0), Center, 4));
	TestFalse(TEXT("Nothing is wanted with outer ring 1"), IsWanted(Center + FIntPoint(2, 0), Center, 1));
	int32 Count = 0;
	for (int32 DX = -10; DX <= 10; ++DX)
		for (int32 DY = -10; DY <= 10; ++DY) Count += IsWanted(Center + FIntPoint(DX, DY), Center, LastRing) ? 1 : 0;
	TestEqual(TEXT("Landblocks in rings 2 to 8"), Count, 17 * 17 - 3 * 3);
	TestFalse(TEXT("Nothing below landblock 0"), IsWanted(FIntPoint(-1, 4), FIntPoint(3, 4), LastRing));
	TestTrue(TEXT("Landblock 254 is in the world"), IsWanted(FIntPoint(254, 100), FIntPoint(250, 100), LastRing));
	TestFalse(TEXT("Landblock 255 is not"), IsWanted(FIntPoint(255, 100), FIntPoint(250, 100), LastRing));

	// Size rule: height x scale x factor. The plan's examples, in meters.
	TestEqual(TEXT("A 0.6 m plant at scale 1.4 is drawn to 336 m"), DrawDistance(0.6f, 1.4f, 400.f), 336.f, 0.01f);
	TestEqual(TEXT("A 6 m tree is drawn to 2.4 km"), DrawDistance(6.f, 1.f, 400.f), 2400.f, 0.01f);
	TestEqual(TEXT("Near edge of ring 3"), NearEdge(3, Block), 384.f);
	TestEqual(TEXT("Near edge of ring 8"), NearEdge(8, Block), 1344.f);
	TestEqual(TEXT("Near edge of the player's ring"), NearEdge(0, Block), 0.f);
	TestFalse(TEXT("The plant is left out of ring 3"), InReach(DrawDistance(0.6f, 1.4f, 400.f), 3, Block));
	TestTrue(TEXT("The plant is in reach in ring 2"), InReach(DrawDistance(0.6f, 1.4f, 400.f), 2, Block));
	TestTrue(TEXT("The 6 m tree is in reach in ring 8"), InReach(DrawDistance(6.f, 1.f, 400.f), 8, Block));
	TestFalse(TEXT("A 3 m bush is left out of ring 8"), InReach(DrawDistance(3.f, 1.f, 400.f), 8, Block));
	TestTrue(TEXT("The 3 m bush is in reach in ring 7"), InReach(DrawDistance(3.f, 1.f, 400.f), 7, Block));
	TestFalse(TEXT("An object with no height is never in reach past the client's rings"), InReach(DrawDistance(0.f, 1.f, 400.f), 2, Block));

	// Fog rule: the measured fog ends.
	TestEqual(TEXT("Night, fog end 400 m: ring 3 only"), FogRing(400.f, Block), 3);
	TestEqual(TEXT("Sunset, fog end 750 m"), FogRing(750.f, Block), 4);
	TestEqual(TEXT("Sunset, fog end 1,050 m"), FogRing(1050.f, Block), 6);
	TestEqual(TEXT("Rain by day, fog end 1,500 m"), FogRing(1500.f, Block), 8);
	TestEqual(TEXT("Clear day: the radius is the limit"), FogRing(9000.f, Block), LastRing);
	TestEqual(TEXT("No fog distance at all"), FogRing(0.f, Block), 1);
	TestEqual(TEXT("A negative fog end is read as 0"), FogRing(-50.f, Block), 1);
	TestTrue(TEXT("Ring 3 near edge is inside a 400 m fog end"), NearEdge(FogRing(400.f, Block), Block) <= 400.f);
	TestTrue(TEXT("The next ring's near edge is past it"), NearEdge(FogRing(400.f, Block) + 1, Block) > 400.f);

	// Handoff.
	TestTrue(TEXT("Past the client's rings the far objects are shown"), ShowFar(3, false));
	TestTrue(TEXT("Past the client's rings, whatever the near state"), ShowFar(3, true));
	TestTrue(TEXT("Ring 2 while the client's scenery is not there yet: shown"), ShowFar(2, false));
	TestFalse(TEXT("Ring 2 once the client's scenery is complete and shown: hidden"), ShowFar(2, true));
	TestFalse(TEXT("Ring 1 with the client's scenery: hidden"), ShowFar(1, true));

	// The settings. A separate settings file: this test must not change player preferences.
	{
		TGuardValue<FString> SettingsPath(GGameUserSettingsIni, FPaths::ProjectSavedDir() / TEXT("Automation/PersonalFarSceneryFixture.ini"));
		FConfigFile FixtureConfig; FixtureConfig.NoSave = false; FixtureConfig.bCanSaveAllSections = true;
		GConfig->SetFile(GGameUserSettingsIni, &FixtureConfig);
		IConsoleVariable* Draw = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.Draw"));
		IConsoleVariable* RadiusVar = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.Radius"));
		IConsoleVariable* FogRule = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.FogRule"));
		if (!TestNotNull(TEXT("Draw console variable"), Draw) || !TestNotNull(TEXT("Radius console variable"), RadiusVar) || !TestNotNull(TEXT("Fog rule console variable"), FogRule))
		{
			return false;
		}
		ON_SCOPE_EXIT { Draw->Set(-1); RadiusVar->Set(0); FogRule->Set(-1); };
		Draw->Set(-1); RadiusVar->Set(0); FogRule->Set(-1);

		TestFalse(TEXT("Nothing saved: off"), IsOn());
		TestEqual(TEXT("Nothing saved: radius 6"), Radius(), 6);
		TestFalse(TEXT("Nothing saved: far objects are kept in fog"), HideWithFog());
		ACERuntimeOptions::Set(OptionKey, 1.f);
		TestFalse(TEXT("Check box on, far terrain off: off"), IsOn());
		ACERuntimeOptions::Set(PersonalFarTerrain::OptionKey, 1.f);
		TestTrue(TEXT("Check box and far terrain on: on"), IsOn());
		ACERuntimeOptions::Set(OptionKey, 0.f);
		TestFalse(TEXT("Check box off, far terrain on: off"), IsOn());
		Draw->Set(1);
		TestTrue(TEXT("Console variable 1 draws without the check box"), IsOn());
		ACERuntimeOptions::Set(PersonalFarTerrain::OptionKey, 0.f);
		TestFalse(TEXT("Console variable 1 still needs the far terrain"), IsOn());
		ACERuntimeOptions::Set(PersonalFarTerrain::OptionKey, 1.f);
		ACERuntimeOptions::Set(OptionKey, 1.f);
		Draw->Set(0);
		TestFalse(TEXT("Console variable 0 is off with the check box on"), IsOn());
		Draw->Set(-1);

		for (const float Saved : {0.f, 3.f, 5.f, 8.f, 20.f})
		{
			ACERuntimeOptions::Set(RadiusKey, Saved);
			TestEqual(*FString::Printf(TEXT("Slider at %g: within 3 to 8"), Saved), Radius(), FMath::Clamp(FMath::RoundToInt32(Saved), 3, 8));
		}
		RadiusVar->Set(7);
		TestEqual(TEXT("Radius console variable overrides the slider"), Radius(), 7);
		RadiusVar->Set(2);
		TestEqual(TEXT("Radius console variable can go down to the handoff ring"), Radius(), NearRing);
		RadiusVar->Set(30);
		TestEqual(TEXT("Radius never passes the last ring"), Radius(), LastRing);
		RadiusVar->Set(0);

		ACERuntimeOptions::Set(FogKey, 1.f);
		TestTrue(TEXT("Fog check box on: hidden past the fog end"), HideWithFog());
		FogRule->Set(0);
		TestFalse(TEXT("Fog console variable 0 overrides the check box"), HideWithFog());
		ACERuntimeOptions::Set(FogKey, 0.f);
		FogRule->Set(1);
		TestTrue(TEXT("Fog console variable 1 overrides the check box"), HideWithFog());
	}

	// The number under a radius slider: what the feature uses, a whole number.
	TestEqual(TEXT("Slider label rounds down"), UPersonalSliderLabel::Format(6.2f, TEXT("landblocks")), FString(TEXT("6 landblocks")));
	TestEqual(TEXT("Slider label rounds up"), UPersonalSliderLabel::Format(75.6f, TEXT("landblocks")), FString(TEXT("76 landblocks")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarSceneryLevelsTest, "ACE.Personal.FarScenery.Levels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarSceneryLevelsTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFarSceneryMeshes;
	// Which level is drawn when one is asked for.
	{
		FLevels None;
		TestEqual(TEXT("No simpler models: the full one at every level"), None.Effective(2), 0);
		FLevels OnlySimplest;
		OnlySimplest.Sections[1].AddDefaulted();
		TestEqual(TEXT("Middle asked, only a simplest model: the full one"), OnlySimplest.Effective(1), 0);
		TestEqual(TEXT("Simplest asked, simplest there"), OnlySimplest.Effective(2), 2);
		FLevels OnlyMiddle;
		OnlyMiddle.Sections[0].AddDefaulted();
		TestEqual(TEXT("Simplest asked, only a middle model: the middle one"), OnlyMiddle.Effective(2), 1);
		TestEqual(TEXT("Full asked: full"), OnlyMiddle.Effective(0), 0);
		TestEqual(TEXT("A level past the last is read as the last"), OnlyMiddle.Effective(7), 1);
	}

	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	const FSourcePtr Source = Open(DatDirectory);
	if (!Source)
	{
		AddInfo(TEXT("DAT files not found; skipped the levels of real objects"));
		return !HasAnyErrors();
	}
	// The main trees of Hoshino and Sawato, a small tree, a ground plant, and an id that is no Setup.
	const TArray<uint32> Ids = {0x02000258u, 0x020003EEu, 0x020003D3u, 0x02001063u, 0x020012FCu, 0x7FFFFFFFu};
	TMap<uint32, FLevelsPtr> Built;
	Build(*Source, Ids, 100.f, Built);
	TestEqual(TEXT("Every id asked for has an answer"), Built.Num(), Ids.Num());
	for (const uint32 Id : Ids)
	{
		const FLevelsPtr* Found = Built.Find(Id);
		if (!TestTrue(FString::Printf(TEXT("Setup 0x%08X has levels"), Id), Found && Found->IsValid()))
		{
			continue;
		}
		const FLevels& Levels = **Found;
		TestTrue(FString::Printf(TEXT("Setup 0x%08X: no level has more triangles than the one before"), Id),
			Levels.Triangles[1] <= Levels.Triangles[0] && Levels.Triangles[2] <= Levels.Triangles[1]);
		for (int32 Level = 1; Level < LevelCount; ++Level)
		{
			int32 Triangles = 0;
			for (const FSection& Section : Levels.Sections[Level - 1])
			{
				Triangles += Section.Triangles.Num() / 3;
				TestTrue(TEXT("A section has a normal and a UV per vertex"), Section.Normals.Num() == Section.Vertices.Num() && Section.UVs.Num() == Section.Vertices.Num());
			}
			const bool bSimpler = Levels.Triangles[Level] < Levels.Triangles[Level - 1];
			TestEqual(FString::Printf(TEXT("Setup 0x%08X level %d: own geometry only when it is simpler"), Id, Level), Levels.Sections[Level - 1].Num() > 0, bSimpler);
			if (bSimpler)
			{
				TestEqual(FString::Printf(TEXT("Setup 0x%08X level %d: the geometry has the counted triangles"), Id, Level), Triangles, Levels.Triangles[Level]);
			}
		}
		AddInfo(FString::Printf(TEXT("Setup 0x%08X: %d, %d, %d triangles"), Id, Levels.Triangles[0], Levels.Triangles[1], Levels.Triangles[2]));
	}
	if (const FLevelsPtr* Tree = Built.Find(0x02000258u))
	{
		TestEqual(TEXT("Hoshino's tree, full"), (*Tree)->Triangles[0], 612);
		TestEqual(TEXT("Hoshino's tree, middle"), (*Tree)->Triangles[1], 384);
		TestEqual(TEXT("Hoshino's tree, simplest"), (*Tree)->Triangles[2], 146);
	}
	if (const FLevelsPtr* Tree = Built.Find(0x020003EEu))
	{
		TestEqual(TEXT("Sawato's tree has no simpler middle model: full is drawn there"), (*Tree)->Effective(1), 0);
		TestEqual(TEXT("Sawato's tree has a simplest model"), (*Tree)->Effective(2), 2);
	}
	if (const FLevelsPtr* Missing = Built.Find(0x7FFFFFFFu))
	{
		TestEqual(TEXT("An unknown id keeps the full model"), (*Missing)->Effective(2), 0);
	}
	return !HasAnyErrors();
}

// Runs the streaming and drawing code in a bare game world, with no server: the player is "in" a landblock by argument.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarSceneryDrawTest, "ACE.Personal.FarScenery.Draw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarSceneryDrawTest::RunTest(const FString& Parameters)
{
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (PersonalFarTerrainBake::SourceKey(DatDirectory) == 0)
	{
		AddInfo(TEXT("DAT files not found; skipped"));
		return true;
	}
	const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(true)
		.CreateNavigation(false).CreateAISystem(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	UGameInstance* Game = NewObject<UGameInstance>(GEngine);
	World->SetGameInstance(Game);
	Context.OwningGameInstance = Game;
	Context.SetCurrentWorld(World);
	Game->OnWorldChanged(nullptr, World);
	Game->Init();
	ON_SCOPE_EXIT { Game->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	World->Tick(LEVELTICK_TimeOnly, .01f);

	UACEDatSubsystem* Dat = Game->GetSubsystem<UACEDatSubsystem>();
	UPersonalFarScenerySubsystem* Far = World->GetSubsystem<UPersonalFarScenerySubsystem>();
	IConsoleVariable* SizeFactor = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.SizeFactor"));
	if (!TestNotNull(TEXT("The DAT subsystem exists"), Dat) || !TestNotNull(TEXT("The far scenery subsystem exists in a game world"), Far)
		|| !TestNotNull(TEXT("The size factor console variable exists"), SizeFactor)
		|| !TestTrue(TEXT("DAT files load"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}
	const PersonalFarTerrainBake::FBakePtr Bake = MakeShared<FPersonalFarTerrainBake, ESPMode::ThreadSafe>();
	if (!TestTrue(TEXT("World bake loads"), PersonalFarTerrainBake::LoadOrBuild(DatDirectory, *Bake)))
	{
		return false;
	}
	IConsoleVariable* Detail = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.Detail"));
	IConsoleVariable* FogRule = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.FogRule"));
	if (!TestNotNull(TEXT("The detail console variable exists"), Detail) || !TestNotNull(TEXT("The fog rule console variable exists"), FogRule))
	{
		return false;
	}
	const float SavedFactor = SizeFactor->GetFloat();
	IConsoleVariable* RadiusVar = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.Radius"));
	if (!TestNotNull(TEXT("The radius console variable exists"), RadiusVar))
	{
		return false;
	}
	// Back to "follow the settings" at the end. The test fixes radius and fog rule, so player preferences play no part.
	ON_SCOPE_EXIT { Far->ClearAll(); SizeFactor->Set(SavedFactor); Detail->Set(1); FogRule->Set(-1); RadiusVar->Set(0); };
	RadiusVar->Set(8);
	FogRule->Set(1);

	// The command file of the measuring scripts: one left from an earlier session is not run.
	{
		const FString CommandFile = FPaths::ProjectSavedDir() / TEXT("Personal/FarSceneryCommands.txt");
		if (!IFileManager::Get().FileExists(*CommandFile))
		{
			IConsoleVariable* Budget = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.BudgetMs"));
			// A command sets a variable as the console does, and later plain sets would be ignored: so a variable
			// the rest of the test does not set, put back the same way.
			const float SavedBudget = Budget ? Budget->GetFloat() : 1.f;
			ON_SCOPE_EXIT { IFileManager::Get().Delete(*CommandFile); if (Budget) Budget->Set(SavedBudget, ECVF_SetByConsole); };
			if (!TestNotNull(TEXT("The budget console variable exists"), Budget))
			{
				return false;
			}
			Far->bCommandFileSeen = false;
			Far->CommandFileTime = FDateTime();
			FFileHelper::SaveStringToFile(TEXT("personal.FarScenery.BudgetMs 7\n"), *CommandFile);
			Far->RunCommandFile();
			TestEqual(TEXT("A command file that is there at the start is not run"), Budget->GetFloat(), SavedBudget);
			Far->RunCommandFile();
			TestEqual(TEXT("It is not run later either while it does not change"), Budget->GetFloat(), SavedBudget);
			FFileHelper::SaveStringToFile(TEXT("personal.FarScenery.BudgetMs 9\nr.ScreenPercentage 10\n"), *CommandFile);
			IFileManager::Get().SetTimeStamp(*CommandFile, FDateTime::UtcNow() + FTimespan::FromSeconds(5.));
			Far->RunCommandFile();
			TestEqual(TEXT("A command file that changes afterwards is run"), Budget->GetFloat(), 9.f);
		}
		else
		{
			AddInfo(TEXT("A command file exists in the editor's Saved folder; that part is skipped"));
		}
	}
	// Full models everywhere first; the detail levels have their own part below.
	Detail->Set(0);

	constexpr float WorldScale = 100.f;
	constexpr float BlockSize = 192.f * WorldScale;
	constexpr float ClearDay = 1.e9f;
	constexpr int32 FullSet = 17 * 17 - 3 * 3;
	TSet<FIntPoint> NearShown;
	// Update until nothing is left to do, the way the timer does in game.
	auto Run = [&](FIntPoint Center, float FogEnd) -> double
	{
		const double Start = FPlatformTime::Seconds();
		Far->bComplete = false;
		while (!Far->bComplete && FPlatformTime::Seconds() - Start < 180.)
		{
			++GFrameCounter;
			Far->Stream(*Dat, Bake, Center, WorldScale, FogEnd, [&NearShown](FIntPoint Block) { return NearShown.Contains(Block); });
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.001f);
		}
		World->SendAllEndOfFrameUpdates();
		FlushRenderingCommands();
		return FPlatformTime::Seconds() - Start;
	};
	auto MemoryMb = []() { return double(FPlatformMemory::GetStats().UsedPhysical) / (1024. * 1024.); };
	auto Describe = [&](const TCHAR* What, double Seconds)
	{
		const UPersonalFarScenerySubsystem::FStats S = Far->GetStats();
		AddInfo(FString::Printf(TEXT("%s: %d landblocks in the set, %d with objects, %d hidden, %d components, %d instances, %lld triangles, %d too small; %.1f s, game thread %.1f ms, longest update %.2f ms"),
			What, S.InSet, S.Landblocks, S.Hidden, S.Components, S.Instances, S.Triangles, S.TooSmall, Seconds, Far->WorkSeconds * 1000., Far->LongestUpdate * 1000.));
	};

	// 1. No size rule (a huge factor), no fog, no near scenery: everything the placement gives.
	SizeFactor->Set(1.e7f);
	const FIntPoint Sawato(0xC1, 0x62);
	double Seconds = Run(Sawato, ClearDay);
	const UPersonalFarScenerySubsystem::FStats First = Far->GetStats();
	Describe(TEXT("Sawato, everything drawn (builds the meshes)"), Seconds);
	TestTrue(TEXT("The fill finishes"), Far->bComplete);
	TestEqual(TEXT("Rings 2 to 8 are in the set"), First.InSet, FullSet);
	TestEqual(TEXT("Nothing is hidden without near scenery"), First.Hidden, 0);
	TestTrue(TEXT("Sawato has far objects"), First.Instances > 10000 && First.Components > 300 && First.Triangles > 1000000);
	{
		int32 Components = 0, Instances = 0, WithPhysics = 0, WithShadow = 0, Culled = 0, Shells = 0, ShellsWithoutMaterial = 0, Panels = 0, PanelsWrong = 0;
		for (const auto& Pair : Far->Units)
		{
			if (const AActor* Actor = Pair.Value.Actor.Get())
			{
				// Instance components for scenery and stabs, a plain component per building.
				TInlineComponentArray<UStaticMeshComponent*> Found(Actor);
				for (const UStaticMeshComponent* Component : Found)
				{
					++Components;
					const UInstancedStaticMeshComponent* AsInstances = Cast<UInstancedStaticMeshComponent>(Component);
					if (Component->ComponentHasTag(TEXT("PersonalFarPanel")))
					{
						// The dark panels in a building's door and window openings.
						++Panels;
						PanelsWrong += (Component->GetStaticMesh() && Component->GetStaticMesh()->GetNumTriangles(0) > 0
							&& Component->GetMaterial(0) && Component->GetMaterial(0) == Far->PanelMaterial) ? 0 : 1;
					}
					else
					{
						Instances += AsInstances ? AsInstances->GetInstanceCount() : 1;
					}
					if (!AsInstances && !Component->ComponentHasTag(TEXT("PersonalFarPanel")))
					{
						++Shells;
						ShellsWithoutMaterial += (Component->GetStaticMesh() && Component->GetNumMaterials() > 0 && Component->GetMaterial(0)) ? 0 : 1;
					}
					WithPhysics += (Component->IsPhysicsStateCreated() || Component->IsCollisionEnabled()) ? 1 : 0;
					WithShadow += Component->CastShadow ? 1 : 0;
					Culled += Component->bNeverDistanceCull ? 0 : 1;
				}
			}
		}
		TestEqual(TEXT("Components counted"), Components, First.Components);
		TestEqual(TEXT("Instances counted"), Instances, First.Instances);
		TestEqual(TEXT("No component has collision"), WithPhysics, 0);
		TestEqual(TEXT("No component casts a shadow"), WithShadow, 0);
		TestEqual(TEXT("With a huge factor nothing has a cull distance"), Culled, 0);

		// Stabs and buildings: what the set holds by kind against what is drawn.
		int32 StabTypes = 0, BuildingItems = 0;
		FIntPoint BuildingBlock(-1, -1);
		for (const auto& Pair : Far->Blocks)
		{
			for (const auto& Type : Pair.Value.Types)
			{
				const PersonalFarSceneryPlacement::EKind Kind = PersonalFarSceneryPlacement::TypeKind(Type.Key);
				StabTypes += Kind == PersonalFarSceneryPlacement::EKind::Stab ? 1 : 0;
				if (Kind == PersonalFarSceneryPlacement::EKind::Building)
				{
					BuildingItems += Type.Value.Transforms.Num();
					if (PersonalFarScenery::Ring(Pair.Key, Sawato) >= 3) BuildingBlock = Pair.Key;
				}
			}
		}
		TestTrue(TEXT("The landblocks around Sawato have stabs and buildings"), StabTypes > 0 && BuildingItems > 0);
		TestTrue(TEXT("Buildings are drawn, one plain component each"), Shells > 0 && Shells <= BuildingItems);
		TestEqual(TEXT("Every building has its mesh and materials"), ShellsWithoutMaterial, 0);
		TestTrue(TEXT("Buildings with openings have a panel component, at most one each"), Panels > 0 && Panels <= Shells);
		TestEqual(TEXT("Every panel has a mesh and the shared dark material"), PanelsWrong, 0);
		// Opening sizes: very large ones get no panel.
		{
			int32 Small = 0, Medium = 0, Large = 0, Huge = 0, Wrong = 0;
			float Largest = 0.f;
			for (const auto& Pair : Far->PanelMeshes)
			{
				TArray<float> Areas;
				TObjectPtr<UMaterialInterface> Material = Far->PanelMaterial;
				const UStaticMesh* Again = PersonalFarSceneryMeshes::MakePanelMesh(*Dat, Far, PersonalFarSceneryPlacement::TypeId(Pair.Key), WorldScale, Material, &Areas);
				bool bAnyFilled = false;
				for (const float Area : Areas)
				{
					(Area <= 4.f ? Small : (Area <= PersonalFarSceneryMeshes::PanelMaxArea ? Medium : (Area <= 30.f ? Large : Huge)))++;
					Largest = FMath::Max(Largest, Area);
					bAnyFilled = bAnyFilled || Area <= PersonalFarSceneryMeshes::PanelMaxArea;
				}
				Wrong += ((Again != nullptr) == bAnyFilled && (Pair.Value != nullptr) == bAnyFilled) ? 0 : 1;
			}
			TestEqual(TEXT("A building type has a panel mesh exactly when it has an opening within the size limit"), Wrong, 0);
			AddInfo(FString::Printf(TEXT("Openings around Sawato by area: %d up to 4 m2, %d up to %.0f m2 (filled), %d up to 30 m2, %d larger (not filled); largest %.1f m2"),
				Small, Medium, PersonalFarSceneryMeshes::PanelMaxArea, Large, Huge, Largest));
		}
		int32 PanelTypes = 0;
		for (const auto& Pair : Far->PanelMeshes) PanelTypes += Pair.Value ? 1 : 0;
		AddInfo(FString::Printf(TEXT("Rings 2 to 8 around Sawato: %d stab types in landblocks, %d buildings placed, %d drawn, %d with panels; %d of %d building types have openings"),
			StabTypes, BuildingItems, Shells, Panels, PanelTypes, Far->PanelMeshes.Num()));

		// A landblock with a building where one is known, else a plain one.
		const FIntPoint Block = BuildingBlock.X >= 0 ? BuildingBlock : Sawato + FIntPoint(3, 0);
		TArray<FACEDatRegionSceneryItem> Theirs;
		if (TestTrue(TEXT("A far landblock is built and the client has its scenery"),
			Far->ActorOf(Block) && Dat->CollectRegionScenery((uint32(Block.X) << 24) | (uint32(Block.Y) << 16), WorldScale, Theirs) && Theirs.Num() > 0))
		{
			// The client's stabs and buildings of that landblock, in the same form.
			FACEDatLandblockInfo BlockInfo;
			Dat->LoadLandblockInfo((uint32(Block.X) << 24) | (uint32(Block.Y) << 16), BlockInfo);
			for (const FACEDatLandblockStab& Stab : BlockInfo.Objects)
			{
				FACEDatRegionSceneryItem& Item = Theirs.AddDefaulted_GetRef();
				Item.OriginAc = FVector(Stab.Origin);
				Item.Orientation = FQuat(Stab.Orientation);
				Item.Scale = 1.f;
			}
			for (const FACEDatLandblockBuilding& Building : BlockInfo.Buildings)
			{
				FACEDatRegionSceneryItem& Item = Theirs.AddDefaulted_GetRef();
				Item.OriginAc = FVector(Building.Origin);
				Item.Orientation = FQuat(Building.Orientation);
				Item.Scale = 1.f;
			}
			TInlineComponentArray<UStaticMeshComponent*> Found(Far->ActorOf(Block));
			TArray<FTransform> Drawn;
			for (const UStaticMeshComponent* Component : Found)
			{
				const UInstancedStaticMeshComponent* AsInstances = Cast<UInstancedStaticMeshComponent>(Component);
				if (Component->ComponentHasTag(TEXT("PersonalFarPanel")))
				{
					continue;
				}
				if (!AsInstances)
				{
					Drawn.Add(Component->GetComponentTransform());
					continue;
				}
				for (int32 I = 0; I < AsInstances->GetInstanceCount(); ++I)
				{
					FTransform Transform;
					AsInstances->GetInstanceTransform(I, Transform, /*bWorldSpace*/ true);
					Drawn.Add(Transform);
				}
			}
			const FVector Origin = FACEPosition::AceVectorToUnreal(FVector(Block.X * 192., Block.Y * 192., 0.), WorldScale);
			int32 Matched = 0;
			for (const FTransform& Transform : Drawn)
			{
				for (const FACEDatRegionSceneryItem& Item : Theirs)
				{
					// The client's own steps (AACELandblockActor::TryQueueRegionDesc, TrySpawnOneScenery).
					const FVector3f Local(Item.OriginAc);
					const FVector Location = Origin + FACEPosition::AceVectorToUnreal(FVector(Local.X, Local.Y, Local.Z), WorldScale);
					const FQuat4f Q(Item.Orientation);
					const FQuat Rotation = FACEPosition::AceQuatToUnreal(Q.W, FVector(Q.X, Q.Y, Q.Z));
					if (Transform.GetLocation().Equals(Location, 0.01) && Transform.GetRotation().Equals(Rotation, 1.e-4)
						&& FMath::IsNearlyEqual(Transform.GetScale3D().X, double(Item.Scale), 1.e-5))
					{
						++Matched;
						break;
					}
				}
			}
			// Animated types and types with no visible mesh are left out, so fewer can be drawn than the client places.
			TestEqual(TEXT("Every drawn object is where the client puts one: position, rotation, scale"), Matched, Drawn.Num());
			TestTrue(TEXT("Nearly all of the client's objects are drawn"), Drawn.Num() <= Theirs.Num() && Drawn.Num() >= Theirs.Num() * 9 / 10);
			AddInfo(FString::Printf(TEXT("Landblock %02X%02X: %d drawn of the client's %d (scenery, %d stabs, %d buildings)"),
				Block.X, Block.Y, Drawn.Num(), Theirs.Num(), BlockInfo.Objects.Num(), BlockInfo.Buildings.Num()));
		}
	}

	// 2. Size rule at the default factor, meshes already built.
	Far->ClearAll();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	FlushRenderingCommands();
	TestEqual(TEXT("Nothing is left after a clear"), Far->GetStats().InSet, 0);
	SizeFactor->Set(400.f);
	const double Memory2 = MemoryMb();
	Seconds = Run(Sawato, ClearDay);
	const UPersonalFarScenerySubsystem::FStats Sized = Far->GetStats();
	const double Memory3 = MemoryMb();
	Describe(TEXT("Sawato, size rule at factor 400"), Seconds);
	TestTrue(TEXT("The size rule leaves instances out"), Sized.Instances < First.Instances && Sized.TooSmall > 0);
	TestTrue(TEXT("Left out and drawn add up to at least what was drawn without the rule"), Sized.Instances + Sized.TooSmall >= First.Instances);
	AddInfo(FString::Printf(TEXT("Size rule: %d of %d instances drawn (%d%%), %d of %d components; process memory %+.0f MB for this fill"),
		Sized.Instances, First.Instances, First.Instances > 0 ? Sized.Instances * 100 / First.Instances : 0, Sized.Components, First.Components, Memory3 - Memory2));
	{
		int32 OutOfReach = 0, WithCull = 0;
		for (const auto& Pair : Far->Units)
		{
			if (const AActor* Actor = Pair.Value.Actor.Get())
			{
				const float Edge = PersonalFarScenery::NearEdge(PersonalFarScenery::Ring(FIntPoint(Pair.Key.X, Pair.Key.Y), Sawato), BlockSize);
				TInlineComponentArray<UInstancedStaticMeshComponent*> Found(Actor);
				for (const UInstancedStaticMeshComponent* Component : Found)
				{
					if (!Component->bNeverDistanceCull)
					{
						++WithCull;
						OutOfReach += Component->InstanceEndCullDistance < Edge - 1.f ? 1 : 0;
						if (Component->InstanceStartCullDistance > Component->InstanceEndCullDistance || Component->InstanceEndCullDistance <= 0)
						{
							AddError(TEXT("A component has a bad cull distance"));
						}
					}
				}
			}
		}
		TestTrue(TEXT("Smaller types have a cull distance"), WithCull > 0);
		TestEqual(TEXT("No component is created that cannot be seen from the player's landblock"), OutOfReach, 0);
	}

	// 3. Handoff: ring 2 is hidden where the client's own scenery is complete and shown.
	for (int32 DX = -2; DX <= 2; ++DX)
		for (int32 DY = -2; DY <= 2; ++DY) NearShown.Add(Sawato + FIntPoint(DX, DY));
	Run(Sawato, ClearDay);
	TestEqual(TEXT("Ring 2 is hidden under the client's scenery"), Far->GetStats().Hidden, 5 * 5 - 3 * 3);
	TestTrue(TEXT("A ring 2 landblock's actor is hidden"), Far->ActorOf(Sawato + FIntPoint(2, 0)) && Far->ActorOf(Sawato + FIntPoint(2, 0))->IsHidden());
	TestTrue(TEXT("A ring 3 landblock's actor is shown"), Far->ActorOf(Sawato + FIntPoint(3, 0)) && !Far->ActorOf(Sawato + FIntPoint(3, 0))->IsHidden());
	NearShown.Remove(Sawato + FIntPoint(2, 0));
	Run(Sawato, ClearDay);
	TestEqual(TEXT("A landblock whose near scenery goes away is shown again"), Far->GetStats().Hidden, 5 * 5 - 3 * 3 - 1);

	// Detail levels: the same objects in the same places, with the simpler models of the DAT further out.
	{
		auto Collect = [&](TArray<FVector>& Out)
		{
			for (const auto& Pair : Far->Units)
			{
				if (const AActor* Actor = Pair.Value.Actor.Get())
				{
					TInlineComponentArray<UInstancedStaticMeshComponent*> Found(Actor);
					for (const UInstancedStaticMeshComponent* Component : Found)
					{
						for (int32 I = 0; I < Component->GetInstanceCount(); ++I)
						{
							FTransform Transform;
							Component->GetInstanceTransform(I, Transform, /*bWorldSpace*/ true);
							Out.Add(Transform.GetLocation());
						}
					}
				}
			}
			Out.Sort([](const FVector& A, const FVector& B) { return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z); });
		};
		// Triangles drawn per ring, from the mesh each component really has.
		auto RingTriangles = [&](int32 Ring) -> int64
		{
			int64 Triangles = 0;
			for (const auto& Pair : Far->Units)
			{
				if (PersonalFarScenery::Ring(FIntPoint(Pair.Key.X, Pair.Key.Y), Sawato) == Ring)
				{
					Triangles += Pair.Value.Triangles;
				}
			}
			return Triangles;
		};
		const TSet<FIntPoint> SavedNearShown = NearShown;
		NearShown.Reset();
		Run(Sawato, ClearDay);
		const UPersonalFarScenerySubsystem::FStats Full = Far->GetStats();
		TArray<FVector> Before;
		Collect(Before);
		int64 FullRing[9];
		for (int32 Ring = 2; Ring <= 8; ++Ring) FullRing[Ring] = RingTriangles(Ring);

		Detail->Set(1);
		Seconds = Run(Sawato, ClearDay);
		const UPersonalFarScenerySubsystem::FStats Stepped = Far->GetStats();
		Describe(TEXT("Sawato, detail levels (full to ring 3, middle in 4, simplest from 5)"), Seconds);
		TArray<FVector> After;
		Collect(After);
		TestTrue(TEXT("The detail fill finishes"), Far->bComplete);
		TestEqual(TEXT("Detail levels draw the same number of instances"), Stepped.Instances, Full.Instances);
		TestTrue(TEXT("Detail levels draw clearly fewer triangles"), Stepped.Triangles * 10 < Full.Triangles * 8);
		int32 Moved = 0;
		if (TestEqual(TEXT("Same instances collected"), After.Num(), Before.Num()))
		{
			for (int32 I = 0; I < After.Num(); ++I) Moved += After[I].Equals(Before[I], 0.5) ? 0 : 1;
		}
		TestEqual(TEXT("Every instance is in the same place at every detail level"), Moved, 0);
		TestEqual(TEXT("Ring 2 keeps the full models"), RingTriangles(2), FullRing[2]);
		TestEqual(TEXT("Ring 3 keeps the full models"), RingTriangles(3), FullRing[3]);
		TestTrue(TEXT("Ring 4 has fewer triangles"), RingTriangles(4) < FullRing[4]);
		TestTrue(TEXT("Ring 6 has clearly fewer triangles"), RingTriangles(6) * 10 < FullRing[6] * 7);
		TestTrue(TEXT("Ring 6 is simpler than ring 4, in proportion"), double(RingTriangles(6)) / FullRing[6] < double(RingTriangles(4)) / FullRing[4]);
		AddInfo(FString::Printf(TEXT("Detail levels: %lld of %lld triangles (%d%%), %d own meshes"), Stepped.Triangles, Full.Triangles,
			int32(Stepped.Triangles * 100 / FMath::Max<int64>(Full.Triangles, 1)), Far->LevelMeshes.Num()));

		// An own simpler mesh is the same object: about the same size as the full mesh of the client, fewer triangles, no collision.
		int32 Compared = 0, WrongSize = 0, NotSimpler = 0, WithCollision = 0;
		for (const auto& Pair : Far->LevelMeshes)
		{
			const uint32 SetupId = uint32(Pair.Key / 4u);
			UStaticMesh* Own = Pair.Value.Mesh;
			UStaticMesh* Theirs = Dat->GetOrCreateSetupStaticMesh(SetupId, WorldScale, true, 0, false, false, true);
			if (!Own || !Theirs)
			{
				continue;
			}
			++Compared;
			const FVector A = Own->GetBounds().BoxExtent, B = Theirs->GetBounds().BoxExtent;
			// Small objects get padded bounds in both builds; compare the larger ones.
			if (B.GetMax() > 500.f && (A.Z < B.Z * 0.6 || A.Z > B.Z * 1.4))
			{
				++WrongSize;
				AddInfo(FString::Printf(TEXT("Setup 0x%08X level %d: height %.0f against %.0f"), SetupId, int32(Pair.Key % 4u), A.Z * 2., B.Z * 2.));
			}
			NotSimpler += Own->GetNumTriangles(0) < Theirs->GetNumTriangles(0) ? 0 : 1;
			// The fast build always makes a body setup; it must be empty.
			WithCollision += (Own->GetBodySetup() && Own->GetBodySetup()->AggGeom.GetElementCount() > 0) ? 1 : 0;
			TestEqual(TEXT("An own mesh has one material per section"), Pair.Value.Materials.Num(), Own->GetNumSections(0));
		}
		TestTrue(TEXT("Own simpler meshes were built"), Compared > 10);
		TestEqual(TEXT("Own meshes are about as tall as the full ones"), WrongSize, 0);
		TestEqual(TEXT("Own meshes have fewer triangles than the full ones"), NotSimpler, 0);
		TestEqual(TEXT("Own meshes have no collision"), WithCollision, 0);

		// Fog: past the fog end every landblock gets the simplest models. With the fog rule off the whole set stays, as silhouettes.
		FogRule->Set(0);
		Seconds = Run(Sawato, 400.f * WorldScale);
		const UPersonalFarScenerySubsystem::FStats Night = Far->GetStats();
		Describe(TEXT("Sawato at night with the fog rule off"), Seconds);
		TestEqual(TEXT("Fog rule off: the whole set is kept at night"), Night.InSet, FullSet);
		TestEqual(TEXT("Fog rule off: the same instances at night"), Night.Instances, Full.Instances);
		TestTrue(TEXT("At night ring 4 is simpler than by day"), RingTriangles(4) < FullRing[4] && Night.Triangles < Stepped.Triangles);
		TestEqual(TEXT("At night ring 3, inside the fog end, keeps the full models"), RingTriangles(3), FullRing[3]);
		FogRule->Set(1);
		Run(Sawato, ClearDay);
		TestEqual(TEXT("By day again: the day levels"), Far->GetStats().Triangles, Stepped.Triangles);

		Detail->Set(0);
		Run(Sawato, ClearDay);
		TestEqual(TEXT("Detail off again: the full models, as before"), Far->GetStats().Triangles, Full.Triangles);
		NearShown = SavedNearShown;
	}

	// Groups: past the client's rings one component per object type for a square of landblocks. The same objects, fewer components.
	{
		IConsoleVariable* Group = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.Group"));
		if (TestNotNull(TEXT("The group console variable exists"), Group))
		{
			Run(Sawato, ClearDay);
			const UPersonalFarScenerySubsystem::FStats Single = Far->GetStats();
			TArray<FVector> Before;
			auto Collect = [&](TArray<FVector>& Out)
			{
				for (const auto& Pair : Far->Units)
				{
					if (const AActor* Actor = Pair.Value.Actor.Get())
					{
						TInlineComponentArray<UInstancedStaticMeshComponent*> Found(Actor);
						for (const UInstancedStaticMeshComponent* Component : Found)
						{
							for (int32 I = 0; I < Component->GetInstanceCount(); ++I)
							{
								FTransform Transform;
								Component->GetInstanceTransform(I, Transform, /*bWorldSpace*/ true);
								Out.Add(Transform.GetLocation());
							}
						}
					}
				}
				Out.Sort([](const FVector& A, const FVector& B) { return A.X != B.X ? A.X < B.X : (A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z); });
			};
			Collect(Before);
			Group->Set(4);
			Seconds = Run(Sawato, ClearDay);
			const UPersonalFarScenerySubsystem::FStats Grouped = Far->GetStats();
			Describe(TEXT("Sawato, groups of 4 x 4 landblocks"), Seconds);
			TArray<FVector> After;
			Collect(After);
			TestEqual(TEXT("Groups draw the same number of instances"), Grouped.Instances, Single.Instances);
			TestEqual(TEXT("Groups draw the same triangles"), Grouped.Triangles, Single.Triangles);
			TestTrue(TEXT("Groups need far fewer components"), Grouped.Components * 2 < Single.Components);
			TestEqual(TEXT("Ring 2 keeps one unit per landblock, hidden under the client's scenery"), Grouped.Hidden, 5 * 5 - 3 * 3 - 1);
			int32 Moved = 0;
			if (TestEqual(TEXT("Same instances collected"), After.Num(), Before.Num()))
			{
				for (int32 I = 0; I < After.Num(); ++I) Moved += After[I].Equals(Before[I], 0.5) ? 0 : 1;
			}
			TestEqual(TEXT("Every instance is in the same place with groups"), Moved, 0);
			TestEqual(TEXT("No old unit is left"), Far->Retiring.Num() + Far->Releasing.Num(), 0);
			AddInfo(FString::Printf(TEXT("Groups of 4: %d components in %d units, against %d components in %d units"), Grouped.Components, Grouped.Units, Single.Components, Single.Units));
			// A border with groups: landblocks change units.
			Seconds = Run(Sawato + FIntPoint(1, 0), ClearDay);
			Describe(TEXT("One border east with groups of 4"), Seconds);
			TestTrue(TEXT("The border step with groups finishes"), Far->bComplete);
			TestEqual(TEXT("No old unit is left after the border"), Far->Retiring.Num() + Far->Releasing.Num(), 0);
			Group->Set(1);
			Run(Sawato, ClearDay);
			TestEqual(TEXT("Back to single landblocks: the same components as before"), Far->GetStats().Components, Single.Components);
		}
	}

	// Indoors or in portal space everything is hidden, a few landblocks per update, and shown again the same way.
	{
		NearShown.Add(Sawato + FIntPoint(2, 0));
		Run(Sawato, ClearDay);
		int32 Updates = 0;
		do
		{
			Far->HideAll();
			++Updates;
		} while (Far->GetStats().Hidden < Far->GetStats().InSet && Updates < 2000);
		TestEqual(TEXT("Everything ends up hidden"), Far->GetStats().Hidden, Far->GetStats().InSet);
		TestTrue(TEXT("Hiding is spread over many updates"), Updates > 10);
		Updates = 0;
		Far->bComplete = false;
		while (!Far->bComplete && Updates < 2000)
		{
			Far->Stream(*Dat, Bake, Sawato, WorldScale, ClearDay, [&NearShown](FIntPoint Block) { return NearShown.Contains(Block); });
			++Updates;
		}
		TestEqual(TEXT("Back outdoors only ring 2 stays hidden"), Far->GetStats().Hidden, 5 * 5 - 3 * 3);
		TestTrue(TEXT("Showing is spread over many updates"), Updates > 10);
		AddInfo(FString::Printf(TEXT("Showing %d landblocks again took %d updates"), Far->GetStats().InSet - (5 * 5 - 3 * 3), Updates));
	}

	// One landblock border north: the client's rings move with the player.
	NearShown.Reset();
	for (int32 DX = -2; DX <= 2; ++DX)
		for (int32 DY = -2; DY <= 2; ++DY) NearShown.Add(Sawato + FIntPoint(DX, DY + 1));
	Seconds = Run(Sawato + FIntPoint(0, 1), ClearDay);
	Describe(TEXT("One border north of Sawato"), Seconds);
	TestTrue(TEXT("The border step finishes"), Far->bComplete);
	TestEqual(TEXT("The set is full after a border"), Far->GetStats().InSet, FullSet);
	TestEqual(TEXT("Ring 2 is hidden after the border"), Far->GetStats().Hidden, 5 * 5 - 3 * 3);
	TestEqual(TEXT("Nothing is waiting to be removed"), Far->Releasing.Num(), 0);
	for (const auto& Pair : Far->Blocks)
	{
		if (!PersonalFarScenery::IsWanted(Pair.Key, Sawato + FIntPoint(0, 1), PersonalFarScenery::LastRing))
		{
			AddError(TEXT("A landblock outside the rings is still in the set"));
			break;
		}
	}

	// 4. Fog rule. Night (fog end 400 m): what exists is kept one ring past the fog ring, nothing new is made past it.
	Run(Sawato + FIntPoint(0, 1), 400.f * WorldScale);
	TestEqual(TEXT("Fog closes in: rings 2 to 4 are kept"), Far->GetStats().InSet, 9 * 9 - 3 * 3);
	Far->ClearAll();
	Run(Sawato + FIntPoint(0, 1), 400.f * WorldScale);
	Describe(TEXT("Sawato at night, fog end 400 m"), 0.);
	TestEqual(TEXT("Starting in fog: rings 2 and 3 only"), Far->GetStats().InSet, 7 * 7 - 3 * 3);
	Run(Sawato + FIntPoint(0, 1), 1050.f * WorldScale);
	TestEqual(TEXT("Fog opens to 1,050 m: rings 2 to 6"), Far->GetStats().InSet, 13 * 13 - 3 * 3);

	// A jump, like a portal: everything leaves, everything is new.
	NearShown.Reset();
	Seconds = Run(FIntPoint(0x49, 0xE4), ClearDay);
	Describe(TEXT("Hoshino, after a jump from Sawato"), Seconds);
	TestTrue(TEXT("The jump finishes"), Far->bComplete);
	TestTrue(TEXT("Hoshino has far objects"), Far->GetStats().Instances > 3000);
	TestTrue(TEXT("The pool of empty actors stays small"), Far->Pool.Num() <= UPersonalFarScenerySubsystem::MaxPooledActors);
	TestEqual(TEXT("Nothing is waiting to be removed after the jump"), Far->Releasing.Num(), 0);
	return !HasAnyErrors();
}
#endif
