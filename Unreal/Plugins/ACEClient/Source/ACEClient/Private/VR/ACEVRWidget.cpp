#include "VR/ACEVRWidget.h"
#include "ACEClientBuild.h"
#include "ACERuntimeOptions.h"
#include "Sky/ACELegionsSky.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "VR/ACEVRInputLayout.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "InputCoreTypes.h"

namespace
{
#if PLATFORM_ANDROID
	// Multiple menu instances can exist while a transfer is being imported.
	struct FQuestSkyMenu
	{
		bool HasSource = false;
		bool HasCache = false;
		bool Importing = false;
		bool NeedsWeather = false;
		FString Message;

		void Refresh()
		{
			HasSource = IFileManager::Get().FileExists(*ACELegionsSky::StagedPath(ACELegionsSky::CachePath()));
			HasCache = ACELegionsSky::HasCache(&NeedsWeather);
		}
		void StartImport()
		{
			if (!HasSource || Importing) return;
			Importing = true;
			Message = TEXT("Importing your AC2 sky...");
			Async(EAsyncExecution::ThreadPool, [this, Destination = ACELegionsSky::CachePath()]()
			{
				FString Error;
				const bool Success = ACELegionsSky::ImportStaged(Destination, Error);
				AsyncTask(ENamedThreads::GameThread, [this, Success, Error = MoveTemp(Error)]()
				{
					Importing = false;
					Refresh();
					Message = Success ? TEXT("AC2 sky installed.") : Error;
					if (Success)
					{
						ACERuntimeOptions::Set(TEXT("LegionsSky"), 1.f);
						ACERuntimeOptions::Apply();
					}
				});
			});
		}
	};
	FQuestSkyMenu QuestSky;
#endif
	const FLinearColor MenuGold(.66f, .44f, .16f);
	const FLinearColor MenuText(.90f, .83f, .66f);
	const FButtonStyle& RetailButtonStyle()
	{
		static const FButtonStyle Style = FButtonStyle()
			.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.065f, .050f, .026f), 0.f, MenuGold, 1.5f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.16f, .11f, .043f), 0.f, FLinearColor(.95f, .72f, .31f), 2.f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(.033f, .027f, .015f), 0.f, MenuGold, 2.f))
			.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.035f, .032f, .023f), 0.f, FLinearColor(.22f, .18f, .10f), 1.f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(1.f, 2.f, -1.f, -2.f));
		return Style;
	}
	const FSliderStyle& MenuSliderStyle()
	{
		static const FSliderStyle Style=FSliderStyle()
			.SetNormalBarImage(FSlateRoundedBoxBrush(FLinearColor(.10f,.08f,.04f),3.f,MenuGold,1.f))
			.SetHoveredBarImage(FSlateRoundedBoxBrush(FLinearColor(.17f,.13f,.06f),3.f,MenuGold,1.f))
			.SetDisabledBarImage(FSlateRoundedBoxBrush(FLinearColor(.06f,.05f,.04f),3.f))
			.SetNormalThumbImage(FSlateRoundedBoxBrush(MenuGold,4.f,FLinearColor(.95f,.82f,.53f),1.f,FVector2D(24,32)))
			.SetHoveredThumbImage(FSlateRoundedBoxBrush(FLinearColor(.95f,.72f,.31f),4.f,MenuText,1.f,FVector2D(24,32)))
			.SetDisabledThumbImage(FSlateRoundedBoxBrush(FLinearColor(.22f,.18f,.10f),4.f,FVector2D(24,32)))
			.SetBarThickness(10.f);
		return Style;
	}
	const FComboBoxStyle& MenuComboStyle()
	{
		static const FComboBoxStyle Style=[]()
		{
			auto S=FCoreStyle::Get().GetWidgetStyle<FComboBoxStyle>("ComboBox");
			auto B=S.ComboButtonStyle;B.SetButtonStyle(RetailButtonStyle()).SetContentPadding(FMargin(12,8));
			S.SetComboButtonStyle(B);return S;
		}();
		return Style;
	}

}

TSharedRef<SWidget> UACEVRWidget::RebuildWidget()
{
	TWeakObjectPtr<UACEVRComponent> Rig = VR;
	auto Text = [](TAttribute<FText> Label, int32 Size = 24)
	{
		return SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Regular", Size))
			.ColorAndOpacity(MenuText).ShadowOffset(FVector2D(1, 1)).ShadowColorAndOpacity(FLinearColor::Black).AutoWrapText(true);
	};
	auto Button = [&](TAttribute<FText> Label, TFunction<void()> Click, int32 Size=22)
	{
		TSharedRef<SWidget> Caption = Text(Label, bKeyboard ? 24 : Size);
		if (bWrist)
			Caption = SNew(SBox).HeightOverride(28.f)[SNew(SScaleBox).Stretch(EStretch::ScaleToFit)
				.StretchDirection(EStretchDirection::DownOnly).HAlign(HAlign_Left)
				[SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Regular", 22)).ColorAndOpacity(MenuText)]];
		return SNew(SButton).ButtonStyle(&RetailButtonStyle()).IsFocusable(false).ContentPadding(FMargin(12.f, 8.f))
			.OnClicked_Lambda([Click]() { Click(); return FReply::Handled(); })[Caption];
	};
	auto Literal = [](const TCHAR* S) { return TAttribute<FText>(FText::FromString(S)); };
	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	if (bKeyboard)
	{
		List->AddSlot().AutoHeight()[Text(TAttribute<FText>::CreateLambda([Rig]() { return FText::FromString(Rig.IsValid() ? Rig->GetTextInputPreview() : FString()); }), 22)];
		for (const FString& Row : {FString(TEXT("`1234567890-=")), FString(TEXT("qwertyuiop[]\\")), FString(TEXT("asdfghjkl;'")), FString(TEXT("zxcvbnm,./"))})
		{
			auto Keys = SNew(SHorizontalBox);
			for (TCHAR C : Row)
			{
				const FString Key(1, &C);
				Keys->AddSlot().FillWidth(1.f).Padding(2.f)[Button(TAttribute<FText>::CreateLambda([this, Key]()
					{ return FText::FromString(ACEVRMath::KeyboardCharacter(Key[0], bShift)); }), [this, Rig, Key]()
					{ if (Rig.IsValid()) Rig->TypeText(ACEVRMath::KeyboardCharacter(Key[0], bShift)); })];
			}
			List->AddSlot().AutoHeight()[Keys];
		}
		auto Keys = SNew(SHorizontalBox);
		Keys->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Shift")), [this]() { bShift = !bShift; })];
		Keys->AddSlot().FillWidth(.7f)[Button(Literal(TEXT("Up")), [Rig]() { if (Rig.IsValid()) Rig->TypeKey(EKeys::Up); })];
		Keys->AddSlot().FillWidth(.7f)[Button(Literal(TEXT("Down")), [Rig]() { if (Rig.IsValid()) Rig->TypeKey(EKeys::Down); })];
		Keys->AddSlot().FillWidth(2.f)[Button(Literal(TEXT("Space")), [Rig]() { if (Rig.IsValid()) Rig->TypeText(TEXT(" ")); })];
		Keys->AddSlot().FillWidth(1.5f)[Button(Literal(TEXT("Delete")), [Rig]() { if (Rig.IsValid()) Rig->TypeKey(EKeys::BackSpace); })];
		Keys->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Enter")), [Rig]() { if (Rig.IsValid()) Rig->TypeKey(EKeys::Enter); })];
		Keys->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Close")), [Rig]() { if (Rig.IsValid()) Rig->ToggleKeyboard(); })];
		List->AddSlot().AutoHeight()[Keys];
	}
	else if (bWrist)
	{
		List->AddSlot().AutoHeight().Padding(0, 0, 0, 8)[Text(Literal(TEXT("LEFT WRIST / SPELLS")), 25)];
		List->AddSlot().AutoHeight().Padding(0, 0, 0, 10)[Text(TAttribute<FText>::CreateLambda([Rig]()
			{ return FText::FromString(Rig.IsValid() ? Rig->GetSelectionText() : FString()); }), 20)];
		for (int32 I = 0; I < 8; ++I)
			List->AddSlot().AutoHeight().Padding(0, 2)
			[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).Padding(2)
				.BorderBackgroundColor_Lambda([Rig, I]() { return Rig.IsValid() && Rig->IsWristSlotSelected(I)
					? FLinearColor(.95f, .65f, .18f) : FLinearColor(.12f, .09f, .04f); })
				[Button(TAttribute<FText>::CreateLambda([Rig, I]()
					{ return FText::FromString(Rig.IsValid() ? Rig->GetWristSlotText(I) : FString()); }), [Rig, I]()
					{ if (Rig.IsValid()) Rig->SelectWristSlot(I); })]];
		auto Row = SNew(SHorizontalBox);
		Row->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Previous")), [Rig]() { if (Rig.IsValid()) Rig->CycleSpell(-1); })];
		Row->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Next")), [Rig]() { if (Rig.IsValid()) Rig->CycleSpell(1); })];
		List->AddSlot().AutoHeight().Padding(0, 5)[Row];
		List->AddSlot().AutoHeight()[Button(Literal(TEXT("Next spell bank")), [Rig]() { if (Rig.IsValid()) Rig->CycleSpellBar(1); })];
		List->AddSlot().AutoHeight().Padding(0, 8)[Text(TAttribute<FText>::CreateLambda([Rig]()
			{ return FText::FromString(Rig.IsValid() ? Rig->GetCastFeedback() : FString()); }), 20)];
	}
	else
	{
		List->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(FString(ACEClientBuild::VRProductName) + TEXT(" - Options")))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 30)).ColorAndOpacity(FLinearColor(.95f, .73f, .33f))
			.ShadowOffset(FVector2D(1, 2)).ShadowColorAndOpacity(FLinearColor::Black)];
		List->AddSlot().AutoHeight().Padding(0, 10)[Text(TAttribute<FText>::CreateLambda([Rig]()
			{ return FText::FromString(Rig.IsValid() ? Rig->GetStatusText() : FString()); }), 20)];
		auto Pages = SNew(SHorizontalBox);
		Pages->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Inventory")), [Rig]() { if (Rig.IsValid()) Rig->OpenRetailPanel(TEXT("InventoryPanel_Field")); })];
		Pages->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Spellbook")), [Rig]() { if (Rig.IsValid()) Rig->OpenRetailPanel(TEXT("SpellManagementPanel_Field")); })];
		Pages->AddSlot().FillWidth(1.f)[Button(Literal(TEXT("Game options")), [Rig]() { if (Rig.IsValid()) Rig->OpenRetailPanel(TEXT("OptionsPanel_Field")); })];
		List->AddSlot().AutoHeight().Padding(0, 6)[Pages];
		auto Categories=SNew(SHorizontalBox);
		auto Switcher=SNew(SWidgetSwitcher).WidgetIndex_Lambda([this](){return SettingsCategory;});
		TArray<TSharedRef<SScrollBox>> Sections;
		const TCHAR* CategoryNames[]={TEXT("Movement"),TEXT("Panels"),TEXT("Combat"),TEXT("Controls"),TEXT("Graphics")};
		for(int32 I=0;I<UE_ARRAY_COUNT(CategoryNames);++I)
		{
			Categories->AddSlot().FillWidth(1).Padding(2,0)
			[SNew(SBorder).Padding(0,0,0,3).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this,I](){return SettingsCategory==I?MenuGold:FLinearColor::Transparent;})
			[Button(Literal(CategoryNames[I]),[this,I](){SettingsCategory=I;},16)]];
			auto Page=SNew(SScrollBox).Tag(FName(CategoryNames[I]));Sections.Add(Page);Switcher->AddSlot()[Page];
		}
		List->AddSlot().AutoHeight().Padding(0,8)[Categories];
		auto Scroll=Sections[1];
		Scroll->AddSlot().Padding(0,6)[Text(Literal(TEXT("Panel placement: unlock and hold Move. Move or turn your controller to position the panel; use the right stick to push it away or bring it closer. Hold Resize to change its size.")),18)];
		for(FName Setting:{FName("Fellowship"),FName("FellowshipAnchor"),FName("FellowshipLock")})
			Scroll->AddSlot().Padding(0,6)[Button(TAttribute<FText>::CreateLambda([Rig,Setting]()
			{
				if(!Rig.IsValid())return FText::GetEmpty();const auto* S=Rig->GetSettings();
				if(Setting=="Fellowship")return FText::FromString(S->bShowFellowship?TEXT("Fellowship: Shown"):TEXT("Fellowship: Hidden"));
				if(Setting=="FellowshipLock")return FText::FromString(S->bFellowshipLocked?TEXT("Fellowship placement: Locked"):TEXT("Fellowship placement: Unlocked"));
				return FText::FromString(S->FellowshipAnchorMode==0?TEXT("Fellowship anchor: Head"):S->FellowshipAnchorMode==1?TEXT("Fellowship anchor: Body"):TEXT("Fellowship anchor: World"));
			}),[Rig,Setting](){if(Rig.IsValid())Rig->ChangeSetting(Setting);})];
		Scroll=Sections[3];
		Scroll->AddSlot().Padding(0,12)[Text(Literal(TEXT("Controller button layout")),24)];
		static TArray<TSharedPtr<FName>> BindingOptions=[](){TArray<TSharedPtr<FName>> R;for(const auto& B:ACEVRInputLayout::Buttons())R.Add(MakeShared<FName>(B.Input));R.Add(MakeShared<FName>("VRChat"));return R;}();
		for(const auto& B:ACEVRInputLayout::Buttons())
		{
			Scroll->AddSlot().Padding(0,5)[SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[Text(Literal(B.Physical),20)]
				+ SVerticalBox::Slot().AutoHeight()[SNew(SComboBox<TSharedPtr<FName>>).ComboBoxStyle(&MenuComboStyle()).HasDownArrow(false).ForegroundColor(MenuText).ContentPadding(FMargin(12,8)).OptionsSource(&BindingOptions)
					.OnGenerateWidget_Lambda([](TSharedPtr<FName> Name){return SNew(STextBlock).Text(FText::FromString(ACEVRInputLayout::ActionLabel(*Name))).Font(FCoreStyle::GetDefaultFontStyle("Regular",22));})
					.OnSelectionChanged_Lambda([Rig,Input=B.Input](TSharedPtr<FName> Name,ESelectInfo::Type){if(Rig.IsValid() && Name.IsValid())Rig->ConfigureButton(Input,*Name);})
					[SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1)[Text(TAttribute<FText>::CreateLambda([Rig,Input=B.Input](){return FText::FromString(Rig.IsValid()?ACEVRInputLayout::ActionLabel(Rig->GetSettings()->GetButtonAction(Input)):FString());}),22)]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Text(Literal(TEXT("\u25BC")),16)]]]];
		}
		Scroll->AddSlot().Padding(0,12)[Text(Literal(TEXT("Inventory and spellbook shortcuts")),24)];
		Scroll->AddSlot().Padding(0,6)[Text(Literal(TEXT("These actions apply while the native menu is open. Point at an item or spell, or use the current selection.")),18)];
		static TArray<TSharedPtr<FName>> MenuOptions={MakeShared<FName>(NAME_None),MakeShared<FName>("VRMenuUse"),MakeShared<FName>("VRMenuInspect")};
		auto MenuLabel=[](FName Action){return Action=="VRMenuUse"?TEXT("Use / equip item"):Action=="VRMenuInspect"?TEXT("Inspect item / spell"):TEXT("Normal controller action");};
		for(const auto& B:ACEVRInputLayout::Buttons())
		{
			Scroll->AddSlot().Padding(0,5)[SNew(SVerticalBox)
				+SVerticalBox::Slot().AutoHeight()[Text(Literal(B.Physical),20)]
				+SVerticalBox::Slot().AutoHeight()[SNew(SComboBox<TSharedPtr<FName>>).ComboBoxStyle(&MenuComboStyle()).ForegroundColor(MenuText).ContentPadding(FMargin(12,8)).OptionsSource(&MenuOptions)
					.OnGenerateWidget_Lambda([MenuLabel](TSharedPtr<FName> Action){return SNew(STextBlock).Text(FText::FromString(MenuLabel(*Action))).Font(FCoreStyle::GetDefaultFontStyle("Regular",22));})
					.OnSelectionChanged_Lambda([Rig,Input=B.Input](TSharedPtr<FName> Action,ESelectInfo::Type){if(Rig.IsValid() && Action)Rig->GetSettings()->SetMenuButtonAction(Input,*Action);})
					[Text(TAttribute<FText>::CreateLambda([Rig,Input=B.Input,MenuLabel](){return FText::FromString(MenuLabel(Rig.IsValid()?Rig->GetSettings()->GetMenuButtonAction(Input):NAME_None));}),22)]]];
		}
		Scroll->AddSlot().Padding(0,6)[Button(Literal(TEXT("Restore default controller buttons")),[Rig](){if(Rig.IsValid())Rig->ResetButtonBindings();})];
		Scroll=Sections[4];
		Scroll->AddSlot().Padding(0,6)[Button(TAttribute<FText>::CreateLambda([]()
			{ return FText::FromString(ACERuntimeOptions::Get(TEXT("ShowFrameRate"))>.5f ? TEXT("FPS overlay: On") : TEXT("FPS overlay: Off")); }),
			[](){ACERuntimeOptions::Set(TEXT("ShowFrameRate"),ACERuntimeOptions::Get(TEXT("ShowFrameRate"))>.5f ? 0.f : 1.f);ACERuntimeOptions::Apply();})];
#if PLATFORM_ANDROID
		QuestSky.Refresh();
		auto SkyToggle = Button(TAttribute<FText>::CreateLambda([]()
		{
			return FText::FromString(ACERuntimeOptions::LegionsSky() ? TEXT("AC2 sky: On") : TEXT("AC2 sky: Off"));
		}), []()
		{
			ACERuntimeOptions::Set(TEXT("LegionsSky"), ACERuntimeOptions::LegionsSky() ? 0.f : 1.f);
			ACERuntimeOptions::Apply();
		});
		SkyToggle->SetEnabled(TAttribute<bool>::CreateLambda([](){ return QuestSky.HasCache && !QuestSky.Importing; }));
		Scroll->AddSlot().Padding(0,6)[SkyToggle];
		auto ImportSky = Button(Literal(TEXT("Import and use AC2 sky")), [](){ QuestSky.StartImport(); });
		ImportSky->SetVisibility(TAttribute<EVisibility>::CreateLambda([](){ return QuestSky.HasSource ? EVisibility::Visible : EVisibility::Collapsed; }));
		ImportSky->SetEnabled(TAttribute<bool>::CreateLambda([](){ return !QuestSky.Importing; }));
		Scroll->AddSlot().Padding(0,4)[ImportSky];
		Scroll->AddSlot()[Text(TAttribute<FText>::CreateLambda([]()
		{
			if (!QuestSky.Message.IsEmpty()) return FText::FromString(QuestSky.Message);
			if (QuestSky.NeedsWeather && !QuestSky.HasSource)
				return FText::FromString(TEXT("Transfer your Legions DAT again to add storm skies."));
			if (!QuestSky.HasCache && !QuestSky.HasSource)
				return FText::FromString(TEXT("On your PC, run Install-AC2-Sky.cmd to transfer your sky."));
			return FText::GetEmpty();
		}), 18)];
#endif
		Scroll=Sections[0];
		Scroll->AddSlot().Padding(0, 6)[Button(TAttribute<FText>::CreateLambda([Rig]()
			{ return FText::FromString(Rig.IsValid() && Rig->GetSettings()->bMatchCharacterHeight ? TEXT("Eye height: Character") : TEXT("Eye height: Physical")); }),
			[Rig]() { if (Rig.IsValid()) Rig->ChangeSetting(TEXT("CharacterHeight")); })];
		Scroll->AddSlot().Padding(0, 6)[Button(TAttribute<FText>::CreateLambda([Rig]()
			{ return FText::FromString(Rig.IsValid() && Rig->GetSettings()->bRun ? TEXT("Movement: Run") : TEXT("Movement: Walk")); }),
			[Rig]() { if (Rig.IsValid()) Rig->ChangeSetting(TEXT("Run")); })];
		for (FName Setting : {FName("Height"), FName("Panel"), FName("Distance"), FName("MenuRecenter"), FName("Vitals"), FName("CompassSize"), FName("OptionsSize"), FName("Chat"), FName("Wrist"), FName("Speed"), FName("ForwardAssist"), FName("TurnSpeed"), FName("Stability"), FName("HandPitch"), FName("Draw"), FName("BowAnchor")})
		{
			Scroll=Sections[(Setting=="Height" || Setting=="Speed" || Setting=="ForwardAssist" || Setting=="TurnSpeed")?0:
				(Setting=="HandPitch" || Setting=="Draw" || Setting=="BowAnchor" || Setting=="Stability")?2:1];
			Scroll->AddSlot().Padding(10, 8)[SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[Text(TAttribute<FText>::CreateLambda([Rig, Setting]()
				{
					if (!Rig.IsValid()) return FText::GetEmpty(); const auto* S = Rig->GetSettings();
					if (Setting == "Height") return FText::FromString(FString::Printf(TEXT("Eye height adjustment: %+.0f cm"), S->EyeHeightOffset));
					if (Setting == "Panel") return FText::FromString(FString::Printf(TEXT("Menu scale: %.0f%%"), S->PanelScale / .11f * 100));
					if (Setting == "Distance") return FText::FromString(FString::Printf(TEXT("Menu distance: %.0f cm"), S->PanelDistance));
					if (Setting == "MenuRecenter") return FText::FromString(FString::Printf(TEXT("Inventory recenter radius: %.0f degrees minimum"), S->MenuRecenterDegrees));
					if (Setting == "Wrist") return FText::FromString(FString::Printf(TEXT("Hotbar scale: %.0f%%"), S->WristScale / .06f * 100));
					if (Setting == "CompassSize") return FText::FromString(FString::Printf(TEXT("Compass scale: %.0f%%"), S->CompassScale / .065f * 100));
					if (Setting == "OptionsSize") return FText::FromString(FString::Printf(TEXT("VR options scale: %.0f%%"), S->OptionsScale / .1f * 100));
					if (Setting == "Vitals") return FText::FromString(FString::Printf(TEXT("Pinned vitals scale: %.0f%%"), S->VitalsScale / .0935f * 100));
					if (Setting == "Chat") return FText::FromString(FString::Printf(TEXT("Pinned chat scale: %.0f%%"), S->ChatScale / .0715f * 100));
					if (Setting == "ForwardAssist") return FText::FromString(FString::Printf(TEXT("Straight movement assistance: %.0f degrees"), S->ForwardAssistDegrees));
					if (Setting == "Speed") return FText::FromString(FString::Printf(TEXT("Movement speed: %.0f%%"), S->MovementScale * 100));
					if (Setting == "TurnSpeed") return FText::FromString(FString::Printf(TEXT("Smooth turn speed: %.0f degrees/sec"), S->SmoothTurnDegreesPerSecond));
					if (Setting == "HandPitch") return FText::FromString(FString::Printf(TEXT("Wrist pitch: %+.0f degrees"), S->HandPitch));
					if (Setting == "Draw") return FText::FromString(FString::Printf(TEXT("Full bow draw: %.0f cm"), S->BowFullDraw));
					if (Setting == "BowAnchor") return FText::FromString(FString::Printf(TEXT("Bow cheek-side offset: %.0f cm"), S->BowAnchorOffset));
					return FText::FromString(FString::Printf(TEXT("Wrist stabilization: %.0f ms"), S->WristSmoothing * 1000));
				}), 22)]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)[SNew(SBox).HeightOverride(36)
				[SNew(SSlider).Style(&MenuSliderStyle()).SliderBarColor(FLinearColor::White).SliderHandleColor(FLinearColor::White)
				.Tag(Setting)
				.IsEnabled_Lambda([Rig, Setting]() { return Rig.IsValid() && (Setting != "MenuRecenter" || Rig->GetSettings()->MenuAnchorMode == 1); })
				.Value_Lambda([Rig, Setting]() { return Rig.IsValid() ? Rig->GetSliderSetting(Setting) : 0.f; })
				.OnValueChanged_Lambda([Rig, Setting](float Value) { if (Rig.IsValid()) Rig->SetSliderSetting(Setting, Value); })
				.OnMouseCaptureEnd_Lambda([Rig]() { if (Rig.IsValid()) Rig->GetSettings()->Persist(); })]]];
			if (Setting == "MenuRecenter")
				Scroll->AddSlot().Padding(10,0,10,8)[Text(FText::FromString(TEXT("Body anchor only. Higher values delay recentering. The radius expands to fit wider panels.")),18)];
		}
		for (FName Setting : {FName("PinMenu"), FName("ShowWrist"), FName("PinHotbar"), FName("Compass"), FName("CompassAnchor"), FName("CompassLock"), FName("MenuLock"), FName("OptionsLock"), FName("PinVitals"), FName("VitalsAnchor"), FName("VitalsLock"), FName("PinChat"), FName("Turn"), FName("Angle"), FName("Hand"), FName("Movement"), FName("Seated"), FName("Body"), FName("Haptics"), FName("Render")})
		{
			Scroll=Sections[(Setting=="Turn" || Setting=="Angle" || Setting=="Movement" || Setting=="Seated" || Setting=="Body")?0:
				(Setting=="Hand" || Setting=="Haptics")?2:Setting=="Render"?4:1];
			Scroll->AddSlot().Padding(0, 3)[Button(TAttribute<FText>::CreateLambda([Rig, Setting]()
			{
				if (!Rig.IsValid()) return FText::GetEmpty(); const auto* S = Rig->GetSettings();
				FString Label;
				if (Setting == "Turn") Label = FString::Printf(TEXT("Turning: %s"), S->bSnapTurn ? TEXT("Snap") : TEXT("Smooth"));
				else if (Setting == "Angle") Label = FString::Printf(TEXT("Snap angle: %.0f degrees"), S->SnapDegrees);
				else if (Setting == "Hand") Label = FString::Printf(TEXT("Weapon hand: %s"), S->bLeftHanded ? TEXT("Left") : TEXT("Right"));
				else if (Setting == "Movement") Label = FString::Printf(TEXT("Movement direction: %s"), S->MovementDirection == 2 ? TEXT("None (stick only)") : S->MovementDirection == 1 ? TEXT("Left controller") : TEXT("Head"));
				else if (Setting == "Seated") Label = FString::Printf(TEXT("Play posture: %s"), S->bSeated ? TEXT("Seated") : TEXT("Standing"));
				else if (Setting == "Body") Label = FString::Printf(TEXT("Avatar: %s"), S->bShowBody ? TEXT("Body and arms") : TEXT("Arms only"));
				else if (Setting == "PinMenu") Label = FString::Printf(TEXT("Inventory anchor: %s"), S->MenuAnchorMode==0 ? TEXT("Head") : S->MenuAnchorMode==1 ? TEXT("Body") : TEXT("World"));
				else if (Setting == "PinHotbar") Label = FString::Printf(TEXT("Retail hotbar pinned to: %s"), S->bPinHotbarToView ? TEXT("View") : TEXT("Left wrist"));
				else if (Setting == "ShowWrist") Label = FString::Printf(TEXT("Wrist spell bar: %s"), S->bShowWristSpellBar ? TEXT("Shown") : TEXT("Hidden (use spell wheel)"));
				else if (Setting == "Compass") Label = FString::Printf(TEXT("Compass and coordinates: %s"), S->bShowCompass ? TEXT("Shown") : TEXT("Hidden"));
				else if (Setting == "CompassAnchor") Label = S->CompassAnchorMode == 0 ? TEXT("Compass anchor: Head") : S->CompassAnchorMode == 1 ? TEXT("Compass anchor: Body (stable)") : TEXT("Compass anchor: World (Recenter to bring back)");
				else if (Setting == "CompassLock") Label = S->bCompassLocked ? TEXT("Compass placement: Locked") : TEXT("Compass placement: Unlocked / Move or Resize below compass");
				else if (Setting == "MenuLock") Label = S->bMenuLocked ? TEXT("Main menu placement: Locked") : TEXT("Main menu placement: Unlocked / Move or Resize below menu");
				else if (Setting == "OptionsLock") Label = S->bOptionsLocked ? TEXT("VR options placement: Locked") : TEXT("VR options placement: Unlocked / Move or Resize below menu");
				else if (Setting == "PinVitals") Label = FString::Printf(TEXT("Show pinned vitals: %s"), S->bPinVitalsToView ? TEXT("On") : TEXT("Off"));
				else if (Setting == "VitalsLock") Label = S->bVitalsLocked ? TEXT("Vitals placement: Locked") : TEXT("Vitals placement: Unlocked / drag bar to move");
                else if (Setting == "PinChat") Label = FString::Printf(TEXT("Chat anchor: %s"), S->ChatAnchorMode==0?TEXT("Head"):S->ChatAnchorMode==1?TEXT("Body"):TEXT("World"));
				else if (Setting == "Haptics") Label = FString::Printf(TEXT("Haptics: %s"), S->bHaptics ? TEXT("On") : TEXT("Off"));
				else if (Setting == "Panel") Label = FString::Printf(TEXT("Menu size: %.0f%%"), S->PanelScale / .11f * 100.f);
				else if (Setting == "Distance") Label = FString::Printf(TEXT("Menu distance: %.0f cm"), S->PanelDistance);
				else if (Setting == "Wrist") Label = FString::Printf(TEXT("Wrist menu size: %.0f%%"), S->WristScale / .06f * 100.f);
				else if (Setting == "VitalsAnchor") Label = S->VitalsAnchorMode == 0 ? TEXT("Vitals anchor: View") : S->VitalsAnchorMode == 1 ? TEXT("Vitals anchor: Body") : TEXT("Vitals anchor: World (Recenter to bring back)");
				else if (Setting == "Render") Label = FString::Printf(TEXT("Render resolution: %.0f%%"), S->RenderScale);
				return FText::FromString(Label);
			}), [Rig, Setting]() { if (Rig.IsValid()) Rig->ChangeSetting(Setting); })];
		}
		Scroll=Sections[4];
		if (UACEVRSettings::SupportsMSAASettings())
			Scroll->AddSlot().Padding(0, 3)[Button(TAttribute<FText>::CreateLambda([Rig]()
			{
				return FText::FromString(Rig.IsValid() ? FString::Printf(TEXT("Edge smoothing: %dx MSAA (%s)"),
					Rig->GetSettings()->MSAASamples, Rig->GetSettings()->MSAASamples == 4 ? TEXT("Quality") : TEXT("Performance")) : FString());
			}), [Rig]() { if (Rig.IsValid()) Rig->ChangeSetting(TEXT("EdgeSmoothing")); })];
		List->AddSlot().FillHeight(1.f)[Switcher];
		List->AddSlot().AutoHeight().Padding(0, 6)[Button(Literal(TEXT("Recenter / Calibrate height")), [Rig]() { if (Rig.IsValid()) Rig->ResetTrackingOrigin(); })];
#if !PLATFORM_ANDROID
		List->AddSlot().AutoHeight()[Button(Literal(TEXT("Keyboard")), [Rig]() { if (Rig.IsValid()) Rig->ToggleKeyboard(); })];
#endif
		List->AddSlot().AutoHeight().Padding(0, 6)[Button(Literal(TEXT("Return to game")), [Rig]() { if (Rig.IsValid()) Rig->ToggleSettings(); })];
		List->AddSlot().AutoHeight()[Text(Literal(TEXT("Settings save automatically. Customize controller actions in Controls.")), 18)];
	}
	return SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(MenuGold).Padding(2.f)
		[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.008f, .007f, .004f)).Padding(2.f)
		[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.22f, .16f, .065f)).Padding(1.f)
		[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FLinearColor(.026f, .024f, .014f, 1.f)).Padding(bKeyboard ? 8.f : 18.f)[List]]]];
}
