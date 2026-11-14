#include "UI/ACEVideoSettingsWidget.h"
#include "ACEHoverTooltipWidget.h"
#include "ACECameraSettings.h"
#include "ACEScreenshotSettings.h"
#include "Components/EditableTextBox.h"
#include "ACERuntimeOptions.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACEDesktopDisplay.h"
#include "ACECharacterOptions.h"
#include "Components/ScrollBox.h"
#include "Engine/GameInstance.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIResourceResolver.h"
#include "Blueprint/WidgetTree.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "UI/ACERetailComboBoxString.h"
#include "Components/CheckBox.h"
#include "Components/Slider.h"
#include "Engine/Texture2D.h"
#include "GameFramework/GameUserSettings.h"
#include "Styling/CoreStyle.h"
#include "Misc/ConfigCacheIni.h"
#include "Personal/PersonalOptionCheckBox.h" // personal: far-textures

TSharedRef<SWidget> UACEVideoSettingsWidget::RebuildWidget()
{
 if (Resolution) return Super::RebuildWidget();
 auto* Canvas = GetTypedOuter<UACEUICanvasWidget>();
 auto* Resources = Canvas ? Canvas->GetResourceResolver() : nullptr;
 auto Brush = [Resources](uint32 Did, FVector2D Size, bool bBorder = false)
 {
  FSlateBrush B; B.SetResourceObject(Resources ? Resources->ResolveTexture(Did) : nullptr);
  B.ImageSize=Size; B.DrawAs=bBorder ? ESlateBrushDrawType::Box : ESlateBrushDrawType::Image;
  if (bBorder) B.Margin=FMargin(2.f/Size.X,2.f/Size.Y);
  return B;
 };
 auto* Box=WidgetTree->ConstructWidget<UVerticalBox>();
 Scroll=WidgetTree->ConstructWidget<UScrollBox>(); Scroll->SetScrollBarVisibility(ESlateVisibility::Collapsed); Scroll->SetAllowOverscroll(false); Scroll->SetAnimateWheelScrolling(true); Scroll->AddChild(Box); WidgetTree->RootWidget=Scroll;
 auto Label=[&](const TCHAR* Text, int32 Size=8)
 {
  auto* L=WidgetTree->ConstructWidget<UACERetailTextBlock>();
  L->SetText(FText::FromString(Text)); L->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),Size));
  L->SetColorAndOpacity(FLinearColor::White); return L;
 };
 auto Fixed=[&](UWidget* Widget,float W,float H)
 {
  auto* S=WidgetTree->ConstructWidget<USizeBox>(); S->SetWidthOverride(W); S->SetHeightOverride(H); S->SetContent(Widget); return S;
 };
 auto Section=[&](const TCHAR* Name)
 {
  auto* Divider=WidgetTree->ConstructWidget<UImage>(); Divider->SetBrush(Brush(0x060012C5,FVector2D(292,8)));
  Box->AddChild(Fixed(Divider,272,8));
  auto* Title=Label(Name); Title->SetColorAndOpacity(FLinearColor::Yellow); Box->AddChild(Fixed(Title,272,22));
 };
 auto Row=[&](const TCHAR* Name,UWidget* Widget,float Height=20.f)
 {
  auto* R=WidgetTree->ConstructWidget<UHorizontalBox>();
  auto* Caption=Label(Name); Caption->SetJustification(ETextJustify::Right);
  R->AddChildToHorizontalBox(Fixed(Caption,130,14))->SetVerticalAlignment(VAlign_Center);
  R->AddChildToHorizontalBox(Fixed(Widget,120,18))->SetVerticalAlignment(VAlign_Center);
  Box->AddChild(Fixed(R,272,Height));
 };
 auto Combo=[&](const TCHAR* Name)
 {
  auto* C=WidgetTree->ConstructWidget<UACERetailComboBoxString>();
  FButtonStyle Button;
  const auto Background=Brush(0x060012B3,FVector2D(103,18),true);
  Button.SetNormal(Background).SetHovered(Background).SetPressed(Background);
  Button.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
  FComboBoxStyle Style=C->GetWidgetStyle();
  Style.ComboButtonStyle.SetButtonStyle(Button).SetDownArrowImage(Brush(0x060012B1,FVector2D(17,19)))
   .SetDownArrowPadding(FMargin(0)).SetContentPadding(FMargin(2,0,0,0)).SetMenuBorderBrush(Background).SetMenuBorderPadding(FMargin(2));
  C->SetWidgetStyle(Style); C->SetContentPadding(FMargin(0));
  auto Items=C->GetItemStyle();
  const auto Selected=Brush(0x060012B4,FVector2D(103,18),true);
  Items.SetEvenRowBackgroundBrush(Background).SetOddRowBackgroundBrush(Background)
   .SetEvenRowBackgroundHoveredBrush(Selected).SetOddRowBackgroundHoveredBrush(Selected)
   .SetActiveBrush(Selected).SetActiveHoveredBrush(Selected).SetInactiveBrush(Selected).SetInactiveHoveredBrush(Selected)
   .SetSelectorFocusedBrush(Selected).SetTextColor(FLinearColor::White).SetSelectedTextColor(FLinearColor::White);
  C->SetItemStyle(Items); C->OnGenerateWidgetEvent.BindDynamic(this,&UACEVideoSettingsWidget::GenerateOption);
  Row(Name,C); return C;
 };
 FSliderStyle RuntimeSlider;
 RuntimeSlider.SetNormalBarImage(Brush(0x06001285,FVector2D(120,12))).SetHoveredBarImage(RuntimeSlider.NormalBarImage).SetDisabledBarImage(RuntimeSlider.NormalBarImage)
  .SetNormalThumbImage(Brush(0x06001286,FVector2D(7,12))).SetHoveredThumbImage(RuntimeSlider.NormalThumbImage).SetDisabledThumbImage(RuntimeSlider.NormalThumbImage).SetBarThickness(4);
 FCheckBoxStyle RoundToggle;
 RoundToggle.SetUncheckedImage(Brush(0x06004D15,FVector2D(13,13))).SetUncheckedHoveredImage(RoundToggle.UncheckedImage).SetUncheckedPressedImage(Brush(0x06004D16,FVector2D(13,13)))
  .SetCheckedImage(Brush(0x06004D17,FVector2D(13,13))).SetCheckedHoveredImage(RoundToggle.CheckedImage).SetCheckedPressedImage(Brush(0x06004D18,FVector2D(13,13)));
 auto AddValue = [&](const TCHAR* Key) {
  // Resolve by key: adding a command-only option must not shift UI controls.
  const ACERuntimeOptions::FOption* Option=nullptr;
  for (const auto& Candidate:ACERuntimeOptions::Values)
   if (FCString::Strcmp(Candidate.Key,Key)==0) {Option=&Candidate;break;}
  if (!ensureMsgf(Option,TEXT("Unknown presentation option: %s"),Key)) return;
  const auto& O=*Option;
  auto* V=WidgetTree->ConstructWidget<USlider>(USlider::StaticClass(),O.Key); V->SetWidgetStyle(RuntimeSlider); V->SetMinValue(O.Min); V->SetMaxValue(O.Max); V->SetStepSize(.01f);
  V->SetValue(ACERuntimeOptions::Get(O.Key)); ValueSliders.Add(O.Key,V); Row(O.Label,V);
  if (FCString::Strcmp(O.Key,TEXT("FieldOfViewDegrees"))==0)
  {
   V->SetStepSize(1.f); V->OnValueChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeFieldOfView);
   FieldOfViewLabel=Label(TEXT(""),7); FieldOfViewLabel->SetJustification(ETextJustify::Right);
   Box->AddChild(Fixed(FieldOfViewLabel,250,14)); ChangeFieldOfView(V->GetValue());
  }
 };
 Section(TEXT("Sound Options"));
 for (const TCHAR* Key:{TEXT("MasterVolume"),TEXT("EffectsVolume"),TEXT("AmbientVolume")}) AddValue(Key);
 ActiveSoundOnly=WidgetTree->ConstructWidget<UCheckBox>(); ActiveSoundOnly->SetWidgetStyle(RoundToggle); ActiveSoundOnly->SetContent(Label(TEXT("Play Sounds Only When Active"))); Box->AddChild(Fixed(ActiveSoundOnly,272,20));
 Section(TEXT("Camera Options"));
 for (const TCHAR* Key:{TEXT("CameraStiffness"),TEXT("CameraAdjustment"),TEXT("PersonalCameraHeight"),TEXT("FieldOfViewDegrees")}) AddValue(Key); // personal: camera
 CameraAlignToSlope=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("CameraAlignToSlope"));
 CameraAlignToSlope->SetWidgetStyle(RoundToggle); CameraAlignToSlope->SetContent(Label(TEXT("Align camera to slope"))); Box->AddChild(Fixed(CameraAlignToSlope,272,20));
 Section(TEXT("Graphics Options"));
 DesktopScale=Combo(TEXT("Desktop UI scale"));
 DesktopScale->Rename(TEXT("DesktopUIScale"));
 for(int32 Percent=100;Percent<=300;Percent+=25) DesktopScale->AddOption(FString::Printf(TEXT("%d%%"),Percent));
 UACEHoverTooltipWidget::SetWidgetTooltip(DesktopScale, FText::FromString(TEXT("Scales the desktop interface in 25% steps. Limited to fit the window; VR uses its own panel scale. 200% and 300% give whole-pixel enlargement of native artwork.")));
 ConstrainUIWindows=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("ConstrainUIWindows"));
 ConstrainUIWindows->SetWidgetStyle(RoundToggle);ConstrainUIWindows->SetContent(Label(TEXT("Keep UI windows on screen")));Box->AddChild(Fixed(ConstrainUIWindows,272,20));
 UACEHoverTooltipWidget::SetWidgetTooltip(ConstrainUIWindows,FText::FromString(TEXT("Keep game and plugin windows inside the game interface. Turn off to allow dragging windows beyond its edges. Apply to bring offscreen windows back into view.")));
 CursorScale=WidgetTree->ConstructWidget<USlider>(USlider::StaticClass(),TEXT("CursorScale"));
 CursorScale->SetWidgetStyle(RuntimeSlider); CursorScale->SetMinValue(.5f); CursorScale->SetMaxValue(3.f); CursorScale->SetStepSize(.25f); CursorScale->MouseUsesStep=true;
 CursorScale->OnValueChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeCursorScale);
 Row(TEXT("Cursor size"),CursorScale);
 CursorScaleLabel=Label(TEXT("100%")); CursorScaleLabel->SetJustification(ETextJustify::Right); Box->AddChild(Fixed(CursorScaleLabel,250,14));
 UACEHoverTooltipWidget::SetWidgetTooltip(CursorScale, FText::FromString(TEXT("50% to 300% in 25% steps. Previews and saves immediately, including move, resize, and targeting pointers.")));
 ObjectGlow=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("ObjectGlow"));
 ObjectGlow->SetWidgetStyle(RoundToggle); ObjectGlow->SetContent(Label(TEXT("Custom object highlighting"))); Box->AddChild(Fixed(ObjectGlow,272,20));
 ObjectGlow->OnCheckStateChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeObjectGlow);
 UACEHoverTooltipWidget::SetWidgetTooltip(ObjectGlow,FText::FromString(TEXT("Glow on selected and hovered objects, including your own character. Turn off to remove the custom glow in desktop and VR. Selection and interaction still work. Changes apply and save immediately.")));
 ShowFrameRate=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("ShowFrameRate"));
 ShowFrameRate->SetWidgetStyle(RoundToggle);ShowFrameRate->SetContent(Label(TEXT("Show FPS overlay")));Box->AddChild(Fixed(ShowFrameRate,272,20));
 Box->AddChild(Fixed(UPersonalOptionCheckBox::Make(WidgetTree,RoundToggle,Label(TEXT("Far Terrain Textures")),TEXT("PersonalFarTerrainTextures"),TEXT("Draw the far terrain within about 2.7 km with the terrain textures. Off leaves it at flat colors. Changes apply and save immediately.")),272,20)); // personal: far-textures
 AddValue(TEXT("PersonalFarTerrainRadius")); ValueSliders[TEXT("PersonalFarTerrainRadius")]->SetStepSize(1.f); // personal: far-terrain
 Box->AddChild(Fixed(UPersonalOptionCheckBox::Make(WidgetTree,RoundToggle,Label(TEXT("Night Sky To Horizon")),TEXT("PersonalSkyHorizon"),TEXT("Continue the night sky below eye level, down to the far terrain. Removes the bright strip at the horizon from sunset to dawn. Changes apply and save immediately.")),272,20)); // personal: sky-horizon
 Resolution=Combo(TEXT("Resolution")); Resolution->Rename(TEXT("DisplayResolution"));
 Resolution->OnOpening.AddDynamic(this,&UACEVideoSettingsWidget::RefreshResolutions);
 UACEHoverTooltipWidget::SetWidgetTooltip(Resolution,FText::FromString(TEXT("Resolution for fullscreen and windowed modes. Borderless fills the desktop at its native resolution.")));
 WindowMode=Combo(TEXT("Display")); WindowMode->Rename(TEXT("DisplayMode")); for(const TCHAR* S:{TEXT("Fullscreen"),TEXT("Borderless"),TEXT("Windowed")})WindowMode->AddOption(S);
 FrameLimit=Combo(TEXT("Frame limit")); for(const TCHAR* S:{TEXT("Unlimited"),TEXT("30"),TEXT("60"),TEXT("120"),TEXT("144"),TEXT("240")})FrameLimit->AddOption(S);
 VSync=WidgetTree->ConstructWidget<UCheckBox>();
 FCheckBoxStyle Toggle;
 Toggle.SetUncheckedImage(Brush(0x06004D15,FVector2D(13,13))).SetUncheckedHoveredImage(Brush(0x06004D15,FVector2D(13,13)))
  .SetUncheckedPressedImage(Brush(0x06004D16,FVector2D(13,13))).SetCheckedImage(Brush(0x06004D17,FVector2D(13,13)))
  .SetCheckedHoveredImage(Brush(0x06004D17,FVector2D(13,13))).SetCheckedPressedImage(Brush(0x06004D18,FVector2D(13,13)))
  .SetPadding(FMargin(3,0));
 VSync->SetWidgetStyle(Toggle); VSync->SetContent(Label(TEXT("Sync with Refresh Rate"))); Box->AddChild(Fixed(VSync,272,20));
 AddValue(TEXT("ScreenBrightness"));
 auto* Brightness=ValueSliders[TEXT("ScreenBrightness")].Get();
 Brightness->OnValueChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeScreenBrightness);
 ScreenBrightnessLabel=Label(TEXT(""),7); ScreenBrightnessLabel->SetJustification(ETextJustify::Right);
 Box->AddChild(Fixed(ScreenBrightnessLabel,250,14));
 UACEHoverTooltipWidget::SetWidgetTooltip(Brightness,FText::FromString(TEXT("Dims or brightens the world, including glowing effects. 100% is neutral. Changes save immediately.")));
 AddValue(TEXT("Contrast"));
 auto* Contrast=ValueSliders[TEXT("Contrast")].Get();
 Contrast->OnValueChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeScreenContrast);
 ScreenContrastLabel=Label(TEXT(""),7); ScreenContrastLabel->SetJustification(ETextJustify::Right);
 Box->AddChild(Fixed(ScreenContrastLabel,250,14));
 UACEHoverTooltipWidget::SetWidgetTooltip(Contrast,FText::FromString(TEXT("Adjusts the difference between light and dark. 100% is neutral. Changes save immediately.")));
 Section(TEXT("Rendering Quality Options"));
 Quality=Combo(TEXT("Graphics quality")); for(const TCHAR* S:{TEXT("Low"),TEXT("Medium"),TEXT("High"),TEXT("Epic"),TEXT("Cinematic"),TEXT("Custom")})Quality->AddOption(S);
 Quality->Rename(TEXT("GraphicsQuality"));
 static const TCHAR* QualityNames[] = {TEXT("Texture Detail"),TEXT("Landscape Draw Distance"),TEXT("Shadows"),TEXT("Effects"),TEXT("Post Processing"),TEXT("Anti-Aliasing"),TEXT("Foliage"),TEXT("Shading")};
 for (const TCHAR* Name : QualityNames) {
  auto* C=Combo(Name);
  if(FCString::Strcmp(Name,TEXT("Texture Detail"))==0)
  {
   C->Rename(TEXT("TextureQuality"));
   UACEHoverTooltipWidget::SetWidgetTooltip(C,FText::FromString(TEXT("Low uses quarter-resolution world textures; Medium uses half resolution. High and above preserve full DAT detail. Applies to loaded terrain, objects and effects without restarting. Interface artwork stays sharp.")));
  }
  if(FCString::Strcmp(Name,TEXT("Shadows"))==0)
  {
   C->Rename(TEXT("ShadowQuality"));
   for(const TCHAR* V:{TEXT("Off"),TEXT("Low"),TEXT("Medium"),TEXT("High"),TEXT("Cinematic")}) C->AddOption(V);
   UACEHoverTooltipWidget::SetWidgetTooltip(C,FText::FromString(TEXT("Off removes dynamic shadows. Low and Medium reduce shadow distance and detail. Turn off for the lowest GPU cost.")));
  }
  else for(const TCHAR* V:{TEXT("Low"),TEXT("Medium"),TEXT("High"),TEXT("Epic"),TEXT("Cinematic")}) C->AddOption(V);
  QualityLevels.Add(C);
 }
 DynamicLighting=Combo(TEXT("Dynamic lights")); DynamicLighting->Rename(TEXT("DynamicLightingQuality"));
 for(const TCHAR* V:{TEXT("Off"),TEXT("Low"),TEXT("Full")}) DynamicLighting->AddOption(V);
 UACEHoverTooltipWidget::SetWidgetTooltip(DynamicLighting,FText::FromString(TEXT("Off removes light cast by spells, weapons and doorways. Low keeps up to two nearby effect lights. Particles, sunlight and ambient room illumination remain visible. Saved with Apply.")));
 Box->AddChild(Fixed(Label(TEXT("Older hardware: turn shadows and dynamic lights off."),7),272,18));
 AddValue(TEXT("EffectLightLimit"));
 auto* LightLimit=ValueSliders[TEXT("EffectLightLimit")].Get();
 LightLimit->SetStepSize(1.f); LightLimit->MouseUsesStep=true;
 LightLimit->OnValueChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeEffectLightLimit);
 EffectLightLimitLabel=Label(TEXT(""),7); EffectLightLimitLabel->SetJustification(ETextJustify::Right);
 Box->AddChild(Fixed(EffectLightLimitLabel,250,14));
 UACEHoverTooltipWidget::SetWidgetTooltip(LightLimit,FText::FromString(TEXT("Maximum simultaneous desktop weapon, portal and spell lights. More lights reduce switching in crowds but cost more GPU time. Default: 8. Dynamic lights Off disables them; Low caps this at 2. Saved with Apply.")));
 Filtering=Combo(TEXT("Texture Filtering")); for (const TCHAR* V:{TEXT("1x"),TEXT("2x"),TEXT("4x"),TEXT("8x"),TEXT("16x")}) Filtering->AddOption(V);
 Section(TEXT("Input Options"));
 InvertMouseX=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("InvertMouseX"));
 InvertMouseY=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("InvertMouseY"));
 InvertMouseX->SetWidgetStyle(RoundToggle);InvertMouseX->SetContent(Label(TEXT("Invert mouse X (horizontal look)")));Box->AddChild(Fixed(InvertMouseX,272,20));
 InvertMouseY->SetWidgetStyle(RoundToggle);InvertMouseY->SetContent(Label(TEXT("Invert mouse Y (vertical look)")));Box->AddChild(Fixed(InvertMouseY,272,20));
 ToggleMouseLook=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("ToggleMouseLook"));
 ToggleMouseLook->SetWidgetStyle(RoundToggle);ToggleMouseLook->SetContent(Label(TEXT("Toggle mouse look with right-click")));Box->AddChild(Fixed(ToggleMouseLook,272,20));
 MouseTurnSpeed=WidgetTree->ConstructWidget<USlider>(USlider::StaticClass(),TEXT("MouseTurnSpeed"));
 FSliderStyle Slider;
 Slider.SetNormalBarImage(Brush(0x06001285,FVector2D(120,12))).SetHoveredBarImage(Slider.NormalBarImage).SetDisabledBarImage(Slider.NormalBarImage)
  .SetNormalThumbImage(Brush(0x06001286,FVector2D(7,12))).SetHoveredThumbImage(Slider.NormalThumbImage).SetDisabledThumbImage(Slider.NormalThumbImage).SetBarThickness(4);
 MouseTurnSpeed->SetWidgetStyle(Slider); MouseTurnSpeed->SetMinValue(ACECameraSettings::MinMouseTurnSpeed); MouseTurnSpeed->SetMaxValue(ACECameraSettings::MaxMouseTurnSpeed);
 MouseTurnSpeed->SetValue(ACECameraSettings::GetMouseTurnSpeed()); MouseTurnSpeed->SetStepSize(.05f);
 MouseTurnSpeed->OnValueChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeMouseTurnSpeed);
 Row(TEXT("Mouse Turn Speed"),MouseTurnSpeed);
 auto* Ends=Label(TEXT("Slow                         Fast"),7); Ends->SetJustification(ETextJustify::Right); Box->AddChild(Fixed(Ends,250,14));
 TArray<const FACECharacterOptionDesc*> Options; ACECharacterOptions::GetPageOptions(ACECharacterOptions::PageConfig,Options);
 for (const auto* O:Options) {
  auto* C=WidgetTree->ConstructWidget<UCheckBox>(); C->SetWidgetStyle(RoundToggle); C->SetContent(Label(O->Label)); Box->AddChild(Fixed(C,272,20)); CharacterChecks.Add(O->Option,C);
 }
 Section(TEXT("User Interface Options"));
 ChatFontFace=Combo(TEXT("Chat Font")); ChatFontFace->Rename(TEXT("ChatFontFace"));
 for(const TCHAR* Name:{TEXT("Arial"),TEXT("Courier New"),TEXT("Palatino Linotype"),TEXT("Tahoma"),TEXT("Times New Roman")}) ChatFontFace->AddOption(Name);
 ChatFontSize=Combo(TEXT("Chat Font Size")); ChatFontSize->Rename(TEXT("ChatFontSize"));
 for(const TCHAR* Name:{TEXT("Tiny"),TEXT("Small"),TEXT("Medium"),TEXT("Large"),TEXT("Extra Large")}) ChatFontSize->AddOption(Name);
 PluginTextScale=Combo(TEXT("Lua Plugin Text Scale")); PluginTextScale->Rename(TEXT("PluginTextScale"));
 for(int32 Percent=75;Percent<=200;Percent+=25)PluginTextScale->AddOption(FString::Printf(TEXT("%d%%"),Percent));
 UACEHoverTooltipWidget::SetWidgetTooltip(PluginTextScale,FText::FromString(TEXT("Scales plugin labels, buttons and editable text independently of chat. Applies to bundled and third-party Lua plugins, including open windows. Saved with Apply.")));
 Section(TEXT("Screenshots"));
 Box->AddChild(Fixed(Label(TEXT("Save screenshots to:")),272,18));
 ScreenshotDirectory=WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(),TEXT("ScreenshotDirectory"));
 auto ScreenshotStyle=ScreenshotDirectory->GetWidgetStyle();
 ScreenshotStyle.SetTextStyle(FTextBlockStyle(ScreenshotStyle.TextStyle).SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),11)).SetColorAndOpacity(FLinearColor::White));
 ScreenshotStyle.SetForegroundColor(FLinearColor::White).SetFocusedForegroundColor(FLinearColor::White)
  .SetBackgroundColor(FLinearColor(.025f,.035f,.05f,1.f)).SetPadding(FMargin(6,4));
 FSlateBrush PathBackground; PathBackground.DrawAs=ESlateBrushDrawType::Box; PathBackground.TintColor=FLinearColor::White;
 ScreenshotStyle.SetBackgroundImageNormal(PathBackground).SetBackgroundImageHovered(PathBackground).SetBackgroundImageFocused(PathBackground);
 ScreenshotDirectory->SetWidgetStyle(ScreenshotStyle);
 ScreenshotDirectory->SetHintText(FText::FromString(ACEScreenshotSettings::DefaultDirectory()));
 UACEHoverTooltipWidget::SetWidgetTooltip(ScreenshotDirectory, FText::FromString(TEXT("Screenshot folder. Leave blank to use the default. Use Apply to save changes.")));
 Box->AddChild(Fixed(ScreenshotDirectory,272,32));
 Quality->OnSelectionChanged.AddDynamic(this,&UACEVideoSettingsWidget::ChangeQuality);
 ResetVideo();
 return Super::RebuildWidget();
}
void UACEVideoSettingsWidget::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
 Super::NativeTick(Geometry,DeltaTime);
 auto* S=UGameUserSettings::GetGameUserSettings();
 if (!S || !Resolution || !WindowMode) return;
 // Alt+Enter changes only the display controls; preserve unapplied quality edits.
 const auto R=S->GetScreenResolution(); const int32 Mode=int32(S->GetFullscreenMode());
 if (Mode!=ObservedWindowMode || R!=ObservedResolution)
 {
  ObservedWindowMode=Mode; ObservedResolution=R;
  WindowMode->SetSelectedIndex(Mode);
  RefreshResolutions();
  const FString Value=FString::Printf(TEXT("%d x %d"),R.X,R.Y);
  if(Resolution->FindOptionIndex(Value)<0)Resolution->AddOption(Value);
  Resolution->SetSelectedOption(Value);
 }
}
void UACEVideoSettingsWidget::RefreshResolutions()
{
 if(!Resolution)return;
 const FString Selected=Resolution->GetSelectedOption();
 Resolution->ClearOptions();
 for(const auto& Size:FACEDesktopDisplay::GetAvailableResolutions(Cast<AACEPlayerController>(GetOwningPlayer())))
  Resolution->AddOption(FString::Printf(TEXT("%d x %d"),Size.X,Size.Y));
 // Refresh when reopening after moving between monitors or hotplugging one.
 // Keep the user's unapplied selection, including a custom window size.
 if(!Selected.IsEmpty())
 {
  if(Resolution->FindOptionIndex(Selected)<0)Resolution->AddOption(Selected);
  Resolution->SetSelectedOption(Selected);
 }
}
void UACEVideoSettingsWidget::ResetVideo()
{
 TGuardValue<bool> Loading(LoadingSettings,true);
 auto* S=UGameUserSettings::GetGameUserSettings();if(!S||!Resolution)return;
 RefreshResolutions();
 const auto R=S->GetScreenResolution(); ObservedResolution=R; ObservedWindowMode=int32(S->GetFullscreenMode()); const FString Value=FString::Printf(TEXT("%d x %d"),R.X,R.Y);
 if(Resolution->FindOptionIndex(Value)<0)Resolution->AddOption(Value);Resolution->SetSelectedOption(Value);
 WindowMode->SetSelectedIndex(int32(S->GetFullscreenMode()));
 const int32 Q=S->GetOverallScalabilityLevel();Quality->SetSelectedIndex(Q>=0?Q:5);
 const int32 FPS=FMath::RoundToInt(S->GetFrameRateLimit());const FString Limit=FPS>0?FString::FromInt(FPS):TEXT("Unlimited");
 if(FrameLimit->FindOptionIndex(Limit)<0)FrameLimit->AddOption(Limit);FrameLimit->SetSelectedOption(Limit);
 VSync->SetIsChecked(S->IsVSyncEnabled());
 InitialQuality=Quality->GetSelectedIndex();
 const int32 Levels[]={S->GetTextureQuality(),S->GetViewDistanceQuality(),S->GetShadowQuality(),S->GetVisualEffectQuality(),S->GetPostProcessingQuality(),S->GetAntiAliasingQuality(),S->GetFoliageQuality(),S->GetShadingQuality()};
 for(int32 I=0;I<QualityLevels.Num();++I)QualityLevels[I]->SetSelectedIndex(Levels[I]);
 DynamicLighting->SetSelectedIndex(FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("DynamicLightingQuality"))));
 for(auto& P:ValueSliders)P.Value->SetValue(ACERuntimeOptions::Get(*P.Key.ToString()));
 ChangeFieldOfView(GetPreviewFieldOfView());
 ChangeScreenBrightness(ValueSliders[TEXT("ScreenBrightness")]->GetValue());
 ChangeScreenContrast(ValueSliders[TEXT("Contrast")]->GetValue());
 ChangeEffectLightLimit(ValueSliders[TEXT("EffectLightLimit")]->GetValue());
 ActiveSoundOnly->SetIsChecked(ACERuntimeOptions::Get(TEXT("ActiveSoundOnly"))>.5f);
 CameraAlignToSlope->SetIsChecked(ACERuntimeOptions::Get(TEXT("CameraAlignToSlope"))>.5f);
 Filtering->SetSelectedIndex(FMath::Clamp(FMath::FloorLog2(FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("Anisotropy")))),0,4));
 if(auto* GI=GetGameInstance()) if(auto* Client=GI->GetSubsystem<UACEClientSubsystem>()) for(auto& P:CharacterChecks)P.Value->SetIsChecked(Client->IsCharacterOptionSet(P.Key));
 if(MouseTurnSpeed)MouseTurnSpeed->SetValue(ACECameraSettings::GetMouseTurnSpeed());
 InvertMouseX->SetIsChecked(ACECameraSettings::GetInvertMouseX());InvertMouseY->SetIsChecked(ACECameraSettings::GetInvertMouseY());
 ToggleMouseLook->SetIsChecked(ACECameraSettings::GetToggleMouseLook());
 DesktopScale->SetSelectedIndex(FMath::RoundToInt((ACERuntimeOptions::Get(TEXT("DesktopUIScale"))-1.f)*4.f));
 CursorScale->SetValue(ACERuntimeOptions::Get(TEXT("CursorScale")));
 CursorScaleLabel->SetText(FText::FromString(FString::Printf(TEXT("%d%%"),FMath::RoundToInt(CursorScale->GetValue()*100.f))));
 ObjectGlow->SetIsChecked(ACERuntimeOptions::Get(TEXT("ObjectGlow"))>.5f);
 ConstrainUIWindows->SetIsChecked(ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"))>.5f);
 RefreshFrameRateOption();
 ChatFontFace->SetSelectedIndex(FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("ChatFontFace"))));
 ChatFontSize->SetSelectedIndex(FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("ChatFontSize"))));
 PluginTextScale->SetSelectedIndex(FMath::RoundToInt((ACERuntimeOptions::Get(TEXT("PluginTextScale"))-.75f)*4.f));
 ScreenshotDirectory->SetText(FText::FromString(ACEScreenshotSettings::GetDirectory()));
}
void UACEVideoSettingsWidget::RefreshFrameRateOption()
{
 // Chat toggles only this setting, leaving other unapplied options intact.
 if(ShowFrameRate)ShowFrameRate->SetIsChecked(ACERuntimeOptions::Get(TEXT("ShowFrameRate"))>.5f);
}
UWidget* UACEVideoSettingsWidget::GenerateOption(FString Option)
{
 auto* Row=WidgetTree->ConstructWidget<UACERetailOptionWidget>();
 Row->Option=MoveTemp(Option);
 return Row;
}
TSharedRef<SWidget> UACERetailOptionWidget::RebuildWidget()
{
 if (!WidgetTree->RootWidget)
 {
  auto* Label=WidgetTree->ConstructWidget<UACERetailTextBlock>();
  Label->SetText(FText::FromString(Option));
  Label->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),8));
  Label->SetJustification(ETextJustify::Center);
  Label->SetColorAndOpacity(FSlateColor(FLinearColor(.95f,.91f,.78f,1)));
  WidgetTree->RootWidget=Label;
 }
 return Super::RebuildWidget();
}
float UACEVideoSettingsWidget::GetPreviewFieldOfView() const
{
 const auto* Slider=ValueSliders.Find(TEXT("FieldOfViewDegrees"));
 return Slider ? (*Slider)->GetValue() : ACERuntimeOptions::Get(TEXT("FieldOfViewDegrees"));
}
void UACEVideoSettingsWidget::ChangeFieldOfView(float Value)
{
 if(FieldOfViewLabel) FieldOfViewLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f degrees (live preview)"),Value)));
}
bool UACEVideoSettingsWidget::HasPendingChanges() const
{
 const auto* S=UGameUserSettings::GetGameUserSettings(); if(!S || !Resolution) return false;
 const auto R=S->GetScreenResolution();
 if(Resolution->GetSelectedOption()!=FString::Printf(TEXT("%d x %d"),R.X,R.Y)
  || WindowMode->GetSelectedIndex()!=int32(S->GetFullscreenMode())
  || Quality->GetSelectedIndex()!=InitialQuality
  || DynamicLighting->GetSelectedIndex()!=FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("DynamicLightingQuality")))
  || !FMath::IsNearlyEqual(FCString::Atof(*FrameLimit->GetSelectedOption()),S->GetFrameRateLimit())
  || VSync->IsChecked()!=S->IsVSyncEnabled()) return true;
 const int32 Levels[]={S->GetTextureQuality(),S->GetViewDistanceQuality(),S->GetShadowQuality(),S->GetVisualEffectQuality(),S->GetPostProcessingQuality(),S->GetAntiAliasingQuality(),S->GetFoliageQuality(),S->GetShadingQuality()};
 for(int32 I=0;I<QualityLevels.Num();++I) if(QualityLevels[I]->GetSelectedIndex()!=Levels[I]) return true;
 for(const auto& P:ValueSliders) if(!FMath::IsNearlyEqual(P.Value->GetValue(),ACERuntimeOptions::Get(*P.Key.ToString()))) return true;
 if(ActiveSoundOnly->IsChecked()!=(ACERuntimeOptions::Get(TEXT("ActiveSoundOnly"))>.5f)
  || Filtering->GetSelectedIndex()!=FMath::Clamp(FMath::FloorLog2(FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("Anisotropy")))),0,4)) return true;
 if(auto* GI=GetGameInstance()) if(auto* Client=GI->GetSubsystem<UACEClientSubsystem>())
  for(const auto& P:CharacterChecks) if(P.Value->IsChecked()!=Client->IsCharacterOptionSet(P.Key)) return true;
 return InvertMouseX->IsChecked()!=ACECameraSettings::GetInvertMouseX()
  || CameraAlignToSlope->IsChecked()!=(ACERuntimeOptions::Get(TEXT("CameraAlignToSlope"))>.5f)
  || ToggleMouseLook->IsChecked()!=ACECameraSettings::GetToggleMouseLook()
  || InvertMouseY->IsChecked()!=ACECameraSettings::GetInvertMouseY()
  || DesktopScale->GetSelectedIndex()!=FMath::RoundToInt((ACERuntimeOptions::Get(TEXT("DesktopUIScale"))-1.f)*4.f)
  || ObjectGlow->IsChecked()!=(ACERuntimeOptions::Get(TEXT("ObjectGlow"))>.5f)
  || ConstrainUIWindows->IsChecked()!=(ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"))>.5f)
  || ShowFrameRate->IsChecked()!=(ACERuntimeOptions::Get(TEXT("ShowFrameRate"))>.5f)
  || ChatFontFace->GetSelectedIndex()!=FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("ChatFontFace")))
  || ChatFontSize->GetSelectedIndex()!=FMath::RoundToInt(ACERuntimeOptions::Get(TEXT("ChatFontSize")))
  || PluginTextScale->GetSelectedIndex()!=FMath::RoundToInt((ACERuntimeOptions::Get(TEXT("PluginTextScale"))-.75f)*4.f)
  || ScreenshotDirectory->GetText().ToString()!=ACEScreenshotSettings::GetDirectory();
}

void UACEVideoSettingsWidget::ApplyVideo()
{
 auto* S=UGameUserSettings::GetGameUserSettings();if(!S||!Resolution)return;
 S->ValidateSettings();
 auto* PC=Cast<AACEPlayerController>(GetOwningPlayer());
 FIntPoint Requested=S->GetScreenResolution();
 FString X,Y;if(Resolution->GetSelectedOption().Split(TEXT(" x "),&X,&Y))Requested=FIntPoint(FCString::Atoi(*X),FCString::Atoi(*Y));
 const auto Mode=EWindowMode::Type(FMath::Clamp(WindowMode->GetSelectedIndex(),0,2));
 Requested=FACEDesktopDisplay::ResolveResolution(S->GetFullscreenMode(),Mode,Requested,
  FACEDesktopDisplay::GetMonitorResolution(PC),Requested!=ObservedResolution);
 ApplyRenderingOptions();
 // Validation can reload/reset the config on first launch or after an engine
 // settings-version change. Save our preferences after that reload completes.
 S->SetFrameRateLimit(FCString::Atof(*FrameLimit->GetSelectedOption()));S->SetVSyncEnabled(VSync->IsChecked());
 S->ApplyNonResolutionSettings();
 FACEDesktopDisplay::ApplyModeChange(PC,Mode,Requested);
 ApplyRuntimeOptions(); ResetVideo();
 if(PC)PC->RefreshDesktopDisplayInputMode();
}

void UACEVideoSettingsWidget::ApplyRenderingOptions()
{
 auto* S=UGameUserSettings::GetGameUserSettings(); if(!S || !Quality) return;
 if(Quality->GetSelectedIndex()<5 && Quality->GetSelectedIndex()!=InitialQuality) S->SetOverallScalabilityLevel(Quality->GetSelectedIndex());
 if(QualityLevels.Num()==8) {
  S->SetTextureQuality(QualityLevels[0]->GetSelectedIndex()); S->SetViewDistanceQuality(QualityLevels[1]->GetSelectedIndex()); S->SetShadowQuality(QualityLevels[2]->GetSelectedIndex());
  S->SetVisualEffectQuality(QualityLevels[3]->GetSelectedIndex()); S->SetPostProcessingQuality(QualityLevels[4]->GetSelectedIndex()); S->SetAntiAliasingQuality(QualityLevels[5]->GetSelectedIndex());
  S->SetFoliageQuality(QualityLevels[6]->GetSelectedIndex()); S->SetShadingQuality(QualityLevels[7]->GetSelectedIndex());
 }
}

void UACEVideoSettingsWidget::ChangeQuality(FString, ESelectInfo::Type)
{
 if(LoadingSettings || !Quality || Quality->GetSelectedIndex()<0 || Quality->GetSelectedIndex()>=5)return;
 // Show what the preset will apply, and let subsequent individual edits win.
 for(auto Level:QualityLevels)Level->SetSelectedIndex(Quality->GetSelectedIndex());
}

void UACEVideoSettingsWidget::ApplyRuntimeOptions()
{
 for(const auto& P:ValueSliders) ACERuntimeOptions::Set(*P.Key.ToString(),P.Value->GetValue());
 ACERuntimeOptions::Set(TEXT("DynamicLightingQuality"),DynamicLighting->GetSelectedIndex());
 ApplyInterfaceOptions();
 ACERuntimeOptions::Set(TEXT("ActiveSoundOnly"),ActiveSoundOnly->IsChecked()?1:0);
 ACERuntimeOptions::Set(TEXT("CameraAlignToSlope"),CameraAlignToSlope->IsChecked()?1:0);
 ACERuntimeOptions::Set(TEXT("Anisotropy"),float(1<<FMath::Clamp(Filtering->GetSelectedIndex(),0,4)));
 if(auto* GI=GetGameInstance()) if(auto* Client=GI->GetSubsystem<UACEClientSubsystem>()) for(auto& P:CharacterChecks)Client->SendSetSingleCharacterOption(P.Key,P.Value->IsChecked());
 ACERuntimeOptions::Apply();
}


void UACEVideoSettingsWidget::ApplyInterfaceOptions()
{
 if(ScreenshotDirectory) ACEScreenshotSettings::SetDirectory(ScreenshotDirectory->GetText().ToString());
 ACECameraSettings::SetMouseInversion(InvertMouseX->IsChecked(),InvertMouseY->IsChecked());
 ACECameraSettings::SetToggleMouseLook(ToggleMouseLook->IsChecked());
 if(DesktopScale->GetSelectedIndex()>=0) ACERuntimeOptions::Set(TEXT("DesktopUIScale"),1.f+DesktopScale->GetSelectedIndex()*.25f);
 if(CursorScale) ACERuntimeOptions::Set(TEXT("CursorScale"),CursorScale->GetValue());
 ACERuntimeOptions::Set(TEXT("ObjectGlow"),ObjectGlow->IsChecked()?1.f:0.f);
 ACERuntimeOptions::Set(TEXT("ConstrainUIWindows"),ConstrainUIWindows->IsChecked()?1.f:0.f);
 ACERuntimeOptions::Set(TEXT("ShowFrameRate"),ShowFrameRate->IsChecked()?1.f:0.f);
 if(ChatFontFace->GetSelectedIndex()>=0) ACERuntimeOptions::Set(TEXT("ChatFontFace"),ChatFontFace->GetSelectedIndex());
 if(ChatFontSize->GetSelectedIndex()>=0) ACERuntimeOptions::Set(TEXT("ChatFontSize"),ChatFontSize->GetSelectedIndex());
 if(PluginTextScale->GetSelectedIndex()>=0) ACERuntimeOptions::Set(TEXT("PluginTextScale"),.75f+PluginTextScale->GetSelectedIndex()*.25f);
 ACERuntimeOptions::Apply();
}

void UACEVideoSettingsWidget::ChangeCursorScale(float Value)
{
 Value=FMath::Clamp(FMath::RoundToFloat(Value*4.f)/4.f,.5f,3.f);
 CursorScale->SetValue(Value);
 if(CursorScaleLabel) CursorScaleLabel->SetText(FText::FromString(FString::Printf(TEXT("%d%%"),FMath::RoundToInt(Value*100.f))));
 if(!FMath::IsNearlyEqual(Value,ACERuntimeOptions::Get(TEXT("CursorScale"))))
 {
  ACERuntimeOptions::Set(TEXT("CursorScale"),Value);
  if(GConfig)GConfig->Flush(false,GGameUserSettingsIni);
 }
}

void UACEVideoSettingsWidget::ChangeObjectGlow(bool bEnabled)
{
 const float Value=bEnabled?1.f:0.f;
 if(ACERuntimeOptions::Get(TEXT("ObjectGlow"))!=Value)
 {
  ACERuntimeOptions::Set(TEXT("ObjectGlow"),Value);
  if(GConfig)GConfig->Flush(false,GGameUserSettingsIni);
 }
}

void UACEVideoSettingsWidget::ChangeMouseTurnSpeed(float Value)
{
 ACECameraSettings::SetMouseTurnSpeed(Value);
 UACEHoverTooltipWidget::SetWidgetTooltip(MouseTurnSpeed, FText::FromString(FString::Printf(TEXT("Mouse turn speed: %d%%"),FMath::RoundToInt(Value*100.f))));
}
void UACEVideoSettingsWidget::ChangeScreenBrightness(float Value)
{
 if(ScreenBrightnessLabel) ScreenBrightnessLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f%% (saved immediately)"),Value*100.f)));
 if(!FMath::IsNearlyEqual(Value,ACERuntimeOptions::Get(TEXT("ScreenBrightness"))))
 {
  ACERuntimeOptions::Set(TEXT("ScreenBrightness"),Value);
  ACERuntimeOptions::Apply();
 }
}
void UACEVideoSettingsWidget::ChangeEffectLightLimit(float Value)
{
 if(EffectLightLimitLabel)EffectLightLimitLabel->SetText(FText::FromString(FString::Printf(TEXT("%d lights (saved with Apply)"),FMath::RoundToInt(Value))));
}
void UACEVideoSettingsWidget::ChangeScreenContrast(float Value)
{
 if(ScreenContrastLabel) ScreenContrastLabel->SetText(FText::FromString(FString::Printf(TEXT("%.0f%% (saved immediately)"),Value*100.f)));
 if(!FMath::IsNearlyEqual(Value,ACERuntimeOptions::Get(TEXT("Contrast"))))
 {
  ACERuntimeOptions::Set(TEXT("Contrast"),Value);
  ACERuntimeOptions::Apply();
 }
}
void UACEVideoSettingsWidget::ResetMouseTurnSpeed()
{
 ACECameraSettings::SetMouseTurnSpeed(ACECameraSettings::DefaultMouseTurnSpeed);
 if(MouseTurnSpeed)MouseTurnSpeed->SetValue(ACECameraSettings::DefaultMouseTurnSpeed);
}
void UACEVideoSettingsWidget::DefaultsVideo()
{
 if(!Resolution)return;
 WindowMode->SetSelectedIndex(0); Quality->SetSelectedIndex(2); FrameLimit->SetSelectedIndex(0); VSync->SetIsChecked(false);
 for(const auto& O:ACERuntimeOptions::Values) if(auto* V=ValueSliders.Find(O.Key))(*V)->SetValue(O.Default);
 ChangeFieldOfView(GetPreviewFieldOfView());
 ChangeScreenBrightness(ValueSliders[TEXT("ScreenBrightness")]->GetValue());
 ChangeScreenContrast(ValueSliders[TEXT("Contrast")]->GetValue());
 ActiveSoundOnly->SetIsChecked(true); Filtering->SetSelectedIndex(4);
 CameraAlignToSlope->SetIsChecked(true);
 for(UComboBoxString* C:QualityLevels)C->SetSelectedIndex(2);
 DynamicLighting->SetSelectedIndex(2);
 for(auto& P:CharacterChecks) if(const auto* O=ACECharacterOptions::Find(P.Key)) P.Value->SetIsChecked(((O->bInOptions2?ACECharacterOptions::Options2Default:ACECharacterOptions::Options1Default)&O->Flag)!=0);
 ResetMouseTurnSpeed();
 InvertMouseX->SetIsChecked(false);InvertMouseY->SetIsChecked(false);DesktopScale->SetSelectedIndex(0);
 ToggleMouseLook->SetIsChecked(false);
 CursorScale->SetValue(1.f); ChangeCursorScale(1.f);
 ObjectGlow->SetIsChecked(true); ChangeObjectGlow(true);
 ConstrainUIWindows->SetIsChecked(true);
 ShowFrameRate->SetIsChecked(false);
 ChatFontFace->SetSelectedIndex(2); ChatFontSize->SetSelectedIndex(1);
 PluginTextScale->SetSelectedIndex(1);
 ScreenshotDirectory->SetText(FText::FromString(ACEScreenshotSettings::DefaultDirectory()));
}

float UACEVideoSettingsWidget::GetScrollOffset() const { return Scroll?Scroll->GetScrollOffset():0; }
float UACEVideoSettingsWidget::GetScrollEnd() const { return Scroll?Scroll->GetScrollOffsetOfEnd():0; }
void UACEVideoSettingsWidget::SetScrollOffset(float Offset) { if(Scroll)Scroll->SetScrollOffset(FMath::Clamp(Offset,0.f,GetScrollEnd())); }
