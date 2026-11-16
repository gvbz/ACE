#pragma once

#include "CoreMinimal.h"
#include "Dat/ACEEnvCellMeshBuilder.h"

/** On-disk EnvCell draw/collision tiles (streamed indoors). */
namespace ACEEnvCellTileCache
{
	static constexpr uint32 SchemaVersion = 17u; // Include the placed DAT light points owned by this cell
	static constexpr uint32 Magic = 0x43454341u; // 'ACEC'

	FString MakeCacheFilePath(uint32 EnvCellId, float WorldScale, uint64 DatFingerprint);
	bool Save(const FString& Path, const FACEBuiltEnvCellMesh& Mesh, float WorldScale, uint64 DatFingerprint);
	bool Load(const FString& Path, FACEBuiltEnvCellMesh& OutMesh, float ExpectedWorldScale, uint64 ExpectedFingerprint);
}
