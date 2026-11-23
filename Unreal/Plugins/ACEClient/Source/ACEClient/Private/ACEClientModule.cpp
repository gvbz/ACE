#include "ACEClientModule.h"
#include "ACEClientBuild.h"
#include "ACEProjectMigration.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FACEClientModule"

void FACEClientModule::StartupModule()
{
	ACEProjectMigration::ImportWindowsProfile();
	// Use compatible CSM rendering; user scalability controls quality and can disable shadows.
	// PView (portal draw admission) is not CSM — keep cascaded shadows for lighting.
	auto ForceInt = [](const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	};
#if PLATFORM_WINDOWS || PLATFORM_LINUX
	// The ACE connection and UCM need world/controller ticks even while another
	// window is active. Idle mode skips them entirely, including route movement.
	ForceInt(TEXT("t.IdleWhenNotForeground"), 0);
#endif
	ForceInt(TEXT("r.Shadow.Virtual.Enable"), 0);
	ForceInt(TEXT("r.Shadow.CSM.Enable"), 1);
	// UE 5.8 RendererScene initializes PrimitiveOctree with UE_OLD_HALF_WORLD_MAX
	// (10.5 km). AC coordinates extend to 49 km; octree shadow gathering skips
	// Yaraq and most of Dereth. The flat gather retains per-primitive frustum and
	// distance culling, without changing authoritative AC/physics coordinates.
	ForceInt(TEXT("r.Shadow.UseOctreeForCulling"), 0);
	// Mobile receiver admission has a separate query against the same bounded
	// octree. Use the visible primitive list so distant-world terrain receives CSM.
	ForceInt(TEXT("r.Mobile.Shadow.CSMShaderCullingMethod"), 0);
	ForceInt(TEXT("r.Shadow.Enable"), 1);
	ForceInt(TEXT("r.DistanceFieldShadowing"), 0);
	ForceInt(TEXT("r.HeightFieldShadowing"), 0);
	ForceInt(TEXT("r.MegaLights.Allowed"), 0);
	ForceInt(TEXT("r.DynamicGlobalIlluminationMethod"), 0);
	ForceInt(TEXT("r.ReflectionMethod"), 0);
	ForceInt(TEXT("r.Lumen.DiffuseIndirect.Allow"), 0);
	ForceInt(TEXT("r.Lumen.Reflections.Allow"), 0);
	ForceInt(TEXT("r.AllowStaticLighting"), 0);
	// Outdoor doorway holes are world-space slab clips on building shells (not CustomStencil).
	ForceInt(TEXT("r.CustomDepth"), 3);

	UE_LOG(LogACEClient, Log,
		TEXT("ACEClient loaded (build %s - retail geometry, portal views, bitmap UI and weather)"), ACEClientBuild::Version);
	UE_LOG(LogTemp, Log,
		TEXT("ACEClient BUILD STAMP %s - retail geometry, portal views, bitmap UI and weather"), ACEClientBuild::Version);
}

void FACEClientModule::ShutdownModule()
{
}

DEFINE_LOG_CATEGORY(LogACEClient);

IMPLEMENT_MODULE(FACEClientModule, ACEClient)

#undef LOCTEXT_NAMESPACE
