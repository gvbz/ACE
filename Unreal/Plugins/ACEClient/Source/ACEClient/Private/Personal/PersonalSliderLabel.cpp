#include "Personal/PersonalSliderLabel.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Slider.h"

UPersonalSliderLabel* UPersonalSliderLabel::Make(UWidgetTree* Tree, USlider* InSlider, const UTextBlock* Style, const TCHAR* InUnit)
{
	UPersonalSliderLabel* Label = Tree->ConstructWidget<UPersonalSliderLabel>(UPersonalSliderLabel::StaticClass());
	Label->Slider = InSlider;
	Label->Unit = InUnit;
	if (Style)
	{
		Label->SetFont(Style->GetFont());
		Label->SetColorAndOpacity(Style->GetColorAndOpacity());
	}
	Label->SetJustification(ETextJustify::Right);
	// Read by the window every frame it is open: no event of the slider is needed.
	Label->TextDelegate.BindUFunction(Label, GET_FUNCTION_NAME_CHECKED(UPersonalSliderLabel, ValueText));
	return Label;
}

FString UPersonalSliderLabel::Format(float Value, const FString& Unit)
{
	return FString::Printf(TEXT("%d %s"), FMath::RoundToInt32(Value), *Unit);
}

FText UPersonalSliderLabel::ValueText()
{
	return FText::FromString(Slider ? Format(Slider->GetValue(), Unit) : FString());
}
