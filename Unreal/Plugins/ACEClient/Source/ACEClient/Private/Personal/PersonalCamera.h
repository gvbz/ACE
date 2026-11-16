#pragma once

#include "CoreMinimal.h"

class USpringArmComponent;

/** Personal camera changes: adjustable height, optional free zoom limit, optional saved position. */
namespace PersonalCamera
{
	/** With Free Camera Restriction on: same zoom-out limit at every angle. Retail allows 450 only straight above. */
	constexpr float MaxDistanceAc = 450.f;
	constexpr float MinDistanceAc = 0.5f;
	constexpr float MinPitch = -89.f;
	constexpr float MaxPitch = 20.f;
	/** Seconds the camera must stay unchanged before its position is saved. */
	constexpr float SaveDelaySeconds = 1.f;

	/** Option rows (check boxes, off by default). */
	constexpr const TCHAR* SaveKey = TEXT("PersonalCameraSave");
	constexpr const TCHAR* FreeKey = TEXT("PersonalCameraFree");

	/** Save Camera Position: keep distance and pitch between sessions. */
	bool SavePosition();
	/** Free Camera Restriction: one zoom-out limit at every angle. */
	bool FreeZoom();

	/** Orbit pivot height above the feet, in AC units. First person keeps retail's height. */
	float PivotHeightAc(bool bInHead);

	/** Zoom-out limit: 450 units with Free Camera Restriction, else retail's limit for this angle. */
	float MaxArmLength(const FRotator& Rotation, float ScaleCm);

	/** Apply the saved distance and pitch after the login reset (Save Camera Position on). */
	void Restore(USpringArmComponent* Boom, float& UserArmLength, float ScaleCm);

	/** Call every frame. Saves distance and pitch once they stop changing (Save Camera Position on). */
	void Update(const USpringArmComponent* Boom, float UserArmLength, float ScaleCm, bool bSpecialView, float DeltaSeconds);
}
