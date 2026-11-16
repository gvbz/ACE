#pragma once

#include "CoreMinimal.h"
#include "Dat/ACEDatDatabase.h"
#include "Personal/PersonalFarWindows.h"

class UACEDatSubsystem;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UTexture2D;
struct FACEBuiltSetupMesh;

/**
 * Personal far scenery: the DAT's lower-detail models of an object type (GfxObj degrade tables),
 * as meshes for far objects. Level 0 is the full model, which the client builds itself; this is for levels 1 and 2.
 */
namespace PersonalFarSceneryMeshes
{
	/** Detail levels: 0 full, 1 middle, 2 simplest. */
	inline constexpr int32 LevelCount = 3;

	/** One drawn piece of a level: one surface, in the object's own space (parts already in place). */
	struct FSection
	{
		uint32 SurfaceId = 0;
		bool bWrapTexture = false;
		bool bClipMap = false;
		TArray<FVector> Vertices;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<int32> Triangles;
	};

	/** The lower levels of one object type. Read-only after it is built. */
	struct FLevels
	{
		/** Triangles per level. A level that is not simpler than the one before repeats that one's count. */
		int32 Triangles[LevelCount] = {};
		/** Geometry of levels 1 and 2, index Level - 1. Empty when the level is not simpler than the one before. */
		TArray<FSection> Sections[LevelCount - 1];

		/** The level to draw when Level is asked for: the nearest one at or below it that has its own, simpler geometry. 0 is the client's mesh. */
		int32 Effective(int32 Level) const
		{
			for (int32 L = FMath::Clamp(Level, 0, LevelCount - 1); L > 0; --L)
			{
				if (Sections[L - 1].Num() > 0)
				{
					return L;
				}
			}
			return 0;
		}
	};
	using FLevelsPtr = TSharedPtr<const FLevels, ESPMode::ThreadSafe>;

	/** Own handle to the portal DAT for workers. Its reads are locked. */
	struct FSource
	{
		FACEDatDatabase Portal;
	};
	using FSourcePtr = TSharedPtr<FSource, ESPMode::ThreadSafe>;
	/** Slow; call off the game thread. Null on failure. */
	FSourcePtr Open(const FString& DatDirectory);

	/** Build the lower levels of the given object types. Call off the game thread; one call at a time per source. */
	void Build(FSource& Source, const TArray<uint32>& SetupIds, float WorldScale, TMap<uint32, FLevelsPtr>& Out);

	/**
	 * Game thread: a static mesh of one level, with the materials the client's outdoor scenery uses.
	 * No collision. Null when nothing of it can be drawn. A copy of the mesh part of UACEDatSubsystem::GetOrCreateSetupStaticMesh.
	 */
	UStaticMesh* MakeStaticMesh(UACEDatSubsystem& Dat, UObject* Outer, const TArray<FSection>& Sections, TArray<UMaterialInterface*>& OutMaterials);

	/** The materials of the opening panels, one per kind (PersonalFarWindows::EKind), each with its own 1 x 1 texture. Owned by the far windows subsystem. */
	struct FPanelLook
	{
		TArray<TObjectPtr<UMaterialInterface>>& Materials;
		TArray<TObjectPtr<UTexture2D>>& Textures;
	};

	/**
	 * Game thread: panels for the openings of a building (far windows: door panels, and every opening in the test colors).
	 * The client's building mesh leaves those polygons out, because the rooms show behind them.
	 * Null when the building has no such openings. One section per kind of opening it has (PersonalFarWindows::EKind;
	 * an opening over 12 m2 in the open-entrance section), each with the material of that kind.
	 * Look: the materials all panels share, one per kind, made on first use.
	 * OutAreas: the area of every opening found, in square meters.
	 * OnlyPortalIndex: when set, only the openings with that index in the building's portal list (one mesh per opening).
	 * InKinds: the kinds to use, in model order (after the overrides); else the rule.
	 */
	UStaticMesh* MakePanelMesh(UACEDatSubsystem& Dat, UObject* Outer, uint32 ModelId, float WorldScale, const FPanelLook& Look, TArray<float>* OutAreas = nullptr,
		int32 OnlyPortalIndex = INDEX_NONE, const TArray<PersonalFarWindows::EKind>* InKinds = nullptr);
	/** Openings larger than this, in square meters, are of their own kind. */
	inline constexpr float PanelMaxArea = PersonalFarWindows::MaxArea;
	/** Area of an opening polygon in square meters. */
	float OpeningArea(const TArray<FVector>& Vertices, float WorldScale);
	/**
	 * The kind of every opening of a building, in the order of its parts and their portal polygons (those with 3 corners or more).
	 * OutOpenings: their sizes, in the same order.
	 */
	void OpeningKinds(const FACEBuiltSetupMesh& Built, float WorldScale, TArray<PersonalFarWindows::EKind>& OutKinds,
		TArray<PersonalFarWindows::FOpening>* OutOpenings = nullptr, const PersonalFarWindows::FDoorRule& Door = PersonalFarWindows::FDoorRule());
	/** A panel material: an instance of Parent (the parent of the client's outdoor lit material) with its own 1 x 1 texture. False when it cannot be made. */
	bool MakePanelMaterial(UMaterialInterface* Parent, UObject* Outer, TObjectPtr<UMaterialInterface>& OutMaterial, TObjectPtr<UTexture2D>& OutTexture);
	/** Rewrite the one pixel of a panel material's texture. */
	void SetPanelColor(UTexture2D* Texture, FColor Color);
}
