#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACEDatSubsystem.h"
#include "ACETypes.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Personal/PersonalFarScenery.h"
#include "Dat/ACEEnvCellMeshBuilder.h"
#include "Dat/ACESetupMeshBuilder.h"
#include "Personal/PersonalFarSceneryMeshes.h"
#include "Personal/PersonalFarScenerySubsystem.h"
#include "Personal/PersonalFarRooms.h"
#include "Personal/PersonalFarRoomsMeshes.h"
#include "Personal/PersonalFarRoomComponent.h"
#include "PrimitiveSceneProxy.h"
#include "ProceduralMeshComponent.h"
#include "Personal/PersonalFarWindows.h"
#include "ACEEnvCellActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "EngineUtils.h"
#include "Personal/PersonalFarWindowsSubsystem.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Dat/ACEDatDatabase.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Personal/PersonalFarScenery.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "ProceduralMeshComponent.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "Dat/ACEOutdoorPortalPlan.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "ACERuntimeOptions.h"
#include "Misc/ConfigCacheIni.h"
#include "Personal/PersonalFarTerrain.h"
#include "Personal/PersonalFarWindowsOverrides.h"

// Stage 1 of far windows: what the openings of the buildings around some towns look like in the DAT.
// Reads only; writes Saved/Personal/FarWindowsSurvey.txt. Nothing is drawn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarWindowsSurveyTest, "ACE.Personal.FarWindows.Survey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarWindowsSurveyTest::RunTest(const FString& Parameters)
{
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
	TStrongObjectPtr<UACEDatSubsystem> Owner(NewObject<UACEDatSubsystem>(Instance.Get()));
	UACEDatSubsystem* Dat = Owner.Get();
	if (!FPaths::DirectoryExists(DatDirectory) || !Dat->LoadDatDirectory(DatDirectory))
	{
		AddInfo(TEXT("DAT files not found; skipped"));
		return true;
	}
	FACEDatTextureResolver* Textures = Dat->GetTextureResolver();
	constexpr float WorldScale = 100.f;
	constexpr float PanelMaxArea = 12.f;
	constexpr int32 Rings = 8;

	struct FOpening
	{
		float Area = 0.f, Width = 0.f, Height = 0.f, Bottom = 0.f;
		bool bFlat = false;       // lies flat (a hatch or a floor opening), not in a wall
		bool bValidIndex = false; // has an entry in the building's portal list
		int32 WithRoom = 0, NoRoom = 0, EmptyRoom = 0, Connector = 0, NoFloor = 0;
		float FloorMin = MAX_flt, FloorMax = -MAX_flt; // lowest point above the floor next to it
		float LowMin = MAX_flt, LowMax = -MAX_flt;     // lowest point above the room's lowest point
	};
	struct FType
	{
		int32 Instances = 0;
		TArray<FOpening> Openings;
		int32 ClearFaces = 0; // drawn-section faces that are fully transparent but no opening
		float ClearArea = 0.f;
		TSet<int32> Centers;
		TArray<FString> Places; // where some of them stand, as game coordinates
	};
	TMap<uint32, FType> Types;

	struct FCenter { const TCHAR* Name; FIntPoint Block; };
	const TArray<FCenter> Centers = {
		{TEXT("Holtburg A9B4"), FIntPoint(0xA9, 0xB4)}, {TEXT("Shoushi DA55"), FIntPoint(0xDA, 0x55)},
		{TEXT("Sawato baseline C162"), FIntPoint(0xC1, 0x62)}, {TEXT("Sawato town C95B"), FIntPoint(0xC9, 0x5B)},
		{TEXT("Hoshino baseline 49E4"), FIntPoint(0x49, 0xE4)}, {TEXT("Qalabar baseline 8A0E"), FIntPoint(0x8A, 0x0E)}, {TEXT("Yaraq 7D64"), FIntPoint(0x7D, 0x64)}};

	auto AreaOf = [](const TArray<FVector>& V)
	{
		FVector Cross = FVector::ZeroVector;
		for (int32 I = 1; I + 1 < V.Num(); ++I) Cross += FVector::CrossProduct(V[I] - V[0], V[I + 1] - V[0]);
		return float(Cross.Size() * 0.5);
	};

	TSet<uint32> Seen;
	TArray<int32> BuildingsPerCenter;
	BuildingsPerCenter.SetNumZeroed(Centers.Num());
	for (int32 Ci = 0; Ci < Centers.Num(); ++Ci)
	{
		for (int32 DX = -Rings; DX <= Rings; ++DX)
		for (int32 DY = -Rings; DY <= Rings; ++DY)
		{
			const FIntPoint Block = Centers[Ci].Block + FIntPoint(DX, DY);
			if (Block.X < 0 || Block.X > 254 || Block.Y < 0 || Block.Y > 254) continue;
			const uint32 Key = (uint32(Block.X) << 24) | (uint32(Block.Y) << 16);
			FACEDatLandblockInfo Info;
			if (!Dat->LoadLandblockInfo(Key, Info)) continue;
			BuildingsPerCenter[Ci] += Info.Buildings.Num();
			const bool bCounted = Seen.Contains(Key);
			Seen.Add(Key);
			for (const FACEDatLandblockBuilding& Building : Info.Buildings)
			{
				const FACEBuiltSetupMesh* Setup = Dat->GetOrBuildSetupMesh(Building.ModelId, WorldScale);
				if (!Setup) continue;
				FType& Type = Types.FindOrAdd(Building.ModelId);
				Type.Centers.Add(Ci);
				if (bCounted) continue; // towns overlap; a landblock counts once
				const bool bFirst = Type.Instances++ == 0;
				if (Type.Places.Num() < 4)
				{
					const double East = (Block.X * 192. + Building.Origin.X) / 240. - 101.95, North = (Block.Y * 192. + Building.Origin.Y) / 240. - 101.95;
					Type.Places.Add(FString::Printf(TEXT("%.1f%s %.1f%s (%02X%02X)"), FMath::Abs(North), North >= 0. ? TEXT("N") : TEXT("S"), FMath::Abs(East), East >= 0. ? TEXT("E") : TEXT("W"), Block.X, Block.Y));
				}
				// As UACEDatSubsystem::GetBuildingDoorwayApertures, relative to the landblock.
				const FTransform Frame(FACEPosition::AceQuatToUnreal(FQuat(Building.Orientation)),
					FACEPosition::AceVectorToUnreal(FVector(Building.Origin), WorldScale));
				int32 At = 0;
				for (const FACEBuiltSetupPart& Part : Setup->Parts)
				{
					if (bFirst)
					{
						for (const FACEBuiltMeshSection& Section : Part.Sections)
						{
							if (Section.bCollisionOnly || Section.IsEmpty()) continue;
							FACEDatSurfaceRenderFlags Decoded;
							const bool bClear = Section.bFullyTransparent
								|| (Textures && Section.SurfaceId != 0 && Textures->ResolveSurfaceRenderFlags(Section.SurfaceId, Decoded) && Decoded.bFullyTransparent);
							if (!bClear) continue;
							for (int32 T = 0; T + 2 < Section.Triangles.Num(); T += 3)
							{
								++Type.ClearFaces;
								Type.ClearArea += AreaOf({Section.Vertices[Section.Triangles[T]], Section.Vertices[Section.Triangles[T + 1]], Section.Vertices[Section.Triangles[T + 2]]})
									/ FMath::Square(WorldScale);
							}
						}
					}
					for (const FACEBuiltSetupPortal& Poly : Part.Portals)
					{
						if (Poly.Vertices.Num() < 3) continue;
						if (bFirst) Type.Openings.AddDefaulted();
						if (!Type.Openings.IsValidIndex(At)) break;
						FOpening& Opening = Type.Openings[At++];
						TArray<FVector> Verts;
						FVector Middle = FVector::ZeroVector;
						double Low = MAX_dbl, High = -MAX_dbl, Wide = 0.;
						for (const FVector& V : Poly.Vertices)
						{
							Verts.Add((Part.BindTransform * Frame).TransformPosition(V));
							Middle += Verts.Last();
							Low = FMath::Min(Low, Verts.Last().Z);
							High = FMath::Max(High, Verts.Last().Z);
						}
						Middle /= Verts.Num();
						for (const FVector& A : Verts) for (const FVector& B : Verts) Wide = FMath::Max(Wide, FVector::Dist2D(A, B));
						if (bFirst)
						{
							Opening.Area = AreaOf(Verts) / FMath::Square(WorldScale);
							Opening.Width = float(Wide / WorldScale);
							Opening.Height = float((High - Low) / WorldScale);
							Opening.Bottom = float((Low - Frame.GetLocation().Z) / WorldScale);
							Opening.bFlat = FMath::Abs((Part.BindTransform * Frame).TransformVectorNoScale(Poly.Normal).GetSafeNormal().Z) > 0.7;
							Opening.bValidIndex = Building.Portals.IsValidIndex(Poly.PortalIndex);
						}
						const uint16 Other = Building.Portals.IsValidIndex(Poly.PortalIndex) ? Building.Portals[Poly.PortalIndex].OtherCellId : 0;
						if (Other < 0x100 || Other == 0xFFFF)
						{
							++Opening.NoRoom;
							continue;
						}
						++Opening.WithRoom;
						const FACEBuiltEnvCellMesh* Room = Dat->GetOrBuildEnvCellMesh(Key | Other, WorldScale);
						if (!Room)
						{
							++Opening.EmptyRoom;
							continue;
						}
						// The presenter spawns no actor for these, so IsWorldCellVisible stays false.
						if (Room->StaticObjects.Num() == 0 && Room->IsEmpty() && !Room->bPortalConnector) ++Opening.EmptyRoom;
						if (Room->bPortalConnector) ++Opening.Connector;
						const FTransform ToBlock = Room->GetCellLocalToLandblock(WorldScale);
						const FVector Foot(Middle.X, Middle.Y, Low);
						double Floor = -MAX_dbl, RoomLow = MAX_dbl;
						for (const TArray<FACEBuiltMeshSection>* Sections : {&Room->CollisionSections, &Room->Sections})
						{
							for (const FACEBuiltMeshSection& Section : *Sections)
							{
								for (int32 T = 0; T + 2 < Section.Triangles.Num(); T += 3)
								{
									const FVector A = ToBlock.TransformPosition(Section.Vertices[Section.Triangles[T]]);
									const FVector B = ToBlock.TransformPosition(Section.Vertices[Section.Triangles[T + 1]]);
									const FVector C = ToBlock.TransformPosition(Section.Vertices[Section.Triangles[T + 2]]);
									RoomLow = FMath::Min(RoomLow, FMath::Min3(A.Z, B.Z, C.Z));
									// A face one can stand on, next to the opening and not above its lowest point.
									if (FMath::Abs(FVector::CrossProduct(B - A, C - A).GetSafeNormal().Z) < 0.7) continue;
									const FVector Near = FMath::ClosestPointOnTriangleToPoint(Foot, A, B, C);
									if (FVector::Dist2D(Near, Foot) <= 1.5 * WorldScale && Near.Z <= Low + 0.3 * WorldScale) Floor = FMath::Max(Floor, Near.Z);
								}
							}
							if (RoomLow < MAX_dbl) break; // drawn faces only when the room has no physics faces
						}
						if (Floor > -MAX_dbl)
						{
							const float Above = float((Low - Floor) / WorldScale);
							Opening.FloorMin = FMath::Min(Opening.FloorMin, Above);
							Opening.FloorMax = FMath::Max(Opening.FloorMax, Above);
						}
						else ++Opening.NoFloor;
						if (RoomLow < MAX_dbl)
						{
							const float Above = float((Low - RoomLow) / WorldScale);
							Opening.LowMin = FMath::Min(Opening.LowMin, Above);
							Opening.LowMax = FMath::Max(Opening.LowMax, Above);
						}
					}
				}
			}
		}
	}

	// One line per building type, then the totals the window rule is chosen from.
	FString Out = TEXT("Far windows survey. Per opening: area, width x height, lowest point above the floor next to it (floor), above the room's lowest point (low),\n")
		TEXT("above the building's lowest opening up to 12 m2 (base); rooms: instances with a room / all. Meters.\n\n");
	for (int32 Ci = 0; Ci < Centers.Num(); ++Ci)
	{
		int32 TypeCount = 0;
		for (const auto& Pair : Types) TypeCount += Pair.Value.Centers.Contains(Ci) ? 1 : 0;
		Out += FString::Printf(TEXT("%s: %d buildings of %d types within %d landblocks\n"), Centers[Ci].Name, BuildingsPerCenter[Ci], TypeCount, Rings);
	}
	Out += TEXT("\n");
	Types.KeySort(TLess<uint32>());
	static const float Edges[] = {0.1f, 0.3f, 0.5f, 0.75f, 1.f, 1.5f, 2.5f};
	constexpr int32 Buckets = UE_ARRAY_COUNT(Edges) + 1;
	// [rule][tall or short][bucket], counted per placed opening up to 12 m2 in a wall.
	int32 Histogram[3][2][Buckets] = {};
	int32 Total = 0, TotalTypes = 0, Flat = 0, Large = 0, NoRoom = 0, NoIndex = 0, Empty = 0, Connectors = 0, NoFloor = 0, Varies = 0, ClearTypes = 0, Buildings = 0, NoOpenings = 0;
	auto Bucket = [](float Value) { int32 B = 0; while (B < UE_ARRAY_COUNT(Edges) && Value >= Edges[B]) ++B; return B; };
	for (const auto& Pair : Types)
	{
		const FType& Type = Pair.Value;
		if (Type.Instances == 0) continue;
		++TotalTypes;
		Buildings += Type.Instances;
		NoOpenings += Type.Openings.Num() == 0 ? Type.Instances : 0;
		float Base = MAX_flt;
		for (const FOpening& O : Type.Openings) if (O.Area <= PanelMaxArea && !O.bFlat) Base = FMath::Min(Base, O.Bottom);
		if (Base == MAX_flt) for (const FOpening& O : Type.Openings) Base = FMath::Min(Base, O.Bottom);
		FString Line = FString::Printf(TEXT("0x%08X x%d:"), Pair.Key, Type.Instances);
		for (int32 I = 0; I < Type.Openings.Num(); ++I)
		{
			const FOpening& O = Type.Openings[I];
			const int32 Placed = O.WithRoom + O.NoRoom;
			Total += Placed;
			NoRoom += O.NoRoom;
			NoIndex += O.bValidIndex ? 0 : Placed;
			Empty += O.EmptyRoom;
			Connectors += O.Connector;
			Line += FString::Printf(TEXT(" [%d] %.1f m2 %.1fx%.1f"), I, O.Area, O.Width, O.Height);
			if (O.bFlat) Line += TEXT(" flat");
			if (O.FloorMin <= O.FloorMax) Line += FString::Printf(TEXT(" floor %+.2f"), O.FloorMin);
			if (O.FloorMax - O.FloorMin > 0.05f) Line += FString::Printf(TEXT("..%+.2f"), O.FloorMax);
			if (O.LowMin <= O.LowMax) Line += FString::Printf(TEXT(" low %+.2f"), O.LowMin);
			if (O.LowMax - O.LowMin > 0.05f) Line += FString::Printf(TEXT("..%+.2f"), O.LowMax);
			Line += FString::Printf(TEXT(" base %+.2f rooms %d/%d"), O.Bottom - Base, O.WithRoom, Placed);
			if (O.EmptyRoom > 0) Line += FString::Printf(TEXT(" empty %d"), O.EmptyRoom);
			if (O.Connector > 0) Line += FString::Printf(TEXT(" connector %d"), O.Connector);
			Line += TEXT(";");
			if (O.bFlat) { Flat += Placed; continue; }
			if (O.Area > PanelMaxArea) { Large += Placed; continue; }
			NoFloor += O.NoFloor;
			Varies += (O.FloorMax - O.FloorMin > 0.05f) ? 1 : 0;
			const int32 Tall = O.Height >= 2.1f ? 0 : 1;
			if (O.FloorMin <= O.FloorMax) Histogram[0][Tall][Bucket(O.FloorMin)] += O.WithRoom - O.NoFloor;
			if (O.LowMin <= O.LowMax) Histogram[1][Tall][Bucket(O.LowMin)] += O.WithRoom;
			Histogram[2][Tall][Bucket(O.Bottom - Base)] += Placed;
		}
		if (Type.ClearFaces > 0)
		{
			++ClearTypes;
			Line += FString::Printf(TEXT(" clear faces that are no opening: %d, %.1f m2"), Type.ClearFaces, Type.ClearArea);
		}
		if (Type.Places.Num() > 0) Line += TEXT(" at ") + FString::Join(Type.Places, TEXT(", "));
		Out += Line + TEXT("\n");
	}
	FString Summary = FString::Printf(TEXT("%d buildings of %d types (%d buildings without openings), %d openings placed: %d flat, %d over %.0f m2, %d without a room (%d of them not in the portal list), ")
		TEXT("%d behind an empty room with no actor, %d behind a connector room, %d with no floor found; %d openings differ by more than 5 cm between instances; %d types have clear faces that are no opening"),
		Buildings, TotalTypes, NoOpenings, Total, Flat, Large, PanelMaxArea, NoRoom, NoIndex, Empty, Connectors, NoFloor, Varies, ClearTypes);
	Out += TEXT("\n") + Summary + TEXT("\n\nOpenings in walls up to 12 m2 by height of the lowest point, 2.1 m tall or more / less tall:\n");
	static const TCHAR* Rules[] = {TEXT("above the floor next to it"), TEXT("above the room's lowest point"), TEXT("above the building's lowest opening")};
	for (int32 R = 0; R < 3; ++R)
	{
		FString Line = FString::Printf(TEXT("%s:"), Rules[R]);
		for (int32 B = 0; B < Buckets; ++B)
		{
			Line += B < UE_ARRAY_COUNT(Edges) ? FString::Printf(TEXT(" <%.2f: %d/%d;"), Edges[B], Histogram[R][0][B], Histogram[R][1][B])
				: FString::Printf(TEXT(" more: %d/%d"), Histogram[R][0][B], Histogram[R][1][B]);
		}
		Out += Line + TEXT("\n");
		AddInfo(Line);
	}
	AddInfo(Summary);

	// Stage 3: the openings in walls up to 12 m2 that start within 0.5 m of the building's lowest opening, by width and height,
	// and how many of them each door rule would take. Then the kinds under the first numbers.
	{
		struct FSize { int32 Placed = 0; TArray<uint32> TypeIds; };
		TMap<FIntPoint, FSize> Sizes; // width, height in cm, rounded to 5
		struct FRule { float MaxWidth, MinHeight, MaxHeight; int32 Taken = 0; };
		TArray<FRule> RuleList = {{2.0f, 2.1f, 2.8f}, {2.05f, 2.1f, 2.8f}, {2.2f, 2.1f, 2.8f}, {2.4f, 2.1f, 2.8f}, {2.7f, 2.1f, 2.8f}, {3.1f, 2.1f, 2.8f},
			{2.05f, 2.1f, 3.1f}, {2.2f, 2.1f, 3.1f}, {2.7f, 2.1f, 3.1f}, {2.7f, 2.1f, 3.6f}, {4.1f, 2.1f, 3.6f}};
		int32 Ground = 0, Kinds[PersonalFarWindows::KindCount + 1] = {};
		for (const auto& Pair : Types)
		{
			const FType& Type = Pair.Value;
			if (Type.Instances == 0) continue;
			TArray<PersonalFarWindows::FOpening> Shapes;
			for (const FOpening& O : Type.Openings)
			{
				PersonalFarWindows::FOpening& Shape = Shapes.AddDefaulted_GetRef();
				Shape.Area = O.Area; Shape.Width = O.Width; Shape.Height = O.Height; Shape.Bottom = O.Bottom; Shape.bFlat = O.bFlat;
			}
			const float Lowest = PersonalFarWindows::LowestOpening(Shapes);
			for (int32 I = 0; I < Shapes.Num(); ++I)
			{
				const int32 Placed = Type.Openings[I].WithRoom + Type.Openings[I].NoRoom;
				Kinds[int32(PersonalFarWindows::KindOf(Shapes[I], Lowest))] += Placed;
				const PersonalFarWindows::FOpening& O = Shapes[I];
				if (O.bFlat || O.Area > PanelMaxArea || O.Bottom - Lowest >= 0.5f) continue;
				Ground += Placed;
				FSize& Size = Sizes.FindOrAdd(FIntPoint(FMath::RoundToInt32(O.Width * 20.f) * 5, FMath::RoundToInt32(O.Height * 20.f) * 5));
				Size.Placed += Placed;
				Size.TypeIds.AddUnique(Pair.Key);
				for (FRule& Rule : RuleList)
				{
					Rule.Taken += (O.Height >= Rule.MinHeight && O.Height <= Rule.MaxHeight && O.Width <= Rule.MaxWidth) ? Placed : 0;
				}
			}
		}
		Sizes.KeySort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
		Out += FString::Printf(TEXT("\nOpenings in walls up to 12 m2 that start within 0.5 m of the building's lowest opening: %d. Width x height (to 5 cm), how many, building types:\n"), Ground);
		for (const auto& Pair : Sizes)
		{
			FString Line = FString::Printf(TEXT("%.2f x %.2f: %d;"), Pair.Key.X / 100.f, Pair.Key.Y / 100.f, Pair.Value.Placed);
			for (const uint32 Id : Pair.Value.TypeIds) Line += FString::Printf(TEXT(" 0x%08X"), Id);
			Out += Line + TEXT("\n");
		}
		Out += TEXT("\nDoor rules (up to this wide, this tall), how many of those openings each takes:\n");
		for (const FRule& Rule : RuleList)
		{
			const FString Line = FString::Printf(TEXT("up to %.2f m wide, %.1f to %.1f m tall: %d of %d"), Rule.MaxWidth, Rule.MinHeight, Rule.MaxHeight, Rule.Taken, Ground);
			Out += Line + TEXT("\n");
			AddInfo(Line);
		}
		const FString KindLine = FString::Printf(TEXT("Kinds under the first numbers: %d windows, %d doors, %d open entrances, %d flat, %d over 12 m2 (no panel)"),
			Kinds[0], Kinds[1], Kinds[2], Kinds[3], Kinds[4]);
		Out += TEXT("\n") + KindLine + TEXT("\n");
		AddInfo(KindLine);
		TestEqual(TEXT("Every placed opening has a kind"), Kinds[0] + Kinds[1] + Kinds[2] + Kinds[3] + Kinds[4], Total);
	}
	// Stage 7b: the kind of every opening by the door rule of 2026-11-15, per type, for the in-game check with the test colors.
	// D: door (red, brown panel), R: far room (green). * : changed from the 7b rule (2.2 to 2.6 m tall, at any height). o : set by an override.
	{
		PersonalFarWindowsOverrides::FOverrides Overrides;
		// The list the client ships, then the packaged client's Saved file (personal.FarWindows.Mark writes it), which wins.
		FString ShippedText, SavedText;
		const FString SavedFile = FPaths::ProjectSavedDir() / TEXT("VRWindowsArchive/Windows/ACUnreal/Saved/Personal/FarWindowsOverrides.txt");
		if (FFileHelper::LoadFileToString(ShippedText, *PersonalFarWindowsOverrides::ShippedPath())) PersonalFarWindowsOverrides::Parse(ShippedText, Overrides);
		const int32 ShippedCount = Overrides.Types.Num() + Overrides.Buildings.Num();
		PersonalFarWindowsOverrides::FOverrides Saved;
		if (FFileHelper::LoadFileToString(SavedText, *SavedFile)) PersonalFarWindowsOverrides::Parse(SavedText, Saved);
		PersonalFarWindowsOverrides::Merge(Saved, Overrides);
		const FString OverrideFile = FString::Printf(TEXT("%s (%d lines) and %s (%d lines)"), *PersonalFarWindowsOverrides::ShippedPath(), ShippedCount, *SavedFile, Saved.Types.Num() + Saved.Buildings.Num());
		Out += FString::Printf(TEXT("\nOverrides from %s: %d for types, %d for single buildings\n"), *OverrideFile, Overrides.Types.Num(), Overrides.Buildings.Num());
		for (const auto& Entry : Overrides.Types) Out += FString::Printf(TEXT("  0x%08X [%u] %s\n"), uint32(Entry.Key >> 32), uint32(Entry.Key), PersonalFarWindowsOverrides::Name(Entry.Value));
		for (const auto& Entry : Overrides.Buildings) Out += FString::Printf(TEXT("  %04X building %u: 0x%08X [%u] %s\n"), uint32(Entry.Key >> 48), uint32((Entry.Key >> 32) & 0xFFFFu), Entry.Value.Key, uint32(Entry.Key), PersonalFarWindowsOverrides::Name(Entry.Value.Value));
		Out += TEXT("\nKinds by the door rule of 2026-11-15, 7c (a wall opening 2.2 to 2.8 m tall and 1.1 to 2.3, 2.9 to 3.1 or 3.9 to 4.1 m wide, starting within 0.5 m of the building's lowest opening or at least 2.5 m above it, is a door; every other opening gets a far room), after the type overrides.\n")
			TEXT("Per opening: width x height, lowest point above the building's lowest opening, D door or R far room, * changed from the 7b rule, o set by an override. Meters.\n");
		int32 Doors = 0, RoomsKind = 0, ToDoor = 0, ToRoom = 0, Overridden = 0;
		TArray<FString> ChangedTypes;
		for (const auto& Pair : Types)
		{
			const FType& Type = Pair.Value;
			if (Type.Instances == 0 || Type.Openings.Num() == 0) continue;
			TArray<PersonalFarWindows::FOpening> Shapes;
			for (const FOpening& O : Type.Openings)
			{
				PersonalFarWindows::FOpening& Shape = Shapes.AddDefaulted_GetRef();
				Shape.Area = O.Area; Shape.Width = O.Width; Shape.Height = O.Height; Shape.Bottom = O.Bottom; Shape.bFlat = O.bFlat;
			}
			const float Lowest = PersonalFarWindows::LowestOpening(Shapes);
			TArray<PersonalFarWindows::EKind> Kinds;
			for (const PersonalFarWindows::FOpening& O : Shapes) Kinds.Add(PersonalFarWindows::KindOf(O, Lowest));
			TArray<PersonalFarWindows::EKind> ByRule = Kinds;
			// Type lines only: a line for one building is not shown per type.
			PersonalFarWindowsOverrides::FOverrides TypeOnly;
			TypeOnly.Types = Overrides.Types;
			PersonalFarWindowsOverrides::Apply(TypeOnly, Pair.Key, 0, -1, Kinds);
			FString Line = FString::Printf(TEXT("0x%08X x%d:"), Pair.Key, Type.Instances);
			int32 TypeDoors = 0, TypeRooms = 0;
			bool bChanged = false;
			for (int32 I = 0; I < Shapes.Num(); ++I)
			{
				const PersonalFarWindows::FOpening& O = Shapes[I];
				const int32 Placed = Type.Openings[I].WithRoom + Type.Openings[I].NoRoom;
				const bool bDoor = Kinds[I] == PersonalFarWindows::EKind::Door;
				const bool bSet = Kinds[I] != ByRule[I];
				PersonalFarWindows::FDoorRule Rule7b;
				Rule7b.MaxHeight = 2.6f;
				const bool bWasDoor = !O.bFlat && O.Area <= PanelMaxArea && PersonalFarWindows::IsDoorSize(O.Width, O.Height, Rule7b);
				const bool bChange = bDoor != bWasDoor;
				bChanged |= bChange;
				Overridden += bSet ? Placed : 0;
				(bDoor ? Doors : RoomsKind) += Placed;
				ToDoor += (bChange && bDoor) ? Placed : 0;
				ToRoom += (bChange && !bDoor) ? Placed : 0;
				(bDoor ? TypeDoors : TypeRooms) += 1;
				const FString Marks = FString(bChange ? TEXT("*") : TEXT("")) + (bSet ? TEXT("o") : TEXT(""));
				Line += O.bFlat ? FString::Printf(TEXT(" [%d] %.1f m2 flat %s%s;"), I, O.Area, bDoor ? TEXT("D") : TEXT("R"), *Marks)
					: FString::Printf(TEXT(" [%d] %.2fx%.2f %+.1f %s%s;"), I, O.Width, O.Height, O.Bottom - Lowest, bDoor ? TEXT("D") : TEXT("R"), *Marks);
			}
			Line += FString::Printf(TEXT(" doors %d, far rooms %d"), TypeDoors, TypeRooms);
			if (Type.Places.Num() > 0) Line += TEXT(" at ") + FString::Join(Type.Places, TEXT(", "));
			Out += Line + TEXT("\n");
			if (bChanged) ChangedTypes.Add(FString::Printf(TEXT("0x%08X"), Pair.Key));
		}
		for (int32 Ci = 0; Ci < Centers.Num(); ++Ci)
		{
			TArray<FString> Ids;
			for (const auto& Pair : Types) if (Pair.Value.Centers.Contains(Ci) && Pair.Value.Openings.Num() > 0) Ids.Add(FString::Printf(TEXT("0x%08X"), Pair.Key));
			Out += FString::Printf(TEXT("Types with openings around %s: %s\n"), Centers[Ci].Name, *FString::Join(Ids, TEXT(" ")));
		}
		const FString KindLine = FString::Printf(TEXT("By the door rule of 7c and the overrides: %d doors, %d far rooms (%d set by an override); against the 7b rule %d openings become doors, %d become far rooms; %d types change: %s"),
			Doors, RoomsKind, Overridden, ToDoor, ToRoom, ChangedTypes.Num(), *FString::Join(ChangedTypes, TEXT(" ")));
		Out += KindLine + TEXT("\n");
		AddInfo(KindLine);
	}
	const FString File = FPaths::ProjectSavedDir() / TEXT("Personal/FarWindowsSurvey.txt");
	TestTrue(TEXT("The survey file is written"), FFileHelper::SaveStringToFile(Out, *File));
	TestTrue(TEXT("Buildings were found"), Buildings > 0 && Total > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarWindowsRulesTest, "ACE.Personal.FarWindows.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarWindowsRulesTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFarWindows;
	// The show rule of a door panel.
	TestTrue(TEXT("A door far away: shown"), ShowPanel(false, false, false));
	TestFalse(TEXT("The door has taken over (in reach, room drawn): hidden"), ShowPanel(false, true, false));
	TestFalse(TEXT("The player is inside that building: hidden"), ShowPanel(false, false, true));
	TestTrue(TEXT("Test colors: shown whatever the rules"), ShowPanel(true, true, true));
	TestFalse(TEXT("A door 120 m away is out of reach at 96 m"), DoorInReach(120.f, 96.f, false));
	TestTrue(TEXT("At 96 m it is in reach"), DoorInReach(96.f, 96.f, false));
	TestTrue(TEXT("Once in reach it stays so at 100 m"), DoorInReach(100.f, 96.f, true));
	TestFalse(TEXT("And is out of reach again past 10% more"), DoorInReach(106.f, 96.f, true));

	// The kind of made-up openings, in a building whose lowest opening starts at 0.
	{
		auto Make = [](float Width, float Height, float Bottom, bool bFlat = false)
		{
			FOpening Opening;
			Opening.Area = bFlat ? Width * Width : Width * Height;
			Opening.Width = Width; Opening.Height = bFlat ? 0.f : Height; Opening.Bottom = Bottom; Opening.bFlat = bFlat;
			return Opening;
		};
		const TArray<FOpening> Building = {Make(1.5f, 2.2f, 0.f), Make(1.1f, 1.5f, 1.f), Make(5.f, 1.8f, 0.f), Make(1.9f, 2.5f, 1.2f), Make(2.5f, 0.f, 6.f, true), Make(5.3f, 3.8f, 0.f),
			Make(3.f, 2.5f, 0.f), Make(2.f, 3.5f, 0.f), Make(1.f, 1.5f, 0.3f), Make(4.f, 2.5f, 0.f), Make(2.6f, 2.6f, 0.f), Make(2.3f, 2.6f, 1.35f), Make(2.f, 2.5f, 5.6f),
			Make(1.2f, 2.2f, 0.f), Make(1.f, 2.5f, 0.f), Make(3.5f, 2.5f, 0.f), Make(1.9f, 2.1f, 0.f), Make(1.9f, 2.9f, 0.f), Make(2.35f, 2.55f, 1.4f), Make(2.f, 2.8f, 0.f), Make(2.f, 2.8f, 3.6f), Make(1.9f, 2.55f, 1.f)};
		const float Lowest = LowestOpening(Building);
		TestEqual(TEXT("The lowest opening"), Lowest, 0.f);
		TestTrue(TEXT("2.2 m tall, 1.5 m wide, at ground level: door"), KindOf(Building[0], Lowest) == EKind::Door);
		TestTrue(TEXT("1.5 m tall, 1 m up: window"), KindOf(Building[1], Lowest) == EKind::Window);
		TestTrue(TEXT("1.8 m tall, 5 m wide, at ground level: open entrance"), KindOf(Building[2], Lowest) == EKind::Entrance);
		TestTrue(TEXT("1.9 x 2.5 m, 1.2 m up: open entrance (0.5 to 2.5 m up is never a door)"), KindOf(Building[3], Lowest) == EKind::Entrance);
		TestTrue(TEXT("Flat: flat"), KindOf(Building[4], Lowest) == EKind::Flat);
		TestTrue(TEXT("Over 12 m2: over 12 m2"), KindOf(Building[5], Lowest) == EKind::None);
		TestTrue(TEXT("3.0 x 2.5 m at ground level: double door"), KindOf(Building[6], Lowest) == EKind::Door);
		TestTrue(TEXT("3.5 m tall at ground level: open entrance"), KindOf(Building[7], Lowest) == EKind::Entrance);
		TestTrue(TEXT("1.5 m tall, 0.3 m up: open entrance, not a window"), KindOf(Building[8], Lowest) == EKind::Entrance);
		TestTrue(TEXT("4.0 x 2.5 m: double door"), KindOf(Building[9], Lowest) == EKind::Door);
		TestTrue(TEXT("2.6 x 2.6 m at ground level: open entrance"), KindOf(Building[10], Lowest) == EKind::Entrance);
		TestTrue(TEXT("2.3 x 2.6 m, 1.35 m up: no door"), KindOf(Building[11], Lowest) != EKind::Door);
		TestTrue(TEXT("2.0 x 2.5 m, 5.6 m up: door"), KindOf(Building[12], Lowest) == EKind::Door);
		TestTrue(TEXT("1.2 x 2.2 m: door"), KindOf(Building[13], Lowest) == EKind::Door);
		TestTrue(TEXT("1.0 m wide: no door"), KindOf(Building[14], Lowest) != EKind::Door);
		TestTrue(TEXT("3.5 m wide, between single and double: no door"), KindOf(Building[15], Lowest) != EKind::Door);
		TestTrue(TEXT("2.1 m tall: no door"), KindOf(Building[16], Lowest) != EKind::Door);
		TestTrue(TEXT("2.9 m tall: no door"), KindOf(Building[17], Lowest) != EKind::Door);
		TestTrue(TEXT("2.35 x 2.55 m, 1.4 m up: no door"), KindOf(Building[18], Lowest) != EKind::Door);
		TestTrue(TEXT("2.0 x 2.8 m at ground (Yaraq arched doors): door"), KindOf(Building[19], Lowest) == EKind::Door);
		TestTrue(TEXT("2.0 x 2.8 m, 3.6 m up: door"), KindOf(Building[20], Lowest) == EKind::Door);
		TestTrue(TEXT("1.9 x 2.55 m, 1.0 m up (0x010025E3 [0]): no door"), KindOf(Building[21], Lowest) != EKind::Door);
		FDoorRule Narrow;
		Narrow.DoubleWidths[0] = Narrow.DoubleWidths[1] = 0.f;
		TestTrue(TEXT("A door rule without double doors leaves the 3 m opening"), KindOf(Building[6], Lowest, Narrow) == EKind::Entrance);
		// Overrides: by type and opening order, and for one building, which wins.
		{
			using namespace PersonalFarWindowsOverrides;
			FOverrides Overrides;
			Parse(TEXT("# comment\n0x010014C3 1 open\n0x01001117 1 open\n0x01001117 2 open\n0x01000E3B 0 door\n7E65 1 0x01001117 2 door\nnot a line\n0x0100 x open\n"), Overrides);
			TestEqual(TEXT("Overrides: four type lines"), Overrides.Types.Num(), 4);
			TestEqual(TEXT("Overrides: one building line"), Overrides.Buildings.Num(), 1);
			TestTrue(TEXT("Overrides: the type line"), Find(Overrides, 0x010014C3u, 0xA9B40000u, 3, 1) == EOverride::Open);
			TestTrue(TEXT("Overrides: another opening of that type has none"), Find(Overrides, 0x010014C3u, 0xA9B40000u, 3, 0) == EOverride::None);
			TestTrue(TEXT("Overrides: the building line wins"), Find(Overrides, 0x01001117u, 0x7E650000u, 1, 2) == EOverride::Door);
			TestTrue(TEXT("Overrides: another building of the type has the type line"), Find(Overrides, 0x01001117u, 0x7E650000u, 0, 2) == EOverride::Open);
			TestTrue(TEXT("Overrides: a building line for another type does not apply"), Find(Overrides, 0x01000E3Bu, 0x7E650000u, 1, 2) == EOverride::None);
			TArray<EKind> Kinds = {EKind::Flat, EKind::Door, EKind::Door};
			Apply(Overrides, 0x01001117u, 0xA9B40000u, 0, Kinds);
			TestTrue(TEXT("Overrides: open makes a door an open entrance"), Kinds[1] == EKind::Entrance && Kinds[2] == EKind::Entrance && Kinds[0] == EKind::Flat);
			TArray<EKind> Open = {EKind::Entrance};
			Apply(Overrides, 0x01000E3Bu, 0xA9B40000u, 0, Open);
			TestTrue(TEXT("Overrides: door makes an opening a door"), Open[0] == EKind::Door);
			FOverrides Again;
			Parse(Write(Overrides), Again);
			TestTrue(TEXT("Overrides: written and read back the same"), Again.Types.OrderIndependentCompareEqual(Overrides.Types) && Again.Buildings.Num() == 1
				&& Find(Again, 0x01001117u, 0x7E650000u, 1, 2) == EOverride::Door);
			// A rule line in the Saved file undoes a shipped line; the Saved file wins.
			FOverrides Shipped, Saved;
			Parse(TEXT("0x01003906 9 door\n0x0100081C 1 open\n"), Shipped);
			Parse(TEXT("0x01003906 9 rule\n0x0100081C 2 open\n"), Saved);
			TestTrue(TEXT("Overrides: a rule line is read"), Saved.Types.Num() == 2 && Saved.Types[TypeKey(0x01003906u, 9)] == EOverride::None);
			FOverrides Merged = Shipped;
			Merge(Saved, Merged);
			TestTrue(TEXT("Overrides: the Saved rule line wins over the shipped door"), Find(Merged, 0x01003906u, 0, 0, 9) == EOverride::None);
			TestTrue(TEXT("Overrides: shipped and saved lines both apply"), Find(Merged, 0x0100081Cu, 0, 0, 1) == EOverride::Open && Find(Merged, 0x0100081Cu, 0, 0, 2) == EOverride::Open);
			TestTrue(TEXT("Overrides: a rule line is written as rule"), Write(Saved).Contains(TEXT("0x01003906 9 rule")));
			// The list shipped with the client.
			FString ShippedText;
			FOverrides FromPlugin;
			TestTrue(TEXT("Overrides: the shipped list is in the plugin"), FFileHelper::LoadFileToString(ShippedText, *ShippedPath()));
			Parse(ShippedText, FromPlugin);
			TestEqual(TEXT("Overrides: the shipped list has the 21 lines made in game"), FromPlugin.Types.Num(), 21);
			TestTrue(TEXT("Overrides: the shipped list has the castle's four-part door"), Find(FromPlugin, 0x01003906u, 0, 0, 8) == EOverride::Door && Find(FromPlugin, 0x01003906u, 0, 0, 11) == EOverride::Door);
		}
		// The lowest opening is taken from openings in walls up to 12 m2.
		const TArray<FOpening> Tower = {Make(2.f, 2.5f, 3.f), Make(4.f, 0.f, 0.f, true), Make(5.f, 5.f, -1.f)};
		TestEqual(TEXT("Flat and oversized openings do not set the lowest opening"), LowestOpening(Tower), 3.f);
		TestTrue(TEXT("So a raised only door is a door"), KindOf(Tower[0], LowestOpening(Tower)) == EKind::Door);
		const TArray<FOpening> OnlyFlat = {Make(4.f, 0.f, 2.f, true)};
		TestEqual(TEXT("With nothing else, any opening sets it"), LowestOpening(OnlyFlat), 2.f);
		TestTrue(TEXT("Test colors: doors red"), TestColor(EKind::Door) == FColor(220, 0, 0));
		TestTrue(TEXT("Test colors: every other opening green"), TestColor(EKind::Window) == FColor(0, 200, 0) && TestColor(EKind::Entrance) == FColor(0, 200, 0)
			&& TestColor(EKind::Flat) == FColor(0, 200, 0) && TestColor(EKind::None) == FColor(0, 200, 0));
	}

	const FIntPoint Center(0xC9, 0x5B);
	TestTrue(TEXT("The player's landblock has door panels"), IsPrepared(Center, Center, 6));
	TestTrue(TEXT("Ring 6 of 6 has door panels"), IsPrepared(Center + FIntPoint(6, -2), Center, 6));
	TestFalse(TEXT("Ring 7 of 6 has none"), IsPrepared(Center + FIntPoint(0, 7), Center, 6));
	TestFalse(TEXT("Never past the last ring of far scenery"), IsPrepared(Center + FIntPoint(PersonalFarScenery::LastRing + 1, 0), Center, 99));
	TestFalse(TEXT("Nothing outside the world"), IsPrepared(FIntPoint(-1, 3), FIntPoint(1, 3), 6) || IsPrepared(FIntPoint(255, 3), FIntPoint(253, 3), 6));

	// Color. How much it is night, by the day fraction (follows the stars layer).
	TestEqual(TEXT("Midnight: night"), NightAmount(0.f), 1.f);
	TestEqual(TEXT("0.16: still night"), NightAmount(0.16f), 1.f);
	TestEqual(TEXT("0.21: dawn, about half"), NightAmount(0.21f), 0.545f, 0.01f);
	TestEqual(TEXT("0.27: day"), NightAmount(0.27f), 0.f, 0.001f);
	TestEqual(TEXT("Noon: day"), NightAmount(0.5f), 0.f);
	TestEqual(TEXT("0.84: still day"), NightAmount(0.84f), 0.f, 0.001f);
	TestEqual(TEXT("0.90: dusk, half"), NightAmount(0.90f), 0.5f, 0.01f);
	TestEqual(TEXT("0.96: night"), NightAmount(0.96f), 1.f, 0.001f);
	TestEqual(TEXT("A fraction past 1 wraps"), NightAmount(1.5f), 0.f);
	TestEqual(TEXT("Doors at night: dimmed"), DimLevel(NightAmount(0.f)), NightDim);
	TestEqual(TEXT("Doors by day: their color"), DimLevel(NightAmount(0.5f)), 1.f);
	TestEqual(TEXT("Doors at dusk: between"), DimLevel(NightAmount(0.9f)), 0.625f, 0.01f);
	TestTrue(TEXT("A level above 1 is the color itself"), Scaled(FColor(55, 31, 16), 3.f) == FColor(55, 31, 16));
	TestTrue(TEXT("The door color by default"), DoorColor() == FColor(55, 31, 16));

	// The switch: the check box Far Terrain Rooms, and far terrain objects are needed.
	{
		TGuardValue<FString> SettingsPath(GGameUserSettingsIni, FPaths::ProjectSavedDir() / TEXT("Automation/PersonalFarWindowsFixture.ini"));
		FConfigFile FixtureConfig; FixtureConfig.NoSave = false; FixtureConfig.bCanSaveAllSections = true;
		GConfig->SetFile(GGameUserSettingsIni, &FixtureConfig);
		IConsoleVariable* Draw = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarWindows.Draw"));
		IConsoleVariable* Rooms = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarWindows.Rooms"));
		IConsoleVariable* FarDraw = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarScenery.Draw"));
		if (!TestNotNull(TEXT("Draw console variable"), Draw) || !TestNotNull(TEXT("Rooms console variable"), Rooms) || !TestNotNull(TEXT("Far scenery draw console variable"), FarDraw))
		{
			return false;
		}
		ON_SCOPE_EXIT { Draw->Set(-1); Rooms->Set(-1); FarDraw->Set(-1); };
		Draw->Set(-1); Rooms->Set(-1); FarDraw->Set(-1);
		TestFalse(TEXT("Nothing saved: off"), IsOn());
		TestFalse(TEXT("Nothing saved: no far rooms"), PersonalFarRooms::IsOn());
		ACERuntimeOptions::Set(OptionKey, 1.f);
		TestFalse(TEXT("Check box on, far terrain objects off: off"), IsOn());
		ACERuntimeOptions::Set(PersonalFarTerrain::OptionKey, 1.f);
		ACERuntimeOptions::Set(PersonalFarScenery::OptionKey, 1.f);
		TestTrue(TEXT("Check box and far terrain objects on: on"), IsOn());
		TestTrue(TEXT("Far rooms follow the check box"), PersonalFarRooms::IsOn());
		Rooms->Set(0);
		TestFalse(TEXT("personal.FarWindows.Rooms 0: no far rooms"), PersonalFarRooms::IsOn());
		Rooms->Set(-1);
		ACERuntimeOptions::Set(OptionKey, 0.f);
		TestFalse(TEXT("Check box off: off"), IsOn());
		TestFalse(TEXT("Check box off: no far rooms"), PersonalFarRooms::IsOn());
		Draw->Set(1);
		TestTrue(TEXT("Console variable 1 draws without the check box"), IsOn());
		ACERuntimeOptions::Set(PersonalFarScenery::OptionKey, 0.f);
		TestFalse(TEXT("Console variable 1 still needs far terrain objects"), IsOn());
		ACERuntimeOptions::Set(PersonalFarScenery::OptionKey, 1.f);
		ACERuntimeOptions::Set(OptionKey, 1.f);
		Draw->Set(0);
		TestFalse(TEXT("Console variable 0 is off with the check box on"), IsOn());
	}
	return !HasAnyErrors();
}

// Makes the door panels in a bare game world, with no server: the player is "in" a landblock by argument.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarWindowsDrawTest, "ACE.Personal.FarWindows.Draw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarWindowsDrawTest::RunTest(const FString& Parameters)
{
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (!FPaths::DirectoryExists(DatDirectory))
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
	UPersonalFarWindowsSubsystem* Windows = World->GetSubsystem<UPersonalFarWindowsSubsystem>();
	if (!TestNotNull(TEXT("The DAT subsystem exists"), Dat) || !TestNotNull(TEXT("The far windows subsystem exists in a game world"), Windows)
		|| !TestTrue(TEXT("DAT files load"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}
	ON_SCOPE_EXIT { Windows->ClearAll(); Windows->Meshes.Reset(); Windows->bKindsWereOn = false; };
	constexpr float WorldScale = 100.f;
	constexpr int32 Rings = 3;

	// Off (the default): the timer's update makes nothing.
	Windows->Update();
	TestEqual(TEXT("Switched off, nothing is made"), Windows->Blocks.Num(), 0);

	// Holtburg, rings 0 to 3, until every landblock has its panels (building meshes load on workers).
	const FIntPoint Center(0xA9, 0xB4);
	auto Fill = [&](int32& OutUpdates, double& OutLongest, double& OutSeconds)
	{
		const double Start = FPlatformTime::Seconds();
		bool bDone = false;
		OutUpdates = 0;
		OutLongest = 0.;
		while (!bDone && FPlatformTime::Seconds() - Start < 120.)
		{
			++GFrameCounter;
			++OutUpdates;
			const double Before = FPlatformTime::Seconds();
			Windows->Stream(*Dat, Center, WorldScale, Rings);
			OutLongest = FMath::Max(OutLongest, FPlatformTime::Seconds() - Before);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.001f);
			bDone = Windows->Blocks.Num() > 0;
			for (const auto& Pair : Windows->Blocks) bDone = bDone && Pair.Value.bComplete;
		}
		OutSeconds = FPlatformTime::Seconds() - Start;
		return bDone;
	};
	int32 Updates = 0;
	double Longest = 0., Seconds = 0.;
	TestTrue(TEXT("Every landblock gets its panels"), Fill(Updates, Longest, Seconds));
	TestEqual(TEXT("Landblocks in rings 0 to 3"), Windows->Blocks.Num(), 7 * 7);
	const UPersonalFarWindowsSubsystem::FStats Stats = Windows->GetStats();
	TestTrue(TEXT("Holtburg has door panels"), Stats.Panels > 10 && Stats.Meshes > 3 && Stats.Meshes <= Stats.Panels);
	TestEqual(TEXT("Every panel is a door panel"), Stats.Doors, Stats.Panels);
	TestEqual(TEXT("Nothing is shown before the per-frame check says so"), Stats.Shown, 0);
	AddInfo(FString::Printf(TEXT("Holtburg, rings 0 to 3: %d door panels in %d landblocks from %d meshes; %d updates in %.1f s, longest %.2f ms"),
		Stats.Panels, Stats.Landblocks, Stats.Meshes, Updates, Seconds, Longest * 1000.));

	// Each panel, and per room the place of its panels against the client's own list of door openings.
	auto CheckPlaces = [&](bool bDoorsOnly, int32& OutOpenings, int32& OutRooms, int32& OutMissing, int32& OutOff, double& OutFurthest)
	{
		int32 Wrong = 0;
		OutOpenings = OutRooms = OutMissing = OutOff = 0;
		OutFurthest = 0.;
		for (const auto& Pair : Windows->Blocks)
		{
			TMap<int32, FBox> Mine;
			for (const auto& Panel : Pair.Value.Panels)
			{
				const UStaticMeshComponent* Component = Panel.Component.Get();
				const bool bGood = Component && Component->GetStaticMesh() && Component->GetStaticMesh()->GetNumTriangles(0) > 0
					&& Component->GetMaterial(0) && Windows->PanelMaterials.Contains(Component->GetMaterial(0)) && !Component->IsVisible()
					&& !Component->IsCollisionEnabled() && !Component->CastShadow && Component->ComponentHasTag(TEXT("PersonalFarWindowsPanel"));
				Wrong += bGood ? 0 : 1;
				if (Component)
				{
					const auto& Data = Component->GetCustomPrimitiveData().Data;
					const int32 Index = PersonalFarScenery::SizeFadePrimitiveDataIndex;
					TestTrue(TEXT("Panel size-fade opt-out is initialized before its first draw"), Data.IsValidIndex(Index) && Data[Index] == 0.f);
				}
				if (Component) Mine.FindOrAdd(Panel.Room, FBox(ForceInit)) += Component->Bounds.GetBox();
			}
			const uint32 Key = (uint32(Pair.Key.X) << 24) | (uint32(Pair.Key.Y) << 16);
			TMap<int32, FBox> Theirs;
			if (const auto Apertures = Dat->GetBuildingDoorwayApertures(Key, WorldScale))
			{
				for (const auto& Aperture : *Apertures)
				{
					// Every opening of the client's list; door panels are checked against it from their side below.
					++OutOpenings;
					FBox& Box = Theirs.FindOrAdd(int32(Aperture.DestEnvCellId), FBox(ForceInit));
					for (const FVector& V : Aperture.WorldVerts) Box += V;
				}
			}
			if (bDoorsOnly)
			{
				// Each door panel lies in an opening of the client's list to its room.
				for (const auto& Room : Mine)
				{
					++OutRooms;
					const FBox* Box = Theirs.Find(Room.Key);
					if (!Box)
					{
						++OutMissing;
						continue;
					}
					const FBox Grown = Box->ExpandBy(1.);
					OutOff += (Grown.IsInside(Room.Value.Min) && Grown.IsInside(Room.Value.Max)) ? 0 : 1;
				}
				continue;
			}
			for (const auto& Room : Theirs)
			{
				++OutRooms;
				const FBox* Box = Mine.Find(Room.Key);
				if (!Box)
				{
					++OutMissing;
					continue;
				}
				// A room can have a door and other openings: a door panel lies within the box of all of them.
				const FBox Grown = Room.Value.ExpandBy(1.);
				const bool bInside = bDoorsOnly ? (Grown.IsInside(Box->Min) && Grown.IsInside(Box->Max)) : true;
				const double Off = bDoorsOnly ? 0. : FMath::Max((Box->Min - Room.Value.Min).GetAbsMax(), (Box->Max - Room.Value.Max).GetAbsMax());
				OutFurthest = FMath::Max(OutFurthest, Off);
				OutOff += (!bInside || Off > 1.) ? 1 : 0;
			}
		}
		return Wrong;
	};
	int32 Openings = 0, Rooms = 0, Missing = 0, Off = 0;
	double Furthest = 0.;
	TestEqual(TEXT("Every panel has a mesh and the panel material, is hidden, has no collision and no shadow"), CheckPlaces(true, Openings, Rooms, Missing, Off, Furthest), 0);
	TestTrue(TEXT("Door panels to more than 5 rooms"), Rooms > 5);
	TestEqual(TEXT("Every door panel opens to a room of the client's list"), Missing, 0);
	TestEqual(TEXT("Every door panel lies in an opening of the client's list, within 1 cm"), Off, 0);
	AddInfo(FString::Printf(TEXT("Door panels to %d rooms; the client lists %d openings there"), Rooms, Openings));

	// Test colors: a panel in every opening, doors red and the others green, shown whatever the rules.
	{
		Windows->bKindsWereOn = true;
		Windows->ClearAll();
		TestTrue(TEXT("Test colors: every landblock gets its panels"), Fill(Updates, Longest, Seconds));
		const UPersonalFarWindowsSubsystem::FStats Test = Windows->GetStats();
		TestTrue(TEXT("Test colors: more panels than doors"), Test.Panels > Test.Doors && Test.Doors == Stats.Doors);
		TestEqual(TEXT("Test colors: every panel in its place, every opening of the client's list has one"), CheckPlaces(false, Openings, Rooms, Missing, Off, Furthest), 0);
		TestEqual(TEXT("Test colors: no room of the client's list is missing"), Missing, 0);
		TestEqual(TEXT("Test colors: the panels of a room are where the client's openings are, within 1 cm"), Off, 0);
		Windows->ApplyLook();
		int32 WrongColor = 0;
		for (int32 Kind = 0; Kind < PersonalFarWindows::KindCount; ++Kind)
		{
			WrongColor += Windows->AppliedColor.IsValidIndex(Kind) && Windows->AppliedColor[Kind] == PersonalFarWindows::TestColor(PersonalFarWindows::EKind(Kind)) ? 0 : 1;
		}
		TestEqual(TEXT("Test colors: doors red, the others green"), WrongColor, 0);
		AddInfo(FString::Printf(TEXT("Test colors, rings 0 to 3: %d panels (%d doors) for %d openings of the client's list to %d rooms; largest difference %.3f cm"),
			Test.Panels, Test.Doors, Openings, Rooms, Furthest));
		Windows->bKindsWereOn = false;
		Windows->ClearAll();
		Windows->ApplyLook();
		TestTrue(TEXT("Test colors off: the door color again"), Windows->AppliedColor.IsValidIndex(1) && Windows->AppliedColor[1] != PersonalFarWindows::TestColor(PersonalFarWindows::EKind::Door));
	}

	// Moving on: landblocks out of reach lose their panels.
	Fill(Updates, Longest, Seconds);
	Windows->Stream(*Dat, Center + FIntPoint(9, 0), WorldScale, Rings);
	for (const auto& Pair : Windows->Blocks)
	{
		TestTrue(TEXT("Only landblocks within reach are kept"), PersonalFarWindows::IsPrepared(Pair.Key, Center + FIntPoint(9, 0), Rings + 1));
	}
	Windows->ClearAll();
	TestEqual(TEXT("Nothing is left after a clear"), Windows->GetStats().Panels, 0);
	return !HasAnyErrors();
}

// Stage 5 of far windows (far rooms): what drawing the rooms behind windows and open entrances would cost.
// Reads only; writes Saved/Personal/FarRoomsSurvey.txt. Nothing is drawn, except one offscreen capture with -RenderOffscreen.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarWindowsRoomsTest, "ACE.Personal.FarWindows.Rooms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarWindowsRoomsTest::RunTest(const FString& Parameters)
{
	using namespace PersonalFarWindows;
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (!FPaths::DirectoryExists(DatDirectory))
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
	UACEDatSubsystem* Dat = Game->GetSubsystem<UACEDatSubsystem>();
	if (!TestNotNull(TEXT("The DAT subsystem exists"), Dat) || !TestTrue(TEXT("DAT files load"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}
	// Own DAT handles and an own room builder, as a worker would have them: not the client's cache.
	FACEDatDatabase OwnPortal, OwnCell;
	if (!TestTrue(TEXT("Own DAT handles open"), OwnPortal.Open(DatDirectory / TEXT("client_portal.dat")) && OwnCell.Open(DatDirectory / TEXT("client_cell_1.dat"))))
	{
		return false;
	}
	FACEDatTextureResolver OwnTextures(&OwnPortal, nullptr);
	FACEEnvCellMeshBuilder Builder(&OwnCell, &OwnPortal, &OwnTextures);
	constexpr float WorldScale = 100.f;
	constexpr int32 Rings = 8;

	struct FRoom
	{
		bool bBuilt = false;
		int32 Triangles = 0, Sections = 0, Vertices = 0;
		uint32 ShapeKey = 0;  // environment, cell structure, surfaces
		uint32 LitKey = 0;    // and the baked colors
		double BuildMs = 0.;
		FBox Bounds = FBox(ForceInit); // in its landblock
		FVector Origin = FVector::ZeroVector;
		FTransform ToBlock;
		TArray<uint32> Surfaces;
		TSharedPtr<FACEBuiltEnvCellMesh> Mesh;
	};
	TMap<uint32, FRoom> Rooms;
	double BuildMsTotal = 0., BuildMsMax = 0.;
	auto GetRoom = [&](uint32 Id) -> FRoom&
	{
		if (FRoom* Found = Rooms.Find(Id)) return *Found;
		FRoom& Room = Rooms.Add(Id);
		Room.Mesh = MakeShared<FACEBuiltEnvCellMesh>();
		const double Start = FPlatformTime::Seconds();
		Room.bBuilt = Builder.BuildEnvCell(Id, WorldScale, *Room.Mesh);
		Room.BuildMs = (FPlatformTime::Seconds() - Start) * 1000.;
		BuildMsTotal += Room.BuildMs;
		BuildMsMax = FMath::Max(BuildMsMax, Room.BuildMs);
		FACEDatEnvCell Cell;
		if (Dat->LoadEnvCell(Id, Cell))
		{
			Room.ShapeKey = HashCombine(GetTypeHash(Cell.EnvironmentId), GetTypeHash(Cell.CellStructure));
			for (const uint32 Surface : Cell.Surfaces) Room.ShapeKey = HashCombine(Room.ShapeKey, GetTypeHash(Surface));
		}
		Room.LitKey = Room.ShapeKey;
		if (!Room.bBuilt) return Room;
		Room.ToBlock = Room.Mesh->GetCellLocalToLandblock(WorldScale);
		Room.Origin = Room.ToBlock.GetLocation();
		for (const FACEBuiltMeshSection& Section : Room.Mesh->Sections)
		{
			if (Section.IsEmpty()) continue;
			++Room.Sections;
			Room.Triangles += Section.Triangles.Num() / 3;
			Room.Vertices += Section.Vertices.Num();
			Room.Surfaces.AddUnique(Section.SurfaceId);
			for (int32 I = 0; I < Section.Vertices.Num(); ++I)
			{
				Room.Bounds += Room.ToBlock.TransformPosition(Section.Vertices[I]);
				if (Section.VertexColors.IsValidIndex(I))
				{
					Room.LitKey = HashCombine(Room.LitKey, GetTypeHash(Section.VertexColors[I].ToFColor(false).DWColor()));
				}
			}
		}
		return Room;
	};

	struct FBuilding
	{
		FIntPoint Block;
		uint32 ModelId = 0;
		int32 Openings = 0, LargeOpenings = 0;
		TArray<uint32> Deep1, Deep2, Full; // rooms of the windows and open entrances
		TArray<uint32> LargeRooms;         // rooms only behind openings over 12 m2
		uint32 MeshKey = 0;                // equal for buildings that can share one far-room mesh (full list)
		uint32 MeshKey2 = 0;               // the same at two rooms per opening
		uint32 ShapeMeshKey = 0;           // the same without the baked colors
		TArray<TArray<FVector>> Holes;     // the far-room openings, in the landblock's space
		TArray<uint32> HoleRooms;          // and the room behind each
		FTransform Frame;
		float Overhang = 0.f, Below = 0.f; // how far the rooms reach past the shell's box, sideways or up, and down
	};
	TArray<FBuilding> Buildings;
	struct FCenter { const TCHAR* Name; FIntPoint Block; };
	const TArray<FCenter> Centers = {
		{TEXT("Holtburg A9B4"), FIntPoint(0xA9, 0xB4)}, {TEXT("Shoushi DA55"), FIntPoint(0xDA, 0x55)},
		{TEXT("C162"), FIntPoint(0xC1, 0x62)}, {TEXT("Sawato C95B"), FIntPoint(0xC9, 0x5B)},
		{TEXT("49E4"), FIntPoint(0x49, 0xE4)}, {TEXT("8A0E"), FIntPoint(0x8A, 0x0E)}};
	TSet<FIntPoint> Blocks;
	for (const FCenter& Center : Centers)
		for (int32 DX = -Rings; DX <= Rings; ++DX)
			for (int32 DY = -Rings; DY <= Rings; ++DY)
			{
				const FIntPoint Block = Center.Block + FIntPoint(DX, DY);
				if (Block.X >= 0 && Block.X < 255 && Block.Y >= 0 && Block.Y < 255) Blocks.Add(Block);
			}

	int32 AllBuildings = 0;
	for (const FIntPoint Block : Blocks)
	{
		const uint32 Key = (uint32(Block.X) << 24) | (uint32(Block.Y) << 16);
		FACEDatLandblockInfo Info;
		if (!Dat->LoadLandblockInfo(Key, Info)) continue;
		for (const FACEDatLandblockBuilding& Source : Info.Buildings)
		{
			const FACEBuiltSetupMesh* Setup = Dat->GetOrBuildSetupMesh(Source.ModelId, WorldScale);
			if (!Setup) continue;
			++AllBuildings;
			TArray<EKind> Kinds;
			PersonalFarSceneryMeshes::OpeningKinds(*Setup, WorldScale, Kinds);
			FBuilding Building;
			Building.Block = Block;
			Building.ModelId = Source.ModelId;
			Building.Frame = FTransform(FACEPosition::AceQuatToUnreal(Source.Orientation.W, FVector(Source.Orientation.X, Source.Orientation.Y, Source.Orientation.Z)),
				FACEPosition::AceVectorToUnreal(FVector(Source.Origin.X, Source.Origin.Y, Source.Origin.Z), WorldScale));
			FBox Shell(ForceInit);
			int32 At = 0;
			for (const FACEBuiltSetupPart& Part : Setup->Parts)
			{
				for (const FACEBuiltMeshSection& Section : Part.Sections)
				{
					if (Section.bCollisionOnly) continue;
					for (const FVector& V : Section.Vertices) Shell += Part.BindTransform.TransformPosition(V);
				}
				for (const FACEBuiltSetupPortal& Polygon : Part.Portals)
				{
					if (Polygon.Vertices.Num() < 3) continue;
					const EKind Kind = Kinds[At++];
					if (!Source.Portals.IsValidIndex(Polygon.PortalIndex)) continue;
					const FACEDatBuildingPortal& Portal = Source.Portals[Polygon.PortalIndex];
					if (Portal.OtherCellId < 0x100 || Portal.OtherCellId == 0xFFFF) continue;
					if (Kind != EKind::Window && Kind != EKind::Entrance && Kind != EKind::None) continue;
					FVector Middle = FVector::ZeroVector;
					for (const FVector& V : Polygon.Vertices) Middle += (Part.BindTransform * Building.Frame).TransformPosition(V);
					Middle /= Polygon.Vertices.Num();
					// The room behind it, then the rooms of the portal's stab list, nearest first.
					TArray<uint32> List;
					List.Add(Key | Portal.OtherCellId);
					TArray<uint32> Behind;
					for (const uint16 Stab : Portal.StabCells)
					{
						if (Stab >= 0x100 && Stab != 0xFFFF && (Key | Stab) != List[0]) Behind.AddUnique(Key | Stab);
					}
					for (const uint32 Id : Behind) GetRoom(Id);
					GetRoom(List[0]);
					Behind.Sort([&](uint32 A, uint32 B) { return FVector::DistSquared(Rooms[A].Origin, Middle) < FVector::DistSquared(Rooms[B].Origin, Middle); });
					List.Append(Behind);
					if (Kind == EKind::None)
					{
						++Building.LargeOpenings;
						for (const uint32 Id : List) Building.LargeRooms.AddUnique(Id);
						continue;
					}
					++Building.Openings;
					{
						// The opening itself, in the landblock's space, for the visibility cut.
						TArray<FVector>& Corners = Building.Holes.AddDefaulted_GetRef();
						for (const FVector& V : Polygon.Vertices) Corners.Add((Part.BindTransform * Building.Frame).TransformPosition(V));
						Building.HoleRooms.Add(List[0]);
					}
					for (int32 I = 0; I < List.Num(); ++I)
					{
						if (I < 1) Building.Deep1.AddUnique(List[I]);
						if (I < 2) Building.Deep2.AddUnique(List[I]);
						Building.Full.AddUnique(List[I]);
					}
				}
			}
			if (Building.Openings == 0 && Building.LargeOpenings == 0) continue;
			for (const uint32 Id : Building.Full) Building.LargeRooms.Remove(Id);
			// Sharing: the same rooms with the same baked light at the same places relative to the building.
			auto MeshKeyOf = [&](const TArray<uint32>& Ids, bool bLit = true)
			{
				TArray<uint32> Parts;
				for (const uint32 Id : Ids)
				{
					const FRoom& Room = Rooms[Id];
					if (!Room.bBuilt) continue;
					const FTransform Relative = Room.ToBlock.GetRelativeTransform(Building.Frame);
					const FVector L = Relative.GetLocation();
					const FQuat Q = Relative.GetRotation();
					uint32 Part = HashCombine(bLit ? Room.LitKey : Room.ShapeKey, GetTypeHash(FIntVector(FMath::RoundToInt32(L.X), FMath::RoundToInt32(L.Y), FMath::RoundToInt32(L.Z))));
					Part = HashCombine(Part, GetTypeHash(FIntVector(FMath::RoundToInt32(FMath::Abs(Q.X) * 1000.), FMath::RoundToInt32(FMath::Abs(Q.Y) * 1000.), FMath::RoundToInt32(FMath::Abs(Q.Z) * 1000.))));
					Parts.Add(Part);
				}
				Parts.Sort();
				uint32 Result = GetTypeHash(Building.ModelId);
				for (const uint32 Part : Parts) Result = HashCombine(Result, Part);
				return Result;
			};
			Building.MeshKey = MeshKeyOf(Building.Full);
			Building.MeshKey2 = MeshKeyOf(Building.Deep2);
			Building.ShapeMeshKey = MeshKeyOf(Building.Full, false);
			// Rooms against the shell, in the building's own space.
			if (Shell.IsValid)
			{
				for (const uint32 Id : Building.Full)
				{
					const FRoom& Room = Rooms[Id];
					if (!Room.bBuilt || !Room.Bounds.IsValid) continue;
					FVector Corners[8];
					Room.Bounds.GetVertices(Corners);
					for (const FVector& Corner : Corners)
					{
						const FVector P = Building.Frame.InverseTransformPosition(Corner);
						const double Side = FMath::Max3(Shell.Min.X - P.X, P.X - Shell.Max.X, FMath::Max3(Shell.Min.Y - P.Y, P.Y - Shell.Max.Y, P.Z - Shell.Max.Z));
						Building.Overhang = FMath::Max(Building.Overhang, float(Side / WorldScale));
						Building.Below = FMath::Max(Building.Below, float((Shell.Min.Z - P.Z) / WorldScale));
					}
				}
			}
			Buildings.Add(MoveTemp(Building));
		}
	}

	// The visibility cut, once per different building mesh: triangles that can be seen from outside through an opening.
	struct FCut { int32 Before = 0, AfterCone = 0, AfterSeen = 0; bool bOccluded = true; double Ms = 0.; int32 Instances = 0; uint32 ModelId = 0; int32 RoomCount = 0; };
	TMap<uint32, FCut> Cuts;
	for (const FBuilding& Building : Buildings)
	{
		FCut& Cut = Cuts.FindOrAdd(Building.MeshKey);
		++Cut.Instances;
		if (Cut.Instances > 1 || Building.Openings == 0) continue;
		Cut.ModelId = Building.ModelId;
		Cut.RoomCount = Building.Full.Num();
		const double Start = FPlatformTime::Seconds();
		// Triangles of its rooms and its far-room openings, in the landblock's space.
		TArray<FVector> Tri;
		for (const uint32 Id : Building.Full)
		{
			const FRoom& Room = Rooms[Id];
			if (!Room.bBuilt) continue;
			for (const FACEBuiltMeshSection& Section : Room.Mesh->Sections)
				for (int32 T = 0; T + 2 < Section.Triangles.Num(); T += 3)
					for (int32 K = 0; K < 3; ++K) Tri.Add(Room.ToBlock.TransformPosition(Section.Vertices[Section.Triangles[T + K]]));
		}
		Cut.Before = Tri.Num() / 3;
		struct FHole { TArray<FVector> Corners; FVector Normal; FVector Middle; };
		TArray<FHole> Holes;
		for (int32 H = 0; H < Building.Holes.Num(); ++H)
		{
			FHole Hole;
			Hole.Corners = Building.Holes[H];
			Hole.Middle = FVector::ZeroVector;
			FVector Cross = FVector::ZeroVector;
			for (int32 I = 0; I < Hole.Corners.Num(); ++I)
			{
				Hole.Middle += Hole.Corners[I];
				if (I > 0 && I + 1 < Hole.Corners.Num()) Cross += FVector::CrossProduct(Hole.Corners[I] - Hole.Corners[0], Hole.Corners[I + 1] - Hole.Corners[0]);
			}
			Hole.Middle /= Hole.Corners.Num();
			Hole.Normal = Cross.GetSafeNormal();
			// Outward: away from the room behind it.
			const FRoom* Behind = Rooms.Find(Building.HoleRooms[H]);
			if (Behind && Behind->Bounds.IsValid && FVector::DotProduct(Hole.Normal, Hole.Middle - Behind->Bounds.GetCenter()) < 0.) Hole.Normal = -Hole.Normal;
			if (!Hole.Normal.IsNearlyZero()) Holes.Add(MoveTemp(Hole));
		}
		TArray<FVector> Eyes;
		TArray<int32> EyeHole;
		for (int32 H = 0; H < Holes.Num(); ++H)
		{
			const FVector N = Holes[H].Normal;
			const FVector R = FVector::CrossProduct(N, FVector::UpVector).GetSafeNormal();
			if (R.IsNearlyZero()) continue;
			for (const double A : {-1.5, -0.6, 0., 0.6, 1.5})
				for (const double B : {-0.2, 0.1, 0.6})
				{
					Eyes.Add(Holes[H].Middle + (N + R * A + FVector::UpVector * B).GetSafeNormal() * 3000.);
					EyeHole.Add(H);
				}
		}
		auto Through = [](const FHole& Hole, const FVector& From, const FVector& To, FVector& OutHit)
		{
			const double D0 = FVector::DotProduct(From - Hole.Corners[0], Hole.Normal), D1 = FVector::DotProduct(To - Hole.Corners[0], Hole.Normal);
			if (D0 <= 0. || D1 >= 0.) return false;
			OutHit = From + (To - From) * (D0 / (D0 - D1));
			double Sign = 0.;
			for (int32 I = 0; I < Hole.Corners.Num(); ++I)
			{
				const FVector& A = Hole.Corners[I];
				const FVector& B = Hole.Corners[(I + 1) % Hole.Corners.Num()];
				const double S = FVector::DotProduct(FVector::CrossProduct(B - A, OutHit - A), Hole.Normal);
				if (FMath::Abs(S) < 1.e-6) continue;
				if (Sign != 0. && (S > 0.) != (Sign > 0.)) return false;
				Sign = S;
			}
			return true;
		};
		const int32 Count = Tri.Num() / 3;
		const bool bOcclude = Count <= 6000;
		Cut.bOccluded = bOcclude;
		for (int32 T = 0; T < Count; ++T)
		{
			const FVector A = Tri[T * 3], B = Tri[T * 3 + 1], C = Tri[T * 3 + 2];
			const FVector Mid = (A + B + C) / 3.;
			const FVector Points[4] = {Mid, FMath::Lerp(Mid, A, 0.8), FMath::Lerp(Mid, B, 0.8), FMath::Lerp(Mid, C, 0.8)};
			bool bCone = false, bSeen = false;
			for (int32 E = 0; E < Eyes.Num() && !bSeen; ++E)
			{
				for (int32 P = 0; P < 4 && !bSeen; ++P)
				{
					FVector Hit;
					if (!Through(Holes[EyeHole[E]], Eyes[E], Points[P], Hit)) continue;
					bCone = true;
					if (!bOcclude) { bSeen = true; break; }
					// Blocked by another triangle between the opening and the point?
					bool bBlocked = false;
					const FVector Dir = Points[P] - Hit;
					for (int32 O = 0; O < Count && !bBlocked; ++O)
					{
						if (O == T) continue;
						const FVector E1 = Tri[O * 3 + 1] - Tri[O * 3], E2 = Tri[O * 3 + 2] - Tri[O * 3];
						const FVector PV = FVector::CrossProduct(Dir, E2);
						const double Det = FVector::DotProduct(E1, PV);
						if (FMath::Abs(Det) < 1.e-9) continue;
						const FVector TV = Hit - Tri[O * 3];
						const double U = FVector::DotProduct(TV, PV) / Det;
						if (U < 0. || U > 1.) continue;
						const FVector QV = FVector::CrossProduct(TV, E1);
						const double V = FVector::DotProduct(Dir, QV) / Det;
						if (V < 0. || U + V > 1.) continue;
						const double Along = FVector::DotProduct(E2, QV) / Det;
						bBlocked = Along > 0.01 && Along < 0.99;
					}
					bSeen = !bBlocked;
				}
			}
			Cut.AfterCone += bCone ? 1 : 0;
			Cut.AfterSeen += bSeen ? 1 : 0;
		}
		Cut.Ms = (FPlatformTime::Seconds() - Start) * 1000.;
	}

	// Textures of the surfaces the rooms use.
	TMap<uint32, int64> SurfaceBytes;
	auto BytesOf = [&](uint32 SurfaceId) -> int64
	{
		if (const int64* Found = SurfaceBytes.Find(SurfaceId)) return *Found;
		FACEDatDecodedSurface Decoded;
		const int64 Bytes = OwnTextures.ResolveSurface(SurfaceId, Decoded) ? int64(Decoded.Width) * Decoded.Height * 4 * 4 / 3 : 0;
		SurfaceBytes.Add(SurfaceId, Bytes);
		return Bytes;
	};

	// Totals for a set of buildings.
	struct FTotals { int32 Buildings = 0, Openings = 0, Rooms = 0, Triangles = 0, Sections = 0, Surfaces = 0, Shapes = 0, Lits = 0, Meshes = 0, UniqueTriangles = 0, SeenTriangles = 0, ConeTriangles = 0, LargeOpenings = 0, LargeRooms = 0, LargeTriangles = 0; int64 TextureBytes = 0, VertexBytes = 0, UniqueVertexBytes = 0; };
	auto Sum = [&](TFunctionRef<bool(const FBuilding&)> Take, int32 Deep) -> FTotals
	{
		FTotals Totals;
		TSet<uint32> Surfaces, Shapes, Lits, Meshes, AllRooms;
		for (const FBuilding& Building : Buildings)
		{
			if (!Take(Building)) continue;
			Totals.LargeOpenings += Building.LargeOpenings;
			for (const uint32 Id : Building.LargeRooms)
			{
				const FRoom& Room = Rooms[Id];
				++Totals.LargeRooms;
				Totals.LargeTriangles += Room.Triangles;
			}
			if (Building.Openings == 0) continue;
			++Totals.Buildings;
			Totals.Openings += Building.Openings;
			const TArray<uint32>& Ids = Deep == 1 ? Building.Deep1 : (Deep == 2 ? Building.Deep2 : Building.Full);
			int32 Triangles = 0;
			int64 VertexBytes = 0;
			for (const uint32 Id : Ids)
			{
				const FRoom& Room = Rooms[Id];
				if (!Room.bBuilt) continue;
				++Totals.Rooms;
				Triangles += Room.Triangles;
				Totals.Sections += Room.Sections;
				// Position, normal and tangent, one UV, color; and a 16-bit index per corner.
				VertexBytes += int64(Room.Vertices) * (12 + 8 + 8 + 4) + int64(Room.Triangles) * 6;
				Shapes.Add(Room.ShapeKey);
				Lits.Add(Room.LitKey);
				for (const uint32 Surface : Room.Surfaces) Surfaces.Add(Surface);
			}
			Totals.Triangles += Triangles;
			Totals.VertexBytes += VertexBytes;
			bool bNew = false;
			Meshes.Add(Deep == 2 ? Building.MeshKey2 : Building.MeshKey, &bNew);
			if (!bNew)
			{
				Totals.UniqueTriangles += Triangles;
				Totals.UniqueVertexBytes += VertexBytes;
				if (const FCut* Cut = Deep == 0 ? Cuts.Find(Building.MeshKey) : nullptr)
				{
					Totals.SeenTriangles += Cut->AfterSeen;
					Totals.ConeTriangles += Cut->AfterCone;
				}
			}
		}
		Totals.Surfaces = Surfaces.Num();
		Totals.Shapes = Shapes.Num();
		Totals.Lits = Lits.Num();
		Totals.Meshes = Meshes.Num();
		for (const uint32 Surface : Surfaces) Totals.TextureBytes += BytesOf(Surface);
		return Totals;
	};
	auto Mb = [](int64 Bytes) { return double(Bytes) / (1024. * 1024.); };

	FString Out = TEXT("Far rooms survey (stage 5). Rooms behind windows and open entrances up to 12 m2; \"deep\" is rooms per opening (1: the room behind it, 2: and the nearest of its stab list, all: the whole stab list).\n\n");
	Out += TEXT("1. Totals, every surveyed landblock (within 8 of Holtburg A9B4, Shoushi DA55, C162, C95B, 49E4, 8A0E)\n");
	Out += FString::Printf(TEXT("%d buildings, %d with windows or open entrances.\n"), AllBuildings, Sum([](const FBuilding& B) { return true; }, 0).Buildings);
	Out += TEXT("deep | openings | rooms | triangles | sections | surfaces | textures MB | mesh MB\n");
	for (const int32 Deep : {1, 2, 0})
	{
		const FTotals T = Sum([](const FBuilding& B) { return true; }, Deep);
		const FString Line = FString::Printf(TEXT("%s | %d | %d | %d | %d | %d | %.1f | %.1f"), Deep == 0 ? TEXT("all") : *FString::FromInt(Deep), T.Openings, T.Rooms, T.Triangles, T.Sections, T.Surfaces, Mb(T.TextureBytes), Mb(T.VertexBytes));
		Out += Line + TEXT("\n");
		AddInfo(TEXT("Totals, deep ") + Line);
	}
	{
		const FTotals T = Sum([](const FBuilding& B) { return true; }, 0);
		Out += FString::Printf(TEXT("Openings over 12 m2: %d, with %d rooms only they lead to, %d triangles.\n"), T.LargeOpenings, T.LargeRooms, T.LargeTriangles);
		Out += TEXT("\n2. Sharing (deep all)\n");
		Out += FString::Printf(TEXT("%d rooms: %d different by environment, cell structure and surfaces; %d different with the baked colors.\n"), T.Rooms, T.Shapes, T.Lits);
		Out += FString::Printf(TEXT("%d buildings: %d different far-room meshes (%.1f buildings per mesh). Triangles %d, in the different meshes only %d. Mesh memory %.1f MB, shared %.1f MB.\n"),
			T.Buildings, T.Meshes, T.Meshes > 0 ? double(T.Buildings) / T.Meshes : 0., T.Triangles, T.UniqueTriangles, Mb(T.VertexBytes), Mb(T.UniqueVertexBytes));
		const FTotals T2 = Sum([](const FBuilding& B) { return true; }, 2);
		Out += FString::Printf(TEXT("At deep 2: %d different meshes, %d triangles in them.\n"), T2.Meshes, T2.UniqueTriangles);
		{
			TSet<uint32> ShapeMeshes, Models;
			for (const FBuilding& Building : Buildings)
			{
				if (Building.Openings > 0) { ShapeMeshes.Add(Building.ShapeMeshKey); Models.Add(Building.ModelId); }
			}
			Out += FString::Printf(TEXT("Without the baked colors: %d different meshes. Building models among them: %d.\n"), ShapeMeshes.Num(), Models.Num());
			AddInfo(FString::Printf(TEXT("Sharing without baked colors: %d meshes, %d models"), ShapeMeshes.Num(), Models.Num()));
		}
		AddInfo(FString::Printf(TEXT("Sharing: %d rooms, %d shapes, %d with baked colors; %d buildings, %d meshes"), T.Rooms, T.Shapes, T.Lits, T.Buildings, T.Meshes));
	}

	Out += TEXT("\n3. The visibility cut, per different far-room mesh (deep all): triangles before, in the view cone of an opening, and seen (not hidden behind another room triangle)\n");
	Out += TEXT("model | buildings | rooms | before | in cone | seen | ms\n");
	Cuts.ValueSort([](const FCut& A, const FCut& B) { return A.Before > B.Before; });
	int32 CutBefore = 0, CutCone = 0, CutSeen = 0, NotOccluded = 0;
	double CutMs = 0.;
	for (const auto& Pair : Cuts)
	{
		const FCut& Cut = Pair.Value;
		if (Cut.Before == 0) continue;
		Out += FString::Printf(TEXT("0x%08X | %d | %d | %d | %d | %d%s | %.0f\n"), Cut.ModelId, Cut.Instances, Cut.RoomCount, Cut.Before, Cut.AfterCone, Cut.AfterSeen, Cut.bOccluded ? TEXT("") : TEXT(" (cone only: too many triangles for the test's blocking check)"), Cut.Ms);
		CutBefore += Cut.Before; CutCone += Cut.AfterCone; CutSeen += Cut.AfterSeen; CutMs += Cut.Ms; NotOccluded += Cut.bOccluded ? 0 : 1;
	}
	Out += FString::Printf(TEXT("All different meshes: %d before, %d in cone, %d seen (%d meshes cone only); %.1f s for the cut in this test.\n"), CutBefore, CutCone, CutSeen, NotOccluded, CutMs / 1000.);
	AddInfo(FString::Printf(TEXT("Visibility cut: %d triangles before, %d in cone, %d seen"), CutBefore, CutCone, CutSeen));

	Out += TEXT("\n4. Build time (own builder, own DAT handles, this machine, one thread)\n");
	{
		int32 Built = 0;
		for (const auto& Pair : Rooms) Built += Pair.Value.bBuilt ? 1 : 0;
		Out += FString::Printf(TEXT("%d rooms built, %d failed; %.1f s in all, %.2f ms per room on average, longest %.1f ms.\n"), Built, Rooms.Num() - Built, BuildMsTotal / 1000., Rooms.Num() > 0 ? BuildMsTotal / Rooms.Num() : 0., BuildMsMax);
		double PerBuilding = 0., MaxBuilding = 0.;
		int32 Counted = 0, MaxRooms = 0, MaxTriangles = 0;
		for (const FBuilding& Building : Buildings)
		{
			if (Building.Openings == 0) continue;
			double Ms = 0.;
			int32 Triangles = 0;
			for (const uint32 Id : Building.Full) { Ms += Rooms[Id].BuildMs; Triangles += Rooms[Id].Triangles; }
			PerBuilding += Ms; MaxBuilding = FMath::Max(MaxBuilding, Ms); ++Counted;
			MaxRooms = FMath::Max(MaxRooms, Building.Full.Num()); MaxTriangles = FMath::Max(MaxTriangles, Triangles);
		}
		Out += FString::Printf(TEXT("Per building (deep all): %.1f ms on average, longest %.0f ms; most rooms in one building %d, most triangles %d.\n"), Counted > 0 ? PerBuilding / Counted : 0., MaxBuilding, MaxRooms, MaxTriangles);
		AddInfo(FString::Printf(TEXT("Build: %d rooms, %.2f ms per room, longest %.1f ms"), Built, Rooms.Num() > 0 ? BuildMsTotal / Rooms.Num() : 0., BuildMsMax));
	}

	Out += TEXT("\n5. Totals for rings 2 to the radius around a town (far rooms are for rings 2 and further)\n");
	Out += TEXT("town, radius | buildings | openings | deep | rooms | triangles | meshes | triangles shared | seen after cut | sections | surfaces | textures MB | mesh MB shared | light cards (triangles)\n");
	for (const FCenter& Center : {Centers[0], Centers[1]})
	{
		for (const int32 Radius : {6, 8})
		{
			auto Take = [&](const FBuilding& B) { const int32 Ring = PersonalFarScenery::Ring(B.Block, Center.Block); return Ring >= 2 && Ring <= Radius; };
			for (const int32 Deep : {1, 2, 0})
			{
				const FTotals T = Sum(Take, Deep);
				const FString Line = FString::Printf(TEXT("%s, %d | %d | %d | %s | %d | %d | %d | %d | %s | %d | %d | %.1f | %.1f | %d"), Center.Name, Radius, T.Buildings, T.Openings,
					Deep == 0 ? TEXT("all") : *FString::FromInt(Deep), T.Rooms, T.Triangles, T.Meshes, T.UniqueTriangles, Deep == 0 ? *FString::FromInt(T.SeenTriangles) : TEXT("-"),
					T.Sections, T.Surfaces, Mb(T.TextureBytes), Mb(T.UniqueVertexBytes), T.Openings * 2);
				Out += Line + TEXT("\n");
				AddInfo(Line);
			}
		}
	}

	Out += TEXT("\n1b. Per landblock with far-room buildings (deep all): buildings | openings | rooms | triangles\n");
	{
		TMap<FIntPoint, FIntVector4> PerBlock;
		for (const FBuilding& Building : Buildings)
		{
			if (Building.Openings == 0) continue;
			FIntVector4& Entry = PerBlock.FindOrAdd(Building.Block, FIntVector4(0, 0, 0, 0));
			Entry.X += 1; Entry.Y += Building.Openings; Entry.Z += Building.Full.Num();
			for (const uint32 Id : Building.Full) Entry.W += Rooms[Id].Triangles;
		}
		PerBlock.ValueSort([](const FIntVector4& A, const FIntVector4& B) { return A.W > B.W; });
		for (const auto& Pair : PerBlock) Out += FString::Printf(TEXT("%02X%02X | %d | %d | %d | %d\n"), Pair.Key.X, Pair.Key.Y, Pair.Value.X, Pair.Value.Y, Pair.Value.Z, Pair.Value.W);
	}

	Out += TEXT("\n6. Rooms against their building's shell (deep all): how far the rooms' boxes reach past the shell's box\n");
	{
		int32 Inside = 0, Side = 0, Below = 0;
		float MostSide = 0.f, MostBelow = 0.f;
		TMap<uint32, FVector2f> ByModel;
		for (const FBuilding& Building : Buildings)
		{
			if (Building.Openings == 0) continue;
			Inside += (Building.Overhang <= 0.1f && Building.Below <= 0.1f) ? 1 : 0;
			Side += Building.Overhang > 0.1f ? 1 : 0;
			Below += Building.Below > 0.1f ? 1 : 0;
			MostSide = FMath::Max(MostSide, Building.Overhang); MostBelow = FMath::Max(MostBelow, Building.Below);
			FVector2f& Entry = ByModel.FindOrAdd(Building.ModelId, FVector2f::ZeroVector);
			Entry.X = FMath::Max(Entry.X, Building.Overhang); Entry.Y = FMath::Max(Entry.Y, Building.Below);
		}
		Out += FString::Printf(TEXT("%d buildings within 10 cm of the shell's box; %d reach past it sideways or up (most %.1f m); %d reach below it (most %.1f m).\n"), Inside, Side, MostSide, Below, MostBelow);
		for (const auto& Pair : ByModel)
		{
			if (Pair.Value.X > 0.1f || Pair.Value.Y > 0.1f) Out += FString::Printf(TEXT("0x%08X: %.1f m sideways or up, %.1f m below\n"), Pair.Key, Pair.Value.X, Pair.Value.Y);
		}
		AddInfo(FString::Printf(TEXT("Shell: %d inside, %d past it sideways or up (most %.1f m), %d below (most %.1f m)"), Inside, Side, MostSide, Below, MostBelow));
	}

	// 6b. The room materials as saved for the packaged client: can they draw on static and instanced static meshes?
	Out += TEXT("\n6b. Room material bases in Content/ACE/RuntimeMaterials\n");
	for (const TCHAR* Name : {TEXT("M_ACEEnvCell_v3"), TEXT("M_ACEEnvCellMasked_v3")})
	{
		const UMaterial* Material = LoadObject<UMaterial>(nullptr, *FString::Printf(TEXT("/Game/ACE/RuntimeMaterials/%s.%s"), Name, Name));
		const FString Line = Material
			? FString::Printf(TEXT("%s: static mesh %d, instanced static mesh %d, blend %d, two-sided %d, shading models field 0x%X"), Name,
				Material->GetUsageByFlag(MATUSAGE_StaticMesh) ? 1 : 0, Material->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes) ? 1 : 0,
				int32(Material->GetBlendMode()), Material->IsTwoSided() ? 1 : 0, uint32(Material->GetShadingModels().GetShadingModelField()))
			: FString::Printf(TEXT("%s: not found"), Name);
		Out += Line + TEXT("\n");
		AddInfo(Line);
	}
	{
		// What a room's sections get: the kinds of material, over every surface the surveyed rooms use.
		int32 Opaque = 0, Masked = 0, Other = 0, None = 0, Wrap = 0, SectionsSeen = 0;
		TSet<uint32> Seen;
		for (const auto& Pair : Rooms)
		{
			if (!Pair.Value.bBuilt) continue;
			for (const FACEBuiltMeshSection& Section : Pair.Value.Mesh->Sections)
			{
				if (Section.IsEmpty()) continue;
				++SectionsSeen;
				Wrap += Section.bWrapTexture ? 1 : 0;
				bool bNew = false;
				Seen.Add(HashCombine(GetTypeHash(Section.SurfaceId), GetTypeHash(Section.bWrapTexture)), &bNew);
				if (bNew) continue;
				const UMaterialInterface* Material = Dat->GetOrCreateEnvCellMaterial(Section.SurfaceId, Section.bWrapTexture);
				const FString Base = Material && Material->GetMaterial() ? Material->GetMaterial()->GetName() : FString();
				(!Material ? None : (Base.StartsWith(TEXT("M_ACEEnvCellMasked")) ? Masked : (Base.StartsWith(TEXT("M_ACEEnvCell")) ? Opaque : Other)))++;
			}
		}
		const FString Line = FString::Printf(TEXT("Surfaces of the surveyed rooms (with wrap or clamp): %d get the room material, %d the masked one, %d another material (additive or see-through), %d none; %d of %d sections wrap their texture."),
			Opaque, Masked, Other, None, Wrap, SectionsSeen);
		Out += Line + TEXT("\n");
		AddInfo(Line);
	}

	// 7. The same room drawn twice in the same place: does it flicker?
	if (FApp::CanEverRender())
	{
		uint32 Pick = 0;
		for (const FBuilding& Building : Buildings)
		{
			if (Building.Block == FIntPoint(0xA9, 0xB4) && Building.Deep2.Num() > 1) { Pick = Building.Deep2[1]; break; }
		}
		const FRoom* Room = Pick != 0 ? Rooms.Find(Pick) : nullptr;
		if (Room && Room->bBuilt && Room->Bounds.IsValid)
		{
			AActor* Owner = World->SpawnActor<AActor>();
			auto MakeCopy = [&]()
			{
				UProceduralMeshComponent* Copy = NewObject<UProceduralMeshComponent>(Owner);
				Copy->RegisterComponent();
				int32 Index = 0;
				for (const FACEBuiltMeshSection& Section : Room->Mesh->Sections)
				{
					if (Section.IsEmpty()) continue;
					UMaterialInterface* Material = Dat->GetOrCreateEnvCellMaterial(Section.SurfaceId, Section.bWrapTexture);
					if (!Material) continue;
					TArray<FVector> Vertices;
					for (const FVector& V : Section.Vertices) Vertices.Add(Room->ToBlock.TransformPosition(V));
					TArray<FLinearColor> Colors = Section.VertexColors;
					Colors.SetNum(Vertices.Num());
					Copy->CreateMeshSection_LinearColor(Index, Vertices, Section.Triangles, Section.Normals, Section.UVs, Colors, TArray<FProcMeshTangent>(), false, false);
					Copy->SetMaterial(Index, Material);
					++Index;
				}
				return Copy;
			};
			UProceduralMeshComponent* First = MakeCopy();
			UProceduralMeshComponent* Second = MakeCopy();
			USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(Owner);
			Capture->RegisterComponent();
			UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Owner);
			Target->ClearColor = FLinearColor::Black;
			Target->InitCustomFormat(128, 128, PF_FloatRGBA, true);
			Target->UpdateResourceImmediate(true);
			Capture->TextureTarget = Target;
			Capture->FOVAngle = 90;
			Capture->CaptureSource = SCS_SceneColorHDRNoAlpha;
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
			Capture->ShowFlags.SetAtmosphere(false);
			Capture->ShowFlags.SetFog(false);
			Capture->ShowFlags.SetBloom(false);
			Capture->ShowFlags.SetEyeAdaptation(false);
			Capture->ShowFlags.SetAntiAliasing(false);
			auto Read = [&](TArray<FFloat16Color>& Pixels)
			{
				if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
				World->SendAllEndOfFrameUpdates();
				Capture->CaptureScene();
				FlushRenderingCommands();
				Target->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels);
			};
			double Worst = 0.;
			int64 Differing = 0, Lit = 0;
			for (int32 View = 0; View < 12; ++View)
			{
				// From the middle of the room, turning, and a little off center each time.
				Capture->SetWorldLocationAndRotation(Room->Bounds.GetCenter() + FVector(View * 3.7, View * -2.3, View * 1.1), FRotator(View % 3 == 0 ? -20. : 10., View * 30., 0.));
				TArray<FFloat16Color> One, Two;
				Capture->ClearShowOnlyComponents();
				Capture->ShowOnlyComponent(First);
				Read(One);
				Capture->ShowOnlyComponent(Second);
				Read(Two);
				for (int32 I = 0; I < One.Num() && I < Two.Num(); ++I)
				{
					const FLinearColor A(One[I]), B(Two[I]);
					const double Diff = FMath::Max3(FMath::Abs(A.R - B.R), FMath::Abs(A.G - B.G), FMath::Abs(A.B - B.B));
					Worst = FMath::Max(Worst, Diff);
					Differing += Diff > 0.004 ? 1 : 0;
					Lit += (A.R + A.G + A.B) > 0.001f ? 1 : 0;
				}
			}
			const FString Line = FString::Printf(TEXT("Room 0x%08X drawn once and twice in the same place, 12 views of 128 x 128: %lld pixels show the room, %lld differ by more than 0.004, largest difference %.4f."), Pick, Lit, Differing, Worst);
			Out += TEXT("\n7. Two copies of a room in one place\n") + Line + TEXT("\n");
			AddInfo(Line);
			TestTrue(TEXT("The capture shows the room"), Lit > 1000);
		}
	}
	else
	{
		Out += TEXT("\n7. Two copies of a room in one place: needs -RenderOffscreen, not run.\n");
	}

	const FString File = FPaths::ProjectSavedDir() / TEXT("Personal/FarRoomsSurvey.txt");
	TestTrue(TEXT("The survey file is written"), FFileHelper::SaveStringToFile(Out, *File));
	TestTrue(TEXT("Rooms were found"), Rooms.Num() > 100 && Buildings.Num() > 10);
	return !HasAnyErrors();
}

// Stage 6 of far windows, first: a building's far rooms as one static mesh against the client's own rooms in the same place.
// Needs -RenderOffscreen for the pictures; the places are compared without.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarWindowsRoomsMatchTest, "ACE.Personal.FarWindows.RoomsMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarWindowsRoomsMatchTest::RunTest(const FString& Parameters)
{
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (!FPaths::DirectoryExists(DatDirectory))
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
	UACEDatSubsystem* Dat = Game->GetSubsystem<UACEDatSubsystem>();
	if (!TestNotNull(TEXT("The DAT subsystem exists"), Dat) || !TestTrue(TEXT("DAT files load"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}
	constexpr float WorldScale = 100.f;
	const uint32 Key = 0xA9B40000u;
	const FVector BlockOrigin = FACEPosition::AceVectorToUnreal(FVector(0xA9 * 192., 0xB4 * 192., 0.), WorldScale);
	FACEDatLandblockInfo Info;
	if (!TestTrue(TEXT("Holtburg's landblock loads"), Dat->LoadLandblockInfo(Key, Info) && Info.Buildings.Num() > 0))
	{
		return false;
	}
	const PersonalFarRoomsMeshes::FSourcePtr Source = PersonalFarRoomsMeshes::Open(DatDirectory);
	if (!TestTrue(TEXT("Own DAT handles open"), Source.IsValid()))
	{
		return false;
	}
	AActor* Owner = World->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(Owner);
	Owner->SetRootComponent(Root);
	Root->RegisterComponent();

	// Every building of the landblock that has far-room openings: its rooms by the client, and its far-room mesh.
	struct FPair { TArray<AACEEnvCellActor*> Client; UStaticMeshComponent* Far = nullptr; FBox ClientBox = FBox(ForceInit); TArray<FVector> Views; int32 Rooms = 0; uint32 ModelId = 0; };
	TArray<FPair> Pairs;
	int32 Triangles = 0, Sections = 0, DoorMaterials = 0;
	double WorstPlace = 0.;
	TSet<uint32> Keys;
	for (const FACEDatLandblockBuilding& Building : Info.Buildings)
	{
		const FACEBuiltSetupMesh* Setup = Dat->GetOrBuildSetupMesh(Building.ModelId, WorldScale);
		if (!Setup) continue;
		TArray<PersonalFarWindows::EKind> Kinds;
		PersonalFarSceneryMeshes::OpeningKinds(*Setup, WorldScale, Kinds);
		TArray<uint32> RoomIds;
		int32 At = 0;
		for (const FACEBuiltSetupPart& Part : Setup->Parts)
		{
			for (const FACEBuiltSetupPortal& Polygon : Part.Portals)
			{
				if (Polygon.Vertices.Num() < 3) continue;
				const PersonalFarWindows::EKind Kind = Kinds[At++];
				if (!PersonalFarRooms::GetsRooms(Kind) || !Building.Portals.IsValidIndex(Polygon.PortalIndex)) continue;
				PersonalFarRooms::AddRooms(Key, Building.Portals[Polygon.PortalIndex].OtherCellId, Building.Portals[Polygon.PortalIndex].StabCells, RoomIds);
			}
		}
		if (RoomIds.Num() == 0) continue;
		FPair& Pair = Pairs.AddDefaulted_GetRef();
		Pair.ModelId = Building.ModelId;
		Pair.Rooms = RoomIds.Num();
		const FTransform Frame(FACEPosition::AceQuatToUnreal(Building.Orientation.W, FVector(Building.Orientation.X, Building.Orientation.Y, Building.Orientation.Z)),
			FACEPosition::AceVectorToUnreal(FVector(Building.Origin.X, Building.Origin.Y, Building.Origin.Z), WorldScale));
		// The client's rooms, as its presenter makes them.
		for (const uint32 Id : RoomIds)
		{
			const FACEBuiltEnvCellMesh* Built = Dat->GetOrBuildEnvCellMesh(Id, WorldScale);
			if (!Built) continue;
			AACEEnvCellActor* Room = World->SpawnActor<AACEEnvCellActor>(AACEEnvCellActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!Room || !Room->LoadEnvCell(int32(Id), BlockOrigin, WorldScale))
			{
				if (Room) Room->Destroy();
				continue;
			}
			Room->SetEnvCellHiddenInGame(false);
			Pair.Client.Add(Room);
			if (Room->CellMesh && Room->CellMesh->GetNumSections() > 0)
			{
				// The exact box of what is drawn, corner by corner.
				const FTransform ToWorld = Built->GetCellLocalToLandblock(WorldScale) * FTransform(BlockOrigin);
				FBox Box(ForceInit);
				for (const FACEBuiltMeshSection& Section : Built->Sections)
				{
					if (Section.IsEmpty() || Section.bFullyTransparent) continue;
					for (const FVector& V : Section.Vertices) Box += ToWorld.TransformPosition(V);
				}
				if (!Box.IsValid) continue;
				Pair.ClientBox += Box;
				Pair.Views.Add(Box.GetCenter());
			}
		}
		// The far rooms: built by the own builder, one static mesh, where the building stands.
		const PersonalFarRoomsMeshes::FBuiltPtr Built = PersonalFarRoomsMeshes::Build(*Source, RoomIds, Frame, WorldScale);
		TArray<UMaterialInterface*> Materials;
		TArray<int32> SlotSections;
		UStaticMesh* Mesh = PersonalFarRoomsMeshes::MakeStaticMesh(*Dat, Owner, *Built, Materials, &SlotSections);
		if (!TestNotNull(TEXT("A building's far-room mesh is made"), Mesh)) continue;
		Keys.Add(Built->Key);
		Triangles += Built->Triangles;
		Sections += Built->Sections.Num();
		TestEqual(TEXT("Every room of the list is in the far-room mesh"), Built->Rooms, RoomIds.Num());
		TestEqual(TEXT("The static mesh has the triangles the rooms have"), Mesh->GetNumTriangles(0), Built->Triangles);
		Pair.Far = NewObject<UPersonalFarRoomComponent>(Owner);
		Pair.Far->SetupAttachment(Root);
		Pair.Far->SetStaticMesh(Mesh);
		Pair.Far->SetMobility(EComponentMobility::Movable);
		Pair.Far->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pair.Far->SetCastShadow(false);
		Pair.Far->SetLightingChannels(false, true, false);
		Pair.Far->SetWorldTransform(Frame * FTransform(BlockOrigin));
		Pair.Far->RegisterComponent();
		// Never occlusion culled, as the client's rooms.
		if (Pair.Far->SceneProxy)
		{
			TestFalse(TEXT("A far room cannot be occluded"), Pair.Far->SceneProxy->CanBeOccluded());
			for (const AACEEnvCellActor* Client : Pair.Client)
			{
				if (Client && Client->CellMesh && Client->CellMesh->SceneProxy) TestFalse(TEXT("The client's room cannot be occluded either"), Client->CellMesh->SceneProxy->CanBeOccluded());
			}
		}
		// The client's doorway shading, on the far rooms too.
		TArray<UMaterialInstanceDynamic*> Shaded;
		PersonalFarRoomsMeshes::ApplyDoorShading(*Pair.Far, *Built, SlotSections, Frame * FTransform(BlockOrigin), Shaded);
		DoorMaterials += Shaded.Num();
		// Places: the far rooms' box against the client's rooms' box.
		FBox FarBox(ForceInit);
		for (const PersonalFarRoomsMeshes::FSection& Section : Built->Sections)
			for (const FVector3f& V : Section.Vertices) FarBox += (Frame * FTransform(BlockOrigin)).TransformPosition(FVector(V));
		if (Pair.ClientBox.IsValid)
		{
			const double Off = FMath::Max((FarBox.Min - Pair.ClientBox.Min).GetAbsMax(), (FarBox.Max - Pair.ClientBox.Max).GetAbsMax());
			WorstPlace = FMath::Max(WorstPlace, Off);
			TestTrue(FString::Printf(TEXT("Building 0x%08X: the far rooms are where the client's rooms are, within 1 cm (%.3f cm)"), Building.ModelId, Off), Off <= 1.);
		}
		// The same again gives the same key; one room less gives another.
		const PersonalFarRoomsMeshes::FBuiltPtr Again = PersonalFarRoomsMeshes::Build(*Source, RoomIds, Frame, WorldScale);
		TestEqual(TEXT("Built twice, the sharing key is the same"), Again->Key, Built->Key);
		if (RoomIds.Num() > 1)
		{
			TArray<uint32> Fewer(RoomIds);
			Fewer.Pop();
			TestNotEqual(TEXT("One room less gives another sharing key"), PersonalFarRoomsMeshes::Build(*Source, Fewer, Frame, WorldScale)->Key, Built->Key);
		}
	}
	TestTrue(TEXT("Holtburg has buildings with far rooms"), Pairs.Num() >= 5);
	AddInfo(FString::Printf(TEXT("Holtburg A9B4: %d buildings with far rooms, %d different meshes, %d sections (%d with the doorway shading), %d triangles; largest difference in place %.3f cm"),
		Pairs.Num(), Keys.Num(), Sections, DoorMaterials, Triangles, WorstPlace));
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("The picture comparison needs -RenderOffscreen; skipped"));
		return !HasAnyErrors();
	}

	USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(Owner);
	Capture->RegisterComponent();
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Owner);
	Target->ClearColor = FLinearColor::Black;
	Target->InitCustomFormat(96, 96, PF_FloatRGBA, true);
	Target->UpdateResourceImmediate(true);
	Capture->TextureTarget = Target;
	Capture->FOVAngle = 90;
	Capture->CaptureSource = SCS_SceneColorHDRNoAlpha;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->ShowFlags.SetAtmosphere(false);
	Capture->ShowFlags.SetFog(false);
	Capture->ShowFlags.SetBloom(false);
	Capture->ShowFlags.SetEyeAdaptation(false);
	Capture->ShowFlags.SetAntiAliasing(false);
	auto Read = [&](TArray<FFloat16Color>& Pixels)
	{
		if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
		World->SendAllEndOfFrameUpdates();
		Capture->CaptureScene();
		FlushRenderingCommands();
		Target->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Pixels);
	};
	// A sun for the day pictures, on the default lighting channel as the client's is.
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>();
	Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sun->SetActorRotation(FRotator(-50., 30., 0.));
	Sun->GetLightComponent()->SetIntensity(6.f);
	// The client darkens a room near its outside doorways (per-room material instances with Door0, Door1) and has
	// spot lights there that only reach 40 m. The lights are off here, as they are at the distance of a handoff.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(TEXT("ACEDoorwaySpill"))) It->SetActorHiddenInGame(true);
	}
	const bool FarDoorShade = true;
	for (int32 Mode = 0; Mode < 4; ++Mode)
	{
		const bool bDay = (Mode & 1) != 0;
		const bool bDoorShade = Mode < 2;
		if (Mode == 2)
		{
			for (const FPair& Pair : Pairs)
			{
				for (int32 I = 0; Pair.Far && I < Pair.Far->GetNumMaterials(); ++I)
					if (UMaterialInstanceDynamic* Own = Cast<UMaterialInstanceDynamic>(Pair.Far->GetMaterial(I)); Own && Own->GetOuter() == Pair.Far)
					{
						Own->SetVectorParameterValue(TEXT("Door0"), FLinearColor::Transparent);
						Own->SetVectorParameterValue(TEXT("Door1"), FLinearColor::Transparent);
					}
			}
			for (const FPair& Pair : Pairs)
				for (AACEEnvCellActor* Room : Pair.Client)
					for (int32 I = 0; Room->CellMesh && I < Room->CellMesh->GetNumMaterials(); ++I)
						if (UMaterialInstanceDynamic* Own = Cast<UMaterialInstanceDynamic>(Room->CellMesh->GetMaterial(I)); Own && Own->GetOuter() == Room)
						{
							Own->SetVectorParameterValue(TEXT("Door0"), FLinearColor::Transparent);
							Own->SetVectorParameterValue(TEXT("Door1"), FLinearColor::Transparent);
						}
		}
		Sun->GetLightComponent()->SetVisibility(bDay);
		int64 RoomPixels = 0, Differing = 0, Pictures = 0;
		double Worst = 0., SumClient = 0., SumFar = 0.;
		for (const FPair& Pair : Pairs)
		{
			if (!Pair.Far) continue;
			for (int32 V = 0; V < Pair.Views.Num() && V < 6; ++V)
			{
				for (const double Yaw : {20., 140., 260.})
				{
					Capture->SetWorldLocationAndRotation(Pair.Views[V], FRotator(-10., Yaw + V * 15., 0.));
					TArray<FFloat16Color> Client, Far;
					Capture->ClearShowOnlyComponents();
					for (AACEEnvCellActor* Room : Pair.Client) Capture->ShowOnlyActorComponents(Room, false);
					Read(Client);
					Capture->ClearShowOnlyComponents();
					Capture->ShowOnlyComponent(Pair.Far);
					Read(Far);
					++Pictures;
					for (int32 I = 0; I < Client.Num() && I < Far.Num(); ++I)
					{
						const FLinearColor A(Client[I]), B(Far[I]);
						const double Diff = FMath::Max3(FMath::Abs(A.R - B.R), FMath::Abs(A.G - B.G), FMath::Abs(A.B - B.B));
						Worst = FMath::Max(Worst, Diff);
						Differing += Diff > 0.01 ? 1 : 0;
						RoomPixels += (A.R + A.G + A.B) > 0.001f ? 1 : 0;
						SumClient += A.R + A.G + A.B;
						SumFar += B.R + B.G + B.B;
					}
				}
			}
		}
		const FString Line = FString::Printf(TEXT("%s, %lld pictures from inside the rooms: %lld pixels show a room; %lld differ by more than 0.01 (%.2f%%), largest difference %.3f; mean light client %.4f, far %.4f"),
			*FString::Printf(TEXT("%s, client's doorway shading %s"), bDay ? TEXT("Day (sun on)") : TEXT("Night (no sun)"), bDoorShade ? TEXT("on") : TEXT("off")), Pictures, RoomPixels, Differing, RoomPixels > 0 ? Differing * 100. / RoomPixels : 0., Worst,
			RoomPixels > 0 ? SumClient / (3. * RoomPixels) : 0., RoomPixels > 0 ? SumFar / (3. * RoomPixels) : 0.);
		AddInfo(Line);
		TestTrue(TEXT("The pictures show rooms"), RoomPixels > 10000);
		if (!bDoorShade) TestTrue(FString::Printf(TEXT("%s: without the client's doorway shading, under 1%% of the room pixels differ between the client's rooms and the far rooms"), bDay ? TEXT("Day") : TEXT("Night")), Differing * 100 < RoomPixels);
		if (FarDoorShade) TestTrue(FString::Printf(TEXT("%s: with doorway shading on both, under 1%% of the room pixels differ"), bDay ? TEXT("Day") : TEXT("Night")), !bDoorShade || Differing * 100 < RoomPixels);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarWindowsRoomsRulesTest, "ACE.Personal.FarWindows.RoomsRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarWindowsRoomsRulesTest::RunTest(const FString& Parameters)
{
	using PersonalFarWindows::EKind;
	// Which openings show rooms.
	TestTrue(TEXT("A window shows its rooms"), PersonalFarRooms::GetsRooms(EKind::Window));
	TestTrue(TEXT("An open entrance shows its rooms"), PersonalFarRooms::GetsRooms(EKind::Entrance));
	TestTrue(TEXT("An opening over 12 m2 shows its rooms"), PersonalFarRooms::GetsRooms(EKind::None));
	TestFalse(TEXT("A door keeps its panel, no rooms"), PersonalFarRooms::GetsRooms(EKind::Door));
	TestTrue(TEXT("A floor or roof opening shows its rooms"), PersonalFarRooms::GetsRooms(EKind::Flat));

	// Which rooms an opening gets: the room behind it and its stab list, once each, as full ids.
	TArray<uint32> Rooms;
	const uint16 Stab[] = {0x0101, 0x0102, 0x0100, 0xFFFF, 0x0040};
	PersonalFarRooms::AddRooms(0xA9B40000u, 0x0100, Stab, Rooms);
	TestEqual(TEXT("The room behind and two more from the list"), Rooms.Num(), 3);
	TestTrue(TEXT("Full ids of the landblock"), Rooms.Contains(0xA9B40100u) && Rooms.Contains(0xA9B40101u) && Rooms.Contains(0xA9B40102u));
	PersonalFarRooms::AddRooms(0xA9B4FFFFu, 0x0101, Stab, Rooms);
	TestEqual(TEXT("A second opening to the same rooms adds none"), Rooms.Num(), 3);
	PersonalFarRooms::AddRooms(0xA9B40000u, 0xFFFF, Stab, Rooms);
	PersonalFarRooms::AddRooms(0xA9B40000u, 0x0020, Stab, Rooms);
	TestEqual(TEXT("An opening to the outside adds none"), Rooms.Num(), 3);

	// Where far rooms are made.
	const FIntPoint Center(0xA9, 0xB4);
	TestTrue(TEXT("The player's landblock"), PersonalFarRooms::IsWanted(Center, Center, 4));
	TestTrue(TEXT("Ring 4 of 4"), PersonalFarRooms::IsWanted(Center + FIntPoint(4, -2), Center, 4));
	TestFalse(TEXT("Ring 5 of 4"), PersonalFarRooms::IsWanted(Center + FIntPoint(5, 0), Center, 4));
	TestFalse(TEXT("Never past the last ring of far scenery"), PersonalFarRooms::IsWanted(Center + FIntPoint(PersonalFarScenery::LastRing + 1, 0), Center, 99));
	TestFalse(TEXT("Not off the map"), PersonalFarRooms::IsWanted(FIntPoint(-1, 3), FIntPoint(0, 3), 4));

	// The show rule.
	TestTrue(TEXT("Past the client's rooms: shown"), PersonalFarRooms::ShowRooms(2, false, false));
	TestTrue(TEXT("Far away: shown"), PersonalFarRooms::ShowRooms(8, false, false));
	TestFalse(TEXT("Within the client's rooms, all drawn: hidden"), PersonalFarRooms::ShowRooms(1, false, false));
	TestTrue(TEXT("Within the client's rooms, one not drawn yet: shown"), PersonalFarRooms::ShowRooms(1, true, false));
	TestTrue(TEXT("The player's landblock, one not drawn: shown"), PersonalFarRooms::ShowRooms(0, true, false));
	TestFalse(TEXT("The building the player is in: hidden"), PersonalFarRooms::ShowRooms(0, true, true));

	// Day fog.
	TestEqual(TEXT("Night: no fog far away"), PersonalFarRooms::RoomFog(1.f, 2000.f, 200.f, 600.f), 0.f);
	TestEqual(TEXT("Day, 150 m: no fog"), PersonalFarRooms::RoomFog(0.f, 150.f, 200.f, 600.f), 0.f);
	TestEqual(TEXT("Day, 200 m: no fog"), PersonalFarRooms::RoomFog(0.f, 200.f, 200.f, 600.f), 0.f);
	TestEqual(TEXT("Day, 400 m: half"), PersonalFarRooms::RoomFog(0.f, 400.f, 200.f, 600.f), 0.5f, 0.001f);
	TestEqual(TEXT("Day, 600 m and on: all"), PersonalFarRooms::RoomFog(0.f, 900.f, 200.f, 600.f), 1.f);
	TestEqual(TEXT("Dusk, half night, 900 m: half"), PersonalFarRooms::RoomFog(0.5f, 900.f, 200.f, 600.f), 0.5f, 0.001f);
	TestTrue(TEXT("It rises smoothly: slow at the start"), PersonalFarRooms::RoomFog(0.f, 240.f, 200.f, 600.f) < 0.05f);
	TestEqual(TEXT("Steps: 0"), PersonalFarRooms::FogStep(0.f), 0);
	TestEqual(TEXT("Steps: all"), PersonalFarRooms::FogStep(1.f), FMath::RoundToInt32(1.f / PersonalFarRooms::FogStepSize));
	TestEqual(TEXT("A change under half a step is no change"), PersonalFarRooms::FogStep(0.5f), PersonalFarRooms::FogStep(0.5f + PersonalFarRooms::FogStepSize * 0.4f));

	// The sharing key.
	TestEqual(TEXT("Same values, same key"), PersonalFarRooms::MixKey(PersonalFarRooms::MixKey(0, 7), 9), PersonalFarRooms::MixKey(PersonalFarRooms::MixKey(0, 7), 9));
	TestNotEqual(TEXT("Another order, another key"), PersonalFarRooms::MixKey(PersonalFarRooms::MixKey(0, 7), 9), PersonalFarRooms::MixKey(PersonalFarRooms::MixKey(0, 9), 7));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFarWindowsRoomsDrawTest, "ACE.Personal.FarWindows.RoomsDraw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFarWindowsRoomsDrawTest::RunTest(const FString& Parameters)
{
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (!FPaths::DirectoryExists(DatDirectory))
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
	UPersonalFarWindowsSubsystem* Windows = World->GetSubsystem<UPersonalFarWindowsSubsystem>();
	if (!TestNotNull(TEXT("The DAT subsystem exists"), Dat) || !TestNotNull(TEXT("The far windows subsystem exists in a game world"), Windows)
		|| !TestTrue(TEXT("DAT files load"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}
	ON_SCOPE_EXIT { Windows->ClearAll(); Windows->ClearRooms(); Windows->Meshes.Reset(); };
	constexpr float WorldScale = 100.f;
	// As the timer: out to where far scenery draws buildings (radius 6, no fog rule, by default).
	const int32 Rings = PersonalFarWindows::OuterRing(Dat->GetWorldFogEndCm(), WorldScale);

	// The places below are checked against the client's own openings: no lift for them.
	IConsoleVariable* Lift = IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarWindows.RoomLift"));
	if (!TestNotNull(TEXT("RoomLift console variable"), Lift))
	{
		return false;
	}
	TestEqual(TEXT("RoomLift is 3 cm by default"), PersonalFarRooms::RoomLiftCm(), 3.f);
	Lift->Set(0.f, ECVF_SetByCode);
	ON_SCOPE_EXIT { Lift->Set(3.f, ECVF_SetByCode); };

	// Off (the default): the timer's update makes nothing.
	TestFalse(TEXT("Far rooms are off by default"), PersonalFarRooms::IsOn());
	Windows->Update();
	TestEqual(TEXT("Switched off, no far rooms are made"), Windows->RoomBlocks.Num(), 0);

	// Holtburg: every landblock out to where far scenery draws buildings.
	const FIntPoint Center(0xA9, 0xB4);
	const double Start = FPlatformTime::Seconds();
	int32 Updates = 0;
	double Longest = 0.;
	while (!Windows->bRoomsLogged && FPlatformTime::Seconds() - Start < 180.)
	{
		++GFrameCounter;
		++Updates;
		const double Before = FPlatformTime::Seconds();
		Windows->StreamRooms(*Dat, Center, WorldScale, Rings);
		Longest = FMath::Max(Longest, FPlatformTime::Seconds() - Before);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.001f);
	}
	TestTrue(TEXT("Every landblock gets its far rooms"), Windows->bRoomsLogged);
	const UPersonalFarWindowsSubsystem::FRoomStats Stats = Windows->GetRoomStats();
	TestTrue(TEXT("Holtburg has far rooms"), Stats.Buildings >= 12 && Stats.Rooms >= Stats.Buildings && Stats.Triangles > 1000);
	TestEqual(TEXT("One component per building"), Stats.Components, Stats.Buildings);
	TestTrue(TEXT("Buildings with the same rooms share a mesh"), Stats.Meshes > 0 && Stats.Meshes <= Stats.Buildings);
	TestEqual(TEXT("Nothing is shown before the per-frame check says so"), Stats.Shown, 0);
	int32 Blocks = 0, Ring = 0;
	for (const auto& Pair : Windows->RoomBlocks)
	{
		Blocks += Pair.Value.Buildings.Num() > 0 ? 1 : 0;
		Ring = FMath::Max(Ring, PersonalFarScenery::Ring(Pair.Key, Center));
		TestEqual(TEXT("Every landblock is done"), Pair.Value.State, 3);
	}
	AddInfo(FString::Printf(TEXT("Holtburg, rings 0 to %d: %d buildings in %d landblocks, %d rooms, %d triangles, %d components, %d meshes; %d updates in %.1f s, longest %.2f ms"),
		Ring, Stats.Buildings, Blocks, Stats.Rooms, Stats.Triangles, Stats.Components, Stats.Meshes, Updates, FPlatformTime::Seconds() - Start, Longest * 1000.));

	// Each component, and its place against the client's own list of openings: an opening to one of the
	// building's entry rooms lies on that room's wall, so inside the far rooms' box.
	int32 Wrong = 0, Checked = 0, Outside = 0;
	double Furthest = 0.;
	for (const auto& Pair : Windows->RoomBlocks)
	{
		const uint32 Key = (uint32(Pair.Key.X) << 24) | (uint32(Pair.Key.Y) << 16);
		const auto Apertures = Dat->GetBuildingDoorwayApertures(Key, WorldScale);
		for (const auto& Building : Pair.Value.Buildings)
		{
			const UStaticMeshComponent* Component = Building.Component.Get();
			const FLightingChannels Channels = Component ? Component->LightingChannels : FLightingChannels();
			const bool bGood = Component && Component->GetStaticMesh() && Component->GetStaticMesh()->GetNumTriangles(0) == Building.Triangles
				&& Component->GetMaterial(0) && !Component->IsVisible() && !Component->IsCollisionEnabled() && !Component->CastShadow
				&& !Channels.bChannel0 && Channels.bChannel1 && Component->ComponentHasTag(TEXT("PersonalFarRooms")) && Building.Entries.Num() > 0;
			Wrong += bGood ? 0 : 1;
			if (Component)
			{
				const auto& Data = Component->GetCustomPrimitiveData().Data;
				const int32 Index = PersonalFarScenery::SizeFadePrimitiveDataIndex;
				TestTrue(TEXT("Room size-fade opt-out is initialized before its first draw"), Data.IsValidIndex(Index) && Data[Index] == 0.f);
			}
			if (!Component || !Apertures)
			{
				continue;
			}
			const FBox Box = Component->Bounds.GetBox().ExpandBy(1.);
			for (const auto& Aperture : *Apertures)
			{
				if (!Building.Entries.Contains(int32(Aperture.DestEnvCellId)))
				{
					continue;
				}
				++Checked;
				for (const FVector& V : Aperture.WorldVerts)
				{
					const FVector Clamped = Box.GetClosestPointTo(V);
					Furthest = FMath::Max(Furthest, FVector::Dist(Clamped, V));
				}
				bool bIn = true;
				for (const FVector& V : Aperture.WorldVerts) bIn = bIn && Box.IsInsideOrOn(V);
				Outside += bIn ? 0 : 1;
			}
		}
	}
	TestEqual(TEXT("Every component has its mesh and materials, is hidden, has no collision and no shadow, and is lit as the client's rooms"), Wrong, 0);
	TestTrue(TEXT("The client lists openings to the far rooms"), Checked > 10);
	TestEqual(TEXT("The client's openings lie on the far rooms, within 1 cm"), Outside, 0);
	AddInfo(FString::Printf(TEXT("%d openings of the client's list checked against the far rooms; furthest outside %.3f cm"), Checked, Furthest));

	// Day fog, with the camera over the middle of Holtburg's landblock.
	{
		const FVector Camera = FACEPosition::AceVectorToUnreal(FVector(Center.X * 192. + 96., Center.Y * 192. + 96., 20.), WorldScale);
		auto Check = [&](float Night, int32& OutNear, int32& OutFar, int32& OutWrong)
		{
			Windows->UpdateRoomFog(Camera, Night, WorldScale);
			OutNear = OutFar = OutWrong = 0;
			for (const auto& Pair : Windows->RoomBlocks)
			{
				for (const auto& Building : Pair.Value.Buildings)
				{
					const UStaticMeshComponent* Component = Building.Component.Get();
					if (!Component) continue;
					const float Meters = float(FVector::Dist(Component->Bounds.Origin, Camera) / WorldScale);
					const bool bNear = Meters < PersonalFarRooms::RoomFogNear(), bFar = Meters > PersonalFarRooms::RoomFogFar();
					OutNear += bNear ? 1 : 0;
					OutFar += bFar ? 1 : 0;
					const float Want = (Night >= 1.f || bNear) ? 0.f : (bFar ? 1.f : -1.f);
					if (Want < 0.f) continue;
					for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
					{
						float Fog = -1.f;
						const UMaterialInterface* Material = Component->GetMaterial(Slot);
						const bool bHas = Material && Material->GetScalarParameterValue(TEXT("FogAmount"), Fog);
						OutWrong += (bHas && FMath::IsNearlyEqual(Fog, Want, 0.001f)) ? 0 : 1;
						// No fog: the client's own instance (or the doorway-shaded copy) is drawn, or an own copy set to 0.
					}
					OutWrong += (Building.FogStep == PersonalFarRooms::FogStep(Want)) ? 0 : 1;
				}
			}
		};
		int32 Near = 0, Far = 0, FogWrong = 0;
		Check(0.f, Near, Far, FogWrong);
		TestTrue(TEXT("By day there are far rooms near and far"), Near > 0 && Far > 0);
		TestEqual(TEXT("By day: no fog within 200 m, all of it past 600 m"), FogWrong, 0);
		const int32 Instances = Windows->GetRoomStats().FogInstances;
		TestTrue(TEXT("By day the far buildings have own instances for the fog"), Instances > 0);
		Check(1.f, Near, Far, FogWrong);
		TestEqual(TEXT("At night: no fog at any distance"), FogWrong, 0);
		AddInfo(FString::Printf(TEXT("Day fog: %d buildings within 200 m, %d past 600 m; %d own material instances"), Near, Far, Instances));
		// The day fog is always on: there is no switch for it any more.
		TestNull(TEXT("No switch for the day fog"), IConsoleManager::Get().FindConsoleVariable(TEXT("personal.FarWindows.RoomFog")));
	}

	// The per-frame check without a player or presenter shows nothing.
	Windows->ApplyRoomsShown();
	TestEqual(TEXT("No player, nothing shown"), Windows->GetRoomStats().Shown, 0);

	// Far rooms are drawn as far as far scenery draws their building.
	{
		int32 Culled = 0, Never = 0;
		for (const auto& Pair : Windows->RoomBlocks)
		{
			for (const auto& Building : Pair.Value.Buildings)
			{
				if (const UStaticMeshComponent* Component = Building.Component.Get())
				{
					Never += Component->bNeverDistanceCull ? 1 : 0;
					Culled += (!Component->bNeverDistanceCull && Component->LDMaxDrawDistance > 0.f) ? 1 : 0;
				}
			}
		}
		TestEqual(TEXT("Every far-room component has the cull distance of its building, or none past the last ring"), Culled + Never, Stats.Components);
		AddInfo(FString::Printf(TEXT("Cull distance: %d with one, %d never culled"), Culled, Never));
	}

	// The lift, changed live: every far-room component moves up by it, and back.
	{
		auto Offsets = [&](double& OutMin, double& OutMax)
		{
			OutMin = MAX_dbl;
			OutMax = -MAX_dbl;
			for (const auto& Pair : Windows->RoomBlocks)
			{
				for (const auto& Building : Pair.Value.Buildings)
				{
					if (const UStaticMeshComponent* Component = Building.Component.Get())
					{
						const double Z = Component->GetRelativeLocation().Z - Building.Frame.GetLocation().Z;
						OutMin = FMath::Min(OutMin, Z);
						OutMax = FMath::Max(OutMax, Z);
					}
				}
			}
		};
		double Low = 0., High = 0.;
		Windows->UpdateRoomLift(WorldScale);
		Offsets(Low, High);
		TestTrue(TEXT("Lift 0: every far room where the client's room is"), FMath::Abs(Low) < 0.001 && FMath::Abs(High) < 0.001);
		Lift->Set(10.f, ECVF_SetByCode);
		Windows->UpdateRoomLift(WorldScale);
		Offsets(Low, High);
		TestTrue(TEXT("Lift 10: every far room 10 cm higher, without a rebuild"), FMath::IsNearlyEqual(Low, 10. * WorldScale / 100., 0.001) && FMath::IsNearlyEqual(High, 10. * WorldScale / 100., 0.001));
		TestEqual(TEXT("Lift 10: the same components"), Windows->GetRoomStats().Components, Stats.Components);
		Lift->Set(0.f, ECVF_SetByCode);
		Windows->UpdateRoomLift(WorldScale);
		Offsets(Low, High);
		TestTrue(TEXT("Lift back to 0"), FMath::Abs(Low) < 0.001 && FMath::Abs(High) < 0.001);
	}

	// Moving on: landblocks out of reach lose their far rooms, and their meshes go when no building draws them.
	const FIntPoint Away = Center + FIntPoint(2 * PersonalFarScenery::LastRing + 4, 0);
	const int32 MeshesBefore = Windows->RoomMeshes.Num();
	Windows->StreamRooms(*Dat, Away, WorldScale, Rings);
	for (const auto& Pair : Windows->RoomBlocks)
	{
		TestTrue(TEXT("Only landblocks within reach are kept"), PersonalFarScenery::Ring(Pair.Key, Away) <= PersonalFarScenery::LastRing + 1);
	}
	{
		TSet<uint32> Used;
		for (const auto& Pair : Windows->RoomBlocks) for (const auto& Building : Pair.Value.Buildings) Used.Add(Building.MeshKey);
		int32 Unused = 0;
		for (const auto& Pair : Windows->RoomMeshes) Unused += Used.Contains(Pair.Key) ? 0 : 1;
		TestEqual(TEXT("No mesh is kept that no building draws"), Unused, 0);
		TestTrue(TEXT("The meshes of the old landblocks are gone"), Windows->RoomMeshes.Num() < MeshesBefore);
		TestTrue(TEXT("Only the materials of the meshes left are kept"), Windows->RoomMeshes.Num() > 0 || Windows->RoomMaterials.Num() == 0);
		AddInfo(FString::Printf(TEXT("Moved away: %d of %d meshes left, %d materials kept"), Windows->RoomMeshes.Num(), MeshesBefore, Windows->RoomMaterials.Num()));
	}
	Windows->ClearRooms();
	const UPersonalFarWindowsSubsystem::FRoomStats After = Windows->GetRoomStats();
	TestEqual(TEXT("Nothing is left after a clear"), After.Buildings + After.Meshes, 0);

	// Benchmark: the counts at Shoushi (and Holtburg above), at the default radius.
	{
		const FIntPoint Shoushi(0xDA, 0x55);
		const double ShoushiStart = FPlatformTime::Seconds();
		int32 ShoushiUpdates = 0;
		double ShoushiLongest = 0.;
		while (!Windows->bRoomsLogged && FPlatformTime::Seconds() - ShoushiStart < 180.)
		{
			++GFrameCounter;
			++ShoushiUpdates;
			const double Before = FPlatformTime::Seconds();
			Windows->StreamRooms(*Dat, Shoushi, WorldScale, Rings);
			ShoushiLongest = FMath::Max(ShoushiLongest, FPlatformTime::Seconds() - Before);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.001f);
		}
		const UPersonalFarWindowsSubsystem::FRoomStats Far = Windows->GetRoomStats();
		TestTrue(TEXT("Shoushi has far rooms"), Windows->bRoomsLogged && Far.Buildings > 10);
		AddInfo(FString::Printf(TEXT("Shoushi, rings 0 to %d: %d buildings, %d rooms, %d triangles, %d components, %d meshes, %d materials; %d updates in %.1f s, longest %.2f ms"),
			Rings, Far.Buildings, Far.Rooms, Far.Triangles, Far.Components, Far.Meshes, Windows->RoomMaterials.Num(), ShoushiUpdates, FPlatformTime::Seconds() - ShoushiStart, ShoushiLongest * 1000.));
		Windows->ClearRooms();
	}
	return !HasAnyErrors();
}
#endif
