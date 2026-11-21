#pragma once
#include "ACELoginProfile.h"

namespace ACELoginSettings
{
	FString GetPath();
	/** Uses Windows DPAPI, Android Keystore, or an owner-only Linux/macOS file with opt-in passwords. */
	bool Load(FACELoginSettings& Settings, const FString& Path = GetPath());
	bool Save(const FACELoginSettings& Settings, const FString& Path = GetPath());
}
