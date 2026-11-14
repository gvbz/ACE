#include "Personal/PersonalFarTerrain.h"
#include "ACERuntimeOptions.h"
#include "Misc/ConfigCacheIni.h"

bool PersonalFarTerrain::IsOn()
{
	if (ACERuntimeOptions::Get(OptionKey) <= 0.5f)
	{
		return false;
	}
	// ACERuntimeOptions::Get raises a saved value to the row's minimum, so a saved 0
	// can only be seen in the settings file itself.
	float Saved = 1.f;
	const bool bSaved = GConfig && GConfig->GetFloat(TEXT("ACE.Presentation"), RadiusKey, Saved, GGameUserSettingsIni);
	return !(bSaved && FMath::IsFinite(Saved) && FMath::RoundToInt(Saved) <= 0);
}

int32 PersonalFarTerrain::Radius()
{
	// The option row limits the value to the slider's range.
	return FMath::RoundToInt(ACERuntimeOptions::Get(RadiusKey));
}
