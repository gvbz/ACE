#include "Personal/PersonalFarRoomsMeshes.h"
#include "Personal/PersonalFarRooms.h"
#include "ACEDatSubsystem.h"
#include "ACETypes.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACEEnvCellMeshBuilder.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace PersonalFarRoomsMeshes
{
FSource::FSource() = default;
FSource::~FSource() = default;

FSourcePtr Open(const FString& DatDirectory)
{
	FSourcePtr Source = MakeShared<FSource, ESPMode::ThreadSafe>();
	if (!Source->Portal.Open(DatDirectory / TEXT("client_portal.dat")) || !Source->Cell.Open(DatDirectory / TEXT("client_cell_1.dat")))
	{
		UE_LOG(LogTemp, Warning, TEXT("PersonalFarWindows: DAT files not opened in '%s'; no far rooms"), *DatDirectory);
		return nullptr;
	}
	Source->Textures = MakeUnique<FACEDatTextureResolver>(&Source->Portal, nullptr);
	Source->Builder = MakeUnique<FACEEnvCellMeshBuilder>(&Source->Cell, &Source->Portal, Source->Textures.Get());
	return Source;
}

FBuiltPtr Build(FSource& Source, TConstArrayView<uint32> RoomIds, const FTransform& BuildingToBlock, float WorldScale)
{
	TSharedPtr<FBuilt, ESPMode::ThreadSafe> Result = MakeShared<FBuilt, ESPMode::ThreadSafe>();
	if (!Source.Builder)
	{
		return Result;
	}
	// The same order whatever order the openings named them in, so equal buildings give equal meshes.
	TArray<uint32> Ids(RoomIds);
	Ids.Sort();
	TMap<uint64, int32> BySurface;
	for (const uint32 Id : Ids)
	{
		FACEBuiltEnvCellMesh Room;
		if (!Source.Builder->BuildEnvCell(Id, WorldScale, Room))
		{
			continue;
		}
		++Result->Rooms;
		// Room space to landblock space, then into the building's.
		const FTransform ToBuilding = Room.GetCellLocalToLandblock(WorldScale).GetRelativeTransform(BuildingToBlock);
		// The room's outside doorways, as AACEEnvCellActor::LoadEnvCell takes them for its doorway shading: the first two.
		int32 Door = INDEX_NONE;
		{
			FDoors Doors;
			for (int32 I = 0; I < Room.CellPortals.Num() && Doors.Num < 2; ++I)
			{
				if (!Room.CellPortals[I].IsOutsidePortal() || !Room.PortalApertureLocalVerts.IsValidIndex(I) || Room.PortalApertureLocalVerts[I].IsEmpty()
					|| !Room.PortalApertureLocalNormals.IsValidIndex(I))
				{
					continue;
				}
				FVector Center = FVector::ZeroVector;
				for (const FVector& Vertex : Room.PortalApertureLocalVerts[I]) Center += Vertex;
				Center /= Room.PortalApertureLocalVerts[I].Num();
				const FVector C = ToBuilding.TransformPosition(Center);
				const FVector N = ToBuilding.TransformVectorNoScale(Room.PortalApertureLocalNormals[I]).GetSafeNormal();
				const FVector Side(-N.Y, N.X, 0.);
				float HalfWidth = 0.f, HalfHeight = 0.f;
				for (const FVector& Vertex : Room.PortalApertureLocalVerts[I])
				{
					const FVector D = ToBuilding.TransformPosition(Vertex) - C;
					HalfWidth = FMath::Max(HalfWidth, float(FMath::Abs(FVector::DotProduct(D, Side))));
					HalfHeight = FMath::Max(HalfHeight, float(FMath::Abs(D.Z)));
				}
				Doors.Center[Doors.Num] = C;
				Doors.Normal[Doors.Num] = N;
				Doors.HalfWidth[Doors.Num] = HalfWidth;
				Doors.HalfHeight[Doors.Num] = HalfHeight;
				++Doors.Num;
			}
			if (Doors.Num > 0)
			{
				Door = Result->Doors.Add(Doors);
			}
		}
		for (const FACEBuiltMeshSection& From : Room.Sections)
		{
			// As AACEEnvCellActor::LoadEnvCell draws them.
			if (From.IsEmpty() || From.bFullyTransparent)
			{
				continue;
			}
			bool bValid = true;
			for (const int32 Index : From.Triangles)
			{
				bValid = bValid && From.Vertices.IsValidIndex(Index);
			}
			if (!bValid)
			{
				continue;
			}
			// One section per surface; a room with outside doorways keeps its own, for the doorway shading.
			const uint64 SurfaceKey = (uint64(Door + 1) << 40) | (uint64(From.SurfaceId) << 1) | (From.bWrapTexture ? 1 : 0);
			int32* Found = BySurface.Find(SurfaceKey);
			if (!Found)
			{
				Found = &BySurface.Add(SurfaceKey, Result->Sections.AddDefaulted());
				Result->Sections[*Found].SurfaceId = From.SurfaceId;
				Result->Sections[*Found].bWrapTexture = From.bWrapTexture;
				Result->Sections[*Found].Door = Door;
			}
			FSection& To = Result->Sections[*Found];
			const int32 Base = To.Vertices.Num();
			for (int32 I = 0; I < From.Vertices.Num(); ++I)
			{
				To.Vertices.Add(FVector3f(ToBuilding.TransformPosition(From.Vertices[I])));
				To.Normals.Add(From.Normals.IsValidIndex(I) ? FVector3f(ToBuilding.TransformVectorNoScale(From.Normals[I])) : FVector3f::UpVector);
				To.UVs.Add(From.UVs.IsValidIndex(I) ? FVector2f(From.UVs[I]) : FVector2f::ZeroVector);
				To.Colors.Add(From.VertexColors.IsValidIndex(I) ? From.VertexColors[I] : FLinearColor::White);
			}
			for (const int32 Index : From.Triangles)
			{
				To.Triangles.Add(Base + Index);
			}
			Result->Triangles += From.Triangles.Num() / 3;
		}
	}
	// The sharing key: what is drawn, to the millimeter and to the color step.
	uint32 Key = GetTypeHash(Result->Sections.Num());
	for (const FSection& Section : Result->Sections)
	{
		Key = PersonalFarRooms::MixKey(Key, Section.SurfaceId);
		Key = PersonalFarRooms::MixKey(Key, Section.bWrapTexture ? 1u : 0u);
		Key = PersonalFarRooms::MixKey(Key, uint32(Section.Triangles.Num()));
		Key = PersonalFarRooms::MixKey(Key, uint32(Section.Door + 1));
		for (int32 I = 0; I < Section.Vertices.Num(); ++I)
		{
			const FVector3f& V = Section.Vertices[I];
			Key = PersonalFarRooms::MixKey(Key, GetTypeHash(FIntVector(FMath::RoundToInt32(V.X * 10.f), FMath::RoundToInt32(V.Y * 10.f), FMath::RoundToInt32(V.Z * 10.f))));
			Key = PersonalFarRooms::MixKey(Key, Section.Colors[I].ToFColor(true).DWColor());
		}
	}
	Result->Key = Key;
	return Result;
}

UStaticMesh* MakeStaticMesh(UACEDatSubsystem& Dat, UObject* Outer, const FBuilt& Built, TArray<UMaterialInterface*>& OutMaterials, TArray<int32>* OutSlotSections)
{
	OutMaterials.Reset();
	if (OutSlotSections) OutSlotSections->Reset();
	FMeshDescription MeshDesc;
	FStaticMeshAttributes Attributes(MeshDesc);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(1);
	for (const FSection& Section : Built.Sections)
	{
		// The client's own room material for the surface; its fallback is the vertex color material.
		UMaterialInterface* Material = Dat.GetOrCreateEnvCellMaterial(Section.SurfaceId, Section.bWrapTexture);
		if (!Material)
		{
			Material = Dat.GetVertexColorMaterial();
		}
		if (!Material || Section.Triangles.Num() < 3)
		{
			continue;
		}
		const int32 Slot = OutMaterials.Add(Material);
		if (OutSlotSections) OutSlotSections->Add(int32(&Section - Built.Sections.GetData()));
		const FPolygonGroupID Group = MeshDesc.CreatePolygonGroup();
		SlotNames[Group] = FName(*FString::Printf(TEXT("ACE_%d"), Slot));
		TArray<FVertexID> Vertices;
		Vertices.Reserve(Section.Vertices.Num());
		for (const FVector3f& Vertex : Section.Vertices)
		{
			const FVertexID Id = MeshDesc.CreateVertex();
			Positions[Id] = Vertex;
			Vertices.Add(Id);
		}
		for (int32 T = 0; T + 2 < Section.Triangles.Num(); T += 3)
		{
			TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 I = Section.Triangles[T + K];
				const FVertexInstanceID Corner = MeshDesc.CreateVertexInstance(Vertices[I]);
				Normals[Corner] = Section.Normals[I];
				UVs.Set(Corner, 0, Section.UVs[I]);
				Colors[Corner] = FVector4f(Section.Colors[I]);
				Corners.Add(Corner);
			}
			MeshDesc.CreatePolygon(Group, Corners);
		}
	}
	if (OutMaterials.Num() == 0)
	{
		return nullptr;
	}
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient | RF_DuplicateTransient);
	if (!Mesh)
	{
		return nullptr;
	}
	Mesh->bAllowCPUAccess = false;
	TArray<FStaticMaterial> StaticMaterials;
	for (int32 Slot = 0; Slot < OutMaterials.Num(); ++Slot)
	{
		const FName SlotName(*FString::Printf(TEXT("ACE_%d"), Slot));
		StaticMaterials.Add(FStaticMaterial(OutMaterials[Slot], SlotName));
		StaticMaterials.Last().UVChannelData.bInitialized = true;
#if WITH_EDITORONLY_DATA
		StaticMaterials.Last().ImportedMaterialSlotName = SlotName;
#endif
	}
	Mesh->SetStaticMaterials(StaticMaterials);
	TArray<const FMeshDescription*> Descriptions;
	Descriptions.Add(&MeshDesc);
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bBuildSimpleCollision = false;
	Params.bFastBuild = true;
	Params.bAllowCpuAccess = false;
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = true;
	Mesh->BuildFromMeshDescriptions(Descriptions, Params);
	Mesh->CalculateExtendedBounds();
	return Mesh;
}

void ApplyDoorShading(UStaticMeshComponent& Component, const FBuilt& Built, TConstArrayView<int32> SlotSections, const FTransform& BuildingToWorld, TArray<UMaterialInstanceDynamic*>& OutMade)
{
	for (int32 Slot = 0; Slot < SlotSections.Num(); ++Slot)
	{
		const int32 Door = Built.Sections.IsValidIndex(SlotSections[Slot]) ? Built.Sections[SlotSections[Slot]].Door : INDEX_NONE;
		UMaterialInterface* Source = Built.Doors.IsValidIndex(Door) ? Component.GetMaterial(Slot) : nullptr;
		if (!Source || !Source->GetMaterial())
		{
			continue;
		}
		// As the client: an instance per room, with the shared one's values.
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Source->GetMaterial(), &Component);
		if (!Material)
		{
			continue;
		}
		Material->CopyMaterialUniformParameters(Source);
		const FDoors& Doors = Built.Doors[Door];
		for (int32 D = 0; D < Doors.Num; ++D)
		{
			const FVector C = BuildingToWorld.TransformPosition(Doors.Center[D]);
			const FVector N = BuildingToWorld.TransformVectorNoScale(Doors.Normal[D]).GetSafeNormal();
			Material->SetVectorParameterValue(FName(*FString::Printf(TEXT("Door%d"), D)), FLinearColor(C.X, C.Y, C.Z, Doors.HalfWidth[D]));
			Material->SetVectorParameterValue(FName(*FString::Printf(TEXT("Normal%d"), D)), FLinearColor(N.X, N.Y, N.Z, Doors.HalfHeight[D]));
		}
		Component.SetMaterial(Slot, Material);
		OutMade.Add(Material);
	}
}
}
