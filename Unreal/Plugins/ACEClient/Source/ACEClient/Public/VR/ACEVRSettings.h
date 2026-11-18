#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ACEVRSettings.generated.h"

/** Per-user settings, saved independently of retail character options. Distances are centimeters. */
UCLASS(Config = ACEVR, DefaultConfig)
class ACECLIENT_API UACEVRSettings : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(Config, EditAnywhere, Category = "VR") float EyeHeightOffset = 0.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bMatchCharacterHeight = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bEnabled = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bLeftHanded = false;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bSnapTurn = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float SnapDegrees = 30.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float SmoothTurnDegreesPerSecond = 60.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float StickDeadZone = .2f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float MovementScale = 1.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bRun = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float MovementSmoothing = .12f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float WristSmoothing = .07f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bPinMenuToView = false;
	// -1 migrates the legacy pin setting; 0 Head, 1 Body, 2 persistent World.
	UPROPERTY(Config) int32 MenuAnchorMode = -1;
	/** Minimum body-follow yaw radius; expands to include the inventory's visible edges. */
	UPROPERTY(Config, EditAnywhere, Category = "VR", meta = (ClampMin = "30", ClampMax = "120")) float MenuRecenterDegrees = 55.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bPinHotbarToView = false;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bShowWristSpellBar = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bShowCompass = true;
	UPROPERTY(Config) bool bShowFellowship = false;
	UPROPERTY(Config) bool bFellowshipLocked = true;
	UPROPERTY(Config) int32 FellowshipAnchorMode = 1;
	UPROPERTY(Config) FVector FellowshipViewOffset = FVector(120,-55,-5);
	UPROPERTY(Config) FRotator FellowshipViewRotation = FRotator::ZeroRotator;
	UPROPERTY(Config) float FellowshipScale = .07f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bPinVitalsToView = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bPinChatToView = false;
	UPROPERTY(Config) bool bChatLocked = true;
	UPROPERTY(Config) int32 ChatAnchorMode = 2;
	UPROPERTY(Config) FVector ChatViewOffset = FVector(115, -45, -10);
	UPROPERTY(Config) FRotator ChatViewRotation = FRotator::ZeroRotator;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bVitalsLocked = true;
	/** 0: view, 1: stable body frame, 2: world position until recentered. */
	UPROPERTY(Config, EditAnywhere, Category = "VR") int32 VitalsAnchorMode = 1;
	UPROPERTY(Config, EditAnywhere, Category = "VR") int32 CompassAnchorMode = 1;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bCompassLocked = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bMenuLocked = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bOptionsLocked = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") FVector CompassViewOffset = FVector(110, 38, -16);
	UPROPERTY(Config, EditAnywhere, Category = "VR") FVector MenuViewOffset = FVector(90, 0, -10);
	UPROPERTY(Config, EditAnywhere, Category = "VR") FVector VitalsViewOffset = FVector(100, -24, -22);
	UPROPERTY(Config) FRotator VitalsViewRotation = FRotator::ZeroRotator;
	UPROPERTY(Config) FRotator CompassViewRotation = FRotator::ZeroRotator;
	UPROPERTY(Config) FRotator MenuViewRotation = FRotator::ZeroRotator;
	UPROPERTY(Config) TMap<FName,FTransform> PanelLayouts;
	UPROPERTY(Config) TMap<FName,FName> ButtonBindings;
	UPROPERTY(Config) TMap<FName,FName> MenuButtonBindings;
	FName GetMenuButtonAction(FName Input) const;
	void SetMenuButtonAction(FName Input,FName Action);
	FName GetButtonAction(FName Input) const;
	void SetButtonAction(FName Input,FName Action);
	UPROPERTY(Config) int32 SettingsVersion = 0;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bHeadRelativeMovement = true;
	/** -1 imports the legacy preference; 0=head, 1=hand, 2=playspace/stick only. */
	UPROPERTY(Config, EditAnywhere, Category = "VR") int32 MovementDirection = -1;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bSeated = false;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float SeatedEyeHeight = 165.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bShowBody = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") bool bHaptics = true;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float PanelDistance = 90.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float PanelScale = .11f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float WristScale = .06f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float VitalsScale = .0935f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float CompassScale = .065f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float OptionsScale = .1f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float ChatScale = .0715f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float ForwardAssistDegrees = 10.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float MeleeMinSpeed = 250.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float BowFullDraw = 60.f;
	/** Natural cheek-side anchor, mirrored with handedness; centimeters. */
	UPROPERTY(Config, EditAnywhere, Category = "VR") float BowAnchorOffset = 10.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float HandPitch = 10.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") float RenderScale = 100.f;
	UPROPERTY(Config, EditAnywhere, Category = "VR") int32 MSAASamples = 4;
	static bool SupportsMSAASettings();
	void ApplyEdgeSmoothing() const;
	/** UE 5.8's HMD render-target scale, shared by OpenXR on PC and Android. */
	void ApplyRenderScale() const;
	void Sanitize();
	void Persist();
};
