#pragma once

#include "CoreMinimal.h"
#include "Components/CheckBox.h"
#include "PersonalOptionCheckBox.generated.h"

class UWidgetTree;

/**
 * Personal settings: a check box for an on/off row of ACERuntimeOptions. It shows the
 * saved value and saves a change at once, so the settings window needs no other code for it.
 */
UCLASS()
class UPersonalOptionCheckBox : public UCheckBox
{
	GENERATED_BODY()
public:
	/** Key is the option key. It is also the widget's name, so it cannot contain a dot. */
	static UPersonalOptionCheckBox* Make(UWidgetTree* Tree, const FCheckBoxStyle& Style, UWidget* Label, const TCHAR* Key, const TCHAR* Tooltip);

	virtual void SynchronizeProperties() override;

private:
	UFUNCTION()
	void Changed(bool bChecked);

	FString OptionKey;
};
