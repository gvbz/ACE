#pragma once

#include "CoreMinimal.h"
#include "Dat/ACEDatDatabase.h"

class UACEDatSubsystem;
class UMaterialInterface;
class UStaticMesh;

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

	/**
	 * Game thread: dark panels for the door and window openings of a building. The client's building mesh leaves
	 * those polygons out, because the rooms show behind them; a far building has no rooms.
	 * Null when the building has no openings to fill. InOutMaterial: the one material all panels share, made on first use.
	 * OutAreas: the area of every opening found, in square meters, filled or not.
	 */
	UStaticMesh* MakePanelMesh(UACEDatSubsystem& Dat, UObject* Outer, uint32 ModelId, float WorldScale, TObjectPtr<UMaterialInterface>& InOutMaterial, TArray<float>* OutAreas = nullptr);
	/** Openings larger than this, in square meters, get no panel. */
	inline constexpr float PanelMaxArea = 12.f;
}
