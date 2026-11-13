#pragma once
#include "CoreMinimal.h"

class UWorld;

namespace ACEClientBuild
{
	inline constexpr int32 ReleaseNumber = 157;
	inline constexpr const TCHAR* DesktopProductName = TEXT("AC:Unreal");
	inline constexpr const TCHAR* VRProductName = TEXT("AC:VR");
	ACECLIENT_API const TCHAR* ProductName(bool bVR = false);
	ACECLIENT_API void UpdateWindowTitle(UWorld* World, bool bVR = false);
	// One display version for desktop, PC VR, and standalone Quest.
	inline constexpr const TCHAR* Version = TEXT("2026.11.13.157");
}
