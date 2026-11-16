#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Personal/PersonalCamera.h"
#include "ACECameraRetail.h"
#include "ACERuntimeOptions.h"
#include "GameFramework/SpringArmComponent.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPersonalCameraTest, "ACE.Personal.Camera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FPersonalCameraTest::RunTest(const FString& Parameters)
{
	// Use a separate settings file: this test must not change player preferences.
	TGuardValue<FString> SettingsPath(GGameUserSettingsIni, FPaths::ProjectSavedDir() / TEXT("Automation/PersonalCameraFixture.ini"));
	FConfigFile FixtureConfig; FixtureConfig.NoSave = false; FixtureConfig.bCanSaveAllSections = true;
	GConfig->SetFile(GGameUserSettingsIni, &FixtureConfig);
	const float Scale = 100.f;
	const TCHAR* Section = TEXT("personal.Camera");
	float Stored = 0.f;

	TestEqual(TEXT("Height defaults to retail"), PersonalCamera::PivotHeightAc(false), ACECameraRetail::DefaultPivotZAc);
	ACERuntimeOptions::Set(TEXT("PersonalCameraHeight"), 3.f);
	TestEqual(TEXT("Height follows the slider"), PersonalCamera::PivotHeightAc(false), 3.f);
	ACERuntimeOptions::Set(TEXT("PersonalCameraHeight"), .5f);
	TestEqual(TEXT("Height does not go below retail"), PersonalCamera::PivotHeightAc(false), ACECameraRetail::DefaultPivotZAc);
	GConfig->SetFloat(TEXT("ACE.Presentation"), TEXT("PersonalCameraHeight"), .5f, GGameUserSettingsIni);
	TestEqual(TEXT("A saved height below retail reads as retail"), PersonalCamera::PivotHeightAc(false), ACECameraRetail::DefaultPivotZAc);
	ACERuntimeOptions::Set(TEXT("PersonalCameraHeight"), 3.f);
	TestEqual(TEXT("First person keeps retail height"), PersonalCamera::PivotHeightAc(true), ACECameraRetail::DefaultPivotZAc);

	// An unknown option reads as 1, so 0 also shows that the rows exist.
	TestEqual(TEXT("Save Camera Position row exists, off by default"), ACERuntimeOptions::Get(PersonalCamera::SaveKey), 0.f);
	TestEqual(TEXT("Free Camera Restriction row exists, off by default"), ACERuntimeOptions::Get(PersonalCamera::FreeKey), 0.f);
	TestFalse(TEXT("Save off by default"), PersonalCamera::SavePosition());
	TestFalse(TEXT("Free off by default"), PersonalCamera::FreeZoom());

	for (const float Pitch : {-89.f, -45.f, -20.f, 0.f})
	{
		const FRotator Rotation(Pitch, 90.f, 0.f);
		TestEqual(*FString::Printf(TEXT("Free off: retail zoom limit at pitch %.0f"), Pitch),
			PersonalCamera::MaxArmLength(Rotation, Scale), ACECameraRetail::MaximumArmLength(Rotation, Scale));
	}
	ACERuntimeOptions::Set(PersonalCamera::FreeKey, 1.f);
	for (const float Pitch : {-89.f, -45.f, -20.f, 0.f})
	{
		TestEqual(*FString::Printf(TEXT("Free on: 450 at pitch %.0f"), Pitch),
			PersonalCamera::MaxArmLength(FRotator(Pitch, 90.f, 0.f), Scale), 45000.f);
	}
	ACERuntimeOptions::Set(PersonalCamera::FreeKey, 0.f);

	// Save off: a saved position is not applied and nothing is written.
	GConfig->SetFloat(Section, TEXT("Distance"), 20.f, GGameUserSettingsIni);
	GConfig->SetFloat(Section, TEXT("Pitch"), -40.f, GGameUserSettingsIni);
	auto* OffBoom = NewObject<USpringArmComponent>();
	float OffArm = 261.f;
	OffBoom->SetRelativeRotation(FRotator(-17.f, 90.f, 0.f));
	PersonalCamera::Restore(OffBoom, OffArm, Scale);
	TestEqual(TEXT("Save off: login keeps the retail distance"), OffArm, 261.f);
	TestEqual(TEXT("Save off: login keeps the retail pitch"), float(OffBoom->GetRelativeRotation().Pitch), -17.f, .001f);
	for (int32 Frame = 0; Frame < 4; ++Frame) PersonalCamera::Update(OffBoom, 3000.f, Scale, false, .6f);
	GConfig->GetFloat(Section, TEXT("Distance"), Stored, GGameUserSettingsIni);
	TestEqual(TEXT("Save off: nothing written after the delay"), Stored, 20.f);

	// Turning Save on writes the current position, even if the camera has not moved.
	ACERuntimeOptions::Set(PersonalCamera::SaveKey, 1.f);
	PersonalCamera::Update(OffBoom, OffArm, Scale, false, .5f);
	PersonalCamera::Update(OffBoom, OffArm, Scale, false, .6f);
	GConfig->GetFloat(Section, TEXT("Distance"), Stored, GGameUserSettingsIni);
	TestEqual(TEXT("Turning Save on writes the current distance"), Stored, 2.61f);
	GConfig->GetFloat(Section, TEXT("Pitch"), Stored, GGameUserSettingsIni);
	TestEqual(TEXT("Turning Save on writes the current pitch"), Stored, -17.f, .001f);

	// Save on, Free off: a restored distance stays within the limit at the restored pitch.
	GConfig->SetFloat(Section, TEXT("Distance"), 450.f, GGameUserSettingsIni);
	GConfig->SetFloat(Section, TEXT("Pitch"), -17.f, GGameUserSettingsIni);
	auto* LimitBoom = NewObject<USpringArmComponent>();
	float LimitArm = 261.f;
	LimitBoom->SetRelativeRotation(FRotator(-40.f, 90.f, 0.f));
	PersonalCamera::Restore(LimitBoom, LimitArm, Scale);
	TestEqual(TEXT("Free off: restored pitch"), float(LimitBoom->GetRelativeRotation().Pitch), -17.f, .001f);
	TestEqual(TEXT("Free off: restored distance limited at the restored pitch"), LimitArm,
		ACECameraRetail::MaximumArmLength(LimitBoom->GetRelativeRotation(), Scale), .01f);
	TestTrue(TEXT("Free off: limit at a shallow angle is below 450"), LimitArm < 45000.f);

	// Save on and Free on: the saved position as before.
	ACERuntimeOptions::Set(PersonalCamera::FreeKey, 1.f);
	GConfig->EmptySection(Section, GGameUserSettingsIni);

	auto* Boom = NewObject<USpringArmComponent>();
	float Arm = 261.f;
	Boom->SetRelativeRotation(FRotator(-17.f, 90.f, 0.f));
	PersonalCamera::Restore(Boom, Arm, Scale);
	TestEqual(TEXT("Nothing saved leaves the reset position"), Arm, 261.f);

	Arm = 2000.f;
	Boom->SetRelativeRotation(FRotator(-40.f, 90.f, 0.f));
	PersonalCamera::Update(Boom, Arm, Scale, false, .1f);
	PersonalCamera::Update(Boom, Arm, Scale, false, .5f);
	TestFalse(TEXT("No save while the camera just moved"), GConfig->GetFloat(Section, TEXT("Distance"), Stored, GGameUserSettingsIni));
	PersonalCamera::Update(Boom, Arm, Scale, false, .6f);
	TestTrue(TEXT("Saved after the delay"), GConfig->GetFloat(Section, TEXT("Distance"), Stored, GGameUserSettingsIni));
	TestEqual(TEXT("Distance is saved in AC units"), Stored, 20.f);

	Boom->SetRelativeRotation(FRotator(-80.f, 90.f, 0.f));
	for (int32 Frame = 0; Frame < 5; ++Frame) PersonalCamera::Update(Boom, 300.f, Scale, true, 1.f);
	GConfig->GetFloat(Section, TEXT("Distance"), Stored, GGameUserSettingsIni);
	TestEqual(TEXT("Special views are not saved"), Stored, 20.f);

	auto* NextBoom = NewObject<USpringArmComponent>();
	float NextArm = 261.f;
	NextBoom->SetRelativeRotation(FRotator(-17.f, 90.f, 0.f));
	PersonalCamera::Restore(NextBoom, NextArm, Scale);
	TestEqual(TEXT("Next login restores distance"), NextArm, 2000.f);
	TestEqual(TEXT("Next login restores arm length"), NextBoom->TargetArmLength, 2000.f);
	TestEqual(TEXT("Next login restores pitch"), float(NextBoom->GetRelativeRotation().Pitch), -40.f, .001f);
	TestEqual(TEXT("Yaw is left alone"), float(NextBoom->GetRelativeRotation().Yaw), 90.f, .001f);

	GConfig->SetFloat(Section, TEXT("Distance"), 9999.f, GGameUserSettingsIni);
	GConfig->SetFloat(Section, TEXT("Pitch"), 75.f, GGameUserSettingsIni);
	PersonalCamera::Restore(NextBoom, NextArm, Scale);
	TestEqual(TEXT("Out-of-range distance is clamped"), NextArm, 45000.f);
	TestEqual(TEXT("Out-of-range pitch is clamped"), float(NextBoom->GetRelativeRotation().Pitch), PersonalCamera::MaxPitch, .001f);
	return true;
}
#endif
