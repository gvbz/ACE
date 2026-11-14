#include "Personal/PersonalSkyHorizon.h"
#include "Personal/PersonalFarTerrain.h"
#include "ACERuntimeOptions.h"
#include "ACESkyDomeActor.h"
#include "Algo/Sort.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** A rim edge, in the order its face lists it, and that face's third corner. */
	struct FRimEdge
	{
		int32 From = 0;
		int32 To = 0;
		int32 Third = 0;
	};

	FIntPoint EdgeKey(int32 A, int32 B)
	{
		return FIntPoint(FMath::Min(A, B), FMath::Max(A, B));
	}
}

int32 PersonalSkyHorizon::ExtendRim(TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector>& Normals,
	TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors)
{
	const int32 NumVerts = Vertices.Num();
	const int32 NumIndices = Triangles.Num() - Triangles.Num() % 3;
	if (NumVerts < 3 || NumIndices < 3)
	{
		return 0;
	}
	double Extent = 0.0;
	for (const FVector& P : Vertices)
	{
		Extent = FMath::Max(Extent, P.GetAbsMax());
	}
	for (int32 I = 0; I < NumIndices; ++I)
	{
		if (!Vertices.IsValidIndex(Triangles[I]))
		{
			return 0;
		}
	}
	const double Tolerance = Extent * 1.e-4;
	if (Tolerance <= 0.0)
	{
		return 0;
	}

	// The mesh builder gives every polygon its own vertices, so corners are matched by position.
	TArray<int32> Corner;
	Corner.SetNumUninitialized(NumVerts);
	for (int32 I = 0; I < NumVerts; ++I)
	{
		Corner[I] = I;
		for (int32 J = 0; J < I; ++J)
		{
			if (Vertices[J].Equals(Vertices[I], Tolerance))
			{
				Corner[I] = Corner[J];
				break;
			}
		}
	}

	// Faces per edge. A back face (the same corners in the other order) is the same face.
	TSet<FIntVector> Faces;
	TMap<FIntPoint, int32> FacesAtEdge;
	for (int32 T = 0; T < NumIndices; T += 3)
	{
		int32 C[3] = { Corner[Triangles[T]], Corner[Triangles[T + 1]], Corner[Triangles[T + 2]] };
		if (C[0] == C[1] || C[1] == C[2] || C[0] == C[2])
		{
			continue;
		}
		const int32 E[3] = { C[0], C[1], C[2] };
		Algo::Sort(C);
		bool bSeen = false;
		Faces.Add(FIntVector(C[0], C[1], C[2]), &bSeen);
		if (bSeen)
		{
			continue;
		}
		for (int32 S = 0; S < 3; ++S)
		{
			++FacesAtEdge.FindOrAdd(EdgeKey(E[S], E[(S + 1) % 3]));
		}
	}

	TArray<FRimEdge> Rim;
	double HalfWidth = 0.0;
	for (int32 T = 0; T < NumIndices; T += 3)
	{
		for (int32 S = 0; S < 3; ++S)
		{
			const FRimEdge Edge{ Triangles[T + S], Triangles[T + (S + 1) % 3], Triangles[T + (S + 2) % 3] };
			const FVector& A = Vertices[Edge.From];
			const FVector& B = Vertices[Edge.To];
			if (FMath::Abs(A.Z) > Tolerance || FMath::Abs(B.Z) > Tolerance || Vertices[Edge.Third].Z <= Tolerance)
			{
				continue;
			}
			if (Corner[Edge.From] == Corner[Edge.To] || FacesAtEdge.FindRef(EdgeKey(Corner[Edge.From], Corner[Edge.To])) != 1)
			{
				continue;
			}
			Rim.Add(Edge);
			HalfWidth = FMath::Max(HalfWidth, FMath::Max(FMath::Max(FMath::Abs(A.X), FMath::Abs(A.Y)),
				FMath::Max(FMath::Abs(B.X), FMath::Abs(B.Y))));
		}
	}
	if (Rim.Num() == 0 || HalfWidth <= Tolerance)
	{
		return 0;
	}

	const bool bNormals = Normals.Num() == NumVerts;
	const bool bUVs = UVs.Num() == NumVerts;
	const bool bColors = Colors.Num() == NumVerts;
	const FVector Drop(0.0, 0.0, -HalfWidth);
	for (const FRimEdge& Edge : Rim)
	{
		const FVector A = Vertices[Edge.From];
		const FVector B = Vertices[Edge.To];
		const int32 Base = Vertices.Num();
		Vertices.Add(A + Drop);
		Vertices.Add(B + Drop);
		if (bNormals)
		{
			const FVector NormalA = Normals[Edge.From];
			const FVector NormalB = Normals[Edge.To];
			Normals.Add(NormalA);
			Normals.Add(NormalB);
		}
		if (bUVs)
		{
			// The face above maps positions to texture coordinates. Take its change per
			// unit of height, so the texture keeps its size and direction below the rim.
			const FVector Along = B - A;
			const FVector Up = Vertices[Edge.Third] - A;
			const FVector2D UvA = UVs[Edge.From];
			const FVector2D UvB = UVs[Edge.To];
			const double AlongShare = (Up | Along) / Along.SizeSquared();
			const FVector2D PerHeight = (UVs[Edge.Third] - UvA - (UvB - UvA) * AlongShare) / Up.Z;
			UVs.Add(UvA - PerHeight * HalfWidth);
			UVs.Add(UvB - PerHeight * HalfWidth);
		}
		if (bColors)
		{
			const FLinearColor ColorA = Colors[Edge.From];
			const FLinearColor ColorB = Colors[Edge.To];
			Colors.Add(ColorA);
			Colors.Add(ColorB);
		}
		// The face above runs From -> To along the rim; the quad below runs To -> From.
		Triangles.Append({ Edge.To, Edge.From, Base, Edge.To, Base, Base + 1 });
	}
	return Rim.Num() * 2;
}

void PersonalSkyHorizon::ExtendSection(uint32 GfxId, TArray<FVector>& Vertices, TArray<int32>& Triangles,
	TArray<FVector>& Normals, TArray<FVector2D>& UVs, TArray<FLinearColor>& Colors)
{
	// Without the far terrain nothing covers the lower sky, so the sky stays as authored.
	if (GfxId != StarsGfxId || ACERuntimeOptions::Get(OptionKey) <= 0.5f || !PersonalFarTerrain::IsOn())
	{
		return;
	}
	const int32 Added = ExtendRim(Vertices, Triangles, Normals, UVs, Colors);
	UE_LOG(LogTemp, Log, TEXT("PersonalSkyHorizon: stars layer extended below eye level by %d triangles"), Added);
}

void PersonalSkyHorizon::RebuildSky()
{
	for (TObjectIterator<AACESkyDomeActor> It; It; ++It)
	{
		const UWorld* World = IsValid(*It) ? It->GetWorld() : nullptr;
		if (World && World->IsGameWorld())
		{
			It->InvalidateAndRebuild();
		}
	}
}
