#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Personal/PersonalFog.h"
#include "Personal/PersonalFarTerrain.h"
#include "Personal/PersonalFarTerrainDetail.h"
#include "Personal/PersonalSkyHorizon.h"
#include "ACERuntimeOptions.h"
#include "HAL/IConsoleManager.h"

// Max Fog Distance, the fog values while it limits the fog, the horizon band with Far Terrain Horizon, and the texture rings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalFogLimitTest, "ACE.Personal.Fog.Limit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalFogLimitTest::RunTest(const FString& Parameters)
{
	constexpr float B = PersonalFog::BlockMeters;

	// The options are read from a settings file of the test's own, never the developer's.
	const FString PreviousIni = GGameUserSettingsIni;
	GGameUserSettingsIni = FPaths::ProjectSavedDir() / TEXT("Automation/FogPreferences.ini");
	FConfigFile Settings;
	Settings.NoSave = true;
	GConfig->SetFile(GGameUserSettingsIni, &Settings);
	GConfig->EmptySection(TEXT("ACE.Presentation"), GGameUserSettingsIni);
	ON_SCOPE_EXIT { GGameUserSettingsIni = PreviousIni; };

	// The option row: default 11, 5 to 43.
	const ACERuntimeOptions::FOption* Row = nullptr;
	for (const ACERuntimeOptions::FOption& O : ACERuntimeOptions::Values)
	{
		if (FCString::Strcmp(O.Key, PersonalFog::OptionKey) == 0)
		{
			Row = &O;
		}
	}
	if (!TestNotNull(TEXT("Option row PersonalMaxFogDistance"), Row))
	{
		return false;
	}
	TestEqual(TEXT("Label"), FString(Row->Label), FString(TEXT("Max Fog Distance")));
	TestEqual(TEXT("Default 11"), Row->Default, 11.f);
	TestEqual(TEXT("Min 5"), Row->Min, 5.f);
	TestEqual(TEXT("Max 43"), Row->Max, 43.f);
	TestEqual(TEXT("Nothing saved: 11"), PersonalFog::MaxLandblocks(), 11);
	TestTrue(TEXT("The default limits the fog"), PersonalFog::IsLimited());
	ACERuntimeOptions::Set(PersonalFog::OptionKey, 8.4f);
	TestEqual(TEXT("8.4 rounds to 8"), PersonalFog::MaxLandblocks(), 8);
	ACERuntimeOptions::Set(PersonalFog::OptionKey, 3.f);
	TestEqual(TEXT("Below the range: 5"), PersonalFog::MaxLandblocks(), 5);
	ACERuntimeOptions::Set(PersonalFog::OptionKey, 96.f);
	TestEqual(TEXT("Above the range: 43"), PersonalFog::MaxLandblocks(), 43);
	TestFalse(TEXT("43 does not limit the fog"), PersonalFog::IsLimited());

	// The fixed values, and no test variables left.
	TestEqual(TEXT("Clear-weather start 3"), PersonalFog::LimitedStartClearLandblocks, 3.f);
	TestEqual(TEXT("Fog amount 0.73"), PersonalFog::LimitedFogAmount, .73f);
	TestEqual(TEXT("Band opacity with Far Terrain Horizon 0.73"), PersonalFog::HorizonOpacityWithFarHorizon, .73f);
	for (const TCHAR* Name : {TEXT("personal.Fog.Clear"), TEXT("personal.Fog.Night"), TEXT("personal.Fog.Start"), TEXT("personal.Fog.StartClear"),
		TEXT("personal.Fog.StartNight"), TEXT("personal.Fog.Strength"), TEXT("personal.Fog.HorizonBand"), TEXT("personal.Fog.HorizonHeight"),
		TEXT("personal.Fog.HorizonOpacity")})
	{
		TestNull(*FString::Printf(TEXT("%s is gone"), Name), IConsoleManager::Get().FindConsoleVariable(Name));
	}

	// The fog end: at 43 as the client sets it; below 43 never past the limit, whatever the weather or time.
	constexpr float Coverage = 43.f * B;
	constexpr float Wide = 95.f * B;
	for (const float End : {300.f, 1500.f, 2400.f, 7000.f, 8200.f})
	{
		TestEqual(*FString::Printf(TEXT("43: end %.0f unchanged"), End), PersonalFog::LimitEnd(End, Coverage, 43), End);
	}
	TestEqual(TEXT("43 is no limit at radius 96"), PersonalFog::LimitEnd(9600.f, Wide, 43), 9600.f);
	TestEqual(TEXT("11 caps long clear fog"), PersonalFog::LimitEnd(9600.f, Coverage, 11), 11.f * B);
	TestEqual(TEXT("11 caps a 3000 m fog"), PersonalFog::LimitEnd(3000.f, Coverage, 11), 11.f * B);
	TestEqual(TEXT("5 caps a 1200 m fog"), PersonalFog::LimitEnd(1200.f, Coverage, 5), 5.f * B);
	TestEqual(TEXT("42 caps radius 96"), PersonalFog::LimitEnd(9600.f, Wide, 42), 42.f * B);
	TestEqual(TEXT("11 above an 800 m storm or night fog"), PersonalFog::LimitEnd(800.f, Coverage, 11), 800.f);
	TestEqual(TEXT("Never past the coverage"), PersonalFog::LimitEnd(9600.f, 10.f * B, 11), 10.f * B);
	TestEqual(TEXT("Far terrain off: end unchanged"), PersonalFog::LimitFogEnd(1234.f, 0, nullptr, FVector::ZeroVector, 100.f), 1234.f);

	// The fog start rule: the clear-weather start by day, the client's at night, blended at dusk and dawn.
	constexpr float Scale = 100.f;
	constexpr float End11 = 11.f * B * Scale;
	auto Start = [](float Client, float End, bool bClear, float Night, float ClearLB)
	{
		return PersonalFog::StartCm(Client, End, bClear, Night, ClearLB, 100.f);
	};
	for (const bool bClear : {true, false})
	{
		for (const float Night : {0.f, .5f, 1.f})
		{
			TestEqual(*FString::Printf(TEXT("Start 0 keeps the client's start (%s, night %.1f)"), bClear ? TEXT("Clear") : TEXT("Rainy"), Night),
				Start(12345.f, End11, bClear, Night, 0.f), 12345.f);
		}
	}
	TestEqual(TEXT("Start 3 by day in Clear"), Start(12345.f, End11, true, 0.f, 3.f), 3.f * B * Scale);
	TestEqual(TEXT("Start 3 can be nearer than the client's start"), Start(200000.f, End11, true, 0.f, 3.f), 3.f * B * Scale);
	TestEqual(TEXT("Start 3 not in Rainy"), Start(12345.f, End11, false, 0.f, 3.f), 12345.f);
	TestEqual(TEXT("Start 3 not at night"), Start(12345.f, End11, true, 1.f, 3.f), 12345.f);
	TestEqual(TEXT("Start 3 half in at dusk"), Start(12345.f, End11, true, .5f, 3.f), FMath::Lerp(3.f * B * Scale, 12345.f, .5f));
	TestEqual(TEXT("Start past the end stays below the end"), Start(12345.f, 2.f * B * Scale, true, 0.f, 3.f), 2.f * B * Scale - 1.f);

	// The rules UpdateFog uses, with the options of the test's settings file. No shell: the night amount is day (0).
	ACERuntimeOptions::Set(PersonalFog::OptionKey, 43.f);
	TestEqual(TEXT("At 43: the client's start in Clear"), PersonalFog::FogStartCm(12345.f, End11, 43, TEXT("Clear"), nullptr, Scale), 12345.f);
	TestEqual(TEXT("At 43: amount 1"), PersonalFog::FogAmount(43), 1.f);
	ACERuntimeOptions::Set(PersonalFog::OptionKey, 11.f);
	TestEqual(TEXT("At 11: start 3 in Clear by day"), PersonalFog::FogStartCm(12345.f, End11, 43, TEXT("Clear"), nullptr, Scale), 3.f * B * Scale);
	TestEqual(TEXT("At 11: start 3 in Sunny by day"), PersonalFog::FogStartCm(12345.f, End11, 43, TEXT("Sunny"), nullptr, Scale), 3.f * B * Scale);
	TestEqual(TEXT("At 11: the client's start in Rainy"), PersonalFog::FogStartCm(12345.f, End11, 43, TEXT("Rainy"), nullptr, Scale), 12345.f);
	TestEqual(TEXT("At 11: amount 0.73"), PersonalFog::FogAmount(43), .73f);
	TestEqual(TEXT("Far terrain off: the client's start"), PersonalFog::FogStartCm(12345.f, End11, 0, TEXT("Clear"), nullptr, Scale), 12345.f);
	TestEqual(TEXT("Far terrain off: amount 1"), PersonalFog::FogAmount(0), 1.f);

	// The horizon band: opacity 0.73 only while Far Terrain Horizon is on (which needs Far Terrain).
	TestEqual(TEXT("Band opacity 1 without Far Terrain Horizon"), PersonalFog::HorizonOpacity(), 1.f);
	ACERuntimeOptions::Set(PersonalSkyHorizon::OptionKey, 1.f);
	TestEqual(TEXT("Band opacity 1 with Far Terrain off"), PersonalFog::HorizonOpacity(), 1.f);
	ACERuntimeOptions::Set(PersonalFarTerrain::OptionKey, 1.f);
	TestTrue(TEXT("Far Terrain Horizon is on"), PersonalSkyHorizon::IsEnabled());
	TestEqual(TEXT("Band opacity 0.73 with Far Terrain Horizon"), PersonalFog::HorizonOpacity(), .73f);

	// The texture rings follow the limit: limit + 1, at most the 14 rings the texture covers.
	TestEqual(TEXT("MeshRings is 14"), PersonalFarTerrainDetail::MeshRings, 14);
	TestEqual(TEXT("Limit 5: 6 rings"), PersonalFog::TextureRings(5), 6);
	TestEqual(TEXT("Default 11: 12 rings"), PersonalFog::TextureRings(11), 12);
	TestEqual(TEXT("Limit 13: 14 rings"), PersonalFog::TextureRings(13), 14);
	TestEqual(TEXT("Limit 43: 14 rings"), PersonalFog::TextureRings(43), 14);
	const FIntPoint Center(0xA9, 0xB4);
	TestTrue(TEXT("14 rings: the texture's mesh cells"), PersonalFog::TextureCells(Center, 14) == PersonalFarTerrainDetail::MeshCells(Center));
	const FIntRect Six = PersonalFog::TextureCells(Center, 6);
	TestTrue(TEXT("6 rings start 6 landblocks before the center"), Six.Min == (Center - FIntPoint(6, 6)) * 8);
	TestTrue(TEXT("6 rings end after 6 landblocks past the center"), Six.Max == (Center + FIntPoint(7, 7)) * 8);
	return true;
}
#endif
