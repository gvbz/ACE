#pragma once
#include "CoreMinimal.h"
namespace ACERuntimeOptions
{
struct FOption { const TCHAR* Key; const TCHAR* Label; float Default, Min, Max; };
inline constexpr FOption Values[] = {
 {TEXT("MasterVolume"),TEXT("Master Volume"),1,0,1},
 {TEXT("EffectsVolume"),TEXT("Sound Effects"),1,0,1},
 {TEXT("AmbientVolume"),TEXT("Ambient Sounds"),1,0,1},
 {TEXT("ActiveSoundOnly"),TEXT("Play Sounds Only When Active"),1,0,1},
 {TEXT("CameraStiffness"),TEXT("Camera Stiffness"),1,.25f,3},
 {TEXT("CameraAdjustment"),TEXT("Camera Adjustment Speed"),1,.25f,3},
 {TEXT("FieldOfViewDegrees"),TEXT("Field of View"),90,10,160},
 {TEXT("PersonalCameraHeight"),TEXT("Camera Height"),1.5f,.5f,5}, // personal: camera
 {TEXT("ScreenBrightness"),TEXT("Screen Brightness"),1.f,.2f,2.f},
 {TEXT("Contrast"),TEXT("Screen Contrast"),1.f,.5f,2.f},
 {TEXT("Anisotropy"),TEXT("Texture Filtering"),16,1,16},
 {TEXT("DesktopUIScale"),TEXT("Desktop UI Scale"),1,1,3},
 {TEXT("ShowFrameRate"),TEXT("Show FPS overlay"),0,0,1},
 {TEXT("ChatFontFace"),TEXT("Chat Font"),2,0,4},
 {TEXT("ChatFontSize"),TEXT("Chat Font Size"),1,0,4},
 {TEXT("PluginTextScale"),TEXT("Lua Plugin Text Scale"),1,.75f,2},
 {TEXT("CursorScale"),TEXT("Cursor size"),1,.5f,3},
 {TEXT("ObjectGlow"),TEXT("Custom object highlighting"),1,0,1},
 {TEXT("ConstrainUIWindows"),TEXT("Keep UI windows on screen"),1,0,1},
 {TEXT("DynamicLightingQuality"),TEXT("Dynamic lights"),2,0,2},
 {TEXT("CameraAlignToSlope"),TEXT("Align camera to slope"),1,0,1},
 {TEXT("EffectLightLimit"),TEXT("Effect light limit"),8,0,64},
 {TEXT("LandscapeRadius"),TEXT("Landscape Radius"),5,5,25}
};
ACECLIENT_API float Get(const TCHAR* Key);
ACECLIENT_API void Set(const TCHAR* Key, float Value);
ACECLIENT_API void Apply();
/** Applied lighting preference: 0 off, 1 reduced, 2 full. No config reads per effect. */
ACECLIENT_API int32 DynamicLightingQuality();
ACECLIENT_API float SoundGain(bool bAmbient);
/** Retail EnumIDMap 25000012; actual glyph atlases from the user's DAT on every platform. */
ACECLIENT_API uint32 ChatFontId();
/** Shared by plugin labels and editable controls; at most one config read per frame. */
ACECLIENT_API float PluginTextScale();
ACECLIENT_API float DesktopUIScale(FIntPoint ViewportSize, bool bVR);
ACECLIENT_API void ApplyDesktopUIScale(FIntPoint ViewportSize, bool bVR);
}
