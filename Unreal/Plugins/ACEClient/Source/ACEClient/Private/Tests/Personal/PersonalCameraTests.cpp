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

	TestEqual(TEXT("Height defaults to retail"), PersonalCamera::PivotHeightAc(false), ACECameraRetail::DefaultPivotZAc);
	ACERuntimeOptions::Set(TEXT("PersonalCameraHeight"), 3.f);
	TestEqual(TEXT("Height follows the slider"), PersonalCamera::PivotHeightAc(false), 3.f);
	TestEqual(TEXT("First person keeps retail height"), PersonalCamera::PivotHeightAc(true), ACECameraRetail::DefaultPivotZAc);
	TestEqual(TEXT("Zoom limit does not depend on angle"), PersonalCamera::MaxArmLength(Scale), 45000.f);

	auto* Boom = NewObject<USpringArmComponent>();
	float Arm = 261.f;
	Boom->SetRelativeRotation(FRotator(-17.f, 90.f, 0.f));
	PersonalCamera::Restore(Boom, Arm, Scale);
	TestEqual(TEXT("Nothing saved leaves the reset position"), Arm, 261.f);

	Arm = 2000.f;
	Boom->SetRelativeRotation(FRotator(-40.f, 90.f, 0.f));
	PersonalCamera::Update(Boom, Arm, Scale, false, .1f);
	PersonalCamera::Update(Boom, Arm, Scale, false, .5f);
	float Stored = 0.f;
	TestFalse(TEXT("No save while the camera just moved"), GConfig->GetFloat(TEXT("personal.Camera"), TEXT("Distance"), Stored, GGameUserSettingsIni));
	PersonalCamera::Update(Boom, Arm, Scale, false, .6f);
	TestTrue(TEXT("Saved after the delay"), GConfig->GetFloat(TEXT("personal.Camera"), TEXT("Distance"), Stored, GGameUserSettingsIni));
	TestEqual(TEXT("Distance is saved in AC units"), Stored, 20.f);

	Boom->SetRelativeRotation(FRotator(-80.f, 90.f, 0.f));
	for (int32 Frame = 0; Frame < 5; ++Frame) PersonalCamera::Update(Boom, 300.f, Scale, true, 1.f);
	GConfig->GetFloat(TEXT("personal.Camera"), TEXT("Distance"), Stored, GGameUserSettingsIni);
	TestEqual(TEXT("Special views are not saved"), Stored, 20.f);

	auto* NextBoom = NewObject<USpringArmComponent>();
	float NextArm = 261.f;
	NextBoom->SetRelativeRotation(FRotator(-17.f, 90.f, 0.f));
	PersonalCamera::Restore(NextBoom, NextArm, Scale);
	TestEqual(TEXT("Next login restores distance"), NextArm, 2000.f);
	TestEqual(TEXT("Next login restores arm length"), NextBoom->TargetArmLength, 2000.f);
	TestEqual(TEXT("Next login restores pitch"), float(NextBoom->GetRelativeRotation().Pitch), -40.f, .001f);
	TestEqual(TEXT("Yaw is left alone"), float(NextBoom->GetRelativeRotation().Yaw), 90.f, .001f);

	GConfig->SetFloat(TEXT("personal.Camera"), TEXT("Distance"), 9999.f, GGameUserSettingsIni);
	GConfig->SetFloat(TEXT("personal.Camera"), TEXT("Pitch"), 75.f, GGameUserSettingsIni);
	PersonalCamera::Restore(NextBoom, NextArm, Scale);
	TestEqual(TEXT("Out-of-range distance is clamped"), NextArm, PersonalCamera::MaxArmLength(Scale));
	TestEqual(TEXT("Out-of-range pitch is clamped"), float(NextBoom->GetRelativeRotation().Pitch), PersonalCamera::MaxPitch, .001f);
	return true;
}
#endif
