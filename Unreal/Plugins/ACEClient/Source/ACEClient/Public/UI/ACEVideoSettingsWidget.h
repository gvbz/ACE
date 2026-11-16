#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ACEVideoSettingsWidget.generated.h"
class UComboBoxString;
class UCheckBox;
class USlider;
class UScrollBox;
class UEditableTextBox;
class UTextBlock;
/** An option is a user widget so its Slate SObjectWidget keeps the DAT label alive during GC. */
UCLASS()
class ACECLIENT_API UACERetailOptionWidget : public UUserWidget
{
 GENERATED_BODY()
public:
 FString Option;
 virtual TSharedRef<SWidget> RebuildWidget() override;
};
UCLASS()
class ACECLIENT_API UACEVideoSettingsWidget : public UUserWidget
{
 GENERATED_BODY()
public:
 virtual TSharedRef<SWidget> RebuildWidget() override;
 virtual void NativeTick(const FGeometry& Geometry, float DeltaTime) override;
 UFUNCTION() void ApplyVideo();
 void ApplyInterfaceOptions();
 void ApplyRuntimeOptions();
 /** Apply rendering preferences without changing display resolution or window mode. */
 void ApplyRenderingOptions();
 UFUNCTION() void ResetVideo();
 UFUNCTION() void RefreshResolutions();
 void RefreshFrameRateOption();
 UFUNCTION() void DefaultsVideo();
 bool HasPendingChanges() const;
 float GetPreviewFieldOfView() const;
 UFUNCTION() void ChangeFieldOfView(float Value);
 UFUNCTION() void ChangeQuality(FString Value, ESelectInfo::Type SelectionType);
 UFUNCTION() UWidget* GenerateOption(FString Option);
 UFUNCTION() void ChangeMouseTurnSpeed(float Value);
 UFUNCTION() void ResetMouseTurnSpeed();
 UFUNCTION() void ChangeCursorScale(float Value);
 UFUNCTION() void ChangeObjectGlow(bool bEnabled);
 UFUNCTION() void ChangeUniformDungeonLighting(bool bEnabled);
 UFUNCTION() void ChangeScreenBrightness(float Value);
 UFUNCTION() void ChangeEffectLightLimit(float Value);
 UFUNCTION() void ChangeScreenContrast(float Value);
 float GetScrollOffset() const;
 float GetScrollEnd() const;
 void SetScrollOffset(float Offset);
private:
 bool LoadingSettings=false;
 int32 InitialQuality = 0;
 int32 ObservedWindowMode = -1;
 FIntPoint ObservedResolution = FIntPoint::ZeroValue;
 UPROPERTY() TObjectPtr<UScrollBox> Scroll;
 UPROPERTY() TArray<TObjectPtr<UComboBoxString>> QualityLevels;
 UPROPERTY() TMap<FName,TObjectPtr<USlider>> ValueSliders;
 UPROPERTY() TMap<int32,TObjectPtr<UCheckBox>> CharacterChecks;
 UPROPERTY() TObjectPtr<UCheckBox> ActiveSoundOnly;
 UPROPERTY() TObjectPtr<UCheckBox> CameraAlignToSlope;
 UPROPERTY() TObjectPtr<UComboBoxString> Filtering;
 UPROPERTY() TObjectPtr<UComboBoxString> Resolution;
 UPROPERTY() TObjectPtr<UComboBoxString> WindowMode;
 UPROPERTY() TObjectPtr<UComboBoxString> Quality;
 UPROPERTY() TObjectPtr<UComboBoxString> DynamicLighting;
 UPROPERTY() TObjectPtr<UComboBoxString> FrameLimit;
 UPROPERTY() TObjectPtr<UCheckBox> VSync;
 UPROPERTY() TObjectPtr<UCheckBox> ShowFrameRate;
 UPROPERTY() TObjectPtr<UCheckBox> ObjectGlow;
 UPROPERTY() TObjectPtr<UCheckBox> UniformDungeonLighting;
 UPROPERTY() TObjectPtr<UCheckBox> ConstrainUIWindows;
 UPROPERTY() TObjectPtr<USlider> MouseTurnSpeed;
 UPROPERTY() TObjectPtr<UCheckBox> InvertMouseX;
 UPROPERTY() TObjectPtr<UCheckBox> InvertMouseY;
 UPROPERTY() TObjectPtr<UCheckBox> ToggleMouseLook;
 UPROPERTY() TObjectPtr<UComboBoxString> DesktopScale;
 UPROPERTY() TObjectPtr<USlider> CursorScale;
 UPROPERTY() TObjectPtr<UTextBlock> CursorScaleLabel;
 UPROPERTY() TObjectPtr<UTextBlock> FieldOfViewLabel;
 UPROPERTY() TObjectPtr<UTextBlock> ScreenBrightnessLabel;
 UPROPERTY() TObjectPtr<UTextBlock> EffectLightLimitLabel;
 UPROPERTY() TObjectPtr<UTextBlock> ScreenContrastLabel;
 UPROPERTY() TObjectPtr<UComboBoxString> ChatFontFace;
 UPROPERTY() TObjectPtr<UComboBoxString> ChatFontSize;
    UPROPERTY() TObjectPtr<UComboBoxString> PluginTextScale;
 UPROPERTY() TObjectPtr<UEditableTextBox> ScreenshotDirectory;
};
