#include "Personal/PersonalFarRoomComponent.h"
#include "StaticMeshSceneProxy.h"

namespace
{
	class FPersonalFarRoomSceneProxy final : public FStaticMeshSceneProxy
	{
	public:
		explicit FPersonalFarRoomSceneProxy(UStaticMeshComponent* Component)
			: FStaticMeshSceneProxy(Component, false)
		{
		}
		static SIZE_T TypeHash()
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}
		virtual SIZE_T GetTypeHash() const override
		{
			return TypeHash();
		}
		virtual bool CanBeOccluded() const override
		{
			return false;
		}
	};
}

FPrimitiveSceneProxy* UPersonalFarRoomComponent::CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite)
{
	// Far-room meshes are made at run time and are never Nanite.
	if (bCreateNanite)
	{
		return Super::CreateStaticMeshSceneProxy(NaniteMaterials, bCreateNanite);
	}
	return ::new FPersonalFarRoomSceneProxy(this);
}

bool UPersonalFarRoomComponent::HasOwnProxy() const
{
	return SceneProxy && SceneProxy->GetTypeHash() == FPersonalFarRoomSceneProxy::TypeHash();
}
