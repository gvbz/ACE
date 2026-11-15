#pragma once

#include "CoreMinimal.h"
#include "Components/TextBlock.h"
#include "PersonalSliderLabel.generated.h"

class USlider;
class UWidgetTree;

/**
 * Personal settings: a line of text under a slider that shows the slider's value as a whole number,
 * for sliders where the number matters (a radius in landblocks). It reads the slider while the
 * window is open, so it also follows Reset and Defaults; the settings window needs no other code for it.
 */
UCLASS()
class UPersonalSliderLabel : public UTextBlock
{
	GENERATED_BODY()
public:
	/** Style: a text block of the window whose font and color are copied. Unit: the text after the number. */
	static UPersonalSliderLabel* Make(UWidgetTree* Tree, USlider* Slider, const UTextBlock* Style, const TCHAR* Unit);

	/** The text for a value: the number the feature uses (rounded), then the unit. */
	static FString Format(float Value, const FString& Unit);

private:
	UFUNCTION()
	FText ValueText();

	UPROPERTY()
	TObjectPtr<USlider> Slider;
	FString Unit;
};
