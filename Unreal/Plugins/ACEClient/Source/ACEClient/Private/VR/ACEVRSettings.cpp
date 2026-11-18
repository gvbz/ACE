#include "VR/ACEVRSettings.h"
#include "VR/ACEVRInputLayout.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/IConsoleManager.h"
#include "SceneUtils.h"
#include "RHI.h"

bool UACEVRSettings::SupportsMSAASettings()
{
	return GetDefaultAntiAliasingMethod(GMaxRHIFeatureLevel) == AAM_MSAA;
}

FName UACEVRSettings::GetButtonAction(FName Input) const
{
	const auto* Bound=ButtonBindings.Find(Input);
	return Bound && ACEVRInputLayout::IsAction(*Bound) ? *Bound : Input=="VRLeftGrip" ? FName("VRChat") : Input;
}
FName UACEVRSettings::GetMenuButtonAction(FName Input) const
{
	if(const auto* Bound=MenuButtonBindings.Find(Input))
		if(Bound->IsNone() || *Bound=="VRMenuUse" || *Bound=="VRMenuInspect")return *Bound;
	return Input=="VRSelect"?FName("VRMenuUse"):Input=="VRPrevious"?FName("VRMenuInspect"):NAME_None;
}
void UACEVRSettings::SetMenuButtonAction(FName Input,FName Action)
{
	if(!ACEVRInputLayout::IsAction(Input) || (!Action.IsNone() && Action!="VRMenuUse" && Action!="VRMenuInspect"))return;
	MenuButtonBindings.Add(Input,Action);Persist();
}
void UACEVRSettings::SetButtonAction(FName Input,FName Action)
{
	if (!ACEVRInputLayout::IsAction(Input) || !ACEVRInputLayout::IsAction(Action)) return;
	const FName Previous=GetButtonAction(Input);
	for(const auto& B:ACEVRInputLayout::Buttons())
		if(B.Input!=Input && GetButtonAction(B.Input)==Action){ButtonBindings.Add(B.Input,Previous);break;}
	ButtonBindings.Add(Input,Action);Persist();
}

void UACEVRSettings::ApplyEdgeSmoothing() const
{
	// The deferred PC renderer keeps its existing temporal AA. Forward OpenXR
	// renderers expose the sample count; 4x is the supported Quest quality default.
	if (SupportsMSAASettings())
		if (auto* Samples = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MSAACount")))
			// ProjectSetting outranks GameSetting, so the previous assignment was
			// ignored. A deliberate user choice must override the project default.
			Samples->Set(MSAASamples == 2 ? 2 : 4, ECVF_SetByCode);
}

void UACEVRSettings::ApplyRenderScale() const
{
	// UE 5.8 no longer registers vr.PixelDensity. Primary screen percentage is
	// also ineffective for a fixed-size mobile LDR eye buffer.
	if (auto* Scale = IConsoleManager::Get().FindConsoleVariable(TEXT("xr.SecondaryScreenPercentage.HMDRenderTarget")))
		Scale->Set(FMath::IsFinite(RenderScale) ? FMath::Clamp(RenderScale, 60.f, 150.f) : 100.f, ECVF_SetByGameSetting);
}

void UACEVRSettings::Sanitize()
{
	if (MenuAnchorMode < 0)
	{
		MenuAnchorMode = bPinMenuToView ? 0 : 2;
		if (const auto* Pose = PanelLayouts.Find("Menu"); Pose && !bPinMenuToView)
		{
			MenuViewOffset = Pose->GetLocation();
			MenuViewRotation = (Pose->GetRotation() * FRotator(0,180,0).Quaternion().Inverse()).Rotator();
		}
	}
	MenuAnchorMode = FMath::Clamp(MenuAnchorMode, 0, 2);
	bPinMenuToView = MenuAnchorMode == 0;
	if (MovementDirection < 0) MovementDirection = bHeadRelativeMovement ? 0 : 1;
	MovementDirection = FMath::Clamp(MovementDirection, 0, 2);
	if (SettingsVersion < 1)
	{
		if (FMath::IsNearlyEqual(WristScale, .045f)) WristScale = .06f;
		SettingsVersion = 1;
	}
	if (SettingsVersion < 2)
	{
		if (FMath::IsNearlyEqual(MovementScale, .65f)) MovementScale = 1.f;
		SettingsVersion = 2;
	}
	auto Clamp = [](float& V, float Default, float Low, float High)
	{ V = FMath::IsFinite(V) ? FMath::Clamp(V, Low, High) : Default; };
	Clamp(SnapDegrees, 30.f, 15.f, 90.f);
	Clamp(SmoothTurnDegreesPerSecond, 60.f, 15.f, 360.f);
	Clamp(StickDeadZone, .2f, .05f, .5f);
	Clamp(MovementScale, 1.f, .25f, 1.f);
	Clamp(MovementSmoothing, .12f, 0.f, .35f);
	Clamp(WristSmoothing, .07f, 0.f, .2f);
	Clamp(SeatedEyeHeight, 165.f, 100.f, 210.f);
	Clamp(EyeHeightOffset, 0.f, -30.f, 30.f);
	Clamp(PanelDistance, 90.f, 70.f, 180.f);
	Clamp(MenuRecenterDegrees, 55.f, 30.f, 120.f);
	Clamp(PanelScale, .11f, .05f, .15f);
	Clamp(WristScale, .06f, .03f, .085f);
	Clamp(VitalsScale, .0935f, .05f, .18f);
	Clamp(CompassScale, .065f, .04f, .15f);
	Clamp(OptionsScale, .1f, .06f, .15f);
	Clamp(ChatScale, .0715f, .04f, .15f);
	Clamp(ForwardAssistDegrees, 10.f, 0.f, 20.f);
	// Placement is a full 3D offset, not a rectangle projected at fixed depth.
	auto Placement=[](FVector& V, FRotator& R, const FVector& Default)
	{
		if (V.ContainsNaN()) V=Default;
		V=V.GetClampedToSize(35.,350.);
		if (R.ContainsNaN()) R=FRotator::ZeroRotator;
		R.Normalize();
	};
	Placement(MenuViewOffset,MenuViewRotation,FVector(PanelDistance,0,-10));
	Placement(CompassViewOffset,CompassViewRotation,FVector(110,38,-16));
	Placement(VitalsViewOffset,VitalsViewRotation,FVector(100,-24,-22));
	Placement(FellowshipViewOffset,FellowshipViewRotation,FVector(120,-55,-5));
	Placement(ChatViewOffset,ChatViewRotation,FVector(115,-45,-10));
	ChatAnchorMode=FMath::Clamp(ChatAnchorMode,0,2);
	Clamp(FellowshipScale,.07f,.04f,.15f);
	FellowshipAnchorMode=FMath::Clamp(FellowshipAnchorMode,0,2);
	CompassAnchorMode=FMath::Clamp(CompassAnchorMode,0,2);
	VitalsAnchorMode=FMath::Clamp(VitalsAnchorMode,0,2);
	Clamp(MeleeMinSpeed, 250.f, 250.f, 400.f);
	Clamp(BowFullDraw, 60.f, 30.f, 90.f);
	Clamp(BowAnchorOffset, 10.f, 0.f, 20.f);
	Clamp(HandPitch, 10.f, -30.f, 30.f);
	Clamp(RenderScale, 100.f, 60.f, 150.f);
	if (MSAASamples != 2 && MSAASamples != 4) MSAASamples = 4;
}

void UACEVRSettings::Persist()
{
	Sanitize();
	// Explicit generated config path: never overwrite DefaultACEVR.ini in a packaged game.
	const FString Filename = GConfig->GetConfigFilename(TEXT("ACEVR"));
	SaveConfig(CPF_Config, *Filename);
}
