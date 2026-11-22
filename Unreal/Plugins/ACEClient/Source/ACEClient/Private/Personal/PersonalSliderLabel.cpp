#include "Personal/PersonalSliderLabel.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Slider.h"

UPersonalSliderLabel* UPersonalSliderLabel::Make(UWidgetTree* Tree, USlider* InSlider, const UTextBlock* Style, const TCHAR* InUnit, int32 InDecimals)
{
	UPersonalSliderLabel* Label = Tree->ConstructWidget<UPersonalSliderLabel>(UPersonalSliderLabel::StaticClass());
	Label->Slider = InSlider;
	Label->Unit = InUnit;
	Label->Decimals = FMath::Clamp(InDecimals, 0, 4);
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

FString UPersonalSliderLabel::Format(float Value, const FString& Unit, int32 Decimals)
{
	FString Number;
	switch (FMath::Clamp(Decimals, 0, 4))
	{
	case 1: Number = FString::Printf(TEXT("%.1f"), Value); break;
	case 2: Number = FString::Printf(TEXT("%.2f"), Value); break;
	case 3: Number = FString::Printf(TEXT("%.3f"), Value); break;
	case 4: Number = FString::Printf(TEXT("%.4f"), Value); break;
	default: Number = FString::FromInt(FMath::RoundToInt32(Value)); break;
	}
	return Unit.IsEmpty() ? Number : Number + TEXT(" ") + Unit;
}

FText UPersonalSliderLabel::ValueText()
{
	return FText::FromString(Slider ? Format(Slider->GetValue(), Unit, Decimals) : FString());
}
