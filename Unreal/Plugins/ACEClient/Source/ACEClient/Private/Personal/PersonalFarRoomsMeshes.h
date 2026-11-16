#pragma once

#include "CoreMinimal.h"
#include "Dat/ACEDatDatabase.h"

class FACEDatTextureResolver;
class FACEEnvCellMeshBuilder;
class UACEDatSubsystem;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Personal far rooms: the rooms of one building merged into one mesh, from the client's own room builder
 * (FACEEnvCellMeshBuilder::BuildEnvCell): the drawn walls, floors and ceilings with their baked light. No objects,
 * no collision. Built on a worker with own DAT handles; the client's room cache is not used, because the client
 * cancels and drops rooms outside its own set.
 */
namespace PersonalFarRoomsMeshes
{
	/** One drawn piece: one surface, in the building's own space. */
	struct FSection
	{
		uint32 SurfaceId = 0;
		bool bWrapTexture = false;
		TArray<FVector3f> Vertices;
		TArray<FVector3f> Normals;
		TArray<FVector2f> UVs;
		/** The baked light, as the client's room builder gives it. */
		TArray<FLinearColor> Colors;
		TArray<int32> Triangles;
		/** Index into FBuilt::Doors when the section belongs to a room with an outside doorway, else none. */
		int32 Door = INDEX_NONE;
	};

	/**
	 * The outside doorways of one room, as the client keeps them for its doorway shading (AACEEnvCellActor::LoadEnvCell):
	 * at most two; center and half width, normal and half height, in the building's space.
	 */
	struct FDoors
	{
		int32 Num = 0;
		FVector Center[2] = {FVector::ZeroVector, FVector::ZeroVector};
		FVector Normal[2] = {FVector::ZeroVector, FVector::ZeroVector};
		float HalfWidth[2] = {0.f, 0.f};
		float HalfHeight[2] = {0.f, 0.f};
	};

	/** The far rooms of one building. Read-only after it is built. */
	struct FBuilt
	{
		TArray<FSection> Sections;
		TArray<FDoors> Doors;
		/** Equal for buildings whose far rooms are the same: same surfaces, places and baked light. */
		uint32 Key = 0;
		int32 Rooms = 0;
		int32 Triangles = 0;
	};
	using FBuiltPtr = TSharedPtr<const FBuilt, ESPMode::ThreadSafe>;

	/** Own handles to the DAT files and an own room builder, for workers. One Build call at a time per source. */
	struct FSource
	{
		FSource();
		~FSource();
		FACEDatDatabase Portal;
		FACEDatDatabase Cell;
		TUniquePtr<FACEDatTextureResolver> Textures;
		TUniquePtr<FACEEnvCellMeshBuilder> Builder;
	};
	using FSourcePtr = TSharedPtr<FSource, ESPMode::ThreadSafe>;
	/** Slow; call off the game thread. Null on failure. */
	FSourcePtr Open(const FString& DatDirectory);

	/**
	 * Call off the game thread. Builds the rooms and merges them, one section per surface, in the building's space.
	 * BuildingToBlock: where the building stands in its landblock (the client's shell placement).
	 * Rooms that cannot be built are left out. Never null.
	 */
	FBuiltPtr Build(FSource& Source, TConstArrayView<uint32> RoomIds, const FTransform& BuildingToBlock, float WorldScale);

	/**
	 * Game thread: a static mesh of a building's far rooms, with the client's own room materials
	 * (UACEDatSubsystem::GetOrCreateEnvCellMaterial) and the baked light as vertex colors. No collision.
	 * Null when nothing of it can be drawn.
	 * OutSlotSections: for each material slot of the mesh, the section of Built it draws.
	 */
	UStaticMesh* MakeStaticMesh(UACEDatSubsystem& Dat, UObject* Outer, const FBuilt& Built, TArray<UMaterialInterface*>& OutMaterials, TArray<int32>* OutSlotSections = nullptr);

	/**
	 * Game thread: the client darkens a room near its outside doorways with a material instance of its own per room
	 * (parameters Door0, Door1, Normal0, Normal1, in world space). The same for a far-room component: its slots that
	 * draw such a room get own instances, copied from the shared ones. BuildingToWorld: where the component stands.
	 * OutMade: the instances, for the caller to keep.
	 */
	void ApplyDoorShading(UStaticMeshComponent& Component, const FBuilt& Built, TConstArrayView<int32> SlotSections, const FTransform& BuildingToWorld, TArray<UMaterialInstanceDynamic*>& OutMade);
}
