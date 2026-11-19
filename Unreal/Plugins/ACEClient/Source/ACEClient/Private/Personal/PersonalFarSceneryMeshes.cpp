#include "Personal/PersonalFarSceneryMeshes.h"
#include "Personal/PersonalFarSceneryPlacement.h"
#include "ACEDatSubsystem.h"
#include "ACETypes.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Dat/ACESetupMeshBuilder.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"

namespace
{
	int32 DrawnTriangles(const TArray<FACEBuiltMeshSection>& Sections)
	{
		int32 Count = 0;
		for (const FACEBuiltMeshSection& Section : Sections)
		{
			if (!Section.bCollisionOnly && !Section.bFullyTransparent && !Section.IsEmpty())
			{
				Count += Section.Triangles.Num() / 3;
			}
		}
		return Count;
	}

	void AppendSections(const TArray<FACEBuiltMeshSection>& From, const FTransform& Bind, TArray<PersonalFarSceneryMeshes::FSection>& Out)
	{
		for (const FACEBuiltMeshSection& Section : From)
		{
			if (Section.bCollisionOnly || Section.bFullyTransparent || Section.IsEmpty())
			{
				continue;
			}
			PersonalFarSceneryMeshes::FSection& Added = Out.AddDefaulted_GetRef();
			Added.SurfaceId = Section.SurfaceId;
			Added.bWrapTexture = Section.bWrapTexture;
			Added.bClipMap = Section.bClipMap;
			Added.UVs = Section.UVs;
			Added.Triangles = Section.Triangles;
			Added.Vertices.Reserve(Section.Vertices.Num());
			Added.Normals.Reserve(Section.Vertices.Num());
			for (int32 I = 0; I < Section.Vertices.Num(); ++I)
			{
				Added.Vertices.Add(Bind.TransformPosition(Section.Vertices[I]));
				Added.Normals.Add(Section.Normals.IsValidIndex(I) ? Bind.TransformVectorNoScale(Section.Normals[I]).GetSafeNormal() : FVector::UpVector);
			}
		}
	}

	/** UACEDatSubsystem.cpp IsDatPortalFillSurface: solid portal fills are not drawn. */
	bool IsPortalFill(const FACEDatSurfaceRenderFlags& Decoded)
	{
		if (!Decoded.bFullyTransparent || Decoded.bClipMap)
		{
			return false;
		}
		const FLinearColor& C = Decoded.SolidColor;
		const bool bMagenta = Decoded.bIsSolid && C.R > 0.15f && C.B > 0.15f && C.G < 0.08f;
		return bMagenta || (C.A < 0.08f && Decoded.Translucency > 0.85f);
	}
}

namespace PersonalFarSceneryMeshes
{
FSourcePtr Open(const FString& DatDirectory)
{
	FSourcePtr Source = MakeShared<FSource, ESPMode::ThreadSafe>();
	if (!Source->Portal.Open(DatDirectory / TEXT("client_portal.dat")))
	{
		UE_LOG(LogTemp, Warning, TEXT("PersonalFarScenery: portal DAT not opened in '%s'; far objects keep full detail"), *DatDirectory);
		return nullptr;
	}
	return Source;
}

void Build(FSource& Source, const TArray<uint32>& SetupIds, float WorldScale, TMap<uint32, FLevelsPtr>& Out)
{
	FACEDatTextureResolver Textures(&Source.Portal, nullptr);
	FACESetupMeshBuilder Builder(&Source.Portal, nullptr, &Textures);
	for (const uint32 SetupId : SetupIds)
	{
		TSharedPtr<FLevels, ESPMode::ThreadSafe> Levels = MakeShared<FLevels, ESPMode::ThreadSafe>();
		FACEBuiltSetupMesh Built;
		// SetupId is a type key: the DAT id with the kind, which gives the placement (resting for stabs).
		if (Builder.BuildSetupWithAppearance(PersonalFarSceneryPlacement::TypeId(SetupId), FACEObjDesc(), WorldScale, Built,
			PersonalFarSceneryPlacement::TypePlacement(SetupId), /*bDistanceLevels*/ true))
		{
			// Per part, the sections each level draws: the DAT's model for that level when it has one
			// with fewer triangles than the level before, else the level before.
			TArray<const TArray<FACEBuiltMeshSection>*> Chosen;
			Chosen.Reserve(Built.Parts.Num());
			for (const FACEBuiltSetupPart& Part : Built.Parts)
			{
				Chosen.Add(&Part.Sections);
				Levels->Triangles[0] += DrawnTriangles(Part.Sections);
			}
			for (int32 Level = 1; Level < LevelCount; ++Level)
			{
				bool bSimpler = false;
				for (int32 P = 0; P < Built.Parts.Num(); ++P)
				{
					const FACEBuiltSetupPart& Part = Built.Parts[P];
					if (Part.DistanceLevels.IsValidIndex(Level) && Part.DistanceLevels[Level].GfxObjId != 0)
					{
						const TArray<FACEBuiltMeshSection>& Candidate = Part.DistanceLevels[Level].Sections;
						const int32 Triangles = DrawnTriangles(Candidate);
						if (Triangles > 0 && Triangles < DrawnTriangles(*Chosen[P]))
						{
							Chosen[P] = &Candidate;
							bSimpler = true;
						}
					}
				}
				for (int32 P = 0; P < Built.Parts.Num(); ++P)
				{
					Levels->Triangles[Level] += DrawnTriangles(*Chosen[P]);
				}
				if (bSimpler)
				{
					for (int32 P = 0; P < Built.Parts.Num(); ++P)
					{
						AppendSections(*Chosen[P], Built.Parts[P].BindTransform, Levels->Sections[Level - 1]);
					}
				}
			}
		}
		Out.Add(SetupId, Levels);
	}
}

/** A static mesh with one material slot per section. No collision. */
static UStaticMesh* BuildMesh(UObject* Outer, const TArray<const FSection*>& Drawn, const TArray<UMaterialInterface*>& OutMaterials);

UStaticMesh* MakeStaticMesh(UACEDatSubsystem& Dat, UObject* Outer, const TArray<FSection>& Sections, TArray<UMaterialInterface*>& OutMaterials)
{
	OutMaterials.Reset();
	// Which sections are drawn, and with what: the rules of GetOrCreateSetupStaticMesh for outdoor scenery.
	TArray<const FSection*> Drawn;
	FACEDatTextureResolver* Textures = Dat.GetTextureResolver();
	for (const FSection& Section : Sections)
	{
		bool bAdditive = false;
		if (Textures && Section.SurfaceId != 0)
		{
			FACEDatSurfaceRenderFlags Decoded;
			if (Textures->ResolveSurfaceRenderFlags(Section.SurfaceId, Decoded))
			{
				if (IsPortalFill(Decoded) && !Section.bClipMap)
				{
					continue;
				}
				bAdditive = Decoded.bAdditive;
			}
		}
		UMaterialInterface* Material = Dat.GetOrCreateOutdoorLitMaterial(Section.SurfaceId, Section.bWrapTexture);
		if (!Material)
		{
			if (bAdditive)
			{
				continue;
			}
			Material = Dat.GetVertexColorMaterial();
		}
		if (!Material)
		{
			continue;
		}
		Drawn.Add(&Section);
		OutMaterials.Add(Material);
	}
	return Drawn.Num() > 0 ? BuildMesh(Outer, Drawn, OutMaterials) : nullptr;
}

static UStaticMesh* BuildMesh(UObject* Outer, const TArray<const FSection*>& Drawn, const TArray<UMaterialInterface*>& OutMaterials)
{
	FMeshDescription MeshDesc;
	FStaticMeshAttributes Attributes(MeshDesc);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(1);
	for (int32 Slot = 0; Slot < Drawn.Num(); ++Slot)
	{
		const FSection& Section = *Drawn[Slot];
		const FPolygonGroupID Group = MeshDesc.CreatePolygonGroup();
		SlotNames[Group] = FName(*FString::Printf(TEXT("ACE_%d"), Slot));
		TArray<FVertexID> Vertices;
		Vertices.Reserve(Section.Vertices.Num());
		for (const FVector& Vertex : Section.Vertices)
		{
			const FVertexID Id = MeshDesc.CreateVertex();
			Positions[Id] = FVector3f(Vertex);
			Vertices.Add(Id);
		}
		for (int32 T = 0; T + 2 < Section.Triangles.Num(); T += 3)
		{
			const int32 Index[3] = {Section.Triangles[T], Section.Triangles[T + 1], Section.Triangles[T + 2]};
			if (!Vertices.IsValidIndex(Index[0]) || !Vertices.IsValidIndex(Index[1]) || !Vertices.IsValidIndex(Index[2]))
			{
				continue;
			}
			TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
			for (const int32 I : Index)
			{
				const FVertexInstanceID Corner = MeshDesc.CreateVertexInstance(Vertices[I]);
				Normals[Corner] = Section.Normals.IsValidIndex(I) ? FVector3f(Section.Normals[I]) : FVector3f::UpVector;
				UVs.Set(Corner, 0, Section.UVs.IsValidIndex(I) ? FVector2f(Section.UVs[I]) : FVector2f::ZeroVector);
				Corners.Add(Corner);
			}
			MeshDesc.CreatePolygon(Group, Corners);
		}
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
		// Fast runtime builds skip the editor's UV-density derivation; streaming queries still need valid data.
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
	// Editor builds require source models even for transient fast-built meshes.
	Params.bCommitMeshDescription = true;
	Mesh->BuildFromMeshDescriptions(Descriptions, Params);
	Mesh->CalculateExtendedBounds();
	if (Mesh->GetBounds().SphereRadius < 40.f)
	{
		Mesh->SetPositiveBoundsExtension(FVector(400.f));
		Mesh->SetNegativeBoundsExtension(FVector(400.f));
		Mesh->CalculateExtendedBounds();
	}
	return Mesh;
}

float OpeningArea(const TArray<FVector>& Vertices, float WorldScale)
{
	FVector Cross = FVector::ZeroVector;
	for (int32 I = 1; I + 1 < Vertices.Num(); ++I)
	{
		Cross += FVector::CrossProduct(Vertices[I] - Vertices[0], Vertices[I + 1] - Vertices[0]);
	}
	return float(Cross.Size() * 0.5 / FMath::Square(double(FMath::Max(WorldScale, 1.f))));
}

void OpeningKinds(const FACEBuiltSetupMesh& Built, float WorldScale, TArray<PersonalFarWindows::EKind>& OutKinds, TArray<PersonalFarWindows::FOpening>* OutOpenings,
	const PersonalFarWindows::FDoorRule& Door)
{
	const double Scale = FMath::Max(WorldScale, 1.f);
	TArray<PersonalFarWindows::FOpening> Openings;
	for (const FACEBuiltSetupPart& Part : Built.Parts)
	{
		for (const FACEBuiltSetupPortal& Polygon : Part.Portals)
		{
			if (Polygon.Vertices.Num() < 3)
			{
				continue;
			}
			// In the building's own space: a building stands upright wherever it is placed.
			TArray<FVector> Corners;
			double Low = MAX_dbl, High = -MAX_dbl, Wide = 0.;
			for (const FVector& Vertex : Polygon.Vertices)
			{
				Corners.Add(Part.BindTransform.TransformPosition(Vertex));
				Low = FMath::Min(Low, Corners.Last().Z);
				High = FMath::Max(High, Corners.Last().Z);
			}
			FVector Cross = FVector::ZeroVector;
			for (int32 I = 0; I < Corners.Num(); ++I)
			{
				for (int32 J = I + 1; J < Corners.Num(); ++J) Wide = FMath::Max(Wide, FVector::Dist2D(Corners[I], Corners[J]));
				if (I > 0 && I + 1 < Corners.Num()) Cross += FVector::CrossProduct(Corners[I] - Corners[0], Corners[I + 1] - Corners[0]);
			}
			PersonalFarWindows::FOpening& Opening = Openings.AddDefaulted_GetRef();
			Opening.Area = OpeningArea(Polygon.Vertices, WorldScale);
			Opening.Width = float(Wide / Scale);
			Opening.Height = float((High - Low) / Scale);
			Opening.Bottom = float(Low / Scale);
			Opening.bFlat = FMath::Abs(Cross.GetSafeNormal().Z) > 0.7;
		}
	}
	const float Lowest = PersonalFarWindows::LowestOpening(Openings);
	OutKinds.Reset(Openings.Num());
	for (const PersonalFarWindows::FOpening& Opening : Openings)
	{
		OutKinds.Add(PersonalFarWindows::KindOf(Opening, Lowest, Door));
	}
	if (OutOpenings)
	{
		*OutOpenings = MoveTemp(Openings);
	}
}

void SetPanelColor(UTexture2D* Texture, FColor Color)
{
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return;
	}
	Color.A = 255;
	FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
	FMemory::Memcpy(Mip.BulkData.Lock(LOCK_READ_WRITE), &Color, sizeof(FColor));
	Mip.BulkData.Unlock();
	Texture->UpdateResource();
}

bool MakePanelMaterial(UMaterialInterface* Parent, UObject* Outer, TObjectPtr<UMaterialInterface>& OutMaterial, TObjectPtr<UTexture2D>& OutTexture)
{
	UTexture2D* Dark = Parent ? UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8) : nullptr;
	UMaterialInstanceDynamic* Material = (Dark && Dark->GetPlatformData() && Dark->GetPlatformData()->Mips.Num() > 0) ? UMaterialInstanceDynamic::Create(Parent, Outer) : nullptr;
	if (!Material)
	{
		return false;
	}
	Dark->CompressionSettings = TC_EditorIcon;
	Dark->SRGB = true;
	Dark->NeverStream = true;
	SetPanelColor(Dark, PersonalFarWindows::DoorColor());
	Material->SetTextureParameterValue(TEXT("ACETexture"), Dark);
	Material->SetTextureParameterValue(TEXT("SlateUI"), Dark);
	Material->SetTextureParameterValue(TEXT("Texture"), Dark);
	Material->SetScalarParameterValue(TEXT("OpacityMul"), 1.f);
	Material->SetScalarParameterValue(TEXT("DiffuseStrength"), 1.f);
	Material->SetScalarParameterValue(TEXT("AuthoredLuminosity"), 0.f);
	Material->SetScalarParameterValue(TEXT("EmissiveStrength"), 0.f);
	OutMaterial = Material;
	OutTexture = Dark;
	return true;
}



UStaticMesh* MakePanelMesh(UACEDatSubsystem& Dat, UObject* Outer, uint32 ModelId, float WorldScale, const FPanelLook& Look, TArray<float>* OutAreas, int32 OnlyPortalIndex, const TArray<PersonalFarWindows::EKind>* InKinds)
{
	const FACEBuiltSetupMesh* Built = Dat.FindSetupMesh(ModelId, WorldScale, 0);
	FACEDatTextureResolver* Textures = Dat.GetTextureResolver();
	if (!Built || !Textures)
	{
		return nullptr;
	}
	// The openings of a building are not in its drawn sections: the client's builder keeps them apart as
	// Part.Portals (FACESetupMeshBuilder, BSP PORT polygons). Each becomes a panel seen from both sides,
	// in the section of its kind (far windows: window, door, open entrance, flat).
	TArray<PersonalFarWindows::EKind> Kinds;
	OpeningKinds(*Built, WorldScale, Kinds);
	if (InKinds && InKinds->Num() == Kinds.Num())
	{
		Kinds = *InKinds;
	}
	FSection Panels[PersonalFarWindows::KindCount];
	uint32 WallSurface = 0;
	bool bWallWrap = false;
	int32 At = 0;
	for (const FACEBuiltSetupPart& Part : Built->Parts)
	{
		for (const FACEBuiltMeshSection& Section : Part.Sections)
		{
			// A plain wall surface, to find the client's outdoor material below.
			FACEDatSurfaceRenderFlags Decoded;
			if (WallSurface == 0 && !Section.bCollisionOnly && !Section.IsEmpty() && Section.SurfaceId != 0 && Textures->ResolveSurfaceRenderFlags(Section.SurfaceId, Decoded)
				&& !Decoded.bAdditive && !Decoded.bFullyTransparent && !Decoded.bClipMap && !Decoded.bUsesAlpha)
			{
				WallSurface = Section.SurfaceId;
				bWallWrap = Section.bWrapTexture;
			}
		}
		for (const FACEBuiltSetupPortal& Opening : Part.Portals)
		{
			if (Opening.Vertices.Num() < 3)
			{
				continue;
			}
			const PersonalFarWindows::EKind Kind = Kinds[At++];
			if (OnlyPortalIndex != INDEX_NONE && Opening.PortalIndex != OnlyPortalIndex)
			{
				continue;
			}
			if (OutAreas)
			{
				OutAreas->Add(OpeningArea(Opening.Vertices, WorldScale));
			}
			// An opening over 12 m2 is drawn with the open entrances (only the test colors give it a panel).
			FSection& Panel = Panels[Kind == PersonalFarWindows::EKind::None ? int32(PersonalFarWindows::EKind::Entrance) : int32(Kind)];
			const int32 Base = Panel.Vertices.Num();
			for (const FVector& Vertex : Opening.Vertices)
			{
				Panel.Vertices.Add(Part.BindTransform.TransformPosition(Vertex));
				Panel.Normals.Add(FVector::UpVector);
				Panel.UVs.Add(FVector2D::ZeroVector);
			}
			for (int32 I = 1; I + 1 < Opening.Vertices.Num(); ++I)
			{
				Panel.Triangles.Append({Base, Base + I, Base + I + 1, Base, Base + I + 1, Base + I});
			}
		}
	}
	TArray<const FSection*> Drawn;
	TArray<int32> DrawnKinds;
	for (int32 Kind = 0; Kind < PersonalFarWindows::KindCount; ++Kind)
	{
		if (Panels[Kind].Triangles.Num() > 0)
		{
			Drawn.Add(&Panels[Kind]);
			DrawnKinds.Add(Kind);
		}
	}
	if (Drawn.Num() == 0)
	{
		return nullptr;
	}
	if (Look.Materials.Num() != PersonalFarWindows::KindCount)
	{
		// One material per kind for every panel: the client's outdoor material (it takes the world's distance fog
		// from the shared parameter collection), each with its own 1 x 1 texture. All start as the same flat dark grey.
		UMaterialInstanceDynamic* Wall = WallSurface != 0 ? Cast<UMaterialInstanceDynamic>(Dat.GetOrCreateOutdoorLitMaterial(WallSurface, bWallWrap)) : nullptr;
		UMaterialInterface* FadingWall = Wall ? Dat.GetFarSceneryMaterial(Wall, /*bPreserveLighting*/ true) : nullptr;
		UMaterialInterface* Base = FadingWall ? FadingWall->GetMaterial() : nullptr;
		if (!Base)
		{
			return nullptr;
		}
		TArray<TObjectPtr<UMaterialInterface>> NewMaterials;
		TArray<TObjectPtr<UTexture2D>> NewTextures;
		for (int32 Kind = 0; Kind < PersonalFarWindows::KindCount; ++Kind)
		{
			TObjectPtr<UMaterialInterface> Material;
			TObjectPtr<UTexture2D> Dark;
			if (!MakePanelMaterial(Base, Outer, Material, Dark))
			{
				return nullptr;
			}
			NewMaterials.Add(Material);
			NewTextures.Add(Dark);
		}
		Look.Materials = MoveTemp(NewMaterials);
		Look.Textures = MoveTemp(NewTextures);
	}
	TArray<UMaterialInterface*> Materials;
	for (const int32 Kind : DrawnKinds)
	{
		Materials.Add(Look.Materials[Kind]);
	}
	return BuildMesh(Outer, Drawn, Materials);
}
}
