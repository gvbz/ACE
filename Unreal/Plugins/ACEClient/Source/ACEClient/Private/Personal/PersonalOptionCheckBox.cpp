#include "Personal/PersonalOptionCheckBox.h"
#include "Personal/PersonalFarTerrain.h"
#include "Personal/PersonalSkyHorizon.h"
#include "ACEHoverTooltipWidget.h"
#include "ACERuntimeOptions.h"
#include "Blueprint/WidgetTree.h"
#include "Misc/ConfigCacheIni.h"

UPersonalOptionCheckBox* UPersonalOptionCheckBox::Make(UWidgetTree* Tree, const FCheckBoxStyle& Style, UWidget* Label, const TCHAR* Key, const TCHAR* Tooltip)
{
	UPersonalOptionCheckBox* Box = Tree->ConstructWidget<UPersonalOptionCheckBox>(UPersonalOptionCheckBox::StaticClass(), Key);
	Box->OptionKey = Key;
	Box->SetWidgetStyle(Style);
	Box->SetContent(Label);
	Box->SetIsChecked(ACERuntimeOptions::Get(Key) > 0.5f);
	Box->OnCheckStateChanged.AddDynamic(Box, &UPersonalOptionCheckBox::Changed);
	UACEHoverTooltipWidget::SetWidgetTooltip(Box, FText::FromString(Tooltip));
	return Box;
}

void UPersonalOptionCheckBox::SynchronizeProperties()
{
	if (!OptionKey.IsEmpty())
	{
		// The window is built again when it opens: show what is saved.
		SetIsChecked(ACERuntimeOptions::Get(*OptionKey) > 0.5f);
	}
	Super::SynchronizeProperties();
}

void UPersonalOptionCheckBox::Changed(bool bChecked)
{
	const float Value = bChecked ? 1.f : 0.f;
	if (ACERuntimeOptions::Get(*OptionKey) != Value)
	{
		ACERuntimeOptions::Set(*OptionKey, Value);
		if (GConfig)
		{
			GConfig->Flush(false, GGameUserSettingsIni);
		}
		if (OptionKey == PersonalSkyHorizon::OptionKey || OptionKey == PersonalFarTerrain::OptionKey)
		{
			// The sky meshes are made when the sky is built, and the stars layer
			// depends on both options, so build the sky again.
			PersonalSkyHorizon::RebuildSky();
		}
	}
}
