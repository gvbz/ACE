#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalSkyHorizon.h"
#include "ACEDatSubsystem.h"
#include "ACERuntimeOptions.h"
#include "Dat/ACESetupMeshBuilder.h"
#include "Engine/GameInstance.h"
#include "Misc/Paths.h"

namespace
{
	/** One mesh section, with every polygon on its own vertices, the way the client builds them. */
	struct FSkyHorizonMesh
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FLinearColor> Colors;

		/** A quad as a fan. UVs: U runs A to B, V is 1 at A and B and 0 at C and D. */
		void AddQuad(const FVector& A, const FVector& B, const FVector& C, const FVector& D, bool bBack = false)
		{
			const int32 Base = Vertices.Num();
			const FVector Normal = ((B - A) ^ (D - A)).GetSafeNormal() * (bBack ? -1.0 : 1.0);
			const FVector Corners[4] = { A, B, C, D };
			const FVector2D CornerUVs[4] = { FVector2D(0, 1), FVector2D(1, 1), FVector2D(1, 0), FVector2D(0, 0) };
			for (int32 I = 0; I < 4; ++I)
			{
				Vertices.Add(Corners[I]);
				Normals.Add(Normal);
				UVs.Add(CornerUVs[I]);
				Colors.Add(FLinearColor(0.1f * (Base + I), 0.5f, 0.25f, 1.f));
			}
			for (int32 I = 1; I < 3; ++I)
			{
				Triangles.Append({ Base, Base + (bBack ? I + 1 : I), Base + (bBack ? I : I + 1) });
			}
		}

		/** The upper half of a cube: four walls from Bottom up by Half, and a top. No floor. */
		void AddHalfCube(double Half, double Bottom, bool bBack = false)
		{
			const double Top = Bottom + Half;
			const FVector2D Ring[4] = { FVector2D(-Half, -Half), FVector2D(Half, -Half), FVector2D(Half, Half), FVector2D(-Half, Half) };
			for (int32 I = 0; I < 4; ++I)
			{
				const FVector2D P = Ring[I];
				const FVector2D Q = Ring[(I + 1) % 4];
				AddQuad(FVector(P.X, P.Y, Bottom), FVector(Q.X, Q.Y, Bottom), FVector(Q.X, Q.Y, Top), FVector(P.X, P.Y, Top), bBack);
			}
			AddQuad(FVector(-Half, -Half, Top), FVector(-Half, Half, Top), FVector(Half, Half, Top), FVector(Half, -Half, Top), bBack);
		}

		int32 Extend()
		{
			return PersonalSkyHorizon::ExtendRim(Vertices, Triangles, Normals, UVs, Colors);
		}

		bool SameLengths() const
		{
			return Normals.Num() == Vertices.Num() && UVs.Num() == Vertices.Num() && Colors.Num() == Vertices.Num()
				&& Triangles.Num() % 3 == 0;
		}

		/** Starts with the same vertices and triangles as Before. */
		bool StartsWith(const FSkyHorizonMesh& Before) const
		{
			if (Vertices.Num() < Before.Vertices.Num() || Triangles.Num() < Before.Triangles.Num())
			{
				return false;
			}
			for (int32 I = 0; I < Before.Vertices.Num(); ++I)
			{
				if (Vertices[I] != Before.Vertices[I] || Normals[I] != Before.Normals[I] || UVs[I] != Before.UVs[I]
					|| Colors[I] != Before.Colors[I])
				{
					return false;
				}
			}
			for (int32 I = 0; I < Before.Triangles.Num(); ++I)
			{
				if (Triangles[I] != Before.Triangles[I])
				{
					return false;
				}
			}
			return true;
		}

		double LowestZ() const
		{
			double Lowest = TNumericLimits<double>::Max();
			for (const FVector& P : Vertices)
			{
				Lowest = FMath::Min(Lowest, P.Z);
			}
			return Lowest;
		}

		/** +1 when the triangle faces the vertical axis through the origin, -1 when it faces away. */
		int32 FacesInward(int32 FirstIndex) const
		{
			const FVector& A = Vertices[Triangles[FirstIndex]];
			const FVector& B = Vertices[Triangles[FirstIndex + 1]];
			const FVector& C = Vertices[Triangles[FirstIndex + 2]];
			const FVector Normal = (B - A) ^ (C - A);
			const FVector Center = (A + B + C) / 3.0;
			const double Side = Normal.X * -Center.X + Normal.Y * -Center.Y;
			return Side > 0.0 ? 1 : (Side < 0.0 ? -1 : 0);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalSkyHorizonTest, "ACE.Personal.SkyHorizon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalSkyHorizonTest::RunTest(const FString& Parameters)
{
	constexpr double Half = 500.0;

	// A half cube with an open bottom: one quad under each of the four rim edges.
	{
		FSkyHorizonMesh Mesh;
		Mesh.AddHalfCube(Half, 0.0);
		const FSkyHorizonMesh Before = Mesh;
		TestEqual(TEXT("Two triangles per rim edge"), Mesh.Extend(), 8);
		TestEqual(TEXT("Two vertices per rim edge"), Mesh.Vertices.Num(), Before.Vertices.Num() + 8);
		TestEqual(TEXT("Six indices per rim edge"), Mesh.Triangles.Num(), Before.Triangles.Num() + 24);
		TestTrue(TEXT("Array lengths stay equal"), Mesh.SameLengths());
		TestTrue(TEXT("Original vertices and triangles unchanged and first"), Mesh.StartsWith(Before));
		TestEqual(TEXT("Nothing lower than the half-width below the rim"), Mesh.LowestZ(), -Half);

		const int32 WallFacing = Mesh.FacesInward(0);
		TestTrue(TEXT("Walls face one way"), WallFacing != 0);
		for (int32 T = Before.Triangles.Num(); T < Mesh.Triangles.Num(); T += 3)
		{
			TestEqual(TEXT("Added triangle faces the same way as the wall"), Mesh.FacesInward(T), WallFacing);
		}
		// Each added quad is To, From, below From, To, below From, below To.
		for (int32 T = Before.Triangles.Num(); T + 5 < Mesh.Triangles.Num(); T += 6)
		{
			const int32 Rim[2] = { Mesh.Triangles[T + 1], Mesh.Triangles[T] };
			const int32 Below[2] = { Mesh.Triangles[T + 2], Mesh.Triangles[T + 5] };
			for (int32 S = 0; S < 2; ++S)
			{
				TestTrue(TEXT("Quad starts at the rim"), Rim[S] < Before.Vertices.Num() && Mesh.Vertices[Rim[S]].Z == 0.0);
				TestTrue(TEXT("Quad ends at an added vertex"), Below[S] >= Before.Vertices.Num());
				TestTrue(TEXT("Added vertex is the half-width straight below the rim"),
					Mesh.Vertices[Below[S]] == Mesh.Vertices[Rim[S]] - FVector(0.0, 0.0, Half));
				TestTrue(TEXT("Normal copied from the rim"), Mesh.Normals[Below[S]] == Mesh.Normals[Rim[S]]);
				TestTrue(TEXT("Color copied from the rim"), Mesh.Colors[Below[S]] == Mesh.Colors[Rim[S]]);
				// The wall maps its height to V 1..0, so the same height below the rim is V 2.
				TestTrue(TEXT("Texture continues at the same scale"),
					Mesh.UVs[Below[S]].Equals(Mesh.UVs[Rim[S]] + FVector2D(0.0, 1.0), 1.e-6));
			}
		}
	}

	// Polygons with a back face get the quad on both faces.
	{
		FSkyHorizonMesh Mesh;
		Mesh.AddHalfCube(Half, 0.0);
		Mesh.AddHalfCube(Half, 0.0, /*bBack*/ true);
		const FSkyHorizonMesh Before = Mesh;
		TestEqual(TEXT("Two triangles per rim edge and face"), Mesh.Extend(), 16);
		TestTrue(TEXT("Two-sided: array lengths stay equal"), Mesh.SameLengths());
		TestTrue(TEXT("Two-sided: original unchanged and first"), Mesh.StartsWith(Before));
		int32 Inward = 0;
		int32 Outward = 0;
		for (int32 T = Before.Triangles.Num(); T < Mesh.Triangles.Num(); T += 3)
		{
			(Mesh.FacesInward(T) > 0 ? Inward : Outward) += 1;
		}
		TestEqual(TEXT("Two-sided: half of the added triangles face inward"), Inward, 8);
		TestEqual(TEXT("Two-sided: half of the added triangles face outward"), Outward, 8);
	}

	// A closed mesh has no rim.
	{
		FSkyHorizonMesh Mesh;
		Mesh.AddHalfCube(Half, 0.0);
		Mesh.AddQuad(FVector(-Half, -Half, 0), FVector(Half, -Half, 0), FVector(Half, Half, 0), FVector(-Half, Half, 0));
		const FSkyHorizonMesh Before = Mesh;
		TestEqual(TEXT("Closed mesh: nothing added"), Mesh.Extend(), 0);
		TestTrue(TEXT("Closed mesh: unchanged"), Mesh.Vertices.Num() == Before.Vertices.Num()
			&& Mesh.Triangles.Num() == Before.Triangles.Num() && Mesh.StartsWith(Before));
	}

	// An open edge that is not at height 0 is not a rim.
	{
		FSkyHorizonMesh Mesh;
		Mesh.AddHalfCube(Half, 10.0);
		const FSkyHorizonMesh Before = Mesh;
		TestEqual(TEXT("No edge at 0: nothing added"), Mesh.Extend(), 0);
		TestTrue(TEXT("No edge at 0: unchanged"), Mesh.Vertices.Num() == Before.Vertices.Num()
			&& Mesh.Triangles.Num() == Before.Triangles.Num() && Mesh.StartsWith(Before));
	}

	// A floor alone has open edges at height 0 but nothing above them.
	{
		FSkyHorizonMesh Mesh;
		Mesh.AddQuad(FVector(-Half, -Half, 0), FVector(Half, -Half, 0), FVector(Half, Half, 0), FVector(-Half, Half, 0));
		TestEqual(TEXT("Flat mesh: nothing added"), Mesh.Extend(), 0);
	}

	// Empty and broken input.
	{
		FSkyHorizonMesh Mesh;
		TestEqual(TEXT("Empty mesh: nothing added"), Mesh.Extend(), 0);
		Mesh.AddHalfCube(Half, 0.0);
		Mesh.Triangles[4] = 999;
		TestEqual(TEXT("Index out of range: nothing added"), Mesh.Extend(), 0);
	}

	// The other sky objects are left alone, and the option turns the stars layer off.
	{
		FSkyHorizonMesh Mesh;
		Mesh.AddHalfCube(Half, 0.0);
		const int32 Count = Mesh.Vertices.Num();
		const float Saved = ACERuntimeOptions::Get(PersonalSkyHorizon::OptionKey);
		ACERuntimeOptions::Set(PersonalSkyHorizon::OptionKey, 1.f);
		PersonalSkyHorizon::ExtendSection(0x010015F0u, Mesh.Vertices, Mesh.Triangles, Mesh.Normals, Mesh.UVs, Mesh.Colors);
		TestEqual(TEXT("Dome is not extended"), Mesh.Vertices.Num(), Count);
		ACERuntimeOptions::Set(PersonalSkyHorizon::OptionKey, 0.f);
		PersonalSkyHorizon::ExtendSection(PersonalSkyHorizon::StarsGfxId, Mesh.Vertices, Mesh.Triangles, Mesh.Normals, Mesh.UVs, Mesh.Colors);
		TestEqual(TEXT("Option off: stars are not extended"), Mesh.Vertices.Num(), Count);
		ACERuntimeOptions::Set(PersonalSkyHorizon::OptionKey, 1.f);
		PersonalSkyHorizon::ExtendSection(PersonalSkyHorizon::StarsGfxId, Mesh.Vertices, Mesh.Triangles, Mesh.Normals, Mesh.UVs, Mesh.Colors);
		TestEqual(TEXT("Option on: stars are extended"), Mesh.Vertices.Num(), Count + 8);
		ACERuntimeOptions::Set(PersonalSkyHorizon::OptionKey, Saved);
	}

	// Real DAT files: the stars layer the client builds.
	const FString DatDirectory = TEXT("C:/Turbine/Asheron's Call");
	if (!FPaths::FileExists(DatDirectory / TEXT("client_portal.dat")))
	{
		AddInfo(TEXT("DAT files not found; skipped the check of the real stars layer"));
		return !HasAnyErrors();
	}
	UGameInstance* GameInstance = NewObject<UGameInstance>();
	UACEDatSubsystem* Dat = NewObject<UACEDatSubsystem>(GameInstance);
	if (!TestTrue(TEXT("DAT files load"), Dat->LoadDatDirectory(DatDirectory)))
	{
		return false;
	}
	const auto Built = Dat->GetOrBuildSetupMeshShared(PersonalSkyHorizon::StarsGfxId, 250.f);
	if (!TestTrue(TEXT("Stars layer builds"), Built.IsValid() && !Built->IsEmpty()))
	{
		return false;
	}
	int32 Sections = 0;
	for (const FACEBuiltSetupPart& Part : Built->Parts)
	{
		for (const FACEBuiltMeshSection& Sec : Part.Sections)
		{
			if (Sec.IsEmpty() || Sec.bFullyTransparent || Sec.bCollisionOnly)
			{
				continue;
			}
			++Sections;
			FSkyHorizonMesh Mesh;
			Mesh.Vertices = Sec.Vertices;
			Mesh.Triangles = Sec.Triangles;
			Mesh.Normals = Sec.Normals;
			Mesh.UVs = Sec.UVs;
			Mesh.Colors = Sec.VertexColors;
			for (FVector& P : Mesh.Vertices)
			{
				P = Part.BindTransform.TransformPosition(P);
			}
			const FSkyHorizonMesh Before = Mesh;
			const int32 Added = Mesh.Extend();
			UE_LOG(LogTemp, Display, TEXT("PersonalSkyHorizon test: stars section surface 0x%08X, %d vertices, %d triangles, %d added"),
				Sec.SurfaceId, Before.Vertices.Num(), Before.Triangles.Num() / 3, Added);
			TestTrue(TEXT("Stars: triangles added"), Added > 0);
			TestTrue(TEXT("Stars: array lengths stay equal"), Mesh.SameLengths());
			TestTrue(TEXT("Stars: original unchanged and first"), Mesh.StartsWith(Before));
			TestTrue(TEXT("Stars: the rim is at eye level"), FMath::IsNearlyZero(Before.LowestZ(), 0.01));

			// The layer is half a cube: as far down as it was up, and no further.
			double Highest = 0.0;
			for (const FVector& P : Before.Vertices)
			{
				Highest = FMath::Max(Highest, P.Z);
			}
			TestEqual(TEXT("Stars: extended by the half-width"), Mesh.LowestZ(), -Highest, Highest * 1.e-4);

			// The polygons have a back face, so half of the added triangles face each way,
			// and every wall continues its texture downward by the same step.
			int32 Inward = 0;
			int32 Outward = 0;
			FVector2D Step = FVector2D::ZeroVector;
			bool bSameStep = true;
			for (int32 T = Before.Triangles.Num(); T + 5 < Mesh.Triangles.Num(); T += 6)
			{
				const int32 Facing = Mesh.FacesInward(T);
				TestTrue(TEXT("Stars: both triangles of a quad face the same way"), Facing != 0 && Facing == Mesh.FacesInward(T + 3));
				(Facing > 0 ? Inward : Outward) += 1;
				const FVector2D Steps[2] = { Mesh.UVs[Mesh.Triangles[T + 2]] - Mesh.UVs[Mesh.Triangles[T + 1]],
					Mesh.UVs[Mesh.Triangles[T + 5]] - Mesh.UVs[Mesh.Triangles[T]] };
				for (const FVector2D& S : Steps)
				{
					if (Step.IsZero())
					{
						Step = S;
					}
					bSameStep &= S.Equals(Step, 1.e-3);
				}
			}
			TestEqual(TEXT("Stars: as many added quads face inward as outward"), Inward, Outward);
			TestTrue(TEXT("Stars: every wall continues its texture by the same step"), bSameStep);
			TestTrue(TEXT("Stars: the texture continues downward, not sideways"),
				FMath::IsNearlyZero(Step.X, 1.e-3) && FMath::Abs(Step.Y) > 0.1);
			UE_LOG(LogTemp, Display, TEXT("PersonalSkyHorizon test: half-width %.0f, texture step below the rim %s"), Highest, *Step.ToString());
		}
	}
	TestEqual(TEXT("Stars layer is one section"), Sections, 1);

	// The rim stays at eye level only while the stars layer is not tilted in any weather.
	const FACEDatRegionSky* Sky = Dat->GetRegionSkyInfo();
	if (TestTrue(TEXT("Sky description loads"), Sky && Sky->bValid))
	{
		int32 Found = 0;
		for (const FACEDatSkyDayGroup& Group : Sky->DayGroups)
		{
			for (const FACEDatSkyObject& Object : Group.Objects)
			{
				if (Object.DefaultGfxObjectId != PersonalSkyHorizon::StarsGfxId)
				{
					continue;
				}
				++Found;
				TestTrue(*FString::Printf(TEXT("Stars are not tilted in '%s'"), *Group.DayName),
					Object.BeginAngle == 0.f && Object.EndAngle == 0.f);
			}
		}
		TestTrue(TEXT("Stars layer found in the sky description"), Found > 0);
		UE_LOG(LogTemp, Display, TEXT("PersonalSkyHorizon test: stars layer in %d of %d day groups"), Found, Sky->DayGroups.Num());
	}
	return !HasAnyErrors();
}
#endif
