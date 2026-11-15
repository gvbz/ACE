#include "Personal/PersonalFarScenery.h"
#include "Personal/PersonalFarTerrain.h"
#include "ACERuntimeOptions.h"
#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<float> CVarFarScenerySizeFactor(TEXT("personal.FarScenery.SizeFactor"), 400.f,
		TEXT("Far scenery draws an object out to height x scale x this factor. 400 is about 4 pixels tall at 3440x1440."));
	// The three settings of the options window, with a console variable each for measuring in one session.
	TAutoConsoleVariable<int32> CVarFarSceneryDraw(TEXT("personal.FarScenery.Draw"), -1,
		TEXT("-1 follows the Far Terrain Objects check box, 0 removes the far scenery, 1 draws it. A file Saved/Personal/FarSceneryOff does the same as 0."));
	TAutoConsoleVariable<int32> CVarFarSceneryRadius(TEXT("personal.FarScenery.Radius"), 0,
		TEXT("0 follows the Far Terrain Objects Radius slider, else the outermost ring of landblocks with far scenery, 2 to 8."));
	TAutoConsoleVariable<int32> CVarFarSceneryFogRule(TEXT("personal.FarScenery.FogRule"), -1,
		TEXT("-1 follows the Hide Far Terrain with Fog check box, 1 draws no far objects past the fog end, 0 draws them out to the radius whatever the fog."));
}

bool PersonalFarScenery::IsOn()
{
	const int32 Draw = CVarFarSceneryDraw.GetValueOnGameThread();
	return Draw != 0 && (Draw > 0 || ACERuntimeOptions::Get(OptionKey) > 0.5f) && PersonalFarTerrain::IsOn();
}

int32 PersonalFarScenery::Radius()
{
	const int32 Forced = CVarFarSceneryRadius.GetValueOnGameThread();
	// The option row limits the slider's value to its range.
	return FMath::Clamp(Forced != 0 ? Forced : FMath::RoundToInt32(ACERuntimeOptions::Get(RadiusKey)), NearRing, LastRing);
}

bool PersonalFarScenery::HideWithFog()
{
	const int32 Forced = CVarFarSceneryFogRule.GetValueOnGameThread();
	return Forced >= 0 ? Forced != 0 : ACERuntimeOptions::Get(FogKey) > 0.5f;
}

float PersonalFarScenery::SizeFactor()
{
	return FMath::Max(CVarFarScenerySizeFactor.GetValueOnGameThread(), 1.f);
}
