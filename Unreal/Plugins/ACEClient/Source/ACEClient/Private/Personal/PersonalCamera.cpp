#include "Personal/PersonalCamera.h"
#include "ACECameraRetail.h"
#include "ACERuntimeOptions.h"
#include "GameFramework/SpringArmComponent.h"
#include "Misc/ConfigCacheIni.h"

namespace PersonalCamera
{
	namespace
	{
		constexpr const TCHAR* Section = TEXT("personal.Camera");
		constexpr const TCHAR* DistanceKey = TEXT("Distance");
		constexpr const TCHAR* PitchKey = TEXT("Pitch");

		// Distance is in AC units so it does not depend on the world scale.
		float SeenDistance = -1.f, SeenPitch = 0.f;
		float SavedDistance = -1.f, SavedPitch = 0.f;
		float QuietSeconds = 0.f;

		bool Same(float A, float B) { return FMath::IsNearlyEqual(A, B, .001f); }
	}

	float PivotHeightAc(bool bInHead)
	{
		return bInHead ? ACECameraRetail::DefaultPivotZAc : ACERuntimeOptions::Get(TEXT("PersonalCameraHeight"));
	}

	void Restore(USpringArmComponent* Boom, float& UserArmLength, float ScaleCm)
	{
		if (!Boom || ScaleCm <= 0.f) return;
		float Distance = 0.f, Pitch = 0.f;
		if (GConfig
			&& GConfig->GetFloat(Section, DistanceKey, Distance, GGameUserSettingsIni)
			&& GConfig->GetFloat(Section, PitchKey, Pitch, GGameUserSettingsIni)
			&& FMath::IsFinite(Distance) && FMath::IsFinite(Pitch))
		{
			UserArmLength = Boom->TargetArmLength = FMath::Clamp(Distance, MinDistanceAc, MaxDistanceAc) * ScaleCm;
			FRotator Rotation = Boom->GetRelativeRotation();
			Rotation.Pitch = FMath::Clamp(Pitch, MinPitch, MaxPitch);
			Boom->SetRelativeRotation(Rotation);
		}
		// Start from the current position so a login alone does not write the file.
		SeenDistance = SavedDistance = UserArmLength / ScaleCm;
		SeenPitch = SavedPitch = Boom->GetRelativeRotation().Pitch;
		QuietSeconds = 0.f;
	}

	void Update(const USpringArmComponent* Boom, float UserArmLength, float ScaleCm, bool bSpecialView, float DeltaSeconds)
	{
		// First person, look-down and map views are not saved.
		if (!Boom || bSpecialView || ScaleCm <= 0.f || UserArmLength < 0.f || !GConfig) return;
		const float Distance = UserArmLength / ScaleCm;
		const float Pitch = Boom->GetRelativeRotation().Pitch;
		if (!Same(Distance, SeenDistance) || !Same(Pitch, SeenPitch))
		{
			SeenDistance = Distance;
			SeenPitch = Pitch;
			QuietSeconds = 0.f;
			return;
		}
		if (Same(Distance, SavedDistance) && Same(Pitch, SavedPitch)) return;
		QuietSeconds += FMath::Max(0.f, DeltaSeconds);
		if (QuietSeconds < SaveDelaySeconds) return;
		GConfig->SetFloat(Section, DistanceKey, Distance, GGameUserSettingsIni);
		GConfig->SetFloat(Section, PitchKey, Pitch, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
		SavedDistance = Distance;
		SavedPitch = Pitch;
	}
}
