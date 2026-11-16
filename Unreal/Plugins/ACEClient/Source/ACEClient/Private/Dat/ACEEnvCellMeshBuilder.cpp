#include "Dat/ACEEnvCellMeshBuilder.h"
#include "Dat/ACEDatCursor.h"
#include "ACETypes.h"
#include "Dat/ACEPolygonMeshBuilder.h"

void FACEEnvCellMeshBuilder::BakeStaticLighting(const FACEDatEnvCell& Cell, float WorldScale, FACEBuiltEnvCellMesh& Mesh)
{
	// CEnvCell::init_static_objects / CObjCell::add_static_to_global_lights.
	// Resolve the authored visible-cell light set before streaming the furniture,
	// so adjoining rooms do not change brightness as their objects finish loading.
	struct FLight { FVector Position; FLinearColor Color; double Intensity; double Range; };
	TArray<FLight> Lights;
	const FTransform CellFrame(FQuat(Cell.Orientation), FVector(Cell.Origin));
	auto Gather = [&](const FACEDatEnvCell& Source)
	{
		for (const FACEDatStab& Stab : Source.StaticObjects)
		{
			if ((Stab.Id >> 24) != 2) continue;
			if (!SetupLightCache.Contains(Stab.Id))
			{
				TArray<FACEDatLightInfo> ParsedLights;
				TArray<uint8> Blob;
				if (Portal->ReadFile(Stab.Id, Blob))
				{
					FACEDatCursor Cursor(Blob); FACEDatSetupModel Setup;
					if (ACEDatUnpack::UnpackSetupModel(Cursor, Setup)) Setup.Lights.GenerateValueArray(ParsedLights);
				}
				SetupLightCache.Add(Stab.Id, MoveTemp(ParsedLights));
			}
			const FTransform Placement(FQuat(Stab.Orientation), FVector(Stab.Origin));
			for (const auto& Info : SetupLightCache[Stab.Id])
			{
				if (Info.Intensity <= 0 || Info.Falloff <= 0) continue;
				const FVector BlockPosition = Placement.TransformPosition(FVector(Info.Origin));
				const FVector Local = CellFrame.InverseTransformPosition(BlockPosition);
				const FLinearColor Color(((Info.Color >> 16)&255)/255.f, ((Info.Color >> 8)&255)/255.f, (Info.Color&255)/255.f);
				const float Range = Info.Falloff * 1.3f; // retail static_light_factor
				Lights.Add({FACEPosition::AceVectorToUnreal(Local, 1.f), Color, Info.Intensity, Range});
				if (Source.Id == Cell.Id)
					Mesh.PointLights.Add({FACEPosition::AceVectorToUnreal(BlockPosition, WorldScale), Color, Info.Intensity, Range * WorldScale});
			}
		}
	};
	Gather(Cell);
	TSet<uint32> Visited; Visited.Add(Cell.Id);
	for (uint16 ShortId : Cell.VisibleCells)
	{
		const uint32 Id = (Cell.Id & 0xFFFF0000u) | ShortId;
		if (ShortId < 0x100 || ShortId == 0xFFFF || Visited.Contains(Id)) continue;
		Visited.Add(Id);
		TArray<uint8> Blob;
		if (CellDat->ReadFile(Id, Blob))
		{
			FACEDatCursor Cursor(Blob); FACEDatEnvCell Other;
			if (ACEDatUnpack::UnpackEnvCell(Cursor, Other)) Gather(Other);
		}
	}
	// D3DPolyRender::SetStaticLightingVertexColors / calc_point_light.
	for (auto& Section : Mesh.Sections)
	{
		Section.VertexColors.SetNum(Section.Vertices.Num());
		for (int32 I=0; I<Section.Vertices.Num(); ++I)
		{
			FLinearColor Sum(0,0,0,1);
			const FVector Normal = Section.Normals.IsValidIndex(I) ? Section.Normals[I] : FVector::UpVector;
			for (const FLight& Light : Lights)
			{
				const FVector Delta = Light.Position - Section.Vertices[I] / WorldScale;
				const double Distance = Delta.Size();
				if (Distance >= Light.Range) continue;
				const double WrappedCosine = (0.5 + FVector::DotProduct(Normal, Delta) / FMath::Max(Distance, 1.e-6)) / 1.5;
				const float Strength = FMath::Clamp(WrappedCosine * (1.0 - Distance / Light.Range)
					* Light.Intensity / FMath::Max(1.0, Distance * Distance), 0.0, 1.0);
				Sum.R += Light.Color.R * Strength; Sum.G += Light.Color.G * Strength; Sum.B += Light.Color.B * Strength;
			}
			Section.VertexColors[I] = Sum.GetClamped();
		}
	}
}

bool FACEEnvCellMeshBuilder::LoadEnvironment(uint32 EnvironmentId, FACEDatEnvironment& Out)
{
	if (const FACEDatEnvironment* Cached = GetEnvironment(EnvironmentId))
	{
		Out = *Cached;
		return true;
	}
	return false;
}

const FACEDatEnvironment* FACEEnvCellMeshBuilder::GetEnvironment(uint32 EnvironmentId)
{
	if (const FACEDatEnvironment* Cached = EnvironmentCache.Find(EnvironmentId)) return Cached;
	if (!Portal)
	{
		return nullptr;
	}

	TArray<uint8> Blob;
	if (!Portal->ReadFile(EnvironmentId, Blob))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: missing Environment 0x%08X"), EnvironmentId);
		return nullptr;
	}

	FACEDatCursor Cur(Blob);
	FACEDatEnvironment Parsed;
	if (!ACEDatUnpack::UnpackEnvironment(Cur, Parsed))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: failed to unpack Environment 0x%08X"), EnvironmentId);
		return nullptr;
	}

	// Workers only need a borrowed view. Copying the complete dungeon environment
	// on each cache miss doubled its transient memory and copied every cell BSP.
	return &EnvironmentCache.Add(EnvironmentId, MoveTemp(Parsed));
}

void FACEEnvCellMeshBuilder::AppendCellStructLocal(
    const FACEDatCellStruct& CellStruct, const TArray<uint32>& Surfaces,
    const TArray<FACEDatCellPortal>& Portals,
    float WorldScale, TArray<FACEBuiltMeshSection>& OutSections, FBox& OutLocalBounds)
{
    // ACRenderDevice::DrawEnvCell submits DrawingBSP InPolys. The dictionary also
    // contains connectivity planes with blue/black diagnostic surfaces. Conversely,
    // Town Network ceiling caps are both connectivity planes and drawable InPolys.
    TMap<uint16, FACEDatPolygon> DrawPolygons;
    for (uint16 PolygonId : CellStruct.DrawingPolygonIds)
        if (const auto* Polygon = CellStruct.Polygons.Find(PolygonId))
            DrawPolygons.Add(PolygonId, *Polygon);
    // Preserve containment bounds even for portal-only connector cells.
    for (const auto& Pair : CellStruct.Vertices)
        OutLocalBounds += FACEPosition::AceVectorToUnreal(FVector(Pair.Value.Origin), WorldScale);
    ACEPolygonMeshBuilder::Append(DrawPolygons, CellStruct.Vertices, Surfaces,
        Textures, INDEX_NONE, nullptr, WorldScale, OutSections);
    for (const auto& Section : OutSections)
        for (const FVector& Position : Section.Vertices) OutLocalBounds += Position;
}

void FACEEnvCellMeshBuilder::AppendCellStructPhysicsLocal(
	const FACEDatCellStruct& CellStruct,
	float WorldScale,
	TArray<FACEBuiltMeshSection>& OutCollisionSections,
	FBox& OutLocalBounds)
{
	// Retail indoor collision walks PhysicsBSP leaf poly ids into PhysicsPolygons.
	if (CellStruct.PhysicsPolygons.Num() == 0)
	{
		return;
	}

	// Only PhysicsBSP leaf references participate in retail collision. An empty
	// leaf set is empty space, not permission to use draw faces or unused polygons.
	TArray<uint16> PolyIds = CellStruct.PhysicsCollisionPolyIds.Array();

	FACEBuiltMeshSection Section;
	Section.bCollisionOnly = true;
	Section.SurfaceId = 0;

	for (const uint16 PolyId : PolyIds)
	{
		const FACEDatPolygon* PolyPtr = CellStruct.PhysicsPolygons.Find(PolyId);
		if (!PolyPtr)
		{
			continue;
		}
		const FACEDatPolygon& Poly = *PolyPtr;
		if (Poly.NumPts < 3 || Poly.VertexIds.Num() < 3)
		{
			continue;
		}

		const int32 BaseIndex = Section.Vertices.Num();
		TArray<int32> LocalIndices;
		LocalIndices.Reserve(Poly.VertexIds.Num());

		for (const int16 VertId : Poly.VertexIds)
		{
			const FACEDatSWVertex* SW = CellStruct.Vertices.Find(static_cast<uint16>(VertId));
			if (!SW)
			{
				continue;
			}

			const FVector LocalPos = FACEPosition::AceVectorToUnreal(
				FVector(SW->Origin.X, SW->Origin.Y, SW->Origin.Z), WorldScale);
			const FVector LocalN = FACEPosition::AceVectorToUnreal(
				FVector(SW->Normal.X, SW->Normal.Y, SW->Normal.Z), 1.f).GetSafeNormal();

			Section.Vertices.Add(LocalPos);
			Section.Normals.Add(LocalN);
			Section.UVs.Add(FVector2D::ZeroVector);
			Section.VertexColors.Add(FLinearColor::White);
			LocalIndices.Add(BaseIndex + LocalIndices.Num());
			OutLocalBounds += LocalPos;
		}

		if (LocalIndices.Num() < 3)
		{
			continue;
		}

		// Preserve every PhysicsBSP face, including stair risers.
		// Double-sided fans: PhysicsBSP winding after RH→LH is not always walkable from above
		// (stairs/floors fell through when collision was single-sided).
		for (int32 i = 1; i + 1 < LocalIndices.Num(); ++i)
		{
			Section.Triangles.Add(LocalIndices[0]);
			Section.Triangles.Add(LocalIndices[i + 1]);
			Section.Triangles.Add(LocalIndices[i]);

			Section.Triangles.Add(LocalIndices[0]);
			Section.Triangles.Add(LocalIndices[i]);
			Section.Triangles.Add(LocalIndices[i + 1]);
		}
	}

	if (!Section.IsEmpty())
	{
		OutCollisionSections.Add(MoveTemp(Section));
	}
}

bool FACEEnvCellMeshBuilder::BuildEnvCell(uint32 EnvCellId, float WorldScale, FACEBuiltEnvCellMesh& OutMesh)
{
	OutMesh = FACEBuiltEnvCellMesh();
	if (!CellDat || EnvCellId == 0)
	{
		return false;
	}

	TArray<uint8> Blob;
	if (!CellDat->ReadFile(EnvCellId, Blob))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: missing EnvCell 0x%08X"), EnvCellId);
		return false;
	}

	FACEDatCursor Cur(Blob);
	FACEDatEnvCell EnvCell;
	if (!ACEDatUnpack::UnpackEnvCell(Cur, EnvCell))
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: failed to unpack EnvCell 0x%08X"), EnvCellId);
		return false;
	}

	OutMesh.EnvCellId = EnvCell.Id;
	OutMesh.Origin = EnvCell.Origin;
	OutMesh.Orientation = EnvCell.Orientation;
	OutMesh.Flags = EnvCell.Flags;
	OutMesh.StaticObjects = EnvCell.StaticObjects;
	OutMesh.VisibleCells = EnvCell.VisibleCells;
	OutMesh.CellPortals = EnvCell.CellPortals;

	const FACEDatEnvironment* Env = GetEnvironment(EnvCell.EnvironmentId);
	if (!Env)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: EnvCell 0x%08X missing Environment 0x%08X"), EnvCellId, EnvCell.EnvironmentId);
		return false;
	}

	const FACEDatCellStruct* CellStruct = Env->Cells.Find(EnvCell.CellStructure);
	if (!CellStruct)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: EnvCell 0x%08X CellStructure %u not found in Environment 0x%08X"),
			EnvCellId, EnvCell.CellStructure, EnvCell.EnvironmentId);
		return false;
	}

	// CellBSP for EnvCell.point_in_cell (retail BSPNode.point_inside_cell_bsp).
	OutMesh.CellBspNodes = CellStruct->CellBspNodes;

	// CellPortal planes define visibility/transition apertures independently of
	// the DrawingBSP membership used to build the room's render mesh above.
	OutMesh.OutsidePortalPlanes.Reset();
	OutMesh.OutsidePortalMesh = FACEBuiltMeshSection();
	OutMesh.OutsidePortalMesh.bFullyTransparent = true;
	OutMesh.PortalApertureLocalVerts.SetNum(EnvCell.CellPortals.Num());
	OutMesh.PortalApertureLocalNormals.SetNum(EnvCell.CellPortals.Num());
	OutMesh.PortalApertureLocalD.SetNum(EnvCell.CellPortals.Num());
	for (int32 PortalIdx = 0; PortalIdx < EnvCell.CellPortals.Num(); ++PortalIdx)
	{
		const FACEDatCellPortal& CellPortal = EnvCell.CellPortals[PortalIdx];

		const FACEDatPolygon* Poly = CellStruct->Polygons.Find(CellPortal.PolygonId);
		if (!Poly || Poly->VertexIds.Num() < 3)
		{
			continue;
		}
		// Build the plane in ACE space first, then convert — geometric cross after
		// AceVectorToUnreal X-flip inverts winding and breaks outdoor half-space tests.
		TArray<FVector, TInlineAllocator<8>> AcePts;
		for (const int16 VertId : Poly->VertexIds)
		{
			const FACEDatSWVertex* SW = CellStruct->Vertices.Find(static_cast<uint16>(VertId));
			if (!SW)
			{
				continue;
			}
			AcePts.Add(FVector(SW->Origin.X, SW->Origin.Y, SW->Origin.Z));
		}
		if (AcePts.Num() < 3)
		{
			continue;
		}
		FVector AceN = FVector::CrossProduct(AcePts[1] - AcePts[0], AcePts[2] - AcePts[0]).GetSafeNormal();
		if (AceN.IsNearlyZero())
		{
			continue;
		}
		float AceD = -FVector::DotProduct(AceN, AcePts[0]);
		// CCellPortal::UnPack / PView::FindInPortals: portal_side selects the
		// polygon half-space. A cell's frame origin need not lie inside the cell.
		// Normalize to outgoing positive for transit and view clipping alike.
		if (!CellPortal.IsPortalSide())
		{
			AceN = -AceN;
			AceD = -AceD;
		}
		const FVector UeN = FACEPosition::AceVectorToUnreal(AceN, 1.f).GetSafeNormal();
		const FVector UeP0 = FACEPosition::AceVectorToUnreal(AcePts[0], WorldScale);
		const float UeD = -FVector::DotProduct(UeN, UeP0);

		TArray<FVector>& LocalVerts = OutMesh.PortalApertureLocalVerts[PortalIdx];
		LocalVerts.Reserve(AcePts.Num());
		for (const FVector& AceP : AcePts)
		{
			LocalVerts.Add(FACEPosition::AceVectorToUnreal(AceP, WorldScale));
		}
		OutMesh.PortalApertureLocalNormals[PortalIdx] = UeN;
		OutMesh.PortalApertureLocalD[PortalIdx] = UeD;

		if (!CellPortal.IsOutsidePortal())
		{
			continue;
		}
		OutMesh.OutsidePortalPlanes.Add(FVector4f(
			static_cast<float>(UeN.X), static_cast<float>(UeN.Y), static_cast<float>(UeN.Z),
			static_cast<float>(UeD)));

		// Portal poly mesh (cell-local) for CustomDepth stencil writers (look-out only).
		const int32 BaseVert = OutMesh.OutsidePortalMesh.Vertices.Num();
		TArray<int32, TInlineAllocator<8>> FanIdx;
		for (const FVector& UeP : LocalVerts)
		{
			OutMesh.OutsidePortalMesh.Vertices.Add(UeP);
			OutMesh.OutsidePortalMesh.Normals.Add(UeN);
			OutMesh.OutsidePortalMesh.UVs.Add(FVector2D::ZeroVector);
			OutMesh.OutsidePortalMesh.VertexColors.Add(FLinearColor(0.f, 0.f, 0.f, 0.f));
			FanIdx.Add(BaseVert + FanIdx.Num());
		}
		for (int32 Ti = 1; Ti + 1 < FanIdx.Num(); ++Ti)
		{
			OutMesh.OutsidePortalMesh.Triangles.Add(FanIdx[0]);
			OutMesh.OutsidePortalMesh.Triangles.Add(FanIdx[Ti]);
			OutMesh.OutsidePortalMesh.Triangles.Add(FanIdx[Ti + 1]);
		}
	}
	FBox LocalBounds(ForceInit);
	AppendCellStructLocal(
		*CellStruct, EnvCell.Surfaces, EnvCell.CellPortals, WorldScale, OutMesh.Sections, LocalBounds);
	BakeStaticLighting(EnvCell, WorldScale, OutMesh);
	// CEnvCell::find_env_collisions uses PhysicsBSP with PhysicsPolygons, not drawing
	// polygon ids or surface opacity. The two dictionaries reuse numeric keys; removing
	// a drawing portal id from physics removed unrelated floors and walls. Preserve
	// the authored collision mesh without synthetic floor caps or draw-mesh treads.
	AppendCellStructPhysicsLocal(*CellStruct, WorldScale, OutMesh.CollisionSections, LocalBounds);
	if (LocalBounds.IsValid)
	{
		OutMesh.LocalBoundsMin = LocalBounds.Min;
		OutMesh.LocalBoundsMax = LocalBounds.Max;
		OutMesh.bHasLocalBounds = true;
	}

	const bool bOk = !OutMesh.IsEmpty() || !OutMesh.OutsidePortalMesh.IsEmpty();
	if (bOk && (OutMesh.OutsidePortalPlanes.Num() > 0 || !OutMesh.OutsidePortalMesh.IsEmpty()))
	{
		UE_LOG(LogTemp, Log, TEXT("ACEDat: EnvCell 0x%08X outsidePortalPlanes=%d portalTris=%d drawSections=%d"),
			EnvCellId, OutMesh.OutsidePortalPlanes.Num(),
			OutMesh.OutsidePortalMesh.Triangles.Num() / 3, OutMesh.Sections.Num());
	}
	if (!bOk)
	{
		// Stab-only hubs first — marketplace balcony props live in portal-only shells.
		if (OutMesh.StaticObjects.Num() > 0)
		{
			return true;
		}
		// Portal-only connector cells (all draw polys are CellPortals, no physics) are valid
		// for VisibleCells sync but have nothing to mesh.
		const bool bPortalOnly = OutMesh.CellPortals.Num() > 0
			&& CellStruct->Polygons.Num() > 0
			&& CellStruct->PhysicsPolygons.Num() == 0;
		if (bPortalOnly)
		{
			OutMesh.bPortalConnector = true;
			UE_LOG(LogTemp, Verbose, TEXT("ACEDat: EnvCell 0x%08X is portal-only (no draw/collision mesh)"), EnvCellId);
			return true;
		}
		UE_LOG(LogTemp, Warning, TEXT("ACEDat: EnvCell 0x%08X produced no drawable/collision geometry"), EnvCellId);
	}
	return bOk;
}
