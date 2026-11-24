#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraComponent.h"

/**
 * Retail CameraSet / SmartBox / Render FOV constants.
 * Viewer offset is AC local (Y forward, Z up). WorldScale converts AC → cm.
 */
namespace ACECameraRetail
{
	/** SmartBox::m_fGameFOV — stored as 90° in radians, then divided by (aspect − 0.1). */
	constexpr float GameFovRadians = 1.5707964f;
	constexpr float DefaultPivotZAc = 1.5f;
	/** SmartBox::viewer_sphere used for the camera's cell/wall transition. */
	constexpr float ViewerRadiusAc = 0.3f;
	constexpr float DefaultOffsetYAc = -2.5f;
	constexpr float DefaultOffsetZAc = 0.75f;
	constexpr float InHeadOffsetYAc = 0.18000001f;
	constexpr float CloserMinLengthAc = 0.5f;
	constexpr float LeaveHeadYAc = -0.6f;
	constexpr float LeaveHeadZAc = 0.5f;
	constexpr float LookDownOffsetYAc = -2.0f;
	constexpr float LookDownOffsetZAc = 0.75f;
	/** CameraSet::SetMapMode uses -450 along the look-down camera frame. */
	constexpr float MapOffsetYAc = -450.f;
	/** CameraManager target direction while looking down: (0, 0.5, -1.8). */
	constexpr float LookDownDirY = 0.5f;
	constexpr float LookDownDirZ = -1.8f;
	constexpr float FartherMaxXYAc = 10.f;
	// CameraManager::UseTime: translation alpha = stiffness * dt * 10.
	constexpr float TranslationalStiffness = 0.45f;
	constexpr float TranslationLagSpeed = TranslationalStiffness * 10.f;
	constexpr float AdjustmentSpeed = 40.f;
	constexpr float ZoomRate = 0.2f;
	/** SpringArm defaults along −X; AC facing is pawn +Y, so boom yaw +90. */
	constexpr float BoomYawToFaceAcForward = 90.f;
	// CameraSet's angle is 8 degrees, multiplied by the adjustment speed (40).
	constexpr float NumpadOrbitDegreesPerSecond = 8.f * AdjustmentSpeed;
	// CameraSet::Rotate caps mouse speed at 1.5, applies HoldRun, then the
	// DAT TurnRight modifier supplies 1.5 rad/s (run contributes another 1.5).
	constexpr float MouseTurnRadiansPerSecond = 1.5f * 1.5f * 1.5f;
	inline float KeyboardPitchRate(float AdjustmentMultiplier, bool MouseTurning)
	{
		return NumpadOrbitDegreesPerSecond * AdjustmentMultiplier * (MouseTurning ? .25f : 1.f);
	}
	/** CameraSet::Raise/Lower translate by 0.2 units per call in look-down mode.
	 * Normalize the 60 Hz step to our shared keyboard/mouse orbit delta. */
	constexpr float LookDownUnitsPerOrbitDegree = 0.2f * 60.f / NumpadOrbitDegreesPerSecond;
	inline bool CanUseMapView(int32 CellId)
	{
		// A 450-unit exterior view cannot represent an interior cell's PVS.
		const uint32 Cell = uint32(CellId) & 0xffffu;
		return Cell > 0 && Cell < 0x100u;
	}
	/** Optional camera controls requested by the player: orbit at rest, follow on movement. */
	inline float FollowTurnDegrees(float BoomYaw, float DeltaSeconds, bool bMoving, bool bFaceCamera = false)
	{
		if (!bMoving) return 0.f;
		const float Angle = FMath::UnwindDegrees(BoomYaw - BoomYawToFaceAcForward);
		const float Limit = FMath::RadiansToDegrees(MouseTurnRadiansPerSecond) * FMath::Max(0.f, DeltaSeconds);
		return bFaceCamera ? FMath::Clamp(Angle, -Limit, Limit)
			: Angle * (1.f - FMath::Exp(-8.f * FMath::Max(0.f, DeltaSeconds)));
	}

	inline float HorizontalFovDegrees(float AspectWH, float GameFovDegrees = 90.f)
	{
		const float Aspect = FMath::Max(AspectWH, 0.2f);
		// Keep the retail lens through 3840x1600. On wider desktops hold that
		// vertical framing and let the extra width extend the horizontal view.
		constexpr float WidestRetailAspect = 3840.f / 1600.f;
		const float Denom = FMath::Max(FMath::Min(Aspect, WidestRetailAspect) - 0.1f, 0.2f);
		// Apply the preference before converting retail's vertical projection
		// to Unreal's horizontal FOV. Scaling the result changes the lens curve.
		const float VRad = FMath::DegreesToRadians(FMath::Clamp(GameFovDegrees / Denom, 1.f, 179.f));
		return FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(VRad * 0.5f) * Aspect));
	}

	inline void ApplyFovToCamera(UCameraComponent* Camera, int32 SizeX, int32 SizeY, float GameFovDegrees = 90.f,
		bool bMapView = false)
	{
		if (!Camera || SizeY <= 0 || SizeX <= 0)
		{
			return;
		}
		Camera->SetConstraintAspectRatio(false);
		// CameraSet::SetMapMode disables detail degradation, not the configured
		// lens. A wide FOV must not further degrade the already distant map scene.
		Camera->SetUseFieldOfViewForLOD(!bMapView);
		// The angle below already includes retail's viewport conversion. Do not
		// let LocalPlayer's MaintainYFOV policy apply another aspect conversion.
		Camera->bOverrideAspectRatioAxisConstraint = true;
		Camera->AspectRatioAxisConstraint = AspectRatio_MaintainXFOV;
		Camera->SetFieldOfView(HorizontalFovDegrees(static_cast<float>(SizeX) / static_cast<float>(SizeY), GameFovDegrees));
	}

	inline float ZoomMultiplier(bool bCloser, float DeltaSeconds, float AdjustmentMultiplier, float Steps = 1.f)
	{
		const float Change = AdjustmentSpeed * AdjustmentMultiplier * ZoomRate * FMath::Max(0.f, DeltaSeconds);
		return FMath::Pow(FMath::Max(0.f, 1.f + (bCloser ? -Change : Change)), Steps);
	}

	inline float MaximumArmLength(const FRotator& Rotation, float ScaleCm)
	{
		// CameraSet::Farther bounds each offset axis, not the vector's length.
		const FVector OffsetDirection = -Rotation.Vector();
		const float Horizontal = FMath::Max(FMath::Abs(OffsetDirection.X), FMath::Abs(OffsetDirection.Y));
		return FMath::Min(FartherMaxXYAc * ScaleCm / FMath::Max(Horizontal, .0001f),
			OffsetDirection.Z > 0.f ? 450.f * ScaleCm / OffsetDirection.Z : MAX_flt);
	}

	inline float TranslationAlpha(float DeltaSeconds, float StiffnessMultiplier)
	{
		const float Stiffness = FMath::Clamp(TranslationalStiffness * StiffnessMultiplier, 0.f, 1.f);
		return Stiffness >= .9998f ? 1.f : FMath::Clamp(Stiffness * DeltaSeconds * 10.f, 0.f, 1.f);
	}

	inline float OffsetLengthCm(float OffsetYAc, float OffsetZAc, float ScaleCm)
	{
		const float Y = OffsetYAc * ScaleCm;
		const float Z = OffsetZAc * ScaleCm;
		return FMath::Sqrt(Y * Y + Z * Z);
	}

	inline float PitchFromOffsetDegrees(float OffsetYAc, float OffsetZAc)
	{
		return FMath::RadiansToDegrees(FMath::Atan2(-OffsetZAc, FMath::Abs(OffsetYAc)));
	}

	inline float DefaultArmLengthCm(float ScaleCm)
	{
		return OffsetLengthCm(DefaultOffsetYAc, DefaultOffsetZAc, ScaleCm);
	}

	inline float DefaultPitchDegrees()
	{
		return PitchFromOffsetDegrees(DefaultOffsetYAc, DefaultOffsetZAc);
	}

	inline float LookDownPitchDegrees()
	{
		return FMath::RadiansToDegrees(FMath::Atan2(LookDownDirZ, LookDownDirY));
	}

	inline float InHeadForwardCm(float ScaleCm)
	{
		return InHeadOffsetYAc * ScaleCm;
	}
}
