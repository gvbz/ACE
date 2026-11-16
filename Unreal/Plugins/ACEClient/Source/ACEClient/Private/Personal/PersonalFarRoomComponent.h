#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "PersonalFarRoomComponent.generated.h"

/**
 * Personal far rooms: the static mesh component of a building's far rooms. Never occlusion culled, as the client's
 * own rooms (AACEEnvCellActor sets bDisableOcclusionQueries on its procedural mesh): a far room lies inside its
 * building and shows only through openings, so the engine's test against the previous frame flips for it.
 * UE 5.8 has no such switch on a static mesh component; its scene proxy says it cannot be occluded instead.
 */
UCLASS()
class UPersonalFarRoomComponent : public UStaticMeshComponent
{
	GENERATED_BODY()
public:
	virtual FPrimitiveSceneProxy* CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite) override;
	/** Its scene proxy is the own one (not Nanite, not the engine's). Game thread. */
	bool HasOwnProxy() const;
};
