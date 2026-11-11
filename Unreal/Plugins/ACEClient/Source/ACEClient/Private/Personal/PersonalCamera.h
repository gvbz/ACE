#pragma once

#include "CoreMinimal.h"

class USpringArmComponent;

/** Personal camera changes: adjustable height, one zoom limit, saved position. */
namespace PersonalCamera
{
	/** Same zoom-out limit at every angle. Retail allows 450 only straight above. */
	constexpr float MaxDistanceAc = 450.f;
	constexpr float MinDistanceAc = 0.5f;
	constexpr float MinPitch = -89.f;
	constexpr float MaxPitch = 20.f;
	/** Seconds the camera must stay unchanged before its position is saved. */
	constexpr float SaveDelaySeconds = 1.f;

	/** Orbit pivot height above the feet, in AC units. First person keeps retail's height. */
	float PivotHeightAc(bool bInHead);

	inline float MaxArmLength(float ScaleCm) { return MaxDistanceAc * ScaleCm; }

	/** Apply the saved distance and pitch after the login reset. */
	void Restore(USpringArmComponent* Boom, float& UserArmLength, float ScaleCm);

	/** Call every frame. Saves distance and pitch once they stop changing. */
	void Update(const USpringArmComponent* Boom, float UserArmLength, float ScaleCm, bool bSpecialView, float DeltaSeconds);
}
