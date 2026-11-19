#pragma once
#include "CoreMinimal.h"

namespace ACESkyMotion
{
// Measure only vertices referenced by the solid celestial card. Imported
// sections can share a vertex buffer containing a much larger glow halo.
struct FBodyShape
{
	FVector Center = FVector::ZeroVector;
	double TanHalfAngle = 0.;

	static FBodyShape Measure(const TArray<FVector>& Vertices, const TArray<int32>& Indices)
	{
		FBodyShape Result;
		FBox Bounds(ForceInit);
		for (int32 Index : Indices) Bounds += Vertices[Index];
		if (!Bounds.IsValid) return Result;
		Result.Center = Bounds.GetCenter();
		const FQuat View = Result.Center.Rotation().Quaternion();
		FBox2D Extent(ForceInit);
		for (int32 Index : Indices)
		{
			const FVector P = View.UnrotateVector(Vertices[Index]);
			if (P.X > UE_SMALL_NUMBER) Extent += FVector2D(P.Y,P.Z)/P.X;
		}
		// A square card and a bevelled disc have different corner radii even
		// when their visible diameters match. Compare projected width/height.
		if (Extent.bIsValid) Result.TanHalfAngle = Extent.GetSize().GetMax()*.5;
		return Result;
	}
};

// Reduce in double precision before uploading to float material parameters.
// Portal-year timestamps are too large to pass directly to a shader clock.
inline double Offset(double Seconds, double Velocity, double Initial = 0.)
{
	const double Value = FMath::Fmod(Initial + Seconds * Velocity, 1.);
	return Value < 0. ? Value + 1. : Value;
}

struct FUVBasis
{
	FVector U = FVector::ZeroVector, V = FVector::ZeroVector;
	double Area = 0.;

	void Consider(const TArray<FVector>& Vertices, const TArray<FVector2D>& UVs, const TArray<int32>& Triangles)
	{
		for (int32 I = 0; I + 2 < Triangles.Num(); I += 3)
		{
			const int32 A = Triangles[I], B = Triangles[I+1], C = Triangles[I+2];
			const FVector P = Vertices[B]-Vertices[A], Q = Vertices[C]-Vertices[A];
			const FVector2D S = UVs[B]-UVs[A], T = UVs[C]-UVs[A];
			const double Det = S.X*T.Y-S.Y*T.X;
			const double ProjectedArea = FMath::Abs(P.X*Q.Y-P.Y*Q.X);
			if (ProjectedArea <= Area || FMath::Abs(Det) < 1.e-12) continue;
			U = (P*T.Y-Q*S.Y)/Det;
			V = (Q*S.X-P*T.X)/Det;
			Area = ProjectedArea;
		}
	}
	FVector Travel(const FVector2D& Velocity) const { return -U*Velocity.X-V*Velocity.Y; }
	FVector2D Along(const FVector& Direction, double Speed) const
	{
		const double Det = U.X*V.Y-U.Y*V.X;
		if (FMath::Abs(Det) < 1.e-12) return FVector2D::ZeroVector;
		return FVector2D((-Direction.X*V.Y+Direction.Y*V.X)/Det,
			(-U.X*Direction.Y+U.Y*Direction.X)/Det).GetSafeNormal(1.e-30) * Speed;
	}
};
}
