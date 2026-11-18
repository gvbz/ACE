#include "ACERuntimeOptions.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/IConsoleManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/UserInterfaceSettings.h"
#include "Engine/World.h"
#include "ACEEnvCellActor.h"
#include "ACEDatSubsystem.h"
#include "ACETerrainPresenterComponent.h"
#include "Dat/ACEWorldTextureMipProvider.h"
#include "UObject/UObjectIterator.h"
namespace ACERuntimeOptions
{
namespace
{
 uint64 SoundSettingsFrame=MAX_uint64;
 uint64 PluginTextSettingsFrame=MAX_uint64;
 float CachedPluginTextScale=1.f;
 float AmbientGain=1.f,EffectsGain=1.f;
 int32 AppliedDynamicLightingQuality=2;
 bool bUniformDungeonLighting=false;
 bool bLegionsSky=false;
}
int32 DynamicLightingQuality() { return AppliedDynamicLightingQuality; }
bool UniformDungeonLighting() { return bUniformDungeonLighting; }
bool LegionsSky() { return bLegionsSky; }
float Get(const TCHAR* Key)
{
 for (const auto& O : Values) if (FCString::Strcmp(O.Key,Key)==0) {
  float V=O.Default;
  if (GConfig && !GConfig->GetFloat(TEXT("ACE.Presentation"),Key,V,GGameUserSettingsIni))
  {
   if (FCString::Strcmp(Key,TEXT("FieldOfViewDegrees"))==0)
   {
    // Older builds stored a multiplier. Keep the preference without silently
    // interpreting an existing value of 1 as a one-degree field of view.
    float Legacy=1.f;
    if(GConfig->GetFloat(TEXT("ACE.Presentation"),TEXT("FieldOfView"),Legacy,GGameUserSettingsIni)) V=90.f*Legacy;
   }
   else if (FCString::Strcmp(Key,TEXT("ScreenBrightness"))==0)
   {
    // Old values were gamma, not an output multiplier. Preserve the neutral
    // point and the preferred direction without turning 2.2 into 220% gain.
    float Legacy=2.2f;
    if(GConfig->GetFloat(TEXT("ACE.Presentation"),TEXT("Brightness"),Legacy,GGameUserSettingsIni) && FMath::IsFinite(Legacy))
     V=Legacy<2.2f?FMath::GetMappedRangeValueClamped(FVector2D(1.6f,2.2f),FVector2D(.2f,1.f),Legacy)
      :FMath::GetMappedRangeValueClamped(FVector2D(2.2f,2.8f),FVector2D(1.f,2.f),Legacy);
   }
  }
  return FMath::IsFinite(V)?FMath::Clamp(V,O.Min,O.Max):O.Default;
 }
 return 1;
}
void Set(const TCHAR* Key,float Value)
{
 if (!FMath::IsFinite(Value)) return;
 if (FCString::Strcmp(Key,TEXT("DesktopUIScale"))==0) Value=FMath::RoundToFloat(Value*4.f)/4.f;
 SoundSettingsFrame=MAX_uint64;
 PluginTextSettingsFrame=MAX_uint64;
 for (const auto& O : Values) if (FCString::Strcmp(O.Key,Key)==0 && GConfig)
  GConfig->SetFloat(TEXT("ACE.Presentation"),Key,FMath::Clamp(Value,O.Min,O.Max),GGameUserSettingsIni);
}
void Apply()
{
 bLegionsSky=Get(TEXT("LegionsSky"))>.5f;
 SoundSettingsFrame=MAX_uint64;
 PluginTextSettingsFrame=MAX_uint64;
 UACEWorldTextureMipProvider::RefreshQuality();
 for (TObjectIterator<UACETerrainPresenterComponent> It;It;++It)
  if (IsValid(*It) && It->GetWorld() && It->GetWorld()->IsGameWorld())
   It->ApplyLandscapeDrawRadius(FMath::RoundToInt(Get(TEXT("LandscapeRadius"))));
 const int32 LightingQuality=FMath::RoundToInt(Get(TEXT("DynamicLightingQuality")));
 if (auto* C=IConsoleManager::Get().FindConsoleVariable(TEXT("ace.FXLightBudget")))
  C->Set(FMath::RoundToInt(Get(TEXT("EffectLightLimit"))),ECVF_SetByGameSetting);
 const bool bUniform=Get(TEXT("UniformDungeonLighting"))>.5f;
 const bool bUniformChanged=bUniform!=bUniformDungeonLighting;
 if(bUniformChanged)
 {
  bUniformDungeonLighting=bUniform;
  for(TObjectIterator<UACEDatSubsystem> It;It;++It)
   if(IsValid(*It) && !It->IsTemplate()) It->RefreshDungeonLighting();
 }
 if(AppliedDynamicLightingQuality!=LightingQuality || bUniformChanged)
 {
  AppliedDynamicLightingQuality=LightingQuality;
  // Loaded rooms do not tick. Refresh only on preference changes; newly
  // loaded rooms apply the same preference when creating their lights.
  for(TObjectIterator<AACEEnvCellActor> It;It;++It)
   if(IsValid(*It) && It->GetWorld() && It->GetWorld()->IsGameWorld()) It->RefreshLightingOptions();
 }
 // Keep the existing neutral transfer function; brightness now adjusts the
 // final world image rather than bending gamma while leaving whites at full intensity.
 if (auto* C=IConsoleManager::Get().FindConsoleVariable(TEXT("r.TonemapperGamma"))) C->Set(2.2f,ECVF_SetByGameSetting);
 if (auto* C=IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Display.Brightness"))) C->Set(Get(TEXT("ScreenBrightness")),ECVF_SetByGameSetting);
 if (auto* C=IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Display.Contrast"))) C->Set(Get(TEXT("Contrast")),ECVF_SetByGameSetting);
 if (auto* C=IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaxAnisotropy"))) C->Set(FMath::RoundToInt(Get(TEXT("Anisotropy"))),ECVF_SetByGameSetting);
 if (GConfig) GConfig->Flush(false,GGameUserSettingsIni);
}
float SoundGain(bool bAmbient)
{
 // Every live emitter asks for this gain each tick. Read the shared settings
 // once per frame, with immediate invalidation when the player moves a slider.
 if (SoundSettingsFrame!=GFrameCounter)
 {
  SoundSettingsFrame=GFrameCounter;
  const bool Muted=Get(TEXT("ActiveSoundOnly"))>.5f && FSlateApplication::IsInitialized() && !FSlateApplication::Get().IsActive();
  const float Master=Muted ? 0.f : Get(TEXT("MasterVolume"));
  AmbientGain=Master*Get(TEXT("AmbientVolume"));EffectsGain=Master*Get(TEXT("EffectsVolume"));
 }
 return bAmbient?AmbientGain:EffectsGain;
}
uint32 ChatFontId()
{
 // gmClient exposes these five faces/sizes; ChatInterface changes existing
 // lines too. Do not substitute platform fonts (Quest has no Windows fonts).
 static constexpr uint32 Fonts[5][5]={
  {0x40000026,0x40000027,0x40000028,0x4000002E,0x4000002F}, // Arial
  {0x4000001E,0x4000001A,0x4000001F,0x40000030,0x40000031}, // Courier New
  {0x40000002,0x40000000,0x40000001,0x40000004,0x40000005}, // Palatino Linotype
  {0x40000008,0x40000009,0x4000000A,0x4000000B,0x4000000C}, // Tahoma
  {0x4000002A,0x4000002B,0x40000032,0x4000002C,0x4000002D}  // Times New Roman
 };
 return Fonts[FMath::Clamp(FMath::RoundToInt(Get(TEXT("ChatFontFace"))),0,4)]
  [FMath::Clamp(FMath::RoundToInt(Get(TEXT("ChatFontSize"))),0,4)];
}
float PluginTextScale()
{
 if(PluginTextSettingsFrame!=GFrameCounter)
 {
  PluginTextSettingsFrame=GFrameCounter;
  CachedPluginTextScale=Get(TEXT("PluginTextScale"));
 }
 return CachedPluginTextScale;
}
float DesktopUIScale(FIntPoint Size,bool bVR,float WindowDPIScale)
{
 if (bVR || Size.X<=0 || Size.Y<=0) return 1.f;
 // A small/unsupported display mode must still expose the complete interface
 // so Escape -> Options can recover it. Do not persist this emergency scale.
 const float MinimumFit=FMath::Min(Size.X/800.f,Size.Y/600.f);
 if (MinimumFit<1.f) return FMath::Max(.01f,MinimumFit);
 // Whole quarter steps, with room for the complete native 800x600 interface.
 const float Fit=FMath::Max(1.f,FMath::FloorToFloat(FMath::Min(Size.X/800.f,Size.Y/600.f)*4.f)/4.f);
 // SGameLayerManager cancels the window's native DPI. Include it here once
 // so all retail chrome, content and hit targets share the Windows scale.
 const float NativeScale=FMath::IsFinite(WindowDPIScale)?FMath::Clamp(WindowDPIScale,.5f,8.f):1.f;
 return FMath::Min(FMath::RoundToFloat(Get(TEXT("DesktopUIScale"))*4.f)/4.f*NativeScale,Fit);
}
void ApplyDesktopUIScale(FIntPoint Size,bool bVR,float WindowDPIScale)
{
 // Slate applies this once to the whole viewport, including child controls,
 // text rasterization and input coordinates. VR surfaces have their own scale.
 GetMutableDefault<UUserInterfaceSettings>()->ApplicationScale=DesktopUIScale(Size,bVR,WindowDPIScale);
}
}
