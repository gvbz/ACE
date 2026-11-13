#include "UI/ACEUIGameplayBinder.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEActionDeckSubsystem.h"
#include "UI/ACERetailVendorCategories.h"
#include "ACERetailChat.h"
#include "ACESpellbookFilters.h"
#include "UI/ACERadarColors.h"
#include "ACEHoverTooltipWidget.h"
#include "ACEVendorPricing.h"
#include "Components/ComboBoxString.h"
#include "ACERadarVisuals.h"
#include "UI/ACEChatEntry.h"
#include "ACEEquipmentRules.h"
#include "UI/ACERetailTextEntry.h"
#include "ACEProfiling.h"
#include "Misc/ScopeExit.h"
#include "UI/ACEVideoSettingsWidget.h"
#include "Components/MultiLineEditableText.h"
#include "ACEAppraisalFormatting.h"
#include "ACEInputBindings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UI/ACECaptureImage.h"
#include "ACERetailPaperDoll.h"
#include "ACERetailObjectNames.h"
#include "ACEInventoryRules.h"
#include "ACECharacterOptions.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIElement.h"
#include "UI/ACEUITypes.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEPlayerController.h"
#include "VR/ACEVRComponent.h"
#include "UI/ACEUICharGenBinder.h"
#include "ACEWorldEntityActor.h"
#include "ACEWorldPresenterComponent.h"
#include "GameFramework/GameModeBase.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACECombatStance.h"
#include "ACEOpcodes.h"
#include "ACEMotionCommandNames.generated.h"
#include "ACESession.h"
#include "Components/Border.h"
#include "Components/ProgressBar.h"
#include "Components/Image.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/TextBlock.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SizeBox.h"
#include "Components/VerticalBox.h"
#include "Components/CapsuleComponent.h"
#include "Components/MeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/SlateBlueprintLibrary.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "UI/ACEUIFontStyles.h"
#include "Dat/ACEDatTextLayout.h"
#include "Misc/App.h"
#include "HAL/PlatformTime.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Optional.h"
#include "Algo/Sort.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Templates/TypeHash.h"

#include "Protocol/ACECharacterTitleNames.inl"

namespace
{
	AACEWorldEntityActor* FindSelectionActor(UWorld* World, int32 Guid)
	{
		if (!World || !Guid) return nullptr;
		if (auto* Mode = World->GetAuthGameMode())
			if (auto* Presenter = Mode->FindComponentByClass<UACEWorldPresenterComponent>())
				return Presenter->FindEntityActor(Guid);
		// Standalone preview/automation worlds can construct entities without
		// the game's presenter. Normal gameplay always uses its lifetime index.
		for (TActorIterator<AACEWorldEntityActor> It(World); It; ++It)
			if (It->GetACEGuid() == Guid) return *It;
		return nullptr;
	}

	struct FSpellbookRowTemplate
	{
		TSharedPtr<FACEUIElement> Root, Icon, Text, Selected;
		FSpellbookRowTemplate()
		{
			// UI_ItemList_ItemSlotID in SpellBook_SpellList selects this retail template.
			Root = UACEUILayoutResolver::LoadTemplate(0x21000037, 0x10000343);
			if (Root) for (const auto& Child : Root->Children)
			{
				if (Child->ElementName == TEXT("ItemSlot_Icon")) Icon = Child;
				if (Child->ElementName == TEXT("ItemSlot_Text")) Text = Child;
				if (Child->ElementName == TEXT("ItemSlot_Icon_Selected")) Selected = Child;
			}
		}
		int32 Height() const { return Root ? FMath::Max(1, Root->Height) : 32; }
	};
	const FSpellbookRowTemplate& SpellbookTemplate()
	{
		static const FSpellbookRowTemplate Value;
		return Value;
	}
	const FLinearColor HealthColor(0.75f, 0.12f, 0.12f, 0.92f);
	const FLinearColor StaminaColor(0.75f, 0.65f, 0.1f, 0.92f);
	const FLinearColor ManaColor(0.15f, 0.35f, 0.85f, 0.92f);
	const FLinearColor SlotEmptyColor(0.05f, 0.05f, 0.06f, 0.55f);
	const FLinearColor TextGold(0.92f, 0.82f, 0.45f, 1.f);
	const FLinearColor TextWhite(0.95f, 0.95f, 0.9f, 1.f);
	const FLinearColor TextGreen(0.35f, 0.85f, 0.40f, 1.f);
	const FLinearColor TextRed(0.90f, 0.22f, 0.18f, 1.f);
	/** Overlays must sit above DAT chrome (InOutZOrder routinely exceeds 500). */
	constexpr int32 StatOverlayZ = 100000;
	FString FormatGenderHeritageTitle(const FACEPlayerVitals& V)
	{
		FString Gender;
		if (V.Gender == 1) { Gender = TEXT("Male"); }
		else if (V.Gender == 2) { Gender = TEXT("Female"); }
		FString Heritage;
		switch (V.HeritageGroup)
		{
		case 1: Heritage = TEXT("Aluvian"); break;
		case 2: Heritage = TEXT("Gharu'ndim"); break;
		case 3: Heritage = TEXT("Sho"); break;
		case 4: Heritage = TEXT("Viamontian"); break;
		case 5: Heritage = TEXT("Umbraen"); break;
		case 6: Heritage = TEXT("Gearknight"); break;
		case 7: Heritage = TEXT("Tumerok"); break;
		case 8: Heritage = TEXT("Lugian"); break;
		case 9: Heritage = TEXT("Empyrean"); break;
		case 10: Heritage = TEXT("Penumbraen"); break;
		case 11: Heritage = TEXT("Undead"); break;
		case 12:
		case 13: Heritage = TEXT("Olthoi"); break;
		default: break;
		}
		FString Line;
		if (!Gender.IsEmpty()) { Line = Gender; }
		if (!Heritage.IsEmpty())
		{
			Line = Line.IsEmpty() ? Heritage : (Line + TEXT(" ") + Heritage);
		}
		if (!V.TemplateName.IsEmpty())
		{
			Line = Line.IsEmpty() ? V.TemplateName : (Line + TEXT(" ") + V.TemplateName);
		}
		return Line;
	}
	FString FormatPkStatus(int32 Status)
	{
		if ((Status & 0x04) != 0) { return TEXT("Player Killer"); }
		if ((Status & 0x40) != 0) { return TEXT("Player Killer Lite"); }
		if ((Status & 0x20) != 0) { return TEXT("Free"); }
		return TEXT("Non-Player Killer");
	}
	FString FormatXpNumber(int64 Value)
	{
		return FText::AsNumber(Value).ToString();
	}
	/** Retail selected-item / stack label: "10,200 Pyreals" or "10637 Pyreals (of 10637)". */
	FString FormatItemStackName(const FACEWorldObject& Obj, int32 AmountOverride = INDEX_NONE)
	{
		const int32 TotalRaw = Obj.StackSize > 0 ? Obj.StackSize : 1;
		const int32 Total = (Obj.MaxStackSize > 0) ? FMath::Min(TotalRaw, Obj.MaxStackSize) : TotalRaw;
		if (Total <= 1)
		{
			return ACERetailObjectNames::Name(Obj);
		}
		const int32 Amt = (AmountOverride > 0) ? FMath::Clamp(AmountOverride, 1, Total) : Total;
		const FString Name = ACERetailObjectNames::Name(Obj, Amt != 1);
		if (Amt < Total)
		{
			return FString::Printf(TEXT("%s %s (of %s)"),
				*FormatXpNumber(Amt), *Name, *FormatXpNumber(Total));
		}
		return FString::Printf(TEXT("%s %s"), *FormatXpNumber(Amt), *Name);
	}
	/** Retail MotionCommand.Pickup — play locally when looting (server also broadcasts). */
	void PlayLocalPickupMotion(AACEPlayerController* PC)
	{
		if (!PC)
		{
			return;
		}
		if (APawn* Pawn = PC->GetPawn())
		{
			if (UACECharacterAppearanceComponent* App =
				Pawn->FindComponentByClass<UACECharacterAppearanceComponent>())
			{
				App->PlayActionMotion(static_cast<int32>(0x40000018));
			}
		}
	}

	/** Fill the retail ordered item list, followed by the remaining empty grid cells. */
	void BuildPackSlotGuids(const TArray<FACEWorldObject>& Items, int32 Capacity,
		TArray<int32>& OutSlotGuids, TArray<FACEWorldObject>& OutSlotObjs)
	{
		// Retail IDList is dense; empty grid cells follow the last server item.
		OutSlotGuids.Init(0, Capacity);
		OutSlotObjs.SetNum(Capacity);
		for (int32 I = 0; I < FMath::Min(Items.Num(), Capacity); ++I)
		{
			OutSlotGuids[I] = Items[I].Guid;
			OutSlotObjs[I] = Items[I];
		}
	}
	using ACEVendorPricing::VendorSellCost;
	using ACEVendorPricing::VendorBuyPayout;
	/** Retail ItemSlot_Generic empty cell / selected overlay (LayoutDesc 0x21000037). */
	constexpr int32 DidInvSlotBg = 0x06004D20;
	constexpr int32 DidInvSlotSelected = 0x06004D09;
	/** ACE.Entity.Enum.UiEffects — tint inventory cell behind magical/elemental icons. */
	namespace ACEUiEffects
	{
		constexpr uint32 Magical = 0x0001;
		constexpr uint32 Poisoned = 0x0002;
		constexpr uint32 BoostHealth = 0x0004;
		constexpr uint32 BoostMana = 0x0008;
		constexpr uint32 BoostStamina = 0x0010;
		constexpr uint32 Fire = 0x0020;
		constexpr uint32 Lightning = 0x0040;
		constexpr uint32 Frost = 0x0080;
		constexpr uint32 Acid = 0x0100;
		constexpr uint32 Bludgeoning = 0x0200;
		constexpr uint32 Slashing = 0x0400;
		constexpr uint32 Piercing = 0x0800;
		constexpr uint32 Nether = 0x1000;
	}
	bool TryUiEffectTint(uint32 Effects, FLinearColor& OutTint)
	{
		if (Effects == 0)
		{
			return false;
		}
		// Match retail priority: Magical blue is the common loot glow.
		if (Effects & ACEUiEffects::Magical)
		{
			OutTint = FLinearColor(0.20f, 0.55f, 1.0f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::Fire)
		{
			OutTint = FLinearColor(1.0f, 0.35f, 0.10f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::Frost)
		{
			OutTint = FLinearColor(0.45f, 0.85f, 1.0f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::Lightning)
		{
			OutTint = FLinearColor(0.70f, 0.40f, 1.0f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::Acid)
		{
			OutTint = FLinearColor(0.35f, 0.90f, 0.25f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::Nether)
		{
			OutTint = FLinearColor(0.45f, 0.15f, 0.55f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::Poisoned)
		{
			OutTint = FLinearColor(0.25f, 0.75f, 0.20f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::BoostHealth)
		{
			OutTint = FLinearColor(0.95f, 0.20f, 0.20f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::BoostStamina)
		{
			OutTint = FLinearColor(0.95f, 0.80f, 0.15f, 1.f);
			return true;
		}
		if (Effects & ACEUiEffects::BoostMana)
		{
			OutTint = FLinearColor(0.25f, 0.45f, 0.95f, 1.f);
			return true;
		}
		if (Effects & (ACEUiEffects::Bludgeoning | ACEUiEffects::Slashing | ACEUiEffects::Piercing))
		{
			OutTint = FLinearColor(0.85f, 0.85f, 0.85f, 1.f);
			return true;
		}
		return false;
	}
	/** SmallRoundCheckBox: off / pressed (light green) / selected (full green).
	 *  0x06004D17 is a large fill atlas tile — do not use it as the checkbox glyph. */
	constexpr int32 DidCheckboxOff = 0x06004D15;
	constexpr int32 DidCheckboxPressed = 0x06004D16;
	constexpr int32 DidCheckboxOn = 0x06004D18;
	constexpr int32 DidCombatBtn = 0x06004D1C;
	constexpr int32 DidEmptyPackSlot = 0x06000F6E; // ItemSlot_Container, not toolbar backpack art.
	constexpr int32 DidAttrIconStrength = 0x060002C8;
	constexpr int32 DidAttrIconEndurance = 0x060002C4;
	constexpr int32 DidAttrIconQuickness = 0x060002C6;
	constexpr int32 DidAttrIconCoordination = 0x060002C9;
	constexpr int32 DidAttrIconFocus = 0x060002C5;
	constexpr int32 DidAttrIconSelf = 0x060002C7;
	constexpr int32 DidAttrIconHealth = 0x06004C3B;
	constexpr int32 DidAttrIconStamina = 0x06004C3C;
	constexpr int32 DidAttrIconMana = 0x06004C3D;
	constexpr int32 DidListInfoBg = 0x06000F93;
	constexpr int32 DidSkillHeaderSpec = 0x06000F90;
	constexpr int32 DidSkillHeaderTrain = 0x06000F86;
	constexpr int32 DidSkillHeaderUntrain = 0x06000F98;
	/** Raise triangles: lit (available) vs dark (unavailable). */
	constexpr uint32 DidRaise1Lit = 0x06004CB6;
	constexpr uint32 DidRaise1Dark = 0x06004CB7;
	constexpr uint32 DidRaise10Lit = 0x0600712B;
	constexpr uint32 DidRaise10Dark = 0x0600712C;

	struct FDollSlotMap
	{
		const TCHAR* Name;
		int64 Mask;
		/** Retail ItemSlot_Equip_* empty silhouette (LayoutDesc 0x21000037). */
		int32 EmptyIconDid;
	};

	/**
	 * Doll slot → preferred EquipMask for GetAndWield.
	 * Mag-nus gmPaperDollUI::GetLocationInfoFromElementID uses single bits for pants
	 * (UpperLegWear=0x40) and shirt (ChestWear=0x2). Display matching still uses
	 * ItemMatchesDollSlot for multi-bit CurrentWieldedLocation.
	 */
	const FDollSlotMap GDollSlots[] = {
		{ TEXT("Inv_HeadSlot"), ACEEquipMask::HeadWear, 0x06006D7F },
		{ TEXT("Inv_ChestSlot"), ACEEquipMask::ChestArmor, 0x06006D7B },
		{ TEXT("Inv_AbdomenSlot"), ACEEquipMask::AbdomenArmor, 0x06006D79 },
		{ TEXT("Inv_UpperArmSlot"), ACEEquipMask::UpperArmArmor, 0x06006D87 },
		{ TEXT("Inv_LowerArmSlot"), ACEEquipMask::LowerArmArmor, 0x06006D81 },
		{ TEXT("Inv_HandSlot"), ACEEquipMask::HandWear, 0x06006D7D },
		{ TEXT("Inv_UpperLegSlot"), ACEEquipMask::UpperLegArmor, 0x06006D89 },
		{ TEXT("Inv_LowerLegSlot"), ACEEquipMask::LowerLegArmor, 0x06006D83 },
		// Mag-nus pants element → UpperLegWear only (0x40).
		{ TEXT("Inv_ClothesPantsSlot"), ACEEquipMask::UpperLegWear, 0x060032C4 },
		{ TEXT("Inv_FootSlot"), ACEEquipMask::FootWear, 0x06006D85 },
		{ TEXT("Inv_NeckSlot"), ACEEquipMask::NeckWear, 0x06000F68 },
		{ TEXT("Inv_LeftWristSlot"), ACEEquipMask::WristWearLeft, 0x06000F5D },
		{ TEXT("Inv_RightWristSlot"), ACEEquipMask::WristWearRight, 0x06000F6A },
		{ TEXT("Inv_LeftRingSlot"), ACEEquipMask::FingerWearLeft, 0x06000F5A },
		{ TEXT("Inv_RightRingSlot"), ACEEquipMask::FingerWearRight, 0x06000F6B },
		{ TEXT("Inv_WeaponReadySlot"), ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded
			| ACEEquipMask::Held | ACEEquipMask::MissileWeapon, 0x06000F66 },
		{ TEXT("Inv_ShieldReadySlot"), ACEEquipMask::Shield, 0x06000F6C },
		{ TEXT("Inv_AmmoReadySlot"), ACEEquipMask::MissileAmmo, 0x06000F5E },
		// Mag-nus shirt element → ChestWear only (0x2).
		{ TEXT("Inv_ClothesShirtSlot"), ACEEquipMask::ChestWear, 0x060032C5 },
		{ TEXT("Inv_TrinketOneSlot"), ACEEquipMask::TrinketOne, 0x06006A6C },
		{ TEXT("Inv_CloakSlot"), ACEEquipMask::Cloak, 0x0600708F },
		{ TEXT("Inv_SigilOneSlot"), ACEEquipMask::SigilOne, 0x06006BEF },
		{ TEXT("Inv_SigilTwoSlot"), ACEEquipMask::SigilTwo, 0x06006BF0 },
		{ TEXT("Inv_SigilThreeSlot"), ACEEquipMask::SigilThree, 0x06006BF1 },
	};

	/** Main-body armor only — Show Equipment toggles these; jewelry/clothes/weapons stay. */
	bool IsMainBodyDollSlot(const TCHAR* SlotName)
	{
		return FCString::Strcmp(SlotName, TEXT("Inv_HeadSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_ChestSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_AbdomenSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_UpperArmSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_LowerArmSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_HandSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_UpperLegSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_LowerLegSlot")) == 0
			|| FCString::Strcmp(SlotName, TEXT("Inv_FootSlot")) == 0;
	}

	/** Match equipped items to doll silhouettes (display). */
	bool ItemMatchesDollSlot(const FACEWorldObject& Obj, int64 SlotMask)
	{
		const int64 Loc = Obj.CurrentWieldedLocation;
		if (Loc == 0)
		{
			return false;
		}
		constexpr int64 ClothesLegs = ACEEquipMask::UpperLegWear | ACEEquipMask::LowerLegWear;
		constexpr int64 Foot = ACEEquipMask::FootWear;
		constexpr int64 ShirtCore = ACEEquipMask::ChestWear
			| ACEEquipMask::UpperArmWear | ACEEquipMask::LowerArmWear;
		constexpr int64 ArmorBits = ACEEquipMask::ChestArmor | ACEEquipMask::AbdomenArmor
			| ACEEquipMask::UpperArmArmor | ACEEquipMask::LowerArmArmor
			| ACEEquipMask::UpperLegArmor | ACEEquipMask::LowerLegArmor;

		// Pants preferred bit is UpperLegWear — also accept LowerLegWear-only trousers.
		if (SlotMask == ACEEquipMask::UpperLegWear)
		{
			return (Loc & ClothesLegs) != 0 && (Loc & Foot) == 0 && (Loc & ArmorBits) == 0;
		}
		// Shirt preferred bit is ChestWear — require shirt-core coverage, never armor/pants.
		if (SlotMask == ACEEquipMask::ChestWear)
		{
			if ((Loc & ArmorBits) != 0) { return false; }
			if ((Loc & ClothesLegs) != 0 && (Loc & Foot) == 0) { return false; }
			return (Loc & ShirtCore) != 0;
		}
		if ((Loc & SlotMask) == 0)
		{
			return false;
		}
		if (SlotMask == Foot && (Loc & ClothesLegs) != 0 && (Loc & Foot) == 0)
		{
			return false;
		}
		return true;
	}

	FSlateBrush MakeSolidBrush(const FLinearColor& Color)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::Box;
		Brush.TintColor = FSlateColor(Color);
		Brush.Margin = FMargin(0.f);
		return Brush;
	}

	FSlateBrush MakeRoundBlipBrush(const FLinearColor& Color)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings.CornerRadii = FVector4(4.f, 4.f, 4.f, 4.f);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		return Brush;
	}
}

void UACEUIGameplayBinder::Initialize(UACEClientSubsystem* InClient, UACEUIElementManager* InManager,
	UACEUICanvasWidget* InCanvas, AACEPlayerController* InPC)
{
	Shutdown();
	Client = InClient;
	const uint32 Filters = Client && Client->GetSession() ? Client->GetSession()->GetSpellbookFilters() : 0x3FFFu;
	SpellbookSchoolFilterMask = ACESpellbookFilters::Schools(Filters);
	SpellbookLevelFilterMask = ACESpellbookFilters::Levels(Filters);
	Manager = InManager;
	Canvas = InCanvas;
	PlayerController = InPC;
	CombatMode = static_cast<int32>(ACECombatMode::NonCombat);
	ActivePanelPage.Reset();
	SelectedPackGuid = 0;
	InventoryScrollOffset = 0;

	if (Manager)
	{
		ActivatedHandle = Manager->OnElementActivated.AddUObject(this, &UACEUIGameplayBinder::OnElementActivated);
	}
	if (Client)
	{
		Client->OnRoutedChatMessage.AddDynamic(this, &UACEUIGameplayBinder::HandleChatMessage);
		Client->OnAppraisal.AddDynamic(this, &UACEUIGameplayBinder::HandleAppraisal);
		Client->OnSelectionChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleSelectionChanged);
		Client->OnVitalsUpdated.AddDynamic(this, &UACEUIGameplayBinder::HandleVitalsUpdated);
		Client->OnExternalContainerOpened.AddDynamic(this, &UACEUIGameplayBinder::HandleExternalContainerOpened);
		Client->OnExternalContainerClosed.AddDynamic(this, &UACEUIGameplayBinder::HandleExternalContainerClosed);
		Client->OnVendorOpened.AddDynamic(this, &UACEUIGameplayBinder::HandleVendorOpened);
		Client->OnTradeStateChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleTradeStateChanged);
		Client->OnChessEvent.AddDynamic(this, &UACEUIGameplayBinder::HandleChessEvent);
		Client->OnCharacterTitlesChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleCharacterTitlesChanged);
		Client->OnFellowshipChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleFellowshipChanged);
		Client->OnAllegianceChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleAllegianceChanged);
		Client->OnFriendsChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleFriendsChanged);
		Client->OnContractsChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleContractsChanged);
		Client->OnEnchantmentsChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleEnchantmentsChanged);
		Client->OnSpellLearned.AddDynamic(this, &UACEUIGameplayBinder::HandleSpellLearned);
		Client->OnLinkStatusChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleLinkStatusChanged);
		Client->OnBookChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleBookChanged);
		LastVitals = Client->GetPlayerVitals();
		LastLinkStatus = Client->GetLinkStatus();
		LastSelection = Client->GetSelectedObject();
		SelectedPackGuid = Client->GetPlayerGuid();
		if (LastVitals.bValid && LastVitals.CombatMode != 0)
		{
			CombatMode = LastVitals.CombatMode;
		}
	}
	LoadVitalDisplayPreference();
	bBound = true;
	SanitizeFloatyPanelChrome();
	if (Manager)
	{
		Manager->SetElementVisibleByName(TEXT("RootGameplay_Keyboard_Field"), false);
		Manager->SetElementVisibleByName(TEXT("RootKeyboard_Field"), false);
	}
	ApplyPreferredStance(CombatMode);
	SyncCombatModeButtons();
	SyncSpellcastTabChrome();
	SyncInventoryButtonVisual();
	RefreshStatusIndicators();
	EnsureOverlays();
	RefreshStatusIndicators();
	if (Client) for (const auto& Message : Client->GetChatHistory())
	{
		TGuardValue<uint8> WindowsScope(Client->CurrentChatWindows, Message.ForcedWindows);
		TGuardValue<TOptional<FLinearColor>> ColorScope(Client->CurrentChatColor, Message.Color);
		HandleChatMessage(Message.Text, Message.Sender, Message.Type, Message.FilterType);
	}
}

void UACEUIGameplayBinder::Shutdown()
{
	if (CombatAmmoLabel) CombatAmmoLabel->RemoveFromParent();
	CombatAmmoLabel = nullptr;
	ResetChessGame();
	for(auto& Lines:ChatDisplayLines)Lines.Reset();
	++ChatDisplayRevision;
	CancelGameplayScreenshot();
	SelectionSpellTab = INDEX_NONE;
	for (auto& Tab : SpellTabSelections) Tab = {};
	SelectedCombatSpellSlot = SpellHotbarScrollOffset = 0;
	AttributeScrollOffset = 0;
	bRevealSelectedSpell = true;
	AutoLayoutSession.Reset(); AutoLayoutPlayer = 0; AutoLayoutSize = FIntPoint::ZeroValue;
	CancelPendingUseWith();
	KeyboardOpenedCorpses.Reset();
	if(Barber){Barber->Shutdown();Barber=nullptr;}
	if(Canvas)Canvas->SetBarberBinder(nullptr);
	SeenBarberRevision=0;
	if (OpenHouseLord) CloseHouseOffer();
	for (auto* Array : {&HouseOfferSlots,&HouseOfferBackgrounds,&HouseOfferSelections})
	{
		for (auto& B : *Array) if (B) B->RemoveFromParent();
		Array->Reset();
	}
	for (auto& L : HouseOfferLabels) if (L) L->RemoveFromParent();
	HouseOfferLabels.Reset(); SeenHouseRevision=0;
	if (StackAmountEntry) StackAmountEntry->RemoveFromParent();
	StackAmountEntry=nullptr; StackAmountEntryGuid=0;
	if (KeymapReportScroll) KeymapReportScroll->RemoveFromParent();
	KeymapReportScroll=nullptr; KeymapReportText=nullptr; bKeymapReport=false;
	SelectionFlashUntil=0;
	ToolbarSelectionFlashUntil=0;
	TickSelectionFlash();
	bActionDeckCombatBarHidden=false;
	ACEInputBindings::Cancel();
	for (UWidget* Row:KeyboardRows) if (Row) Row->RemoveFromParent();
	for (UTextBlock* Label:KeyboardLabels) if (Label) Label->RemoveFromParent();
	KeyboardRows.Reset(); KeyboardLabels.Reset();
	for (UTextBlock* Label:KeyboardRowLabels) if(Label)Label->RemoveFromParent();
	for (UTextBlock* Label:KeyboardKeyLabels) if(Label)Label->RemoveFromParent();
	if(KeyboardGroupLabel)KeyboardGroupLabel->RemoveFromParent();
	if(KeymapFileChoice)KeymapFileChoice->RemoveFromParent();
	if(KeymapImportPath)KeymapImportPath->RemoveFromParent();
	for(UTextBlock* Label:KeymapDialogLabels)if(Label)Label->RemoveFromParent();
	KeyboardRowLabels.Reset();KeyboardKeyLabels.Reset();KeyboardEntryElements.Reset();
	KeyboardGroupElement.Reset();KeyboardGroupLabel=nullptr;KeymapFileChoice=nullptr;KeymapImportPath=nullptr;KeymapDialog.Reset();KeymapDialogLabels.Reset();
	if (VideoSettings) VideoSettings->RemoveFromParent();
	VideoSettings=nullptr;
	CancelInventoryDrag();
	CancelSpellDrag();
	LastSpellbookClickId = 0;
	LastSpellbookClickTime = 0.0;
	ReleasePaperDollPreview();
	ReleaseExamPaperDollPreview();
	if (ExamCreatureDetailsScroll) ExamCreatureDetailsScroll->RemoveFromParent();
	ExamCreatureDetailsScroll = nullptr; ExamCreatureDetailsCanvas = nullptr; ExamCreatureDetailsSize = nullptr;
	ExamMiscLabels.Reset(); ExamMiscValues.Reset(); ExamMiscRows.Reset(); ExamCreatureScrolledGuid = 0;
	for (UTextBlock* Text : ExamAttributeLabels) if (Text) Text->RemoveFromParent();
	for (UTextBlock* Text : ExamAttributeValues) if (Text) Text->RemoveFromParent();
	if (ExamLevel) ExamLevel->RemoveFromParent();
	for (UTextBlock* Text : ExamCreatureHeadings) if (Text) Text->RemoveFromParent();
	ExamCreatureHeadings.Reset();
	for (const auto& Row : ExamAttributeRows)
		if (const auto Parent = Row->Parent.Pin()) Parent->RemoveChild(Row);
	ExamAttributeLabels.Reset(); ExamAttributeValues.Reset(); ExamAttributeRows.Reset(); ExamLevel = nullptr;
	if (Manager && ActivatedHandle.IsValid())
	{
		Manager->OnElementActivated.Remove(ActivatedHandle);
		ActivatedHandle.Reset();
	}
	if (Client && bBound)
	{
		Client->OnRoutedChatMessage.RemoveDynamic(this, &UACEUIGameplayBinder::HandleChatMessage);
		Client->OnAppraisal.RemoveDynamic(this, &UACEUIGameplayBinder::HandleAppraisal);
		Client->OnSelectionChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleSelectionChanged);
		Client->OnVitalsUpdated.RemoveDynamic(this, &UACEUIGameplayBinder::HandleVitalsUpdated);
		Client->OnExternalContainerOpened.RemoveDynamic(this, &UACEUIGameplayBinder::HandleExternalContainerOpened);
		Client->OnExternalContainerClosed.RemoveDynamic(this, &UACEUIGameplayBinder::HandleExternalContainerClosed);
		Client->OnVendorOpened.RemoveDynamic(this, &UACEUIGameplayBinder::HandleVendorOpened);
		Client->OnTradeStateChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleTradeStateChanged);
		Client->OnChessEvent.RemoveDynamic(this, &UACEUIGameplayBinder::HandleChessEvent);
		Client->OnCharacterTitlesChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleCharacterTitlesChanged);
		Client->OnFellowshipChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleFellowshipChanged);
		Client->OnAllegianceChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleAllegianceChanged);
		Client->OnFriendsChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleFriendsChanged);
		Client->OnContractsChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleContractsChanged);
		Client->OnEnchantmentsChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleEnchantmentsChanged);
		Client->OnSpellLearned.RemoveDynamic(this, &UACEUIGameplayBinder::HandleSpellLearned);
		Client->OnLinkStatusChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleLinkStatusChanged);
		Client->OnBookChanged.RemoveDynamic(this, &UACEUIGameplayBinder::HandleBookChanged);
	}
	bBound = false;
	bRevealLearnedSpell = false;
	if (TransientInfoBox) TransientInfoBox->RemoveFromParent();
	TransientInfoBox = nullptr;
	TransientInfoText = nullptr;
	TransientInfoLines.Reset();
	TransientInfoTimes.Reset();
	TransientInfoTemplate.Reset();
	TransientInfoScale = FVector2D::ZeroVector;
	for (auto* Root : {&ServerConfirmRoot, &ServerPopupRoot})
	{
		if (*Root)
		{
			(*Root)->bVisible = false;
			if (auto Parent = (*Root)->Parent.Pin()) Parent->RemoveChild(*Root);
			Root->Reset();
		}
	}
	for (auto* Labels : {&ServerConfirmLabels, &ServerPopupLabels})
	{
		for (UTextBlock* Label : *Labels) if (Label) Label->RemoveFromParent();
		Labels->Reset();
	}
	ServerPopupId = 0; ServerPopupPrompt.Reset(); ServerConfirmPrompt.Reset();
	Client = nullptr;
	Manager = nullptr;
	Canvas = nullptr;
	PlayerController = nullptr;
	HideSelectionMarkers();
	SelectionMarkerBox = nullptr; SelectionDirectionArrow = nullptr;
	SelectionMarkerCorners.Reset();
}

void UACEUIGameplayBinder::TickSelectionFlash()
{
	bool bCaptureDoll = false;
	const float Amount = FMath::Clamp(float((SelectionFlashUntil - FPlatformTime::Seconds()) / .4), 0.f, 1.f);
	for (int32 I = 0; I < FlashMeshes.Num(); ++I)
		if (auto* Mesh = FlashMeshes[I].Get(); Mesh && Mesh->GetMaterial(FlashMaterialSlots[I]) == FlashMaterials[I])
		{
			bCaptureDoll |= Mesh->GetOwner() == PaperDollPreviewActor;
			if (Amount <= 0) Mesh->SetMaterial(FlashMaterialSlots[I], FlashOriginalMaterials[I]);
			else
			{
				float Base = 1;
				FlashOriginalMaterials[I]->GetScalarParameterValue(TEXT("EmissiveStrength"), Base);
				FlashMaterials[I]->SetScalarParameterValue(TEXT("EmissiveStrength"), Mesh->GetOwner() == PaperDollPreviewActor ? (Amount > .5f ? Base + 1.f : Base * .6f) : Base + Amount);
			}
		}
	if (bCaptureDoll && PaperDollCapture && !bShowPaperdollSlots) PaperDollCapture->CaptureScene();
	if (Amount <= 0)
	{
		FlashMeshes.Reset(); FlashMaterialSlots.Reset(); FlashOriginalMaterials.Reset(); FlashMaterials.Reset();
	}
}

void UACEUIGameplayBinder::SetRetailTooltip(UWidget* Widget, const FText& Text)
{
	UACEHoverTooltipWidget::SetWidgetTooltip(Widget, Text, UACEHoverTooltipWidget::ObjectTemplate);
}

void UACEUIGameplayBinder::RefreshDesktopHUD()
{
	if (!bBound || !Canvas || !Manager) return;
	Manager->BeginNameLookupPass();
	ON_SCOPE_EXIT { if (Manager) Manager->EndNameLookupPass(); };
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshVitalsOverlays); RefreshVitalsOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshRadarOverlays); RefreshRadarOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshShortcutOverlays); RefreshShortcutOverlays(); }
	RefreshEffectsOverlays(true);
	RefreshEffectsOverlays(false);
}

void UACEUIGameplayBinder::TickRefresh(bool bRefreshDesktopHUD)
{
	if (!PendingScreenshotFilename.IsEmpty() && FPlatformTime::Seconds() - ScreenshotRequestTime > 10.0)
		FinishGameplayScreenshot();
	ACE_PROFILE_SCOPE(HUD);
	if (!bBound || !Canvas || !Manager)
	{
		return;
	}
	UpdateAutoUILayout();
	UpdateInventorySplitSelection();
	UpdateVendorSellSplit();
	UpdateTradeStackSplit();
	EnsureOverlays();
	// "Stay in chat mode after sending" — refocus after the commit's focus clear.
	Manager->BeginNameLookupPass();
	ON_SCOPE_EXIT { if (Manager) Manager->EndNameLookupPass(); };
	ApplyPendingChatFocus();
	RefreshHouseOffer();
	RefreshBarber();
	// Keep drag ghosts tracking the cursor even when MouseMove is sparse under capture.
	if ((bInvDragPending || bSpellDragPending) && FSlateApplication::IsInitialized()
		&& !(PlayerController && PlayerController->IsVRActive()))
	{
		const FVector2D Local = Canvas->GetCachedGeometry().AbsoluteToLocal(
			FSlateApplication::Get().GetCursorPos());
		if (bInvDragPending)
		{
			UpdateInventoryDrag(Local);
		}
		if (bSpellDragPending)
		{
			UpdateSpellDrag(Local);
		}
	}
	TickTransientInfo();
	TickSelectionFlash();
	RefreshNativeCombatBarVisibility();
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSelectionOverlay); RefreshSelectionOverlay(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshExaminationOverlay); RefreshExaminationOverlay(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshInventoryOverlays); RefreshInventoryOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshPanelBodyText); RefreshPanelBodyText(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshAttributeOverlays); RefreshAttributeOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSkillOverlays); RefreshSkillOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshTitleOverlays); RefreshTitleOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSpellHotbarOverlays); RefreshSpellHotbarOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSpellbookOverlays); RefreshSpellbookOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSpellbookChromeLabels); RefreshSpellbookChromeLabels(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshExternalContainerOverlays); RefreshExternalContainerOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshVendorOverlays); RefreshVendorOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshTradeOverlays); RefreshTradeOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshManaStoneConfirmation); RefreshManaStoneConfirmation(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshServerConfirmation); RefreshServerConfirmation(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshServerPopup); RefreshServerPopup(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSalvageOverlays); RefreshSalvageOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSocialOverlays); RefreshSocialOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshQuestOverlays); RefreshQuestOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshOptionsOverlays); RefreshOptionsOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshKeyboardOverlays); RefreshKeyboardOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshComponentOverlays); RefreshComponentOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshWorldOverlays); RefreshWorldOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshAbuseOverlays); RefreshAbuseOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshUrgentOverlays); RefreshUrgentOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshBookOverlays); RefreshBookOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshMiniGameOverlays); RefreshMiniGameOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshChatChromeOverlays); RefreshChatChromeOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshSkillTabLabels); RefreshSkillTabLabels(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshPanelTitleOverlay); RefreshPanelTitleOverlay(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshCombatPanelOverlays); RefreshCombatPanelOverlays(); }
	if (bRefreshDesktopHUD) RefreshDesktopHUD();
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshVitaePanelOverlays); RefreshVitaePanelOverlays(); }
	{ TRACE_CPUPROFILER_EVENT_SCOPE(RefreshLinkStatusPanelOverlays); RefreshLinkStatusPanelOverlays(); }
	TickStatusPanels(FApp::GetDeltaTime());
	TickCombatAutoAttack(FApp::GetDeltaTime());
	TickEnvPanelRangeChecks();

	{
		const int32 EquipMode = ResolveEquippedCombatMode();
		// Never force EquipMode over an explicit Melee/Missile/Magic choice every tick —
		// that spam-fights ChangeCombatMode and flashes peace until a cast recovers
		// LastCombatMode on the server. Only drop to NonCombat when gear no longer
		// supports the active mode (sheathed wand / no bow).
		if (LastEquipModeSnapshot == INDEX_NONE)
		{
			LastEquipModeSnapshot = EquipMode;
		}
		else if (EquipMode != LastEquipModeSnapshot)
		{
			LastEquipModeSnapshot = EquipMode;
			if (CombatMode == static_cast<int32>(ACECombatMode::Magic) && !HasEquippedCaster())
			{
				ApplyCombatMode(static_cast<int32>(ACECombatMode::NonCombat));
			}
			else if (CombatMode == static_cast<int32>(ACECombatMode::Missile) && !HasEquippedMissileWeapon())
			{
				ApplyCombatMode(static_cast<int32>(ACECombatMode::NonCombat));
			}
			else
			{
				ApplyPreferredStance(CombatMode);
				SyncCombatModeButtons();
			}
		}
		else
		{
			const int32 StanceKey = (CombatMode << 8) | EquipMode;
			if (StanceKey != LastAppliedStanceKey)
			{
				LastAppliedStanceKey = StanceKey;
				ApplyPreferredStance(CombatMode);
				SyncCombatModeButtons();
			}
		}
		if (PendingCombatMode != 0 && FPlatformTime::Seconds() >= PendingCombatModeUntil)
		{
			PendingCombatMode = 0;
		}
	}
	SyncInventoryButtonVisual();
	SyncCombatAmmoCount();
	if (bChatStickToBottom && ChatLog)
	{
		ChatLog->ForceLayoutPrepass();
		const float EndOff = ChatLog->GetScrollOffsetOfEnd();
		// Wrapped lines grow after the first layout pass — keep snapping while stick
		// is on and the end offset is still moving (or a message just armed pending).
		if (bPendingChatScrollToEnd || EndOff > ChatStickEndOffset + 0.5f
			|| ChatLog->GetScrollOffset() < EndOff - 2.f)
		{
			ChatLog->SetScrollOffset(EndOff);
			ChatStickEndOffset = EndOff;
		}
		bPendingChatScrollToEnd = false;
	}
	else
	{
		ChatStickEndOffset = -1.f;
	}
	SyncChatScrollbar();
	SyncChatJumpIndicator();
	SyncInventoryScrollbars();
	if (ActivePanelPage == TEXT("SkillManagementPanel_Field"))
	{
		SyncStatListScrollbar();
	}
	if (ActivePanelPage == TEXT("SpellManagementPanel_Field")
		&& ActiveSpellPanelTab == TEXT("SpellbookPage"))
	{
		SyncSpellbookScrollbar();
	}
}

void UACEUIGameplayBinder::OnElementActivated(TSharedPtr<FACEUIElement> Element)
{
	if (HandleHouseControl(Element)) return;
	if (OpenLootContainerGuid && Element && (Element->ElementName == TEXT("ScrollBar_Left") || Element->ElementName == TEXT("ScrollBar_Right")))
	{
		for (auto Parent = Element->Parent.Pin(); Parent; Parent = Parent->Parent.Pin())
		{
			if (Parent->ElementName == TEXT("Ext_ItemListScroll"))
			{
				ExtItemScrollOffset += Element->ElementName == TEXT("ScrollBar_Left") ? -1 : 1;
				RefreshExternalContainerOverlays();
				return;
			}
		}
	}
	if(OpenSalvageToolGuid && Element && (Element->ElementName==TEXT("ScrollBar_Left") || Element->ElementName==TEXT("ScrollBar_Right")))
		for(auto Parent=Element->Parent.Pin();Parent;Parent=Parent->Parent.Pin())
			if(Parent->ElementName==TEXT("Salvage_ItemListScroll"))
			{SetSalvageScrollOffset(SalvageScrollOffset+(Element->ElementName==TEXT("ScrollBar_Left")?-1:1));return;}
	if (bTradeOpen && Element && (Element->ElementName == TEXT("ScrollBar_Left") || Element->ElementName == TEXT("ScrollBar_Right")))
	{
		for (auto Parent=Element->Parent.Pin(); Parent; Parent=Parent->Parent.Pin())
		{
			int32* Offset = Parent->ElementName == TEXT("TradeSelf_ItemListScroll") ? &TradeSelfOffset
				: Parent->ElementName == TEXT("TradeOther_ItemListScroll") ? &TradeOtherOffset : nullptr;
			if (Offset)
			{
				*Offset += Element->ElementName == TEXT("ScrollBar_Left") ? -1 : 1;
				RefreshTradeOverlays();
				return;
			}
		}
	}
	// DAT chat scrollbar buttons (also appear under examine / floating chats).
	for (TSharedPtr<FACEUIElement> Cur = Element; Cur.IsValid(); Cur = Cur->Parent.Pin())
	{
		if (Cur->ElementName != TEXT("ScrollBar_Up") && Cur->ElementName != TEXT("ScrollBar_Down"))
		{
			continue;
		}
		const int32 Dir = (Cur->ElementName == TEXT("ScrollBar_Up")) ? -1 : 1;
		bool bMainChat = false;
		bool bInvItems = false;
		bool bInvPacks = false;
		int32 FloatyChatIdx = 0;
		for (TSharedPtr<FACEUIElement> A = Cur; A.IsValid(); A = A->Parent.Pin())
		{
			if(A->ElementName==TEXT("VassalsListBoxScrollbar"))
			{VassalScrollOffset+=Dir;RefreshAllegianceOverlays();return;}
			if(A->ElementName==TEXT("FellowsListBoxScrollbar"))
			{FellowScrollOffset+=Dir;RefreshFellowshipOverlays();return;}
			if(A->ElementName==TEXT("FreindsListBoxScrollbar"))
			{FriendScrollOffset+=Dir;RefreshFriendsOverlays();return;}
			if(A->ElementName==TEXT("SquelchListBoxScrollbar"))
			{SquelchScrollOffset+=Dir;RefreshSquelchOverlays();return;}
			if(A->ElementName==TEXT("KeyboardMappingScrollbar"))
			{KeyboardScrollOffset+=Dir;RefreshKeyboardOverlays();return;}
			if(A->ElementName==TEXT("NotesScrollbar") && JournalNotes)
			{JournalNotes->SetScrollOffset(JournalNotes->GetScrollOffset()+Dir*28.f);RefreshJournalOverlays();return;}
			if (A->ElementName == TEXT("BookPanel_Field") && BookScroll)
			{
				if (BookPageEditor && BookPageEditor->IsVisible())
					BookPageEditor->SetScrollOffset(BookPageEditor->GetScrollOffset()+Dir*28.f);
				else BookScroll->SetScrollOffset(FMath::Clamp(BookScroll->GetScrollOffset()+Dir*28.f,0.f,BookScroll->GetScrollOffsetOfEnd()));
				RefreshBookOverlays(); return;
			}
			if ((A->ElementName == TEXT("ItemDisplayTextScrollbar") || A->ElementName == TEXT("SpellDisplayTextScrollbar")) && ExamScroll)
			{
				ExamScroll->SetScrollOffset(FMath::Clamp(ExamScroll->GetScrollOffset() + Dir * 28.f,
					0.f, ExamScroll->GetScrollOffsetOfEnd()));
				return;
			}
			if (A->ElementName == TEXT("ItemInscriptionScrollbar") && ExamInscriptionScroll)
			{
				ExamInscriptionScroll->SetScrollOffset(FMath::Clamp(ExamInscriptionScroll->GetScrollOffset()+Dir*28.f,
					0.f,ExamInscriptionScroll->GetScrollOffsetOfEnd()));
				return;
			}

			if (A->ElementName == TEXT("RootGameplay_FloatyMainChat_Field"))
			{
				bMainChat = true;
				break;
			}
			for (int32 W = 1; W <= NumFloatyChats; ++W)
			{
				if (A->ElementName == FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"), W))
				{
					FloatyChatIdx = W;
					break;
				}
			}
			if (FloatyChatIdx > 0)
			{
				break;
			}
			if (A->ElementName == TEXT("Inv_3DItemList_Scrollbar")
				|| A->ElementName == TEXT("ThreeDItemsField"))
			{
				bInvItems = true;
				break;
			}
			if (A->ElementName == TEXT("Inv_ContainerList_Scrollbar")
				|| A->ElementName == TEXT("BackpackField"))
			{
				bInvPacks = true;
				break;
			}
			if (A->ElementName == TEXT("StatManagement_List_Scrollbar")
				|| A->ElementName == TEXT("StatManagement_List"))
			{
				if (ActivePanelPage == TEXT("SkillManagementPanel_Field") && ActiveSkillTab == TEXT("AttributePage"))
				{
					AttributeScrollOffset += Dir;
					RefreshAttributeOverlays();
					return;
				}
				if (ActivePanelPage == TEXT("SkillManagementPanel_Field")
					&& ActiveSkillTab == TEXT("SkillPage"))
				{
					SkillListScrollOffset = FMath::Max(0, SkillListScrollOffset + Dir);
					RefreshSkillOverlays();
					SyncStatListScrollbar();
					return;
				}
			}
			if (A->ElementName == TEXT("SpellBook_SpellList_Scrollbar")
				|| A->ElementName == TEXT("SpellBook_SpellList")
				|| A->ElementName == TEXT("SpellbookPage"))
			{
				if (ActivePanelPage == TEXT("SpellManagementPanel_Field")
					&& ActiveSpellPanelTab == TEXT("SpellbookPage"))
				{
					SpellbookScrollOffset = FMath::Max(0, SpellbookScrollOffset + Dir);
					RefreshSpellbookOverlays();
					SyncSpellbookScrollbar();
					return;
				}
			}
			if (A->ElementName == TEXT("SpellScroll") || A->ElementName == TEXT("Spellcasting"))
			{
				const int32 ActiveBar = Client ? Client->GetActiveSpellBar() : 0;
				const TArray<int32> Bar = Client ? Client->GetSpellBar(ActiveBar) : TArray<int32>();
				int32 Filled = 0;
				for (int32 i = 0; i < Bar.Num(); ++i)
				{
					if (Bar[i] == 0) { break; }
					++Filled;
				}
				constexpr int32 Visible = 13;
                // Scrolling changes the viewport, never the selected spell.
                const int32 MaxOff = FMath::Max(0, Filled + 1 - Visible);
                SpellHotbarScrollOffset = FMath::Clamp(SpellHotbarScrollOffset + Dir, 0, MaxOff);
				RefreshSpellHotbarOverlays();
				return;
			}
		}
        if (bMainChat || FloatyChatIdx>0)
        {
            ScrollChatLog(Dir*28.f,FloatyChatIdx);
            return;
        }
		if (bInvItems && ActivePanelPage == TEXT("InventoryPanel_Field"))
		{
			TSharedPtr<FACEUIElement> GridEl = Manager
				? Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList"))
				: nullptr;
			const int32 Cols = GridEl.IsValid() ? FMath::Max(1, GridEl->Width / 32) : 6;
			const int32 Rows = GridEl.IsValid() ? FMath::Max(1, GridEl->Height / 32) : 3;
			const int32 PageSize = Cols * Rows;
			FACEWorldObject PackObj;
			int32 Capacity = PageSize;
			if (Client && Client->GetWorldObject(SelectedPackGuid, PackObj) && PackObj.ItemsCapacity > 0)
			{
				Capacity = PackObj.ItemsCapacity;
			}
			const int32 MaxOff = FMath::Max(0, Capacity - PageSize);
			InventoryScrollOffset = FMath::Clamp(InventoryScrollOffset + Dir * Cols, 0, MaxOff);
			SyncInventoryScrollbars();
			RefreshInventoryOverlays();
			return;
		}
		if (bInvPacks && ActivePanelPage == TEXT("InventoryPanel_Field"))
		{
			PackScrollOffset = FMath::Max(0, PackScrollOffset + Dir);
			SyncInventoryScrollbars();
			RefreshInventoryOverlays();
			return;
		}
		break;
	}

	// FloatyChat1-4 children share names with MainChat (SendButton / ChatPanelTextEntry /
	// CloseChatWindowButton) — resolve the owning window by ancestor root first.
	{
		int32 ChatWindow = 0;
		for (TSharedPtr<FACEUIElement> A = Element; A.IsValid() && ChatWindow == 0; A = A->Parent.Pin())
		{
			for (int32 W = 1; W <= NumFloatyChats; ++W)
			{
				if (A->ElementName == FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"), W))
				{
					ChatWindow = W;
					break;
				}
			}
		}
		if (ChatWindow > 0)
		{
			for (TSharedPtr<FACEUIElement> Cur = Element; Cur.IsValid(); Cur = Cur->Parent.Pin())
			{
				const FString& N = Cur->ElementName;
				if (N == TEXT("ChatLogNewNonVisibleTextIndicator"))
				{
					if (auto* Log=GetChatLogWidget(ChatWindow)) Log->ScrollToEnd();
					bFloatyChatStickToBottom[ChatWindow-1]=true;
					SyncChatJumpIndicator(ChatWindow);
					return;
				}
				if (N == TEXT("SendButton"))
				{
					TrySendChatFromEntry(nullptr, ChatWindow);
					return;
				}
				if (N == TEXT("ChatPanelTextEntry") || N == TEXT("ChatPanelTextEntryField"))
				{
					FocusChatEntryWindow(ChatWindow);
					return;
				}
				if (N == TEXT("CloseChatWindowButton"))
				{
					SetFloatyVisible(FString::Printf(
						TEXT("RootGameplay_FloatyChat%d_Field"), ChatWindow), false);
					return;
				}
			}
		}
	}

	for (TSharedPtr<FACEUIElement> Cur = Element; Cur.IsValid(); Cur = Cur->Parent.Pin())
	{
		if (!Cur->ElementName.IsEmpty() && HandleNamedClick(Cur->ElementName))
		{
			return;
		}
	}

	if (!Canvas)
	{
		return;
	}
}

bool UACEUIGameplayBinder::HandleNamedClick(const FString& Name)
{
	if (HandleJournalNamedClick(Name)) return true;
	if (HandleOptionsNamedClick(Name))
	{
		return true;
	}
	if (HandleWorldNamedClick(Name))
	{
		return true;
	}
	if (HandleDialogNamedClick(Name))
	{
		return true;
	}
	if (Name == TEXT("HealthMeter") || Name == TEXT("StaminaMeter") || Name == TEXT("ManaMeter")
		|| Name == TEXT("PlayerHealthLabel") || Name == TEXT("PlayerStaminaLabel")
		|| Name == TEXT("PlayerManaLabel"))
	{
		bShowVitalNumbers = !bShowVitalNumbers;
		SaveVitalDisplayPreference();
		RefreshVitalsOverlays();
		return true;
	}
	if (Name == TEXT("ChatLogNewNonVisibleTextIndicator"))
	{
		if (ChatLog)
		{
			ChatLog->ScrollToEnd();
			bChatStickToBottom = true;
			SyncChatScrollbar();
			SyncChatJumpIndicator();
		}
		return true;
	}
	if (Name == TEXT("InventoryButton") || Name == TEXT("BigBackpack_Icon_DragAccept"))
	{
		TogglePanelPage(TEXT("InventoryPanel_Field"));
		return true;
	}
	if (Name == TEXT("PanelButton_SocialButton"))
	{
		TogglePanelPage(TEXT("SocialPanel_Field"));
		if (ActivePanelPage == TEXT("SocialPanel_Field"))
		{
			SyncSocialPanelTab(ActiveSocialTab.IsEmpty() ? TEXT("AllegiancePage") : ActiveSocialTab);
		}
		return true;
	}
	if (Name == TEXT("CloseSocialPanelButton"))
	{
		HidePanel();
		if (Client)
		{
			Client->SendFellowshipUpdateRequest(false);
			Client->SendAllegianceUpdateRequest(false);
		}
		return true;
	}
	if (Name == TEXT("AllegianceTab"))
	{
		SyncSocialPanelTab(TEXT("AllegiancePage"));
		return true;
	}
	if (Name == TEXT("FellowshipTab"))
	{
		SyncSocialPanelTab(TEXT("FellowshipPage"));
		return true;
	}
	if (Name == TEXT("FriendsTab"))
	{
		SyncSocialPanelTab(TEXT("FriendsPage"));
		return true;
	}
	if (Name == TEXT("ContractNameSortButton") || Name == TEXT("ContractStatusSortButton"))
	{
		const bool bStatus = Name == TEXT("ContractStatusSortButton");
		bQuestReverseSort = bQuestSortByStatus == bStatus ? !bQuestReverseSort : false;
		bQuestSortByStatus = bStatus;
		QuestScrollOffset = 0;
		RefreshQuestOverlays();
		return true;
	}
	if (Name == TEXT("ContractAbandonButton"))
	{
		if (Client && SelectedContractId != 0)
		{
			Client->SendAbandonContract(SelectedContractId);
			SelectedContractId = 0;
			RefreshQuestOverlays();
		}
		else
		{
			PostInventorySystemMessage(TEXT("Select a contract to abandon."));
		}
		return true;
	}
	if (Name == TEXT("SquelchTab"))
	{
		SyncSocialPanelTab(TEXT("SquelchPage"));
		return true;
	}
	if (Name == TEXT("SquelchCharacterButton") || Name == TEXT("SquelchAccountButton"))
	{
		EnsureSocialEntryBoxes();
		const FString Target = SquelchNameEntry
			? SquelchNameEntry->GetText().ToString().TrimStartAndEnd() : FString();
		if (Target.IsEmpty())
		{
			PostInventorySystemMessage(TEXT("Type a character name to squelch."));
			return true;
		}
		if (Client)
		{
			if (Name == TEXT("SquelchAccountButton"))
			{
				Client->SendModifyAccountSquelch(true, Target);
			}
			else
			{
				// guid 0 → server resolves by name; 1 = ChatMessageType.AllChannels.
				Client->SendModifyCharacterSquelch(true, 0, Target, 1);
			}
		}
		if (SquelchNameEntry)
		{
			SquelchNameEntry->SetText(FText::GetEmpty());
		}
		return true;
	}
	if (Name == TEXT("SquelchRemoveButton"))
	{
		if (!Client || (SelectedSquelchGuid == 0 && SelectedSquelchName.IsEmpty()))
		{
			PostInventorySystemMessage(TEXT("Select a squelched character first."));
			return true;
		}
		bool bAccount = false;
		for (const FACESquelchEntry& E : Client->GetSquelches())
		{
			if (E.Guid == SelectedSquelchGuid && E.Name == SelectedSquelchName)
			{
				bAccount = E.bAccount;
				break;
			}
		}
		if (bAccount)
		{
			Client->SendModifyAccountSquelch(false, SelectedSquelchName);
		}
		else
		{
			Client->SendModifyCharacterSquelch(false, SelectedSquelchGuid, SelectedSquelchName, 1);
		}
		SelectedSquelchGuid = 0;
		SelectedSquelchName.Reset();
		RefreshSquelchOverlays();
		return true;
	}
	if (Name == TEXT("KickButton"))
	{
		ShowAllegianceConfirmation(Name);
		return true;
	}
	if (Name == TEXT("TellButton"))
	{
		if (SelectedFriendGuid != 0 && Client)
		{
			for (const FACEFriendInfo& F : Client->GetFriends())
			{
				if (F.Guid == SelectedFriendGuid)
				{
					if (ChatEntry)
					{
						const FString Prefix=FString::Printf(TEXT("/tell %s, "),*F.Name);
						if (auto* Chat=Cast<UACEChatEntry>(ChatEntry)) Chat->SetChatText(Prefix);
						else ChatEntry->SetText(FText::FromString(Prefix));
					}
					FocusChatEntry();
					break;
				}
			}
		}
		else
		{
			PostInventorySystemMessage(TEXT("Select a friend first."));
		}
		return true;
	}
	if (Name == TEXT("RemoveButton"))
	{
		if (Client && SelectedFriendGuid != 0)
		{
			Client->SendRemoveFriend(SelectedFriendGuid);
			SelectedFriendGuid = 0;
		}
		else
		{
			PostInventorySystemMessage(TEXT("Select a friend to remove."));
		}
		return true;
	}
	if (Name == TEXT("AddButton"))
	{
		EnsureSocialEntryBoxes();
		const FString FriendName = FriendNameEntry
			? FriendNameEntry->GetText().ToString().TrimStartAndEnd() : FString();
		if (FriendName.IsEmpty())
		{
			PostInventorySystemMessage(TEXT("Type a character name to add."));
			return true;
		}
		if (Client)
		{
			Client->SendAddFriend(FriendName);
		}
		if (FriendNameEntry)
		{
			FriendNameEntry->SetText(FText::GetEmpty());
		}
		return true;
	}
	if (Name == TEXT("IgnoreAllegianceRequests") || Name == TEXT("IgnoreFellowshipRequests")
		|| Name == TEXT("FellowshipAutoAcceptRequests") || Name == TEXT("FellowshipShareLoot")
		|| Name == TEXT("AppearOffline_Checkbox"))
	{
		int32 Option = 0x01;
		if (Name == TEXT("IgnoreFellowshipRequests")) { Option = 0x02; }
		else if (Name == TEXT("FellowshipAutoAcceptRequests")) { Option = 0x12; }
		else if (Name == TEXT("FellowshipShareLoot")) { Option = 0x11; }
		else if (Name == TEXT("AppearOffline_Checkbox")) { Option = 0x27; }
		ToggleCharacterOption(Option);
		RefreshSocialButtonLabels();
		return true;
	}
	if (Name == TEXT("CreateFellowshipButton"))
	{
		EnsureSocialEntryBoxes();
		if(!CanActivateFellowshipControl(Name))return true;
		FString FellowName = FellowshipNameEntry
			? FellowshipNameEntry->GetText().ToString().TrimStartAndEnd() : FString();
		FellowName=FellowName.Left(32);
		if (Client)
		{
			Client->SendFellowshipCreate(FellowName, Client->IsCharacterOptionSet(0x0F));
		}
		return true;
	}
	if (Name == TEXT("FellowQuitButton"))
	{
		if(!CanActivateFellowshipControl(Name))return true;
		const auto Info=Client->GetFellowship();
		if(Info.LeaderGuid==Client->GetPlayerGuid())for(const auto& M:Info.Members)
			if(M.Guid!=Info.LeaderGuid){Client->SendFellowshipAssignNewLeader(M.Guid);break;}
		if (Client) { Client->SendFellowshipQuit(false); }
		return true;
	}
	if (Name == TEXT("FellowDisbandButton"))
	{
		if(!CanActivateFellowshipControl(Name))return true;
		if (Client) { Client->SendFellowshipQuit(true); }
		return true;
	}
	if (Name == TEXT("FellowRecruitButton"))
	{
		if(!CanActivateFellowshipControl(Name))return true;
		if (Client && LastSelection.bValid && LastSelection.Guid != 0)
		{
			Client->SendFellowshipRecruit(LastSelection.Guid);
		}
		else
		{
			PostInventorySystemMessage(TEXT("Select a player to recruit."));
		}
		return true;
	}
	if (Name == TEXT("FellowDismissButton"))
	{
		if(!CanActivateFellowshipControl(Name))return true;
		if (Client && SelectedFellowGuid != 0)
		{
			Client->SendFellowshipDismiss(SelectedFellowGuid);
		}
		return true;
	}
	if (Name == TEXT("FellowLeaderButton"))
	{
		if(!CanActivateFellowshipControl(Name))return true;
		if (Client && SelectedFellowGuid != 0)
		{
			Client->SendFellowshipAssignNewLeader(SelectedFellowGuid);
		}
		return true;
	}
	if (Name == TEXT("FellowOpenButton"))
	{
		if(!CanActivateFellowshipControl(Name))return true;
		if (Client)
		{
			const FACEFellowshipInfo Info = Client->GetFellowship();
			Client->SendFellowshipChangeOpenness(!Info.bOpen);
		}
		return true;
	}
	if (Name == TEXT("FellowshipShareXP"))
	{
		// Retail ShareFellowshipExpAndLuminance (0x0F) — also the default for new fellowships.
		ToggleCharacterOption(0x0F);
		RefreshSocialButtonLabels();
		return true;
	}
	if (Name == TEXT("SwearButton"))
	{
		ShowAllegianceConfirmation(Name);
		return true;
	}
	if (Name == TEXT("BreakButton"))
	{
		ShowAllegianceConfirmation(Name);
		return true;
	}
	if (Name == TEXT("CloseSalvagePanelButton"))
	{
		HideSalvagePanel();
		return true;
	}
	if (Name == TEXT("Salvage_Button"))
	{
		SubmitSalvageQueue();
		return true;
	}
	if (Name == TEXT("CloseQuestPanelButton"))
	{
		HidePanel();
		return true;
	}
	if (Name == TEXT("PanelButton_MagicButton"))
	{
		TogglePanelPage(TEXT("SpellManagementPanel_Field"));
		return true;
	}
	if (Name == TEXT("PanelButton_SkillManagementButton"))
	{
		TogglePanelPage(TEXT("SkillManagementPanel_Field"));
		return true;
	}
	if (Name == TEXT("PanelButton_QuestManagementButton"))
	{
		TogglePanelPage(TEXT("QuestManagementPanel_Field"));
		RefreshQuestOverlays();
		return true;
	}
	if (Name == TEXT("PanelButton_WorldButton"))
	{
		TogglePanelPage(TEXT("WorldPanel_Field"));
		return true;
	}
	if (Name == TEXT("PanelButton_OptionsButton"))
	{
		TogglePanelPage(TEXT("OptionsPanel_Field"));
		return true;
	}
	if (Name == TEXT("CloseInvPanelButton") || Name == TEXT("ClosePanelButton")
		|| Name == TEXT("CloseExaminationWindowButton"))
	{
		if (Name == TEXT("CloseExaminationWindowButton"))
		{
			ShowExamination(false);
		}
		else
		{
			HidePanel();
		}
		return true;
	}
	if (Name == TEXT("PanelButton_UseSelectedButton"))
	{
		UseSelectedObject();
		return true;
	}
	if (Name == TEXT("PanelButton_ExamineSelectedButton"))
	{
		ExamineSelectedObject();
		return true;
	}
	if (Name == TEXT("PeaceModeButton"))
	{
		// Retail: peace button toggles peace ↔ equipped weapon class.
		const int32 Next = (CombatMode == static_cast<int32>(ACECombatMode::NonCombat))
			? ResolveEquippedCombatMode()
			: static_cast<int32>(ACECombatMode::NonCombat);
		ApplyCombatMode(Next);
		return true;
	}
	if (Name == TEXT("MeleeModeButton"))
	{
		const int32 Next = (CombatMode == static_cast<int32>(ACECombatMode::Melee))
			? static_cast<int32>(ACECombatMode::NonCombat)
			: static_cast<int32>(ACECombatMode::Melee);
		ApplyCombatMode(Next);
		return true;
	}
	if (Name == TEXT("MissileModeButton"))
	{
		const int32 Next = (CombatMode == static_cast<int32>(ACECombatMode::Missile))
			? static_cast<int32>(ACECombatMode::NonCombat)
			: static_cast<int32>(ACECombatMode::Missile);
		ApplyCombatMode(Next);
		return true;
	}
	if (Name == TEXT("MagicModeButton"))
	{
		const int32 Next = (CombatMode == static_cast<int32>(ACECombatMode::Magic))
			? static_cast<int32>(ACECombatMode::NonCombat)
			: static_cast<int32>(ACECombatMode::Magic);
		ApplyCombatMode(Next);
		return true;
	}
	if (Name == TEXT("HighAttack") || Name == TEXT("MediumAttack") || Name == TEXT("LowAttack"))
	{
		// gmCombatUI::SetRequestedAttackHeight starts a request at the slider's power.
		BeginCombatPowerCharge(Name == TEXT("HighAttack") ? ACEAttackHeight::High
			: Name == TEXT("LowAttack") ? ACEAttackHeight::Low : ACEAttackHeight::Medium);
		return true;
	}
	if (Name == TEXT("AutoRepeatAttack"))
	{
		bCombatAutoRepeat = !bCombatAutoRepeat;
		if (Client) Client->SendSetSingleCharacterOption(0x00, bCombatAutoRepeat);
		return true;
	}
	if (Name == TEXT("AutoTarget"))
	{
		bCombatAutoTarget = !bCombatAutoTarget;
		if (Client) Client->SendSetSingleCharacterOption(0x0D, bCombatAutoTarget);
		if (bCombatAutoTarget) TryAutoTargetOnCombatEnter();
		return true;
	}
	if (Name == TEXT("ViewCombatTarget"))
	{
		bCombatViewTarget = !bCombatViewTarget;
		if (Client) Client->SendSetSingleCharacterOption(0x07, bCombatViewTarget);
		return true;
	}
	if (Name == TEXT("LockUI"))
	{
		if (Manager)
		{
			Manager->ToggleUiLocked();
		}
		return true;
	}
	if (Name == TEXT("StatManagement_Footer_RaiseButton"))
	{
		RaiseSelectedStat(1);
		return true;
	}
	if (Name == TEXT("StatManagement_Footer_Raise10Button"))
	{
		RaiseSelectedStat(10);
		return true;
	}
	if (Name == TEXT("SendButton"))
	{
		TrySendChatFromEntry();
		return true;
	}
	if (Name == TEXT("ChatPanelTextEntry") || Name == TEXT("ChatEntryField"))
	{
		FocusChatEntry();
		return true;
	}
	if (Name == TEXT("AttributeTab"))
	{
		ShowPanelPage(TEXT("SkillManagementPanel_Field"));
		SyncSkillPanelTab(TEXT("AttributePage"));
		return true;
	}
	if (Name == TEXT("SkillTab"))
	{
		ShowPanelPage(TEXT("SkillManagementPanel_Field"));
		SyncSkillPanelTab(TEXT("SkillPage"));
		return true;
	}
	if (Name == TEXT("CharacterTitleTab"))
	{
		ShowPanelPage(TEXT("SkillManagementPanel_Field"));
		SyncSkillPanelTab(TEXT("CharacterTitlePage"));
		return true;
	}
	if (Name == TEXT("CharacterTitle_SetAsDisplayButton"))
	{
		if (Client && SelectedTitleId != 0)
		{
			Client->SendSetTitle(SelectedTitleId);
		}
		else
		{
			PostInventorySystemMessage(TEXT("Select a title first."));
		}
		return true;
	}
	if (Name == TEXT("ChatTarget") || Name == TEXT("ChatTargetButtonText"))
	{
		// Retail opens destination popup; right-click cycles filter (@filter).
		ToggleChatTargetPopup();
		return true;
	}
	if (Name == TEXT("MaximizeButton"))
	{
		ToggleMainChatExpanded();
		return true;
	}
	if (Name == TEXT("Paperdoll_Slots_Checkbox"))
	{
		bShowPaperdollSlots = !bShowPaperdollSlots;
		RefreshInventoryOverlays();
		return true;
	}
	if (Name == TEXT("CastSpellButton") || Name.StartsWith(TEXT("StripButton")))
	{
		CastSelectedHotbarSpell();
		return true;
	}
	if (Name == TEXT("BuiltInSpell") || Name.StartsWith(TEXT("BuiltInSpell_")))
	{
		if (BuiltInSpellId != 0 && Client)
		{
			constexpr double DoubleClickSeconds = 0.75;
			const double Now = FPlatformTime::Seconds();
			const bool bWasSelected = SelectedCombatSpellSlot < 0;
			SelectCombatSpellSlot(-1);
			if (bWasSelected && BuiltInSpellId == LastSpellClickId
				&& (Now - LastSpellClickTime) < DoubleClickSeconds)
			{
				if (!Client->SendCastSpell(BuiltInSpellId, BuiltInCasterGuid))
				{
					PostInventorySystemMessage(Client->IsInPortalSpace() ? TEXT("You're too busy!") : TEXT("You must select a target for that spell."));
				}
				LastSpellClickSlot = INDEX_NONE;
				LastSpellClickId = 0;
				LastSpellClickTime = 0.0;
			}
			else
			{
				LastSpellClickSlot = -1;
				LastSpellClickId = BuiltInSpellId;
				LastSpellClickTime = Now;
			}
			RefreshSpellHotbarOverlays();
		}
		return true;
	}
	if (Name.StartsWith(TEXT("Spellcast_Tab")))
	{
		FString Rest = Name;
		Rest.RemoveFromStart(TEXT("Spellcast_Tab"));
		const int32 Tab = FCString::Atoi(*Rest);
		if (Tab >= 1 && Tab <= 8 && Client)
		{
			SetCombatSpellBar(Tab - 1);
			return true;
		}
		return true;
	}
	if (Name == TEXT("SpellbookTab"))
	{
		ShowPanelPage(TEXT("SpellManagementPanel_Field"));
		SyncSpellPanelTab(TEXT("SpellbookPage"));
		return true;
	}
	if (Name == TEXT("SpellComponentTab"))
	{
		ShowPanelPage(TEXT("SpellManagementPanel_Field"));
		SyncSpellPanelTab(TEXT("SpellComponentPage"));
		return true;
	}
	if (Name == TEXT("ScrollBar_Up") || Name == TEXT("ScrollBar_Down"))
	{
		// Inventory scrollbars are handled in OnElementActivated (need ancestry).
		if (ActivePanelPage == TEXT("SpellManagementPanel_Field")
			&& ActiveSpellPanelTab == TEXT("SpellbookPage"))
		{
			const int32 Dir = (Name == TEXT("ScrollBar_Up")) ? -1 : 1;
			SpellbookScrollOffset = FMath::Max(0, SpellbookScrollOffset + Dir);
			RefreshSpellbookOverlays();
			SyncSpellbookScrollbar();
			return true;
		}
		if (ActivePanelPage == TEXT("PositiveEffectsPanel_Field")
			|| ActivePanelPage == TEXT("NegativeEffectsPanel_Field"))
		{
			const int32 Dir = (Name == TEXT("ScrollBar_Up")) ? -1 : 1;
			EffectsScrollOffset = FMath::Max(0, EffectsScrollOffset + Dir);
			RefreshEffectsOverlays(ActivePanelPage == TEXT("PositiveEffectsPanel_Field"));
			return true;
		}
	}
	if (Name == TEXT("ScrollBar_Left") || Name == TEXT("ScrollBar_Right"))
	{
		if (OpenVendorGuid != 0)
		{
			const int32 Dir = (Name == TEXT("ScrollBar_Left")) ? -1 : 1;
			int32* Offset = &VendorItemsScrollOffset;
			if (ActiveVendorPage == 1) { Offset = &VendorBuyScrollOffset; }
			else if (ActiveVendorPage == 2) { Offset = &VendorSellScrollOffset; }
			*Offset = FMath::Max(0, *Offset + Dir);
			RefreshVendorOverlays();
			return true;
		}
	}
	if (ToggleSpellbookFilter(Name))
	{
		return true;
	}
	if (Name == TEXT("FloatingChat1")) { ToggleFloaty(TEXT("RootGameplay_FloatyChat1_Field")); return true; }
	if (Name == TEXT("FloatingChat2")) { ToggleFloaty(TEXT("RootGameplay_FloatyChat2_Field")); return true; }
	if (Name == TEXT("FloatingChat3")) { ToggleFloaty(TEXT("RootGameplay_FloatyChat3_Field")); return true; }
	if (Name == TEXT("FloatingChat4")) { ToggleFloaty(TEXT("RootGameplay_FloatyChat4_Field")); return true; }
	if (Name == TEXT("PositiveEffectsIndicator")) { TogglePanelPage(TEXT("PositiveEffectsPanel_Field")); return true; }
	if (Name == TEXT("NegativeEffectsIndicator")) { TogglePanelPage(TEXT("NegativeEffectsPanel_Field")); return true; }
	if (Name == TEXT("LinkStatusIndicator")) { TogglePanelPage(TEXT("LinkStatusPanel_Field")); return true; }
	if (Name == TEXT("VitaeIndicator")) { TogglePanelPage(TEXT("VitaePanel_Field")); return true; }
	if (Name == TEXT("BurdenIndicator")) { TogglePanelPage(TEXT("CharacterInfoPanel_Field")); return true; }
	if (Name == TEXT("MiniGameIndicator")) { TogglePanelPage(TEXT("MiniGamePanel_Field")); return true; }
	if (Name == TEXT("LogoffButton"))
	{
		if (Client)
		{
			Client->Logout();
		}
		return true;
	}
	if (Name == TEXT("Inv_MainPackSlot"))
	{
		if (Client) { SelectedPackGuid = Client->GetPlayerGuid(); }
		InventoryScrollOffset = 0;
		RefreshInventoryOverlays();
		SyncInventoryScrollbars();
		return true;
	}
	if (Name == TEXT("CloseExtContainerPanelButton"))
	{
		HideExternalContainer(true);
		return true;
	}
	if (Name == TEXT("CloseVendorPanelButton") || Name == TEXT("VendorCloseButton"))
	{
		HideVendorPanel();
		return true;
	}
	if (Name == TEXT("VendorItemsTab"))
	{
		SetVendorPage(0);
		return true;
	}
	if (Name == TEXT("VendorBuyTab"))
	{
		SetVendorPage(1);
		return true;
	}
	if (Name == TEXT("VendorSellTab"))
	{
		SetVendorPage(2);
		return true;
	}
	if (Name == TEXT("Menu_Vendor_SelectionWidget") || Name == TEXT("Menu_Vendor_SelectionDisplay")
		|| Name == TEXT("VendorItemTypeList") || Name == TEXT("Menu_Vendor"))
	{
		ToggleVendorFilterDropdown();
		return true;
	}
	if (Name == TEXT("VendorItemBuy_Button") || Name == TEXT("VendorBuyBuyItem_Button"))
	{
		if (ActiveVendorPage == 1)
		{
			BuyVendorCartItem();
		}
		else
		{
			BuySelectedVendorItem();
		}
		return true;
	}
	if (Name == TEXT("VendorItemAdd_Button"))
	{
		AddSelectedVendorItemToBuyCart();
		return true;
	}
	if (Name == TEXT("VendorBuyBuyAll_Button"))
	{
		BuyVendorCart();
		return true;
	}
	if (Name == TEXT("VendorSellSellItem_Button") || Name == TEXT("VendorSellButton")
		|| Name == TEXT("VendorSellButtonSmall") || Name == TEXT("VendorSellSellAll_Button"))
	{
		SellVendorCart(Name == TEXT("VendorSellSellItem_Button"));
		return true;
	}
	if (Name == TEXT("VendorSellClearItem_Button") || Name == TEXT("VendorBuyClearItem_Button"))
	{
		if (ActiveVendorPage == 1)
		{
			if (VendorSelectedGuid != 0)
			{
				VendorBuyCart.RemoveAll([this](const TPair<int32, int32>& P)
				{
					return P.Value == VendorSelectedGuid;
				});
				VendorSelectedGuid = 0;
				RefreshVendorOverlays();
			}
		}
		else if (VendorSellSelectedGuid != 0)
		{
			if (VendorSellSelectedGuid == VendorSellSplit.SourceGuid) VendorSellSplit = {};
			VendorSellCart.RemoveAll([this](const TPair<int32, int32>& P)
			{
				return P.Value == VendorSellSelectedGuid;
			});
			VendorSellSelectedGuid = 0;
			RefreshVendorOverlays();
		}
		return true;
	}
	if (Name == TEXT("VendorSellClearAll_Button") || Name == TEXT("VendorBuyClearAll_Button")
		|| Name == TEXT("VendorItemClear_Button"))
	{
		if (ActiveVendorPage == 1)
		{
			VendorBuyCart.Reset();
		}
		else if (ActiveVendorPage == 2)
		{
			VendorSellSplit = {};
			VendorSellCart.Reset();
			VendorSellSelectedGuid = 0;
		}
		else
		{
			VendorSellSplit = {};
			VendorBuyCart.Reset();
			VendorSellCart.Reset();
			VendorSellSelectedGuid = 0;
		}
		RefreshVendorOverlays();
		return true;
	}
	if (Name == TEXT("CloseSecureTradeButton"))
	{
		HideTradePanel(true);
		return true;
	}
	if (Name == TEXT("TradeSelfTradeButton") || Name == TEXT("AcceptTradeButton"))
	{
		if (TradeStackSplit.SourceGuid)
		{
			PostInventorySystemMessage(TEXT("Please wait for the stack to finish splitting."));
			return true;
		}
		if (Client && bTradeOpen)
		{
			if (Client->HasAcceptedTrade()) Client->SendDeclineTrade();
			else if (!Client->GetTradeSelfItems().IsEmpty() || !Client->GetTradePartnerItems().IsEmpty()) Client->SendAcceptTrade();
		}
		return true;
	}
	if (Name == TEXT("TradeDeclineButton") || Name == TEXT("DeclineTradeButton"))
	{
		if (Client) { Client->SendDeclineTrade(); }
		return true;
	}
	if (Name == TEXT("TradeResetButton") || Name == TEXT("ResetTradeButton")
		|| Name == TEXT("TradeClearButton") || Name == TEXT("Trade_ClearAllButon"))
	{
		TradeStackSplit = {};
		if (Client) { Client->SendResetTrade(); }
		return true;
	}

	// Retail exposes two rows of nine persistent shortcut slots.
	if ((Name.StartsWith(TEXT("ShortcutBar_Shortcut")) || Name.StartsWith(TEXT("ShortcutBar2_Shortcut")))
		&& Name.EndsWith(TEXT("Button")))
	{
		const bool bSecondRow = Name.StartsWith(TEXT("ShortcutBar2_"));
		FString Rest = Name;
		Rest.RemoveFromStart(bSecondRow ? TEXT("ShortcutBar2_Shortcut") : TEXT("ShortcutBar_Shortcut"));
		Rest.RemoveFromEnd(TEXT("Button"));
		const int32 Slot = FCString::Atoi(*Rest);
		if (Slot >= 1 && Slot <= 9)
		{
			const int32 Index = Slot - 1 + (bSecondRow ? 9 : 0);
			const int32 Guid = Client ? Client->GetShortcutObject(Index) : 0;
			// Retail executes the target cursor before normal shortcut selection.
			if (TryCompletePendingUseWithTarget(Guid)) return true;
			const double Now = FPlatformTime::Seconds();
			const bool bDouble = Guid != 0 && Guid == LastInvClickGuid && Now - LastInvClickTime < InventoryDoubleClickSeconds();
			if (!bDouble && !ShouldPreserveShortcutSelection(Guid)) SelectInventoryGuid(Guid);
			LastInvClickGuid = bDouble ? 0 : Guid;
			LastInvClickTime = bDouble ? 0.0 : Now;
			if (bDouble) UseShortcutSlot(Index + 1);
			return true;
		}
	}

	// Paper doll / inventory item click via ElementName when overlay not used
	for (const FDollSlotMap& Slot : GDollSlots)
	{
		if (Name == Slot.Name)
		{
			if (!Client) { return true; }
			if (PendingUseWithSourceGuid || (PlayerController && PlayerController->GetObjectCursorTargeting()))
			{
				TryCompletePendingUseWithTarget(ResolvePaperDollUseTarget(Name, Slot.Mask));
				return true;
			}
			for (const FACEWorldObject& Obj : Client->GetEquippedItems())
			{
				if (ItemMatchesDollSlot(Obj, Slot.Mask))
				{
					SelectInventoryGuid(Obj.Guid);
					return true;
				}
			}
			return true;
		}
	}
	return false;
}

void UACEUIGameplayBinder::SetFloatyVisible(const FString& ElementName, bool bVisible)
{
	if (!Manager)
	{
		return;
	}
	if (ElementName == TEXT("RootGameplay_FloatyCombatPanel_Field"))
	{
		auto* Deck = Client ? Client->GetGameInstance()->GetSubsystem<UACEActionDeckSubsystem>() : nullptr;
		bActionDeckCombatBarHidden = Deck && Deck->HidesNativeBar(CombatMode);
		bVisible &= !bActionDeckCombatBarHidden;
	}
	Manager->SetElementVisibleByName(ElementName, bVisible);
	if (bVisible)
	{
		if (TSharedPtr<FACEUIElement> El = Manager->FindElementByName(ElementName))
		{
			Manager->BringFloatyToFront(El);
		}
	}
}

void UACEUIGameplayBinder::RefreshNativeCombatBarVisibility()
{
	if (!Client || !Manager) return;
	auto* Deck = Client->GetGameInstance()->GetSubsystem<UACEActionDeckSubsystem>();
	const bool Hidden = Deck && Deck->HidesNativeBar(CombatMode);
	if (Hidden != bActionDeckCombatBarHidden)
		SetFloatyVisible(TEXT("RootGameplay_FloatyCombatPanel_Field"), CombatMode != int32(ACECombatMode::NonCombat));
}

void UACEUIGameplayBinder::ToggleFloaty(const FString& ElementName)
{
	if (!Manager)
	{
		return;
	}
	TSharedPtr<FACEUIElement> El = Manager->FindElementByName(ElementName);
	const bool bOn = El.IsValid() && El->bVisible;
	SetFloatyVisible(ElementName, !bOn);
}

void UACEUIGameplayBinder::SanitizeFloatyPanelChrome()
{
	if (!Manager)
	{
		return;
	}
	// LayoutDesc loads every Panel_*_Field as visible — stacked page backgrounds look like
	// a double-shadowed interface. Hide all pages here; ShowPanelPage reveals one.
	static const TCHAR* Pages[] = {
		TEXT("AbusePanel_Field"), TEXT("PositiveEffectsPanel_Field"), TEXT("NegativeEffectsPanel_Field"),
		TEXT("LinkStatusPanel_Field"), TEXT("UrgentAssistancePanel_Field"), TEXT("VitaePanel_Field"),
		TEXT("WorldPanel_Field"), TEXT("OptionsPanel_Field"), TEXT("SkillManagementPanel_Field"),
		TEXT("QuestManagementPanel_Field"), TEXT("SocialPanel_Field"), TEXT("SpellManagementPanel_Field"),
		TEXT("BookPanel_Field"), TEXT("CharacterInfoPanel_Field"), TEXT("MiniGamePanel_Field"),
		TEXT("InventoryPanel_Field"),
	};
	for (const TCHAR* Page : Pages)
	{
		if (!ActivePanelPage.IsEmpty() && ActivePanelPage == Page)
		{
			continue;
		}
		Manager->SetElementVisibleByName(Page, false);
	}
	// Page bodies that re-paint the same slab as the floaty root (0x06004CC2) double-darken
	// ("shadow over the interface areas" on Attributes/Titles/Options/Social tabs). Keep the
	// root fill (inventory/skills rely on it); drop the redundant page copies. Inner list /
	// entry boxes keep theirs — retail intends those as darker insets. Spellbook pages stay
	// opaque (kept behind list overlays, see FACEUIElement::IsOpaquePageSlab).
	// SkillManagement_Attribute_Field exists twice in the layout, so walk the whole tree
	// instead of FindElementByName (first match only).
	static const TSet<FString> DuplicateSlabs = {
		TEXT("VitaeBackground"), TEXT("LinkStatusBackground"),
		TEXT("SkillManagement_Attribute_Field"), TEXT("CharacterTitlePage"),
		TEXT("HousePage"), TEXT("CharacterSettingsPage"), TEXT("GameplayOptionsPage"),
		TEXT("ChatPage"), TEXT("ConfigPage"),
		TEXT("AllegiancePage"), TEXT("FellowshipPage"), TEXT("SquelchPage"), TEXT("FriendsPage"),
		TEXT("ContractsPage"), TEXT("PageListPage"), TEXT("CharacterInfoBackground"),
	};
	if (TSharedPtr<FACEUIElement> PanelRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")))
	{
		TArray<TSharedPtr<FACEUIElement>> Stack;
		Stack.Add(PanelRoot);
		while (Stack.Num() > 0)
		{
			TSharedPtr<FACEUIElement> El = Stack.Pop(EAllowShrinking::No);
			if (!El.IsValid())
			{
				continue;
			}
			if (El->ImageFileId == 0x06004CC2u && DuplicateSlabs.Contains(El->ElementName))
			{
				El->ImageFileId = 0;
				El->DrawMode = 0;
			}
			else if (El->ImageFileId == 0x06004CC2u)
			{
				if (TSharedPtr<FACEUIElement> Parent = El->Parent.Pin())
				{
					if (Parent->ImageFileId == 0x06004CC2u
						&& !El->ElementName.Contains(TEXT("List"))
						&& !El->ElementName.Contains(TEXT("Entry"))
						&& !El->ElementName.Contains(TEXT("Box"))
						&& !El->ElementName.Contains(TEXT("Spellbook")))
					{
						El->ImageFileId = 0;
						El->DrawMode = 0;
					}
				}
			}
			for (const TSharedPtr<FACEUIElement>& Child : El->Children)
			{
				Stack.Add(Child);
			}
		}
	}
}

void UACEUIGameplayBinder::ShowPanelPage(const FString& PageElementName)
{
	if (ActivePanelPage == TEXT("BookPanel_Field") && PageElementName != ActivePanelPage && Client)
		Client->ClearBook();
	SanitizeFloatyPanelChrome();
	SetFloatyVisible(TEXT("RootGameplay_FloatyPanel_Field"), true);
	if (!Manager)
	{
		return;
	}
	static const TCHAR* Pages[] = {
		TEXT("AbusePanel_Field"), TEXT("PositiveEffectsPanel_Field"), TEXT("NegativeEffectsPanel_Field"),
		TEXT("LinkStatusPanel_Field"), TEXT("UrgentAssistancePanel_Field"), TEXT("VitaePanel_Field"),
		TEXT("WorldPanel_Field"), TEXT("OptionsPanel_Field"), TEXT("SkillManagementPanel_Field"),
		TEXT("QuestManagementPanel_Field"), TEXT("SocialPanel_Field"), TEXT("SpellManagementPanel_Field"),
		TEXT("BookPanel_Field"), TEXT("CharacterInfoPanel_Field"), TEXT("MiniGamePanel_Field"),
		TEXT("InventoryPanel_Field"),
	};
	for (const TCHAR* Page : Pages)
	{
		Manager->SetElementVisibleByName(Page, PageElementName == Page);
	}
	ActivePanelPage = PageElementName;
	// Drop overlays that belong to other panel pages so they cannot ghost through.
	RefreshInventoryOverlays();
	RefreshAttributeOverlays();
	RefreshSkillOverlays();
	RefreshSpellbookOverlays();
	RefreshEffectsOverlays(true);
	RefreshEffectsOverlays(false);
	RefreshVitaePanelOverlays();
	RefreshLinkStatusPanelOverlays();
	if (PageElementName == TEXT("InventoryPanel_Field") && Client && SelectedPackGuid == 0)
	{
		SelectedPackGuid = Client->GetPlayerGuid();
	}
	if (PageElementName == TEXT("SocialPanel_Field"))
	{
		SyncSocialPanelTab(ActiveSocialTab.IsEmpty() ? TEXT("AllegiancePage") : ActiveSocialTab);
	}
	if (PageElementName == TEXT("OptionsPanel_Field"))
	{
		// Escape must always recover an offscreen settings panel, including
		// when the user normally permits windows beyond the viewport edges.
		Manager->ConstrainWindow(Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")),
			Manager->GetScreenLayoutSize(),true);
		if (Client)
		{
			// Snapshot for the Reset button before any checkbox edits.
			OptionsSnapshot1 = Client->GetCharacterOptions1();
			OptionsSnapshot2 = Client->GetCharacterOptions2();
			OptionsDraft1 = OptionsSnapshot1; OptionsDraft2 = OptionsSnapshot2;
			MainChatFilterSnapshot = MainChatTypeFilter;
			FloatyChatFilterSnapshot = FloatyChatFilters;
			ChatOpacitySnapshot = FVector2D(ChatInactiveOpacity,ChatActiveOpacity);
		}
		SyncOptionsPanelTab(ActiveOptionsTab.IsEmpty() ? TEXT("GameplayOptionsPage") : ActiveOptionsTab);
	}
	else
	{
		HideOptionsOverlays();
	}
	if (PageElementName == TEXT("QuestManagementPanel_Field"))
	{
		RefreshQuestOverlays();
	}
	if (PageElementName == TEXT("WorldPanel_Field"))
	{
		SyncWorldPanelTab(ActiveWorldTab.IsEmpty() ? TEXT("MapPage") : ActiveWorldTab);
	}
	else
	{
		HideWorldOverlays();
	}
	if (PageElementName == TEXT("AbusePanel_Field"))
	{
		if (AbuseNameEntry) { AbuseNameEntry->SetText(FText::GetEmpty()); }
		if (AbuseComplaintEntry) { AbuseComplaintEntry->SetText(FText::GetEmpty()); }
		SyncAbusePanelPage(1);
	}
	else if (PageElementName == TEXT("UrgentAssistancePanel_Field"))
	{
		if (UrgentComplaintEntry) { UrgentComplaintEntry->SetText(FText::GetEmpty()); }
		SyncUrgentPanelPage(1);
	}
	else if (PageElementName == TEXT("BookPanel_Field"))
	{
		RefreshBookOverlays();
	}
	else if (PageElementName == TEXT("MiniGamePanel_Field"))
	{
		RefreshMiniGameOverlays();
		if (PlayerController && PlayerController->GetPawn())
			if (auto* VR = PlayerController->GetPawn()->FindComponentByClass<UACEVRComponent>(); VR && VR->IsActive())
				VR->OpenRetailPanel(NAME_None);
	}
	else
	{
		HideDialogOverlays();
	}
	if (PageElementName == TEXT("PositiveEffectsPanel_Field"))
	{
		bEffectsListPositive = true;
		EffectsScrollOffset = 0;
		RefreshEffectsOverlays(true);
	}
	if (PageElementName == TEXT("NegativeEffectsPanel_Field"))
	{
		bEffectsListPositive = false;
		EffectsScrollOffset = 0;
		RefreshEffectsOverlays(false);
	}
	if (PageElementName == TEXT("VitaePanel_Field"))
	{
		RefreshVitaePanelOverlays();
	}
	if (PageElementName == TEXT("LinkStatusPanel_Field"))
	{
		LastLinkPingRequestAt = 0.0;
		RefreshLinkStatusPanelOverlays();
	}
	// Skill panel stacks Attribute/Skill/Title pages in LayoutDesc — only one visible (retail tabs).
	if (PageElementName == TEXT("SkillManagementPanel_Field"))
	{
		SyncSkillPanelTab(ActiveSkillTab.IsEmpty() ? TEXT("AttributePage") : ActiveSkillTab);
	}
	if (PageElementName == TEXT("SpellManagementPanel_Field"))
	{
		SyncSpellPanelTab(ActiveSpellPanelTab.IsEmpty() ? TEXT("SpellbookPage") : ActiveSpellPanelTab);
	}
	else
	{
		// Leave spellbook — collapse filter checkbox overlays (they are not DAT-clipped).
		SyncSpellbookFilterCheckboxes();
		RefreshSpellbookChromeLabels();
	}
	if (TSharedPtr<FACEUIElement> Panel = Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")))
	{
		Manager->BringFloatyToFront(Panel);
	}
	SyncInventoryButtonVisual();
}

void UACEUIGameplayBinder::HidePanel()
{
	if (ActivePanelPage == TEXT("BookPanel_Field") && Client) Client->ClearBook();
	SetFloatyVisible(TEXT("RootGameplay_FloatyPanel_Field"), false);
	ActivePanelPage.Reset();
	HideWorldOverlays();
	HideDialogOverlays();
	SanitizeFloatyPanelChrome();
	RefreshSocialOverlays();
	RefreshInventoryOverlays();
	RefreshEffectsOverlays(true);
	RefreshEffectsOverlays(false);
	RefreshVitaePanelOverlays();
	RefreshLinkStatusPanelOverlays();
	SyncSpellbookFilterCheckboxes();
	RefreshSpellbookChromeLabels();
	SyncInventoryButtonVisual();
}

void UACEUIGameplayBinder::SyncInventoryButtonVisual()
{
	if (!Manager)
	{
		return;
	}
	const TCHAR* Pages[] = {TEXT("Social"), TEXT("SpellManagement"), TEXT("SkillManagement"), TEXT("QuestManagement"), TEXT("World"), TEXT("Options")};
	const TCHAR* Buttons[] = {TEXT("Social"), TEXT("Magic"), TEXT("SkillManagement"), TEXT("QuestManagement"), TEXT("World"), TEXT("Options")};
	for (int32 I=0; I<UE_ARRAY_COUNT(Pages); ++I)
	{
		if (auto Button = Manager->FindElementByName(FString::Printf(TEXT("PanelButton_%sButton"), Buttons[I])))
			Button->bHighlighted = ActivePanelPage == FString::Printf(TEXT("%sPanel_Field"), Pages[I]);
	}
	const bool bInvOpen = ActivePanelPage == TEXT("InventoryPanel_Field");
	// Retail toolbar: 0x06004CF7 closed bag, 0x06004CF8 open/pressed bag.
	constexpr uint32 DidBagClosed = 0x06004CF7u;
	constexpr uint32 DidBagOpen = 0x06004CF8u;
	if (TSharedPtr<FACEUIElement> Btn = Manager->FindElementByName(TEXT("InventoryButton")))
	{
		Btn->bHighlighted = bInvOpen;
		Btn->ImageFileId = bInvOpen ? DidBagOpen : DidBagClosed;
	}
	// Drag feedback is managed by the hover path and cleared on cancellation.
	if (!bInvDragActive) Manager->SetElementVisibleByName(TEXT("BigBackpack_Icon_DragAccept"), false);
}

void UACEUIGameplayBinder::TogglePanelPage(const FString& PageElementName)
{
	if (ActivePanelPage == PageElementName)
	{
		HidePanel();
	}
	else
	{
		ShowPanelPage(PageElementName);
	}
}

void UACEUIGameplayBinder::ToggleGameplayPanel(const FString& PageElementName, const FString& TabName)
{
	if (TabName.IsEmpty()) { TogglePanelPage(PageElementName); return; }
	const FString CurrentTab = PageElementName == TEXT("SkillManagementPanel_Field") ? ActiveSkillTab
		: PageElementName == TEXT("SpellManagementPanel_Field") ? ActiveSpellPanelTab
		: PageElementName == TEXT("SocialPanel_Field") ? ActiveSocialTab
		: PageElementName == TEXT("WorldPanel_Field") ? ActiveWorldTab
		: PageElementName == TEXT("OptionsPanel_Field") ? ActiveOptionsTab : ActiveQuestTab;
	if (ActivePanelPage == PageElementName && CurrentTab == TabName) { HidePanel(); return; }
	ShowPanelPage(PageElementName);
	if (PageElementName == TEXT("SkillManagementPanel_Field")) SyncSkillPanelTab(TabName);
	else if (PageElementName == TEXT("SpellManagementPanel_Field")) SyncSpellPanelTab(TabName);
	else if (PageElementName == TEXT("SocialPanel_Field")) SyncSocialPanelTab(TabName);
	else if (PageElementName == TEXT("WorldPanel_Field")) SyncWorldPanelTab(TabName);
	else if (PageElementName == TEXT("OptionsPanel_Field")) SyncOptionsPanelTab(TabName);
	else if (PageElementName == TEXT("QuestManagementPanel_Field"))
	{ ActiveQuestTab = TabName; bJournalFieldsDirty = true; RefreshQuestOverlays(); }
}

void UACEUIGameplayBinder::HandleEscape()
{
	if (PendingUseWithSourceGuid || (PlayerController && PlayerController->GetObjectCursorTargeting()))
	{
		CancelPendingUseWith();
		return;
	}
	if (!Manager) return;
	if (bChatTargetPopupOpen) { CloseChatTargetPopup(); return; }
	if (bVendorFilterDropdownOpen) { CloseVendorFilterDropdown(); return; }
	for (const TCHAR* Name : {TEXT("RootGameplay_Keyboard_Field"), TEXT("RootKeyboard_Field")})
	{
		const auto Keyboard = Manager->FindElementByName(Name);
		if (Keyboard && Keyboard->bVisible)
		{
			ACEInputBindings::Cancel();
			Manager->SetElementVisibleByName(Name, false);
			RefreshKeyboardOverlays();
			return;
		}
	}
	// Deselect is a complete Escape action. Keep it here so both Slate keyboard
	// focus and the viewport/controller path leave the current panel intact.
	if (Client && Client->GetSelectedObject().Guid != 0)
	{
		Client->SelectObject(0);
		return;
	}
	const auto Exam = Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
	if (Exam && Exam->bVisible) { ShowExamination(false); RefreshExaminationOverlay(); return; }
	if (OpenLootContainerGuid != 0) { HideExternalContainer(true); return; }
	if (OpenVendorGuid != 0) { HideVendorPanel(); return; }
	if (bTradeOpen) { HideTradePanel(true); return; }
	if (OpenSalvageToolGuid != 0) { HideSalvagePanel(); return; }
	if (OpenHouseLord != 0) { CloseHouseOffer(); return; }
	if (ActivePanelPage == TEXT("OptionsPanel_Field")) { HidePanel(); return; }
	ActiveOptionsTab = TEXT("GameplayOptionsPage");
	ShowPanelPage(TEXT("OptionsPanel_Field"));
}

void UACEUIGameplayBinder::ToggleKeyboardMappingUI()
{
	if (!Manager)
	{
		return;
	}
	TSharedPtr<FACEUIElement> El = Manager->FindElementByName(TEXT("RootGameplay_Keyboard_Field"));
	if (!El.IsValid())
	{
		El = Manager->FindElementByName(TEXT("RootKeyboard_Field"));
	}
	if (!El.IsValid())
	{
		PostInventorySystemMessage(
			TEXT("Keyboard: W/X move, A/D turn, Z/C sidestep, Q autorun, Shift walk, Space jump, ")
			TEXT("R use, E examine, F12 inventory, F5 spells, F11 options, ~ combat, Enter chat."));
		return;
	}
	const bool bOn = !El->bVisible;
	// Binding capture consumes gameplay shortcuts, including the UI toggle.
	// Always reveal the editor before entering that mode.
	if (bOn && PlayerController) PlayerController->SetDesktopInterfaceHidden(false);
	if (bOn) ACEInputBindings::BeginEdit(); else ACEInputBindings::Cancel();
	El->bVisible = bOn;
	Manager->SetElementVisibleByName(El->ElementName, bOn);
	if (bOn) Manager->BringFloatyToFront(El);
}

void UACEUIGameplayBinder::ToggleCombatModeHotkey()
{
	if (CombatMode == static_cast<int32>(ACECombatMode::NonCombat))
	{
		ApplyCombatMode(ResolveEquippedCombatMode());
	}
	else
	{
		ApplyCombatMode(static_cast<int32>(ACECombatMode::NonCombat));
	}
}

void UACEUIGameplayBinder::PlayEmoteHotkey(uint32 MotionCommand, bool bHoldPose)
{
	if (!Client || MotionCommand == 0)
	{
		return;
	}
	// Keyboard poses use the same DAT text as typed *poses*. Resolve aliases to
	// the actual motion, so different bindings cannot produce different wording.
	if (auto* GI = PlayerController ? PlayerController->GetGameInstance() : nullptr)
	if (auto* Dat = GI->GetSubsystem<UACEDatSubsystem>())
	{
		TArray<FString> Keys;
		Dat->GetChatPoseKeys(Keys);
		bool bFound = false;
		for (bool bStateVariant : {false, true})
		{
			for (const FString& Key : Keys)
			{
				FString Command, My, Other; uint32 Motion = 0;
				if (!Dat->TryGetChatPose(Key, Command, My, Other)) continue;
				FString Name = Command.ToLower();
				// Retail binds some one-shot commands (e.g. BowDeep), while its
				// chat table names only their held-pose variants (BowDeepState).
				// Prefer exact matches before falling back to the same gesture.
				if (bStateVariant && !Name.RemoveFromEnd(TEXT("state"))) Name += TEXT("state");
				ACEMotionName::TryResolve(Name, Motion);
				if (Motion == MotionCommand) { SendPoseChat(Command, My, Other); bFound = true; break; }
			}
			if (bFound) break;
		}
	}
	Client->SendSoulEmoteMotion(static_cast<int32>(MotionCommand));
	APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	UACECharacterAppearanceComponent* App = Pawn
		? Pawn->FindComponentByClass<UACECharacterAppearanceComponent>()
		: nullptr;
	if (App)
	{
		App->PlayActionMotion(static_cast<int32>(MotionCommand), 1.f, 0, bHoldPose);
	}
}

void UACEUIGameplayBinder::ApplyPanelTabChrome(const FString& TabElementName, bool bSelected)
{
	if (!Manager || TabElementName.IsEmpty())
	{
		return;
	}
	// Prefer the tab under the active skill/spell panel — FindElementByName can hit a hidden
	// duplicate from another embedded layout with the same ElementName.
	TSharedPtr<FACEUIElement> Tab = Manager->FindElementUnder(ActivePanelPage, TabElementName);
	if (ActivePanelPage == TEXT("SkillManagementPanel_Field"))
	{
		Tab = Manager->FindElementUnder(TEXT("SkillManagementPanel_Field"), TabElementName);
	}
	else if (ActivePanelPage == TEXT("SpellManagementPanel_Field"))
	{
		Tab = Manager->FindElementUnder(TEXT("SpellManagementPanel_Field"), TabElementName);
	}
	else if (ActivePanelPage == TEXT("OptionsPanel_Field"))
	{
		Tab = Manager->FindElementUnder(TEXT("OptionsPanel_Field"), TabElementName);
	}
	else if (ActivePanelPage == TEXT("SocialPanel_Field"))
	{
		Tab = Manager->FindElementUnder(TEXT("SocialPanel_Field"), TabElementName);
	}
	else if (ActivePanelPage == TEXT("WorldPanel_Field"))
	{
		Tab = Manager->FindElementUnder(TEXT("WorldPanel_Field"), TabElementName);
	}
	if (!Tab.IsValid())
	{
		Tab = Manager->FindElementByName(TabElementName);
	}
	if (!Tab.IsValid())
	{
		return;
	}
	// PanelTabTemplate / Tab_Blue: odd DIDs = selected (raised / connected), even = idle.
	// UIElement_Panel::OpenTab sets 12 for the open tab and 11 for closed tabs.
	if (Tab->States.Contains(11) || Tab->States.Contains(12))
	{
		Tab->DefaultState = bSelected ? 12u : 11u;
		Tab->bUseExplicitState = true;
		return;
	}
	constexpr uint32 DidLeftSel = 0x06005D93, DidMidSel = 0x06005D95, DidRightSel = 0x06005D97;
	constexpr uint32 DidLeftIdle = 0x06005D92, DidMidIdle = 0x06005D94, DidRightIdle = 0x06005D96;
	const uint32 Left = bSelected ? DidLeftSel : DidLeftIdle;
	const uint32 Mid = bSelected ? DidMidSel : DidMidIdle;
	const uint32 Right = bSelected ? DidRightSel : DidRightIdle;
	auto ApplyChild = [](const TSharedPtr<FACEUIElement>& Node, uint32 Did)
	{
		if (!Node.IsValid()) { return; }
		Node->ImageFileId = Did;
		Node->bVisible = true;
		Node->DrawMode = 3;
	};
	TArray<TSharedPtr<FACEUIElement>> Stack;
	Stack.Add(Tab);
	while (Stack.Num() > 0)
	{
		TSharedPtr<FACEUIElement> Node = Stack.Pop(EAllowShrinking::No);
		if (!Node.IsValid()) { continue; }
		if (Node->ElementName == TEXT("LeftEnd") || Node->ElementName == TEXT("left"))
		{
			ApplyChild(Node, Left);
		}
		else if (Node->ElementName == TEXT("middle") || Node->ElementName == TEXT("Middle"))
		{
			ApplyChild(Node, Mid);
		}
		else if (Node->ElementName == TEXT("RightEnd") || Node->ElementName == TEXT("right"))
		{
			ApplyChild(Node, Right);
		}
		for (const TSharedPtr<FACEUIElement>& Ch : Node->Children)
		{
			Stack.Add(Ch);
		}
	}
}

void UACEUIGameplayBinder::SyncSpellcastTabChrome()
{
	if (!Manager || !Client)
	{
		return;
	}
	constexpr uint32 DidActive = 0x06005EB8;
	constexpr uint32 DidIdle = 0x06005EB9;
	const int32 ActiveBar = Client->GetActiveSpellBar();
	for (int32 i = 1; i <= 8; ++i)
	{
		const FString Name = FString::Printf(TEXT("Spellcast_Tab%d"), i);
		if (TSharedPtr<FACEUIElement> Tab = Manager->FindElementByName(Name))
		{
			Tab->ImageFileId = (i - 1 == ActiveBar) ? DidActive : DidIdle;
			Tab->bVisible = true;
			// gmSpellcastingUI handles clicks on these authored Text controls.
			Tab->bActivatable = true;
			// Retail UIElement_Panel::Update: selected=12, closed=11.
			Tab->DefaultState = (i - 1 == ActiveBar) ? 12u : 11u;
		}
	}
}

const TCHAR* UACEUIGameplayBinder::GetStatSelectionFooter() const
{
	const FACESkillInfo* Skill = LastVitals.Skills.FindByPredicate(
		[this](const FACESkillInfo& Entry) { return Entry.SkillId == SelectedSkillId; });
	// gmSkillUI uses the text footer for training and the XP meter for trained skills.
	return ActiveSkillTab == TEXT("SkillPage") && Skill && Skill->AdvancementClass >= 2
		? TEXT("StatManagement_Footer_Meter") : TEXT("StatManagement_Footer_Text");
}

void UACEUIGameplayBinder::SyncRaiseButtonChrome(bool bCanRaise1, bool bCanRaise10)
{
	if (!Manager)
	{
		return;
	}
	const FString Page = ActiveSkillTab.IsEmpty() ? TEXT("AttributePage") : ActiveSkillTab;
	const FString Footer = GetStatSelectionFooter();
	const bool bShowRaise10 = Page != TEXT("SkillPage") || Footer == TEXT("StatManagement_Footer_Meter");
	auto SetBtn = [&](const FString& BtnName, uint32 LitDid, uint32 DarkDid, bool bLit, bool bVisible)
	{
		TSharedPtr<FACEUIElement> Btn;
		if (TSharedPtr<FACEUIElement> Foot = Manager->FindElementUnder(Page, Footer))
		{
			TArray<TSharedPtr<FACEUIElement>> Stack;
			Stack.Add(Foot);
			while (Stack.Num() > 0 && !Btn.IsValid())
			{
				TSharedPtr<FACEUIElement> Cur = Stack.Pop(EAllowShrinking::No);
				if (!Cur.IsValid()) { continue; }
				if (Cur->ElementName == BtnName) { Btn = Cur; break; }
				for (const TSharedPtr<FACEUIElement>& Child : Cur->Children)
				{
					Stack.Add(Child);
				}
			}
		}
		if (Btn.IsValid())
		{
			Btn->ImageFileId = bLit ? LitDid : DarkDid;
			Btn->bVisible = bVisible;
			Btn->bActivatable = bVisible && bLit;
		}
	};
	SetBtn(TEXT("StatManagement_Footer_RaiseButton"), DidRaise1Lit, DidRaise1Dark, bCanRaise1, true);
	SetBtn(TEXT("StatManagement_Footer_Raise10Button"), DidRaise10Lit, DidRaise10Dark, bCanRaise10, bShowRaise10);
}

void UACEUIGameplayBinder::SyncSkillPanelTab(const FString& PageName)
{
	if (!Manager)
	{
		return;
	}
	ActiveSkillTab = PageName;
	static const TCHAR* Tabs[] = {
		TEXT("AttributePage"), TEXT("SkillPage"), TEXT("CharacterTitlePage"),
	};
	for (const TCHAR* Tab : Tabs)
	{
		Manager->SetElementVisibleByName(Tab, PageName == Tab);
	}
	SyncStatFooterChrome();
	ApplyPanelTabChrome(TEXT("AttributeTab"), PageName == TEXT("AttributePage"));
	ApplyPanelTabChrome(TEXT("SkillTab"), PageName == TEXT("SkillPage"));
	ApplyPanelTabChrome(TEXT("CharacterTitleTab"), PageName == TEXT("CharacterTitlePage"));
}

void UACEUIGameplayBinder::SyncStatFooterChrome()
{
	if (!Manager) return;
	const bool bAttributeSelected = SelectedAttributeRow >= 0 && SelectedAttributeRow < 9;
	const bool bSkillSelected = SelectedSkillId != 0 && LastVitals.Skills.ContainsByPredicate(
		[this](const FACESkillInfo& Skill) { return Skill.SkillId == SelectedSkillId; });
	const bool bTraining = GetStatSelectionFooter() == FString(TEXT("StatManagement_Footer_Text"));
	// Retail footers sit on each page — toggle under the active page only (duplicate names).
	auto SetFooter = [this](const FString& Page, const FString& Footer, bool bVis)
	{
		if (TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(Page, Footer))
		{
			El->bVisible = bVis;
		}
	};
	for (const TCHAR* Tab : {TEXT("AttributePage"), TEXT("SkillPage"), TEXT("CharacterTitlePage")})
	{
		const bool bActive = ActiveSkillTab == Tab;
		const bool bText = bActive && ((ActiveSkillTab == TEXT("AttributePage") && bAttributeSelected)
			|| (ActiveSkillTab == TEXT("SkillPage") && bSkillSelected && bTraining));
		const bool bMeter = bActive && ActiveSkillTab == TEXT("SkillPage") && bSkillSelected && !bTraining;
		SetFooter(Tab, TEXT("StatManagement_Footer_Default"), bActive && !bText && !bMeter);
		SetFooter(Tab, TEXT("StatManagement_Footer_Text"), bText);
		SetFooter(Tab, TEXT("StatManagement_Footer_Meter"), bMeter);
	}
}

void UACEUIGameplayBinder::SyncSpellPanelTab(const FString& PageName)
{
	if (!Manager)
	{
		return;
	}
	ActiveSpellPanelTab = PageName;
	SpellbookScrollOffset = 0;
	ComponentScrollOffset = 0;
	Manager->SetElementVisibleByName(TEXT("SpellbookPage"), PageName == TEXT("SpellbookPage"));
	Manager->SetElementVisibleByName(TEXT("SpellComponentPage"), PageName == TEXT("SpellComponentPage"));
	ApplyPanelTabChrome(TEXT("SpellbookTab"), PageName == TEXT("SpellbookPage"));
	ApplyPanelTabChrome(TEXT("SpellComponentTab"), PageName == TEXT("SpellComponentPage"));
	if (PageName == TEXT("SpellbookPage"))
	{
	}
	// Always sync so filter overlays collapse when leaving Spellbook (Components tab).
	SyncSpellbookFilterCheckboxes();
	RefreshSpellbookChromeLabels();
	RefreshComponentOverlays();
}

void UACEUIGameplayBinder::ShowExamination(bool bVisible)
{
	if (!bVisible && EditingInscriptionGuid && ExamInscriptionEditor)
		HandleInscriptionCommitted(ExamInscriptionEditor->GetText(), ETextCommit::OnUserMovedFocus);

	if (!bVisible && Client)
	{
		DismissedIdentifySerial = Client->GetIdentifyRequestSerial();
		bExaminationDismissed = true;
	}
	SetFloatyVisible(TEXT("RootGameplay_FloatyExamination_Field"), bVisible);
	if (!bVisible)
	{
		return;
	}
	EnsureExamineItemChrome();
	EnsureExamineCreatureChrome();
	SyncExamineBodyVisibility(LastAppraisal.bIsCreature);
	if (Manager)
	{
		Manager->SyncLockedChromeVisibility();
	}
}

void UACEUIGameplayBinder::EnsureExamineCreatureChrome()
{
	// Layout 0x2100006B already supplies the frame. Do not graft 0x2100001C here.
}

void UACEUIGameplayBinder::EnsureExamineItemChrome()
{
	// Keep the resolved floaty layout, including its text viewport and paper strip.
}

void UACEUIGameplayBinder::SyncExamineBodyVisibility(bool bCreature)
{
	if (!Manager) return;
	Manager->SetElementVisibleByName(TEXT("BasicCreatureExamineUI"), !ExaminedSpellId && bCreature);
	Manager->SetElementVisibleByName(TEXT("ItemExamineUI"), !ExaminedSpellId && !bCreature);
	Manager->SetElementVisibleByName(TEXT("SpellExamineUI"), ExaminedSpellId != 0);
	// Conditional children retain their authored states. Revealing entire trees
	// stacked the paperdoll, attributes and both scrollbar variants together.
	Manager->SetElementVisibleByName(TEXT("ItemDisplayTextScrollbar_Reveal"), false);
}

void UACEUIGameplayBinder::ApplyCombatMode(int32 Mode)
{
	ApplyCombatModeInternal(Mode, true);
}

void UACEUIGameplayBinder::ApplyCombatModeInternal(int32 Mode, bool bSendToServer)
{
	// ClientCombatSystem::SetCombatMode rejects player requests in portal space.
	// Server stance updates must still be applied, without optimistic UI changes.
	if (bSendToServer && Client && Client->IsInPortalSpace())
	{
		Client->SendChangeCombatMode(Mode); // retail feedback, no packet
		return;
	}
	if (Mode != CombatMode) CancelCombatAttack();
	CombatMode = Mode;
	if (bSendToServer)
	{
		if (Mode == static_cast<int32>(ACECombatMode::NonCombat))
		{
			PendingCombatMode = 0;
			PendingCombatModeUntil = 0.0;
		}
		else
		{
			PendingCombatMode = Mode;
			PendingCombatModeUntil = FPlatformTime::Seconds() + 5.0;
		}
	}
	else
	{
		PendingCombatMode = 0;
	}
	if (Client && bSendToServer)
	{
		Client->SendChangeCombatMode(CombatMode);
	}
	ApplyPreferredStance(CombatMode);
	SyncCombatModeButtons();
	LastAppliedStanceKey = (CombatMode << 8) | ResolveEquippedCombatMode();
	LastEquipModeSnapshot = ResolveEquippedCombatMode();
	const bool bInCombat = CombatMode != static_cast<int32>(ACECombatMode::NonCombat);
	SetFloatyVisible(TEXT("RootGameplay_FloatyCombatPanel_Field"), bInCombat
		&& (CombatMode == int32(ACECombatMode::Magic) || !Client || !Client->IsCharacterOptionSet(0x0C)));
	if (bInCombat)
	{
		if (CombatMode == static_cast<int32>(ACECombatMode::Melee)
				|| CombatMode == static_cast<int32>(ACECombatMode::Missile))
		{
			TryAutoTargetOnCombatEnter();
		}
		SetFloatyVisible(TEXT("RootGameplay_FloatyEnvPanel_Field"),
			OpenLootContainerGuid != 0 || OpenVendorGuid != 0 || bTradeOpen || OpenSalvageToolGuid != 0);
		const bool bMagic = CombatMode == static_cast<int32>(ACECombatMode::Magic);
		if (Manager)
		{
			Manager->SetElementVisibleByName(TEXT("Spellcasting"), bMagic);
			Manager->SetElementVisibleByName(TEXT("CombatPanel"), !bMagic);
			for (int32 i = 0; i < 8; ++i)
			{
				Manager->SetElementVisibleByName(
					FString::Printf(TEXT("Spellcasting_Bank%d"), i + 1),
					bMagic && Client && Client->GetActiveSpellBar() == i);
			}
			SyncSpellcastTabChrome();
		}
		if (bMagic)
		{
			// Fixed 13-slot layout (right arrow against last slot) is applied in RefreshSpellHotbarOverlays.
			if (Manager)
			{
				Manager->SetElementVisibleByName(TEXT("CastSpellButton"), true);
			}
			RefreshSpellHotbarOverlays();
		}
		else
		{
			FitCombatFloatyContentWidth(600);
			if (CastSpellLabel)
			{
				CastSpellLabel->SetVisibility(ESlateVisibility::Collapsed);
			}
			RefreshCombatPanelOverlays();
		}
	}
	else
	{
		bCombatPowerCharging = false;
		bCombatAttackRequestPending = false;
		bCombatRepeatActive = false;
	}
}

void UACEUIGameplayBinder::SyncCombatModeFromServer(int32 Mode)
{
	ApplyCombatModeInternal(Mode, /*bSendToServer*/ false);
}

void UACEUIGameplayBinder::ApplyPreferredStance(int32 Mode)
{
	if (!PlayerController)
	{
		return;
	}
	APawn* P = PlayerController->GetPawn();
	if (!P)
	{
		return;
	}
	UACECharacterAppearanceComponent* App = P->FindComponentByClass<UACECharacterAppearanceComponent>();
	if (!App)
	{
		return;
	}
	uint32 Stance = ACEMotion::StanceNonCombat;
	if (Client)
	{
		const TArray<FACEWorldObject> Equipped = Client->GetEquippedItems();
		Stance = ACECombatStance::ResolveForCombatMode(Equipped, static_cast<uint32>(Mode));
		if (TSharedPtr<FACESession> Session = Client->GetSession())
		{
			Session->SetCurrentStance(Stance);
		}
	}
	App->SetPreferredStyle(static_cast<int32>(Stance));
}

void UACEUIGameplayBinder::RefreshCombatPanelOverlays()
{
	if (Client && Client->GetSessionState() == EACESessionState::InWorld)
	{
		bCombatAutoRepeat = Client->IsCharacterOptionSet(0x00);
		bCombatAutoTarget = Client->IsCharacterOptionSet(0x0D);
		bCombatViewTarget = Client->IsCharacterOptionSet(0x07);
	}
	const bool bMeleeOrMissile = CombatMode == static_cast<int32>(ACECombatMode::Melee)
		|| CombatMode == static_cast<int32>(ACECombatMode::Missile);
	const bool bAdvanced = Client && Client->IsCharacterOptionSet(0x0C);
	if (bAdvanced != bCombatAdvancedUI)
	{
		bCombatAdvancedUI = bAdvanced;
		bCombatPowerDrag = false;
		if (bMeleeOrMissile) SetFloatyVisible(TEXT("RootGameplay_FloatyCombatPanel_Field"), !bAdvanced);
	}
	RefreshPowerbarOverlay();
	auto HideCombatOverlays = [this]()
	{
		auto Collapse = [](UWidget* W)
		{
			if (W) { W->SetVisibility(ESlateVisibility::Collapsed); }
		};
		Collapse(CombatSpeedLabel);
		Collapse(CombatPowerLabel);
		Collapse(CombatPowerFill);
		Collapse(CombatCheckboxAutoRepeat);
		Collapse(CombatCheckboxAutoTarget);
		Collapse(CombatCheckboxViewTarget);
		for (UTextBlock* T : CombatAttackLabels) { Collapse(T); }
		for (UTextBlock* T : CombatCheckboxLabels) { Collapse(T); }
	};
	if (!bMeleeOrMissile || bAdvanced || !Manager || !Canvas || !Canvas->WidgetTree)
	{
		HideCombatOverlays();
		return;
	}
	TSharedPtr<FACEUIElement> CombatRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"));
	TSharedPtr<FACEUIElement> CombatPanel = Manager->FindElementUnder(
		TEXT("RootGameplay_FloatyCombatPanel_Field"), TEXT("CombatPanel"));
	if (!CombatRoot.IsValid() || !CombatRoot->bVisible || !CombatPanel.IsValid() || !CombatPanel->bVisible)
	{
		HideCombatOverlays();
		return;
	}

	// Melee/missile authored frame is 600 — keep internal black fill flush with gold chrome.
	FitCombatFloatyContentWidth(600);

	EnsureOverlays();
	const bool bMissile = CombatMode == static_cast<int32>(ACECombatMode::Missile);
	PlaceTextOnElement(CombatSpeedLabel, TEXT("Speed"), TEXT("Speed"), 8, TextGold, 620);
	PlaceTextOnElement(CombatPowerLabel, TEXT("Power"),
		bMissile ? TEXT("Accuracy") : TEXT("Power"), 8, TextGold, 620);

	while (CombatCheckboxLabels.Num() < 3)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		L->SetJustification(ETextJustify::Left);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8);
		L->SetFont(Font);
		CombatCheckboxLabels.Add(L);
	}
	auto PlaceCheckboxLabel = [&](int32 Index, const FString& ElementName, const FString& Contents)
	{
		UTextBlock* Text = CombatCheckboxLabels[Index];
		TSharedPtr<FACEUIElement> El = Manager->FindElementByName(ElementName);
		if (!Text || !El.IsValid())
		{
			if (Text) { Text->SetVisibility(ESlateVisibility::Collapsed); }
			return;
		}
		// The DAT text state already reserves 18 pixels for its checkbox child.
		PlaceTextOnElement(Text, El, Contents, 8, TextWhite, 621);
	};
	PlaceCheckboxLabel(0, TEXT("AutoRepeatAttack"), TEXT("Auto Repeat"));
	PlaceCheckboxLabel(1, TEXT("AutoTarget"), TEXT("Auto Target"));
	PlaceCheckboxLabel(2, TEXT("ViewCombatTarget"), TEXT("View Combat"));

	while (CombatAttackLabels.Num() < 3)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		L->SetJustification(ETextJustify::Center);
		CombatAttackLabels.Add(L);
	}
	const FLinearColor HiCol = CombatAttackHeight == ACEAttackHeight::High ? TextGold : TextWhite;
	const FLinearColor MedCol = CombatAttackHeight == ACEAttackHeight::Medium ? TextGold : TextWhite;
	const FLinearColor LowCol = CombatAttackHeight == ACEAttackHeight::Low ? TextGold : TextWhite;
	PlaceTextOnElement(CombatAttackLabels[0], TEXT("HighAttack"), TEXT("High"), 8, HiCol, 622);
	PlaceTextOnElement(CombatAttackLabels[1], TEXT("MediumAttack"), TEXT("Medium"), 8, MedCol, 622);
	PlaceTextOnElement(CombatAttackLabels[2], TEXT("LowAttack"), TEXT("Low"), 8, LowCol, 622);
	for (const TCHAR* Name : {TEXT("HighAttack"),TEXT("MediumAttack"),TEXT("LowAttack")})
	{
		if (auto El = Manager->FindElementByName(Name))
			El->bHighlighted = FCString::Strcmp(Name,TEXT("HighAttack")) == 0 ? CombatAttackHeight == ACEAttackHeight::High
				: FCString::Strcmp(Name,TEXT("LowAttack")) == 0 ? CombatAttackHeight == ACEAttackHeight::Low : CombatAttackHeight == ACEAttackHeight::Medium;
	}

	auto SetCheckbox = [&](const FString& Name, bool bChecked, TObjectPtr<UBorder>& Border)
	{
		TSharedPtr<FACEUIElement> El = Manager->FindElementByName(Name);
		if (!El.IsValid())
		{
			return;
		}
		El->bHighlighted = bChecked;
		El->bActivatable = true;
		// Composite button states are passed to the authored image child (4D15..4D19).
		// Do not replace them with the unrelated round option-button icons.
		for (const TSharedPtr<FACEUIElement>& Child : El->Children)
		{
			if (Child.IsValid())
			{
				Child->bActivatable = false;
			}
		}
		if (Border) { Border->SetVisibility(ESlateVisibility::Collapsed); }
	};
	SetCheckbox(TEXT("AutoRepeatAttack"), bCombatAutoRepeat, CombatCheckboxAutoRepeat);
	SetCheckbox(TEXT("AutoTarget"), bCombatAutoTarget, CombatCheckboxAutoTarget);
	SetCheckbox(TEXT("ViewCombatTarget"), bCombatViewTarget, CombatCheckboxViewTarget);

	const float DisplayPower = (bCombatPowerCharging || bCombatRequestSent || bCombatRepeatActive) ? CombatPowerOrAccuracy : 0.f;

	if (TSharedPtr<FACEUIElement> Meter = Manager->FindElementByName(TEXT("PowerMeter")))
	{
		Meter->MeterFillFraction = FMath::Clamp(DisplayPower, 0.f, 1.f);
	}
	if (TSharedPtr<FACEUIElement> Fill = Manager->FindElementByName(TEXT("basic_recklessness_fill")))
	{
		// gmCombatUI gates the authored band on Recklessness training.
		Fill->bVisible = LastVitals.Skills.ContainsByPredicate([](const FACESkillInfo& Skill)
			{ return Skill.SkillId == 50 && Skill.AdvancementClass >= 2; });
		if (TSharedPtr<FACEUIElement> Parent = Fill->Parent.Pin())
		{
			Parent->MeterFillFraction = FMath::Clamp(DisplayPower, 0.f, 1.f);
		}
	}

	// Move the authored thumb; drawing another overlay left two handles on the track.
	if (CombatPowerFill) { CombatPowerFill->SetVisibility(ESlateVisibility::Collapsed); }
	if (TSharedPtr<FACEUIElement> Slider = Manager->FindElementByName(TEXT("PowerSlider")))
	{
		for (const TSharedPtr<FACEUIElement>& Child : Slider->Children)
		{
			if (Child.IsValid() && Child->ElementId == 1)
			{
				Child->X = FMath::RoundToInt(FMath::Clamp(RequestedAttackPower, 0.f, 1.f)
					* FMath::Max(0, Slider->Width - Child->Width));
				Child->bActivatable = false;
			}
		}
	}
}

void UACEUIGameplayBinder::TickCombatAutoAttack(float /*DeltaSeconds*/)
{
	if (Client && Client->IsInPortalSpace())
	{
		PendingAutoTargetLoss = 0;
		CancelCombatAttack();
		return;
	}
	if (Client && (CombatMode == int32(ACECombatMode::Melee) || CombatMode == int32(ACECombatMode::Missile)))
	{
		if (PendingAutoTargetLoss)
		{
			const int32 Lost = PendingAutoTargetLoss; PendingAutoTargetLoss = 0;
			FACEWorldObject Previous;
			const auto Selection=Client->GetSelectedObject();
			// ObjectDelete clears selection before erasing the object. Defer this
			// check until the event is complete; an intentional clear of a living
			// target must not immediately select it again.
			if (!Client->GetWorldObject(Lost,Previous) || Previous.bDying
				|| (Selection.Guid==Lost && Selection.bShowHealth && Selection.HealthFraction<=0.f))
				TryAutoTargetOnCombatEnter(Lost);
		}
	}
	else PendingAutoTargetLoss=0;
	if (PlayerController && PlayerController->IsVRActive())
	{
		bCombatRetargetPending = false;
		bCombatRepeatActive = bCombatAttackRequestPending = bCombatPowerCharging = false;
		return;
	}
	if (CombatMode != static_cast<int32>(ACECombatMode::Melee)
		&& CombatMode != static_cast<int32>(ACECombatMode::Missile))
	{
		bCombatRetargetPending = false;
		return;
	}

	const double Now = FPlatformTime::Seconds();
	const bool AdvancedCombat = Client && Client->IsCharacterOptionSet(0x0C);
	if (Client && Client->GetSession())
	{
		const auto Session = Client->GetSession();
		if (Session->GetCombatEventRevision() != LastCombatEventRevision)
		{
			LastCombatEventRevision = Session->GetCombatEventRevision();
			if (!Session->IsServerAttackInProgress()) bCombatRequestSent = false;
			if (Session->GetLastAttackError() != 0)
			{
				// Retail HandleAttackDone stops automatic repeat, but preserves a
				// new manual request. This response can belong to the shot cancelled
				// before the player pressed/held a different attack height.
				bCombatRepeatActive = false;
			}
			// HandleCommenceAttack preserves a new manual build. AttackDone also
			// preserves a still-held build; a released request queued behind the
			// previous attack starts again with its saved power when acknowledged.
			const bool bKeepManualBuild = bCombatAttackRequestPending && bCombatPowerCharging
				&& (Session->IsServerAttackInProgress() || bCombatAttackHeld);
			if (!bKeepManualBuild)
			{
				bCombatPowerCharging = !Session->IsServerAttackInProgress()
					&& (bCombatAttackRequestPending || (bCombatRepeatActive && bCombatAutoRepeat));
				CombatPowerBuildStartTime = Now;
				// Hold the requested mark through the swing and refill on AttackDone.
				CombatPowerOrAccuracy = Session->IsServerAttackInProgress() && !bCombatAttackRequestPending
					? (AdvancedCombat ? LastCombatAttackPower : RequestedAttackPower) : 0.f;
			}
			// ClientCombatSystem::HandleAttackDone only replaces a server repeat when
			// the desired slider power changed. Sending every second restarted melee turns.
			if (!bCombatPluginAttack && !AdvancedCombat && !bCombatMovementBlocked && IsCombatPoseReady() && bCombatPowerCharging && bCombatRepeatActive && !bCombatAttackRequestPending
				&& !FMath::IsNearlyEqual(LastCombatAttackPower, RequestedAttackPower, .01f))
			{
				CombatPowerOrAccuracy = RequestedAttackPower;
				FireCombatAttack();
				// This changes the server's queued repeat power. It is not the
				// start of the next swing; keep displaying the current refill.
				LastCombatEventRevision = Session->GetCombatEventRevision();
				CombatPowerOrAccuracy = 0.f;
			}
		}
		if (bCombatRepeatActive)
		{
			if (!Client->GetSession() || !Client->GetSession()->IsCombatTarget(LastCombatAttackTarget))
			{
				bCombatRepeatActive = false;
				bCombatPowerCharging = bCombatAttackRequestPending;
			}
		}
	}
	// Selection cancels the old server repeat. Keep the replacement across
	// Reload/Aim and the cancellation acknowledgement; never shoot mid-reload.
	if (bCombatRetargetPending && Client && Client->GetSession())
	{
		const auto Session = Client->GetSession();
		if (!bCombatAutoRepeat || !Session->IsCombatTarget(Client->GetSelectedObject().Guid))
			bCombatRetargetPending = false;
		else if (!bCombatMovementBlocked && IsCombatPoseReady() && !Session->IsServerAttackInProgress())
		{
			bCombatRetargetPending = false;
			CombatPowerOrAccuracy = AdvancedCombat ? LastCombatAttackPower : RequestedAttackPower;
			FireCombatAttack();
			return;
		}
	}
	if (bCombatMovementBlocked || !IsCombatPoseReady())
	{
		bCombatPowerCharging = false;
		CombatPowerOrAccuracy = 0.f;
		bCombatAttackRequestPending = bCombatAttackHeld;
		return;
	}
	// Retail UseTime retries a still-held request after Reload becomes Ready.
	// Time spent waiting is not attack charge time.
	if (!bCombatPowerCharging && bCombatAttackHeld && bCombatAttackRequestPending
		&& (!Client || !Client->GetSession() || !Client->GetSession()->IsServerAttackInProgress()))
	{
		bCombatPowerCharging = true;
		CombatPowerBuildStartTime = Now;
	}
	if (!bCombatPowerCharging) return;
	const float T = GetCombatPowerChargeDuration();
	CombatPowerOrAccuracy = FMath::Clamp(static_cast<float>(Now-CombatPowerBuildStartTime)
		/ FMath::Max(.05f,T),0.f,1.f);
	if (!bCombatAttackRequestPending && bCombatRepeatActive)
		CombatPowerOrAccuracy = FMath::Min(CombatPowerOrAccuracy, AdvancedCombat ? LastCombatAttackPower : RequestedAttackPower);
	if (bCombatAttackRequestPending && !bCombatAttackHeld
		&& (!Client || !Client->GetSession() || !Client->GetSession()->IsServerAttackInProgress())
		&& CombatPowerOrAccuracy >= ReleasedAttackPower - KINDA_SMALL_NUMBER)
	{
		bCombatAttackRequestPending = false;
		bCombatPowerCharging = false;
		CombatPowerOrAccuracy = ReleasedAttackPower;
		FireCombatAttack();
	}
}

void UACEUIGameplayBinder::ShowPluginAttack(int32 Target, uint32 Height, float Power)
{
	bCombatRetargetPending = false;
	bCombatPluginAttack = true;
	// Observe the already-sent action. Never send a second attack from the UI.
	CombatAttackHeight = Height;
	RequestedAttackPower = LastCombatAttackPower = CombatPowerOrAccuracy = FMath::Clamp(Power, 0.f, 1.f);
	LastCombatAttackTarget = Target;
	bCombatRequestSent = true;
	bCombatRepeatActive = Client && Client->IsCharacterOptionSet(0x00); // AutoRepeatAttack
	bCombatPowerCharging = bCombatAttackRequestPending = bCombatAttackHeld = false;
	if (Client && Client->GetSession()) LastCombatEventRevision = Client->GetSession()->GetCombatEventRevision();
}

float UACEUIGameplayBinder::GetCombatPowerChargeDuration() const
{
	// Retail GetPowerBarLevel: T = 0.8 DualWieldCombat (0x80000046), else 1.0.
	uint32 Stance = ACEMotion::StanceNonCombat;
	if (Client && Client->GetSession())
	{
		Stance = Client->GetSession()->GetCurrentStance();
	}
	return (Stance == ACEMotion::StanceDualWield) ? 0.8f : 1.0f;
}

void UACEUIGameplayBinder::BeginCombatPowerCharge(uint32 AttackHeight, bool bHeld)
{
	if (Client && Client->IsInPortalSpace()) return;
	if (PlayerController && PlayerController->IsVRActive()) return;
	if (CombatMode != static_cast<int32>(ACECombatMode::Melee)
		&& CombatMode != static_cast<int32>(ACECombatMode::Missile))
	{
		return;
	}
	CombatAttackHeight = AttackHeight;
	bCombatRetargetPending = false;
	bCombatPluginAttack = false; // a manual attack takes over from automation
	if (PlayerController)
	{
		PlayerController->PrepareManualCombat(true);
		bCombatMovementBlocked = PlayerController->IsJumpAirborne();
	}
	bCombatAttackHeld = bHeld;
	ReleasedAttackPower = RequestedAttackPower;
	bCombatAttackRequestPending = true;
	bCombatPowerCharging = !bCombatMovementBlocked && IsCombatPoseReady();
	CombatPowerBuildStartTime = FPlatformTime::Seconds();
	CombatPowerOrAccuracy = 0.f;
}

void UACEUIGameplayBinder::ReleaseCombatPowerCharge()
{
	if (!bCombatAttackHeld) return;
	if (bCombatMovementBlocked || !IsCombatPoseReady())
	{
		bCombatAttackHeld = bCombatAttackRequestPending = bCombatPowerCharging = false;
		return;
	}
	// ClientCombatSystem::EndAttackRequest: AdvancedCombatUI uses the actual
	// charge on release. Normal mode builds to at least the slider's mark.
	const float Charged = FMath::Clamp(float(FPlatformTime::Seconds() - CombatPowerBuildStartTime)
		/ GetCombatPowerChargeDuration(), 0.f, 1.f);
	ReleasedAttackPower = Client && Client->IsCharacterOptionSet(0x0C)
		? Charged : FMath::Max(RequestedAttackPower, Charged);
	bCombatAttackHeld = false;
}

void UACEUIGameplayBinder::CancelCombatAttack()
{
	const bool bWasRetargeting = bCombatRetargetPending;
	bCombatRetargetPending = false;
	bCombatPluginAttack = false;
	if (!bWasRetargeting && !bCombatAttackRequestPending && !bCombatRepeatActive && !bCombatAttackHeld && !bCombatRequestSent) return;
	bCombatRequestSent = false;
	bCombatAttackHeld = bCombatPointerHeld = bCombatKeyboardHeld = false;
	bCombatAttackRequestPending = bCombatRepeatActive = bCombatPowerCharging = false;
	CombatPowerOrAccuracy = 0.f;
	if (Client) if (const auto Session = Client->GetSession()) Session->SendCancelAttack();
}

void UACEUIGameplayBinder::SetCombatMovementBlocked(bool bBlocked)
{
	const bool bWasBlocked = bCombatMovementBlocked;
	bCombatMovementBlocked = bBlocked;
	if (bBlocked)
	{
		bCombatRetargetPending = false;
		// ClientCombatSystem::UseTime stops the build when not ready, but
		// retains attackRequestInProgress while the attack control is held.
		if (bCombatRequestSent || bCombatRepeatActive)
			if (Client) if (const auto Session = Client->GetSession()) Session->SendCancelAttack();
		bCombatRequestSent = bCombatRepeatActive = bCombatPowerCharging = false;
		bCombatAttackRequestPending = bCombatAttackHeld;
		CombatPowerOrAccuracy = 0.f;
		CombatPowerBuildStartTime = FPlatformTime::Seconds();
	}
	else if (bWasBlocked && bCombatAttackHeld && bCombatAttackRequestPending)
	{
		bCombatPowerCharging = !Client || !Client->GetSession() || !Client->GetSession()->IsServerAttackInProgress();
		CombatPowerBuildStartTime = FPlatformTime::Seconds();
		CombatPowerOrAccuracy = 0.f;
	}
}

bool UACEUIGameplayBinder::TryBeginCombatButton(FVector2D Point)
{
	if (!Manager || !Canvas || (PlayerController && PlayerController->IsVRActive())) return false;
	if (CombatMode != int32(ACECombatMode::Melee) && CombatMode != int32(ACECombatMode::Missile)) return false;
	const TCHAR* Names[] = {TEXT("LowAttack"), TEXT("MediumAttack"), TEXT("HighAttack")};
	const uint32 Heights[] = {ACEAttackHeight::Low, ACEAttackHeight::Medium, ACEAttackHeight::High};
	for (int32 I = 0; I < 3; ++I)
	{
		const auto Button = Manager->FindElementByName(Names[I]);
		if (Button && Canvas->IsElementExposedAt(Button, Point))
		{
			BeginCombatPowerCharge(Heights[I], true);
			bCombatPointerHeld = true;
			return true;
		}
	}
	return false;
}

bool UACEUIGameplayBinder::TryReleaseCombatButton()
{
	if (!bCombatPointerHeld) return false;
	bCombatPointerHeld = false;
	ReleaseCombatPowerCharge();
	return true;
}

void UACEUIGameplayBinder::TryAutoTargetOnCombatEnter(int32 ExcludeGuid)
{
    if (!Client || !Client->GetSession() || !PlayerController
        || (CombatMode != int32(ACECombatMode::Melee) && CombatMode != int32(ACECombatMode::Missile))) return;
    bCombatAutoTarget = Client->IsCharacterOptionSet(0x0D);
    if (!bCombatAutoTarget) return;
    FACEPosition PlayerPosition = Client->GetPlayerPosition();
    PlayerController->TryGetLocallyPredictedPosition(PlayerPosition);
    if (!PlayerPosition.IsValid()) return;
    const FVector Origin = PlayerPosition.ToUnrealLocation(1.f);
    const double RangeSquared = FMath::Square(ACERadarVisuals::RangeAc(uint32(PlayerPosition.CellId)));
    const auto Fellowship = Client->GetFellowship();
    auto Eligible = [&](int32 Guid)
    {
        FACEWorldObject Obj;
        return Guid && Guid != ExcludeGuid && Guid != Client->GetPlayerGuid()
            && Client->GetWorldObject(Guid,Obj) && Obj.IsSelectableWorldObject()
            && FVector::DistSquaredXY(Obj.Position.ToUnrealLocation(1.f),Origin) <= RangeSquared
            && Client->GetSession()->IsCombatTarget(Guid)
            && !Obj.IsVendor()
            && !Obj.PetOwnerId
            && !Fellowship.Members.ContainsByPredicate([&](const auto& M) { return M.Guid==Guid; });
    };
    if (Eligible(Client->GetSelectedObject().Guid)) return;
    // Retail AutoTarget only changes selection. End the previous attack before
    // broadcasting the new selection, otherwise HandleSelectionChanged treats
    // a kill as a deliberate target switch and queues another melee charge or
    // missile shot (including at enemies behind walls).
    CancelCombatAttack();
    const auto Session = Client->GetSession();
    const int32 Attacker = Session->GetLastAttackerGuid();
    // Retaliate on combat entry only. After a kill, choose the nearest eligible
    // creature in radar range, even when a farther creature attacked recently.
    if (!ExcludeGuid && Eligible(Attacker) && FPlatformTime::Seconds()-Session->GetLastAttackerTimeSeconds()<15.)
        Client->SelectObject(Attacker);
    else if (const int32 Nearest = PlayerController->FindNearbyTarget(true,0,ExcludeGuid))
        Client->SelectObject(Nearest);
}
bool UACEUIGameplayBinder::IsCombatPoseReady() const
{
	if (Client && Client->IsInPortalSpace()) return false;
	if (CombatMode != int32(ACECombatMode::Missile)) return true;
	const auto Session = Client ? Client->GetSession() : nullptr;
	if (!Session) return false;
	// ClientCombatSystem::PlayerInReadyPosition(true) requires an interpreted
	// Ready substate in a missile style. Reload/Aim cannot build a new shot.
	const uint32 Style = Session->GetCurrentStance();
	return Session->GetCurrentForwardCommand() == ACEMotion::Ready
		&& (Style == ACEMotion::StanceBowCombat || Style == ACEMotion::StanceCrossbow
			|| Style == ACEMotion::StanceSling || Style == ACEMotion::StanceThrownWeapon
			|| Style == ACEMotion::StanceAtlatl || Style == ACEMotion::StanceThrownShield);
}

void UACEUIGameplayBinder::FireCombatAttack()
{
	if (PlayerController && PlayerController->IsVRActive()) return;
	if (!Client || !IsCombatPoseReady())
	{
		return;
	}
	int32 TargetGuid = 0;
	if (LastSelection.bValid && LastSelection.Guid != 0
		&& LastSelection.Guid != Client->GetPlayerGuid())
	{
		TargetGuid = LastSelection.Guid;
	}
	else
	{
		const FACESelectedObject Sel = Client->GetSelectedObject();
		if (Sel.bValid && Sel.Guid != 0 && Sel.Guid != Client->GetPlayerGuid())
		{
			TargetGuid = Sel.Guid;
		}
	}
	if (TargetGuid == 0)
	{
		return;
	}

	FACEWorldObject Target;
	if (!Client->GetSession() || !Client->GetSession()->IsCombatTarget(TargetGuid)
		|| !Client->GetWorldObject(TargetGuid, Target))
	{
		return;
	}

	if (CombatMode == static_cast<int32>(ACECombatMode::Missile))
	{
		uint32 Stance = ACEMotion::StanceNonCombat;
		if (Client->GetSession())
		{
			Stance = Client->GetSession()->GetCurrentStance();
		}
		const bool bMissileStance =
			Stance == ACEMotion::StanceBowCombat
			|| Stance == ACEMotion::StanceCrossbow
			|| Stance == ACEMotion::StanceSling
			|| Stance == ACEMotion::StanceThrownWeapon
			|| Stance == ACEMotion::StanceAtlatl
			|| Stance == ACEMotion::StanceThrownShield;
		if (!bMissileStance)
		{
			return;
		}
		Client->SendTargetedMissileAttack(TargetGuid, static_cast<int32>(CombatAttackHeight), CombatPowerOrAccuracy);
	}
	else if (CombatMode == static_cast<int32>(ACECombatMode::Melee))
	{
		Client->SendTargetedMeleeAttack(TargetGuid, static_cast<int32>(CombatAttackHeight), CombatPowerOrAccuracy);
	}
	LastCombatAttackTarget = TargetGuid;
	bCombatRequestSent = true;
	LastCombatAttackPower = CombatPowerOrAccuracy;
	bCombatRepeatActive = bCombatAutoRepeat;
}

bool UACEUIGameplayBinder::TryBeginCombatPowerDrag(FVector2D ViewportPos)
{
	if (!Manager || (CombatMode != static_cast<int32>(ACECombatMode::Melee)
		&& CombatMode != static_cast<int32>(ACECombatMode::Missile)))
	{
		return false;
	}
	TSharedPtr<FACEUIElement> Slider = Manager->FindElementByName(TEXT("PowerSlider"));
	TSharedPtr<FACEUIElement> CombatRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"));
	if (!Slider.IsValid() || !CombatRoot.IsValid() || !CombatRoot->bVisible
		|| !Canvas || !Canvas->IsElementExposedAt(Slider, ViewportPos))
	{
		return false;
	}
	const FVector2D CanvasLocalPos = Canvas->ViewportToLayout(ViewportPos);
	const FIntPoint O = Slider->GetScreenOrigin();
	if (CanvasLocalPos.X < O.X || CanvasLocalPos.Y < O.Y
		|| CanvasLocalPos.X >= O.X + Slider->Width || CanvasLocalPos.Y >= O.Y + Slider->Height)
	{
		return false;
	}
	// The authored slider is in layout units, while pointer events are in viewport
	// units. Preserve a thumb grab, or center it at the clicked track position.
	float ThumbWidth = 0.f;
	for (const auto& Child : Slider->Children)
		if (Child && Child->ElementId == 1) ThumbWidth = Child->Width;
	const float ThumbX = RequestedAttackPower * FMath::Max(0.f, Slider->Width - ThumbWidth);
	const float LocalX = CanvasLocalPos.X - O.X;
	CombatPowerDragGrabOffset = LocalX >= ThumbX && LocalX < ThumbX + ThumbWidth
		? LocalX - ThumbX : ThumbWidth * .5f;
	bCombatPowerDrag = true;
	UpdateCombatPowerDrag(ViewportPos);
	return true;
}

void UACEUIGameplayBinder::UpdateCombatPowerDrag(FVector2D ViewportPos)
{
	if (!bCombatPowerDrag || !Manager)
	{
		return;
	}
	TSharedPtr<FACEUIElement> Slider = Manager->FindElementByName(TEXT("PowerSlider"));
	if (!Slider.IsValid())
	{
		return;
	}
	const FIntPoint O = Slider->GetScreenOrigin();
	const FVector2D CanvasLocalPos = Canvas ? Canvas->ViewportToLayout(ViewportPos) : ViewportPos;
	float ThumbWidth = 0.f;
	for (const auto& Child : Slider->Children)
		if (Child && Child->ElementId == 1) ThumbWidth = Child->Width;
	const float TrackW = FMath::Max(1.f, Slider->Width - ThumbWidth);
	RequestedAttackPower = FMath::Clamp((CanvasLocalPos.X - O.X - CombatPowerDragGrabOffset) / TrackW, 0.f, 1.f);
}

bool UACEUIGameplayBinder::TryFinishCombatPowerDrag(FVector2D CanvasLocalPos)
{
	if (!bCombatPowerDrag)
	{
		return false;
	}
	UpdateCombatPowerDrag(CanvasLocalPos);
	bCombatPowerDrag = false;
	return true;
}

bool UACEUIGameplayBinder::HitTestSpellBarSlot(FVector2D Absolute, int32& OutSlotIndex) const
{
	OutSlotIndex = INDEX_NONE;
	auto Test = [&](const TArray<TObjectPtr<UBorder>>& Borders) -> bool
	{
		for (int32 i = 0; i < Borders.Num() && i < SpellBarSpellIds.Num(); ++i)
		{
			UBorder* Border = Borders[i];
			if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
			{
				continue;
			}
			if (Canvas->IsWidgetExposedAt(Border, Absolute))
			{
				OutSlotIndex = i;
				return true;
			}
		}
		return false;
	};
	// Icons for filled spells; empty-cell backgrounds for append / insert targets.
	return Test(SpellBarIcons) || Test(SpellBarSlotBgs);
}

bool UACEUIGameplayBinder::HitTestSpellbookRow(FVector2D Absolute, int32& OutSpellId) const
{
	OutSpellId = 0;
	for (int32 i = 0; i < SpellbookIcons.Num() && i < SpellbookRowIds.Num(); ++i)
	{
		UBorder* Background = SpellbookRowBackgrounds.IsValidIndex(i) ? SpellbookRowBackgrounds[i] : nullptr;
		if (Background && Background->GetVisibility() != ESlateVisibility::Collapsed && Canvas->IsWidgetExposedAt(Background, Absolute))
		{ OutSpellId = SpellbookRowIds[i]; return OutSpellId != 0; }
		UBorder* Icon = SpellbookIcons[i];
		if (Icon && Icon->GetVisibility() != ESlateVisibility::Collapsed
			&& Canvas->IsWidgetExposedAt(Icon, Absolute))
		{
			OutSpellId = SpellbookRowIds[i];
			return OutSpellId != 0;
		}
		UTextBlock* Row = SpellbookRows.IsValidIndex(i) ? SpellbookRows[i] : nullptr;
		if (Row && Row->GetVisibility() != ESlateVisibility::Collapsed
			&& Canvas->IsWidgetExposedAt(Row, Absolute))
		{
			OutSpellId = SpellbookRowIds[i];
			return OutSpellId != 0;
		}
	}
	return false;
}

bool UACEUIGameplayBinder::TryBeginSpellDrag(FVector2D CanvasLocalPos)
{
	if (!Client || !Canvas || !Canvas->GetElementLayer())
	{
		return false;
	}
	// Retail ExecuteTargetModeForSpell examines on a target-mode click instead
	// of selecting, dragging or casting that spell.
	if (PlayerController && PlayerController->GetObjectCursorTargeting()==2 && InspectSpellAt(CanvasLocalPos))
	{
		PlayerController->SetObjectCursorTargeting(0);
		return true;
	}
	const FVector2D Absolute = Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos);

	int32 SpellId = 0;
	int32 SourceSlot = INDEX_NONE;
	if (HitTestSpellbookRow(Absolute, SpellId) && SpellId != 0)
	{
		SelectedSpellbookId = SpellId;
		RefreshSpellbookOverlays();
		SourceSlot = INDEX_NONE;
	}
	else if (HitTestSpellBarSlot(Absolute, SourceSlot)
		&& SpellBarSpellIds.IsValidIndex(SourceSlot) && SpellBarSpellIds[SourceSlot] != 0)
	{
		SpellId = SpellBarSpellIds[SourceSlot];
	}
	else
	{
		return false;
	}

	// Retail: second press within the double-click window casts immediately (same as inventory).
	// Detect on MouseDown — spell drag otherwise swallows the click before TryHandleOverlayClick.
	if (SourceSlot != INDEX_NONE && SpellId != 0)
	{
		LastSpellbookClickId = 0;
		SelectCombatSpellSlot(SourceSlot + SpellHotbarScrollOffset);
		const double Now = FPlatformTime::Seconds();
		constexpr double DoubleClickSeconds = 0.75;
		if (SourceSlot == LastSpellClickSlot && SpellId == LastSpellClickId
			&& (Now - LastSpellClickTime) < DoubleClickSeconds)
		{
			if (!Client->SendCastSpell(SpellId))
			{
				PostInventorySystemMessage(Client->IsInPortalSpace() ? TEXT("You're too busy!") : TEXT("You must select a target for that spell."));
			}
			LastSpellClickSlot = INDEX_NONE;
			LastSpellClickId = 0;
			LastSpellClickTime = 0.0;
			RefreshSpellHotbarOverlays();
			return true;
		}
		LastSpellClickSlot = SourceSlot;
		LastSpellClickId = SpellId;
		LastSpellClickTime = Now;
		RefreshSpellHotbarOverlays();
	}

	FString SpellName;
	uint32 IconDid = 0;
	if (PlayerController)
	{
		if (UGameInstance* GI = PlayerController->GetGameInstance())
		{
			if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			{
				Dat->TryGetSpellInfo(static_cast<uint32>(SpellId), SpellName, IconDid);
			}
		}
	}
	SpellDragId = SpellId;
	SpellDragIconDid = static_cast<int32>(IconDid);
	SpellDragSourceBarSlot = SourceSlot;
	SpellDragSourceBar = Client->GetActiveSpellBar();
	SpellDragStartLocal = CanvasLocalPos;
	bSpellDragPending = true;
	bSpellDragActive = false;
	return true;
}

void UACEUIGameplayBinder::SelectVRSpell(int32 Spell)
{
	if (!Client) return;
	const int32 Slot = Client->GetSpellBar(Client->GetActiveSpellBar()).Find(Spell);
	if (Slot == INDEX_NONE) return;
	SelectCombatSpellSlot(Slot);
	RefreshSpellHotbarOverlays();
}

void UACEUIGameplayBinder::CancelPointerGestures()
{
	if (bCombatPointerHeld) CancelCombatAttack();
	CancelSpellDrag(); CancelInventoryDrag();
	bCombatPowerDrag = false;
	TryFinishScrollbarDrag();
	if (Manager) Manager->CancelPointerCapture();
	if (Canvas) Canvas->ResetPointerOwnership();
}

void UACEUIGameplayBinder::UpdateSpellDrag(FVector2D CanvasLocalPos)
{
	if (!bSpellDragPending || SpellDragId == 0 || !Canvas || !Canvas->WidgetTree)
	{
		return;
	}
	if (!bSpellDragActive)
	{
		if (FVector2D::Distance(CanvasLocalPos, SpellDragStartLocal) < 12.f)
		{
			return;
		}
		bSpellDragActive = true;
		LastSpellbookClickId = 0;
		LastSpellClickId = 0;
		LastSpellClickSlot = INDEX_NONE;
		// gmSpellcastingUI::RecvNotice_ItemListBeginDrag removes the favorite
		// immediately. The remaining spells slide left; dropping inserts anew.
		if (SpellDragSourceBarSlot != INDEX_NONE)
		{
			Client->SendRemoveSpellFromBar(SpellDragId, SpellDragSourceBar);
			RefreshSpellHotbarOverlays();
		}
	}
	// Retail keeps tab targets live during an ItemList drag. Hovering a tab
	// opens it without releasing the lifted spell, then drop inserts there.
	for (int32 Tab = 1; Tab <= 8; ++Tab)
		if (auto El = Manager->FindElementByName(FString::Printf(TEXT("Spellcast_Tab%d"), Tab));
			El && Canvas->IsElementExposedAt(El, CanvasLocalPos) && Client->GetActiveSpellBar() != Tab - 1)
		{
			SetCombatSpellBar(Tab - 1); SyncSpellcastTabChrome(); RefreshSpellHotbarOverlays();
			break;
		}
	if (!SpellDragIcon)
	{
		SpellDragIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		SpellDragIcon->SetPadding(FMargin(0.f));
		SpellDragIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	SetSpellIcon(SpellDragIcon, SpellDragId);
	SpellDragIcon->SetBrushColor(FLinearColor(1.f, 1.f, 1.f, 0.85f));
	SpellDragIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
    Canvas->ShowDragIcon(SpellDragIcon,CanvasLocalPos);
	if (!SpellDropMarker)
	{
		SpellDropMarker = Canvas->WidgetTree->ConstructWidget<UBorder>();
		SpellDropMarker->SetPadding(FMargin(0));
		Canvas->GetElementLayer()->AddChild(SpellDropMarker);
		SetIconDid(SpellDropMarker, 0x060011F9); // ItemSlot_DragOver_Accept
	}
	SpellDropMarker->SetVisibility(ESlateVisibility::Collapsed);
	int32 HoverSlot = INDEX_NONE;
	const FVector2D Absolute = Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos);
	if (HitTestSpellBarSlot(Absolute, HoverSlot))
	{
		const auto Bar = Client->GetSpellBar(Client->GetActiveSpellBar());
		const int32 Empty = Bar.Find(0);
		const int32 Count = Empty == INDEX_NONE ? Bar.Num() : Empty;
		const int32 InsertSlot = FMath::Min(HoverSlot + SpellHotbarScrollOffset, Count) - SpellHotbarScrollOffset;
		if (SpellBarSlotBgs.IsValidIndex(InsertSlot))
		{
			const auto* TargetSlot = Cast<UCanvasPanelSlot>(SpellBarSlotBgs[InsertSlot]->Slot);
			if (auto* Slot = Cast<UCanvasPanelSlot>(SpellDropMarker->Slot); Slot && TargetSlot)
			{
				Slot->SetPosition(TargetSlot->GetPosition()); Slot->SetSize(TargetSlot->GetSize());
				Canvas->SetOverlayOrder(SpellDropMarker, Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field")), 590);
				SpellDropMarker->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
		}
	}
}

bool UACEUIGameplayBinder::TryFinishSpellDrag(FVector2D CanvasLocalPos)
{
	if (!bSpellDragPending || SpellDragId == 0)
	{
		return false;
	}
	const int32 SpellId = SpellDragId;
	const int32 SourceSlot = SpellDragSourceBarSlot;
    const int32 SourceBar = SpellDragSourceBar;
	const bool bWasDragging = bSpellDragActive;
	CancelSpellDrag();
	if (!Client || !bWasDragging)
	{
		if (!Client) return true;
		if (SourceSlot == INDEX_NONE)
		{
			// gmSpellbookUI -> RecvNotice_AddSpellShortcut -> AddFavorite(-1, false):
			// append to the open tab, select/reveal the new shortcut, and leave
			// duplicates where they are. A spellbook click is never a cast in VR.
			const double Now = FPlatformTime::Seconds();
			const int32 BarIndex = Client->GetActiveSpellBar();
			if (LastSpellbookClickId == SpellId && LastSpellbookClickBar == BarIndex
				&& Now - LastSpellbookClickTime < 0.75
				&& FVector2D::Distance(CanvasLocalPos, LastSpellbookClickPosition) < 12.f)
			{
				LastSpellbookClickId = 0;
				const TArray<int32> Bar = Client->GetSpellBar(BarIndex);
				if (!Bar.Contains(SpellId))
				{
					const int32 Empty = Bar.Find(0);
					const int32 AppendAt = Empty == INDEX_NONE ? Bar.Num() : Empty;
					Client->SendAddSpellToBar(SpellId, AppendAt, BarIndex);
					SelectCombatSpellSlot(AppendAt);
					RefreshSpellHotbarOverlays();
				}
			}
			else
			{
				LastSpellbookClickId = SpellId;
				LastSpellbookClickBar = BarIndex;
				LastSpellbookClickTime = Now;
				LastSpellbookClickPosition = CanvasLocalPos;
			}
		}
		else if (PlayerController && PlayerController->IsVRActive()) Client->SendCastSpell(SpellId);
		return true;
	}
	const FVector2D Absolute = Canvas
		? Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos)
		: CanvasLocalPos;

    if(auto* GI=Client->GetGameInstance())
        if(auto* Deck=GI->GetSubsystem<UACEActionDeckSubsystem>();Deck&&Deck->DropAt(Absolute,0,SpellId))
        {
            // Native dragging lifts the favorite at drag start. The deck is a
            // copy destination: restore its original slot.
            if(SourceSlot!=INDEX_NONE)Client->SendAddSpellToBar(SpellId,SourceSlot,SourceBar);
            RefreshSpellHotbarOverlays();return true;
        }

	int32 DestSlot = INDEX_NONE;
	for (int32 Tab = 1; Tab <= 8; ++Tab)
		if (auto El = Manager->FindElementByName(FString::Printf(TEXT("Spellcast_Tab%d"), Tab));
			El && Canvas->IsElementExposedAt(El, CanvasLocalPos))
		{
			SetCombatSpellBar(Tab - 1);
			const auto Bar = Client->GetSpellBar(Tab - 1);
			const int32 Empty = Bar.Find(0);
			Client->SendAddSpellToBar(SpellId, Empty == INDEX_NONE ? Bar.Num() : Empty, Tab - 1);
			SyncSpellcastTabChrome(); RefreshSpellHotbarOverlays();
			return true;
		}
	if (HitTestSpellBarSlot(Absolute, DestSlot))
	{
		const int32 BarIndex = Client->GetActiveSpellBar();
		const TArray<int32> Bar = Client->GetSpellBar(BarIndex);
		int32 Filled = 0;
		for (int32 i = 0; i < Bar.Num(); ++i)
		{
			if (Bar[i] == 0) { break; }
			++Filled;
		}
		const int32 AbsDest = DestSlot + SpellHotbarScrollOffset;
		// Empty / past-end → first available (append). Occupied → insert + shift right.
		int32 InsertAt = AbsDest;
		if (InsertAt >= Filled || (Bar.IsValidIndex(InsertAt) && Bar[InsertAt] == 0))
		{
			InsertAt = Filled;
		}
        const int32 SelectedId=Bar.IsValidIndex(SelectedCombatSpellSlot) ? Bar[SelectedCombatSpellSlot] : 0;
		Client->SendAddSpellToBar(SpellId, InsertAt, BarIndex);
        if (SelectedId) SelectedCombatSpellSlot=Client->GetSpellBar(BarIndex).Find(SelectedId);
		RefreshSpellHotbarOverlays();
		return true;
	}

	// A favorite was removed at drag start, matching retail's off-bar drop.
	return true;
}

void UACEUIGameplayBinder::SyncCombatAmmoCount()
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	const auto Button = Manager ? Manager->FindElementByName(TEXT("MissileModeButton")) : nullptr;
	if (!Button || !Canvas || !Canvas->WidgetTree) return;
	// gmToolbarUI::UpdateAmmoID: stacked thrown weapons precede readied ammo.
	const auto* Ammo = Session ? Session->FindEquippedItem(ACEEquipMask::MissileWeapon) : nullptr;
	if (!Ammo || Ammo->MaxStackSize <= 1)
		Ammo = Session ? Session->FindEquippedItem(ACEEquipMask::MissileAmmo) : nullptr;
	if (!CombatAmmoLabel) CombatAmmoLabel = Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>();
	CombatAmmoLabel->SetVisibility(Button->bVisible && Ammo ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (Button->bVisible && Ammo)
		PlaceTextOnElement(CombatAmmoLabel, Button, FString::FromInt(FMath::Max(1, Ammo->StackSize)), 8, FLinearColor::White, 520);
}

void UACEUIGameplayBinder::SyncCombatModeButtons()
{
	if (!Manager)
	{
		return;
	}
	SyncCombatAmmoCount();
	// Retail stacks four mode buttons at the same rect; show the active mode art.
	const int32 Equipped = ResolveEquippedCombatMode();
	const bool bPeace = CombatMode == static_cast<int32>(ACECombatMode::NonCombat);
	Manager->SetElementVisibleByName(TEXT("PeaceModeButton"), bPeace);
	Manager->SetElementVisibleByName(TEXT("MeleeModeButton"),
		!bPeace && CombatMode == static_cast<int32>(ACECombatMode::Melee));
	Manager->SetElementVisibleByName(TEXT("MissileModeButton"),
		!bPeace && CombatMode == static_cast<int32>(ACECombatMode::Missile));
	Manager->SetElementVisibleByName(TEXT("MagicModeButton"),
		!bPeace && CombatMode == static_cast<int32>(ACECombatMode::Magic));
	// When in peace, still leave Melee/Missile/Magic hittable via Peace button cycling —
	// clicking PeaceMode when already peace enters equipped mode (retail peace↔combat).
	if (bPeace)
	{
		// Expose only PeaceModeButton visually; click handler on Peace enters Equipped.
		(void)Equipped;
	}
}

int32 UACEUIGameplayBinder::ResolveEquippedCombatMode() const
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	return Session ? static_cast<int32>(ACECombatStance::ResolveEquippedMode(*Session))
		: static_cast<int32>(ACECombatMode::Melee);
}

bool UACEUIGameplayBinder::HasEquippedCaster() const
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	return Session && Session->FindEquippedItem(ACEEquipMask::Held);
}

bool UACEUIGameplayBinder::HasEquippedMissileWeapon() const
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	return Session && (Session->FindEquippedItem(ACEEquipMask::MissileWeapon)
		|| Session->FindEquippedItem(ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded, ACEItemType::MissileWeapon));
}

void UACEUIGameplayBinder::UseSelectedObject()
{
	if (!Client || !LastSelection.bValid || LastSelection.Guid == 0)
	{
		if (Client&&PlayerController) { CancelPendingUseWith();PlayerController->SetObjectCursorTargeting(1); }
		return;
	}
	FACEWorldObject Owned;
	if(Client->GetWorldObject(LastSelection.Guid,Owned) && Client->IsOwnedInventoryItem(Owned))
	{UseInventoryItem(Owned.Guid);return;}

	if (PlayerController)
	{
		PlayerController->InteractWithObject(LastSelection.Guid);
	}
	else
	{
		FACEWorldObject Loot;
		if (Client->GetWorldObject(LastSelection.Guid, Loot) && ACEInventoryRules::IsWorldPickupOnUse(Loot))
		{
			PickupInventoryAmount(LastSelection.Guid);
		}
		else
		{
			Client->SendUseItem(LastSelection.Guid);
		}
	}
}

void UACEUIGameplayBinder::ExamineSelectedObject()
{
	if (!Client || !LastSelection.bValid || LastSelection.Guid == 0)
	{
		if (Client&&PlayerController) { CancelPendingUseWith();PlayerController->SetObjectCursorTargeting(2); }
		return;
	}
	// Retail toggles an already displayed appraisal of this selection. A new
	// selection still requests fresh server qualities, including custom items.
	if (Manager && LastAppraisal.ObjectGuid == LastSelection.Guid)
	{
		const auto Window = Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
		if (Window && Window->bVisible) { ShowExamination(false); return; }
	}
	Client->SendIdentifyObject(LastSelection.Guid);
}

void UACEUIGameplayBinder::UseShortcutSlot(int32 SlotIndex1Based)
{
	if (!Client || SlotIndex1Based < 1)
	{
		return;
	}
	const int32 Guid = Client->GetShortcutObject(SlotIndex1Based - 1);
	if (Guid != 0)
	{
		// Same as inventory double-click: wield weapons/armor, Use gems/food/etc.
		UseInventoryItem(Guid);
	}
}

bool UACEUIGameplayBinder::ShouldPreserveShortcutSelection(int32 Guid) const
{
	if (PendingUseWithSourceGuid || (PlayerController && PlayerController->GetObjectCursorTargeting())) return true;
	if (!Client || CombatMode == int32(ACECombatMode::NonCombat)) return false;
	const auto Session = Client->GetSession();
	if (!Session) return false;
	const auto* Target = Session->GetWorldObjects().Find(Client->GetSelectedObject().Guid);
	const auto* Item = Session->GetWorldObjects().Find(Guid);
	// The first click of a mouse/controller double-click must also keep the
	// monster selected while preparing a targeted consumable during combat.
	// Ordinary selection and right-click inspection retain their normal behavior.
	return Target && Target->Guid != Client->GetPlayerGuid() && Target->IsAttackable()
		&& Item && ACEItemUseable::IsTargeted(Item->ItemUseable);
}

void UACEUIGameplayBinder::ActivateHotbarSlot(int32 SlotIndex)
{
	if (!Client || SlotIndex < 0 || SlotIndex >= 12)
	{
		return;
	}
	const bool bMagic = CombatMode == static_cast<int32>(ACECombatMode::Magic);
	TSharedPtr<FACEUIElement> CombatRoot = Manager
		? Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"))
		: nullptr;
	const bool bSpellBarOpen = bMagic && CombatRoot.IsValid() && CombatRoot->bVisible;
	if (bSpellBarOpen)
	{
		// Scrolling changes the view, never the twelve spell key bindings.
		const TArray<int32> Bar = Client->GetSpellBar(Client->GetActiveSpellBar());
		const int32 SpellId = Bar.IsValidIndex(SlotIndex) ? Bar[SlotIndex] : 0;
		if (SpellId != 0)
		{
			SelectCombatSpellSlot(SlotIndex);
			RefreshSpellHotbarOverlays();
			if (!Client->SendCastSpell(SpellId))
			{
				PostInventorySystemMessage(Client->IsInPortalSpace() ? TEXT("You're too busy!") : TEXT("You must select a target for that spell."));
			}
		}
		return;
	}
	if (SlotIndex < 10) UseShortcutSlot(SlotIndex + 1);
}

void UACEUIGameplayBinder::SelectInventoryGuid(int32 Guid)
{
	if (!Client || Guid == 0)
	{
		return;
	}
	Client->SelectInventoryObject(Guid);
}

void UACEUIGameplayBinder::PostInventorySystemMessage(const FString& Text)
{
	// Retail shows these ("You're too busy!" etc.) as the transient center-top banner.
	HandleChatMessage(Text, FString(), ACEChatMessageType::TransientInfo);
}

void UACEUIGameplayBinder::ShowTransientInfo(const FString& Message)
{
	const FString Notice = Message.TrimEnd(); // gmSpewBoxUI trims trailing whitespace.
	if (!Canvas || !Canvas->WidgetTree || Notice.IsEmpty())
	{
		return;
	}
	if (!TransientInfoBox)
	{
		TransientInfoBox = Canvas->WidgetTree->ConstructWidget<USizeBox>();
		TransientInfoBox->SetVisibility(ESlateVisibility::HitTestInvisible);
		auto* Rows = Canvas->WidgetTree->ConstructWidget<UVerticalBox>();
		TransientInfoBox->AddChild(Rows);
		TransientInfoTemplate = UACEUILayoutResolver::LoadTemplate(0x21000011u, 0x1000004Au);
		if (TransientInfoTemplate)
		{
			// Use the notice palette, not the template's generic "changed" red state.
			TransientInfoTemplate->States.Reset();
			TransientInfoTemplate->bTextOutline = true;
		}
		// classic_spewbox permits four concurrent messages. Reuse fixed rows so
		// repeated server failures cannot grow either the widget tree or a queue.
		for (int32 I = 0; I < 4; ++I)
		{
			auto* Text = Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>();
			Text->SetJustification(ETextJustify::Center);
			Text->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12));
			Text->SetColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.9f, 0.2f)));
			Text->SetShadowColorAndOpacity(FLinearColor::Black);
			Text->SetShadowOffset(FVector2D(1.f, 1.f));
			Text->SetVisibility(ESlateVisibility::Collapsed);
			Rows->AddChildToVerticalBox(Text);
			TransientInfoLines.Add(Text);
			TransientInfoTimes.Add(0.0);
		}
		TransientInfoText = TransientInfoLines[0];
	}
	// Retail inserts newest first and refreshes an immediately repeated message.
	if (TransientInfoText->GetText().ToString() != Notice)
	{
		for (int32 I = TransientInfoLines.Num() - 1; I > 0; --I)
		{
			TransientInfoLines[I]->SetText(TransientInfoLines[I - 1]->GetText());
			TransientInfoTimes[I] = TransientInfoTimes[I - 1];
		}
	}
	TransientInfoText->SetText(FText::FromString(Notice));
	TransientInfoTimes[0] = FPlatformTime::Seconds();

	if (UCanvasPanel* Layer = Canvas->GetElementLayer())
	{
		if (TransientInfoBox->GetParent() != Layer)
		{
			Layer->AddChild(TransientInfoBox);
		}
		if (UCanvasPanelSlot* InfoSlot = Cast<UCanvasPanelSlot>(TransientInfoBox->Slot))
		{
			InfoSlot->SetAnchors(FAnchors(0.5f, 0.f, 0.5f, 0.f));
			InfoSlot->SetAlignment(FVector2D(0.5f, 0.f));
			InfoSlot->SetAutoSize(true);
			Canvas->SetOverlayOrder(TransientInfoBox, Manager ? Manager->FindElementByName(TEXT("RootGameplay_SmartBox_Field")) : nullptr, 15000);
		}
	}
	TickTransientInfo();
}

void UACEUIGameplayBinder::TickTransientInfo()
{
	if (!TransientInfoBox || !Canvas)
	{
		return;
	}
	const FVector2D Scale = Canvas->GetLastScale2D();
	if (Scale != TransientInfoScale)
	{
		TransientInfoScale = Scale;
		// Native spewbox width; bitmap text wraps before leaving the game view.
		TransientInfoBox->SetWidthOverride(450.f * Scale.X);
		for (const auto& Text : TransientInfoLines)
		{
			Text->SetWrapTextAt(450.f * Scale.X);
			CastChecked<UACERetailTextBlock>(Text.Get())->SetRetailElement(
				Canvas->GetResourceResolver(), TransientInfoTemplate, Scale, 450.f, false);
		}
	}
	// Retail places the spewbox 20 pixels below the game view's top. Our movable
	// vitals may occupy that space: keep the notices below overlapping meters.
	float Top = 20.f * Scale.Y;
	const float Center = Canvas->GetCachedGeometry().GetLocalSize().X * .5f;
	if (Manager)
	{
		for (const TCHAR* Name : {TEXT("RootGameplay_FloatyVitals_Field"), TEXT("RootGameplay_FloatySideVitals_Field")})
		{
			const auto Vitals = Manager->FindElementByName(Name);
			if (!Vitals || !Vitals->bVisible) continue;
			const FVector2D Origin = Canvas->LayoutToViewport(FVector2D(Vitals->GetScreenOrigin()));
			const FVector2D Size = FVector2D(Vitals->Width, Vitals->GetLayoutHeight()) * Scale;
			if (Origin.X < Center + 225.f * Scale.X && Origin.X + Size.X > Center - 225.f * Scale.X
				&& Origin.Y < Top + 72.f * Scale.Y && Origin.Y + Size.Y > Top)
				Top = Origin.Y + Size.Y + 6.f * Scale.Y;
		}
	}
	if (auto* Slot = Cast<UCanvasPanelSlot>(TransientInfoBox->Slot))
		if (!Slot->GetPosition().Equals(FVector2D(0.f, Top))) Slot->SetPosition(FVector2D(0.f, Top));
	constexpr double HoldSeconds = 4.0;
	constexpr double FadeSeconds = 1.0;
	const double Now = FPlatformTime::Seconds();
	for (int32 I = 0; I < TransientInfoLines.Num(); ++I)
	{
		auto* Text = TransientInfoLines[I].Get();
		const double Elapsed = Now - TransientInfoTimes[I];
		const bool bVisible = TransientInfoTimes[I] > 0 && Elapsed < HoldSeconds + FadeSeconds;
		Text->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bVisible) Text->SetRenderOpacity(FMath::Clamp(1.f - static_cast<float>((Elapsed - HoldSeconds) / FadeSeconds), 0.f, 1.f));
	}
}

void UACEUIGameplayBinder::FitCombatFloatyContentWidth(int32 ContentW)
{
	if (!Manager || ContentW <= 0)
	{
		return;
	}
	TSharedPtr<FACEUIElement> CombatRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"));
	TSharedPtr<FACEUIElement> CombatPanel = Manager->FindElementUnder(
		TEXT("RootGameplay_FloatyCombatPanel_Field"), TEXT("CombatPanel"));
	if (!CombatRoot.IsValid())
	{
		return;
	}
	constexpr int32 Chrome = 10; // left+right borders
	CombatRoot->Width = ContentW + Chrome;
	if (CombatPanel.IsValid())
	{
		CombatPanel->Width = ContentW;
		// Authored mid/right backgrounds target 800 — resize so the fill matches the gold frame.
		if (TSharedPtr<FACEUIElement> Left = Manager->FindElementUnder(TEXT("CombatPanel"), TEXT("background_left")))
		{
			Left->Width = 10;
			Left->X = 0;
		}
		constexpr int32 RightW = 100;
		if (TSharedPtr<FACEUIElement> Mid = Manager->FindElementUnder(TEXT("CombatPanel"), TEXT("background_mid")))
		{
			Mid->X = 10;
			Mid->Width = FMath::Max(1, ContentW - 10 - RightW);
		}
		if (TSharedPtr<FACEUIElement> Right = Manager->FindElementUnder(TEXT("CombatPanel"), TEXT("background_right")))
		{
			Right->X = FMath::Max(10, ContentW - RightW);
			Right->Width = RightW;
		}
	}
	for (const TCHAR* Border : {
		TEXT("CombatPanelTopBorder"), TEXT("CombatPanelBottomBorder"),
		TEXT("CombatPanelTopBorder_Locked"), TEXT("CombatPanelBottomBorder_Locked") })
	{
		if (TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(
			TEXT("RootGameplay_FloatyCombatPanel_Field"), Border))
		{
			El->Width = ContentW;
		}
	}
	for (const TCHAR* Corner : {
		TEXT("CombatPanelTopRightCorner"), TEXT("CombatPanelBottomRightCorner"),
		TEXT("CombatPanelRightBorder"), TEXT("CombatPanelTopRightCorner_Locked"),
		TEXT("CombatPanelBottomRightCorner_Locked"), TEXT("CombatPanelRightBorder_Locked") })
	{
		if (TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(
			TEXT("RootGameplay_FloatyCombatPanel_Field"), Corner))
		{
			El->X = ContentW + 5;
		}
	}
	if (TSharedPtr<FACEUIElement> Spell = Manager->FindElementByName(TEXT("Spellcasting")))
	{
		Spell->Width = ContentW;
	}
	if (TSharedPtr<FACEUIElement> Bg = Manager->FindElementUnder(TEXT("Spellcasting"), TEXT("SpellcastSlot_Background")))
	{
		Bg->Width = ContentW;
	}
	if (TSharedPtr<FACEUIElement> TabBg = Manager->FindElementUnder(TEXT("Spellcasting"), TEXT("SpellcastSlot_TabBackground")))
	{
		TabBg->Width = ContentW;
	}
	if (TSharedPtr<FACEUIElement> Panel = Manager->FindElementUnder(TEXT("Spellcasting"), TEXT("SpellcastPanel")))
	{
		Panel->Width = ContentW;
	}
	// Keep Cast inside the framed content (caller may place Cast explicitly before this).
	if (TSharedPtr<FACEUIElement> Cast = Manager->FindElementByName(TEXT("CastSpellButton")))
	{
		constexpr int32 CastW = 75;
		Cast->Width = CastW;
		// Only snap to the right edge when still on the authored 800-wide Cast x.
		if (Cast->X <= 0 || Cast->X >= ContentW - CastW)
		{
			Cast->X = FMath::Max(0, ContentW - CastW - 6);
		}
	}
}

void UACEUIGameplayBinder::ExpandCombatFloatyForWideContent()
{
	FitCombatFloatyContentWidth(800);
}

void UACEUIGameplayBinder::ExpandEnvFloatyForWideContent()
{
	// The resolved retail layout is 610x120 with 600x110 content. The former
	// 800-pixel override stretched the lists but left their nested bevels at 600.
	// Keep the authored geometry; normal edge anchoring handles user resizing.
	if (!Manager) return;
	if (auto Root = Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field")))
	{
		UACEUIElementManager::ApplyFloatyResizeLayout(Root);
	}
}
int64 UACEUIGameplayBinder::ResolveWieldLocation(const FACEWorldObject& Obj, int64 PreferredSlotMask) const
{
	if (PreferredSlotMask == ACEEquipMask::Shield && ACEEquipmentRules::CanWieldInSlot(Obj, PreferredSlotMask))
		return ACEEquipMask::Shield;
	if (Obj.ValidLocations == 0)
	{
		return 0;
	}
	// Armor / clothing: ACE expects the full ValidLocations mask.
	if ((Obj.ItemType & (ACEItemType::Armor | ACEItemType::Clothing)) != 0)
	{
		return Obj.ValidLocations;
	}

	auto SlotOccupied = [this](int64 Bit) -> bool
	{
		if (!Client || Bit == 0)
		{
			return false;
		}
		for (const FACEWorldObject& Eq : Client->GetEquippedItems())
		{
			if ((Eq.CurrentWieldedLocation & Bit) != 0)
			{
				return true;
			}
		}
		return false;
	};

	auto PickDual = [&](int64 Left, int64 Right) -> int64
	{
		const int64 Both = Left | Right;
		if ((Obj.ValidLocations & Both) == 0)
		{
			return 0;
		}
		if (PreferredSlotMask != 0 && (Obj.ValidLocations & PreferredSlotMask) != 0
			&& (PreferredSlotMask & Both) != 0 && !SlotOccupied(PreferredSlotMask))
		{
			return PreferredSlotMask;
		}
		const bool bCanLeft = (Obj.ValidLocations & Left) != 0;
		const bool bCanRight = (Obj.ValidLocations & Right) != 0;
		if (bCanLeft && !SlotOccupied(Left))
		{
			return Left;
		}
		if (bCanRight && !SlotOccupied(Right))
		{
			return Right;
		}
		// Both occupied (or only one valid and occupied) — return preferred/left for swap.
		if (PreferredSlotMask != 0 && (Obj.ValidLocations & PreferredSlotMask) != 0
			&& (PreferredSlotMask & Both) != 0)
		{
			return PreferredSlotMask;
		}
		if (bCanLeft)
		{
			return Left;
		}
		if (bCanRight)
		{
			return Right;
		}
		return 0;
	};

	if (const int64 Finger = PickDual(ACEEquipMask::FingerWearLeft, ACEEquipMask::FingerWearRight))
	{
		return Finger;
	}
	if (const int64 Wrist = PickDual(ACEEquipMask::WristWearLeft, ACEEquipMask::WristWearRight))
	{
		return Wrist;
	}

	// Never return a multi-bit doll mask (e.g. full ShirtWear) as the wield location —
	// ACE clothing/armor already returned ValidLocations above; other wearables need a bit.
	auto IsSingleBit = [](int64 M) { return M != 0 && (M & (M - 1)) == 0; };
	if (PreferredSlotMask != 0 && (Obj.ValidLocations & PreferredSlotMask) != 0
		&& IsSingleBit(PreferredSlotMask))
	{
		return PreferredSlotMask;
	}
	if (PreferredSlotMask != 0)
	{
		const int64 Overlap = Obj.ValidLocations & PreferredSlotMask;
		if (IsSingleBit(Overlap))
		{
			return Overlap;
		}
		// Multi-bit overlap (shirt core): use the object's authored ValidLocations.
		if (Overlap != 0)
		{
			return Obj.ValidLocations;
		}
	}
	static const int64 Priority[] = {
		ACEEquipMask::Held,
		ACEEquipMask::MeleeWeapon,
		ACEEquipMask::TwoHanded,
		ACEEquipMask::MissileWeapon,
		ACEEquipMask::Shield,
		ACEEquipMask::MissileAmmo,
		ACEEquipMask::HeadWear,
		ACEEquipMask::ChestArmor,
		ACEEquipMask::ChestWear,
		ACEEquipMask::AbdomenArmor,
		ACEEquipMask::UpperArmArmor,
		ACEEquipMask::LowerArmArmor,
		ACEEquipMask::HandWear,
		ACEEquipMask::UpperLegArmor,
		ACEEquipMask::LowerLegArmor,
		ACEEquipMask::FootWear,
		ACEEquipMask::NeckWear,
		ACEEquipMask::WristWearLeft,
		ACEEquipMask::WristWearRight,
		ACEEquipMask::FingerWearLeft,
		ACEEquipMask::FingerWearRight,
	};
	for (const int64 Bit : Priority)
	{
		if ((Obj.ValidLocations & Bit) != 0)
		{
			return Bit;
		}
	}
	return Obj.ValidLocations;
}

void UACEUIGameplayBinder::UnequipConflictsAndWield(int32 Guid, const FACEWorldObject& Obj, int64 Loc)
{
	// The session sequences the swap on server containment acknowledgements.
	if (Client) Client->SendGetAndWieldItem(Guid, Loc);
}

void UACEUIGameplayBinder::TargetActionDeckSelf()
{
    if (!Client) return;
    // Like the retail backpack shortcut, complete a pending targeted item on
    // self without replacing the combat target. Otherwise explicitly select self.
    if (!TryCompletePendingUseWithTarget(Client->GetPlayerGuid())) Client->SelectObject(Client->GetPlayerGuid());
}

void UACEUIGameplayBinder::UseInventoryItem(int32 Guid)
{
	if (!Client || Guid == 0)
	{
		return;
	}
	if (PlayerController&&PlayerController->GetObjectCursorTargeting()&&TryCompletePendingUseWithTarget(Guid)) return;
	FACEWorldObject Obj;
	if (!Client->GetWorldObject(Guid, Obj))
	{
		Client->SendUseItem(Guid);
		return;
	}
	const bool bManaStone = (Obj.ItemType & ACEItemType::ManaStone) != 0;
	// Use and selection are separate in gmToolbarUI::UseShortcut. In particular,
	// a kit shortcut followed by the main backpack targets self without replacing
	// the combat target. Inventory clicks select explicitly before activation.
	// Completing a pending dual-use: second click on another inventory item.
	if (PendingUseWithSourceGuid != 0 && PendingUseWithSourceGuid != Guid)
	{
		TryCompletePendingUseWithTarget(Guid);
		return;
	}
    // Re-clicking the source is not a request to consume it on the player.
    if (PendingUseWithSourceGuid == Guid) return;

	if (Guid == Client->GetPlayerGuid())
	{
		SelectedPackGuid = Guid;
		InventoryScrollOffset = 0;
		ShowPanelPage(TEXT("InventoryPanel_Field"));
		RefreshInventoryOverlays();
		SyncInventoryScrollbars();
		return;
	}

	// Inventory activation follows the retail decision tree. Neither item charges
	// nor a guessed list of wearable item types changes the advertised useability.
	const int32 Self = Client->GetPlayerGuid();
	bool bOwned = Obj.WielderId == Self || Obj.ContainerId == Self;
	int32 Owner = Obj.ContainerId;
	TSet<int32> Visited;
	while (!bOwned && Owner != 0 && !Visited.Contains(Owner))
	{
		Visited.Add(Owner);
		FACEWorldObject Container;
		if (!Client->GetWorldObject(Owner, Container)) break;
		bOwned = Container.ContainerId == Self || Container.WielderId == Self;
		Owner = Container.ContainerId;
	}
	// R, shortcuts and double-clicks may refer to world objects too. Use the
	// controller's approach/arrival path so Use is sent once at the destination,
	// rather than starting a second server move/use while our player is moving.
	// Combat stance itself is not a busy condition in retail ItemHolder::UseObject.
	if (!bOwned && Obj.bHasPosition && PlayerController)
	{
		PlayerController->InteractWithObject(Guid);
		return;
	}
	if (Client->IsUseBusy())
	{
		PostInventorySystemMessage(TEXT("You're too busy!"));
		return;
	}
	if (bOwned)
	{
		const EACEOwnedItemUse Result = ACEInventoryRules::DetermineOwnedUse(Obj, Self);
		if (Result == EACEOwnedItemUse::Backpack)
		{
			Client->SendPutItemInContainer(Guid, Self, 0);
			ApplyPreferredStance(CombatMode);
			return;
		}
		if (Result == EACEOwnedItemUse::WieldRight || Result == EACEOwnedItemUse::WieldLeft
			|| Result == EACEOwnedItemUse::AutoWear)
		{
			const int64 Preferred = Result == EACEOwnedItemUse::WieldLeft ? ACEEquipMask::Shield : 0;
			const int64 Location = ResolveWieldLocation(Obj, Preferred);
			if (Location) UnequipConflictsAndWield(Guid, Obj, Location);
			return;
		}
		if (Result == EACEOwnedItemUse::Salvage)
		{
			ShowSalvagePanel(Guid);
			return;
		}
		if (ACEInventoryRules::IsContainer(Obj))
		{
			SelectedPackGuid = Guid;
			InventoryScrollOffset = 0;
			ShowPanelPage(TEXT("InventoryPanel_Field"));
			RefreshInventoryOverlays();
			SyncInventoryScrollbars();
			return;
		}
	}
	if (!ACEInventoryRules::IsUsable(Obj.ItemUseable))
	{
		PostInventorySystemMessage(FString::Printf(TEXT("The %s cannot be used"), *ACERetailObjectNames::Name(Obj)));
		return;
	}
	if (Obj.CurrentWieldedLocation == 0 && ACEInventoryRules::LeastLimitedSourceUse(Obj.ItemUseable) == 4)
	{
		PostInventorySystemMessage(FString::Printf(TEXT("You must wield the %s to use it"), *ACERetailObjectNames::Name(Obj)));
		return;
	}
	const bool bDualUseTargeting = ACEItemUseable::IsTargeted(Obj.ItemUseable);
	if (bDualUseTargeting)
	{
		PendingUseWithSourceGuid = Guid;
		SyncPendingUseCursor();
		if (bManaStone)
		{
			// Structure>0 ≈ charged stone (PublicWeenieDesc); empty stones drain a target item.
			PostInventorySystemMessage((Obj.UiEffects & 1) != 0
				? TEXT("Select an item to give mana to, or click yourself to charge all equipped items.")
				: TEXT("Select an item to drain mana from (the item will be destroyed)."));
		}
		else
		{
			PostInventorySystemMessage(TEXT("Select a target for that item."));
		}
		return;
	}
	if (Client->IsUseBusy())
	{
		PostInventorySystemMessage(TEXT("You're too busy!"));
		return;
	}
	Client->SendUseItem(Guid);
}

bool UACEUIGameplayBinder::TryCompletePendingUseWithTarget(int32 TargetGuid)
{
	if (Client&&TargetGuid&&PlayerController&&PlayerController->GetObjectCursorTargeting())
	{
		const int32 Mode=PlayerController->GetObjectCursorTargeting();
		PlayerController->SetObjectCursorTargeting(0);
		if (Mode==2) Client->SendIdentifyObject(TargetGuid);
		else UseInventoryItem(TargetGuid);
		return true;
	}
	if (PendingUseWithSourceGuid == 0 || TargetGuid == 0 || !Client)
	{
		return false;
	}
	// Same guid only allowed when targeting self via the mana stone (use → player).
	if (PendingUseWithSourceGuid == TargetGuid && TargetGuid != Client->GetPlayerGuid())
	{
		return false;
	}
	if (Client->IsUseBusy())
	{
		PostInventorySystemMessage(TEXT("You're too busy!"));
		CancelPendingUseWith();
		return true;
	}
	FACEWorldObject Source, Target;
	if (!Client->GetWorldObject(PendingUseWithSourceGuid, Source) || !Client->GetWorldObject(TargetGuid, Target))
	{
		CancelPendingUseWith();
		return true;
	}
	// Use the same public target mask, ownership and trade restrictions as the
	// retail targeting cursor, before offering any destructive confirmation.
	if (!IsUseTargetCompatible(Source.Guid, TargetGuid))
	{
		PostInventorySystemMessage(TEXT("Choose a compatible target. Use on item is still active."));
		return true;
	}
	// ItemHolder::TargetAcquired: empty mana stones destroy the selected item.
	// Retail blocks Retained items and requires the authored Yes/No dialog first.
	if ((Source.ItemType & ACEItemType::ManaStone) && !(Source.UiEffects & 1))
	{
		if (Target.ObjectDescriptionFlags & ACEObjectDescFlag::Retained)
			PostInventorySystemMessage(TEXT("You cannot drain the mana of this item because it is \"Retained\"."));
		else if (TargetGuid != Client->GetPlayerGuid()) ShowManaStoneConfirmation(Source.Guid, Target);
		else PostInventorySystemMessage(TEXT("Select an item to drain into the empty mana stone."));
	}
	else Client->SendUseWithTarget(Source.Guid, TargetGuid);
	PendingUseWithSourceGuid = 0;
	SyncPendingUseCursor();
	return true;
}

void UACEUIGameplayBinder::ShowManaStoneConfirmation(int32 Source, const FACEWorldObject& Target)
{
	if (!Manager || !Canvas || !Canvas->WidgetTree) return;
	if (!ManaStoneConfirmRoot)
	{
		ManaStoneConfirmRoot = UACEUILayoutResolver::LoadTemplate(0x2100003C, 0x15);
		if (!ManaStoneConfirmRoot) return;
		ManaStoneConfirmRoot->SetElementName(TEXT("RootGameplay_FloatyManaStoneConfirmation_Field"));
		TFunction<void(TSharedPtr<FACEUIElement>)> NameControls = [&](TSharedPtr<FACEUIElement> Node)
		{
			if (Node->ElementId == 0x3D)
			{
				Node->LeftEdge = Node->RightEdge = Node->TopEdge = Node->BottomEdge = 3;
			}
			if (Node->ElementId == 0x3E) Node->SetElementName(TEXT("ManaStoneConfirmationBody"));
			if (Node->ElementId == 0x17) Node->SetElementName(TEXT("ManaStoneConfirmationYes"));
			if (Node->ElementId == 0x19) Node->SetElementName(TEXT("ManaStoneConfirmationNo"));
			for (const auto& Child : Node->Children) NameControls(Child);
		};
		NameControls(ManaStoneConfirmRoot);
		// Join the gameplay window stack so DAT chrome and text share one paint base.
		if (auto GameplayRoot=Manager->FindElementByName(TEXT("RootGameplay_Field"))) GameplayRoot->AddChild(ManaStoneConfirmRoot);
		else Manager->AddRoot(ManaStoneConfirmRoot);
		for (int32 I=0; I<3; ++I)
			ManaStoneConfirmLabels.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	}
	ManaStoneConfirmSource = Source;
	ManaStoneConfirmTarget = Target.Guid;
	ManaStoneConfirmRoot->bVisible = true;
	// Size the body from the same DAT glyph advances that paint it. DialogFactory
	// grows the authored box to the wrapped text, then centers it in the viewport.
	const auto Body = Manager->FindElementByName(TEXT("ManaStoneConfirmationBody"));
	FACEDatFont Font;
	if (Body && Canvas->GetResourceResolver()->ResolveFont(Body->FontId, Font))
	{
		const FString Prompt = FString::Printf(TEXT("\nAre you sure you want to attempt to destroy your %s and drain its mana into this stone?"), *ACERetailObjectNames::Name(Target));
		const int32 H = ACEDatText::Layout(Font,Prompt,Body->Width-Body->TextMargins.Left-Body->TextMargins.Right,false).Num()*Font.MaxCharHeight
			+ Body->TextMargins.Top+Body->TextMargins.Bottom;
		const int32 Delta=H-Body->Height;
		if (auto Box=Body->Parent.Pin())
		{
			Box->Height+=Delta; Box->LayoutAuthoredH=Box->LastReflowH=Box->Height;
			for (const auto& Child:Box->Children)
			{
				if (Child->TopEdge==2) Child->Y+=Delta;
				else if (Child->BottomEdge==1) Child->Height+=Delta;
				Child->LayoutAuthoredH=Child->LastReflowH=Child->Height;
			}
		}
	}
	Manager->BringFloatyToFront(ManaStoneConfirmRoot);
	RefreshManaStoneConfirmation();
}

void UACEUIGameplayBinder::RefreshManaStoneConfirmation()
{
	if (!ManaStoneConfirmSource || !Client || ManaStoneConfirmLabels.Num()!=3) return;
	FACEWorldObject Target;
	if (!Client->GetWorldObject(ManaStoneConfirmTarget, Target)) { FinishManaStoneConfirmation(false); return; }
	const FString Prompt = FString::Printf(TEXT("\nAre you sure you want to attempt to destroy your %s and drain its mana into this stone?"), *ACERetailObjectNames::Name(Target));
	PlaceTextOnElement(ManaStoneConfirmLabels[0], TEXT("ManaStoneConfirmationBody"), Prompt, 12, TextWhite, 200020);
	PlaceTextOnElement(ManaStoneConfirmLabels[1], TEXT("ManaStoneConfirmationYes"), TEXT("Yes"), 12, TextWhite, 200020, true);
	PlaceTextOnElement(ManaStoneConfirmLabels[2], TEXT("ManaStoneConfirmationNo"), TEXT("No"), 12, TextWhite, 200020, true);
}

void UACEUIGameplayBinder::FinishManaStoneConfirmation(bool bAccept)
{
	const int32 Source = ManaStoneConfirmSource, Target = ManaStoneConfirmTarget;
	ManaStoneConfirmSource = ManaStoneConfirmTarget = 0;
	if (ManaStoneConfirmRoot) ManaStoneConfirmRoot->bVisible = false;
	for (UTextBlock* Label : ManaStoneConfirmLabels) if (Label) Label->SetVisibility(ESlateVisibility::Collapsed);
	FACEWorldObject SourceObject, TargetObject;
	if (bAccept && Source && Client && !Client->IsUseBusy()
		&& Client->GetWorldObject(Source, SourceObject) && Client->GetWorldObject(Target, TargetObject)
		&& IsUseTargetCompatible(Source, Target)
		&& !(TargetObject.ObjectDescriptionFlags & ACEObjectDescFlag::Retained))
		Client->SendUseWithTarget(Source, Target);
}

void UACEUIGameplayBinder::SyncPendingUseCursor()
{
	if (PlayerController)
	{
		PlayerController->SetPendingUseTargeting(PendingUseWithSourceGuid != 0);
	}
}

void UACEUIGameplayBinder::CancelPendingUseWith()
{
	if (PlayerController) PlayerController->SetObjectCursorTargeting(0);
	FinishManaStoneConfirmation(false);
	if (PendingUseWithSourceGuid == 0)
	{
		return;
	}
	PendingUseWithSourceGuid = 0;
	SyncPendingUseCursor();
}

int32 UACEUIGameplayBinder::GetPendingUseWithSourceGuid() const
{
	return PendingUseWithSourceGuid;
}

bool UACEUIGameplayBinder::IsPendingUseTargetCompatible(int32 TargetGuid) const
{
	return IsUseTargetCompatible(PendingUseWithSourceGuid, TargetGuid);
}

bool UACEUIGameplayBinder::IsUseTargetCompatible(int32 SourceGuid, int32 TargetGuid) const
{
	if (!Client) return false;
	FACEWorldObject Source, Target;
	if (!Client->GetWorldObject(SourceGuid, Source) || !Client->GetWorldObject(TargetGuid, Target)) return false;
	auto Owned = [&](FACEWorldObject Object)
	{
		TSet<int32> Visited;
		while (Object.Guid && !Visited.Contains(Object.Guid))
		{
			Visited.Add(Object.Guid);
			const int32 Parent = Object.WielderId ? Object.WielderId : Object.ContainerId;
			if (Parent == Client->GetPlayerGuid()) return true;
			if (!Parent || !Client->GetWorldObject(Parent,Object)) break;
		}
		return false;
	};
	return ACEInventoryRules::IsTargetCompatible(Source,Target,Client->GetPlayerGuid(),
		Owned(Source),Owned(Target),Client->GetTradeSelfItems().Contains(TargetGuid));
}

int32 UACEUIGameplayBinder::FindUpperEquippedItem(int64 Mask) const
{
	int32 Guid = 0; uint32 Priority = 0;
	if (Client && Mask) for (const auto& Item : Client->GetEquippedItems())
	{
		const uint32 Locations = uint32(Item.CurrentWieldedLocation & Mask);
		if (!Locations) continue;
		uint32 Candidate = uint32(Item.ClothingPriority);
		if (!Candidate && Locations >= 0x200 && Locations <= 0x4000) Candidate = 127;
		if (!Guid || Candidate >= Priority) { Guid = Item.Guid; Priority = Candidate; }
	}
	return Guid;
}

int32 UACEUIGameplayBinder::ResolvePaperDollUseTarget(const FString& SlotName, int64 Mask) const
{
	if (!Client) return 0;
	// gmPaperDollUI::ListenToElementMessage targets GetPlayerID() in target mode,
	// even when the silhouette's body mask identifies a worn item. This lets the
	// server distribute a charged mana stone across equipped items just like a
	// world-model click. Visible slots still target only the item in that slot.
	if (!bShowPaperdollSlots && SlotName == TEXT("PaperDoll")) return Client->GetPlayerGuid();
	return FindUpperEquippedItem(Mask);
}

int32 UACEUIGameplayBinder::GetInventoryTargetAt(FVector2D Absolute) const
{
	if (!Client || !Canvas || !Manager || ActivePanelPage != TEXT("InventoryPanel_Field")) return 0;
	int32 Guid=0, Slot=0;
	if (HitTestInventorySlot(Absolute,Guid,Slot)) return Guid;
	if (HitTestPackSlot(Absolute,Guid)) return Guid;
	FString SlotName; int64 Mask=0;
	if (HitTestDollSlot(Absolute,SlotName,Mask))
	{
		return ResolvePaperDollUseTarget(SlotName, Mask);
	}
	const auto Doll=Manager->FindElementUnder(TEXT("InventoryPanel_Field"),TEXT("PaperDoll"));
	const FVector2D Local=Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute);
	if (!bShowPaperdollSlots && Doll && Canvas->IsElementExposedAt(Doll,Local))
	{
		const FVector2D P=Local/Canvas->GetLastScale2D()-FVector2D(Doll->GetScreenOrigin());
		if (P.X>=0 && P.Y>=0 && P.X<Doll->Width && P.Y<Doll->Height) return Client->GetPlayerGuid();
	}
	return 0;
}

void UACEUIGameplayBinder::CancelInventoryDrag()
{
	bInvDragFromVendorSell = false;
	bInvDragFromSalvage = false;
	bInvDragFromHouse = false;
	if (PaperDollDragTargetIcon) PaperDollDragTargetIcon->SetVisibility(ESlateVisibility::Collapsed);
	InvDragGuid = 0;
	InvDragAmount = 0;
	InvDragShortcutSlot = INDEX_NONE;
	InvDragIconDid = 0;
	InvDragSourcePack = 0;
	InvDragSourceSlot = INDEX_NONE;
	InvDragPackSlotIndex = INDEX_NONE;
	bInvDragPending = false;
	bInvDragActive = false;
	bInvDoubleClickPending = false;
	if (InvDragIcon)
	{
		InvDragIcon->SetVisibility(ESlateVisibility::Collapsed);
        if(Canvas)Canvas->HideDragIcon(InvDragIcon);
	}
	SyncInventoryButtonVisual();
}

void UACEUIGameplayBinder::CancelSpellDrag()
{
	SpellDragId = 0;
	SpellDragIconDid = 0;
	SpellDragSourceBarSlot = INDEX_NONE;
	SpellDragSourceBar = INDEX_NONE;
	bSpellDragPending = false;
	bSpellDragActive = false;
	if (SpellDropMarker) SpellDropMarker->SetVisibility(ESlateVisibility::Collapsed);
	if (SpellDragIcon)
	{
		SpellDragIcon->SetVisibility(ESlateVisibility::Collapsed);
        if(Canvas)Canvas->HideDragIcon(SpellDragIcon);
	}
}

bool UACEUIGameplayBinder::HitTestInventorySlot(FVector2D Absolute, int32& OutGuid, int32& OutSlotIndex) const
{
	OutGuid = 0;
	OutSlotIndex = INDEX_NONE;
	auto TestArray = [&](const TArray<TObjectPtr<UBorder>>& Borders) -> bool
	{
		for (int32 i = 0; i < Borders.Num() && i < InventorySlotGuids.Num(); ++i)
		{
			UBorder* Border = Borders[i];
			if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
			{
				continue;
			}
			if (Canvas->IsWidgetExposedAt(Border, Absolute))
			{
				OutGuid = InventorySlotGuids[i];
				OutSlotIndex = InventoryScrollOffset + i;
				return true;
			}
		}
		return false;
	};
	// Prefer icon, then empty cell background (drop targets).
	return TestArray(InventorySlots) || TestArray(InventorySlotBgs);
}

bool UACEUIGameplayBinder::HitTestPackSlot(FVector2D Absolute, int32& OutPackGuid, bool IncludeEmpty) const
{
	OutPackGuid = 0;
	for (int32 i = 0; i < PackSlots.Num() && i < PackSlotGuids.Num(); ++i)
	{
		UBorder* Border = PackSlots[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			OutPackGuid = PackSlotGuids[i];
			return IncludeEmpty || OutPackGuid != 0;
		}
	}
	return false;
}

bool UACEUIGameplayBinder::HitTestDollSlot(FVector2D Absolute, FString& OutSlotName, int64& OutMask) const
{
	OutSlotName.Reset();
	OutMask = 0;
	// Prefer overlay icons when visible (equipped / show-equipment on).
	for (const auto& Pair : PaperDollIcons)
	{
		UBorder* Border = Pair.Value;
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (!Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			continue;
		}
		for (const FDollSlotMap& Slot : GDollSlots)
		{
			if (Pair.Key == Slot.Name)
			{
				OutSlotName = Slot.Name;
				OutMask = Slot.Mask;
				return true;
			}
		}
	}
	// Retail: drops still work when Show Equipment is off — hit DAT slot geometry.
	if (!Manager || !Canvas || !Canvas->GetElementLayer())
	{
		return false;
	}
	const FGeometry LayerGeo = Canvas->GetElementLayer()->GetCachedGeometry();
	const FVector2D Local = LayerGeo.AbsoluteToLocal(Absolute);
	for (const FDollSlotMap& Slot : GDollSlots)
	{
		TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), Slot.Name);
		if (!El.IsValid())
		{
			El = Manager->FindElementByName(Slot.Name);
		}
		if (!El.IsValid() || !El->bVisible || (!bShowPaperdollSlots && IsMainBodyDollSlot(Slot.Name)))
		{
			continue;
		}
		const FIntPoint O = El->GetScreenOrigin();
		if (Local.X >= O.X && Local.Y >= O.Y
			&& Local.X < O.X + El->Width && Local.Y < O.Y + El->Height)
		{
			OutSlotName = Slot.Name;
			OutMask = Slot.Mask;
			return true;
		}
	}
	if (!bShowPaperdollSlots)
	{
		const auto Mask = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("PaperDollDragMask"));
		if (Mask && Canvas->IsElementExposedAt(Mask, Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute)))
		{
			const FVector2D P = Local - FVector2D(Mask->GetScreenOrigin());
			if (P.X >= 0 && P.Y >= 0 && P.X < Mask->Width && P.Y < Mask->Height)
			{
				OutSlotName = TEXT("PaperDoll");
				if (auto* Resources = Canvas->GetResourceResolver())
					OutMask = Resources->ResolvePaperDollSelectionMask(FIntPoint(FMath::FloorToInt(P.X), FMath::FloorToInt(P.Y)));
				return true;
			}
		}
	}
	return false;
}

bool UACEUIGameplayBinder::IsPointerOverInventoryPanel(FVector2D CanvasLocalPos) const
{
	if (!Manager || ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		return false;
	}
	TSharedPtr<FACEUIElement> Page = Manager->FindElementByName(TEXT("InventoryPanel_Field"));
	if (!Page.IsValid() || !Page->bVisible)
	{
		return false;
	}
	const FIntPoint O = Page->GetScreenOrigin();
	return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
		&& CanvasLocalPos.X < O.X + Page->Width && CanvasLocalPos.Y < O.Y + Page->Height;
}

bool UACEUIGameplayBinder::TryBeginWorldInventoryDrag(FVector2D CanvasLocalPos)
{
	if (!Canvas || !PlayerController || PlayerController->IsVRActive() || PlayerController->IsMobileActive()
		|| CombatMode != int32(ACECombatMode::NonCombat) || PendingUseWithSourceGuid
		|| PlayerController->GetObjectCursorTargeting() || PlayerController->IsInputKeyDown(EKeys::RightMouseButton)
		|| !PlayerController->bShowMouseCursor || bInvDragPending) return false;
	FVector2D Pixels, WidgetPosition;
	USlateBlueprintLibrary::AbsoluteToViewport(PlayerController,
		Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos), Pixels, WidgetPosition);
	const auto* Entity = PlayerController->PickWorldEntityAtScreenPosition(Pixels.X, Pixels.Y);
	return Entity && BeginWorldItemDrag(Entity->GetACEGuid(), CanvasLocalPos);
}

bool UACEUIGameplayBinder::BeginWorldItemDrag(int32 Guid, FVector2D CanvasLocalPos)
{
	// UIElement_SmartBoxWrapper: peace mode, movable non-creature, not wielded.
	// Opening a ground pack is still Use; moving it requires an actual drag.
	FACEWorldObject Item;
	if (!Client || !Canvas || Client->IsUseBusy() || CombatMode != int32(ACECombatMode::NonCombat)
		|| !Client->GetWorldObject(Guid, Item) || !Item.IsWorldLootable()) return false;
	CancelInventoryDrag();
	ClearChatEntryFocus();
	InvDragGuid = Guid;
	InvDragIconDid = Item.IconId;
	InvDragStartLocal = CanvasLocalPos;
	bInvDragPending = true;
	bInvDoubleClickPending = Guid == LastInvClickGuid && FPlatformTime::Seconds() - LastInvClickTime < InventoryDoubleClickSeconds();
	SelectInventoryGuid(Guid);
	InvDragAmount = GetSelectedItemAmount(Guid);
	return true;
}

bool UACEUIGameplayBinder::TryBeginInventoryDrag(FVector2D CanvasLocalPos)
{
	if (!Canvas || !Canvas->GetElementLayer())
	{
		return false;
	}
	const FVector2D Absolute = Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos);
	// Commit a typed quantity before selection refresh repaints the toolbar.
	// Slate can deliver focus loss only after this mouse-down handler.
	if (StackAmountEntry && StackAmountEntry->HasKeyboardFocus())
		HandleStackAmountCommitted(StackAmountEntry->GetText(), ETextCommit::OnUserMovedFocus);
	ON_SCOPE_EXIT
	{
		if (bInvDragPending && InvDragGuid) InvDragAmount = GetSelectedItemAmount(InvDragGuid);
	};
	if (TryHouseItemClick(CanvasLocalPos)) return true;

	InvDragShortcutSlot = INDEX_NONE;
	const int32 Index = HitTestShortcutSlot(CanvasLocalPos);
	if (Client && Index != INDEX_NONE)
	{
		const int32 Guid = Client->GetShortcutObject(Index);
		FACEWorldObject Obj;
		if (!Guid || !Client->GetWorldObject(Guid, Obj)) return false;
		InvDragGuid = Guid;
		InvDragIconDid = Guid == Client->GetPlayerGuid() ? 0x06004CF7u : Obj.IconId;
		InvDragShortcutSlot = Index;
		InvDragSourcePack = 0;
		InvDragSourceSlot = InvDragPackSlotIndex = INDEX_NONE;
		InvDragStartLocal = CanvasLocalPos;
		bInvDragPending = true;
		bInvDragActive = false;
		bInvDoubleClickPending = Guid == LastInvClickGuid && FPlatformTime::Seconds() - LastInvClickTime < InventoryDoubleClickSeconds();
		if (!bInvDoubleClickPending && !ShouldPreserveShortcutSelection(Guid)) SelectInventoryGuid(Guid);
		return true;
	}

	// Salvage entries reference inventory; dragging out only removes the offer.
	if(OpenSalvageToolGuid)
		for(int32 I=0;I<SalvageItemSlots.Num() && I<SalvageItemSlotGuids.Num();++I)
		{
			if(!SalvageItemSlots[I] || !Canvas->IsWidgetExposedAt(SalvageItemSlots[I],Absolute))continue;
			FACEWorldObject Item;if(!Client || !Client->GetWorldObject(SalvageItemSlotGuids[I],Item))return false;
			InvDragGuid=Item.Guid;InvDragIconDid=Item.IconId;InvDragStartLocal=CanvasLocalPos;
			InvDragSourcePack=0;InvDragSourceSlot=InvDragPackSlotIndex=INDEX_NONE;
			bInvDragPending=true;bInvDragActive=false;bInvDragFromSalvage=true;
			bInvDoubleClickPending=Item.Guid==LastInvClickGuid && FPlatformTime::Seconds()-LastInvClickTime<InventoryDoubleClickSeconds();
			SelectInventoryGuid(Item.Guid);RefreshSalvageOverlays();return true;
		}
	// Selling entries are references to owned inventory. Dragging one back out
	// cancels the offer locally; it must never drop, use, or move the real item.
	if (OpenVendorGuid && ActiveVendorPage == 2)
	{
		for (int32 I=0; I<VendorItemSlots.Num() && I<VendorItemGuids.Num(); ++I)
		{
			if (!VendorItemSlots[I] || !Canvas->IsWidgetExposedAt(VendorItemSlots[I],Absolute)) continue;
			FACEWorldObject Item;
			if (!Client || !Client->GetWorldObject(VendorItemGuids[I],Item)) return false;
			InvDragGuid=Item.Guid; InvDragIconDid=Item.IconId; InvDragStartLocal=CanvasLocalPos;
			InvDragSourcePack=0; InvDragSourceSlot=InvDragPackSlotIndex=INDEX_NONE;
			bInvDragPending=true; bInvDragActive=false; bInvDragFromVendorSell=true;
			bInvDoubleClickPending=Item.Guid==LastInvClickGuid && FPlatformTime::Seconds()-LastInvClickTime<InventoryDoubleClickSeconds();
			VendorSellSelectedGuid=Item.Guid; SelectInventoryGuid(Item.Guid);
			return true;
		}
	}

	// Retail's external container list supports the same drag as its item list.
	// The root is also an ItemList in retail: portable ground packs can be
	// dragged from it, while fixed chests/corpses retain their navigation click.
	if (OpenLootContainerGuid != 0)
	{
		for (int32 i = 0; i < ExtPackSlots.Num() && i < ExtPackGuids.Num(); ++i)
		{
			UBorder* Border = ExtPackSlots[i];
			if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed
				|| !Canvas->IsWidgetExposedAt(Border, Absolute)) continue;
			FACEWorldObject Pack;
			const int32 Guid = ExtPackGuids[i];
			if (!Guid || !Client || !Client->GetWorldObject(Guid, Pack)) return false;
			if (i == 0 && !Pack.IsWorldLootable() && !Pack.ContainerId) return false;
			InvDragGuid = Guid;
			InvDragIconDid = Pack.IconId;
			InvDragSourcePack = OpenLootContainerGuid;
			InvDragSourceSlot = i == 0 ? INDEX_NONE : i - 1 + ExtPackScrollOffset;
			InvDragPackSlotIndex = INDEX_NONE;
			InvDragStartLocal = CanvasLocalPos;
			bInvDragPending = true;
			bInvDragActive = false;
			// Container-list clicks open the pack, including repeated clicks. They
			// must not take the item-list double-click pickup path on release.
			bInvDoubleClickPending = false;
			SelectInventoryGuid(Guid);
			if (!PendingUseWithSourceGuid && !(PlayerController && PlayerController->GetObjectCursorTargeting()))
			{
				OpenLootSelectedPackGuid = Guid;
				ExtItemScrollOffset = 0;
				RefreshExternalContainerOverlays();
			}
			return true;
		}
		// Ordinary loot items also support double-click pickup.
		for (int32 i = 0; i < ExtItemSlots.Num() && i < ExtItemGuids.Num(); ++i)
		{
			UBorder* Border = ExtItemSlots[i];
			if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
			{
				continue;
			}
			if (!Canvas->IsWidgetExposedAt(Border, Absolute))
			{
				continue;
			}
			const int32 Guid = ExtItemGuids[i];
			if (Guid == 0)
			{
				return false;
			}
			const double Now = FPlatformTime::Seconds();
			const double DoubleClickSeconds = InventoryDoubleClickSeconds();
			bInvDoubleClickPending = Guid == LastInvClickGuid && (Now - LastInvClickTime) < DoubleClickSeconds;
			FACEWorldObject Obj;
			if (Client)
			{
				Client->GetWorldObject(Guid, Obj);
			}
			InvDragGuid = Guid;
			InvDragIconDid = Obj.IconId;
			InvDragSourcePack = OpenLootSelectedPackGuid != 0 ? OpenLootSelectedPackGuid : OpenLootContainerGuid;
			InvDragSourceSlot = i + ExtItemScrollOffset;
			InvDragPackSlotIndex = INDEX_NONE;
			InvDragStartLocal = CanvasLocalPos;
			bInvDragPending = true;
			bInvDragActive = false;
			SelectInventoryGuid(Guid);
			return true;
		}
	}

	if (ActivePanelPage != TEXT("InventoryPanel_Field") || !Manager
		|| !Canvas->IsElementExposedAt(Manager->FindElementByName(TEXT("InventoryPanel_Field")), CanvasLocalPos))
	{
		return false;
	}

	int32 Guid = 0;
	int32 SlotIndex = INDEX_NONE;
	const bool bHitSlot = HitTestInventorySlot(Absolute, Guid, SlotIndex);
	// Empty-cell hits must not fall through to grid math (scale mismatch grabbed neighbors).
	if (!bHitSlot)
	{
		// Fallback: DAT grid math in scaled canvas space (borders can lag one frame).
		if (TSharedPtr<FACEUIElement> GridEl = Manager
			? Manager->FindElementByName(TEXT("Inv_3DItemList")) : nullptr)
		{
			const FIntPoint Origin = GridEl->GetScreenOrigin();
			const float SX = FMath::Max(KINDA_SMALL_NUMBER, Canvas->GetLastScaleX());
			const float SY = FMath::Max(KINDA_SMALL_NUMBER, Canvas->GetLastScaleY());
			constexpr int32 Cell = 32;
			const int32 Cols = FMath::Max(1, GridEl->Width / Cell);
			const int32 Rows = FMath::Max(1, GridEl->Height / Cell);
			const int32 RelX = FMath::FloorToInt(CanvasLocalPos.X / SX) - Origin.X;
			const int32 RelY = FMath::FloorToInt(CanvasLocalPos.Y / SY) - Origin.Y;
			if (RelX >= 0 && RelY >= 0 && RelX < GridEl->Width && RelY < GridEl->Height)
			{
				const int32 Col = RelX / Cell;
				const int32 Row = RelY / Cell;
				const int32 PageIndex = Row * Cols + Col;
				if (InventorySlotGuids.IsValidIndex(PageIndex) && InventorySlotGuids[PageIndex] != 0
					&& PageIndex < Cols * Rows)
				{
					Guid = InventorySlotGuids[PageIndex];
					SlotIndex = InventoryScrollOffset + PageIndex;
				}
			}
		}
	}

	if (Guid != 0)
	{
		// Arm double-click on MouseDown, but Use only on MouseUp if this press never
		// becomes an active drag — otherwise select→drag was impossible within 0.75s.
		const double Now = FPlatformTime::Seconds();
		const double DoubleClickSeconds = InventoryDoubleClickSeconds();
		const bool bDoubleClick = Guid == LastInvClickGuid
			&& (Now - LastInvClickTime) < DoubleClickSeconds;

		FACEWorldObject Obj;
		InvDragGuid = Guid;
		InvDragIconDid = Client && Client->GetWorldObject(Guid, Obj) ? Obj.IconId : 0;
		InvDragSourcePack = SelectedPackGuid;
		InvDragSourceSlot = SlotIndex;
		InvDragPackSlotIndex = INDEX_NONE;
		InvDragStartLocal = CanvasLocalPos;
		bInvDragPending = true;
		bInvDragActive = false;
		bInvDoubleClickPending = bDoubleClick;
		SelectInventoryGuid(Guid);
		return true;
	}

	// Packs: retail also permits a shortcut to the player/main backpack.
	for (int32 i = 0; i < PackSlots.Num() && i < PackSlotGuids.Num(); ++i)
	{
		UBorder* Border = PackSlots[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (!Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			continue;
		}
		const int32 PackGuid = PackSlotGuids[i];
		if (!Client || PackGuid == 0)
		{
			continue;
		}
		FACEWorldObject Obj;
		if (!Client->GetWorldObject(PackGuid, Obj))
		{
			continue;
		}
		// Select immediately so a click (no drag) shows this pack's inventory.
		SelectedPackGuid = PackGuid;
		InventoryScrollOffset = 0;
		RefreshInventoryOverlays();
		SyncInventoryScrollbars();
		InvDragGuid = PackGuid;
		InvDragIconDid = PackGuid == Client->GetPlayerGuid() ? 0x06004CF7u : Obj.IconId;
		InvDragSourcePack = Client->GetPlayerGuid();
		InvDragSourceSlot = INDEX_NONE;
		InvDragPackSlotIndex = i;
		InvDragStartLocal = CanvasLocalPos;
		bInvDragPending = true;
		bInvDragActive = false;
		SelectInventoryGuid(PackGuid);
		return true;
	}

	FString DollName;
	int64 DollMask = 0;
	if (HitTestDollSlot(Absolute, DollName, DollMask) && Client)
	{
		// Target-mode clicks take precedence over starting a worn-item drag.
		if (PendingUseWithSourceGuid || (PlayerController && PlayerController->GetObjectCursorTargeting()))
		{
			TryCompletePendingUseWithTarget(ResolvePaperDollUseTarget(DollName, DollMask));
			return true;
		}
		for (const FACEWorldObject& Obj : Client->GetEquippedItems())
		{
			if (Obj.Guid == FindUpperEquippedItem(DollMask))
			{
				const double Now = FPlatformTime::Seconds();
				const double DoubleClickSeconds = InventoryDoubleClickSeconds();
				const bool bDoubleClick = Obj.Guid == LastInvClickGuid
					&& (Now - LastInvClickTime) < DoubleClickSeconds;
				InvDragGuid = Obj.Guid;
				InvDragIconDid = Obj.IconId;
				InvDragSourcePack = 0;
				InvDragSourceSlot = INDEX_NONE;
				InvDragStartLocal = CanvasLocalPos;
				bInvDragPending = true;
				bInvDragActive = false;
				bInvDoubleClickPending = bDoubleClick;
				SelectInventoryGuid(Obj.Guid);
				return true;
			}
		}
	}
	return false;
}

void UACEUIGameplayBinder::UpdateInventoryDrag(FVector2D CanvasLocalPos)
{
	if (!bInvDragPending || InvDragGuid == 0 || !Canvas || !Canvas->WidgetTree)
	{
		return;
	}
	if (!bInvDragActive)
	{
		// 12px — Windows double-click jitter often exceeds 6px and was killing Use.
		if (FVector2D::Distance(CanvasLocalPos, InvDragStartLocal) < InventoryDragThreshold())
		{
			return;
		}
		bInvDragActive = true;
		bInvDoubleClickPending = false;
		LastInvClickGuid = 0;
		LastInvClickTime = 0.0;
		SyncInventoryButtonVisual();
	}
	if (!InvDragIcon)
	{
		InvDragIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		InvDragIcon->SetPadding(FMargin(0.f));
		InvDragIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	SetIconDid(InvDragIcon, InvDragIconDid);
	// ItemSlot's DAT accept/reject artwork belongs to the hovered destination cell.
	if (PaperDollDragTargetIcon) PaperDollDragTargetIcon->SetVisibility(ESlateVisibility::Collapsed);
	FString TargetName; int64 TargetMask = 0;
	const FVector2D Absolute = Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos);
	if (Client && HitTestDollSlot(Absolute, TargetName, TargetMask) && TargetName != TEXT("PaperDoll"))
	{
		const auto TargetElement = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TargetName);
		FACEWorldObject Item;
		if (TargetElement && Client->GetWorldObject(InvDragGuid, Item))
		{
			if (!PaperDollDragTargetIcon)
			{
				PaperDollDragTargetIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
				PaperDollDragTargetIcon->SetPadding(FMargin(0));
			}
			bool bAccept = ACEEquipmentRules::CanWieldInSlot(Item, TargetMask);
			if ((Item.ItemType & ACEItemType::ManaStone) || ACEItemUseable::IsTargeted(Item.ItemUseable))
			{
				const int32 TargetGuid = ResolvePaperDollUseTarget(TargetName, TargetMask);
				bAccept = IsUseTargetCompatible(InvDragGuid, TargetGuid);
			}
			SetIconDid(PaperDollDragTargetIcon, bAccept ? 0x060011F9 : 0x060011F8);
			PaperDollDragTargetIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
			Canvas->PlaceWidgetAtElement(PaperDollDragTargetIcon, TargetElement, 120004);
		}
	}
	if (OpenVendorGuid && Client)
	{
		for (UBorder* Cell : VendorItemSlotBgs)
		{
			if (!Cell || !Cell->IsVisible() || !Canvas->IsWidgetExposedAt(Cell, Absolute)) continue;
			const auto* CellSlot = Cast<UCanvasPanelSlot>(Cell->Slot);
			if (!CellSlot) break;
			if (!PaperDollDragTargetIcon)
			{
				PaperDollDragTargetIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
				PaperDollDragTargetIcon->SetPadding(FMargin(0));
			}
			SetIconDid(PaperDollDragTargetIcon, CanStageVendorSellItem(InvDragGuid) ? 0x060011F9 : 0x060011F8);
			PaperDollDragTargetIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (PaperDollDragTargetIcon->GetParent() != Canvas->GetElementLayer())
				Canvas->GetElementLayer()->AddChild(PaperDollDragTargetIcon);
			if (auto* Slot = Cast<UCanvasPanelSlot>(PaperDollDragTargetIcon->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetAutoSize(false);
				Slot->SetPosition(CellSlot->GetPosition());
				Slot->SetSize(CellSlot->GetSize());
				Canvas->SetOverlayOrder(PaperDollDragTargetIcon,
					Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field")), 120004);
			}
			break;
		}
	}
	UpdateInventoryContainerDropFeedback(CanvasLocalPos);
	InvDragIcon->SetBrushColor(FLinearColor(1.f, 1.f, 1.f, 0.85f));
	InvDragIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
    Canvas->ShowDragIcon(InvDragIcon,CanvasLocalPos);
}

void UACEUIGameplayBinder::AssignInventoryShortcut(int32 Guid, int32 SlotIndex, int32 SourceShortcut)
{
	if (!Client || !Guid || SlotIndex < 0 || SlotIndex >= 18) return;
	// gmToolbarUI::OnUIElementMessage / CreateShortcutToItem: remove the
	// destination, move this item's binding, then rehome the displaced item.
	const int32 Displaced = Client->GetShortcutObject(SlotIndex);
	if (Displaced) Client->SendRemoveShortcut(SlotIndex);
	for (int32 Index = 0; Index < 18; ++Index)
		if (Client->GetShortcutObject(Index) == Guid) Client->SendRemoveShortcut(Index);
	Client->SendAddShortcut(SlotIndex, Guid);
	if (Displaced && Displaced != Guid && SourceShortcut != INDEX_NONE)
	{
		Client->SendAddShortcut(SourceShortcut, Displaced);
	}
	else if (Displaced && Displaced != Guid)
	{
		for (int32 Offset = 1; Offset < 18; ++Offset)
		{
			const int32 Index = (SlotIndex + Offset) % 18;
			if (!Client->GetShortcutObject(Index))
			{
				Client->SendAddShortcut(Index, Displaced);
				break;
			}
		}
	}
	RefreshShortcutOverlays();
	RefreshInventoryOverlays();
}

bool UACEUIGameplayBinder::TryFinishInventoryDrag(FVector2D CanvasLocalPos)
{
	if (!bInvDragPending || InvDragGuid == 0)
	{
		return false;
	}
	const int32 Guid = InvDragGuid;
	FACEWorldObject DraggedItem;
	const bool bHaveDraggedItem = Client && Client->GetWorldObject(Guid, DraggedItem);
	const int32 Amount = bHaveDraggedItem ? FMath::Clamp(InvDragAmount > 0 ? InvDragAmount : GetSelectedItemAmount(Guid),
		1, FMath::Max(1, DraggedItem.StackSize)) : 0;
	const int32 SourcePack = InvDragSourcePack;
	const int32 DragPackSlot = InvDragPackSlotIndex;
	const int32 ShortcutSource = InvDragShortcutSlot;
	const bool bWasDragging = bInvDragActive;
	const bool bDoubleClick = bInvDoubleClickPending;
	const bool bVendorSellSource = bInvDragFromVendorSell;
	const bool bSalvageSource = bInvDragFromSalvage;
	const bool bHouseSource = bInvDragFromHouse;
	const bool bLootSource = OpenLootContainerGuid != 0
		&& (SourcePack == OpenLootContainerGuid || SourcePack == OpenLootSelectedPackGuid);
	CancelInventoryDrag();

    if(bWasDragging&&Client&&Canvas)
        if(auto* GI=Client->GetGameInstance())
            if(auto* Deck=GI->GetSubsystem<UACEActionDeckSubsystem>();Deck
                &&Deck->DropAt(Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos),Guid))
                return true;

	// A swap may have begun before mouse-down. Never submit a second weapon
	// relocation while its dequip/wield acknowledgments are still outstanding.
	if(Client && (bWasDragging||bDoubleClick))if(auto Session=Client->GetSession();Session&&Session->IsEquipmentSwapPending())
	{AppendLocalChatLine(TEXT("Wait for the equipment change to finish, then move the item."),ACEChatMessageType::TransientInfo);return true;}

	if (!Client)
	{
		return true;
	}

	if (!Canvas || !Canvas->GetElementLayer())
	{
		return true;
	}
	const FVector2D Absolute = Canvas->GetCachedGeometry().LocalToAbsolute(CanvasLocalPos);

	if (bHouseSource)
	{
		if (bDoubleClick || (bWasDragging && !HitHouseItemList(CanvasLocalPos)))
		{
			if (auto S=Client->GetSession()) S->RemoveHousePayment(Guid,bHouseRentTab);
			LastInvClickGuid=0; LastInvClickTime=0;
		}
		else if (!bWasDragging) { LastInvClickGuid=Guid; LastInvClickTime=FPlatformTime::Seconds(); }
		RefreshHouseOffer(); return true;
	}
	if(bSalvageSource)
	{
		const auto List=Manager?Manager->FindElementByName(TEXT("SalvageItemsList")):nullptr;
		const bool InList=List && Canvas->IsElementExposedAt(List,CanvasLocalPos);
		if(bDoubleClick || (bWasDragging && !InList))
		{RemoveItemFromSalvageQueue(Guid);LastInvClickGuid=0;LastInvClickTime=0;}
		else if(!bWasDragging){LastInvClickGuid=Guid;LastInvClickTime=FPlatformTime::Seconds();}
		RefreshSalvageOverlays();return true;
	}
	if (bVendorSellSource)
	{
		const auto SellPanel=Manager ? Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field")) : nullptr;
		const bool bInVendor=SellPanel && FBox2D(FVector2D(SellPanel->GetScreenOrigin()),
			FVector2D(SellPanel->GetScreenOrigin())+FVector2D(SellPanel->Width,SellPanel->Height)).IsInside(CanvasLocalPos);
		if (bDoubleClick || (bWasDragging && !bInVendor))
		{
			VendorSellCart.RemoveAll([Guid](const auto& Entry){return Entry.Value==Guid;});
			if (VendorSellSelectedGuid==Guid) VendorSellSelectedGuid=0;
			LastInvClickGuid=0; LastInvClickTime=0;
		}
		else if (!bWasDragging) { LastInvClickGuid=Guid; LastInvClickTime=FPlatformTime::Seconds(); }
		RefreshVendorOverlays(); RefreshInventoryOverlays(); return true;
	}

	auto RecordClick = [this, Guid, ShortcutSource]()
	{
		const double Now = FPlatformTime::Seconds();
		LastInvClickGuid = Guid;
		LastInvClickTime = Now;
		if (ShortcutSource == INDEX_NONE || !ShouldPreserveShortcutSelection(Guid)) SelectInventoryGuid(Guid);
	};

	if (!bWasDragging)
	{
		// A shortcut target click (including the main backpack) must not select
		// the target as a side effect, even if this happens to be a double-click.
		if (ShortcutSource != INDEX_NONE && TryCompletePendingUseWithTarget(Guid))
		{
			LastInvClickGuid = 0;
			LastInvClickTime = 0.0;
			return true;
		}
		// True double-click (second press released without drag) → Use / wield / arm dual-use.
		if (bDoubleClick && Guid != 0)
		{
			if (ShortcutSource == INDEX_NONE) SelectInventoryGuid(Guid);
			if (bLootSource)
			{
				PickupInventoryAmount(Guid, Amount);
				PlayLocalPickupMotion(PlayerController);
			}
			else UseInventoryItem(Guid);
			LastInvClickGuid = 0;
			LastInvClickTime = 0.0;
			return true;
		}
		// Dual-use (mana stone / key): MouseDown starts a pending drag so OverlayClick never
		// runs — complete UseWithTarget on the click release instead.
		if ((PendingUseWithSourceGuid != 0 || (PlayerController && PlayerController->GetObjectCursorTargeting())) && Guid != 0 && Client)
		{
			if (PendingUseWithSourceGuid != Guid)
			{
				TryCompletePendingUseWithTarget(Guid);
				return true;
			}

		}
		// Single click release — arm double-click for the next MouseDown.
		// Side-pack click already selected the pack on MouseDown.
		if (DragPackSlot != INDEX_NONE && Guid != 0)
		{
			SelectedPackGuid = Guid;
			InventoryScrollOffset = 0;
			RefreshInventoryOverlays();
			SyncInventoryScrollbars();
		}
		RecordClick();
		return true;
	}

	// Both authored rows accept inventory items and shortcut-to-shortcut swaps.
	if (HitHouseItemList(CanvasLocalPos))
	{
		StageHouseItem(Guid,Amount); RefreshHouseOffer(); return true;
	}
	if (const int32 Destination = HitTestShortcutSlot(CanvasLocalPos); Destination != INDEX_NONE)
	{
		AssignInventoryShortcut(Guid, Destination, ShortcutSource);
		return true;
	}
	if (!bHaveDraggedItem) return true;
	if (Client->GetTradeSelfItems().Contains(Guid))
	{
		PostInventorySystemMessage(TEXT("Remove that item from the trade before moving it."));
		return true;
	}

	if (ShortcutSource != INDEX_NONE)
	{
		// Dragging a shortcut out of the toolbar removes its binding, never the item.
		Client->SendRemoveShortcut(ShortcutSource);
		RefreshShortcutOverlays();
		RefreshInventoryOverlays();
		return true;
	}

	// gmToolbarUI::HandleDrop: the large toolbar pack accepts items even when
	// the inventory window is closed. Shortcuts are handled above, not relocated.
	if (Manager && Canvas->IsElementExposedAt(Manager->FindElementByName(TEXT("InventoryButton")), CanvasLocalPos))
	{
		if (Guid != Client->GetPlayerGuid()) MoveInventoryAmount(Guid, Client->GetPlayerGuid(), 0, Amount);
		return true;
	}

	// Drop onto open vendor UI → stage the item or a pack's contents in the Sell cart.
	if (OpenVendorGuid != 0 && Manager)
	{
		auto HitVendorUi = [&](const TCHAR* Name) -> bool
		{
			TSharedPtr<FACEUIElement> El = Manager->FindElementByName(Name);
			// Exposure checks already convert viewport coordinates to DAT layout units.
			return Canvas->IsElementExposedAt(El, CanvasLocalPos);
		};
		if (HitVendorUi(TEXT("Vendor")) || HitVendorUi(TEXT("VendorPanel"))
			|| HitVendorUi(TEXT("RootGameplay_FloatyEnvPanel_Field")))
		{
			AddInventoryGuidToVendorSellCart(Guid, Amount);
			return true;
		}
	}


	// Main backpack is the player GUID, but its contents are also a valid bulk
	// salvage selection. Dispatch this before excluding physical player moves.
	if (OpenSalvageToolGuid != 0 && Manager)
	{
		const auto List = Manager->FindElementByName(TEXT("SalvageItemsList"));
		if (List && Canvas->IsElementExposedAt(List, CanvasLocalPos))
		{
			AddItemToSalvageQueue(Guid);
			return true;
		}
	}
	// Retail ItemHolder::AttemptPlaceIn3D permits the main backpack as a bulk
	// vendor offer. Resolve that world drop before excluding physical player moves.
	if (Guid == Client->GetPlayerGuid())
		return IsPointerOverInventoryPanel(CanvasLocalPos) ? true : DropInventoryInWorld(Guid, Amount, Absolute);

	// Paper doll → wield into that slot (or complete dual-use / mana stone on self).
	FString DollName;
	int64 DollMask = 0;
	if (HitTestDollSlot(Absolute, DollName, DollMask))
	{
		FACEWorldObject Obj;
		if (!Client->GetWorldObject(Guid, Obj))
		{
			return true;
		}
		if (PendingUseWithSourceGuid != 0 || (Obj.ItemType & ACEItemType::ManaStone) != 0
			|| ACEItemUseable::IsTargeted(Obj.ItemUseable))
		{
			const int32 Target = ResolvePaperDollUseTarget(DollName, DollMask);
			if (!Target) return true; // An empty equipment slot is not a request to refill everything.
			if (!PendingUseWithSourceGuid) PendingUseWithSourceGuid = Guid;
			TryCompletePendingUseWithTarget(Target);
			return true;
		}
		// AcceptPaperDollDragObject auto-wears body clothing anywhere on the model.
		if (DollName == TEXT("PaperDoll")) DollMask = 0x08007FFF;
		constexpr int64 ClothesLegs = ACEEquipMask::UpperLegWear | ACEEquipMask::LowerLegWear;
		constexpr int64 Foot = ACEEquipMask::FootWear;
		constexpr int64 ShirtCore = ACEEquipMask::ChestWear
			| ACEEquipMask::UpperArmWear | ACEEquipMask::LowerArmWear;
		constexpr int64 ArmorBits = ACEEquipMask::ChestArmor | ACEEquipMask::AbdomenArmor
			| ACEEquipMask::UpperArmArmor | ACEEquipMask::LowerArmArmor
			| ACEEquipMask::UpperLegArmor | ACEEquipMask::LowerLegArmor;
		// Boots belong on Inv_FootSlot only; pants belong on Inv_ClothesPantsSlot only.
		if (DollMask == ACEEquipMask::UpperLegWear && (Obj.ValidLocations & Foot) != 0)
		{
			PostInventorySystemMessage(TEXT("That item cannot be equipped there."));
			return true;
		}
		if (DollMask == Foot && (Obj.ValidLocations & ClothesLegs) != 0
			&& (Obj.ValidLocations & Foot) == 0)
		{
			PostInventorySystemMessage(TEXT("That item cannot be equipped there."));
			return true;
		}
		if (DollMask == ACEEquipMask::ChestWear && (Obj.ValidLocations & ArmorBits) != 0
			&& (Obj.ValidLocations & ShirtCore) == 0)
		{
			PostInventorySystemMessage(TEXT("That item cannot be equipped there."));
			return true;
		}
		// Trousers → undershirt reject.
		if (DollMask == ACEEquipMask::ChestWear
			&& (Obj.ValidLocations & ClothesLegs) != 0
			&& (Obj.ValidLocations & Foot) == 0
			&& (Obj.ValidLocations & ShirtCore) == 0)
		{
			PostInventorySystemMessage(TEXT("That item cannot be equipped there."));
			return true;
		}
		if (Obj.ValidLocations != 0 && !ACEEquipmentRules::CanWieldInSlot(Obj, DollMask))
		{
			PostInventorySystemMessage(TEXT("That item cannot be equipped there."));
			return true;
		}
		const int64 Loc = ResolveWieldLocation(Obj, DollMask);
		if (Loc != 0)
		{
			const int32 Stack = Obj.StackSize > 0 ? Obj.StackSize : 1;
			const int32 Amt = Amount;
			if (Stack > 1 && Amt < Stack)
			{
				TrackInventoryStackSplit(InventorySelectionSplit, Obj, Amt);
				InventorySelectionSplit.WieldLocation = Loc;
				Client->SendStackableSplitToWield(Guid, Loc, Amt);
			}
			else
			{
				UnequipConflictsAndWield(Guid, Obj, Loc);
			}
		}
		else
		{
			Client->SendUseItem(Guid);
		}
		return true;
	}

	// Drop onto open trade UI → offer that item on our side of the window.
	if (bTradeOpen && Manager)
	{
		auto HitTradeUi = [&](const TCHAR* Name) -> bool
		{
			TSharedPtr<FACEUIElement> El = Manager->FindElementByName(Name);
			return Canvas->IsElementExposedAt(El, CanvasLocalPos);
		};
		if (HitTradeUi(TEXT("SecureTrade")) || HitTradeUi(TEXT("TradePanel"))
			|| HitTradeUi(TEXT("TradeSelfItemsList")) || HitTradeUi(TEXT("TradeOtherItemsList"))
			|| HitTradeUi(TEXT("RootGameplay_FloatyEnvPanel_Field")))
		{
			AddInventoryGuidToTrade(Guid, Amount);
			return true;
		}
	}


	// Pack tab → move into that pack (or rearrange side packs onto player placement).
	int32 PackGuid = 0;
	if (HitTestPackSlot(Absolute, PackGuid, true))
	{
		FACEWorldObject SourceBag;
        const bool HaveSourceBag = Client->GetWorldObject(Guid, SourceBag);
		if (DragPackSlot != INDEX_NONE || (HaveSourceBag && ((SourceBag.ItemType & ACEItemType::Container) != 0
            || (SourceBag.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot) != 0)))
		{
			int32 DestPlacement = 0;
			for (int32 i = 0; i < PackSlots.Num() && i < PackSlotGuids.Num(); ++i)
			{
				UBorder* Border = PackSlots[i];
				if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
				{
					continue;
				}
				if (Canvas->IsWidgetExposedAt(Border, Absolute))
				{
					DestPlacement = (i == 0) ? 0 : (PackScrollOffset + (i - 1));
					break;
				}
			}
            if (HaveSourceBag && SourceBag.ContainerId == Client->GetPlayerGuid() && SourceBag.PlacementPosition >= 0
                && SourceBag.PlacementPosition < DestPlacement) --DestPlacement;
            if (PackGuid != Guid) MoveInventoryAmount(Guid, Client->GetPlayerGuid(), FMath::Max(0, DestPlacement), Amount);
			return true;
		}
		if (PackGuid != 0)
		{
			MoveInventoryAmount(Guid, PackGuid, 0, Amount);
			if (bLootSource)
			{
				PlayLocalPickupMotion(PlayerController);
			}
			return true;
		}
	}

	// Grid cell → reorder / merge / move within open pack.
	int32 DestGuid = 0;
	int32 DestSlot = INDEX_NONE;
	if (HitTestInventorySlot(Absolute, DestGuid, DestSlot))
	{
		// UIElement_ItemList::PlaceItem rejects containers in the ordinary grid.
		// Use the backpack button or the separate pack row for portable packs.
		if (ACEInventoryRules::IsContainer(DraggedItem) || (DraggedItem.ItemType & ACEItemType::Container)
			|| (DraggedItem.ObjectDescriptionFlags & ACEObjectDescFlag::RequiresPackSlot))
		{
			PostInventorySystemMessage(TEXT("Cannot place container in item list."));
			return true;
		}
		// Returning a drag to its source cell cancels it; it cannot arm a double-click.
		if (DestGuid == Guid)
		{
			LastInvClickGuid = 0;
			LastInvClickTime = 0.0;
			return true;
		}
		const int32 Container = SelectedPackGuid != 0 ? SelectedPackGuid
			: (SourcePack != 0 ? SourcePack : Client->GetPlayerGuid());
		const int32 InsertSlot = ResolveInventoryDropPlacement(Guid, Amount, Container, DestGuid);
		const auto FinishLootPut = [&]()
		{
			if (bLootSource)
			{
				PlayLocalPickupMotion(PlayerController);
			}
		};
		if (InsertSlot != INDEX_NONE && !MergeInventoryAmount(Guid, DestGuid, Amount))
			MoveInventoryAmount(Guid, Container, InsertSlot, Amount);
		FinishLootPut();
		return true;
	}

	if (TryDropInExternalContainer(Guid, Amount, Absolute)) return true;

	if (!IsPointerOverInventoryPanel(CanvasLocalPos)) return DropInventoryInWorld(Guid, Amount, Absolute);

	// A cancelled drag cannot arm a subsequent double-click use.
	LastInvClickGuid = 0;
	LastInvClickTime = 0.0;
	return true;
}

bool UACEUIGameplayBinder::DropInventoryInWorld(int32 Guid, int32 Amount, const FVector2D& Absolute)
{
	FACEWorldObject Obj;
	if (!Client->GetWorldObject(Guid, Obj))
	{
		return true;
	}

	if (PlayerController && PlayerController->IsPickupApproachActive())
	{
		PostInventorySystemMessage(TEXT("You're too busy!"));
		return true;
	}

	if (PlayerController && Canvas)
	{
		// Drag release under mouse capture: convert slate absolute → viewport local for deproject.
		// Deprojection needs viewport pixels, not DPI-scaled widget coordinates.
		FVector2D ViewportLocal = FVector2D::ZeroVector, ViewportWidgetPosition;
		if (!PlayerController->IsVRActive()) USlateBlueprintLibrary::AbsoluteToViewport(PlayerController, Absolute,
			ViewportLocal, ViewportWidgetPosition);
		// Resolve the release ray only. Captured drags leave the cached cursor hit
		// at the press location; a miss means ground, never the old selection.
		AACEWorldEntityActor* Target = PlayerController->PickWorldEntityAtScreenPosition(ViewportLocal.X, ViewportLocal.Y);

		auto ResolveGiveTargetObj = [&](AACEWorldEntityActor* Candidate, FACEWorldObject& OutObj) -> bool
		{
			if (!Candidate)
			{
				return false;
			}
			return Client->GetWorldObject(Candidate->GetACEGuid(), OutObj)
				&& OutObj.IsGiveOrCreatureTarget();
		};

		FACEWorldObject TargetObj;
		bool bGiveTarget = ResolveGiveTargetObj(Target, TargetObj);


		if (Target || bGiveTarget)
		{
			const int32 TargetGuid = bGiveTarget ? TargetObj.Guid
				: (Target ? Target->GetACEGuid() : 0);
			const bool bHaveTarget = TargetGuid != 0
				&& (bGiveTarget || Client->GetWorldObject(TargetGuid, TargetObj));
			// Main backpack is a bulk vendor selection, never the player object to give or move.
			if (Guid == Client->GetPlayerGuid() && (!bHaveTarget || !TargetObj.IsVendor())) return true;
			if (bHaveTarget && TargetGuid == Client->GetPlayerGuid())
			{
				if ((Obj.ItemType & ACEItemType::ManaStone) || ACEItemUseable::IsTargeted(Obj.ItemUseable))
				{
					if (!PendingUseWithSourceGuid) PendingUseWithSourceGuid = Guid;
					TryCompletePendingUseWithTarget(TargetGuid);
				}
				else MoveInventoryAmount(Guid, TargetGuid, 0, Amount);
				return true;
			}
			if (bHaveTarget && PlayerController->IsVRActive())
			{
				const APawn* Pawn = PlayerController->GetPawn();
				const auto* Capsule = Pawn ? Pawn->FindComponentByClass<UCapsuleComponent>() : nullptr;
				const float Radius = TargetObj.UseRadius > 0.f ? TargetObj.UseRadius : .6f;
				if (!Capsule || PlayerController->GetUseCylinderDistanceCm(TargetObj,
					Pawn->GetActorLocation() - FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight()),
					TargetObj.Position.ToUnrealLocation(PlayerController->WorldScale)) > Radius * PlayerController->WorldScale + 5.f)
				{
					PostInventorySystemMessage(TEXT("Move closer before giving or using an item on that target."));
					return true;
				}
			}
			if (bHaveTarget && TargetObj.IsVendor())
			{
				if (OpenVendorGuid == TargetGuid)
				{
					AddInventoryGuidToVendorSellCart(Guid, Amount);
					return true;
				}
				if (Client->IsUseBusy())
				{
					PostInventorySystemMessage(TEXT("You're too busy!"));
					return true;
				}
				PendingVendorSellTargetGuid = TargetGuid;
				PendingVendorSellGuid = Guid;
				PendingVendorSellAmount = Amount;
				PlayerController->InteractWithObject(TargetGuid);
				return true;
			}
			if (bHaveTarget && TryOpenTradeForDraggedItem(TargetObj, Guid, Amount)) return true;
			if (bHaveTarget && MergeInventoryAmount(Guid, TargetGuid, Amount)) return true;
			if (bHaveTarget && !bGiveTarget && (TargetObj.ItemType & ACEItemType::Container))
			{
				if (TargetGuid == OpenLootContainerGuid || TargetGuid == OpenLootSelectedPackGuid)
					MoveInventoryAmount(Guid, TargetGuid, 0, Amount);
				else PostInventorySystemMessage(TEXT("You must open the container first."));
				return true;
			}
			if (bGiveTarget)
			{
				const int32 Amt = Amount;
				// Give is not a vendor sell — clear any leftover sell cart / pending drag-sell.
				PendingVendorSellGuid = PendingVendorSellTargetGuid = 0;
				VendorSellCart.Reset();
				VendorSellSelectedGuid = 0;
				// Approach without Use — server CreateMoveToChain + GiveObjectRequest completes hand-off.
				float DistAc = TargetObj.UseRadius;
				if (DistAc == 0.f)
				{
					DistAc = 0.6f;
				}
				PlayerController->BeginUseApproach(TargetGuid, DistAc,
					/*bPickupIntoInventory*/ false, /*bFireActionWhenInRange*/ false);
				Client->SendGiveObjectRequest(TargetGuid, Guid, Amt);
				return true;
			}
			// Drag item onto world object → UseWithTarget (keys, dyes, alchemy).
			// Never UseWithTarget for NPCs/creatures — those are GiveObjectRequest turn-ins.
			if (TargetGuid != 0
				&& !TargetObj.IsGiveOrCreatureTarget()
				&& (TargetObj.ItemType & ACEItemType::Creature) == 0
				&& !TargetObj.bIsPlayer)
			{
				Client->SendUseWithTarget(Guid, TargetGuid);
				PendingUseWithSourceGuid = 0;
				SyncPendingUseCursor();
				return true;
			}
			// Creature/NPC that failed give classification — still attempt Give.
			if (TargetGuid != 0
				&& ((TargetObj.ItemType & ACEItemType::Creature) != 0
					|| TargetObj.bIsPlayer || TargetObj.IsVendor()))
			{
				const int32 Amt = Amount;
				PendingVendorSellGuid = PendingVendorSellTargetGuid = 0;
				VendorSellCart.Reset();
				VendorSellSelectedGuid = 0;
				float DistAc = TargetObj.UseRadius;
				if (DistAc == 0.f)
				{
					DistAc = 0.6f;
				}
				PlayerController->BeginUseApproach(TargetGuid, DistAc,
					/*bPickupIntoInventory*/ false, /*bFireActionWhenInRange*/ false);
				Client->SendGiveObjectRequest(TargetGuid, Guid, Amt);
				return true;
			}
		}
	}

	if (Guid == Client->GetPlayerGuid() || !Client->IsOwnedInventoryItem(Obj)) return true;

	// Releasing a tracked inventory drag over empty world uses the same
	// server-authoritative drop/split action as the desktop client.
	DropInventoryAmount(Guid,Amount);
	return true;
}

bool UACEUIGameplayBinder::TryHandleOverlayClick(FVector2D CanvasLocalPos, bool bRightClick)
{
	if (!Canvas || !Canvas->GetElementLayer())
	{
		return false;
	}
	const FGeometry LayerGeo = Canvas->GetElementLayer()->GetCachedGeometry();
	const FVector2D Absolute = LayerGeo.LocalToAbsolute(CanvasLocalPos);
	if (bRightClick && InspectSpellAt(CanvasLocalPos)) return true;
	if (bRightClick && Client)
		if (const int32 Index = HitTestShortcutSlot(CanvasLocalPos); Index != INDEX_NONE)
		{
			const int32 Guid = Client->GetShortcutObject(Index);
			if (Guid) { SelectInventoryGuid(Guid); Client->SendIdentifyObject(Guid); }
			return true;
		}

	if (bRightClick && Client && ActivePanelPage == TEXT("InventoryPanel_Field"))
	{
		FString SlotName;
		int64 Mask = 0;
		if (HitTestDollSlot(Absolute, SlotName, Mask))
		{
			const int32 Guid = FindUpperEquippedItem(Mask);
			if (Guid) { SelectInventoryGuid(Guid); Client->SendIdentifyObject(Guid); }
			return true;
		}
	}
	if (!bRightClick && Manager && CanEditInscription())
	{
		const auto Element = Manager->FindElementUnder(TEXT("RootGameplay_FloatyExamination_Field"), TEXT("ItemInscriptionText"));
		const FVector2D EditPoint = Canvas->ViewportToLayout(CanvasLocalPos);
		const FIntPoint EditOrigin = Element ? Element->GetScreenOrigin() : FIntPoint::ZeroValue;
		if (Element && Canvas->IsElementExposedAt(Element, CanvasLocalPos)
			&& EditPoint.X >= EditOrigin.X && EditPoint.Y >= EditOrigin.Y
			&& EditPoint.X < EditOrigin.X + Element->Width && EditPoint.Y < EditOrigin.Y + Element->Height)
		{
			BeginInscriptionEdit();
			return true;
		}
	}
	if (bTradeOpen)
	{
		for (bool bSelf : {true, false})
		{
			const auto& Slots = bSelf ? TradeSelfSlots : TradeOtherSlots;
			const auto& Guids = bSelf ? TradeSelfGuids : TradeOtherGuids;
			for (int32 I=0; I<Slots.Num() && I<Guids.Num(); ++I)
				if (Guids[I] && Slots[I] && Canvas->IsWidgetExposedAt(Slots[I], Absolute))
				{
					SelectInventoryGuid(Guids[I]);
					if (bRightClick && Client) Client->SendIdentifyObject(Guids[I]);
					RefreshTradeOverlays();
					return true;
				}
		}
	}

	// Show Equipment checkbox (PaperDollDragMask used to steal this hit).
	if (!bRightClick && Manager && ActivePanelPage == TEXT("InventoryPanel_Field"))
	{
		TSharedPtr<FACEUIElement> Cb = Manager->FindElementUnder(
			TEXT("InventoryPanel_Field"), TEXT("Paperdoll_Slots_Checkbox"));
		if (!Cb.IsValid())
		{
			Cb = Manager->FindElementByName(TEXT("Paperdoll_Slots_Checkbox"));
		}
		if (Cb.IsValid() && Cb->bVisible && Canvas->IsElementExposedAt(Cb, CanvasLocalPos))
		{
			const FIntPoint O = Cb->GetScreenOrigin();
			if (CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
				&& CanvasLocalPos.X < O.X + Cb->Width && CanvasLocalPos.Y < O.Y + Cb->Height)
			{
				bShowPaperdollSlots = !bShowPaperdollSlots;
				RefreshInventoryOverlays();
				return true;
			}
		}
	}

	// Dual-use targeting: click equipped doll slot (without starting an inv drag).
	if (!bRightClick && (PendingUseWithSourceGuid != 0 || (PlayerController && PlayerController->GetObjectCursorTargeting())) && Client)
	{
		FString DollName;
		int64 DollMask = 0;
		if (HitTestDollSlot(Absolute, DollName, DollMask))
		{
			TryCompletePendingUseWithTarget(ResolvePaperDollUseTarget(DollName, DollMask));
			return true;
		}
	}

	if (!bRightClick && TryHandleChatTargetPopupClick(Absolute))
	{
		return true;
	}
	if (!bRightClick && TryHandleChatNameClick(Absolute))
	{
		return true;
	}
	if (!bRightClick && TryHandleChessBoardClick(CanvasLocalPos))
	{
		return true;
	}
	if (!bRightClick && TryHandleEffectsListClick(Absolute))
	{
		return true;
	}
	if (!bRightClick && TryHandleOptionsListClick(CanvasLocalPos))
	{
		return true;
	}
	if (TryHandleComponentListClick(CanvasLocalPos, bRightClick))
	{
		return true;
	}
	if (!bRightClick && ActivePanelPage == TEXT("QuestManagementPanel_Field"))
	{
		for (int32 i = 0; i < QuestRows.Num() && i < QuestRowIds.Num(); ++i)
		{
			UTextBlock* Row = QuestRows[i];
			if (!Row || Row->GetVisibility() == ESlateVisibility::Collapsed || QuestRowIds[i] == 0)
			{
				continue;
			}
			bool bJournalRowHit=false;
			if (ActiveQuestTab==TEXT("PageListPage") && JournalRowElements.IsValidIndex(i))
			{
				const auto Entry=JournalRowElements[i];
				if(Entry && Entry->bVisible && Canvas->IsElementExposedAt(Entry,CanvasLocalPos))
				{
					const FIntPoint O=Entry->GetScreenOrigin();
					bJournalRowHit=CanvasLocalPos.X>=O.X && CanvasLocalPos.X<O.X+Entry->Width
						&& CanvasLocalPos.Y>=O.Y && CanvasLocalPos.Y<O.Y+Entry->Height;
				}
			}
			if (bJournalRowHit || Canvas->IsWidgetExposedAt(Row, Absolute)
				|| (QuestStatusRows.IsValidIndex(i) && Canvas->IsWidgetExposedAt(QuestStatusRows[i], Absolute)))
			{
				if (ActiveQuestTab == TEXT("PageListPage"))
				{
					JournalPageIndex = -QuestRowIds[i]-1;
					const double Now=FPlatformTime::Seconds();
					if (LastJournalClickPage==JournalPageIndex && Now-LastJournalClickTime<.4)
						ActiveQuestTab = TEXT("JournalPage");
					LastJournalClickPage=JournalPageIndex; LastJournalClickTime=Now; bJournalFieldsDirty=true;
				}
				else SelectedContractId = QuestRowIds[i];
				RefreshQuestOverlays();
				return true;
			}
		}
	}
	// Right-click ChatTarget cycles the log filter (All / Speech / Combat / System).
	if (bRightClick && Manager)
	{
		TSharedPtr<FACEUIElement> Target = Manager->FindElementByName(TEXT("ChatTarget"));
		TSharedPtr<FACEUIElement> TargetTxt = Manager->FindElementByName(TEXT("ChatTargetButtonText"));
		auto Hit = [&](const TSharedPtr<FACEUIElement>& El) -> bool
		{
			if (!El.IsValid() || !El->bVisible) { return false; }
			const FIntPoint O = El->GetScreenOrigin();
			return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
				&& CanvasLocalPos.X < O.X + El->Width && CanvasLocalPos.Y < O.Y + El->Height;
		};
		if (Hit(Target) || Hit(TargetTxt))
		{
			CycleChatFilterMode();
			return true;
		}
	}

	// Retail item-list right click appraises; double-click/drag removes an offer.
	if (OpenSalvageToolGuid != 0)
	{
		for (int32 i = 0; i < SalvageItemSlots.Num() && i < SalvageItemSlotGuids.Num(); ++i)
		{
			UBorder* Icon = SalvageItemSlots[i];
			if (!Icon || Icon->GetVisibility() == ESlateVisibility::Collapsed)
			{
				continue;
			}
			if (Canvas->IsWidgetExposedAt(Icon, Absolute))
			{
				const int32 Guid = SalvageItemSlotGuids[i];
				if (Guid == 0)
				{
					return true;
				}
				if (bRightClick)
				{
					SelectInventoryGuid(Guid);Client->SendIdentifyObject(Guid);
				}
				else
				{
					SelectInventoryGuid(Guid);
				}
				return true;
			}
		}
	}

	// Social fellowship / friends row selection.
	if (ActivePanelPage == TEXT("SocialPanel_Field") && !bRightClick)
	{
		if (ActiveSocialTab == TEXT("FellowshipPage"))
		{
			for (int32 i = 0; i < FellowRows.Num() && i < FellowRowGuids.Num(); ++i)
			{
				UTextBlock* Row = FellowRows[i];
				if (!Row || Row->GetVisibility() == ESlateVisibility::Collapsed)
				{
					continue;
				}
				if (FellowRowElements.IsValidIndex(i) && Canvas->IsElementExposedAt(FellowRowElements[i],CanvasLocalPos))
				{
					SelectedFellowGuid = FellowRowGuids[i];
					if(Client)Client->SelectObject(SelectedFellowGuid);
					RefreshFellowshipOverlays();
					return true;
				}
			}
		}
		if (ActiveSocialTab == TEXT("FriendsPage"))
		{
			for (int32 i = 0; i < FriendRows.Num() && i < FriendRowGuids.Num(); ++i)
			{
				UTextBlock* Row = FriendRows[i];
				if (!Row || Row->GetVisibility() == ESlateVisibility::Collapsed)
				{
					continue;
				}
				if (FriendRowElements.IsValidIndex(i) && Canvas->IsElementExposedAt(FriendRowElements[i], CanvasLocalPos))
				{
					SelectedFriendGuid = FriendRowGuids[i];
					RefreshFriendsOverlays();
					return true;
				}
			}
		}
		if (ActiveSocialTab == TEXT("SquelchPage"))
		{
			for (int32 i = 0; i < SquelchRows.Num() && i < SquelchRowGuids.Num(); ++i)
			{
				UTextBlock* Row = SquelchRows[i];
				if (!Row || Row->GetVisibility() == ESlateVisibility::Collapsed)
				{
					continue;
				}
				if (SquelchRowElements.IsValidIndex(i) && Canvas->IsElementExposedAt(SquelchRowElements[i],CanvasLocalPos))
				{
					SelectedSquelchGuid = SquelchRowGuids[i];
					SelectedSquelchName = SquelchRowNames.IsValidIndex(i)
						? SquelchRowNames[i] : FString();
					if (SquelchNameEntry && !SelectedSquelchName.IsEmpty())
					{
						SquelchNameEntry->SetText(FText::FromString(SelectedSquelchName));
					}
					RefreshSquelchOverlays();
					return true;
				}
			}
		}
		if (ActiveSocialTab == TEXT("AllegiancePage"))
		{
			for (int32 i = 0; i < VassalRows.Num() && i < VassalRowGuids.Num(); ++i)
			{
				UTextBlock* Row = VassalRows[i];
				if (!Row || Row->GetVisibility() == ESlateVisibility::Collapsed)
				{
					continue;
				}
				if (VassalRowElements.IsValidIndex(i) && Canvas->IsElementExposedAt(VassalRowElements[i], CanvasLocalPos))
				{
					SelectedVassalGuid = VassalRowGuids[i];
					RefreshAllegianceOverlays();
					return true;
				}
			}
		}
	}

	// Spellbook filter overlays — DAT School_/Level_ shells are easy to miss; hit UMG boxes too.
	if (!bRightClick && ActivePanelPage == TEXT("SpellManagementPanel_Field")
		&& ActiveSpellPanelTab == TEXT("SpellbookPage"))
	{
		static const TCHAR* FilterNames[] = {
			TEXT("School_War"), TEXT("School_Life"), TEXT("School_Item"), TEXT("School_Creature"),
			TEXT("School_Void"), TEXT("Level_One"), TEXT("Level_Two"), TEXT("Level_Three"),
			TEXT("Level_Four"), TEXT("Level_Five"), TEXT("Level_Six"), TEXT("Level_Seven"),
			TEXT("Level_Eight"),
		};
		for (int32 i = 0; i < SpellbookFilterCheckboxes.Num() && i < UE_ARRAY_COUNT(FilterNames); ++i)
		{
			UBorder* Box = SpellbookFilterCheckboxes[i];
			if (!Box || Box->GetVisibility() == ESlateVisibility::Collapsed)
			{
				continue;
			}
			if (Canvas->IsWidgetExposedAt(Box, Absolute))
			{
				return ToggleSpellbookFilter(FilterNames[i]);
			}
		}
	}

    // Retail picks the nearest dot within six authored pixels, not the first
    // rectangular widget encountered. Keep that usable tolerance as dots shrink.
    if(Manager && Canvas && Client)
    {
        auto Radar=Manager->FindElementByName(TEXT("RadarImage"));
        const FVector2D Local=Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute);
        const FVector2D Scale=Canvas->GetLastScale2D();
        const FVector2D P=Local/Scale;
        if(Radar && Canvas->IsElementExposedAt(Radar,Local))
        {
            const FIntPoint O=Radar->GetScreenOrigin();
            if(P.X>=O.X && P.Y>=O.Y && P.X<O.X+Radar->Width && P.Y<O.Y+Radar->Height)
            {
                int32 BestGuid=0; double BestDistance=36.01;
                for(int32 I=0;I<RadarBlips.Num() && I<RadarBlipGuids.Num();++I)
                {
                    const auto* Blip=RadarBlips[I].Get();if(!Blip || !Blip->IsVisible() || !RadarBlipGuids[I])continue;
                    const auto& G=Blip->GetCachedGeometry();
                    const FVector2D D=(G.AbsoluteToLocal(Absolute)-G.GetLocalSize()*.5)/Scale;
                    if(D.SizeSquared()<BestDistance){BestDistance=D.SizeSquared();BestGuid=RadarBlipGuids[I];}
                }
                if(BestGuid){Client->SelectObject(BestGuid);if(bRightClick)Client->SendIdentifyObject(BestGuid);return true;}
            }
        }
    }

	if (!bRightClick && Manager && ActivePanelPage == TEXT("SkillManagementPanel_Field"))
	{
		// Raise / Raise×10 sit outside the list overlays — hit-test DAT buttons explicitly
		// so XP spend still works if element activation misses a duplicate footer.
		auto FindNamedUnder = [](const TSharedPtr<FACEUIElement>& Root, const FString& Name) -> TSharedPtr<FACEUIElement>
		{
			if (!Root.IsValid()) { return nullptr; }
			TArray<TSharedPtr<FACEUIElement>> Stack;
			Stack.Add(Root);
			while (Stack.Num() > 0)
			{
				TSharedPtr<FACEUIElement> Cur = Stack.Pop(EAllowShrinking::No);
				if (!Cur.IsValid()) { continue; }
				if (Cur->ElementName == Name && Cur->bVisible) { return Cur; }
				for (const TSharedPtr<FACEUIElement>& Child : Cur->Children)
				{
					Stack.Add(Child);
				}
			}
			return nullptr;
		};
		auto HitRaiseUnder = [&](const FString& Page, const FString& Footer, const FString& BtnName) -> bool
		{
			TSharedPtr<FACEUIElement> Btn;
			if (TSharedPtr<FACEUIElement> Foot = Manager->FindElementUnder(Page, Footer))
			{
				if (!Foot->bVisible) return false;
				Btn = FindNamedUnder(Foot, BtnName);
			}
			if (!Btn.IsValid() || !Btn->bVisible || Btn->Width <= 0 || Btn->Height <= 0)
			{
				return false;
			}
			const FIntPoint O = Btn->GetScreenOrigin();
			const int32 X = FMath::RoundToInt(CanvasLocalPos.X);
			const int32 Y = FMath::RoundToInt(CanvasLocalPos.Y);
			return X >= O.X && Y >= O.Y && X < O.X + Btn->Width && Y < O.Y + Btn->Height;
		};
		const FString Page = ActiveSkillTab;
		const FString Footer = GetStatSelectionFooter();
		if (HitRaiseUnder(Page, Footer, TEXT("StatManagement_Footer_RaiseButton")))
		{
			RaiseSelectedStat(1);
			return true;
		}
		if (HitRaiseUnder(Page, Footer, TEXT("StatManagement_Footer_Raise10Button")))
		{
			RaiseSelectedStat(10);
			return true;
		}

		if (ActiveSkillTab == TEXT("AttributePage"))
		{
			for (int32 i = 0; i < AttributeRowHighlights.Num(); ++i)
			{
				UBorder* Hi = AttributeRowHighlights[i];
				if (!Hi || Hi->GetVisibility() == ESlateVisibility::Collapsed)
				{
					continue;
				}
				if (Canvas->IsWidgetExposedAt(Hi, Absolute))
				{
					// gmAttributeUI::SetSelection toggles an already-selected row off.
					const int32 Row = i + AttributeScrollOffset;
					SelectedAttributeRow = SelectedAttributeRow == Row ? INDEX_NONE : Row;
					RefreshAttributeOverlays();
					return true;
				}
			}
		}
		else if (ActiveSkillTab == TEXT("SkillPage"))
		{
			auto HitSkillRow = [&](const TArray<TObjectPtr<UBorder>>& Borders) -> bool
			{
				for (int32 i = 0; i < Borders.Num() && i < SkillRowIds.Num(); ++i)
				{
					UBorder* Border = Borders[i];
					if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
					{
						continue;
					}
					if (Canvas->IsWidgetExposedAt(Border, Absolute))
					{
						if (SkillRowIds[i] != 0)
						{
							SelectedSkillId = (SelectedSkillId == SkillRowIds[i]) ? 0 : SkillRowIds[i];
							RefreshSkillOverlays();
						}
						return true;
					}
				}
				return false;
			};
			if (HitSkillRow(SkillRowHighlights) || HitSkillRow(SkillRowIcons))
			{
				return true;
			}
		}
		if (!bRightClick && ActiveSkillTab == TEXT("CharacterTitlePage") && Client)
		{
			const TArray<int32>& Titles = SortedTitleIds;
			for (int32 i = 0; i < TitleRows.Num(); ++i)
			{
				UTextBlock* Row = TitleRows[i];
				if (!Row || Row->GetVisibility() == ESlateVisibility::Collapsed)
				{
					continue;
				}
				if (Canvas->IsWidgetExposedAt(Row, Absolute))
				{
					const int32 TitleIndex = TitleListScrollOffset + i;
					if (Titles.IsValidIndex(TitleIndex))
					{
						SelectedTitleId = Titles[TitleIndex];
						RefreshTitleOverlays();
					}
					return true;
				}
			}
		}
	}

	for (int32 i = 0; i < InventorySlots.Num() && i < InventorySlotGuids.Num(); ++i)
	{
		UBorder* Border = InventorySlots[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			const int32 Guid = InventorySlotGuids[i];
			if (Guid != 0)
			{
				if (!bRightClick && ((PendingUseWithSourceGuid != 0 && PendingUseWithSourceGuid != Guid) || (PlayerController && PlayerController->GetObjectCursorTargeting())))
				{
					TryCompletePendingUseWithTarget(Guid);
					return true;
				}
				SelectInventoryGuid(Guid);
				if (bRightClick && Client)
				{
					Client->SendIdentifyObject(Guid);
				}
			}
			return true;
		}
	}
	for (int32 i = 0; i < ExtPackSlots.Num() && i < ExtPackGuids.Num(); ++i)
	{
		UBorder* Border = ExtPackSlots[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			const int32 Guid = ExtPackGuids[i];
			if (Guid != 0)
			{
				SelectInventoryGuid(Guid);
				if (bRightClick && Client) Client->SendIdentifyObject(Guid);
				else if (!bRightClick)
				{
					OpenLootSelectedPackGuid = Guid;
					ExtItemScrollOffset = 0;
					RefreshExternalContainerOverlays();
				}
			}
			return true;
		}
	}
	for (int32 i = 0; i < ExtItemSlots.Num() && i < ExtItemGuids.Num(); ++i)
	{
		UBorder* Border = ExtItemSlots[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			const int32 Guid = ExtItemGuids[i];
			if (Guid != 0 && Client)
			{
				SelectInventoryGuid(Guid);
				if (bRightClick)
				{
					Client->SendIdentifyObject(Guid);
				}
				else
				{
					const double Now = FPlatformTime::Seconds();
					constexpr double DoubleClickSeconds = 0.75;
					if (Guid == LastInvClickGuid && (Now - LastInvClickTime) < DoubleClickSeconds)
					{
						PickupInventoryAmount(Guid);
						PlayLocalPickupMotion(PlayerController);
						LastInvClickGuid = 0;
						LastInvClickTime = 0.0;
					}
					else
					{
						LastInvClickGuid = Guid;
						LastInvClickTime = Now;
					}
				}
			}
			return true;
		}
	}
	for (int32 i = 0; i < VendorItemSlots.Num() && i < VendorItemGuids.Num(); ++i)
	{
		UBorder* Border = VendorItemSlots[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			const int32 Guid = VendorItemGuids[i];
			if (Guid != 0 && Client)
			{
				if (bRightClick)
				{
					if (ActiveVendorPage == 2) VendorSellSelectedGuid = Guid;
					else VendorSelectedGuid = Guid;
					SelectInventoryGuid(Guid);
					Client->SendIdentifyObject(Guid);
					RefreshVendorOverlays();
				}
				else if (ActiveVendorPage == 2)
				{
					VendorSellSelectedGuid = Guid;
					SelectInventoryGuid(Guid);
					RefreshVendorOverlays();
				}
				else
				{
					VendorSelectedGuid = Guid;
					SelectInventoryGuid(Guid);
					const double Now = FPlatformTime::Seconds();
					constexpr double DoubleClickSeconds = 0.75;
					if (Guid == LastInvClickGuid && (Now - LastInvClickTime) < DoubleClickSeconds)
					{
						if (ActiveVendorPage == 1) BuyVendorCartItem(); else BuySelectedVendorItem();
						LastInvClickGuid = 0;
						LastInvClickTime = 0.0;
					}
					else
					{
						LastInvClickGuid = Guid;
						LastInvClickTime = Now;
						RefreshVendorOverlays();
					}
				}
			}
			return true;
		}
	}
	for (int32 i = 0; i < PackSlots.Num() && i < PackSlotGuids.Num(); ++i)
	{
		UBorder* Border = PackSlots[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			const int32 PackGuid = PackSlotGuids[i];
			if (bRightClick)
			{
				if (Client && PackGuid) { SelectInventoryGuid(PackGuid); Client->SendIdentifyObject(PackGuid); }
			}
			else
			{
				if (PackGuid == 0)
				{
					return true;
				}
				SelectedPackGuid = PackGuid;
				InventoryScrollOffset = 0;
				RefreshInventoryOverlays();
				SyncInventoryScrollbars();
			}
			return true;
		}
	}
	for (int32 i = 0; i < SpellBarIcons.Num() && i < SpellBarSpellIds.Num(); ++i)
	{
		UBorder* Border = SpellBarIcons[i];
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			if (!bRightClick)
			{
				SelectCombatSpellSlot(i + SpellHotbarScrollOffset);
				const int32 SpellId = SpellBarSpellIds[i];
				if (SpellId != 0)
				{
					// Retail: first click selects; second click within the double-click window casts.
					constexpr double DoubleClickSeconds = 0.75;
					const double Now = FPlatformTime::Seconds();
					if (i == LastSpellClickSlot && SpellId == LastSpellClickId
						&& (Now - LastSpellClickTime) < DoubleClickSeconds)
					{
						SelectCombatSpellSlot(i + SpellHotbarScrollOffset);
						if (!Client->SendCastSpell(SpellId))
						{
							PostInventorySystemMessage(Client->IsInPortalSpace() ? TEXT("You're too busy!") : TEXT("You must select a target for that spell."));
						}
						LastSpellClickSlot = INDEX_NONE;
						LastSpellClickId = 0;
						LastSpellClickTime = 0.0;
					}
					else
					{
						LastSpellClickSlot = i;
						LastSpellClickId = SpellId;
						LastSpellClickTime = Now;
					}
					RefreshSpellHotbarOverlays();
				}
			}
			return true;
		}
	}
	// BuiltInSpell icon overlays are HitTestInvisible — hit-test their bounds explicitly.
	if (!bRightClick && Client && BuiltInSpellId != 0 && BuiltInSpellIconBorders.Num() > 0)
	{
		if (UBorder* Bi = BuiltInSpellIconBorders[0])
		{
			if (Bi->GetVisibility() != ESlateVisibility::Collapsed
				&& Canvas->IsWidgetExposedAt(Bi, Absolute))
			{
				constexpr double DoubleClickSeconds = 0.75;
				const double Now = FPlatformTime::Seconds();
				const bool bWasSelected = SelectedCombatSpellSlot < 0;
				SelectCombatSpellSlot(-1);
				if (bWasSelected && BuiltInSpellId == LastSpellClickId
					&& (Now - LastSpellClickTime) < DoubleClickSeconds)
				{
					if (!Client->SendCastSpell(BuiltInSpellId, BuiltInCasterGuid))
					{
						PostInventorySystemMessage(Client->IsInPortalSpace() ? TEXT("You're too busy!") : TEXT("You must select a target for that spell."));
					}
					LastSpellClickSlot = INDEX_NONE;
					LastSpellClickId = 0;
					LastSpellClickTime = 0.0;
				}
				else
				{
					LastSpellClickSlot = -1;
					LastSpellClickId = BuiltInSpellId;
					LastSpellClickTime = Now;
				}
				RefreshSpellHotbarOverlays();
				return true;
			}
		}
	}
	for (const auto& Pair : PaperDollIcons)
	{
		UBorder* Border = Pair.Value;
		if (!Border || Border->GetVisibility() == ESlateVisibility::Collapsed)
		{
			continue;
		}
		if (Canvas->IsWidgetExposedAt(Border, Absolute))
		{
			for (const FDollSlotMap& Slot : GDollSlots)
			{
				if (Pair.Key != Slot.Name || !Client)
				{
					continue;
				}
				for (const FACEWorldObject& Obj : Client->GetEquippedItems())
				{
					if (ItemMatchesDollSlot(Obj, Slot.Mask))
					{
						SelectInventoryGuid(Obj.Guid);
						if (bRightClick)
						{
							Client->SendIdentifyObject(Obj.Guid);
						}
						return true;
					}
				}
			}
			return true;
		}
	}
	return false;
}

void UACEUIGameplayBinder::HandleChatMessage(const FString& Text, const FString& Sender, int32 ChatType, int32 FilterType)
{
	if (FilterType == INDEX_NONE) FilterType = ChatType;
	// gmSpewBoxUI::RecvNotice_DisplayFinalStringInfo consumes LogTextType 26,
	// including WeenieError/UseDone failures. Chat receives it independently only
	// if explicitly enabled in that window's filter (retail defaults exclude it).
	if (ACEChatMessageType::IsTransientNotice(ChatType))
	{
		ShowTransientInfo(Text);
		if (ChatType == ACEChatMessageType::TransientInfo) return;
	}

	if (!ChatLog)
	{
		EnsureOverlays();
	}
	if (!ChatLog || !Canvas || !Canvas->WidgetTree)
	{
		return;
	}

	// Local send already appended "You say, …" — ignore the server HearSpeech echo for self.
	if (Client && !Sender.IsEmpty() && ChatType == ACEChatMessageType::Speech)
	{
		FACEWorldObject Self;
		if (Client->GetWorldObject(Client->GetPlayerGuid(), Self) && !Self.Name.IsEmpty()
			&& (Sender.Equals(Self.Name, ESearchCase::IgnoreCase)
				|| Sender.Equals(TEXT("You"), ESearchCase::IgnoreCase)))
		{
			// Allow the explicit local "You" append through; drop only the server echo
			// that uses the character name.
			if (!Sender.Equals(TEXT("You"), ESearchCase::IgnoreCase))
			{
				return;
			}
		}
	}

	// Retail @filter global squelch — client-local errors always show.
	if (ChatType != ACEChatMessageType::ChatError
		&& ((GlobalChatTypeFilter & (1ull << (static_cast<uint32>(ChatType) & 63))) == 0
			|| (GlobalChatTypeFilter & (1ull << (static_cast<uint32>(FilterType) & 63))) == 0))
	{
		return;
	}

	FString Line;
	if (ACEChatMessageType::IsGlobalChannel(ChatType)
        || ChatType == ACEChatMessageType::Allegiance
        || ChatType == ACEChatMessageType::Channel || ChatType == ACEChatMessageType::ChannelSend
        || ChatType == ACEChatMessageType::Social || ChatType == ACEChatMessageType::SocialSend
        || ChatType == ACEChatMessageType::Fellowship || ChatType == ACEChatMessageType::Abuse
        || ChatType == ACEChatMessageType::Help)
	{
		// The channel decoder already includes the channel and speaker.
		Line = Text;
	}
	else if (ChatType == ACEChatMessageType::OutgoingTell)
	{
		Line = Sender.IsEmpty() ? Text : FString::Printf(TEXT("%s, \"%s\""), *Sender, *Text);
	}
	else if (ChatType == ACEChatMessageType::Tell)
	{
		Line = FString::Printf(TEXT("%s tells you, \"%s\""),
			Sender.IsEmpty() ? TEXT("Someone") : *Sender, *Text);
	}
	else if (ChatType == ACEChatMessageType::Emote)
	{
		// DAT ChatEmote strings are fragments ("wave." / "waves.") — Mag-nus prepends
		// "You " locally and "Name " for SoulEmote broadcasts.
		if (Sender.IsEmpty() || Sender.Equals(TEXT("You"), ESearchCase::IgnoreCase))
		{
			Line = Text.StartsWith(TEXT("You "), ESearchCase::CaseSensitive)
				? Text
				: FString::Printf(TEXT("You %s"), *Text);
		}
		else
		{
			Line = FString::Printf(TEXT("%s %s"), *Sender, *Text);
		}
	}
	else if (!Sender.IsEmpty())
	{
		if (Sender.Equals(TEXT("You"), ESearchCase::IgnoreCase))
		{
			Line = FString::Printf(TEXT("You say, \"%s\""), *Text);
		}
		else
		{
			Line = FString::Printf(TEXT("%s says, \"%s\""), *Sender, *Text);
		}
	}
	else
	{
		Line = Text;
	}
	if (ChatType == ACEChatMessageType::OutgoingTell)
	{
		const FString Key = Line.ToLower();
		const double NowSec = FPlatformTime::Seconds();
		if (Key == LastOutgoingTellLineKey && (NowSec - LastOutgoingTellLineTime) < 2.0)
		{
			return;
		}
		LastOutgoingTellLineKey = Key;
		LastOutgoingTellLineTime = NowSec;
	}
	// Retail AddTextToScroll: "Display timestamps" option prefixes "%#H:%M:%S "
	// (hour without leading zero); client-local error lines (LTT 26) are exempt.
	if (Client && ChatType != ACEChatMessageType::ChatError
		&& (Client->GetCharacterOptions2() & 0x00000040u) != 0)
	{
		const FDateTime Now = FDateTime::Now();
		Line = FString::Printf(TEXT("%d:%02d:%02d %s"),
			Now.GetHour(), Now.GetMinute(), Now.GetSecond(), *Line);
	}
	// ClientSystem::AddTextToScroll excludes LogTextType 26 from @log too.
	if (ChatType != ACEChatMessageType::ChatError && bChatMirrorToFile && !ChatMirrorFilePath.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Line + LINE_TERMINATOR, *ChatMirrorFilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(),
			FILEWRITE_Append);
	}

	const FLinearColor Color = ACERetailChat::Color(ChatType);
	// Player-attributed rows become click-to-tell targets (retail <Tell:IIDString>).
	FString ClickSender;
	if (!Sender.IsEmpty() && !Sender.Equals(TEXT("You"), ESearchCase::IgnoreCase)
		&& (ChatType == ACEChatMessageType::Speech
			|| ChatType == ACEChatMessageType::Tell
			|| ChatType == ACEChatMessageType::Emote
			|| ChatType == ACEChatMessageType::Social
			|| ChatType == ACEChatMessageType::Channel
			|| ChatType == ACEChatMessageType::Fellowship
			|| ChatType == ACEChatMessageType::Allegiance))
	{
		ClickSender = Sender;
	}

	// Main window honours the quick filter cycle; retail floaty windows use their
	// own 64-bit type filters. Visibility only controls presentation: closed
	// windows must retain matching messages for when the player opens them again.
	const auto* Plugins = Client ? Client->GetGameInstance()->GetSubsystem<UACEPluginSubsystem>() : nullptr;
	const auto Presentation = Plugins ? Plugins->PresentPluginChat(Text, Sender, ChatType) : FACEPluginChatPresentation();
	const uint8 PluginHidden = Presentation.Hidden;
	if (!(PluginHidden & 1) && ((Presentation.Forced&1) || (PassesChatFilter(FilterType) && (MainChatTypeFilter & (1ull << (static_cast<uint32>(FilterType) & 63))) != 0)))
	{
		AppendChatLineToLog(0, Line, Presentation.Colors.Contains(0)?Presentation.Colors[0]:Color, ClickSender);
	}
	else if (!bChatStickToBottom)
	{
		SyncChatJumpIndicator();
	}
	if (Manager)
	{
		for (int32 W = 1; W <= NumFloatyChats; ++W)
		{
			if (PluginHidden & (1u << W)) continue;
			if (!(Presentation.Forced&(1u<<W)) && FloatyChatFilters.IsValidIndex(W - 1)
				&& (FloatyChatFilters[W - 1] & (1ull << (static_cast<uint32>(FilterType) & 63))) == 0)
			{
				continue;
			}
			AppendChatLineToLog(W, Line, Presentation.Colors.Contains(W)?Presentation.Colors[W]:Color, ClickSender);
		}
	}
}

void UACEUIGameplayBinder::HandleAppraisal(const FACEAppraisalInfo& Appraisal)
{
	if (Client && bExaminationDismissed && Client->GetIdentifyRequestSerial() <= DismissedIdentifySerial) return;
	if (Client && Client->GetIdentifyRequestGuid() && Client->GetIdentifyRequestGuid() != Appraisal.ObjectGuid) return;
	bExaminationDismissed = false;
	if (EditingInscriptionGuid && EditingInscriptionGuid != Appraisal.ObjectGuid && ExamInscriptionEditor)
		HandleInscriptionCommitted(ExamInscriptionEditor->GetText(), ETextCommit::OnUserMovedFocus);
	LastAppraisal = Appraisal;
	ExaminedSpellId = 0;
	ShowExamination(true);
	RefreshExaminationOverlay();
}

void UACEUIGameplayBinder::HandleSelectionChanged(const FACESelectedObject& Selection)
{
	// Retail RecvNotice_SelectionChanged cancels the old attack first. ACE
	// ignores a new missile target while the previous repeat remains active.
	// Health/appraisal refreshes and reselecting the same object are not retargets.
	if (Selection.Guid != LastSelection.Guid && Client && Client->GetSession()
		&& !(PlayerController && PlayerController->IsVRActive())
		&& (CombatMode == int32(ACECombatMode::Melee) || CombatMode == int32(ACECombatMode::Missile))
		&& bCombatAutoRepeat && (bCombatRepeatActive || bCombatRetargetPending))
	{
		// UCM must choose its next action through its own range, sight and loot
		// checks. Showing its power bar does not grant the UI a repeat request.
		const bool bAllowRetarget = !bCombatPluginAttack;
		CancelCombatAttack();
		bCombatRetargetPending = bAllowRetarget && Selection.bValid && Client->GetSession()->IsCombatTarget(Selection.Guid);
	}
	if (!Selection.bValid && LastSelection.bValid && Client)
	{
		FACEWorldObject Previous;
		if (!Client->GetWorldObject(LastSelection.Guid,Previous) || Previous.bDying
			|| (Previous.ItemType & ACEItemType::Creature))
			PendingAutoTargetLoss = LastSelection.Guid;
	}
	if (Selection.bValid && Selection.bShowHealth && Selection.HealthFraction <= 0.f
		&& (Selection.Guid != LastSelection.Guid || LastSelection.HealthFraction > 0.f))
		PendingAutoTargetLoss = Selection.Guid;
	const bool bNewClick = Selection.Guid != LastSelection.Guid
		|| Selection.SelectionSerial != LastSelection.SelectionSerial;
	if (bNewClick)
	{
	ToolbarSelectionFlashUntil = Selection.bValid ? FPlatformTime::Seconds() + .25 : 0;
	SelectionFlashUntil = 0;
	TickSelectionFlash(); // restore the previous selection before changing it
	SelectionFlashUntil = FPlatformTime::Seconds() + .4;
	TArray<UPrimitiveComponent*> Meshes;
	auto AddAppearanceParts = [&Meshes](AActor* Actor)
	{
		if (auto* App = Actor ? Actor->FindComponentByClass<UACECharacterAppearanceComponent>() : nullptr)
			for (int32 Part=0; Part<App->GetPartCount(); ++Part)
				if (auto* Mesh=Cast<UPrimitiveComponent>(App->GetPartMesh(Part))) Meshes.AddUnique(Mesh);
	};
	if (Selection.bValid && PlayerController)
		AddAppearanceParts(FindSelectionActor(PlayerController->GetWorld(), Selection.Guid));
	if (Selection.bValid && Client && PaperDollPreviewActor)
	{
		if (Selection.Guid == Client->GetPlayerGuid())
		{
			AddAppearanceParts(PaperDollPreviewActor);
		}
		else if (auto* App = PaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			// gmPaperDollUI::GetSelectionMaskFromObject / ApplyPartSelectionLighting:
			// only the upper visible garment lights up, using the retail Setup part numbers.
			struct FRegion { int64 Mask; int32 Parts[4]; };
			const FRegion Regions[] = {
				{1,{16,-1,-1,-1}}, {0x202,{9,-1,-1,-1}}, {0x404,{0,-1,-1,-1}},
				{0x808,{10,13,-1,-1}}, {0x1010,{11,14,-1,-1}}, {0x20,{12,15,-1,-1}},
				{0x2040,{1,5,-1,-1}}, {0x4080,{2,6,-1,-1}}, {0x100,{3,4,7,8}}
			};
			for (const auto& Region : Regions) if (FindUpperEquippedItem(Region.Mask) == Selection.Guid)
				for (int32 Part : Region.Parts) if (Part >= 0)
					if (auto* Mesh = Cast<UPrimitiveComponent>(App->GetPartMesh(Part))) Meshes.AddUnique(Mesh);
		}
	}
	// Retail selection lighting belongs to the object's parts. Particle meshes
	// have a separate material owner; flashing pooled glow materials let their
	// next reuse copy the temporary brightness and accumulate it on each click.
	for (UPrimitiveComponent* Mesh : Meshes)
		for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
			if (auto* Original = Mesh->GetMaterial(Index))
			{
				auto* Source = Cast<UMaterialInstanceDynamic>(Original);
				auto* Flash = UMaterialInstanceDynamic::Create(Source ? Source->Parent.Get() : Original, this);
				Flash->CopyMaterialUniformParameters(Original);
				FlashMeshes.Add(Mesh); FlashMaterialSlots.Add(Index);
				FlashOriginalMaterials.Add(Original); FlashMaterials.Add(Flash);
				Mesh->SetMaterial(Index, Flash);
			}
	TickSelectionFlash();
	}
	const bool SameStackSelection = LastSelection.Guid == Selection.Guid;
	const int32 PreviousStackAmount = SelectedStackAmount;
	const int32 PreviousStackMax = SelectedStackMax;
	LastSelection = Selection;
	if(ActivePanelPage==TEXT("SocialPanel_Field") && ActiveSocialTab==TEXT("FellowshipPage"))RefreshFellowshipOverlays();
	if(ActivePanelPage==TEXT("SocialPanel_Field") && ActiveSocialTab==TEXT("AllegiancePage"))RefreshAllegianceOverlays();
	SelectedStackAmount = 1;
	SelectedStackMax = 1;
	if (Client && Selection.bValid && Selection.Guid != 0)
	{
		FACEWorldObject Obj;
		if (Client->GetWorldObject(Selection.Guid, Obj))
		{
			const int32 Total = Obj.StackSize > 0 ? Obj.StackSize : 1;
			SelectedStackMax = (Obj.MaxStackSize > 0) ? FMath::Min(Total, Obj.MaxStackSize) : Total;
			const bool VendorStock=OpenVendorGuid!=0 && Client->GetVendorMerchandise().ContainsByPredicate(
				[&](const FACEWorldObject& Item) { return Item.Guid==Obj.Guid; });
			if (VendorStock)
			{
				SelectedStackMax=GetVendorSelectionLimit(Obj.Guid);
			}
			SelectedStackAmount=SelectedStackMax<=0 ? 0 : SameStackSelection && (VendorStock || PreviousStackAmount<PreviousStackMax) ? FMath::Clamp(PreviousStackAmount,1,SelectedStackMax)
				: VendorStock ? ACEInventoryRules::VendorInitialQuantity(Obj,SelectedStackMax) : SelectedStackMax;
		}
		// Keep an open examine/inspect panel in sync with the current selection.
		if (Manager)
		{
			if (TSharedPtr<FACEUIElement> Exam = Manager->FindElementByName(
					TEXT("RootGameplay_FloatyExamination_Field"));
				Exam.IsValid() && Exam->bVisible && !SameStackSelection)
			{
				if (EditingInscriptionGuid && ExamInscriptionEditor)
					HandleInscriptionCommitted(ExamInscriptionEditor->GetText(), ETextCommit::OnUserMovedFocus);
				// Never leave the previous object's qualities on screen while the
				// server appraises the new selection. Replies are filtered by GUID.
				LastAppraisal = {};
				LastAppraisal.ObjectGuid = Selection.Guid;
				LastAppraisal.Name = Selection.Name;
				LastAppraisal.Summary = TEXT("Waiting for the server's appraisal...");
				ExaminedSpellId = 0;
				Client->SendIdentifyObject(Selection.Guid);
				RefreshExaminationOverlay();
			}
		}
	}
	else if (Manager && !ExaminedSpellId)
	{
		// Retail closes object examination when selection is cleared.
		const auto Exam = Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
		if (Exam && Exam->bVisible) { ShowExamination(false); RefreshExaminationOverlay(); }
	}
	RefreshSelectionOverlay();
}

void UACEUIGameplayBinder::HandleVitalsUpdated(const FACEPlayerVitals& Vitals)
{
	LastVitals = Vitals;
	// The session owns optimistic mode and filters the server's transient peace
	// motion during stance changes. A second pending-mode filter here suppresses
	// an intentional Y-to-peace request and leaves the retail/wrist art stale.
	if (Vitals.bValid && Vitals.CombatMode != CombatMode)
	{
		ApplyCombatModeInternal(Vitals.CombatMode, false);
	}
	RefreshVitalsOverlays();
	// XP / skill-credit spends update PrivateUpdate* → refresh raise footer immediately.
	if (ActivePanelPage == TEXT("SkillManagementPanel_Field"))
	{
		RefreshAttributeOverlays();
		RefreshSkillOverlays();
	}
}

void UACEUIGameplayBinder::FocusChatEntry()
{
	FocusChatEntryWindow(0);
}

double UACEUIGameplayBinder::InventoryDoubleClickSeconds() const
{
	return PlayerController && PlayerController->IsVRActive() ? .85 : .5;
}

float UACEUIGameplayBinder::InventoryDragThreshold() const
{
	return PlayerController && PlayerController->IsVRActive() ? 24.f : 12.f;
}

void UACEUIGameplayBinder::ClearChatEntryFocus()
{
	// Controller pointers own a virtual Slate user; stealing every user's focus
	// here interrupts vendor clicks and captured item/spell drags.
	if (PlayerController && PlayerController->IsVRActive())
	{
		PlayerController->GetVRComponent()->DismissTextEntry();
		return;
	}
	if (ChatEntry && ChatEntry->HasKeyboardFocus())
	{
		FSlateApplication::Get().ClearKeyboardFocus(EFocusCause::Cleared);
	}
	// Always restore viewport focus so WASD works with skills/inventory open.
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
}

void UACEUIGameplayBinder::LoadVitalDisplayPreference()
{
	if (!GConfig)
	{
		return;
	}
	bool bNumbers = false;
	if (GConfig->GetBool(TEXT("ACEClient"), TEXT("ShowVitalNumbers"), bNumbers, GGameUserSettingsIni))
	{
		bShowVitalNumbers = bNumbers;
	}
}

void UACEUIGameplayBinder::SaveVitalDisplayPreference() const
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetBool(TEXT("ACEClient"), TEXT("ShowVitalNumbers"), bShowVitalNumbers, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void UACEUIGameplayBinder::SetJumpChargeFraction(float Fraction)
{
	JumpChargeDisplayFraction = FMath::Clamp(Fraction, 0.f, 1.f);
	RefreshPowerbarOverlay();
}

void UACEUIGameplayBinder::RefreshPowerbarOverlay()
{
	if (!Canvas || !Canvas->WidgetTree || !Canvas->GetElementLayer())
	{
		return;
	}
	EnsureOverlays();
	const bool bJump = JumpChargeDisplayFraction > .01f;
	const bool bMelee = CombatMode == int32(ACECombatMode::Melee);
	const bool bMissile = CombatMode == int32(ACECombatMode::Missile);
	const bool bAttack = (bMelee || bMissile) && Client && Client->IsCharacterOptionSet(0x0C)
		&& (bCombatPowerCharging || bCombatRequestSent || bCombatRepeatActive);
	const bool bShow = bJump || bAttack;
	const float Clamped = bJump ? JumpChargeDisplayFraction : FMath::Clamp(CombatPowerOrAccuracy, 0.f, 1.f);
	const TCHAR* LabelText = bJump ? TEXT("Height") : bMissile ? TEXT("Accuracy") : TEXT("Power");
	const uint32 PowerState = bJump ? 0x10000042 : bMissile ? 0x10000044 : 0x10000043;

	// Prefer retail classic_floatypowerbar embedded as RootGameplay_PowerBar_Field
	// (layout 0x21000072): Meter "Powerbar" track 0x06004D0B, yellow fill 0x06001354.
	// Hide adv_recklessness_fill (0x0600715E red) during jump charge.
	bool bUsedLayoutMeter = false;
	if (Manager)
	{
		FString PowerRootName = TEXT("RootGameplay_PowerBar_Field");
		TSharedPtr<FACEUIElement> PowerRoot = Manager->FindElementByName(PowerRootName);
		if (!PowerRoot.IsValid())
		{
			PowerRootName = TEXT("RootFloatyPowerBar_Field");
			PowerRoot = Manager->FindElementByName(PowerRootName);
		}
		TSharedPtr<FACEUIElement> Meter = PowerRoot.IsValid()
			? Manager->FindElementUnder(PowerRootName, TEXT("Powerbar"))
			: nullptr;
		if (PowerRoot.IsValid() && Meter.IsValid() && Meter->Type == ACEUI::ElementType::Meter)
		{
			bUsedLayoutMeter = true;
			// gmPowerbarUI: advanced attack uses the floating power/accuracy bar;
			// jump temporarily replaces it with Height and the yellow fill.
			PowerRoot->DefaultState = PowerState;
			Manager->SetElementVisibleByName(PowerRootName, bShow);
			Meter->MeterFillFraction = bShow ? Clamped : -1.f;
			Meter->bVisible = bShow;
			for (const TSharedPtr<FACEUIElement>& Child : Meter->Children)
			{
				if (!Child.IsValid())
				{
					continue;
				}
				if (Child->ElementName == TEXT("adv_recklessness_fill")
					|| Child->ImageFileId == 0x0600715E)
				{
					Child->bVisible = bShow && !bJump && bMelee && LastVitals.Skills.ContainsByPredicate(
						[](const FACESkillInfo& Skill) { return Skill.SkillId == 50 && Skill.AdvancementClass >= 2; });
				}
				else if (Child->ImageFileId == 0x06001354)
				{
					Child->bVisible = bShow;
				}
				else if (Child->ElementName == TEXT("Powerbar_Text"))
				{
					Child->bVisible = bShow;
				}
			}
		}
	}

	auto CollapseBorder = [](TObjectPtr<UBorder>& Border)
	{
		if (Border)
		{
			Border->SetVisibility(ESlateVisibility::Collapsed);
		}
	};
	auto CollapseText = [](TObjectPtr<UTextBlock>& Text)
	{
		if (Text)
		{
			Text->SetVisibility(ESlateVisibility::Collapsed);
		}
	};

	if (bUsedLayoutMeter)
	{
		CollapseBorder(JumpChargeFrame);
		CollapseBorder(JumpChargeFill);
        if (bShow)
        {
            if (!JumpChargeLabel)
                JumpChargeLabel=Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
            // JumpMode's UICore_Text_entry is language DAT 0x0529BD74: Height.
            auto Label=Manager->FindElementByName(TEXT("Powerbar_Text"));
            PlaceTextOnElement(JumpChargeLabel,Label,LabelText,14,FLinearColor::White,9002,true);
        }
        else CollapseText(JumpChargeLabel);
		return;
	}

	// Fallback Slate overlay when PowerBar layout is unavailable.
	auto EnsureBarBorder = [&](TObjectPtr<UBorder>& Border, int32 IconDid)
	{
		if (!Border)
		{
			Border = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Border->SetPadding(FMargin(0.f));
			Border->SetVisibility(ESlateVisibility::HitTestInvisible);
			Canvas->GetElementLayer()->AddChild(Border);
		}
		SetIconDid(Border, IconDid);
	};
	EnsureBarBorder(JumpChargeFrame, 0x06004D0B);
	EnsureBarBorder(JumpChargeFill, bJump ? 0x06001354 : 0x06001200);
	if (!JumpChargeLabel)
	{
		JumpChargeLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		JumpChargeLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		JumpChargeLabel->SetJustification(ETextJustify::Left);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		JumpChargeLabel->SetFont(Font);
		JumpChargeLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.95f, 0.9f, 1.f)));
		JumpChargeLabel->SetText(FText::FromString(TEXT("Height")));
		Canvas->GetElementLayer()->AddChild(JumpChargeLabel);
	}
	JumpChargeLabel->SetText(FText::FromString(LabelText));

	if (!bShow)
	{
		JumpChargeFrame->SetVisibility(ESlateVisibility::Collapsed);
		JumpChargeFill->SetVisibility(ESlateVisibility::Collapsed);
		JumpChargeLabel->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	JumpChargeFrame->SetVisibility(ESlateVisibility::HitTestInvisible);
	JumpChargeFill->SetVisibility(ESlateVisibility::HitTestInvisible);
	JumpChargeLabel->SetVisibility(ESlateVisibility::HitTestInvisible);

	const FVector2D View = Canvas->GetCachedGeometry().GetLocalSize();
	const float BarW = FMath::Clamp(View.X * 0.42f, 220.f, 520.f);
	constexpr float BarH = 14.f;
	constexpr float LabelW = 48.f;
	const float TotalW = BarW + 6.f + LabelW;
	const float X = View.X * 0.5f - TotalW * 0.5f;
	const float Y = View.Y * 0.48f;
	const float FillW = FMath::Max(2.f, BarW * Clamped);

	if (UCanvasPanelSlot* FrameSlot = Cast<UCanvasPanelSlot>(JumpChargeFrame->Slot))
	{
		FrameSlot->SetAutoSize(false);
		FrameSlot->SetAnchors(FAnchors(0.f, 0.f));
		FrameSlot->SetAlignment(FVector2D(0.f, 0.f));
		FrameSlot->SetPosition(FVector2D(X, Y));
		FrameSlot->SetSize(FVector2D(BarW, BarH));
		Canvas->SetOverlayOrder(JumpChargeFrame, Manager->FindElementByName(TEXT("RootGameplay_SmartBox_Field")), 9000);
	}
	if (UCanvasPanelSlot* FillSlot = Cast<UCanvasPanelSlot>(JumpChargeFill->Slot))
	{
		FillSlot->SetAutoSize(false);
		FillSlot->SetAnchors(FAnchors(0.f, 0.f));
		FillSlot->SetAlignment(FVector2D(0.f, 0.f));
		FillSlot->SetPosition(FVector2D(X, Y));
		FillSlot->SetSize(FVector2D(FillW, BarH));
		Canvas->SetOverlayOrder(JumpChargeFill, Manager->FindElementByName(TEXT("RootGameplay_SmartBox_Field")), 9001);
	}
	if (UCanvasPanelSlot* LabelSlot = Cast<UCanvasPanelSlot>(JumpChargeLabel->Slot))
	{
		LabelSlot->SetAutoSize(false);
		LabelSlot->SetAnchors(FAnchors(0.f, 0.f));
		LabelSlot->SetAlignment(FVector2D(0.f, 0.f));
		LabelSlot->SetPosition(FVector2D(X + BarW + 6.f, Y - 1.f));
		LabelSlot->SetSize(FVector2D(LabelW, BarH + 2.f));
		Canvas->SetOverlayOrder(JumpChargeLabel, Manager->FindElementByName(TEXT("RootGameplay_SmartBox_Field")), 9002);
	}
}

bool UACEUIGameplayBinder::TrySendChatFromEntry(const FString* OverrideText, int32 SourceWindow)
{
	UEditableTextBox* Entry = GetChatEntryWidget(SourceWindow);
	if (!Client || !Entry)
	{
		return false;
	}
	// Retail m_eWindowID: commands know which window's entry they came from
	// (@title / @clear target the source window).
	ChatSourceWindow = SourceWindow;
	FString Message = OverrideText ? *OverrideText : Entry->GetText().ToString();
	// Strip BOM / zero-width / NBSP that break @/ prefix detection.
	Message.ReplaceInline(TEXT("\uFEFF"), TEXT(""));
	Message.ReplaceInline(TEXT("\u200B"), TEXT(""));
	Message.ReplaceInline(TEXT("\u00A0"), TEXT(" "));
	Message.TrimStartAndEndInline();
	// Retail "Stay in chat mode after sending" (PlayerOption 0x0B): keep the entry
	// focused after the send. Deferred one tick — the edit box clears keyboard
	// focus after the commit delegate returns, which would undo a same-frame focus.
	if (Client && (Client->GetCharacterOptions1() & 0x00000800u) != 0)
	{
		bPendingChatRefocus = true;
		PendingChatRefocusWindow = SourceWindow;
	}
	else if (!(PlayerController && PlayerController->IsVRActive()))
	{
		// EditableText clears focus after the commit callback. Restore gameplay
		// next tick so another Enter/slash reaches the viewport immediately.
		bPendingChatRefocus = true;
		PendingChatRefocusWindow = INDEX_NONE;
	}
	// ChatInterface::HandleEnterKey relinquishes/retains focus before checking
	// for an empty command. Slate clears focus after this callback, so empty
	// submissions need the same deferred handoff as messages that get sent.
	if (Message.IsEmpty()) return true;
	CloseChatTargetPopup();

	if (auto* Chat = Cast<UACEChatEntry>(Entry)) Chat->RememberSubmitted(Message);

	// Retail pose emote: *bow* / *wave* → ChatPoseTable + SoulEmote (+ local motion).
	if (Message.StartsWith(TEXT("*")))
	{
		FString Emote = Message.Mid(1).TrimStartAndEnd();
		if (Emote.EndsWith(TEXT("*")))
		{
			Emote.LeftChopInline(1);
			Emote.TrimStartAndEndInline();
		}
		if (!Emote.IsEmpty())
		{
			UACEDatSubsystem* Dat = nullptr;
			if (PlayerController)
			{
				if (UGameInstance* GI = PlayerController->GetGameInstance())
				{
					Dat = GI->GetSubsystem<UACEDatSubsystem>();
				}
			}
			FString CmdName, MyEmote, OtherEmote;
			if (Dat && Dat->TryGetChatPose(Emote, CmdName, MyEmote, OtherEmote))
			{
				SendPoseChat(CmdName, MyEmote, OtherEmote);
				auto NormalizePoseKey = [](FString S) -> FString
				{
					S = S.ToLower();
					S.ReplaceInline(TEXT(" "), TEXT(""));
					S.ReplaceInline(TEXT("-"), TEXT(""));
					S.ReplaceInline(TEXT("_"), TEXT(""));
					return S;
				};
				uint32 MotionCmd = 0;
				const FString MotionKey = NormalizePoseKey(CmdName);
				const FString EmoteKey = NormalizePoseKey(Emote);
				if (!ACEMotionName::TryResolve(MotionKey, MotionCmd))
				{
					ACEMotionName::TryResolve(EmoteKey, MotionCmd);
				}
				if (MotionCmd != 0)
				{
					if (MotionCmd == 0x13000084u) MotionCmd = 0x430000f0u; // PointState
					Client->SendSoulEmoteMotion(static_cast<int32>(MotionCmd));
					APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
					UACECharacterAppearanceComponent* App = Pawn
						? Pawn->FindComponentByClass<UACECharacterAppearanceComponent>()
						: nullptr;
					if (App)
					{
						// Hold the forward pose; sidestep/turn are independent in retail.
						App->PlayActionMotion(static_cast<int32>(MotionCmd), 1.f, 0, true);
					}
					UE_LOG(LogTemp, Log, TEXT("ACE ChatPose: '%s' → cmd='%s' motion=0x%08X"),
						*Emote, *CmdName, MotionCmd);
				}
				else
				{
					UE_LOG(LogTemp, Warning,
						TEXT("ACE ChatPose: unmapped cmd='%s' emote='%s'"), *CmdName, *Emote);
				}
			}
			else
			{
				// Unknown pose key — fall back to text emote (@e).
				Client->SendEmote(Emote);
			}
		}
		Entry->SetText(FText::GetEmpty());
		return true;
	}

	// Mag-nus CommunicationSystem::OnChatCommand — @ / / / : / ; never go as Speech.
	const TCHAR Lead = Message[0];
	if (Lead == TCHAR('@') || Lead == TCHAR('/') || Lead == TCHAR(':') || Lead == TCHAR(';')
		|| Lead == 0xFF20) // fullwidth ＠
	{
		if (Lead == 0xFF20)
		{
			Message = TEXT("@") + Message.Mid(1);
		}
		else if (Lead == TCHAR('/'))
		{
			Message = TEXT("@") + Message.Mid(1);
		}
		bool bClearEntry = true;
		TryDispatchChatCommand(Message, Entry, &bClearEntry);
		if (bClearEntry)
		{
			Entry->SetText(FText::GetEmpty());
		}
		else
		{
			FocusChatEntryWindow(SourceWindow);
		}
		return true;
	}

	// ChatTarget destination: Say, fellowship/allegiance channels, or Turbine global chat.
	if (ChatSendIsTurbine(ChatSendChannel))
	{
		if (TSharedPtr<FACESession> S = Client->GetSession())
		{
			const uint32 ChatType = ChatSendTurbineChatType(ChatSendChannel);
			const int32 Opt = (ChatType == ACETurbineChat::General) ? 0x23
				: (ChatType == ACETurbineChat::Trade) ? 0x24
				: (ChatType == ACETurbineChat::LFG) ? 0x25
				: (ChatType == ACETurbineChat::Roleplay) ? 0x26
				: (ChatType == ACETurbineChat::Society) ? 0x2E
				: INDEX_NONE;
			if (Opt != INDEX_NONE && !Client->IsCharacterOptionSet(Opt))
			{
				HandleChatMessage(FString::Printf(TEXT("You are not listening to the %s channel!"),
					ChatType == ACETurbineChat::Trade ? TEXT("Trade")
					: ChatType == ACETurbineChat::LFG ? TEXT("LFG")
					: ChatType == ACETurbineChat::Roleplay ? TEXT("Roleplay")
					: ChatType == ACETurbineChat::Society ? TEXT("Society")
					: TEXT("General")),
					FString(), ACEChatMessageType::ChatError);
			}
			else
			{
				S->SendTurbineChat(S->GetTurbineChannelId(ChatType), ChatType, Message);
			}
		}
	}
	else if (ChatSendChannel==12)
	{
		const auto Selection=Client->GetSelectedObject(); FACEWorldObject Target;
		if (Selection.bValid && Client->GetWorldObject(Selection.Guid,Target)
			&& Target.ItemType==ACEItemType::Creature && Target.Guid!=Client->GetPlayerGuid())
		{
			Client->SendTalkDirect(Target.Guid,Message); LastOutgoingTellName=Target.Name;
		}
		else { AppendLocalChatLine(TEXT("Select a character to tell."),ACEChatMessageType::ChatError); return true; }
	}
	else
	{
		const uint32 Chan = ChatSendChannelId(ChatSendChannel);
		if (Chan != 0)
		{
			Client->SendChatChannel(static_cast<int32>(Chan), Message);
		}
		else
		{
			Client->SendChatMessage(Message);
			HandleChatMessage(Message, TEXT("You"), ACEChatMessageType::Speech);
		}
	}
	Entry->SetText(FText::GetEmpty());
	return true;
}

bool UACEUIGameplayBinder::ScrollChatLog(float PixelDelta, int32 Window)
{
    UScrollBox* Log=GetChatLogWidget(Window);
    if (!Log) return false;
    const float MaxOffset=FMath::Max(0.f,Log->GetScrollOffsetOfEnd());
    const float Next=FMath::Clamp(Log->GetScrollOffset()+PixelDelta,0.f,MaxOffset);
    Log->SetScrollOffset(Next);
    const bool Stick=MaxOffset<=KINDA_SMALL_NUMBER || Next>=MaxOffset-2.f;
    if (Window==0)
    {
        bChatStickToBottom=Stick; ChatStickEndOffset=Stick ? MaxOffset : -1.f;
        bPendingChatScrollToEnd=false;
    }
    else bFloatyChatStickToBottom[Window-1]=Stick;
    SyncChatJumpIndicator(Window);
    SyncChatScrollbar(Window);
    return true;
}

int32 UACEUIGameplayBinder::ChatWindowAtPointer(FVector2D CanvasLocalPos) const
{
    if (!Manager || !Canvas) return INDEX_NONE;
    const FVector2D P=Canvas->ViewportToLayout(CanvasLocalPos);
    for (int32 W=0; W<=NumFloatyChats; ++W)
    {
        const FString Root=W ? FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),W) : TEXT("RootGameplay_FloatyMainChat_Field");
        for (const TCHAR* Name : {TEXT("ChatLog"),TEXT("ChatLogScrollbar")})
        {
            const auto El=Manager->FindElementUnder(Root,Name);
            if (!El || !Canvas->IsElementExposedAt(El,CanvasLocalPos)) continue;
            const FIntPoint O=El->GetScreenOrigin();
            if (P.X>=O.X && P.Y>=O.Y && P.X<O.X+El->Width && P.Y<O.Y+El->Height) return W;
        }
    }
    return INDEX_NONE;
}

bool UACEUIGameplayBinder::IsPointerOverChatLog(FVector2D CanvasLocalPos) const
{
    return ChatWindowAtPointer(CanvasLocalPos)!=INDEX_NONE;
}

void UACEUIGameplayBinder::SyncChatScrollbar(int32 Window)
{
    UScrollBox* Log=GetChatLogWidget(Window);
    if (!Manager || !Log) return;
    const FString Root=Window ? FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),Window) : TEXT("RootGameplay_FloatyMainChat_Field");
    const auto Bar=Manager->FindElementUnder(Root,TEXT("ChatLogScrollbar"));
    if (!Bar || Bar->Height<=0) return;
    const float MaxOffset=FMath::Max(0.f,Log->GetScrollOffsetOfEnd());
    const float Frac=MaxOffset>KINDA_SMALL_NUMBER ? FMath::Clamp(Log->GetScrollOffset()/MaxOffset,0.f,1.f) : 0.f;
    const float ViewH=FMath::Max(1.f,Log->GetCachedGeometry().GetLocalSize().Y);
    SyncDatScrollbar(Bar,Frac,ViewH/(ViewH+MaxOffset));
}

void UACEUIGameplayBinder::SyncChatJumpIndicator(int32 Window)
{
	auto* Log=GetChatLogWidget(Window);
	if (!Manager || !Log)
	{
		return;
	}
	const FString Root=Window ? FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),Window) : TEXT("RootGameplay_FloatyMainChat_Field");
	TSharedPtr<FACEUIElement> Indicator = Manager->FindElementUnder(Root, TEXT("ChatLogNewNonVisibleTextIndicator"));
	if (!Indicator.IsValid())
	{
		return;
	}
	const float MaxOffset = FMath::Max(0.f, Log->GetScrollOffsetOfEnd());
	const bool Stick=Window==0 ? bChatStickToBottom : bFloatyChatStickToBottom[Window-1];
	if (Stick) bChatUnread[Window] = false;
	const bool bShow = bChatUnread[Window] && !Stick && MaxOffset > KINDA_SMALL_NUMBER;
	// ChatInterface selects state 1 explicitly; this field has no default
	// image, so visibility alone leaves a blank but clickable rectangle.
	Indicator->bUseExplicitState = true;
	Indicator->DefaultState = bShow ? 1 : 13;
	Indicator->bVisible = bShow;
	if (!bShow)
	{
		return;
	}
	// The unread arrow overlays the log; it does not reserve a permanent text gutter.
	TSharedPtr<FACEUIElement> ChatLogEl = Manager->FindElementUnder(Root, TEXT("ChatLog"));
	if (ChatLogEl.IsValid())
	{
		Indicator->X = 0;
		Indicator->Y = FMath::Max(0, ChatLogEl->Height - 16);
		Indicator->Width = 16;
		Indicator->Height = 16;
		Indicator->EdgeAnchorX = Indicator->EdgeAnchorY = 0;
		Indicator->RecomputeLayoutOffset();
	}
}

void UACEUIGameplayBinder::SyncDatScrollbar(const TSharedPtr<FACEUIElement>& Bar, float Frac, float VisibleFrac)
{
	if (IsScrollbarDragActive() && ScrollDragBar.Pin() == Bar) Frac = ScrollDragFraction;
	if (!Bar.IsValid() || Bar->Height <= 0 || Bar->Width <= 0)
	{
		return;
	}
	constexpr int32 Btn = 16;
	for (const auto& Child : Bar->Children)
	{
		if (!Child) continue;
		Child->EdgeAnchorX=Child->EdgeAnchorY=0; Child->RecomputeLayoutOffset();
		for (const auto& Part : Child->Children)
			if (Part) { Part->EdgeAnchorX=Part->EdgeAnchorY=0; Part->RecomputeLayoutOffset(); }
	}
	TSharedPtr<FACEUIElement> Up;
	TSharedPtr<FACEUIElement> Down;
	TSharedPtr<FACEUIElement> Left;
	TSharedPtr<FACEUIElement> Right;
	TSharedPtr<FACEUIElement> Thumb;
	for (const TSharedPtr<FACEUIElement>& Child : Bar->Children)
	{
		if (!Child.IsValid())
		{
			continue;
		}
		if (Child->ElementName == TEXT("ScrollBar_Up")) { Up = Child; }
		else if (Child->ElementName == TEXT("ScrollBar_Down")) { Down = Child; }
		else if (Child->ElementName == TEXT("ScrollBar_Left")) { Left = Child; }
		else if (Child->ElementName == TEXT("ScrollBar_Right")) { Right = Child; }
		else if (Child->Type == ACEUI::ElementType::Scrollbar
			|| Child->ElementName.IsEmpty()
			|| Child->ElementName.Contains(TEXT("widget_")))
		{
			if (!Thumb.IsValid() || Child->Type == ACEUI::ElementType::Scrollbar
				|| Child->ElementName.IsEmpty())
			{
				Thumb = Child;
			}
		}
	}

	const bool bHorizontal = (Left.IsValid() || Right.IsValid())
		|| (Bar->Width > Bar->Height * 1.5f);
	// Keep the authored track media/draw mode.
	const float SizeFrac = VisibleFrac > 0.f
		? FMath::Clamp(VisibleFrac, 0.08f, 1.f)
		: 0.35f;

	if (bHorizontal)
	{
		const int32 Track = FMath::Max(0, Bar->Width - Btn * 2);
		const int32 ThumbW = FMath::Clamp(
			FMath::RoundToInt(static_cast<float>(Track) * SizeFrac),
			8, FMath::Max(8, Track));
		const int32 Travel = FMath::Max(0, Track - ThumbW);
		const bool bRightFirst = Right.IsValid() && Left.IsValid() && Right->X <= Left->X;
		// Track image on Bar must not outrank arrows/thumb (authored Bar.ZLevel is often high).
		Bar->ZLevel = 0;
		if (Left.IsValid())
		{
			Left->X = bRightFirst ? (Bar->Width - Btn) : 0;
			Left->Y = 0;
			Left->Width = Btn;
			Left->Height = Bar->Height;
			Left->bVisible = true;
			Left->ZLevel = 50;
		}
		if (Right.IsValid())
		{
			Right->X = bRightFirst ? 0 : (Bar->Width - Btn);
			Right->Y = 0;
			Right->Width = Btn;
			Right->Height = Bar->Height;
			Right->bVisible = true;
			Right->ZLevel = 50;
		}
		if (Up.IsValid()) { Up->bVisible = false; }
		if (Down.IsValid()) { Down->bVisible = false; }
		if (Thumb.IsValid())
		{
			const int32 TrackStart = Btn;
			Thumb->X = TrackStart + FMath::RoundToInt(FMath::Clamp(Frac, 0.f, 1.f) * static_cast<float>(Travel));
			Thumb->Y = 0;
			Thumb->Width = ThumbW;
			Thumb->Height = Bar->Height;
			Thumb->bVisible = true;
			Thumb->ZLevel = 40;
			for (const TSharedPtr<FACEUIElement>& Part : Thumb->Children)
			{
				if (!Part.IsValid()) { continue; }
				Part->ZLevel = 40;
				if (Part->ElementName == TEXT("widget_left_field") || Part->ElementName == TEXT("widget_top_field"))
				{
					Part->X = 0; Part->Y = 0; Part->Width = 3; Part->Height = Bar->Height;
				}
				else if (Part->ElementName == TEXT("widget_mid_field"))
				{
					Part->X = 3; Part->Y = 0; Part->Width = FMath::Max(2, ThumbW - 6); Part->Height = Bar->Height;
				}
				else if (Part->ElementName == TEXT("widget_right_field") || Part->ElementName == TEXT("widget_bottom_field"))
				{
					Part->X = FMath::Max(3, ThumbW - 3); Part->Y = 0; Part->Width = 3; Part->Height = Bar->Height;
				}
			}
		}
		return;
	}

	// Vertical: Up always at top, Down at bottom (authored Y can both be 0 before layout).
	Bar->ZLevel = 0; // track/bevel paints under interactive chrome
	if (Up.IsValid())
	{
		Up->X = 0;
		Up->Y = 0;
		Up->Width = Btn;
		Up->Height = Btn;
		Up->bVisible = true;
		Up->ZLevel = 50; // above thumb track / slider line
	}
	if (Down.IsValid())
	{
		Down->X = 0;
		Down->Y = Bar->Height - Btn;
		Down->Width = Btn;
		Down->Height = Btn;
		Down->bVisible = true;
		Down->ZLevel = 50;
	}
	if (Left.IsValid())
	{
		Left->ZLevel = 50;
	}
	if (Right.IsValid())
	{
		Right->ZLevel = 50;
	}
	if (Thumb.IsValid())
	{
		const int32 Track = FMath::Max(0, Bar->Height - Btn * 2);
		const int32 ThumbH = FMath::Clamp(
			FMath::RoundToInt(static_cast<float>(Track) * SizeFrac),
			16, FMath::Max(16, Track));
		const int32 Travel = FMath::Max(0, Track - ThumbH);
		Thumb->X = 0;
		Thumb->Y = Btn + FMath::RoundToInt(FMath::Clamp(Frac, 0.f, 1.f) * static_cast<float>(Travel));
		Thumb->Width = Btn;
		Thumb->Height = ThumbH;
		Thumb->bVisible = true;
		Thumb->ZLevel = 40; // under Up/Down arrows
		for (const TSharedPtr<FACEUIElement>& Part : Thumb->Children)
		{
			if (!Part.IsValid()) { continue; }
			Part->ZLevel = 40;
			if (Part->ElementName == TEXT("widget_top_field"))
			{
				Part->Y = 0; Part->Height = 3;
			}
			else if (Part->ElementName == TEXT("widget_mid_field"))
			{
				Part->Y = 3; Part->Height = FMath::Max(2, ThumbH - 6);
			}
			else if (Part->ElementName == TEXT("widget_bottom_field"))
			{
				Part->Y = FMath::Max(3, ThumbH - 3); Part->Height = 3;
			}
		}
	}
}

void UACEUIGameplayBinder::ReflowInventoryPanelGeometry()
{
	if (!Manager || ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		return;
	}
	TSharedPtr<FACEUIElement> Page = Manager->FindElementByName(TEXT("InventoryPanel_Field"));
	if (!Page.IsValid() || Page->Height <= 0)
	{
		return;
	}
	const int32 PanelH = Page->Height;

	// Retail keeps the 362px gradient at the top (bottomEdge=2) and grows
	// Blackness from y=361 (bottomEdge=1). Repeating the gradient over the
	// whole panel repeats its transparent edge, exposing strips of the world.
	if (TSharedPtr<FACEUIElement> Bg = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("InvBackgroundImage")))
	{
		Bg->X = 0;
		Bg->Y = 0;
		Bg->Width = Page->Width;
		Bg->Height = FMath::Min(362, PanelH);
		Bg->bVisible = true;
		Bg->ImageFileId = 0x06004D0A;
		Bg->DrawMode = 3;
		Bg->ZLevel = 1; // underlay sort still paints first among siblings
	}
	if (TSharedPtr<FACEUIElement> Black = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("Blackness")))
	{
		Black->Y = FMath::Min(361, FMath::Max(0, PanelH - 1));
		Black->Width = Page->Width;
		Black->Height = FMath::Max(1, PanelH - Black->Y);
		Black->bVisible = true;
		if (Black->ImageFileId == 0)
		{
			Black->ImageFileId = 0x06004D0B;
		}
	}

	// Authored: ThreeDItemsField @ y=237 h=120 under a 362 panel. Grow with extra height.
	TSharedPtr<FACEUIElement> ItemsField = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("ThreeDItemsField"));
	if (ItemsField.IsValid())
	{
		const int32 ItemsH = FMath::Max(120, PanelH - ItemsField->Y);
		ItemsField->Height = ItemsH;
		for (const TSharedPtr<FACEUIElement>& Child : ItemsField->Children)
		{
			if (!Child.IsValid())
			{
				continue;
			}
			if (Child->ElementName == TEXT("Inv_3DItemList")
				|| Child->ElementName == TEXT("Inv_3DItemList_Scrollbar"))
			{
				Child->Height = FMath::Max(32, ItemsH - Child->Y);
			}
		}
	}

	TSharedPtr<FACEUIElement> PackField = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("BackpackField"));
	if (PackField.IsValid())
	{
		PackField->Height = FMath::Max(339, PanelH - PackField->Y);
		// Burden chrome occupies the top of the backpack column (bar y=8 h=58 → list at y=73).
		constexpr int32 BurdenReserveY = 73;
		for (const TSharedPtr<FACEUIElement>& Child : PackField->Children)
		{
			if (!Child.IsValid())
			{
				continue;
			}
			if (Child->ElementName == TEXT("Inv_ContainerList")
				|| Child->ElementName == TEXT("Inv_ContainerList_Scrollbar"))
			{
				Child->Y = FMath::Max(Child->Y, BurdenReserveY);
				Child->Height = FMath::Max(36, PackField->Height - Child->Y);
			}
		}
	}
}



void UACEUIGameplayBinder::SyncSpellbookScrollbar()
{
	if (!Manager || ActivePanelPage != TEXT("SpellManagementPanel_Field")
		|| ActiveSpellPanelTab != TEXT("SpellbookPage"))
	{
		return;
	}
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("SpellbookPage"), TEXT("SpellBook_SpellList"));
	TSharedPtr<FACEUIElement> Bar = Manager->FindElementUnder(
		TEXT("SpellbookPage"), TEXT("SpellBook_SpellList_Scrollbar"));
	if (!ListEl.IsValid() || !Bar.IsValid())
	{
		return;
	}
	Bar->Height = ListEl->Height;
	Bar->Y = ListEl->Y;
	Bar->bVisible = true;
	const int32 RowH = SpellbookTemplate().Height();
	const int32 PageSize = FMath::Max(1, ListEl->Height / RowH);
	const int32 MaxOff = FMath::Max(0, SpellbookFilteredCount - PageSize);
	const float Frac = MaxOff > 0
		? FMath::Clamp(static_cast<float>(SpellbookScrollOffset) / static_cast<float>(MaxOff), 0.f, 1.f)
		: 0.f;
	const float VisibleFrac = SpellbookFilteredCount > 0
		? FMath::Clamp(static_cast<float>(PageSize) / static_cast<float>(SpellbookFilteredCount), 0.08f, 1.f)
		: 1.f;
	SyncDatScrollbar(Bar, Frac, VisibleFrac);
}

bool UACEUIGameplayBinder::ToggleSpellbookFilter(const FString& Name)
{
	if (ActivePanelPage != TEXT("SpellManagementPanel_Field")
		|| ActiveSpellPanelTab != TEXT("SpellbookPage"))
	{
		return false;
	}
	auto ToggleBit = [](uint32& Mask, int32 BitIndex) -> bool
	{
		if (BitIndex < 0 || BitIndex > 31)
		{
			return false;
		}
		Mask ^= (1u << BitIndex);
		return true;
	};
	bool bChanged = false;
	if (Name == TEXT("School_War")) { bChanged = ToggleBit(SpellbookSchoolFilterMask, 0); }
	else if (Name == TEXT("School_Life")) { bChanged = ToggleBit(SpellbookSchoolFilterMask, 1); }
	else if (Name == TEXT("School_Item")) { bChanged = ToggleBit(SpellbookSchoolFilterMask, 2); }
	else if (Name == TEXT("School_Creature")) { bChanged = ToggleBit(SpellbookSchoolFilterMask, 3); }
	else if (Name == TEXT("School_Void")) { bChanged = ToggleBit(SpellbookSchoolFilterMask, 4); }
	else if (Name == TEXT("Level_One")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 0); }
	else if (Name == TEXT("Level_Two")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 1); }
	else if (Name == TEXT("Level_Three")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 2); }
	else if (Name == TEXT("Level_Four")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 3); }
	else if (Name == TEXT("Level_Five")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 4); }
	else if (Name == TEXT("Level_Six")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 5); }
	else if (Name == TEXT("Level_Seven")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 6); }
	else if (Name == TEXT("Level_Eight")) { bChanged = ToggleBit(SpellbookLevelFilterMask, 7); }
	if (!bChanged)
	{
		return false;
	}
	SpellbookFilterPressedName = Name;
	if (Client && Client->GetSession())
	{
		const auto Session = Client->GetSession();
		Session->SendSpellbookFilters(ACESpellbookFilters::ToWire(
			SpellbookSchoolFilterMask, SpellbookLevelFilterMask, Session->GetSpellbookFilters()));
	}
	SpellbookFilterPressedUntil = FPlatformTime::Seconds() + 0.15;
	SpellbookScrollOffset = 0;
	SyncSpellbookFilterCheckboxes();
	RefreshSpellbookOverlays();
	SyncSpellbookScrollbar();
	return true;
}

void UACEUIGameplayBinder::SyncSpellbookFilterCheckboxes()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree
		|| ActivePanelPage != TEXT("SpellManagementPanel_Field")
		|| ActiveSpellPanelTab != TEXT("SpellbookPage"))
	{
		for (UBorder* B : SpellbookFilterCheckboxes)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		// Hide DAT filter chrome entirely off the spellbook tab / when panel closed.
		if (TSharedPtr<FACEUIElement> Filter = Manager
			? Manager->FindElementByName(TEXT("FilterBox")) : nullptr)
		{
			Filter->bVisible = false;
		}
		if (TSharedPtr<FACEUIElement> Del = Manager
			? Manager->FindElementUnder(TEXT("FilterBox"), TEXT("DeleteSpell_Button"))
			: nullptr)
		{
			Del->bVisible = false;
			for (const TSharedPtr<FACEUIElement>& C : Del->Children)
			{
				if (C.IsValid()) { C->bVisible = false; }
			}
		}
		return;
	}
	if (TSharedPtr<FACEUIElement> Filter = Manager->FindElementByName(TEXT("FilterBox")))
	{
		Filter->bVisible = true;
	}
	// Retail has no working delete-spell UI here — hide the chrome entirely.
	if (TSharedPtr<FACEUIElement> Del = Manager->FindElementUnder(TEXT("FilterBox"), TEXT("DeleteSpell_Button")))
	{
		Del->bVisible = false;
		for (const TSharedPtr<FACEUIElement>& C : Del->Children)
		{
			if (C.IsValid()) { C->bVisible = false; }
		}
	}
	if (TSharedPtr<FACEUIElement> DelL = Manager->FindElementUnder(TEXT("FilterBox"), TEXT("DeleteSpell_Label")))
	{
		DelL->bVisible = false;
	}

	struct FFilterCb
	{
		const TCHAR* Name;
		uint32 Mask;
		int32 Bit;
	};
	const FFilterCb Cbs[] = {
		{ TEXT("School_War"), SpellbookSchoolFilterMask, 0 },
		{ TEXT("School_Life"), SpellbookSchoolFilterMask, 1 },
		{ TEXT("School_Item"), SpellbookSchoolFilterMask, 2 },
		{ TEXT("School_Creature"), SpellbookSchoolFilterMask, 3 },
		{ TEXT("School_Void"), SpellbookSchoolFilterMask, 4 },
		{ TEXT("Level_One"), SpellbookLevelFilterMask, 0 },
		{ TEXT("Level_Two"), SpellbookLevelFilterMask, 1 },
		{ TEXT("Level_Three"), SpellbookLevelFilterMask, 2 },
		{ TEXT("Level_Four"), SpellbookLevelFilterMask, 3 },
		{ TEXT("Level_Five"), SpellbookLevelFilterMask, 4 },
		{ TEXT("Level_Six"), SpellbookLevelFilterMask, 5 },
		{ TEXT("Level_Seven"), SpellbookLevelFilterMask, 6 },
		{ TEXT("Level_Eight"), SpellbookLevelFilterMask, 7 },
	};
	while (SpellbookFilterCheckboxes.Num() < UE_ARRAY_COUNT(Cbs) && Canvas->WidgetTree)
	{
		UBorder* B = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		B->SetPadding(FMargin(0.f));
		B->SetVisibility(ESlateVisibility::HitTestInvisible);
		SpellbookFilterCheckboxes.Add(B);
	}
	for (int32 i = 0; i < UE_ARRAY_COUNT(Cbs); ++i)
	{
		UBorder* Box = SpellbookFilterCheckboxes.IsValidIndex(i) ? SpellbookFilterCheckboxes[i] : nullptr;
		TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(TEXT("FilterBox"), Cbs[i].Name);
		if (!Box || !El.IsValid())
		{
			if (Box) { Box->SetVisibility(ESlateVisibility::Collapsed); }
			continue;
		}
		// The shared checkbox template is 90px wide, while the spellbook packs
		// several filters 50/65px apart. Retail sizes each instance to its cell.
		if (const auto Parent = El->Parent.Pin())
		{
			int32 Right = Parent->Width;
			for (const auto& Sibling : Parent->Children)
				if (Sibling != El && Sibling->Y == El->Y && Sibling->X > El->X) Right = FMath::Min(Right, Sibling->X);
			El->Width = FMath::Max(13, Right - El->X - 2);
			El->LayoutAuthoredW = El->LastReflowW = El->Width;
			for (const auto& Child : El->Children)
				if (Child->Width > 13) { Child->Width = FMath::Max(0, El->Width - Child->X); Child->LayoutAuthoredW = Child->LastReflowW = Child->Width; }
		}
		bool bAncestorsVisible = El->bVisible;
		for (TSharedPtr<FACEUIElement> P = El->Parent.Pin(); P.IsValid(); P = P->Parent.Pin())
		{
			if (!P->bVisible) { bAncestorsVisible = false; break; }
		}
		if (!bAncestorsVisible)
		{
			Box->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		const bool bOn = (Cbs[i].Mask & (1u << Cbs[i].Bit)) != 0;
		const bool bPressed = !SpellbookFilterPressedName.IsEmpty()
			&& SpellbookFilterPressedName == Cbs[i].Name
			&& FPlatformTime::Seconds() < SpellbookFilterPressedUntil;
		Box->SetVisibility(ESlateVisibility::HitTestInvisible);
		const int32 Did = bPressed ? DidCheckboxPressed : (bOn ? DidCheckboxOn : DidCheckboxOff);
		SetIconDid(Box, Did);
		// Pin to the authored 13×13 image child — never stretch to the full filter row.
		// Keep the DAT image visible for hit-test ancestry; overlay paints the on/off glyph.
		TSharedPtr<FACEUIElement> ImgEl;
		for (const TSharedPtr<FACEUIElement>& Child : El->Children)
		{
			if (Child.IsValid() && (Child->ElementName == TEXT("image") || Child->ImageFileId != 0))
			{
				ImgEl = Child;
				Child->bVisible = true;
				break;
			}
		}
		const TSharedPtr<FACEUIElement>& PlaceEl = ImgEl.IsValid() ? ImgEl : El;
		const FIntPoint Origin = PlaceEl->GetScreenOrigin();
		const float SX = Canvas->GetLastScaleX();
		const float SY = Canvas->GetLastScaleY();
		if (Box->GetParent() != Canvas->GetElementLayer())
		{
			Canvas->GetElementLayer()->AddChild(Box);
		}
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Box->Slot))
		{
			Slot->SetAnchors(FAnchors(0.f, 0.f));
			Slot->SetAlignment(FVector2D(0.f, 0.f));
			Slot->SetAutoSize(false);
			const float ImgY = ImgEl.IsValid() ? 0.f : 1.f;
			Slot->SetPosition(FVector2D(
				static_cast<float>(Origin.X) * SX,
				(static_cast<float>(Origin.Y) + ImgY) * SY));
			Slot->SetSize(FVector2D(13.f * SX, 13.f * SY));
			Canvas->SetOverlayOrder(Box, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ);
		}
	}
}

bool UACEUIGameplayBinder::IsPointerOverSpellbookList(FVector2D CanvasLocalPos) const
{
	if (!Manager || ActivePanelPage != TEXT("SpellManagementPanel_Field")
		|| ActiveSpellPanelTab != TEXT("SpellbookPage"))
	{
		return false;
	}
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("SpellbookPage"), TEXT("SpellBook_SpellList"));
	TSharedPtr<FACEUIElement> BarEl = Manager->FindElementUnder(
		TEXT("SpellbookPage"), TEXT("SpellBook_SpellList_Scrollbar"));
	auto Contains = [&](const TSharedPtr<FACEUIElement>& El) -> bool
	{
		if (!El.IsValid() || !El->bVisible || El->Width <= 0 || El->Height <= 0)
		{
			return false;
		}
		const FIntPoint O = El->GetScreenOrigin();
		return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
			&& CanvasLocalPos.X < O.X + El->Width && CanvasLocalPos.Y < O.Y + El->Height;
	};
	return Contains(ListEl) || Contains(BarEl);
}

bool UACEUIGameplayBinder::IsPointerOverEffectsList(FVector2D CanvasLocalPos) const
{
	if (!Manager
		|| (ActivePanelPage != TEXT("PositiveEffectsPanel_Field")
			&& ActivePanelPage != TEXT("NegativeEffectsPanel_Field")))
	{
		return false;
	}
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(ActivePanelPage, TEXT("Effects_SpellList"));
	TSharedPtr<FACEUIElement> BarEl = Manager->FindElementUnder(ActivePanelPage, TEXT("Effects_SpellList_Scrollbar"));
	auto Contains = [&](const TSharedPtr<FACEUIElement>& El) -> bool
	{
		if (!El.IsValid() || !El->bVisible || El->Width <= 0 || El->Height <= 0)
		{
			return false;
		}
		const FIntPoint O = El->GetScreenOrigin();
		return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
			&& CanvasLocalPos.X < O.X + El->Width && CanvasLocalPos.Y < O.Y + El->Height;
	};
	return Contains(ListEl) || Contains(BarEl);
}

bool UACEUIGameplayBinder::ScrollEffectsList(float WheelDelta)
{
	if (ActivePanelPage != TEXT("PositiveEffectsPanel_Field")
		&& ActivePanelPage != TEXT("NegativeEffectsPanel_Field"))
	{
		return false;
	}
	const int32 Dir = (WheelDelta > 0.f) ? -1 : 1;
	EffectsScrollOffset = FMath::Max(0, EffectsScrollOffset + Dir);
	RefreshEffectsOverlays(ActivePanelPage == TEXT("PositiveEffectsPanel_Field"));
	return true;
}

bool UACEUIGameplayBinder::ScrollSpellbook(float WheelDelta)
{
	if (!Manager || ActivePanelPage != TEXT("SpellManagementPanel_Field")
		|| ActiveSpellPanelTab != TEXT("SpellbookPage"))
	{
		return false;
	}
	const int32 Dir = (WheelDelta > 0.f) ? -1 : 1;
	SpellbookScrollOffset = FMath::Max(0, SpellbookScrollOffset + Dir * 3);
	RefreshSpellbookOverlays();
	SyncSpellbookScrollbar();
	return true;
}

void UACEUIGameplayBinder::SyncStatListScrollbar()
{
	if (!Manager || ActivePanelPage != TEXT("SkillManagementPanel_Field"))
	{
		return;
	}
	const FString PageName = ActiveSkillTab.IsEmpty() ? TEXT("AttributePage") : ActiveSkillTab;
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(PageName, TEXT("StatManagement_List"));
	TSharedPtr<FACEUIElement> Bar = Manager->FindElementUnder(PageName, TEXT("StatManagement_List_Scrollbar"));
	if (!ListEl.IsValid() || !Bar.IsValid())
	{
		return;
	}
	Bar->Height = ListEl->Height;
	Bar->Y = ListEl->Y;
	const int32 Count = PageName == TEXT("SkillPage") ? SkillListContentCount : 9;
	const int32 PageSize = FMath::Max(1, ListEl->Height / 18);
	const int32 MaxOff = FMath::Max(0, Count - PageSize);
	int32& Offset = PageName == TEXT("SkillPage") ? SkillListScrollOffset : AttributeScrollOffset;
	Offset = FMath::Clamp(Offset, 0, MaxOff);
	Bar->bVisible = MaxOff > 0;
	SyncDatScrollbar(Bar, MaxOff ? float(Offset)/MaxOff : 0.f, Count ? FMath::Min(1.f, float(PageSize)/Count) : 1.f);
}

void UACEUIGameplayBinder::SyncInventoryScrollbars()
{
	if (!Manager || ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		return;
	}
	TSharedPtr<FACEUIElement> GridEl = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList"));
	TSharedPtr<FACEUIElement> ItemBar = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList_Scrollbar"));
	if (GridEl.IsValid() && ItemBar.IsValid())
	{
		constexpr int32 Cell = 32;
		const int32 Cols = FMath::Max(1, GridEl->Width / Cell);
		const int32 Rows = FMath::Max(1, GridEl->Height / Cell);
		const int32 PageSize = Cols * Rows;
		FACEWorldObject PackObj;
		int32 Capacity = PageSize;
		if (Client && Client->GetWorldObject(SelectedPackGuid, PackObj) && PackObj.ItemsCapacity > 0)
		{
			Capacity = PackObj.ItemsCapacity;
		}
		else if (Client && SelectedPackGuid == Client->GetPlayerGuid())
		{
			Capacity = 102;
		}
		else
		{
			Capacity = 24;
		}
		Capacity = FMath::Max(Capacity, PageSize);
		const int32 MaxOff = FMath::Max(0, Capacity - PageSize);
		InventoryScrollOffset = FMath::Clamp(InventoryScrollOffset - (InventoryScrollOffset % Cols), 0, MaxOff);
		const float Frac = MaxOff > 0
			? FMath::Clamp(static_cast<float>(InventoryScrollOffset) / static_cast<float>(MaxOff), 0.f, 1.f)
			: 0.f;
		// Authored: grid x=15 w=192, scrollbar x=207 (flush right of grid — not overlapping).
		ItemBar->X = GridEl->X + GridEl->Width;
		ItemBar->Y = GridEl->Y;
		ItemBar->Height = GridEl->Height;
		ItemBar->bVisible = MaxOff > 0;
		const float VisibleFrac = Capacity > 0
			? FMath::Clamp(static_cast<float>(PageSize) / static_cast<float>(Capacity), 0.08f, 1.f)
			: 1.f;
		if (ItemBar->bVisible)
		{
			SyncDatScrollbar(ItemBar, Frac, VisibleFrac);
		}
	}

	TSharedPtr<FACEUIElement> PackList = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_ContainerList"));
	TSharedPtr<FACEUIElement> PackBar = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_ContainerList_Scrollbar"));
	if (PackList.IsValid() && PackBar.IsValid() && Client)
	{
		FACEWorldObject SelfObj;
		Client->GetWorldObject(Client->GetPlayerGuid(), SelfObj);
		const int32 SidePackCapacity = FMath::Clamp(
			SelfObj.ContainersCapacity > 0 ? SelfObj.ContainersCapacity : 7, 1, 24);
		constexpr int32 Cell = 36;
		const int32 MaxVisible = FMath::Max(1, PackList->Height / Cell);
		const int32 MaxOff = FMath::Max(0, SidePackCapacity - MaxVisible);
		PackScrollOffset = FMath::Clamp(PackScrollOffset, 0, MaxOff);
		const float Frac = MaxOff > 0
			? FMath::Clamp(static_cast<float>(PackScrollOffset) / static_cast<float>(MaxOff), 0.f, 1.f)
			: 0.f;
		// Authored: list x=6 w=36, bar x=41 — left of Inv_BurdenBar at x=44.
		PackBar->X = PackList->X + PackList->Width;
		PackBar->Y = PackList->Y;
		PackBar->Height = PackList->Height;
		PackBar->bVisible = MaxOff > 0;
		const float VisibleFrac = SidePackCapacity > 0
			? FMath::Clamp(static_cast<float>(MaxVisible) / static_cast<float>(SidePackCapacity), 0.08f, 1.f)
			: 1.f;
		if (PackBar->bVisible)
		{
			SyncDatScrollbar(PackBar, Frac, VisibleFrac);
		}
	}
}

bool UACEUIGameplayBinder::IsPointerOverInventoryGrid(FVector2D CanvasLocalPos) const
{
	if (!Manager || ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		return false;
	}
	TSharedPtr<FACEUIElement> GridEl = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList"));
	TSharedPtr<FACEUIElement> BarEl = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList_Scrollbar"));
	auto Contains = [&](const TSharedPtr<FACEUIElement>& El) -> bool
	{
		if (!El.IsValid() || !El->bVisible)
		{
			return false;
		}
		const FIntPoint O = El->GetScreenOrigin();
		return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
			&& CanvasLocalPos.X < O.X + El->Width && CanvasLocalPos.Y < O.Y + El->Height;
	};
	return Contains(GridEl) || Contains(BarEl);
}

bool UACEUIGameplayBinder::IsPointerOverPackList(FVector2D CanvasLocalPos) const
{
	if (!Manager || ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		return false;
	}
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementByName(TEXT("Inv_ContainerList"));
	TSharedPtr<FACEUIElement> BarEl = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_ContainerList_Scrollbar"));
	if (!BarEl.IsValid())
	{
		BarEl = Manager->FindElementByName(TEXT("Inv_ContainerList_Scrollbar"));
	}
	auto Contains = [&](const TSharedPtr<FACEUIElement>& El) -> bool
	{
		if (!El.IsValid() || !El->bVisible)
		{
			return false;
		}
		const FIntPoint O = El->GetScreenOrigin();
		return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
			&& CanvasLocalPos.X < O.X + El->Width && CanvasLocalPos.Y < O.Y + El->Height;
	};
	return Contains(ListEl) || Contains(BarEl);
}

bool UACEUIGameplayBinder::ScrollPackList(float WheelDelta)
{
	if (!Manager || ActivePanelPage != TEXT("InventoryPanel_Field") || !Client)
	{
		return false;
	}
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementByName(TEXT("Inv_ContainerList"));
	if (!ListEl.IsValid())
	{
		return false;
	}
	FACEWorldObject SelfObj;
	Client->GetWorldObject(Client->GetPlayerGuid(), SelfObj);
	const int32 SidePackCapacity = FMath::Clamp(
		SelfObj.ContainersCapacity > 0 ? SelfObj.ContainersCapacity : 7, 1, 24);
	constexpr int32 Cell = 36;
	const int32 MaxVisible = FMath::Max(1, ListEl->Height / Cell);
	const int32 MaxOff = FMath::Max(0, SidePackCapacity - MaxVisible);
	const int32 Dir = (WheelDelta > 0.f) ? -1 : 1;
	PackScrollOffset = FMath::Clamp(PackScrollOffset + Dir, 0, MaxOff);
	SyncInventoryScrollbars();
	RefreshInventoryOverlays();
	return true;
}

bool UACEUIGameplayBinder::ScrollInventoryGrid(float WheelDelta)
{
	if (!Manager || ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		return false;
	}
	TSharedPtr<FACEUIElement> GridEl = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList"));
	if (!GridEl.IsValid())
	{
		return false;
	}
	constexpr int32 Cell = 32;
	const int32 Cols = FMath::Max(1, GridEl->Width / Cell);
	const int32 Rows = FMath::Max(1, GridEl->Height / Cell);
	const int32 PageSize = Cols * Rows;
	FACEWorldObject PackObj;
	int32 Capacity = PageSize;
	if (Client && Client->GetWorldObject(SelectedPackGuid, PackObj) && PackObj.ItemsCapacity > 0)
	{
		Capacity = PackObj.ItemsCapacity;
	}
	Capacity = FMath::Max(Capacity, PageSize);
	const int32 MaxOff = FMath::Max(0, Capacity - PageSize);
	const int32 Dir = (WheelDelta > 0.f) ? -Cols : Cols; // wheel up → scroll toward start
	InventoryScrollOffset = FMath::Clamp(InventoryScrollOffset + Dir, 0, MaxOff);
	InventoryScrollOffset -= InventoryScrollOffset % Cols;
	SyncInventoryScrollbars();
	RefreshInventoryOverlays();
	return true;
}

bool UACEUIGameplayBinder::IsPointerOverStatList(FVector2D CanvasLocalPos) const
{
	if (!Manager || ActivePanelPage != TEXT("SkillManagementPanel_Field"))
	{
		return false;
	}
	const FString Page = ActiveSkillTab.IsEmpty() ? TEXT("AttributePage") : ActiveSkillTab;
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(Page, TEXT("StatManagement_List"));
	if (!ListEl.IsValid() && Page == TEXT("CharacterTitlePage"))
	{
		ListEl = Manager->FindElementUnder(Page, TEXT("CharacterTitle_ListBox"));
	}
	TSharedPtr<FACEUIElement> BarEl = Manager->FindElementUnder(Page, TEXT("StatManagement_List_Scrollbar"));
	auto Contains = [&](const TSharedPtr<FACEUIElement>& El) -> bool
	{
		if (!El.IsValid() || !El->bVisible || El->Width <= 0 || El->Height <= 0)
		{
			return false;
		}
		const FIntPoint O = El->GetScreenOrigin();
		return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
			&& CanvasLocalPos.X < O.X + El->Width && CanvasLocalPos.Y < O.Y + El->Height;
	};
	return Contains(ListEl) || Contains(BarEl);
}

bool UACEUIGameplayBinder::GetStatTooltipAt(FVector2D Absolute, FString& OutText, uint32* OutTemplateId) const
{
	if (OutTemplateId) *OutTemplateId = UACEHoverTooltipWidget::OptionsTemplate;
    if (Canvas) for (const auto& Entry : SpellBarIcons)
    {
        const auto* Icon=Entry.Get();
        if (Icon && Icon->GetVisibility()!=ESlateVisibility::Collapsed && Canvas->IsWidgetExposedAt(Icon,Absolute))
        {
            OutText=Icon->GetToolTipText().ToString();
            if (OutTemplateId) *OutTemplateId=UACEHoverTooltipWidget::ObjectTemplate;
            return !OutText.IsEmpty();
        }
    }
	if (GetMapTooltipAt(Absolute, OutText))
	{
		if (OutTemplateId) *OutTemplateId = UACEHoverTooltipWidget::MapTemplate;
		return true;
	}
	if (Canvas && BuiltInSpellId && BuiltInSpellIconBorders.Num() > 0)
	{
		const auto* Icon = BuiltInSpellIconBorders[0].Get();
		if (Icon && Icon->GetVisibility() != ESlateVisibility::Collapsed && Canvas->IsWidgetExposedAt(Icon, Absolute))
		{
			OutText = Icon->GetToolTipText().ToString();
			if (OutTemplateId) *OutTemplateId = UACEHoverTooltipWidget::ObjectTemplate;
			return !OutText.IsEmpty();
		}
	}
	if (Canvas && CastSpellLabel && CastSpellLabel->GetVisibility() != ESlateVisibility::Collapsed
		&& Canvas->IsWidgetExposedAt(CastSpellLabel, Absolute))
	{
		OutText = CastSpellLabel->GetToolTipText().ToString();
		return !OutText.IsEmpty();
	}
	if (!Canvas || ActivePanelPage != TEXT("SkillManagementPanel_Field")) return false;
	const bool Attributes = ActiveSkillTab == TEXT("AttributePage");
	if (!Attributes && ActiveSkillTab != TEXT("SkillPage")) return false;
	const auto& Values = Attributes ? AttributeRowValues : SkillRowValues;
	const auto& Names = Attributes ? AttributeRows : SkillRows;
	for (int32 I = 0; I < Values.Num(); ++I)
	{
		if (!Values[I] || Values[I]->GetVisibility() == ESlateVisibility::Collapsed) continue;
		if (Canvas->IsWidgetExposedAt(Values[I], Absolute)
			|| (Names.IsValidIndex(I) && Names[I] && Canvas->IsWidgetExposedAt(Names[I], Absolute))
			|| (!Attributes && SkillRowHighlights.IsValidIndex(I) && SkillRowHighlights[I]
				&& Canvas->IsWidgetExposedAt(SkillRowHighlights[I], Absolute)))
		{
			OutText = Values[I]->GetToolTipText().ToString();
			if (Attributes && Names.IsValidIndex(I) && Names[I] && !OutText.IsEmpty()) OutText = Names[I]->GetText().ToString() + TEXT("\n") + OutText;
			return !OutText.IsEmpty();
		}
	}
	return false;
}

bool UACEUIGameplayBinder::ScrollStatList(float WheelDelta)
{
	if (!Manager || ActivePanelPage != TEXT("SkillManagementPanel_Field"))
	{
		return false;
	}
	const int32 Dir = (WheelDelta > 0.f) ? -1 : 1;
	if (ActiveSkillTab == TEXT("AttributePage"))
	{
		AttributeScrollOffset += Dir * 3;
		RefreshAttributeOverlays();
		return true;
	}
	if (ActiveSkillTab == TEXT("CharacterTitlePage"))
	{
		TitleListScrollOffset = FMath::Max(0, TitleListScrollOffset + Dir * 3);
		RefreshTitleOverlays();
		return true;
	}
	if (ActiveSkillTab != TEXT("SkillPage"))
	{
		return false;
	}
	SkillListScrollOffset = FMath::Max(0, SkillListScrollOffset + Dir * 3);
	RefreshSkillOverlays();
	SyncStatListScrollbar();
	return true;
}

bool UACEUIGameplayBinder::IsChatEntryFocused() const
{
	if (!ACEInputBindings::IsTextEntryFocused()) return false;
	if (StackAmountEntry && StackAmountEntry->HasKeyboardFocus()) return true;
	// This gate also suppresses movement keys while editing other HUD text.
	if (SquelchNameEntry && SquelchNameEntry->HasKeyboardFocus()) return true;
	if (JournalNotes && JournalNotes->HasKeyboardFocus()) return true;
	if (BookPageEditor && BookPageEditor->HasKeyboardFocus()) return true;
	for (const auto& Entry : JournalEntries) if (Entry && Entry->HasKeyboardFocus()) return true;
	if (EditingInscriptionGuid && ExamInscriptionEditor && ExamInscriptionEditor->HasKeyboardFocus()) return true;
	for (const auto& Entry : ComponentDesiredEntries) if (Entry && Entry->HasKeyboardFocus()) return true;
	if (ChatEntry && ChatEntry->HasKeyboardFocus())
	{
		return true;
	}
	for (const TObjectPtr<UEditableTextBox>& Entry : FloatyChatEntries)
	{
		if (Entry && Entry->HasKeyboardFocus())
		{
			return true;
		}
	}
	if (FellowshipNameEntry && FellowshipNameEntry->HasKeyboardFocus())
	{
		return true;
	}
	if (FriendNameEntry && FriendNameEntry->HasKeyboardFocus())
	{
		return true;
	}
	return false;
}

int32 UACEUIGameplayBinder::HitTestShortcutSlot(FVector2D CanvasLocalPos) const
{
	if (!Manager || !Canvas) return INDEX_NONE;
	const FVector2D P = Canvas->ViewportToLayout(CanvasLocalPos);
	const auto Body = Manager->FindElementByName(TEXT("ToolbarField"));
	if (!Body) return INDEX_NONE;
	const FIntPoint BodyOrigin = Body->GetScreenOrigin();
	if (P.X < BodyOrigin.X || P.Y < BodyOrigin.Y
		|| P.X >= BodyOrigin.X + Body->Width || P.Y >= BodyOrigin.Y + Body->Height) return INDEX_NONE;
	for (int32 Index = 0; Index < 18; ++Index)
	{
		const auto El = Manager->FindElementByName(FString::Printf(
			TEXT("ShortcutBar%s_Shortcut%dButton"), Index < 9 ? TEXT("") : TEXT("2"), Index % 9 + 1));
		if (!El || !Canvas->IsElementExposedAt(El, CanvasLocalPos)) continue;
		const FIntPoint O = El->GetScreenOrigin();
		if (P.X >= O.X && P.Y >= O.Y && P.X < O.X + El->Width && P.Y < O.Y + El->Height) return Index;
	}
	return INDEX_NONE;
}

bool UACEUIGameplayBinder::IsPointerOverShortcutBar(FVector2D CanvasLocalPos) const
{
	return HitTestShortcutSlot(CanvasLocalPos) != INDEX_NONE;
}

bool UACEUIGameplayBinder::ScrollShortcutBar(float WheelDelta)
{
	// Resizing reveals the second retail row; wheel input must not remap its bindings.
	return false;
}

void UACEUIGameplayBinder::CastSelectedHotbarSpell()
{
	if (!Client)
	{
		return;
	}
	SyncSpellTabSelection();
	int32 ToCast = 0;
	if (SelectedCombatSpellSlot < 0)
	{
		ToCast = BuiltInSpellId;
	}
	else
	{
        const TArray<int32> Bar = Client->GetSpellBar(Client->GetActiveSpellBar());
        ToCast = Bar.IsValidIndex(SelectedCombatSpellSlot) ? Bar[SelectedCombatSpellSlot] : 0;
	}
	if (ToCast == 0)
	{
		return;
	}
	const int32 CasterGuid = (SelectedCombatSpellSlot < 0) ? BuiltInCasterGuid : 0;
	if (!Client->SendCastSpell(ToCast, CasterGuid))
	{
		PostInventorySystemMessage(Client->IsInPortalSpace() ? TEXT("You're too busy!") : TEXT("You must select a target for that spell."));
	}
}

void UACEUIGameplayBinder::RefreshChatChromeOverlays()
{
	RefreshChatEntryLayouts();
	RefreshChatOpacity();
	if (!Canvas || !Canvas->WidgetTree || !Manager)
	{
		return;
	}
	EnsureOverlays();
	if (const auto Maximize = Manager->FindElementUnder(TEXT("RootGameplay_FloatyMainChat_Field"), TEXT("MaximizeButton")))
	{
		// Retail changes this button's explicit maximized/minimized state. Normal
		// button rollover states have no artwork and otherwise hide the control.
		Maximize->bUseExplicitState = true;
		const auto MainChat = Manager->FindElementByName(TEXT("RootGameplay_FloatyMainChat_Field"));
		Maximize->DefaultState = MainChat && MainChat->ChatPeekRestore.IsSet() ? 0x10000047 : 0x10000048;
	}
	if (!ChatTargetLabel)
	{
		ChatTargetLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		ChatTargetLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		ChatTargetLabel->SetJustification(ETextJustify::Center);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 8);
		ChatTargetLabel->SetFont(Font);
		ChatTargetLabel->SetColorAndOpacity(FSlateColor(TextGold));
	}
	if (!ChatSendLabel)
	{
		ChatSendLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		ChatSendLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		ChatSendLabel->SetJustification(ETextJustify::Center);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 8);
		ChatSendLabel->SetFont(Font);
		ChatSendLabel->SetColorAndOpacity(FSlateColor(TextWhite));
	}
	const FString Dest = ChatSendChannelLabel(ChatSendChannel);
	while (ChatWindowLabels.Num() < 4)
	{
		ChatWindowLabels.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FString ButtonName = FString::Printf(TEXT("FloatingChat%d"), Index + 1);
		PlaceTextOnElement(ChatWindowLabels[Index], ButtonName, FString::FromInt(Index + 1), 8, TextWhite, 540);
		if (TSharedPtr<FACEUIElement> Button = Manager->FindElementByName(ButtonName))
		{
			const auto Window = Manager->FindElementByName(FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"), Index + 1));
			Button->bHighlighted = Window.IsValid() && Window->bVisible;
		}
	}
	PlaceTextOnElement(ChatTargetLabel, TEXT("ChatTargetButtonText"), Dest, 8, TextGold, 540);
	// Also paint ChatTarget itself when ButtonText node is missing.
	if (TSharedPtr<FACEUIElement> TargetBtn = Manager->FindElementUnder(
		TEXT("RootGameplay_FloatyMainChat_Field"), TEXT("ChatTarget")))
	{
		if (!Manager->FindElementUnder(TEXT("RootGameplay_FloatyMainChat_Field"), TEXT("ChatTargetButtonText")).IsValid())
		{
			PlaceTextOnElement(ChatTargetLabel, TEXT("ChatTarget"), Dest, 8, TextGold, 540);
		}
	}
	// SendButton has no child text node — place label on the button itself.
	if (TSharedPtr<FACEUIElement> Send = Manager->FindElementUnder(
		TEXT("RootGameplay_FloatyMainChat_Field"), TEXT("SendButton")))
	{
		ChatSendLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		ChatSendLabel->SetText(FText::FromString(TEXT("Send")));
		Canvas->PlaceWidgetAtElement(ChatSendLabel, Send, 540, FMargin(2.f, 1.f));
	}
	else
	{
		ChatSendLabel->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (bChatTargetPopupOpen)
	{
		RefreshChatTargetPopup();
	}
}

void UACEUIGameplayBinder::RefreshShortcutOverlays()
{
	if (!Client || !Manager || !Canvas || !Canvas->WidgetTree)
	{
		if (ShortcutClipPanel) ShortcutClipPanel->SetVisibility(ESlateVisibility::Collapsed);
		for (UBorder* B : ShortcutIcons)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : ShortcutSlotBgs)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : ShortcutNumberIcons)
			if (B) B->SetVisibility(ESlateVisibility::Collapsed);
		for (UBorder* B : ShortcutSelectedOverlays)
			if (B) B->SetVisibility(ESlateVisibility::Collapsed);
		for (UBorder* B : ShortcutCooldownOverlays)
			if (B) B->SetVisibility(ESlateVisibility::Collapsed);
		for (UTextBlock* T : ShortcutSlotNumbers)
		{
			if (T) { T->SetVisibility(ESlateVisibility::Collapsed); }
		}
		return;
	}
	const auto Body = Manager->FindElementByName(TEXT("ToolbarField"));
	if (!Body || !Body->IsPaintVisible())
	{
		if (ShortcutClipPanel) ShortcutClipPanel->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	if (!ShortcutClipPanel)
	{
		ShortcutClipPanel = Canvas->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		ShortcutClipPanel->SetClipping(EWidgetClipping::ClipToBoundsAlways);
	}
	ShortcutClipPanel->SetVisibility(ESlateVisibility::HitTestInvisible);
	Canvas->PlaceWidgetAtElement(ShortcutClipPanel, Body, 529);
	const FVector2D Scale = Canvas->GetLastScale2D();
	const FIntPoint BodyOrigin = Body->GetScreenOrigin();
	const auto ShortcutSession = Client->GetSession();
	const int32 SelectedGuid = LastSelection.bValid ? LastSelection.Guid : 0;
	const double Now = FPlatformTime::Seconds();
	auto PlaceShortcut = [&](UWidget* Widget, const TSharedPtr<FACEUIElement>& El, int32 ZOrder)
	{
		if (Widget->GetParent() != ShortcutClipPanel)
		{
			Widget->RemoveFromParent();
			ShortcutClipPanel->AddChild(Widget);
		}
		const FIntPoint Origin = El->GetScreenOrigin() - BodyOrigin;
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Widget->Slot))
		{
			Slot->SetPosition(FVector2D(Origin.X, Origin.Y) * Scale);
			Slot->SetSize(FVector2D(El->Width, El->Height) * Scale);
			Slot->SetZOrder(ZOrder);
		}
	};
	auto PlaceSlot = [&](const FString& ElementName, int32 SlotIndex0, int32 ArrayIndex)
	{
		TSharedPtr<FACEUIElement> El = Manager->FindElementByName(ElementName);
		UBorder* Bg = EnsureIconBorder(ShortcutSlotBgs, ArrayIndex);
		UBorder* Icon = EnsureIconBorder(ShortcutIcons, ArrayIndex);
		UBorder* Number = EnsureIconBorder(ShortcutNumberIcons, ArrayIndex);
		UBorder* Selected = EnsureIconBorder(ShortcutSelectedOverlays, ArrayIndex);
		UBorder* Cooldown = EnsureIconBorder(ShortcutCooldownOverlays, ArrayIndex);
		UTextBlock* Num = ShortcutSlotNumbers.IsValidIndex(ArrayIndex) ? ShortcutSlotNumbers[ArrayIndex] : nullptr;
		if (Num) { Num->SetVisibility(ESlateVisibility::Collapsed); }
		if (!El.IsValid() || !Icon || !Bg)
		{
			if (Icon) { Icon->SetVisibility(ESlateVisibility::Collapsed); }
			if (Bg) { Bg->SetVisibility(ESlateVisibility::Collapsed); }
			if (Number) Number->SetVisibility(ESlateVisibility::Collapsed);
			if (Selected) Selected->SetVisibility(ESlateVisibility::Collapsed);
			if (Cooldown) Cooldown->SetVisibility(ESlateVisibility::Collapsed);
			return;
		}
		bool bAncestorsVisible = El->bVisible;
		for (TSharedPtr<FACEUIElement> P = El->Parent.Pin(); P.IsValid(); P = P->Parent.Pin())
		{
			if (!P->bVisible) { bAncestorsVisible = false; break; }
		}
		const FIntPoint SlotOrigin = El->GetScreenOrigin() - BodyOrigin;
		if (!bAncestorsVisible || SlotOrigin.Y >= Body->Height || SlotOrigin.X >= Body->Width
			|| SlotOrigin.Y + El->Height <= 0 || SlotOrigin.X + El->Width <= 0)
		{
			Icon->SetVisibility(ESlateVisibility::Collapsed);
			Bg->SetVisibility(ESlateVisibility::Collapsed);
			if (Number) Number->SetVisibility(ESlateVisibility::Collapsed);
			if (Selected) Selected->SetVisibility(ESlateVisibility::Collapsed);
			if (Cooldown) Cooldown->SetVisibility(ESlateVisibility::Collapsed);
			return;
		}
		// Retail ItemSlot_Shortcut_01..09 / _10 (key 0) numbered empty backgrounds.
		// Occupied slots also carry IconUnderlay / UiEffects like the inventory grid.

		const int32 Guid = Client->GetShortcutObject(SlotIndex0);
		const FACEWorldObject* Obj = Guid != 0 && ShortcutSession
			? ShortcutSession->GetWorldObjects().Find(Guid) : nullptr;
		// The main-pack shortcut has a presentation-only icon. Ordinary shortcuts
		// read current item state without copying model/property arrays each frame.
		FACEWorldObject MainPackIcon;
		if (Obj && Guid == Client->GetPlayerGuid())
		{
			MainPackIcon = *Obj; MainPackIcon.IconId = 0x06004CF7u; Obj = &MainPackIcon;
		}
		const bool bHave = Obj != nullptr;
		
		Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (bHave)
		{
			SetItemSlotBackground(Bg, Obj);
		}
		else
		{
			// DAT ItemSlot_Shortcut_10..18 all use the same blue empty art.
			SetIconDid(Bg, SlotIndex0 < 9 ? 0x060010FA + SlotIndex0 : 0x060074CF);
		}
		PlaceShortcut(Bg, El, 0);
		Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
		SetItemSlotForeground(Icon, Obj);
		SetRetailTooltip(Icon, Obj ? FText::FromString(ACERetailObjectNames::Name(*Obj)) : FText());
		PlaceShortcut(Icon, El, 1);
		if (Number)
		{
			Number->SetVisibility(bHave && SlotIndex0 < 10 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			SetIconDid(Number, SlotIndex0 < 9 ? 0x0600109E + SlotIndex0 : 0x060074D3);
			PlaceShortcut(Number, El, 2);
		}
		const bool bSelected = bHave && Guid == SelectedGuid;
		if (Selected) Selected->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (Selected && bSelected)
		{
			SetIconDid(Selected, DidInvSlotSelected);
			PlaceShortcut(Selected, El, 3);
		}
		const int32 Phase = Obj && ShortcutSession ? ShortcutSession->GetItemCooldownPhase(*Obj, Now) : 0;
		if (Cooldown) Cooldown->SetVisibility(Phase > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (Cooldown && Phase > 0)
		{
			// Retail ItemSlot_Icon_Cooldown_10..100 from ItemSlot (0x21000037).
			SetIconDid(Cooldown, 0x060067CEu + Phase);
			PlaceShortcut(Cooldown, El, 4);
		}
	};
	for (int32 i = 0; i < 18; ++i)
	{
		PlaceSlot(FString::Printf(TEXT("ShortcutBar%s_Shortcut%dButton"), i < 9 ? TEXT("") : TEXT("2"), i % 9 + 1), i, i);
	}
}

void UACEUIGameplayBinder::RefreshSkillTabLabels()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree
		|| ActivePanelPage != TEXT("SkillManagementPanel_Field"))
	{
		for (UTextBlock* L : SkillTabLabels)
		{
			if (L) { L->SetVisibility(ESlateVisibility::Collapsed); }
		}
		return;
	}
	struct FTabLabel { const TCHAR* Element; const TCHAR* Text; };
	static const FTabLabel Tabs[] = {
		{ TEXT("AttributeTab"), TEXT("Attributes") },
		{ TEXT("SkillTab"), TEXT("Skills") },
		{ TEXT("CharacterTitleTab"), TEXT("Titles") },
	};
	while (SkillTabLabels.Num() < UE_ARRAY_COUNT(Tabs) && Canvas->WidgetTree)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		L->SetJustification(ETextJustify::Center);
		SkillTabLabels.Add(L);
	}
	for (int32 i = 0; i < UE_ARRAY_COUNT(Tabs); ++i)
	{
		UTextBlock* Label = SkillTabLabels.IsValidIndex(i) ? SkillTabLabels[i] : nullptr;
		if (!Label) { continue; }
		TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(
			TEXT("SkillManagementPanel_Field"), Tabs[i].Element);
		if (!El.IsValid())
		{
			Label->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		Label->SetVisibility(ESlateVisibility::HitTestInvisible);
		Label->SetText(FText::FromString(Tabs[i].Text));
		const bool bSel =
			(i == 0 && ActiveSkillTab == TEXT("AttributePage"))
			|| (i == 1 && ActiveSkillTab == TEXT("SkillPage"))
			|| (i == 2 && ActiveSkillTab == TEXT("CharacterTitlePage"));
		Label->SetColorAndOpacity(FSlateColor(bSel ? TextGold : FLinearColor(0.55f, 0.55f, 0.5f, 1.f)));
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(bSel ? TEXT("Bold") : TEXT("Regular"), 9);
		Label->SetFont(Font);
		// DAT image paint climbs past 20k as SyncElementRecursive increments z per element —
		// 575 buried these labels under the tab chrome ("shadow over tab names").
		Canvas->PlaceWidgetAtElement(Label, El, 100000, FMargin(14.f, 4.f, 8.f, 4.f));
	}
}

void UACEUIGameplayBinder::RefreshPanelTitleOverlay()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree || ActivePanelPage.IsEmpty())
	{
		if (PanelTitleLabel) { PanelTitleLabel->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	FString Title;
	if (ActivePanelPage == TEXT("InventoryPanel_Field")) { Title = TEXT("Inventory"); }
	else if (ActivePanelPage == TEXT("SkillManagementPanel_Field"))
	{
		if (ActiveSkillTab == TEXT("SkillPage")) { Title = TEXT("Skills"); }
		else if (ActiveSkillTab == TEXT("CharacterTitlePage")) { Title = TEXT("Titles"); }
		else { Title = TEXT("Attributes"); }
	}
	else if (ActivePanelPage == TEXT("SpellManagementPanel_Field")) { Title = TEXT("Spellbook"); }
	else if (ActivePanelPage == TEXT("SocialPanel_Field")) { Title = TEXT("Social"); }
	else if (ActivePanelPage == TEXT("QuestManagementPanel_Field")) { Title = TEXT("Quests"); }
	if (Title.IsEmpty())
	{
		if (PanelTitleLabel) { PanelTitleLabel->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	if (!PanelTitleLabel)
	{
		PanelTitleLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		PanelTitleLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		PanelTitleLabel->SetJustification(ETextJustify::Center);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11);
		PanelTitleLabel->SetFont(Font);
		PanelTitleLabel->SetColorAndOpacity(FSlateColor(TextGold));
	}
	TSharedPtr<FACEUIElement> TitleEl = Manager->FindElementUnder(ActivePanelPage, TEXT("DisplayedBookNameText"));
	if (!TitleEl.IsValid())
	{
		TitleEl = Manager->FindElementUnder(ActivePanelPage, TEXT("TitleText"));
	}
	if (!TitleEl.IsValid())
	{
		TitleEl = Manager->FindElementUnder(ActivePanelPage, TEXT("TitleBackground"));
	}
	if (!TitleEl.IsValid())
	{
		TitleEl = Manager->FindElementUnder(ActivePanelPage, TEXT("TitleBar"));
	}
	if (!TitleEl.IsValid())
	{
		PanelTitleLabel->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	PanelTitleLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
	PanelTitleLabel->SetText(FText::FromString(Title));
	Canvas->PlaceWidgetAtElement(PanelTitleLabel, TitleEl, 560, FMargin(24.f, 4.f, 24.f, 4.f));
}

void UACEUIGameplayBinder::RefreshAttributeOverlays()
{
	auto CollapseAttr = [this]()
	{
		for (UTextBlock* Row : AttributeRows) { if (Row) { Row->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UTextBlock* Row : AttributeRowValues) { if (Row) { Row->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UBorder* B : AttributeRowIcons) { if (B) { B->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UBorder* B : AttributeRowHighlights) { if (B) { B->SetVisibility(ESlateVisibility::Collapsed); } }
		auto Hide = [](UTextBlock* T) { if (T) { T->SetVisibility(ESlateVisibility::Collapsed); } };
		Hide(AttrHeaderName); Hide(AttrHeaderTitle); Hide(AttrHeaderPk);
		Hide(AttrHeaderLevel); Hide(AttrHeaderXp); Hide(AttrHeaderXpToLevelLabel); Hide(AttrHeaderXpToLevel);
		Hide(AttrHeaderTotalXpLabel); Hide(AttrHeaderLevelLabel);
		Hide(AttrHeaderLuminanceLabel); Hide(AttrHeaderLuminanceValue);
		Hide(AttrFooterTitle); Hide(AttrFooterLine1Label); Hide(AttrFooterLine1Value);
		Hide(AttrFooterLine2Label); Hide(AttrFooterLine2Value);
	};
	if (!Manager || !Canvas || !Canvas->WidgetTree
		|| ActivePanelPage != TEXT("SkillManagementPanel_Field")
		|| ActiveSkillTab != TEXT("AttributePage"))
	{
		CollapseAttr();
		return;
	}
	EnsureOverlays();
	if (!LastVitals.bValid && Client)
	{
		LastVitals = Client->GetPlayerVitals();
	}
	FACEWorldObject Self;
	FString CharName = TEXT("Character");
	if (Client && Client->GetWorldObject(Client->GetPlayerGuid(), Self) && !Self.Name.IsEmpty())
	{
		CharName = Self.Name;
	}
	int64 XpNeeded = 0;
	UACEDatSubsystem* Dat = Canvas->GetResourceResolver() ? Canvas->GetResourceResolver()->GetDatSubsystem() : nullptr;
	if (PlayerController)
	{
		if (UGameInstance* GI = PlayerController->GetGameInstance())
		{
			Dat = GI->GetSubsystem<UACEDatSubsystem>();
		}
	}
	if (Dat && LastVitals.bValid)
	{
		float Progress = 0.f;
		Dat->TryGetXpToNextLevel(LastVitals.TotalExperience, LastVitals.Level, XpNeeded, &Progress);
		if (TSharedPtr<FACEUIElement> Meter = Manager->FindElementUnder(
			TEXT("AttributePage"), TEXT("StatManagement_Header_XPToLevelMeter")))
		{
			Meter->MeterFillFraction = FMath::Clamp(Progress, 0.f, 1.f);
		}
	}

	PlaceTextUnder(AttrHeaderName, TEXT("AttributePage"), TEXT("StatManagement_Header_Name"),
		CharName, 11, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderTitle, TEXT("AttributePage"), TEXT("StatManagement_Header_Title"),
		FormatGenderHeritageTitle(LastVitals), 9, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderPk, TEXT("AttributePage"), TEXT("StatManagement_Header_PKStatus"),
		FormatPkStatus(LastVitals.PlayerKillerStatus), 9, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderLevelLabel, TEXT("AttributePage"), TEXT("StatManagement_Header_LevelLabel"),
		TEXT("Character Level"), 8, TextWhite, StatOverlayZ);
	if (AttrHeaderLevel) { AttrHeaderLevel->SetJustification(ETextJustify::Center); }
	PlaceTextUnder(AttrHeaderLevel, TEXT("AttributePage"), TEXT("StatManagement_Header_LevelValue"),
		LastVitals.bValid ? FString::FromInt(LastVitals.Level) : FString(), 18, TextGold, StatOverlayZ);
	PlaceTextUnder(AttrHeaderTotalXpLabel, TEXT("AttributePage"), TEXT("StatManagement_Header_TotalXPLabel"),
		TEXT("Total Experience (XP)"), 8, TextWhite, StatOverlayZ);
	if (AttrHeaderXp) { AttrHeaderXp->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrHeaderXp, TEXT("AttributePage"), TEXT("StatManagement_Header_TotalXPValue"),
		LastVitals.bValid ? FormatXpNumber(LastVitals.TotalExperience) : FString(), 9, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderXpToLevelLabel, TEXT("AttributePage"), TEXT("StatManagement_Header_XPToLevelMeterLabel"),
		TEXT("XP for next level"), 8, TextRed, StatOverlayZ);
	if (AttrHeaderXpToLevel) { AttrHeaderXpToLevel->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrHeaderXpToLevel, TEXT("AttributePage"), TEXT("StatManagement_Header_XPToLevelMeterValue"),
		XpNeeded > 0 ? FormatXpNumber(XpNeeded) : TEXT("Max"), 8, TextWhite, StatOverlayZ);
	const bool bHasLum = LastVitals.bValid && LastVitals.MaximumLuminance > 0;
	PlaceTextUnder(AttrHeaderLuminanceLabel, TEXT("AttributePage"), TEXT("StatManagement_Header_LuminanceLabel"),
		bHasLum ? TEXT("Luminance") : FString(), 8, TextWhite, StatOverlayZ);
	if (AttrHeaderLuminanceValue) { AttrHeaderLuminanceValue->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrHeaderLuminanceValue, TEXT("AttributePage"), TEXT("StatManagement_Header_LuminanceValue"),
		bHasLum
			? FString::Printf(TEXT("%s / %s"),
				*FormatXpNumber(LastVitals.AvailableLuminance),
				*FormatXpNumber(LastVitals.MaximumLuminance))
			: FString(),
		8, TextGold, StatOverlayZ);

	struct FAttrRow { const TCHAR* Name; const TCHAR* Desc; int32 IconDid; int32 AttrId; int32 VitalId; };
	const FAttrRow Rows[] = {
		{ TEXT("Strength"), TEXT("Melee damage and burden capacity."), DidAttrIconStrength, 1, 0 },
		{ TEXT("Endurance"), TEXT("Raises Health and Stamina pools."), DidAttrIconEndurance, 2, 0 },
		{ TEXT("Coordination"), TEXT("Missile accuracy and craft skills."), DidAttrIconCoordination, 4, 0 },
		{ TEXT("Quickness"), TEXT("Run speed and melee defense."), DidAttrIconQuickness, 3, 0 },
		{ TEXT("Focus"), TEXT("Magic accuracy and mana conversion."), DidAttrIconFocus, 5, 0 },
		{ TEXT("Self"), TEXT("Mana pool and mental resistance."), DidAttrIconSelf, 6, 0 },
		{ TEXT("Health"), TEXT("Your life. Raised with Health XP."), DidAttrIconHealth, 0, 1 },
		{ TEXT("Stamina"), TEXT("Attacks and movement. Raised with Stamina XP."), DidAttrIconStamina, 0, 3 },
		{ TEXT("Mana"), TEXT("Powers spells. Raised with Mana XP."), DidAttrIconMana, 0, 5 },
	};
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("AttributePage"), TEXT("StatManagement_List"));
	if (!ListEl.IsValid())
	{
		return;
	}
	const FIntPoint Origin = ListEl->GetScreenOrigin();
	constexpr int32 RowH = 18;
	const int32 MaxRows = FMath::Min(static_cast<int32>(UE_ARRAY_COUNT(Rows)), FMath::Max(1, ListEl->Height / RowH));
	AttributeScrollOffset = FMath::Clamp(AttributeScrollOffset, 0, int32(UE_ARRAY_COUNT(Rows)) - MaxRows);
	SyncStatListScrollbar();
	if (SelectedAttributeRow < 0 || SelectedAttributeRow >= UE_ARRAY_COUNT(Rows))
	{
		SelectedAttributeRow = INDEX_NONE;
	}

	auto EnsureRowText = [this](TArray<TObjectPtr<UTextBlock>>& Arr, int32 Count, ETextJustify::Type Justify)
	{
		while (Arr.Num() < Count && Canvas->WidgetTree)
		{
			UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			Row->SetVisibility(ESlateVisibility::HitTestInvisible);
			FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
			Row->SetFont(Font);
			Row->SetJustification(Justify);
			Arr.Add(Row);
		}
	};
	EnsureRowText(AttributeRows, MaxRows, ETextJustify::Left);
	EnsureRowText(AttributeRowValues, MaxRows, ETextJustify::Right);

	for (int32 i = 0; i < AttributeRows.Num() || i < AttributeRowIcons.Num() || i < AttributeRowValues.Num(); ++i)
	{
		UBorder* Icon = EnsureIconBorder(AttributeRowIcons, i);
		UBorder* Hi = EnsureIconBorder(AttributeRowHighlights, i);
		UTextBlock* Name = AttributeRows.IsValidIndex(i) ? AttributeRows[i].Get() : nullptr;
		UTextBlock* Val = AttributeRowValues.IsValidIndex(i) ? AttributeRowValues[i].Get() : nullptr;
		if (i >= MaxRows)
		{
			if (Name) { Name->SetVisibility(ESlateVisibility::Collapsed); }
			if (Val) { Val->SetVisibility(ESlateVisibility::Collapsed); }
			if (Icon) { Icon->SetVisibility(ESlateVisibility::Collapsed); }
			if (Hi) { Hi->SetVisibility(ESlateVisibility::Collapsed); }
			continue;
		}
		FString Value;
		FLinearColor ValueColor = TextWhite;
		if (Rows[i + AttributeScrollOffset].AttrId != 0)
		{
			const int32 Current = LastVitals.GetAttributeCurrent(Rows[i + AttributeScrollOffset].AttrId);
			const int32 Base = LastVitals.GetAttributeBase(Rows[i + AttributeScrollOffset].AttrId);
			Value = FString::FromInt(Current);
			ValueColor = Current > Base ? FLinearColor::Green : Current < Base ? FLinearColor::Red : TextWhite;
			if (Val) UACEHoverTooltipWidget::SetWidgetTooltip(Val, FText::FromString(FString::Printf(TEXT("Base: %d\nCurrent: %d"), Base, Current)));
		}
		else
		{
			const int32 Id = Rows[i + AttributeScrollOffset].VitalId;
			const int32 Current = Id == 1 ? LastVitals.Health : Id == 3 ? LastVitals.Stamina : LastVitals.Mana;
			const int32 Maximum = Id == 1 ? LastVitals.MaxHealth : Id == 3 ? LastVitals.MaxStamina : LastVitals.MaxMana;
			const int32 Base = LastVitals.GetVitalBase(Id);
			Value = FString::Printf(TEXT("%d/%d"), Current, Maximum);
			ValueColor = Maximum > Base ? FLinearColor::Green : Maximum < Base ? FLinearColor::Red : TextWhite;
			if (Val) UACEHoverTooltipWidget::SetWidgetTooltip(Val, FText::FromString(FString::Printf(TEXT("Base maximum: %d\nCurrent maximum: %d"), Base, Maximum)));
		}
		const float Y = static_cast<float>(Origin.Y + i * RowH);
		const float ListW = static_cast<float>(ListEl->Width - 18);
		if (Hi)
		{
			// Retail list selection uses textured list-info fill (0x06000F93), not a solid wash.
			// Unselected rows keep an invisible (no-draw) highlight so TryHandleOverlayClick
			// still has a hit target — Collapsed rows made the attributes list unclickable.
			Hi->SetVisibility(ESlateVisibility::HitTestInvisible);
			// Clear through the same cache as selection so reselecting restores the brush.
			SetIconDid(Hi, SelectedAttributeRow == i + AttributeScrollOffset ? DidListInfoBg : 0,
				FLinearColor(1.f, 1.f, 1.f, 0.85f));
			if (Hi->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Hi);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Hi->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X), Y));
				Slot->SetSize(FVector2D(ListW, static_cast<float>(RowH)));
				Canvas->SetOverlayOrder(Hi, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ);
			}
		}
		if (Icon)
		{
			Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
			SetIconDid(Icon, Rows[i + AttributeScrollOffset].IconDid);
			if (Icon->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Icon);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Icon->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + 2), Y));
				Slot->SetSize(FVector2D(18.f, 18.f));
				Canvas->SetOverlayOrder(Icon, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ + 1);
			}
		}
		if (Name)
		{
			Name->SetText(FText::FromString(Rows[i + AttributeScrollOffset].Name));
			Name->SetVisibility(ESlateVisibility::HitTestInvisible);
			Name->SetColorAndOpacity(FSlateColor(i + AttributeScrollOffset == SelectedAttributeRow ? TextGold : TextWhite));
			if (Name->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Name);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Name->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + 24), Y));
				Slot->SetSize(FVector2D(ListW - 100.f, static_cast<float>(RowH)));
				Canvas->SetOverlayOrder(Name, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ + 2);
			}
		}
		if (Val)
		{
			Val->SetText(FText::FromString(Value));
			Val->SetVisibility(ESlateVisibility::HitTestInvisible);
			Val->SetJustification(ETextJustify::Right);
			Val->SetColorAndOpacity(FSlateColor(
				ValueColor));
			if (Val->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Val);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Val->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + ListW - 72.f), Y));
				Slot->SetSize(FVector2D(70.f, static_cast<float>(RowH)));
				Canvas->SetOverlayOrder(Val, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ + 2);
			}
		}
	}

	// Retail Footer_Text: XP to raise for the selected attribute + unassigned XP.
	const bool bHasSel = SelectedAttributeRow >= 0 && SelectedAttributeRow < 9;
	SyncStatFooterChrome();
	int32 SelXpSpent = 0;
	int64 SelXpToNext = 0;
	int64 SelMaxSpend = 0;
	bool bAttributeXpKnown = false;
	if (bHasSel && Dat)
	{
		const FAttrRow& Sel = Rows[SelectedAttributeRow];
		if (Sel.AttrId != 0)
		{
			SelXpSpent = LastVitals.GetAttributeXpSpent(Sel.AttrId);
			bAttributeXpKnown = Dat->TryGetAttributeXpToNextRank(SelXpSpent, SelXpToNext, &SelMaxSpend);
		}
		else
		{
			switch (Sel.VitalId)
			{
			case 1: SelXpSpent = LastVitals.HealthXpSpent; break;
			case 3: SelXpSpent = LastVitals.StaminaXpSpent; break;
			case 5: SelXpSpent = LastVitals.ManaXpSpent; break;
			default: break;
			}
			bAttributeXpKnown = Dat->TryGetVitalXpToNextRank(SelXpSpent, SelXpToNext, &SelMaxSpend);
		}
	}
	FString StatTitle = TEXT("Select an Attribute to Improve");
	int32 Bonus = 0;
	if (bHasSel)
	{
		const FAttrRow& Sel = Rows[SelectedAttributeRow];
		const int32 Current = Sel.AttrId ? LastVitals.GetAttributeCurrent(Sel.AttrId)
			: Sel.VitalId == 1 ? LastVitals.MaxHealth : Sel.VitalId == 3 ? LastVitals.MaxStamina : LastVitals.MaxMana;
		const int32 Base = Sel.AttrId ? LastVitals.GetAttributeBase(Sel.AttrId)
			: LastVitals.GetVitalBase(Sel.VitalId);
		Bonus = Current - Base;
		StatTitle = Sel.AttrId ? FString::Printf(TEXT("%s: %d"), Sel.Name, Current)
			: FString::Printf(TEXT("%s: %d/%d"), Sel.Name,
				Sel.VitalId == 1 ? LastVitals.Health : Sel.VitalId == 3 ? LastVitals.Stamina : LastVitals.Mana, Current);
	}
	const int32 ModifierBegin = StatTitle.Len();
	if (Bonus) StatTitle += FString::Printf(TEXT(" (%+d)"), Bonus);
	PlaceTextUnder(AttrFooterTitle, TEXT("AttributePage"), TEXT("StatManagement_Footer_Title"),
		StatTitle, 9, TextWhite, StatOverlayZ);
	if (auto* Retail = Cast<UACERetailTextBlock>(AttrFooterTitle))
		Retail->SetModifierSuffix(ModifierBegin, Bonus > 0 ? FLinearColor::Green : FLinearColor::Red);

	PlaceTextUnder(AttrFooterLine1Label, TEXT("AttributePage"), TEXT("StatManagement_Footer_LineOneLabel"),
		bHasSel ? TEXT("Experience needed to raise") : TEXT("Skill Credits Available:"), 8, TextWhite, StatOverlayZ);
	if (AttrFooterLine1Label) { AttrFooterLine1Label->SetJustification(ETextJustify::Left); }
	if (AttrFooterLine1Value) { AttrFooterLine1Value->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrFooterLine1Value, TEXT("AttributePage"), TEXT("StatManagement_Footer_LineOneValue"),
		!bHasSel ? FString::FromInt(LastVitals.AvailableSkillCredits)
			: SelXpToNext > 0 ? FormatXpNumber(SelXpToNext)
			: bHasSel && bAttributeXpKnown ? TEXT("Infinite") : TEXT("—"), 8, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrFooterLine2Label, TEXT("AttributePage"), TEXT("StatManagement_Footer_LineTwoLabel"),
		TEXT("Unassigned Experience"), 8, TextWhite, StatOverlayZ);
	if (AttrFooterLine2Label) { AttrFooterLine2Label->SetJustification(ETextJustify::Left); }
	if (AttrFooterLine2Value) { AttrFooterLine2Value->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrFooterLine2Value, TEXT("AttributePage"), TEXT("StatManagement_Footer_LineTwoValue"),
		LastVitals.bValid ? FormatXpNumber(LastVitals.AvailableExperience) : FString(),
		8, TextWhite, StatOverlayZ);
	const bool bCanRaise = bHasSel && SelXpToNext > 0 && SelMaxSpend > 0
		&& LastVitals.AvailableExperience >= SelXpToNext;
	int64 XpTen = 0;
	if (bHasSel && Dat)
	{
		if (Rows[SelectedAttributeRow].AttrId != 0) Dat->TryGetAttributeXpToNextRank(SelXpSpent, XpTen, nullptr, 10);
		else Dat->TryGetVitalXpToNextRank(SelXpSpent, XpTen, nullptr, 10);
	}
	const bool bCanRaise10 = bCanRaise && XpTen > 0 && LastVitals.AvailableExperience >= XpTen;
	SyncRaiseButtonChrome(bCanRaise, bCanRaise10);
	SyncStatListScrollbar();
}

void UACEUIGameplayBinder::RefreshSkillOverlays()
{
	auto Collapse = [this]()
	{
		for (UTextBlock* Row : SkillRows) { if (Row) { Row->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UTextBlock* Row : SkillRowValues) { if (Row) { Row->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UBorder* B : SkillRowIcons) { if (B) { B->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UBorder* B : SkillRowHighlights) { if (B) { B->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UTextBlock* H : SkillSectionHeaders) { if (H) { H->SetVisibility(ESlateVisibility::Collapsed); } }
	};
	if (!Manager || !Canvas || !Canvas->WidgetTree || !Client
		|| ActivePanelPage != TEXT("SkillManagementPanel_Field")
		|| ActiveSkillTab != TEXT("SkillPage"))
	{
		Collapse();
		return;
	}
	EnsureOverlays();
	if (!LastVitals.bValid)
	{
		LastVitals = Client->GetPlayerVitals();
	}
	// Shared header fields with Attributes tab.
	FACEWorldObject Self;
	FString CharName = TEXT("Character");
	if (Client->GetWorldObject(Client->GetPlayerGuid(), Self) && !Self.Name.IsEmpty())
	{
		CharName = Self.Name;
	}
	int64 XpNeeded = 0;
	UACEDatSubsystem* Dat = Canvas->GetResourceResolver() ? Canvas->GetResourceResolver()->GetDatSubsystem() : nullptr;
	if (!Dat && PlayerController)
	{
		if (UGameInstance* GI = PlayerController->GetGameInstance())
		{
			Dat = GI->GetSubsystem<UACEDatSubsystem>();
		}
	}
	if (Dat)
	{
		float Progress = 0.f;
		Dat->TryGetXpToNextLevel(LastVitals.TotalExperience, LastVitals.Level, XpNeeded, &Progress);
		if (TSharedPtr<FACEUIElement> Meter = Manager->FindElementUnder(
			TEXT("SkillPage"), TEXT("StatManagement_Header_XPToLevelMeter")))
		{
			Meter->MeterFillFraction = FMath::Clamp(Progress, 0.f, 1.f);
		}
	}
	PlaceTextUnder(AttrHeaderName, TEXT("SkillPage"), TEXT("StatManagement_Header_Name"),
		CharName, 11, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderTitle, TEXT("SkillPage"), TEXT("StatManagement_Header_Title"),
		FormatGenderHeritageTitle(LastVitals), 9, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderPk, TEXT("SkillPage"), TEXT("StatManagement_Header_PKStatus"),
		FormatPkStatus(LastVitals.PlayerKillerStatus), 9, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderLevelLabel, TEXT("SkillPage"), TEXT("StatManagement_Header_LevelLabel"),
		TEXT("Character Level"), 8, TextWhite, StatOverlayZ);
	if (AttrHeaderLevel) { AttrHeaderLevel->SetJustification(ETextJustify::Center); }
	PlaceTextUnder(AttrHeaderLevel, TEXT("SkillPage"), TEXT("StatManagement_Header_LevelValue"),
		FString::FromInt(LastVitals.Level), 18, TextGold, StatOverlayZ);
	PlaceTextUnder(AttrHeaderTotalXpLabel, TEXT("SkillPage"), TEXT("StatManagement_Header_TotalXPLabel"),
		TEXT("Total Experience (XP)"), 8, TextWhite, StatOverlayZ);
	if (AttrHeaderXp) { AttrHeaderXp->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrHeaderXp, TEXT("SkillPage"), TEXT("StatManagement_Header_TotalXPValue"),
		FormatXpNumber(LastVitals.TotalExperience), 9, TextWhite, StatOverlayZ);
	PlaceTextUnder(AttrHeaderXpToLevelLabel, TEXT("SkillPage"), TEXT("StatManagement_Header_XPToLevelMeterLabel"),
		TEXT("XP for next level"), 8, TextRed, StatOverlayZ);
	if (AttrHeaderXpToLevel) { AttrHeaderXpToLevel->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrHeaderXpToLevel, TEXT("SkillPage"), TEXT("StatManagement_Header_XPToLevelMeterValue"),
		XpNeeded > 0 ? FormatXpNumber(XpNeeded) : TEXT("Max"), 8, TextWhite, StatOverlayZ);
	const bool bHasLum = LastVitals.MaximumLuminance > 0;
	PlaceTextUnder(AttrHeaderLuminanceLabel, TEXT("SkillPage"), TEXT("StatManagement_Header_LuminanceLabel"),
		bHasLum ? TEXT("Luminance") : FString(), 8, TextWhite, StatOverlayZ);
	if (AttrHeaderLuminanceValue) { AttrHeaderLuminanceValue->SetJustification(ETextJustify::Right); }
	PlaceTextUnder(AttrHeaderLuminanceValue, TEXT("SkillPage"), TEXT("StatManagement_Header_LuminanceValue"),
		bHasLum
			? FString::Printf(TEXT("%s / %s"),
				*FormatXpNumber(LastVitals.AvailableLuminance),
				*FormatXpNumber(LastVitals.MaximumLuminance))
			: FString(),
		8, TextGold, StatOverlayZ);

	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("SkillPage"), TEXT("StatManagement_List"));
	if (!ListEl.IsValid())
	{
		return;
	}

	struct FSkillLine
	{
		enum class EKind : uint8 { Header, Skill } Kind = EKind::Skill;
		FString Caption;
		const FACESkillInfo* Skill = nullptr;
		int32 HeaderDid = 0;
	};
	TArray<FSkillLine> Lines;
	// Retail SAC: Inactive=0, Untrained=1, Trained=2, Specialized=3.
	// Untrained vs Unusable both use SAC=Untrained; MinLevel 1 = usable when untrained.
	enum class ESkillSection : uint8 { Specialized, Trained, Untrained, Unusable };
	struct FGroup { ESkillSection Section; const TCHAR* Caption; int32 HeaderDid; };
	const FGroup Groups[] = {
		{ ESkillSection::Specialized, TEXT("Specialized"), DidSkillHeaderSpec },
		{ ESkillSection::Trained, TEXT("Trained"), DidSkillHeaderTrain },
		{ ESkillSection::Untrained, TEXT("Untrained"), DidSkillHeaderUntrain },
		{ ESkillSection::Unusable, TEXT("Unusable"), DidSkillHeaderUntrain },
	};
	auto SectionFor = [&](const FACESkillInfo& Sk) -> TOptional<ESkillSection>
	{
		if (Sk.SkillId <= 0) { return {}; }
		if (Sk.AdvancementClass >= 3) { return ESkillSection::Specialized; }
		if (Sk.AdvancementClass == 2) { return ESkillSection::Trained; }
		if (Sk.AdvancementClass != 1) { return {}; } // Inactive — omit
		uint32 MinLevel = 0;
		if (Dat)
		{
			Dat->TryGetSkillMinLevel(static_cast<uint32>(Sk.SkillId), MinLevel);
		}
		// Default to Untrained list when DAT MinLevel is missing.
		return (MinLevel == 0 || MinLevel == 1) ? ESkillSection::Untrained : ESkillSection::Unusable;
	};
	for (const FGroup& Group : Groups)
	{
		TArray<const FACESkillInfo*> GroupSkills;
		for (const FACESkillInfo& Sk : LastVitals.Skills)
		{
			const TOptional<ESkillSection> Sec = SectionFor(Sk);
			if (!Sec.IsSet() || Sec.GetValue() != Group.Section)
			{
				continue;
			}
			GroupSkills.Add(&Sk);
		}
		if (GroupSkills.Num() == 0)
		{
			continue;
		}
		Algo::Sort(GroupSkills, [](const FACESkillInfo* A, const FACESkillInfo* B)
		{
			return A && B && A->Name < B->Name;
		});
		FSkillLine H;
		H.Kind = FSkillLine::EKind::Header;
		H.Caption = Group.Caption;
		H.HeaderDid = Group.HeaderDid;
		Lines.Add(H);
		for (const FACESkillInfo* Sk : GroupSkills)
		{
			FSkillLine L;
			L.Skill = Sk;
			L.HeaderDid = Group.HeaderDid;
			Lines.Add(L);
		}
	}

	constexpr int32 RowH = 18;
	const int32 PageSize = FMath::Max(1, ListEl->Height / RowH);
	SkillListContentCount = Lines.Num();
	const int32 MaxScroll = FMath::Max(0, Lines.Num() - PageSize);
	SkillListScrollOffset = FMath::Clamp(SkillListScrollOffset, 0, MaxScroll);
	const FIntPoint Origin = ListEl->GetScreenOrigin();
	const float ListW = static_cast<float>(ListEl->Width - 18);

	SkillRowIds.SetNum(PageSize);
	while (SkillRows.Num() < PageSize && Canvas->WidgetTree)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		Row->SetJustification(ETextJustify::Left);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		Row->SetFont(Font);
		SkillRows.Add(Row);
	}
	while (SkillRowValues.Num() < PageSize && Canvas->WidgetTree)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		Row->SetJustification(ETextJustify::Right);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		Row->SetFont(Font);
		SkillRowValues.Add(Row);
	}
	while (SkillSectionHeaders.Num() < PageSize && Canvas->WidgetTree)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		Row->SetJustification(ETextJustify::Left);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 9);
		Row->SetFont(Font);
		Row->SetColorAndOpacity(FSlateColor(TextGold));
		SkillSectionHeaders.Add(Row);
	}

	for (int32 i = 0; i < PageSize; ++i)
	{
		UBorder* Icon = EnsureIconBorder(SkillRowIcons, i);
		UBorder* Hi = EnsureIconBorder(SkillRowHighlights, i);
		UTextBlock* Name = SkillRows.IsValidIndex(i) ? SkillRows[i].Get() : nullptr;
		UTextBlock* Val = SkillRowValues.IsValidIndex(i) ? SkillRowValues[i].Get() : nullptr;
		UTextBlock* Sec = SkillSectionHeaders.IsValidIndex(i) ? SkillSectionHeaders[i].Get() : nullptr;
		SkillRowIds[i] = 0;
		const int32 Idx = i + SkillListScrollOffset;
		if (!Lines.IsValidIndex(Idx))
		{
			if (Name) { Name->SetVisibility(ESlateVisibility::Collapsed); }
			if (Val) { Val->SetVisibility(ESlateVisibility::Collapsed); }
			if (Icon) { Icon->SetVisibility(ESlateVisibility::Collapsed); }
			if (Hi) { Hi->SetVisibility(ESlateVisibility::Collapsed); }
			if (Sec) { Sec->SetVisibility(ESlateVisibility::Collapsed); }
			continue;
		}
		const FSkillLine& Line = Lines[Idx];
		const float Y = static_cast<float>(Origin.Y + i * RowH);
		if (Line.Kind == FSkillLine::EKind::Header)
		{
			if (Name) { Name->SetVisibility(ESlateVisibility::Collapsed); }
			if (Val) { Val->SetVisibility(ESlateVisibility::Collapsed); }
			if (Hi) { Hi->SetVisibility(ESlateVisibility::Collapsed); }
			if (Icon)
			{
				Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
				SetIconDid(Icon, Line.HeaderDid);
				if (Icon->GetParent() != Canvas->GetElementLayer())
				{
					Canvas->GetElementLayer()->AddChild(Icon);
				}
				if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Icon->Slot))
				{
					Slot->SetAnchors(FAnchors(0.f, 0.f));
					Slot->SetPosition(FVector2D(static_cast<float>(Origin.X), Y));
					Slot->SetSize(FVector2D(ListW, static_cast<float>(RowH)));
					Canvas->SetOverlayOrder(Icon, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ);
					Icon->SetBrushColor(FLinearColor::White);
				}
			}
			if (Sec)
			{
				Sec->SetText(FText::FromString(Line.Caption));
				Sec->SetVisibility(ESlateVisibility::HitTestInvisible);
				if (Sec->GetParent() != Canvas->GetElementLayer())
				{
					Canvas->GetElementLayer()->AddChild(Sec);
				}
				if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Sec->Slot))
				{
					Slot->SetAnchors(FAnchors(0.f, 0.f));
					Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + 8), Y));
					Slot->SetSize(FVector2D(ListW - 12.f, static_cast<float>(RowH)));
					Canvas->SetOverlayOrder(Sec, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ + 1);
				}
			}
			continue;
		}
		if (Sec) { Sec->SetVisibility(ESlateVisibility::Collapsed); }
		const FACESkillInfo& Sk = *Line.Skill;
		SkillRowIds[i] = Sk.SkillId;
		const bool bSel = SelectedSkillId != 0 && Sk.SkillId == SelectedSkillId;
		if (Hi)
		{
			Hi->SetVisibility(ESlateVisibility::HitTestInvisible);
			SetIconDid(Hi, bSel ? DidListInfoBg : 0, FLinearColor(1.f, 1.f, 1.f, 0.85f));
			if (Hi->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Hi);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Hi->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X), Y));
				Slot->SetSize(FVector2D(ListW, static_cast<float>(RowH)));
				Canvas->SetOverlayOrder(Hi, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ);
			}
		}
		if (Icon)
		{
			FString SkillName;
			uint32 SkillIcon = 0;
			if (Dat) Dat->TryGetSkillInfo(static_cast<uint32>(Sk.SkillId), SkillName, SkillIcon);
			SetIconDid(Icon, SkillIcon);
			Icon->SetBrushColor(FLinearColor::White);
			Icon->SetVisibility(SkillIcon ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (Icon->GetParent() != Canvas->GetElementLayer()) Canvas->GetElementLayer()->AddChild(Icon);
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Icon->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X), Y));
				Slot->SetSize(FVector2D(RowH, RowH));
				Canvas->SetOverlayOrder(Icon, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ + 1);
			}
		}
		if (Name)
		{
			Name->SetText(FText::FromString(Sk.Name));
			Name->SetVisibility(ESlateVisibility::HitTestInvisible);
			Name->SetJustification(ETextJustify::Left);
			Name->SetColorAndOpacity(FSlateColor(bSel ? TextGold : TextWhite));
			if (Name->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Name);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Name->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + RowH + 2), Y));
				Slot->SetSize(FVector2D(ListW - RowH - 64.f, static_cast<float>(RowH)));
				Canvas->SetOverlayOrder(Name, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ + 1);
			}
		}
		if (Val)
		{
			Val->SetText(FText::AsNumber(Sk.Current));
			Val->SetVisibility(ESlateVisibility::HitTestInvisible);
			Val->SetJustification(ETextJustify::Right);
			const int32 Modifier = Sk.GetDisplayModifier();
			Val->SetColorAndOpacity(FSlateColor(Modifier > 0 ? FLinearColor::Green : Modifier < 0 ? FLinearColor::Red : TextWhite));
			FString Tooltip;
			if (Dat) Dat->TryGetSkillTooltip(static_cast<uint32>(Sk.SkillId), Tooltip);
			UACEHoverTooltipWidget::SetWidgetTooltip(Val, FText::FromString(Tooltip));
			if (Val->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Val);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Val->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + ListW - 56.f), Y));
				Slot->SetSize(FVector2D(52.f, static_cast<float>(RowH)));
				Canvas->SetOverlayOrder(Val, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ + 1);
			}
		}
	}

	const FACESkillInfo* Sel = nullptr;
	for (const FACESkillInfo& Sk : LastVitals.Skills)
	{
		if (Sk.SkillId == SelectedSkillId) { Sel = &Sk; break; }
	}
	SyncStatFooterChrome();
	int64 XpToNext = 0;
	int64 MaxSpend = 0;
	int32 TrainCost = 0;
	bool bSkillXpKnown = false;
	const bool bNeedsTrain = Sel && Sel->AdvancementClass > 0 && Sel->AdvancementClass < 2;
	if (Sel && Dat)
	{
		if (bNeedsTrain)
		{
			Dat->TryGetSkillTrainedCost(static_cast<uint32>(Sel->SkillId), TrainCost);
		}
		else
		{
			bSkillXpKnown = Dat->TryGetSkillXpToNextRank(Sel->AdvancementClass, Sel->XpSpent, XpToNext, &MaxSpend);
		}
	}
	PlaceTextUnder(AttrFooterTitle, TEXT("SkillPage"), TEXT("StatManagement_Footer_Title"),
		bNeedsTrain ? Sel->Name : Sel ? FString::Printf(TEXT("%s: %d%s"), *Sel->Name, Sel->Current,
			Sel->GetDisplayModifier() == 0 ? TEXT("") : *FString::Printf(TEXT(" (%+d)"), Sel->GetDisplayModifier()))
			: TEXT("Select a Skill"), 9, TextWhite, StatOverlayZ);
	if (auto* Retail = Cast<UACERetailTextBlock>(AttrFooterTitle))
		Retail->SetModifierSuffix(Sel && !bNeedsTrain ? FString::Printf(TEXT("%s: %d"), *Sel->Name, Sel->Current).Len() : -1,
			Sel && Sel->GetDisplayModifier() > 0 ? FLinearColor::Green : FLinearColor::Red);
	if (bNeedsTrain)
	{
		PlaceTextUnder(AttrFooterLine1Label, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineOneLabel"),
			TEXT("Skill Credits to Train:"), 8, TextWhite, StatOverlayZ);
		if (AttrFooterLine1Label) { AttrFooterLine1Label->SetJustification(ETextJustify::Left); }
		if (AttrFooterLine1Value) { AttrFooterLine1Value->SetJustification(ETextJustify::Right); }
		PlaceTextUnder(AttrFooterLine1Value, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineOneValue"),
			Sel ? FString::FromInt(TrainCost) : TEXT("—"), 8, TextWhite, StatOverlayZ);
		PlaceTextUnder(AttrFooterLine2Label, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineTwoLabel"),
			TEXT("Skill Credits Available:"), 8, TextWhite, StatOverlayZ);
		if (AttrFooterLine2Label) { AttrFooterLine2Label->SetJustification(ETextJustify::Left); }
		if (AttrFooterLine2Value) { AttrFooterLine2Value->SetJustification(ETextJustify::Right); }
		PlaceTextUnder(AttrFooterLine2Value, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineTwoValue"),
			FString::FromInt(LastVitals.AvailableSkillCredits), 8, TextWhite, StatOverlayZ);
	}
	else
	{
		PlaceTextUnder(AttrFooterLine1Label, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineOneLabel"),
			Sel ? TEXT("XP to raise") : TEXT("Skill Credits Available:"), 8, TextWhite, StatOverlayZ);
		if (AttrFooterLine1Label) { AttrFooterLine1Label->SetJustification(ETextJustify::Left); }
		if (AttrFooterLine1Value) { AttrFooterLine1Value->SetJustification(ETextJustify::Right); }
		PlaceTextUnder(AttrFooterLine1Value, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineOneValue"),
			!Sel ? FString::FromInt(LastVitals.AvailableSkillCredits)
				: XpToNext > 0 ? FormatXpNumber(XpToNext)
				: Sel && bSkillXpKnown && Sel->AdvancementClass >= 2 ? TEXT("Infinite") : TEXT("—"), 8, TextWhite, StatOverlayZ);
		PlaceTextUnder(AttrFooterLine2Label, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineTwoLabel"),
			TEXT("Unassigned Experience"), 8, TextWhite, StatOverlayZ);
		if (AttrFooterLine2Label) { AttrFooterLine2Label->SetJustification(ETextJustify::Left); }
		if (AttrFooterLine2Value) { AttrFooterLine2Value->SetJustification(ETextJustify::Right); }
		PlaceTextUnder(AttrFooterLine2Value, TEXT("SkillPage"), TEXT("StatManagement_Footer_LineTwoValue"),
			FormatXpNumber(LastVitals.AvailableExperience), 8, TextWhite, StatOverlayZ);
	}
	{
		bool bCanRaise1 = false;
		bool bCanRaise10 = false;
		if (Sel)
		{
			if (bNeedsTrain)
			{
				bCanRaise1 = TrainCost > 0 && LastVitals.AvailableSkillCredits >= TrainCost;
				bCanRaise10 = false; // retail: train uses Raise once, not ×10
			}
			else if (XpToNext > 0 && MaxSpend > 0)
			{
				bCanRaise1 = LastVitals.AvailableExperience >= XpToNext;
				int64 XpTen = 0;
				if (ActiveSkillTab == TEXT("SkillPage"))
				{
					for (const auto& S : LastVitals.Skills) if (S.SkillId == SelectedSkillId)
						Dat->TryGetSkillXpToNextRank(S.AdvancementClass, S.XpSpent, XpTen, nullptr, 10);
				}
				else if (SelectedAttributeRow < 6)
					Dat->TryGetAttributeXpToNextRank(LastVitals.GetAttributeXpSpent(SelectedAttributeRow == 2 ? 4 : SelectedAttributeRow == 3 ? 3 : SelectedAttributeRow + 1), XpTen, nullptr, 10);
				else
					Dat->TryGetVitalXpToNextRank(SelectedAttributeRow == 6 ? LastVitals.HealthXpSpent : SelectedAttributeRow == 7 ? LastVitals.StaminaXpSpent : LastVitals.ManaXpSpent, XpTen, nullptr, 10);
				bCanRaise10 = XpTen > 0 && LastVitals.AvailableExperience >= XpTen;
			}
		}
		SyncRaiseButtonChrome(bCanRaise1, bCanRaise10);
	}
	if (TSharedPtr<FACEUIElement> Meter = Manager->FindElementUnder(
		TEXT("SkillPage"), TEXT("StatManagement_Footer_Meter")))
	{
		// Nested meter fill child may share the name — prefer type Meter.
		TSharedPtr<FACEUIElement> Fill = Meter;
		for (const TSharedPtr<FACEUIElement>& Child : Meter->Children)
		{
			if (Child.IsValid() && Child->Type == ACEUI::ElementType::Meter)
			{
				Fill = Child;
				break;
			}
		}
		float Frac = 0.f;
		// gmSkillUI::UpdateFooter uses (_pp - XP(rank)) / (XP(rank+1) - XP(rank)).
		// MaxSpend is the whole skill-cap budget, not the cost of the current rank.
		if (Sel && Dat) Dat->TryGetSkillRankProgress(Sel->AdvancementClass, Sel->Ranks, Sel->XpSpent, Frac);
		Fill->MeterFillFraction = FMath::Clamp(Frac, 0.f, 1.f);
	}
	SyncStatListScrollbar();
}

void UACEUIGameplayBinder::RefreshTitleOverlays()
{
	for (auto Row : TitleRowElements) { if (Row) Row->bVisible = false; }
	for (UTextBlock* Row : TitleRows)
	{
		if (Row) { Row->SetVisibility(ESlateVisibility::Collapsed); }
	}
	auto HideTitleChrome = [this]()
	{
		if (TitleCurrentLabel) { TitleCurrentLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (TitleCurrentValue) { TitleCurrentValue->SetVisibility(ESlateVisibility::Collapsed); }
		if (TitleSetButtonLabel) { TitleSetButtonLabel->SetVisibility(ESlateVisibility::Collapsed); }
	};
	if (!Manager || !Canvas || !Canvas->WidgetTree
		|| ActivePanelPage != TEXT("SkillManagementPanel_Field")
		|| ActiveSkillTab != TEXT("CharacterTitlePage"))
	{
		HideTitleChrome();
		return;
	}
	EnsureOverlays();
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("CharacterTitlePage"), TEXT("CharacterTitle_ListBox"));
	if (!ListEl.IsValid())
	{
		ListEl = Manager->FindElementByName(TEXT("CharacterTitle_ListBox"));
	}
	if (!ListEl.IsValid())
	{
		HideTitleChrome();
		return;
	}
	SortedTitleIds = Client ? Client->GetCharacterTitleIds() : TArray<int32>();
	SortedTitleIds.Remove(0);
	SortedTitleIds.Sort([](int32 A, int32 B) { return FCString::Strcmp(GetCharacterTitleName(A), GetCharacterTitleName(B)) < 0; });
	const TArray<int32>& Titles = SortedTitleIds;
	const int32 DisplayId = Client ? Client->GetDisplayTitleId() : 0;
	if (SelectedTitleId != 0 && !Titles.Contains(SelectedTitleId))
	{
		SelectedTitleId = 0;
	}
	if (SelectedTitleId == 0 && DisplayId != 0)
	{
		SelectedTitleId = DisplayId;
	}

	auto EnsureTitleLabel = [this](TObjectPtr<UTextBlock>& Label, ETextJustify::Type Justify)
	{
		if (!Label && Canvas->WidgetTree)
		{
			Label = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			Label->SetJustification(Justify);
		}
	};
	EnsureTitleLabel(TitleCurrentLabel, ETextJustify::Left);
	EnsureTitleLabel(TitleCurrentValue, ETextJustify::Left);
	EnsureTitleLabel(TitleSetButtonLabel, ETextJustify::Center);
	PlaceTextUnder(TitleCurrentLabel, TEXT("CharacterTitlePage"),
		TEXT("CharacterTitle_CurrentDisplayLabel"), TEXT("Current Title"), 9, TextWhite, 570);
	const FString DisplayName = DisplayId != 0
		? GetCharacterTitleName(static_cast<uint32>(DisplayId)) : TEXT("(none)");
	PlaceTextUnder(TitleCurrentValue, TEXT("CharacterTitlePage"),
		TEXT("CharacterTitle_CurrentDisplayText"), DisplayName, 10, TextGold, 570);
	PlaceTextUnder(AttrFooterTitle, TEXT("CharacterTitlePage"), TEXT("CharacterTitle_ListLabel"),
		TEXT("Character Titles"), 10, TextGold, 570);
	PlaceTextUnder(TitleSetButtonLabel, TEXT("CharacterTitlePage"),
		TEXT("CharacterTitle_SetAsDisplayButton"), TEXT("Set as Display Title"), 9, TextWhite, 575);

	const bool bCanSet = Titles.Contains(SelectedTitleId) && SelectedTitleId != DisplayId;
	if (auto Button = Manager->FindElementUnder(TEXT("CharacterTitlePage"), TEXT("CharacterTitle_SetAsDisplayButton")))
	{
		Button->bGhosted = !bCanSet;
		Button->bActivatable = bCanSet;
	}
	if (TitleSetButtonLabel) TitleSetButtonLabel->SetColorAndOpacity(FSlateColor(bCanSet ? TextWhite : FLinearColor(.45f,.45f,.45f,1.f)));
	// classic_charactertitle's EntryTemplate is a 24px row with native selection
	// artwork and bitmap font, not a stretched label covering the whole list.
	constexpr int32 RowH = 24;
	const int32 MaxTitleRows = FMath::Max(1, ListEl->Height / RowH);
	while (TitleRows.Num() < MaxTitleRows && Canvas->WidgetTree)
	{
		auto Entry = UACEUILayoutResolver::LoadTemplate(0x2100005E, 0x10000536);
		if (!Entry || Entry->Children.IsEmpty()) break;
		Entry->SetElementName(FString::Printf(TEXT("LiveCharacterTitleRow%d"), TitleRows.Num()));
		Entry->bVisible = true; Entry->bDefaultHidden = false;
		ListEl->AddChild(Entry); TitleRowElements.Add(Entry);
		Manager->InvalidateNameLookupIndex();
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Row->SetAutoWrapText(false);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		Row->SetFont(Font);
		TitleRows.Add(Row);
	}

	if (Titles.Num() == 0)
	{
		TitleListScrollOffset = 0;
		for (auto Row : TitleRows) { if (Row) Row->SetVisibility(ESlateVisibility::Collapsed); }
		if (auto Bar = Manager->FindElementUnder(TEXT("CharacterTitlePage"), TEXT("CharacterTitle_ListBox_Scrollbar")))
			SyncDatScrollbar(Bar, 0.f);
		if (TitleRows.Num() > 0 && TitleRows[0])
		{
			UTextBlock* Row = TitleRows[0];
			Row->SetText(FText::FromString(TEXT("(No titles yet.)")));
			Row->SetVisibility(ESlateVisibility::HitTestInvisible);
			Row->SetColorAndOpacity(FSlateColor(TextWhite));
			Canvas->PlaceWidgetAtElement(Row, ListEl, 571, FMargin(6.f, 4.f, 6.f, 4.f));
		}
		return;
	}

	const int32 VisibleRows = MaxTitleRows;
	const int32 MaxOff = FMath::Max(0, Titles.Num() - VisibleRows);
	TitleListScrollOffset = FMath::Clamp(TitleListScrollOffset, 0, MaxOff);
	if (auto Bar = Manager->FindElementUnder(TEXT("CharacterTitlePage"), TEXT("CharacterTitle_ListBox_Scrollbar")))
		SyncDatScrollbar(Bar, MaxOff > 0 ? static_cast<float>(TitleListScrollOffset) / MaxOff : 0.f);

	for (int32 i = 0; i < TitleRows.Num(); ++i)
	{
		UTextBlock* Row = TitleRows[i];
		if (!Row)
		{
			continue;
		}
		const int32 TitleIndex = TitleListScrollOffset + i;
		if (TitleIndex >= Titles.Num() || i >= VisibleRows)
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		const uint32 Tid = static_cast<uint32>(Titles[TitleIndex]);
		const FString Name = GetCharacterTitleName(Tid);
		const bool bSel = (static_cast<int32>(Tid) == SelectedTitleId);
		auto Entry = TitleRowElements[i];
		Entry->bVisible = true; Entry->X = 0; Entry->Y = i * RowH;
		Entry->Width = ListEl->Width; Entry->Height = RowH;
		Entry->DefaultState = bSel ? 6 : 0;
		auto TextElement = Entry->Children[0];
		TextElement->Width = Entry->Width; TextElement->Height = RowH;
		Row->SetText(FText::FromString(Name));
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		Row->SetColorAndOpacity(FSlateColor(TextWhite));
		Canvas->PlaceWidgetAtElement(Row, TextElement, 571 + i);
	}
}

void UACEUIGameplayBinder::DropInventoryAmount(int32 Guid,int32 Amount)
{
    FACEWorldObject Item;
    if(!Client || !Client->GetWorldObject(Guid,Item) || !Client->IsOwnedInventoryItem(Item))return;
    if(!Item.CanDropToWorld()){PostInventorySystemMessage(TEXT("You cannot drop that item."));return;}
    const int32 Stack=FMath::Max(1,Item.StackSize),Quantity=FMath::Clamp(Amount,1,Stack);
    if(Quantity<Stack)
    {
        TrackInventoryStackSplit(InventorySelectionSplit,Item,Quantity);InventorySelectionSplit.bToWorld=true;
        Client->SendStackableSplitTo3D(Guid,Quantity);
    }
    else Client->SendDropItem(Guid);
}

void UACEUIGameplayBinder::RaiseSelectedStat(int32 Multiplier)
{
	if (!Client)
	{
		return;
	}
	LastVitals = Client->GetPlayerVitals();
	if (!LastVitals.bValid)
	{
		PostInventorySystemMessage(TEXT("Character stats are not ready yet."));
		return;
	}
	UACEDatSubsystem* Dat = Canvas && Canvas->GetResourceResolver() ? Canvas->GetResourceResolver()->GetDatSubsystem() : nullptr;
	if (!Dat && PlayerController)
	{
		if (UGameInstance* GI = PlayerController->GetGameInstance())
		{
			Dat = GI->GetSubsystem<UACEDatSubsystem>();
		}
	}
	if (!Dat)
	{
		return;
	}
	if (ActiveSkillTab == TEXT("SkillPage"))
	{
		const FACESkillInfo* Sel = nullptr;
		for (const FACESkillInfo& Sk : LastVitals.Skills)
		{
			if (Sk.SkillId == SelectedSkillId) { Sel = &Sk; break; }
		}
		if (!Sel)
		{
			PostInventorySystemMessage(TEXT("Select a skill first."));
			return;
		}
		if (Sel->AdvancementClass <= 0)
		{
			PostInventorySystemMessage(TEXT("That skill cannot be trained."));
			return;
		}
		// Untrained (1): spend skill credits via TrainSkill. Trained+ (2+): spend XP via RaiseSkill.
		if (Sel->AdvancementClass < 2)
		{
			// Training is a single skill-credit action; the XP +10 action never trains.
			if (Multiplier != 1) return;
			int32 Cost = 0;
			if (!Dat->TryGetSkillTrainedCost(static_cast<uint32>(Sel->SkillId), Cost) || Cost <= 0)
			{
				return;
			}
			if (LastVitals.AvailableSkillCredits < Cost)
			{
				PostInventorySystemMessage(TEXT("You do not have enough skill credits."));
				return;
			}
			Client->SendTrainSkill(Sel->SkillId, Cost);
			return;
		}
		int64 XpToNext = 0;
		int64 MaxSpend = 0;
		Dat->TryGetSkillXpToNextRank(Sel->AdvancementClass, Sel->XpSpent, XpToNext, &MaxSpend, FMath::Max(1, Multiplier));
		if (LastVitals.AvailableExperience <= 0)
		{
			PostInventorySystemMessage(TEXT("You have no unassigned experience."));
			return;
		}
		if (XpToNext <= 0 || MaxSpend <= 0)
		{
			PostInventorySystemMessage(TEXT("That skill is fully raised."));
			return;
		}
		int64 Wanted = (Multiplier <= 0) ? MaxSpend : XpToNext;
		if (Multiplier > 0 && LastVitals.AvailableExperience < Wanted) return;
		Wanted = FMath::Min3<int64>(Wanted, MaxSpend, LastVitals.AvailableExperience);
		Wanted = FMath::Max<int64>(1, Wanted);
		Client->SendRaiseSkill(Sel->SkillId, static_cast<int32>(FMath::Min<int64>(Wanted, MAX_int32)));
		return;
	}
	if (ActiveSkillTab != TEXT("AttributePage"))
	{
		return;
	}
	if (SelectedAttributeRow < 0 || SelectedAttributeRow > 8)
	{
		PostInventorySystemMessage(TEXT("Select an attribute first."));
		return;
	}
	struct FAttrRow { int32 AttrId; int32 VitalId; };
	const FAttrRow Rows[] = {
		{ 1, 0 }, { 2, 0 }, { 4, 0 }, { 3, 0 }, { 5, 0 }, { 6, 0 },
		{ 0, 1 }, { 0, 3 }, { 0, 5 },
	};
	const FAttrRow& Sel = Rows[SelectedAttributeRow];
	int32 XpSpent = 0;
	int64 XpToNext = 0;
	int64 MaxSpend = 0;
	if (Sel.AttrId != 0)
	{
		XpSpent = LastVitals.GetAttributeXpSpent(Sel.AttrId);
		Dat->TryGetAttributeXpToNextRank(XpSpent, XpToNext, &MaxSpend, FMath::Max(1, Multiplier));
	}
	else
	{
		switch (Sel.VitalId)
		{
		case 1: XpSpent = LastVitals.HealthXpSpent; break;
		case 3: XpSpent = LastVitals.StaminaXpSpent; break;
		case 5: XpSpent = LastVitals.ManaXpSpent; break;
		default: break;
		}
		Dat->TryGetVitalXpToNextRank(XpSpent, XpToNext, &MaxSpend, FMath::Max(1, Multiplier));
	}
	if (LastVitals.AvailableExperience <= 0)
	{
		PostInventorySystemMessage(TEXT("You have no unassigned experience."));
		return;
	}
	if (XpToNext <= 0 || MaxSpend <= 0)
	{
		PostInventorySystemMessage(TEXT("That attribute is fully raised."));
		return;
	}
	int64 Wanted = (Multiplier <= 0) ? MaxSpend : XpToNext;
	if (Multiplier > 0 && LastVitals.AvailableExperience < Wanted) return;
	Wanted = FMath::Min3<int64>(Wanted, MaxSpend, LastVitals.AvailableExperience);
	Wanted = FMath::Max<int64>(1, Wanted);
	const int32 Spend = static_cast<int32>(FMath::Min<int64>(Wanted, MAX_int32));
	if (Sel.AttrId != 0)
	{
		Client->SendRaiseAttribute(Sel.AttrId, Spend);
	}
	else
	{
		Client->SendRaiseVital(Sel.VitalId, Spend);
	}
}

void UACEUIGameplayBinder::PlaceTextUnder(UTextBlock* Text, const FString& AncestorName,
	const FString& ElementName, const FString& Contents, int32 FontSize, const FLinearColor& Color, int32 ZOrder)
{
	if (!Text || !Manager || !Canvas)
	{
		return;
	}
	TSharedPtr<FACEUIElement> Ancestor = Manager->FindElementByName(AncestorName);
	TSharedPtr<FACEUIElement> El;
	if (Ancestor.IsValid())
	{
		// Prefer a match whose ancestry is visible (duplicate Footer_* names under Default/Text/Meter).
		TArray<TSharedPtr<FACEUIElement>> Stack;
		Stack.Add(Ancestor);
		while (Stack.Num() > 0)
		{
			TSharedPtr<FACEUIElement> Cur = Stack.Pop(EAllowShrinking::No);
			if (!Cur.IsValid())
			{
				continue;
			}
			if (Cur->ElementName == ElementName)
			{
				bool bVis = Cur->bVisible;
				for (TSharedPtr<FACEUIElement> P = Cur->Parent.Pin(); P.IsValid() && P != Ancestor->Parent.Pin();
					P = P->Parent.Pin())
				{
					if (!P->bVisible) { bVis = false; break; }
					if (P == Ancestor) { break; }
				}
				if (bVis && Ancestor->bVisible)
				{
					El = Cur;
					break;
				}
				if (!El.IsValid())
				{
					El = Cur; // fallback
				}
			}
			for (int32 i = Cur->Children.Num() - 1; i >= 0; --i)
			{
				Stack.Add(Cur->Children[i]);
			}
		}
	}
	if (!El.IsValid())
	{
		El = Manager->FindElementUnder(AncestorName, ElementName);
	}
	if (!El.IsValid())
	{
		El = Manager->FindElementByName(ElementName);
	}
	if (!El.IsValid())
	{
		Text->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	bool bAncestorsVisible = El->bVisible && (!Ancestor.IsValid() || Ancestor->bVisible);
	for (TSharedPtr<FACEUIElement> P = El->Parent.Pin(); P.IsValid(); P = P->Parent.Pin())
	{
		if (!P->bVisible) { bAncestorsVisible = false; break; }
		if (Ancestor.IsValid() && P == Ancestor) { break; }
	}
	if (!bAncestorsVisible || Contents.IsEmpty())
	{
		Text->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	Text->SetVisibility(ESlateVisibility::HitTestInvisible);
	Text->SetText(FText::FromString(Contents));
	Text->SetColorAndOpacity(FSlateColor(Color));
	const ACEUIFontStyles::FACEFontStyle Style =
		ACEUIFontStyles::ResolveForElement(El, FontSize);
	Text->SetFont(ACEUIFontStyles::MakeSlateFont(Style));
	Canvas->PlaceWidgetAtElement(Text, El, ZOrder, FMargin(2.f, 1.f));
}

void UACEUIGameplayBinder::RefreshSpellcastTabLabels()
{
	// Retail draws the spell set index on each tab using GenericSpellcastTab's tabfont base.
	// The LayoutDesc tab instances have no children, so the numerals are drawn here.
	static const TCHAR* const Numerals[] = {
		TEXT("I"), TEXT("II"), TEXT("III"), TEXT("IV"),
		TEXT("V"), TEXT("VI"), TEXT("VII"), TEXT("VIII") };
	constexpr int32 TabCount = UE_ARRAY_COUNT(Numerals);
	const bool bMagic = CombatMode == static_cast<int32>(ACECombatMode::Magic);
	TSharedPtr<FACEUIElement> CombatRoot = Manager
		? Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"))
		: TSharedPtr<FACEUIElement>();
	const bool bShow = Client && Manager && Canvas && Canvas->WidgetTree && bMagic
		&& CombatRoot.IsValid() && CombatRoot->bVisible;
	if (!bShow)
	{
		for (UTextBlock* L : SpellcastTabLabels)
		{
			if (L) { L->SetVisibility(ESlateVisibility::Collapsed); }
		}
		return;
	}
	while (SpellcastTabLabels.Num() < TabCount)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		L->SetJustification(ETextJustify::Center);
		SpellcastTabLabels.Add(L);
	}
	const int32 ActiveBar = Client->GetActiveSpellBar();
	for (int32 i = 0; i < TabCount; ++i)
	{
		UTextBlock* Label = SpellcastTabLabels.IsValidIndex(i) ? SpellcastTabLabels[i] : nullptr;
		if (!Label) { continue; }
		TSharedPtr<FACEUIElement> Tab = Manager->FindElementByName(
			FString::Printf(TEXT("Spellcast_Tab%d"), i + 1));
		if (!Tab.IsValid() || !Tab->bVisible || Tab->Width <= 0 || Tab->Height <= 0)
		{
			Label->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		const bool bSel = (i == ActiveBar);
		Label->SetVisibility(ESlateVisibility::HitTestInvisible);
		Label->SetText(FText::FromString(Numerals[i]));
		Label->SetColorAndOpacity(FSlateColor(bSel ? TextGold : FLinearColor(0.62f, 0.60f, 0.52f, 1.f)));
		Label->SetFont(FCoreStyle::GetDefaultFontStyle(bSel ? TEXT("Bold") : TEXT("Regular"), 8));
		Canvas->PlaceWidgetAtElement(Label, Tab, 585, FMargin(0.f, 3.f, 0.f, 2.f));
	}
}

void UACEUIGameplayBinder::RefreshSpellHotbarOverlays()
{
	SyncSpellTabSelection();
	RefreshSpellcastTabLabels();
	auto CollapseSpellBarWidgets = [this]()
	{
		for (UBorder* B : SpellBarIcons)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : SpellBarSlotBgs)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : SpellBarSlotNumIcons)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : SpellBarSelectedOverlays)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UTextBlock* T : SpellBarSlotNumbers)
		{
			if (T) { T->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : BuiltInSpellIconBorders)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
	};
	if (!Client || !Manager || !Canvas || !Canvas->WidgetTree || !Canvas->GetElementLayer())
	{
		CollapseSpellBarWidgets();
		return;
	}
	const bool bMagic = CombatMode == static_cast<int32>(ACECombatMode::Magic);
	TSharedPtr<FACEUIElement> CombatRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field"));
	if (!bMagic || !CombatRoot.IsValid() || !CombatRoot->bVisible)
	{
		CollapseSpellBarWidgets();
		if (CastSpellLabel)
		{
			CastSpellLabel->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (SpellcastSpellNameLabel)
		{
			SpellcastSpellNameLabel->SetVisibility(ESlateVisibility::Collapsed);
		}
		return;
	}
	// SpellCastSubMenu appends one empty drop entry; arrows only scroll overflowing entries.
	constexpr int32 Cell = 32;
	constexpr int32 SlotGap = 2;
	constexpr int32 ArrowW = 23;
	constexpr int32 BuiltInW = 32;
	constexpr int32 BuiltInX = 4;
	constexpr int32 BankX = 40;
	
	constexpr int32 CastW = 75;
	constexpr int32 GapAfterScroll = 8;
	constexpr int32 FramePadRight = 6;
	const int32 ActiveBarEarly = Client->GetActiveSpellBar();
	const TArray<int32> BarEarly = Client->GetSpellBar(ActiveBarEarly);
	int32 FilledEarly = 0;
	for (int32 i = 0; i < BarEarly.Num(); ++i)
	{
		if (BarEarly[i] == 0) { break; }
		++FilledEarly;
	}
	// Keep a stable frame while arrows appear/disappear, and honor the saved
	// horizontal drag size. Whole icons fit inside a continuously resized frame.
	constexpr int32 DefaultWidth = 625;
	CombatRoot->AuthoredWidth = DefaultWidth;
	CombatRoot->MinWidth = 400;
	CombatRoot->MaxWidth = FMath::Max(CombatRoot->MinWidth,
		FMath::FloorToInt(Canvas->ViewportToLayout(Canvas->GetCachedGeometry().GetLocalSize()).X));
	const int32 Width = FMath::Clamp(DefaultWidth + CombatRoot->UserResizeW, CombatRoot->MinWidth, CombatRoot->MaxWidth);
	const int32 ContentW = Width - 10;
	const int32 ScrollW = ContentW - BankX - GapAfterScroll - CastW - FramePadRight;
	const int32 ListW = ScrollW - 2 * ArrowW;
	const int32 VisibleSlots = FMath::Max(1, (ListW + SlotGap) / (Cell + SlotGap));
	const bool bScrollable = FilledEarly + 1 > VisibleSlots;
    const int32 ScrollArrowW = bScrollable ? ArrowW : 0;
    // The retail item list tiles its entire viewport, independently of its entries.
    // Only favorites plus the end insertion entry contribute to scrolling.
    const int32 MaxSlots = VisibleSlots;
	FitCombatFloatyContentWidth(ContentW);
	if (TSharedPtr<FACEUIElement> Cast = Manager->FindElementByName(TEXT("CastSpellButton")))
	{
		Cast->Width = CastW;
		Cast->X = BankX + ScrollW + GapAfterScroll;
		Cast->bVisible = true;
	}

	auto LayoutSpellScroll = [&](const FString& BankName)
	{
		TSharedPtr<FACEUIElement> Bank = Manager->FindElementByName(BankName);
		TSharedPtr<FACEUIElement> Scroll = Manager->FindElementUnder(BankName, TEXT("SpellScroll"));
		if (!Scroll.IsValid())
		{
			return;
		}
		if (Bank.IsValid())
		{
			Bank->X = BankX;
			Bank->Width = ScrollW;
		}
		Scroll->X = 0;
		Scroll->Width = ScrollW;
		if (TSharedPtr<FACEUIElement> Up = Manager->FindElementUnder(BankName, TEXT("ScrollBar_Up")))
		{
			Up->X = 0;
			Up->Width = ArrowW;
			Up->bVisible = bScrollable;
		}
		if (TSharedPtr<FACEUIElement> Down = Manager->FindElementUnder(BankName, TEXT("ScrollBar_Down")))
		{
			Down->X = FMath::Max(ArrowW, ScrollW - ArrowW);
			Down->Width = ArrowW;
			Down->bVisible = bScrollable;
		}
		if (TSharedPtr<FACEUIElement> List = Manager->FindElementUnder(BankName, TEXT("SpellList")))
		{
			List->X = ScrollArrowW;
			List->Width = ListW;
		}
	};
	for (int32 i = 0; i < 8; ++i)
	{
		LayoutSpellScroll(FString::Printf(TEXT("Spellcasting_Bank%d"), i + 1));
	}
    UACEDatSubsystem* Dat = Canvas->GetResourceResolver()
        ? Canvas->GetResourceResolver()->GetDatSubsystem() : nullptr;

	const int32 ActiveBar = ActiveBarEarly;
	const TArray<int32>& Bar = BarEarly;
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		FString::Printf(TEXT("Spellcasting_Bank%d"), ActiveBar + 1), TEXT("SpellList"));
	if (!ListEl.IsValid())
	{
		ListEl = Manager->FindElementUnder(TEXT("Spellcasting_Bank1"), TEXT("SpellList"));
	}
	if (!ListEl.IsValid())
	{
		return;
	}
	const FIntPoint Origin = ListEl->GetScreenOrigin();
	const float SX = Canvas->GetLastScaleX();
	const float SY = Canvas->GetLastScaleY();
	UCanvasPanel* ElementLayer = Canvas->GetElementLayer();

	// SyncSpellTabSelection has resolved the held caster before selecting a spell.
	if (TSharedPtr<FACEUIElement> BuiltIn = Manager->FindElementByName(TEXT("BuiltInSpell")))
	{
		BuiltIn->bVisible = BuiltInSpellId != 0;
		BuiltIn->bActivatable = BuiltInSpellId != 0;
		BuiltIn->X = BuiltInX;
		BuiltIn->Width = BuiltInW;
		BuiltIn->Height = 32;
		FString BuiltInName;
		uint32 BuiltInIconDid = 0;
		if (BuiltInSpellId != 0 && Dat)
		{
			Dat->TryGetSpellInfo(static_cast<uint32>(BuiltInSpellId), BuiltInName, BuiltInIconDid);
		}
		FACEWorldObject Caster;
		const bool bPresent = BuiltInCasterGuid != 0 && Client->GetWorldObject(BuiltInCasterGuid, Caster);
		const FText Tooltip = bPresent ? FText::FromString(ACERetailObjectNames::Name(Caster)
			+ TEXT(" (") + BuiltInName + TEXT(")\nDouble-click to cast this spell")) : FText::GetEmpty();
		EnsureIconBorder(BuiltInSpellIconBorders, 0);
		if (UBorder* Icon = EnsureIconBorder(BuiltInSpellIconBorders, 0))
		{
			const FIntPoint BiOrigin = BuiltIn->GetScreenOrigin();
			if (Icon->GetParent() != ElementLayer)
			{
				ElementLayer->AddChild(Icon);
			}
			if (BuiltInSpellId != 0 && BuiltInIconDid != 0)
			{
				Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
				SetSpellIcon(Icon, BuiltInSpellId);
				if (!BuiltInName.IsEmpty())
				{
					SetRetailTooltip(Icon, Tooltip);
				}
				if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Icon->Slot))
				{
					Slot->SetAnchors(FAnchors(0.f, 0.f));
					Slot->SetPosition(FVector2D(static_cast<float>(BiOrigin.X) * SX, static_cast<float>(BiOrigin.Y) * SY));
					Slot->SetSize(FVector2D(static_cast<float>(BuiltInW) * SX, 32.f * SY));
					Canvas->SetOverlayOrder(Icon, Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field")), 580);
				}
			}
			else
			{
				Icon->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		// IconData::GetDragIcon excludes the inventory background/underlay and
		// stack/trade decorations. Its alpha reveals the spell below the caster.
		if (UBorder* CasterIcon = EnsureIconBorder(BuiltInSpellIconBorders, 1))
		{
			CasterIcon->SetVisibility(bPresent ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (bPresent)
			{
				FSlateBrush Brush;
				Brush.DrawAs = ESlateBrushDrawType::Image;
				Brush.SetResourceObject(Canvas->GetResourceResolver()->ResolveItemForeground(
					Caster.IconId, Caster.IconOverlayId, Caster.UiEffects));
				Brush.ImageSize = FVector2D(32, 32);
				CasterIcon->SetBrush(Brush);
				CasterIcon->SetBrushColor(FLinearColor::White);
				SetRetailTooltip(CasterIcon, Tooltip);
				Canvas->PlaceWidgetAtElement(CasterIcon, BuiltIn, 581);
			}
		}
		// Native DAT children paint before runtime icon layers. Keep the selection
		// frame above both, like the separate favorite-slot selection overlays.
		if (UBorder* Selected = EnsureIconBorder(BuiltInSpellIconBorders, 2))
		{
			const bool bSelected = SelectedCombatSpellSlot < 0 && BuiltInSpellId != 0;
			Selected->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (bSelected)
			{
				SetIconDid(Selected, 0x06004D09);
				Canvas->PlaceWidgetAtElement(Selected, BuiltIn, 583);
			}
		}
		if (auto Sel = Manager->FindElementUnder(TEXT("BuiltInSpell"), TEXT("BuiltInSpell_Selected")))
			Sel->bVisible = false;
	}
	int32 Filled = FilledEarly;
	const int32 MaxOff = FMath::Max(0, Filled + 1 - VisibleSlots);
	SpellHotbarScrollOffset = FMath::Clamp(SpellHotbarScrollOffset, 0, MaxOff);
	if (bRevealSelectedSpell)
	{
		if (SelectedCombatSpellSlot >= 0)
			SpellHotbarScrollOffset = FMath::Clamp(SpellHotbarScrollOffset,
				FMath::Max(0, SelectedCombatSpellSlot - VisibleSlots + 1), SelectedCombatSpellSlot);
		SpellHotbarScrollOffset = FMath::Clamp(SpellHotbarScrollOffset, 0, MaxOff);
		bRevealSelectedSpell = false;
	}
	SpellBarSpellIds.SetNum(MaxSlots);
	while (SpellBarIcons.Num() < MaxSlots)
	{
		EnsureIconBorder(SpellBarIcons, SpellBarIcons.Num());
	}
	while (SpellBarSlotBgs.Num() < MaxSlots)
	{
		EnsureIconBorder(SpellBarSlotBgs, SpellBarSlotBgs.Num());
	}
	while (SpellBarSlotNumIcons.Num() < MaxSlots)
	{
		EnsureIconBorder(SpellBarSlotNumIcons, SpellBarSlotNumIcons.Num());
	}
	while (SpellBarSelectedOverlays.Num() < MaxSlots)
	{
		EnsureIconBorder(SpellBarSelectedOverlays, SpellBarSelectedOverlays.Num());
	}
	for (UTextBlock* T : SpellBarSlotNumbers)
	{
		if (T) { T->SetVisibility(ESlateVisibility::Collapsed); }
	}
	auto PlaceBorderAt = [&](UBorder* Border, float SlotX, float SlotY, int32 Z)
	{
		if (!IsValid(Border) || !ElementLayer) { return; }
		if (Border->GetParent() != ElementLayer)
		{
			ElementLayer->AddChild(Border);
		}
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Border->Slot))
		{
			Slot->SetAnchors(FAnchors(0.f, 0.f));
			Slot->SetPosition(FVector2D(SlotX * SX, SlotY * SY));
			Slot->SetSize(FVector2D(static_cast<float>(Cell) * SX, static_cast<float>(Cell) * SY));
			Canvas->SetOverlayOrder(Border, Manager->FindElementByName(TEXT("RootGameplay_FloatyCombatPanel_Field")), Z);
		}
	};
	for (int32 i = 0; i < SpellBarIcons.Num(); ++i)
	{
		UBorder* Icon = SpellBarIcons[i];
		UBorder* Bg = SpellBarSlotBgs.IsValidIndex(i) ? SpellBarSlotBgs[i] : nullptr;
		UBorder* NumIcon = SpellBarSlotNumIcons.IsValidIndex(i) ? SpellBarSlotNumIcons[i] : nullptr;
		UBorder* Sel = SpellBarSelectedOverlays.IsValidIndex(i) ? SpellBarSelectedOverlays[i] : nullptr;
		if (!Icon) { continue; }
		if (i >= MaxSlots)
		{
			Icon->SetVisibility(ESlateVisibility::Collapsed);
			if (Bg) { Bg->SetVisibility(ESlateVisibility::Collapsed); }
			if (NumIcon) { NumIcon->SetVisibility(ESlateVisibility::Collapsed); }
			if (Sel) { Sel->SetVisibility(ESlateVisibility::Collapsed); }
			continue;
		}
		const int32 BarIndex = i + SpellHotbarScrollOffset;
		const int32 SpellId = Bar.IsValidIndex(BarIndex) ? Bar[BarIndex] : 0;
		SpellBarSpellIds[i] = SpellId;
		FString SpellName;
		uint32 IconDid = 0;
		if (SpellId != 0 && Dat)
		{
			Dat->TryGetSpellInfo(static_cast<uint32>(SpellId), SpellName, IconDid);
		}
		const float SlotX = static_cast<float>(Origin.X + i * (Cell + SlotGap));
		const float SlotY = static_cast<float>(Origin.Y);
		// Empty favorites 1–9 use the retail blue numbered tiles. Occupied slots
		// retain the small key badge; scrolling never renumbers absolute favorites.
		constexpr int32 DidSpellSlotBg = 0x06001A97;
		const int32 BackgroundDid = (SpellId == 0 && BarIndex < 9)
			? 0x060010FA + BarIndex : DidSpellSlotBg;
		constexpr int32 DidSpellSelected = 0x06004D09;
		const int32 DigitDid = (BarIndex < 9) ? (0x060019ED + BarIndex) : 0x060019EC;
		if (Bg)
		{
			Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
			SetIconDid(Bg, BackgroundDid);
			PlaceBorderAt(Bg, SlotX, SlotY, 578);
		}
		if (SpellId != 0)
		{
			Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
			SetSpellIcon(Icon, SpellId);
			if (!SpellName.IsEmpty())
			{
				SetRetailTooltip(Icon, FText::FromString(SpellName));
			}
			PlaceBorderAt(Icon, SlotX, SlotY, 580);
		}
		else
		{
			Icon->SetVisibility(ESlateVisibility::Collapsed);
			SetIconDid(Icon, 0);
		}
		if (NumIcon && BarIndex < 10 && SpellId != 0)
		{
			NumIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
			SetIconDid(NumIcon, DigitDid);
			PlaceBorderAt(NumIcon, SlotX, SlotY, 582);
		}
		else if (NumIcon)
		{
			NumIcon->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (Sel)
		{
			const bool bSel = (BarIndex == SelectedCombatSpellSlot && SpellId != 0);
			Sel->SetVisibility(bSel ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (bSel)
			{
				SetIconDid(Sel, DidSpellSelected);
				PlaceBorderAt(Sel, SlotX, SlotY, 583);
			}
		}
	}
	if (TSharedPtr<FACEUIElement> NameEl = Manager->FindElementByName(TEXT("Spellcast_SpellName")))
	{
		int32 SpellId = 0;
		if (SelectedCombatSpellSlot < 0)
		{
			SpellId = BuiltInSpellId;
		}
		else if (Bar.IsValidIndex(SelectedCombatSpellSlot))
		{
			SpellId = Bar[SelectedCombatSpellSlot];
		}
		FString SpellName;
		uint32 IconDid = 0;
		if (SpellId != 0 && Dat)
		{
			Dat->TryGetSpellInfo(static_cast<uint32>(SpellId), SpellName, IconDid);
		}
        if (SelectedCombatSpellSlot < 0 && BuiltInCasterGuid != 0)
        {
            FACEWorldObject Caster;
            if (Client->GetWorldObject(BuiltInCasterGuid, Caster))
                SpellName = ACERetailObjectNames::Name(Caster) + TEXT(" (") + SpellName + TEXT(")");
        }
		if (!SpellcastSpellNameLabel && Canvas->WidgetTree)
		{
			SpellcastSpellNameLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			SpellcastSpellNameLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
			SpellcastSpellNameLabel->SetJustification(ETextJustify::Center);
		}
		PlaceTextOnElement(SpellcastSpellNameLabel, TEXT("Spellcast_SpellName"), SpellName, 9, TextGold, 581);
	}
	else if (SpellcastSpellNameLabel)
	{
		SpellcastSpellNameLabel->SetVisibility(ESlateVisibility::Collapsed);
	}

	// Explicit "Cast" label — DAT StripButton has no text layer in UMG.
	if (TSharedPtr<FACEUIElement> CastEl = Manager->FindElementByName(TEXT("CastSpellButton")))
	{
		const int32 SelectedSpell = SelectedCombatSpellSlot < 0 ? BuiltInSpellId
			: Bar.IsValidIndex(SelectedCombatSpellSlot) ? Bar[SelectedCombatSpellSlot] : 0;
		const auto Target = Client->GetSelectedObject();
		int32 ResolvedTarget = 0;
		const bool bCanCast = Client->ResolveSpellCastTarget(SelectedSpell, Target.bValid ? Target.Guid : 0, ResolvedTarget);
		CastEl->bActivatable = bCanCast;
		CastEl->bGhosted = !bCanCast;
		if (!CastSpellLabel && Canvas->WidgetTree)
		{
			CastSpellLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			CastSpellLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
			CastSpellLabel->SetJustification(ETextJustify::Center);
		}
		if (CastSpellLabel)
		{
			CastSpellLabel->SetText(FText::FromString(TEXT("Cast")));
			CastSpellLabel->SetColorAndOpacity(FSlateColor(bCanCast ? FLinearColor(0.95f, 0.92f, 0.75f, 1.f) : FLinearColor(.45f,.45f,.42f,1)));
			FString Name; uint32 Icon = 0;
			if (Dat) Dat->TryGetSpellInfo(SelectedSpell, Name, Icon);
			UACEHoverTooltipWidget::SetWidgetTooltip(CastSpellLabel, FText::FromString(!SelectedSpell ? TEXT("Select a spell to cast")
				: bCanCast ? TEXT("CAST ") + Name : TEXT("You must select an appropriate target for ") + Name));
			FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11);
			CastSpellLabel->SetFont(Font);
			CastSpellLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
			Canvas->PlaceWidgetAtElement(CastSpellLabel, CastEl, 620, FMargin(4.f, 6.f, 4.f, 4.f));
		}
	}
	else if (CastSpellLabel)
	{
		CastSpellLabel->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UACEUIGameplayBinder::RefreshSpellbookChromeLabels()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree
		|| ActivePanelPage != TEXT("SpellManagementPanel_Field"))
	{
		for (UTextBlock* L : SpellbookChromeLabels)
		{
			if (L) { L->SetVisibility(ESlateVisibility::Collapsed); }
		}
		return;
	}

	struct FLabelSpec
	{
		const TCHAR* Ancestor;
		const TCHAR* Element;
		const TCHAR* Text;
		int32 FontSize;
		FMargin Inset;
	};
	static const FLabelSpec Specs[] = {
		{ TEXT("SpellManagementPanel_Field"), TEXT("SpellbookTab"), TEXT("Spellbook"), 9, FMargin(20.f, 5.f, 8.f, 4.f) },
		{ TEXT("SpellManagementPanel_Field"), TEXT("SpellComponentTab"), TEXT("Components"), 9, FMargin(12.f, 5.f, 8.f, 4.f) },
		{ TEXT("FilterBox"), TEXT("Schools_Text"), TEXT("Schools"), 9, FMargin(2.f, 2.f) },
		{ TEXT("FilterBox"), TEXT("Levels_Text"), TEXT("Levels"), 9, FMargin(2.f, 2.f) },
		{ TEXT("FilterBox"), TEXT("School_Creature"), TEXT("Creature"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("School_Item"), TEXT("Item"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("School_Life"), TEXT("Life"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("School_War"), TEXT("War"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("School_Void"), TEXT("Void"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_One"), TEXT("1"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_Two"), TEXT("2"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_Three"), TEXT("3"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_Four"), TEXT("4"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_Five"), TEXT("5"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_Six"), TEXT("6"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_Seven"), TEXT("7"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
		{ TEXT("FilterBox"), TEXT("Level_Eight"), TEXT("8"), 8, FMargin(16.f, 0.f, 2.f, 0.f) },
	};

	while (SpellbookChromeLabels.Num() < UE_ARRAY_COUNT(Specs) && Canvas->WidgetTree)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		SpellbookChromeLabels.Add(L);
	}

	const bool bShowFilters = ActiveSpellPanelTab == TEXT("SpellbookPage");
	for (int32 i = 0; i < UE_ARRAY_COUNT(Specs); ++i)
	{
		UTextBlock* Label = SpellbookChromeLabels.IsValidIndex(i) ? SpellbookChromeLabels[i] : nullptr;
		if (!Label) { continue; }
		const bool bIsTabLabel = (i == 0 || i == 1);
		if (!bIsTabLabel && !bShowFilters)
		{
			Label->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(Specs[i].Ancestor, Specs[i].Element);
		if (!El.IsValid())
		{
			Label->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		bool bAncestorsVisible = El->bVisible;
		for (TSharedPtr<FACEUIElement> P = El->Parent.Pin(); P.IsValid(); P = P->Parent.Pin())
		{
			if (!P->bVisible) { bAncestorsVisible = false; break; }
		}
		if (!bAncestorsVisible)
		{
			Label->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		Label->SetVisibility(ESlateVisibility::HitTestInvisible);
		Label->SetText(FText::FromString(Specs[i].Text));
		Label->SetColorAndOpacity(FSlateColor(TextGold));
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), Specs[i].FontSize);
		Label->SetFont(Font);
		Canvas->PlaceWidgetAtElement(Label, El, StatOverlayZ + 1, Specs[i].Inset);
	}
}

const TArray<int32>& UACEUIGameplayBinder::FilteredSpellbook(UACEDatSubsystem* Dat)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACESpellbookOrdering);
	const auto Session = Client ? Client->GetSession() : nullptr;
	static const TArray<int32> Empty;
	const auto& Known = Session ? Session->GetKnownSpells() : Empty;
	const bool Ready = Dat && Dat->IsDatReady();
	if (SpellbookKnownCache == Known && SpellbookOrderDat.Get() == Dat
		&& bSpellbookOrderDatReady == Ready && SpellbookOrderSchools == SpellbookSchoolFilterMask
		&& SpellbookOrderLevels == SpellbookLevelFilterMask) return SpellbookOrderCache;
	SpellbookOrderDat = Dat; bSpellbookOrderDatReady = Ready;
	SpellbookOrderSchools = SpellbookSchoolFilterMask; SpellbookOrderLevels = SpellbookLevelFilterMask;
	SpellbookKnownCache = Known;
	SpellbookOrderCache = Known;
	auto& Spells = SpellbookOrderCache;
	Spells.RemoveAll([&](int32 SpellId)
	{
		uint32 School = 0, Level = 1;
		if (Dat)
		{
			Dat->TryGetSpellSchoolAndLevel(static_cast<uint32>(SpellId), School, Level);
		}
		const bool bSchoolOk = (School >= 1 && School <= 5)
			? ((SpellbookSchoolFilterMask & (1u << (School - 1))) != 0)
			: true;
		const bool bLevelOk = (Level >= 1 && Level <= 8)
			? ((SpellbookLevelFilterMask & (1u << (Level - 1))) != 0)
			: true;
		return !(bSchoolOk && bLevelOk);
	});
	Spells.Sort([&](int32 A, int32 B)
	{
		uint32 Oa = 0, Ob = 0;
		if (Dat)
		{
			Dat->TryGetSpellDisplayOrder(static_cast<uint32>(A), Oa);
			Dat->TryGetSpellDisplayOrder(static_cast<uint32>(B), Ob);
		}
		return Oa != Ob ? Oa < Ob : A < B;
	});
	return SpellbookOrderCache;
}

void UACEUIGameplayBinder::RefreshSpellbookOverlays()
{
	if (!Client || !Manager || !Canvas || !Canvas->WidgetTree
		|| ActivePanelPage != TEXT("SpellManagementPanel_Field")
		|| ActiveSpellPanelTab != TEXT("SpellbookPage"))
	{
		for (UTextBlock* Row : SpellbookRows)
		{
			if (Row) { Row->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* Icon : SpellbookIcons)
		{
			if (Icon) { Icon->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* Background : SpellbookRowBackgrounds)
			if (Background) Background->SetVisibility(ESlateVisibility::Collapsed);
		for (UBorder* Selection : SpellbookRowSelections)
			if (Selection) Selection->SetVisibility(ESlateVisibility::Collapsed);
		SpellbookFilteredCount = 0;
		return;
	}
	SyncSpellbookFilterCheckboxes();
	UACEDatSubsystem* Dat = Canvas->GetResourceResolver() ? Canvas->GetResourceResolver()->GetDatSubsystem() : nullptr;
	if (PlayerController)
	{
		if (UGameInstance* GI = PlayerController->GetGameInstance())
		{
			Dat = GI->GetSubsystem<UACEDatSubsystem>();
		}
	}
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("SpellbookPage"), TEXT("SpellBook_SpellList"));
	if (!ListEl.IsValid())
	{
		return;
	}
	const auto& Spells = FilteredSpellbook(Dat);
	SpellbookFilteredCount = Spells.Num();
	const FIntPoint Origin = ListEl->GetScreenOrigin();
	const int32 RowH = SpellbookTemplate().Height();
	const auto& Template = SpellbookTemplate();
	const int32 IconW = Template.Icon ? Template.Icon->Width : 32;
	const int32 IconH = Template.Icon ? Template.Icon->Height : 32;
	const int32 IconX = Template.Icon ? Template.Icon->X : 0;
	const int32 IconY = Template.Icon ? Template.Icon->Y : 0;
	const int32 TextX = Template.Text ? Template.Text->X : 42;
	const int32 TextY = Template.Text ? Template.Text->Y : 0;
	const int32 TextH = Template.Text ? Template.Text->Height : RowH;
	const int32 RightMargin = Template.Text && Template.Root ? Template.Root->Width - TextX - Template.Text->Width : 8;
	const int32 TextW = FMath::Max(1, ListEl->Width - TextX - RightMargin);
	const int32 MaxRows = FMath::Max(1, ListEl->Height / RowH);
	if (bRevealLearnedSpell)
	{
		// gmSpellbookUI::SetSelected calls ScrollToView after inserting in DAT
		// display order. Do this once so subsequent manual scrolling stays put.
		const int32 Selected = Spells.Find(SelectedSpellbookId);
		if (Selected != INDEX_NONE)
			SpellbookScrollOffset = FMath::Clamp(SpellbookScrollOffset, FMath::Max(0, Selected - MaxRows + 1), Selected);
		bRevealLearnedSpell = false;
	}
	SpellbookScrollOffset = FMath::Clamp(SpellbookScrollOffset, 0, FMath::Max(0, Spells.Num() - MaxRows));
	SpellbookRowIds.SetNum(MaxRows);
	while (SpellbookRows.Num() < MaxRows && Canvas->WidgetTree)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		Row->SetFont(Font);
		Row->SetColorAndOpacity(FSlateColor(TextWhite));
		SpellbookRows.Add(Row);
	}
	for (int32 i = 0; i < FMath::Max(SpellbookRows.Num(), MaxRows); ++i)
	{
		UBorder* Icon = EnsureIconBorder(SpellbookIcons, i);
		UBorder* Background = EnsureIconBorder(SpellbookRowBackgrounds, i);
		UBorder* Selection = EnsureIconBorder(SpellbookRowSelections, i);
		UTextBlock* Row = SpellbookRows.IsValidIndex(i) ? SpellbookRows[i] : nullptr;
		const int32 SpellIndex = SpellbookScrollOffset + i;
		if (i >= MaxRows || !Spells.IsValidIndex(SpellIndex))
		{
			if (Background) { Background->SetVisibility(ESlateVisibility::Collapsed); }
			if (Selection) { Selection->SetVisibility(ESlateVisibility::Collapsed); }
			if (Row) { Row->SetVisibility(ESlateVisibility::Collapsed); }
			if (Icon) { Icon->SetVisibility(ESlateVisibility::Collapsed); }
			if (SpellbookRowIds.IsValidIndex(i)) { SpellbookRowIds[i] = 0; }
			continue;
		}
		const int32 SpellId = Spells[SpellIndex];
		SpellbookRowIds[i] = SpellId;
		FString SpellName;
		uint32 IconDid = 0;
		if (Dat)
		{
			Dat->TryGetSpellInfo(static_cast<uint32>(SpellId), SpellName, IconDid);
		}
		if (SpellName.IsEmpty())
		{
			SpellName = FString::Printf(TEXT("Spell %d"), SpellId);
		}
		const float RowY = static_cast<float>(Origin.Y + i * RowH);
		if (Background && Template.Root)
		{
			SetIconDid(Background, Template.Root->ImageFileId);
			if (Template.Root->DrawMode == 1)
			{
				FSlateBrush Brush = Background->Background;
				Brush.Tiling = ESlateBrushTileType::Both;
				Background->SetBrush(Brush);
			}
			Background->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (Background->GetParent() != Canvas->GetElementLayer()) Canvas->GetElementLayer()->AddChild(Background);
			if (auto* Slot = Cast<UCanvasPanelSlot>(Background->Slot))
			{
				Slot->SetPosition(FVector2D(Origin.X, RowY));
				Slot->SetSize(FVector2D(ListEl->Width, RowH));
				Canvas->SetOverlayOrder(Background, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ - 1);
			}
		}
		if (Selection)
		{
			SetIconDid(Selection, Template.Selected ? Template.Selected->ImageFileId : 0);
			Selection->SetVisibility(SpellId == SelectedSpellbookId ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (Selection->GetParent() != Canvas->GetElementLayer()) Canvas->GetElementLayer()->AddChild(Selection);
			if (auto* Slot = Cast<UCanvasPanelSlot>(Selection->Slot))
			{
				Slot->SetPosition(FVector2D(Origin.X, RowY));
				Slot->SetSize(FVector2D(ListEl->Width, RowH));
				Canvas->SetOverlayOrder(Selection, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ - 1);
			}
		}
		if (Icon)
		{
			SetSpellIcon(Icon, SpellId);
			Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (Icon->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Icon);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Icon->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + IconX), RowY + IconY));
				Slot->SetSize(FVector2D(static_cast<float>(IconW), static_cast<float>(IconH)));
				Canvas->SetOverlayOrder(Icon, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ);
			}
		}
		if (Row)
		{
			if (auto* Retail = Cast<UACERetailTextBlock>(Row); Retail && Template.Text)
			{
				Retail->SetRetailElement(Canvas->GetResourceResolver(), Template.Text, FVector2D(1,1), TextW);
				Retail->SetColorAndOpacity(FSlateColor(Template.Text->TextColor.Get(FLinearColor::White)));
			}
			Row->SetText(FText::FromString(SpellName));
			Row->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (Row->GetParent() != Canvas->GetElementLayer())
			{
				Canvas->GetElementLayer()->AddChild(Row);
			}
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Row->Slot))
			{
				Slot->SetAnchors(FAnchors(0.f, 0.f));
				Slot->SetPosition(FVector2D(static_cast<float>(Origin.X + TextX), RowY + TextY));
				Slot->SetSize(FVector2D(static_cast<float>(TextW), static_cast<float>(TextH)));
				Canvas->SetOverlayOrder(Row, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), StatOverlayZ);
			}
		}
	}
}

void UACEUIGameplayBinder::HandleChatTextCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnEnter)
	{
		return;
	}
	// Prefer the committed text — GetText() can already be cleared by focus loss.
	const FString Committed = Text.ToString();
	TrySendChatFromEntry(&Committed);
}

void UACEUIGameplayBinder::EnsureOverlays()
{
	if (!Canvas || !Canvas->WidgetTree || !Manager)
	{
		return;
	}
	UWidgetTree* Tree = Canvas->WidgetTree;

	auto MakeLabel = [&](TObjectPtr<UTextBlock>& Label)
	{
		if (!Label)
		{
			Label = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			Label->SetVisibility(ESlateVisibility::HitTestInvisible);
			Label->SetJustification(ETextJustify::Center);
			FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8);
			Label->SetFont(Font);
			Label->SetColorAndOpacity(FSlateColor(TextWhite));
		}
	};
	MakeLabel(HealthLabel);
	MakeLabel(StaminaLabel);
	MakeLabel(ManaLabel);
	MakeLabel(AttrHeaderName);
	MakeLabel(AttrHeaderTitle);
	MakeLabel(AttrHeaderPk);
	MakeLabel(AttrHeaderLevelLabel);
	MakeLabel(AttrHeaderLevel);
	MakeLabel(AttrHeaderTotalXpLabel);
	MakeLabel(AttrHeaderXp);
	MakeLabel(AttrHeaderXpToLevelLabel);
	MakeLabel(AttrHeaderXpToLevel);
	MakeLabel(AttrHeaderLuminanceLabel);
	MakeLabel(AttrHeaderLuminanceValue);
	MakeLabel(CombatSpeedLabel);
	MakeLabel(CombatPowerLabel);
	MakeLabel(AttrFooterTitle);
	MakeLabel(AttrFooterLine1Label);
	MakeLabel(AttrFooterLine1Value);
	MakeLabel(AttrFooterLine2Label);
	MakeLabel(AttrFooterLine2Value);

	if (!SelectionText)
	{
		SelectionText = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		SelectionText->SetVisibility(ESlateVisibility::HitTestInvisible);
		SelectionText->SetJustification(ETextJustify::Center);
		SelectionText->SetAutoWrapText(true);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8);
		SelectionText->SetFont(Font);
		SelectionText->SetColorAndOpacity(FSlateColor(TextWhite));
	}
	if (!SelectionStackAmountLabel)
	{
		SelectionStackAmountLabel = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		SelectionStackAmountLabel->SetVisibility(ESlateVisibility::Collapsed);
		SelectionStackAmountLabel->SetJustification(ETextJustify::Center);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8);
		SelectionStackAmountLabel->SetFont(Font);
		SelectionStackAmountLabel->SetColorAndOpacity(FSlateColor(TextWhite));
	}
	if (!ExamTitle)
	{
		ExamTitle = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		ExamTitle->SetVisibility(ESlateVisibility::Collapsed);
		ExamTitle->SetJustification(ETextJustify::Left);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 10);
		ExamTitle->SetFont(Font);
		ExamTitle->SetColorAndOpacity(FSlateColor(TextGold));
	}
	if (!ExamBody)
	{
		ExamBody = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		ExamBody->SetVisibility(ESlateVisibility::Collapsed);
		ExamBody->SetAutoWrapText(true);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		ExamBody->SetFont(Font);
		ExamBody->SetColorAndOpacity(FSlateColor(TextWhite));
	}
	if (!PanelBodyText)
	{
		PanelBodyText = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		PanelBodyText->SetVisibility(ESlateVisibility::Collapsed);
		PanelBodyText->SetAutoWrapText(true);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		PanelBodyText->SetFont(Font);
		PanelBodyText->SetColorAndOpacity(FSlateColor(TextWhite));
	}
	if (!VendorFilterText)
	{
		VendorFilterText = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		VendorFilterText->SetVisibility(ESlateVisibility::Collapsed);
		VendorFilterText->SetAutoWrapText(false);
		VendorFilterText->SetJustification(ETextJustify::Left);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		VendorFilterText->SetFont(Font);
		VendorFilterText->SetColorAndOpacity(FSlateColor(TextGold));
	}
	if (!ChatLog)
	{
		ChatLog = Tree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
		// Only rows are interactive; empty log space leaves DAT chrome clickable.
		// wheel scrolling is handled by the canvas when the pointer is over the log.
		ChatLog->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		ChatLog->SetScrollBarVisibility(ESlateVisibility::Collapsed);
		ChatLog->SetConsumeMouseWheel(EConsumeMouseWheel::Never);
		ChatLog->SetAnimateWheelScrolling(false);
	}
	if (!ChatEntry)
	{
		auto* MainChat = Tree->ConstructWidget<UACEChatEntry>();
		MainChat->InitializeChat(this); ChatEntry = MainChat;
		ChatEntry->SetVisibility(ESlateVisibility::Collapsed);
		ChatEntry->SetHintText(FText::FromString(TEXT("")));
		// Transparent field — DAT ChatPanelTextEntry / ChatEntryField supply the chrome.
		// Font must fit the retail 17px entry height (default slate font looks huge/scaled).
		FEditableTextBoxStyle Style = ChatEntry->GetWidgetStyle();
		FSlateBrush Clear;
		Clear.DrawAs = ESlateBrushDrawType::NoDrawType;
		Style.BackgroundImageNormal = Clear;
		Style.BackgroundImageHovered = Clear;
		Style.BackgroundImageFocused = Clear;
		Style.BackgroundImageReadOnly = Clear;
		Style.Padding = FMargin(2.f, 0.f);
		FSlateFontInfo EntryFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		Style.TextStyle.SetFont(EntryFont);
		Style.TextStyle.ColorAndOpacity = FSlateColor(TextWhite);
		ChatEntry->SetWidgetStyle(Style);
		ChatEntry->SetClearKeyboardFocusOnCommit(true);
		ChatEntry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleChatTextCommitted);
	}

	// FloatyChat1-4 per-window logs, entry lines, and titles (retail ChatInterface).
	if (FloatyChatLogs.Num() != NumFloatyChats)
	{
		LoadFloatyChatSettings();
	}
	for (int32 W = 0; W < NumFloatyChats; ++W)
	{
		if (!FloatyChatLogs[W])
		{
			UScrollBox* Log = Tree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
			Log->SetVisibility(ESlateVisibility::Collapsed);
			Log->SetScrollBarVisibility(ESlateVisibility::Collapsed);
			Log->SetConsumeMouseWheel(EConsumeMouseWheel::Never);
			Log->SetAnimateWheelScrolling(false);
			FloatyChatLogs[W] = Log;
		}
		if (!FloatyChatEntries[W])
		{
			auto* Entry = Tree->ConstructWidget<UACEChatEntry>();
			Entry->InitializeChat(this);
			Entry->SetVisibility(ESlateVisibility::Collapsed);
			Entry->SetHintText(FText::FromString(TEXT("")));
			FEditableTextBoxStyle Style = Entry->GetWidgetStyle();
			FSlateBrush Clear;
			Clear.DrawAs = ESlateBrushDrawType::NoDrawType;
			Style.BackgroundImageNormal = Clear;
			Style.BackgroundImageHovered = Clear;
			Style.BackgroundImageFocused = Clear;
			Style.BackgroundImageReadOnly = Clear;
			Style.Padding = FMargin(2.f, 0.f);
			Style.TextStyle.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9));
			Style.TextStyle.ColorAndOpacity = FSlateColor(TextWhite);
			Entry->SetWidgetStyle(Style);
			Entry->SetClearKeyboardFocusOnCommit(true);
			switch (W)
			{
			case 0:
				Entry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleFloatyChat1Committed);
				break;
			case 1:
				Entry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleFloatyChat2Committed);
				break;
			case 2:
				Entry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleFloatyChat3Committed);
				break;
			default:
				Entry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleFloatyChat4Committed);
				break;
			}
			FloatyChatEntries[W] = Entry;
		}
		if (!FloatyChatTitleLabels[W])
		{
			UTextBlock* Title = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			Title->SetVisibility(ESlateVisibility::Collapsed);
			Title->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 8));
			Title->SetColorAndOpacity(FSlateColor(TextGold));
			Title->SetJustification(ETextJustify::Center);
			FloatyChatTitleLabels[W] = Title;
		}
	}

	if (!RadarCoordsLabel)
	{
		RadarCoordsLabel = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		RadarCoordsLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		RadarCoordsLabel->SetJustification(ETextJustify::Center);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8);
		RadarCoordsLabel->SetFont(Font);
		RadarCoordsLabel->SetColorAndOpacity(FSlateColor(TextWhite));
	}
	if (!RadarPlayerDot)
	{
		RadarPlayerDot = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		RadarPlayerDot->SetPadding(FMargin(0.f));
		RadarPlayerDot->SetBrush(MakeRoundBlipBrush(FLinearColor::White));
		RadarPlayerDot->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	while (RadarBlips.Num() < MaxRadarBlips)
	{
		UBorder* Blip = Tree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Blip->SetPadding(FMargin(0.f));
		Blip->SetBrush(MakeRoundBlipBrush(FLinearColor::Red));
		Blip->SetVisibility(ESlateVisibility::Collapsed);
		RadarBlips.Add(Blip);
	}
	RadarBlipGuids.SetNum(MaxRadarBlips);
}

void UACEUIGameplayBinder::PlaceTextOnElement(UTextBlock* Text, const FString& ElementName,
	const FString& Contents, int32 FontSize, const FLinearColor& Color, int32 ZOrder,
	bool bCenterInElement)
{
	if (!Text || !Manager || !Canvas)
	{
		return;
	}
	PlaceTextOnElement(Text, Manager->FindElementByName(ElementName), Contents, FontSize, Color,
		ZOrder, bCenterInElement);
}

void UACEUIGameplayBinder::PlaceTextOnElement(UTextBlock* Text, TSharedPtr<FACEUIElement> El,
	const FString& Contents, int32 FontSize, const FLinearColor& Color, int32 ZOrder,
	bool bCenterInElement)
{
	if (!Text || !Manager || !Canvas)
	{
		return;
	}
	if (!El.IsValid())
	{
		Text->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	bool bAncestorsVisible = El->bVisible;
	for (TSharedPtr<FACEUIElement> P = El->Parent.Pin(); P.IsValid(); P = P->Parent.Pin())
	{
		if (!P->bVisible) { bAncestorsVisible = false; break; }
	}
	if (!bAncestorsVisible)
	{
		Text->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	Text->SetVisibility(ESlateVisibility::HitTestInvisible);
	Text->SetText(FText::FromString(Contents));
	Text->SetColorAndOpacity(FSlateColor(El->TextColor.Get(Color)));
	// Prefer the DAT font catalog entry (BaseLayout 0x2100003F) over the caller's guess.
	const ACEUIFontStyles::FACEFontStyle Style =
		ACEUIFontStyles::ResolveForElement(El, FontSize);
	Text->SetFont(ACEUIFontStyles::MakeSlateFont(Style));
	if (El->bHasTextLayout)
	{
		Text->SetJustification(El->TextHorizontalJustification == 1 ? ETextJustify::Center
			: El->TextHorizontalJustification == 3 ? ETextJustify::Right : ETextJustify::Left);
		if (Cast<UACERetailTextBlock>(Text) && El->FontId != 0)
		{
			const FVector2D Scale = Canvas->GetLastScale2D();
			Text->SetMargin(FMargin(El->TextMargins.Left * Scale.X, El->TextMargins.Top * Scale.Y,
				El->TextMargins.Right * Scale.X, El->TextMargins.Bottom * Scale.Y));
			Canvas->PlaceWidgetAtElement(Text, El, ZOrder);
			return;
		}
		FMargin Insets = El->TextMargins;
		const float LineHeight = El->FontHeight > 0 ? El->FontHeight : Style.SlateSize + 2.f;
		const float Spare = FMath::Max(0.f, El->Height - Insets.Top - Insets.Bottom - LineHeight);
		if (El->TextVerticalJustification == 1) Insets.Top += FMath::FloorToFloat(Spare * 0.5f);
		else if (El->TextVerticalJustification == 5) Insets.Top += Spare;
		Canvas->PlaceWidgetAtElement(Text, El, ZOrder, Insets);
	}
	else if (bCenterInElement)
	{
		Text->SetJustification(ETextJustify::Center);
		// Full element box + vertical center via symmetric inset (PlaceWidget is top-left).
		const float VPad = FMath::Max(0.f,
			(static_cast<float>(El->Height) - static_cast<float>(Style.SlateSize) - 2.f) * 0.5f);
		Canvas->PlaceWidgetAtElement(Text, El, ZOrder, FMargin(2.f, VPad, 2.f, VPad));
	}
	else
	{
		Canvas->PlaceWidgetAtElement(Text, El, ZOrder, FMargin(2.f, 1.f));
	}
}

void UACEUIGameplayBinder::RefreshVitalsOverlays()
{
	if (!Client || !Manager)
	{
		return;
	}
	// CharacterOption 0x13 SideBySideVitals: checked = one horizontal strip (Health|Stamina|Mana).
	// Unchecked = stacked FloatyVitals (Health on top of Stamina on top of Mana).
	const bool bSideBySide = Client->IsCharacterOptionSet(0x13);
	auto SetVitalsFloaty = [this](const FString& Name, bool bShow)
	{
		if (TSharedPtr<FACEUIElement> El = Manager->FindElementByName(Name))
		{
			El->bVisible = bShow;
		}
	};
	SetVitalsFloaty(TEXT("RootGameplay_FloatySideVitals_Field"), bSideBySide);
	SetVitalsFloaty(TEXT("RootGameplay_FloatyVitals_Field"), !bSideBySide);

	// Always pull the live session vitals — do not trust a stale LastVitals snapshot.
	LastVitals = Client->GetPlayerVitals();
	if (!LastVitals.bValid)
	{
		for(UTextBlock* Label:{HealthLabel.Get(),StaminaLabel.Get(),ManaLabel.Get()})
			if(Label)Label->SetText(FText::GetEmpty());
		return;
	}
	const float HealthFrac = (LastVitals.MaxHealth > 0)
		? static_cast<float>(LastVitals.Health) / static_cast<float>(LastVitals.MaxHealth) : 0.f;
	const float StamFrac = (LastVitals.MaxStamina > 0)
		? static_cast<float>(LastVitals.Stamina) / static_cast<float>(LastVitals.MaxStamina) : 0.f;
	const float ManaFrac = (LastVitals.MaxMana > 0)
		? static_cast<float>(LastVitals.Mana) / static_cast<float>(LastVitals.MaxMana) : 0.f;

	// Both arrangements use the DAT ShowDetail/HideDetail states. Visibility alone
	// cannot select the textured media: HideDetail deliberately has image ID zero.
	for (const TCHAR* Root : {TEXT("RootGameplay_FloatyVitals_Field"), TEXT("RootGameplay_FloatySideVitals_Field")})
	{
		auto SetMeter = [&](const TCHAR* Name, float Fraction)
		{
			if (const auto Meter = Manager->FindElementUnder(Root, Name))
			{
				Meter->MeterFillFraction = FMath::Clamp(Fraction, 0.f, 1.f);
				Meter->bUseExplicitState = true;
				Meter->DefaultState = bShowVitalNumbers ? 0x10000006u : 0x10000007u;
			}
		};
		SetMeter(TEXT("HealthMeter"), HealthFrac);
		SetMeter(TEXT("StaminaMeter"), StamFrac);
		SetMeter(TEXT("ManaMeter"), ManaFrac);
	}

	const FString HealthText = (bShowVitalNumbers && LastVitals.bValid)
		? FString::Printf(TEXT("%d / %d"), LastVitals.Health, LastVitals.MaxHealth) : FString();
	const FString StamText = (bShowVitalNumbers && LastVitals.bValid)
		? FString::Printf(TEXT("%d / %d"), LastVitals.Stamina, LastVitals.MaxStamina) : FString();
	const FString ManaText = (bShowVitalNumbers && LastVitals.bValid)
		? FString::Printf(TEXT("%d / %d"), LastVitals.Mana, LastVitals.MaxMana) : FString();
	const TCHAR* ActiveRoot=bSideBySide?TEXT("RootGameplay_FloatySideVitals_Field"):TEXT("RootGameplay_FloatyVitals_Field");
	PlaceTextOnElement(HealthLabel, Manager->FindElementUnder(ActiveRoot,TEXT("PlayerHealthLabel")), HealthText, 8, TextWhite, 510);
	PlaceTextOnElement(StaminaLabel, Manager->FindElementUnder(ActiveRoot,TEXT("PlayerStaminaLabel")), StamText, 8, TextWhite, 510);
	PlaceTextOnElement(ManaLabel, Manager->FindElementUnder(ActiveRoot,TEXT("PlayerManaLabel")), ManaText, 8, TextWhite, 510);
}

void UACEUIGameplayBinder::PlaceRadarWidget(UWidget* Widget, float ScreenX, float ScreenY, float Size, int32 ZOrder)
{
	if (!Widget || !Canvas || !Canvas->GetElementLayer())
	{
		return;
	}
	UCanvasPanel* Layer = Canvas->GetElementLayer();
	if (Widget->GetParent() != Layer)
	{
		Layer->AddChild(Widget);
	}
	if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Widget->Slot))
	{
		Slot->SetAnchors(FAnchors(0.f, 0.f));
		Slot->SetAlignment(FVector2D(0.f, 0.f));
		Slot->SetAutoSize(false);
		Slot->SetPosition(FVector2D(ScreenX, ScreenY)*Canvas->GetLastScale2D());
		Slot->SetSize(FVector2D(Size, Size)*Canvas->GetLastScale2D());
		Canvas->SetOverlayOrder(Widget, Manager->FindElementByName(TEXT("RootGameplay_Radar_Field")), ZOrder);
	}
}

FLinearColor UACEUIGameplayBinder::ColorFromRadarBlip(uint8 RadarColor)
{
    return ACERadarColors::Tint(RadarColor);
}

uint8 UACEUIGameplayBinder::ResolveRadarColor(const FACEWorldObject& Obj, const FACEFellowshipInfo* Fellowship)
{
    return ACERadarColors::Resolve(Obj, Fellowship);
}

bool UACEUIGameplayBinder::ShouldShowOnRadar(const FACEWorldObject& Obj, int32 SelfGuid)
{
	if (Obj.Guid == 0 || Obj.Guid == SelfGuid || Obj.bDying || !Obj.bHasPosition || !Obj.Position.IsValid())
	{
		return false;
	}
	if (Obj.ParentGuid != 0 || Obj.ContainerId != 0 || Obj.WielderId != 0)
	{
		return false;
	}
	if (Obj.RadarBehavior == ACERadarBehavior::ShowNever)
	{
		return false;
	}
	if (Obj.IsHiddenAdmin() || Obj.IsCloaked()
		|| (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::UiHidden) != 0)
	{
		return false;
	}
	if (Obj.RadarBehavior == ACERadarBehavior::ShowAlways
		|| Obj.RadarBehavior == ACERadarBehavior::ShowMovement
		|| Obj.RadarBehavior == ACERadarBehavior::ShowAttacking)
	{
		return true;
	}
	// Undefined behavior: retail still shows players / creatures / portals / lifestones.
	if (Obj.bIsPlayer)
	{
		return true;
	}
	if ((Obj.ItemType & ACEItemType::Creature) != 0)
	{
		return true;
	}
	if ((Obj.ItemType & ACEItemType::Portal) != 0
		|| (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::Portal) != 0
		|| Obj.IsLifeStone()
		|| (Obj.ObjectDescriptionFlags & ACEObjectDescFlag::BindStone) != 0)
	{
		return true;
	}
	return Obj.RadarBlipColor != ACERadarColor::Default;
}

void UACEUIGameplayBinder::RefreshRadarOverlays()
{
	if (!Client || !Manager || !Canvas || RadarBlips.Num() == 0)
	{
		return;
	}
	TSharedPtr<FACEUIElement> RadarImage = Manager->FindElementByName(TEXT("RadarImage"));
	if (!RadarImage.IsValid())
	{
		RadarImage = Manager->FindElementByName(TEXT("RootGameplay_Radar_Field"));
	}
	if (!RadarImage.IsValid() || !RadarImage->bVisible)
	{
		if (RadarPlayerDot)
		{
			RadarPlayerDot->SetVisibility(ESlateVisibility::Collapsed);
		}
		for (UBorder* Blip : RadarBlips)
		{
			if (Blip)
			{
				Blip->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		if (RadarCoordsLabel)
		{
			RadarCoordsLabel->SetVisibility(ESlateVisibility::Collapsed);
		}
		return;
	}

	const FACEPosition PlayerPos = Client->GetPlayerPosition();
	const int32 SelfGuid = Client->GetPlayerGuid();
	const FIntPoint Origin = RadarImage->GetScreenOrigin();
	const float RadarW = static_cast<float>(FMath::Max(1, RadarImage->Width));
	const float RadarH = static_cast<float>(FMath::Max(1, RadarImage->Height));
	const float CenterX = static_cast<float>(Origin.X) + RadarW * 0.5f;
	const float CenterY = static_cast<float>(Origin.Y) + RadarH * 0.5f;
	const float RadiusPx = FMath::Min(RadarW, RadarH) * 0.5f - 8.f;
	const float RadarRangeAc = ACERadarVisuals::RangeAc(uint32(PlayerPos.CellId));
	auto* Resources=Canvas->GetResourceResolver();
	if(!Resources)return;
	auto SetBlip=[&](UBorder* Blip,int32 Shape,bool Selected,FLinearColor Color)
	{
		UTexture2D* Texture = Resources->ResolveRadarBlip(Shape, Selected);
		if (Blip->Background.GetResourceObject() != Texture
			|| Blip->Background.DrawAs != ESlateBrushDrawType::Image)
		{
			FSlateBrush Brush; Brush.SetResourceObject(Texture);
			Brush.ImageSize = FVector2D(7,7); Brush.DrawAs = ESlateBrushDrawType::Image;
			Blip->SetBrush(Brush);
		}
		if (Blip->GetBrushColor() != Color) Blip->SetBrushColor(Color);
	};

	auto GlobalXY = [](const FACEPosition& Pos) -> FVector2D
	{
		const uint32 Cell = static_cast<uint32>(Pos.CellId);
		return FVector2D(
			((Cell >> 24) & 0xFF) * 192.f + Pos.Location.X,
			((Cell >> 16) & 0xFF) * 192.f + Pos.Location.Y);
	};

	if (RadarCoordsLabel && PlayerPos.IsValid()
		&& Client && Client->IsCharacterOptionSet(0x14))
	{
		const float NS = (((static_cast<uint32>(PlayerPos.CellId) >> 16) & 0xFF) * 192.f + PlayerPos.Location.Y) / 240.f - 101.95f;
		const float EW = (((static_cast<uint32>(PlayerPos.CellId) >> 24) & 0xFF) * 192.f + PlayerPos.Location.X) / 240.f - 101.95f;
		PlaceTextOnElement(RadarCoordsLabel, TEXT("RadarCoords"),
			FString::Printf(TEXT("%.1f%s, %.1f%s"),
				FMath::Abs(NS), NS >= 0.f ? TEXT("N") : TEXT("S"),
				FMath::Abs(EW), EW >= 0.f ? TEXT("E") : TEXT("W")),
			8, TextWhite, 520);
		RadarCoordsLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	else if (RadarCoordsLabel)
	{
		RadarCoordsLabel->SetVisibility(ESlateVisibility::Collapsed);
	}

	if (RadarPlayerDot)
	{
		RadarPlayerDot->SetVisibility(ESlateVisibility::HitTestInvisible);
		SetBlip(RadarPlayerDot,8,false,ColorFromRadarBlip(ACERadarColor::BrightGreen));
		PlaceRadarWidget(RadarPlayerDot, CenterX - 3.f, CenterY - 3.f, 7.f, 525);
	}

	const FVector2D SelfXY = PlayerPos.IsValid() ? GlobalXY(PlayerPos) : FVector2D::ZeroVector;
	// Facing-up radar: AC forward (sin θ, cos θ) from set_heading.
	const FVector FwdAc = PlayerPos.IsValid()
		? PlayerPos.GetAceForwardInAcSpace().GetSafeNormal2D()
		: FVector(0.f, 1.f, 0.f);
	const float CosH = FwdAc.Y;
	const float SinH = FwdAc.X;
	const float HeadingRad = FMath::Atan2(FwdAc.X, FwdAc.Y);

	// gmRadarUI: NESW tokens orbit so North stays world-north on a facing-up radar.
	{
		TSharedPtr<FACEUIElement> RadarParent = RadarImage->Parent.Pin();
		auto FindToken = [&](const TCHAR* Name) -> TSharedPtr<FACEUIElement>
		{
			if (RadarParent.IsValid())
			{
				for (const TSharedPtr<FACEUIElement>& Ch : RadarParent->Children)
				{
					if (Ch.IsValid() && Ch->ElementName == Name)
					{
						return Ch;
					}
				}
			}
			return Manager->FindElementByName(Name);
		};
		TSharedPtr<FACEUIElement> North = FindToken(TEXT("RadarNorth"));
		TSharedPtr<FACEUIElement> East = FindToken(TEXT("RadarEast"));
		TSharedPtr<FACEUIElement> South = FindToken(TEXT("RadarSouth"));
		TSharedPtr<FACEUIElement> West = FindToken(TEXT("RadarWest"));
		const float Cx = static_cast<float>(RadarImage->X) + RadarW * 0.5f;
		const float Cy = static_cast<float>(RadarImage->Y) + RadarH * 0.5f;
		auto TokenMag = [Cx, Cy](const TSharedPtr<FACEUIElement>& Tok) -> float
		{
			if (!Tok.IsValid())
			{
				return 0.f;
			}
			const float Tx = static_cast<float>(Tok->X) + static_cast<float>(Tok->Width) * 0.5f;
			const float Ty = static_cast<float>(Tok->Y) + static_cast<float>(Tok->Height) * 0.5f;
			return FMath::Sqrt(FMath::Square(Cx - Tx) + FMath::Square(Cy - Ty));
		};
		if (!bRadarCompassAuthored)
		{
			RadarNorthMag = TokenMag(North);
			RadarEastMag = TokenMag(East);
			RadarSouthMag = TokenMag(South);
			RadarWestMag = TokenMag(West);
			bRadarCompassAuthored = RadarNorthMag > 1.f;
		}
		auto PlaceToken = [Cx, Cy, HeadingRad](const TSharedPtr<FACEUIElement>& Tok, float Mag, float AngleBias)
		{
			if (!Tok.IsValid() || Mag <= 1.f)
			{
				return;
			}
			const float Ang = HeadingRad + AngleBias;
			const float CenterX = Cx + FMath::Sin(Ang) * Mag;
			const float CenterY = Cy + FMath::Cos(Ang) * Mag;
			Tok->X = FMath::RoundToInt(CenterX - static_cast<float>(Tok->Width) * 0.5f);
			Tok->Y = FMath::RoundToInt(CenterY - static_cast<float>(Tok->Height) * 0.5f);
		};
		PlaceToken(North, RadarNorthMag, PI);
		PlaceToken(East, RadarEastMag, PI * 0.5f);
		PlaceToken(South, RadarSouthMag, 0.f);
		PlaceToken(West, RadarWestMag, PI * 1.5f);
	}

	const auto RadarSession = Client->GetSession();
	if (!RadarSession) return;
	const auto& Fellowship=RadarSession->GetFellowship();
	const auto* SelfObject=RadarSession->GetWorldObjects().Find(SelfGuid);
	const int32 SelectedGuid=Client->GetSelectedObject().Guid;
	int32 BlipIndex = 0;
	for (const auto& Pair : RadarSession->GetWorldObjects())
	{
		// Tick and packet dispatch both run on the game thread. Reading the
		// session view avoids copying every object's clothing/property arrays.
		const FACEWorldObject& Obj = Pair.Value;
		if (BlipIndex >= RadarBlips.Num())
		{
			break;
		}
		// Radar uses server-known positions and distance, not the rooms admitted
		// for rendering by the current camera/portal view.
		if (!ShouldShowOnRadar(Obj, SelfGuid) || !PlayerPos.IsValid())
		{
			continue;
		}
		const FVector2D Delta = GlobalXY(Obj.Position) - SelfXY;
		if (Delta.SizeSquared() > FMath::Square(RadarRangeAc-1.f))
		{
			continue;
		}
		// Rotate so player facing maps to screen-up; screen Y grows downward.
		const float RelX = Delta.X * CosH - Delta.Y * SinH; // AC right
		const float RelY = Delta.X * SinH + Delta.Y * CosH; // AC forward
		const float ScreenX = CenterX + RelX / RadarRangeAc * RadiusPx;
		const float ScreenY = CenterY - RelY / RadarRangeAc * RadiusPx;

		UBorder* Blip = RadarBlips[BlipIndex];
		int32 Shape=4;
        uint8 RadarColor=ResolveRadarColor(Obj,&Fellowship);
        if(Fellowship.bValid && Fellowship.Members.ContainsByPredicate([&](const auto& M){return M.Guid==Obj.Guid;}))
        { Shape=Fellowship.LeaderGuid==Obj.Guid ? 5:6; }
        else if(SelfObject && Obj.MonarchGuid!=0 && Obj.MonarchGuid==SelfObject->MonarchGuid) Shape=2;
        else if(Obj.bIsPlayer && SelfObject && (Obj.ObjectDescriptionFlags & SelfObject->ObjectDescriptionFlags
            & (ACEObjectDescFlag::PlayerKiller|ACEObjectDescFlag::PkLiteStatus))) Shape=3;
        FLinearColor Color=ColorFromRadarBlip(RadarColor);
        if(FMath::Abs(Obj.Position.Location.Z-PlayerPos.Location.Z)>=5.f)
        {
            const FColor Encoded=Color.ToFColorSRGB();
            Color=FLinearColor::FromSRGBColor(FColor(Encoded.R*.65f,Encoded.G*.65f,Encoded.B*.65f));
        }
        SetBlip(Blip,Shape,Obj.Guid==SelectedGuid,Color);
		Blip->SetVisibility(ESlateVisibility::Visible);
		PlaceRadarWidget(Blip, FMath::FloorToFloat(ScreenX) - 3.f, FMath::FloorToFloat(ScreenY) - 3.f, 7.f, 530);
		RadarBlipGuids[BlipIndex] = Obj.Guid;
		++BlipIndex;
	}
	for (int32 i = BlipIndex; i < RadarBlips.Num(); ++i)
	{
		RadarBlips[i]->SetVisibility(ESlateVisibility::Collapsed);
		RadarBlipGuids[i] = 0;
	}
}

void UACEUIGameplayBinder::RefreshSelectionOverlay()
{
	if (Client)
	{
		// Meter replies can arrive independently of a new selection action.
		LastSelection = Client->GetSelectedObject();
	}
	FString Name = LastSelection.bValid ? LastSelection.Name : FString();
	FACEWorldObject SelObj;
	bool bHaveObj = false;
	if (LastSelection.bValid && Client)
	{
		bHaveObj = Client->GetWorldObject(LastSelection.Guid, SelObj);
		if (bHaveObj)
		{
			const auto Session = Client->GetSession();
			const TConstArrayView<FACEWorldObject> Merchandise = OpenVendorGuid && Session
				? TConstArrayView<FACEWorldObject>(Session->GetVendorMerchandise()) : TConstArrayView<FACEWorldObject>();
			const FACEWorldObject* Stock=Merchandise.FindByPredicate(
				[&](const FACEWorldObject& Item){return Item.Guid==SelObj.Guid;});
			if (Stock)
			{
				SelObj=*Stock;
				SelectedStackMax=GetVendorSelectionLimit(SelObj.Guid);
				SelectedStackAmount=SelectedStackMax>0 ? FMath::Clamp(SelectedStackAmount,1,SelectedStackMax) : 0;
				// Retail describes the stock here, independently of the split slider.
				SelObj.StackSize=FMath::Max(1,SelectedStackMax);
			}
			else
			{
				const bool bFullStackSelected = SelectedStackAmount >= SelectedStackMax;
				SelectedStackMax = FMath::Max(1, SelObj.StackSize);
				SelectedStackAmount = bFullStackSelected ? SelectedStackMax : FMath::Clamp(SelectedStackAmount, 1, SelectedStackMax);
			}
			const bool bStackable = SelObj.MaxStackSize > 1 || SelObj.StackSize > 1;
			if (bStackable)
			{
				const bool VendorStock=OpenVendorGuid && GetVendorPurchaseLimit(SelObj.Guid)>0;
				Name = FormatItemStackName(SelObj, VendorStock ? INDEX_NONE : SelectedStackAmount);
			}
			else
			{
				Name = ACERetailObjectNames::Name(SelObj);
			}
		}
	}

	// gmToolbarUI gives split controls priority; only a server mana reply can
	// expose the item mana vial. Structure is durability/salvage units, not mana.
	const bool bShowStack = LastSelection.bValid && bHaveObj && SelectedStackMax > 1;
	const bool bShowHealth = LastSelection.bValid && !bShowStack && LastSelection.bShowHealth;
	const bool bShowMana = LastSelection.bValid && !bShowStack && !bShowHealth && LastSelection.bShowMana;
	const float HealthFrac = FMath::Clamp(LastSelection.HealthFraction, 0.f, 1.f);
	const float ManaFrac = FMath::Clamp(LastSelection.ManaFraction, 0.f, 1.f);
	const float StackFrac = ScrollDragTarget == EACEUIScrollTarget::StackSize ? ScrollDragFraction : (SelectedStackMax > 0)
		? FMath::Clamp(static_cast<float>(SelectedStackAmount) / static_cast<float>(SelectedStackMax), 0.f, 1.f)
		: 0.f;

	EnsureOverlays();

	// Retail DAT meters only — MeterFillFraction clips the fill art (do not invent UMG bars).
	if (Manager)
	{
		if (const auto Field = Manager->FindElementByName(TEXT("SelectedObjectField")))
		{
			// gmToolbarUI::HandleSelectionChanged starts the DAT's ObjectSelected /
			// StackedItemSelected state. Layout 0x21000016 then pauses for 0.25s
			// and enters Normal (1) / StackedItem (0x1000000D), clearing the green
			// SelectionBlinkField even when no health or mana reply follows.
			const bool bFlash = FPlatformTime::Seconds() < ToolbarSelectionFlashUntil;
			Field->bUseExplicitState = true;
			Field->DefaultState = !LastSelection.bValid ? 0u
				: bShowStack ? (bFlash ? 0x1000000Cu : 0x1000000Du)
				: bFlash ? 0x1000000Bu : 1u;
			Field->ResolvePaintState(false, false, false);
		}
		auto SetMeter = [this](const FString& MeterName, bool bVisible, float Frac)
		{
			if (TSharedPtr<FACEUIElement> Meter = Manager->FindElementByName(MeterName))
			{
				Meter->bVisible = bVisible;
				Meter->MeterFillFraction = bVisible ? FMath::Clamp(Frac, 0.f, 1.f) : -1.f;
			}
		};
		SetMeter(TEXT("ToolbarHealthMeter"), bShowHealth, HealthFrac);
		SetMeter(TEXT("ToolbarManaMeter"), bShowMana, ManaFrac);

		if (TSharedPtr<FACEUIElement> Slider = Manager->FindElementByName(TEXT("StackSizeSlider")))
		{
			Slider->bVisible = bShowStack;
			if (bShowStack)
			{
				if (Slider->Children.IsValidIndex(0) && Slider->Children[0].IsValid())
				{
					Slider->Children[0]->bVisible = true;
					Slider->Children[0]->Width = Slider->Width;
				}
				if (Slider->Children.IsValidIndex(1) && Slider->Children[1].IsValid())
				{
					const int32 ThumbW = FMath::Max(8, Slider->Children[1]->Width);
					Slider->Children[1]->X = FMath::Clamp(
						FMath::FloorToInt(static_cast<float>(Slider->Width - ThumbW) * StackFrac),
						0, FMath::Max(0, Slider->Width - ThumbW));
					Slider->Children[1]->bVisible = true;
				}
			}
		}
		if (TSharedPtr<FACEUIElement> Entry = Manager->FindElementByName(TEXT("StackSizeEntryBox")))
		{
			Entry->bVisible = bShowStack;
		}
	}

	// SelectedObjectText uses the DAT's native white bitmap font and 140x31
	// field. The stacked state is left-aligned and one line above the controls.
	if (SelectionText && Manager && Canvas)
	{
		TSharedPtr<FACEUIElement> TextEl = Manager->FindElementByName(TEXT("SelectedObjectText"));
		if (!LastSelection.bValid || !TextEl)
		{
			SelectionText->SetVisibility(ESlateVisibility::Collapsed);
		}
		else
		{
			TextEl->ResolvePaintState(false, false, false);
			TextEl->bTextOneLine = bShowStack;
			TextEl->TextHorizontalJustification = bShowStack ? 2 : 1;
			SelectionText->SetAutoWrapText(!bShowStack);
			PlaceTextOnElement(SelectionText, TextEl, Name, 8, TextWhite, 520);
		}
	}

	RefreshStackAmountEntry(bShowStack);

	EnsureSelectionMarkers();
	if (SelectionMarkerCorners.Num() < 4 || !LastSelection.bValid || LastSelection.Guid == 0 || !PlayerController || !Canvas
		|| (Client && LastSelection.Guid == Client->GetPlayerGuid()))
	{
		HideSelectionMarkers();
		return;
	}

	UWorld* World = PlayerController->GetWorld();
	if (!World)
	{
		HideSelectionMarkers();
		return;
	}

	AActor* Target = FindSelectionActor(World, LastSelection.Guid);

	// SmartBox::GetObjectBoundingBox uses the setup selection sphere, rotated by
	// the object's frame. Animated limbs, weapons and collision proxies do not
	// enlarge the indicator. Retail's missing-setup fallback is a 0.1m sphere.
	FVector WorldOrigin;
	const float WorldScale = PlayerController->WorldScale;
	float Radius = .1f * WorldScale;
	FVector LocalCenter(0,0,Radius);
	if (bHaveObj && SelObj.SetupId != 0)
	{
		if (auto* GI = PlayerController->GetGameInstance())
		if (auto* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			float Step, Height, BodyRadius, SelectionRadius; uint32 Anim;
			FVector3f SelectionCenter;
			if (Dat->TryGetSetupPhysics(SelObj.SetupId, Step, Height, BodyRadius, Anim,
				nullptr, &SelectionCenter, &SelectionRadius))
			{
				const float Scale = WorldScale * FMath::Max(.01f, SelObj.Scale);
				LocalCenter = FACEPosition::AceVectorToUnreal(FVector(SelectionCenter), Scale);
				Radius = FMath::Max(0.f, SelectionRadius) * Scale;
			}
		}
	}
	if (Target)
	{
		FVector Feet = Target->GetActorLocation();
		if (const auto* Capsule = Cast<UCapsuleComponent>(Target->GetRootComponent()))
			Feet.Z -= Capsule->GetScaledCapsuleHalfHeight();
		WorldOrigin = Feet + Target->GetActorQuat().RotateVector(LocalCenter);
	}
	else if (bHaveObj && SelObj.bHasPosition && SelObj.Position.IsValid())
		WorldOrigin = SelObj.Position.ToUnrealLocation(WorldScale) + SelObj.Position.ToUnrealQuat().RotateVector(LocalCenter);
	else
	{
		HideSelectionMarkers();
		return;
	}

	FVector ViewLoc;
	FRotator ViewRot;
	PlayerController->GetPlayerViewPoint(ViewLoc, ViewRot);
	// Render::GetViewerBBox builds a view-facing square, including camera pitch.
	const FVector Right = ViewRot.RotateVector(FVector::RightVector) * Radius;
	const FVector Up = ViewRot.RotateVector(FVector::UpVector) * Radius;
	const FVector Corners[] = {WorldOrigin-Right+Up, WorldOrigin+Right-Up};

	FVector2D ScreenMin(FLT_MAX, FLT_MAX);
	FVector2D ScreenMax(-FLT_MAX, -FLT_MAX);
	int32 Projected = 0;
	for (const FVector& C : Corners)
	{
		// DPI-aware viewport/widget space — do NOT feed ProjectWorldLocationToScreen(..., true)
		// into AbsoluteToLocal (that expects absolute desktop coords and shifts the frame
		// toward the top-left of the viewport).
		FVector2D WidgetPos;
		if (!UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(
			PlayerController, C, WidgetPos, /*bPlayerViewportRelative=*/true))
		{
			continue;
		}
		ScreenMin.X = FMath::Min(ScreenMin.X, WidgetPos.X);
		ScreenMin.Y = FMath::Min(ScreenMin.Y, WidgetPos.Y);
		ScreenMax.X = FMath::Max(ScreenMax.X, WidgetPos.X);
		ScreenMax.Y = FMath::Max(ScreenMax.Y, WidgetPos.Y);
		++Projected;
	}
    const FVector2D ViewSize=Canvas->GetCachedGeometry().GetLocalSize();
    if (Projected<2 || ScreenMax.X<0 || ScreenMax.Y<0 || ScreenMin.X>=ViewSize.X || ScreenMin.Y>=ViewSize.Y)
    {
        HideSelectionMarkers();
        if(SelectionDirectionArrow && ViewSize.X>1 && ViewSize.Y>1 && Canvas->GetResourceResolver())
        {
            const FVector Relative=ViewRot.UnrotateVector(WorldOrigin-ViewLoc);
            FVector2D Direction=Relative.X>0 && Projected>=2 ? (ScreenMin+ScreenMax)*.5-ViewSize*.5
                : FVector2D(Relative.Y,-Relative.Z);
            if(Direction.IsNearlyZero())Direction=FVector2D(0,1);
            auto* Resources=Canvas->GetResourceResolver();
            const uint32 Did=Resources->ResolveTargetIndicatorId(ACERadarVisuals::ArrowEnum(Direction));
            if(UTexture2D* Texture=Resources->ResolveIconTexture(Did))
            {
                const FVector2D Size=FVector2D(Texture->GetSizeX(),Texture->GetSizeY())*Canvas->GetLastScale2D();
                FSlateBrush Brush;Brush.SetResourceObject(Texture);Brush.ImageSize=Size;Brush.DrawAs=ESlateBrushDrawType::Image;
                SelectionDirectionArrow->SetBrush(Brush);
                const auto Fellowship=Client->GetFellowship();
                SelectionDirectionArrow->SetBrushColor(ColorFromSelectionMarker(bHaveObj?ResolveRadarColor(SelObj,&Fellowship):ACERadarColor::White));
                SelectionDirectionArrow->SetVisibility(ESlateVisibility::HitTestInvisible);
                if(auto* Slot=Cast<UCanvasPanelSlot>(SelectionDirectionArrow->Slot))
                {
                    Slot->SetAutoSize(false);Slot->SetSize(Size);Slot->SetPosition(ACERadarVisuals::EdgePosition(Direction,ViewSize,Size));
                    Canvas->SetOverlayOrder(SelectionDirectionArrow,nullptr,8500);
                }
            }
        }
        return;
    }
    if(SelectionDirectionArrow)SelectionDirectionArrow->SetVisibility(ESlateVisibility::Collapsed);

	const auto Fellowship=Client->GetFellowship();
	const uint8 Radar = bHaveObj ? ResolveRadarColor(SelObj,&Fellowship) : ACERadarColor::White;
	const FLinearColor Tint = ColorFromSelectionMarker(Radar);
	auto* Resources = Canvas->GetResourceResolver();
	if (!Resources) { HideSelectionMarkers(); return; }
	// Same image enumeration used by VividTargetIndicator::SetOnScreenColor.
	const int32 DidNW = Resources->ResolveTargetIndicatorId(1), DidNE = Resources->ResolveTargetIndicatorId(2);
	const int32 DidSW = Resources->ResolveTargetIndicatorId(3), DidSE = Resources->ResolveTargetIndicatorId(4);
	constexpr int32 Z = 8500;
	const FVector2D Corner = FVector2D(12,12) * Canvas->GetLastScale2D();
	const FBox2D Frame = ACERadarVisuals::SelectionFrame(ScreenMin, ScreenMax, ViewSize, Canvas->GetLastScale2D());
	const float W = Frame.GetSize().X, H = Frame.GetSize().Y;
	const float CenterX = Frame.GetCenter().X, CenterY = Frame.GetCenter().Y;

	// Anchor the parent on the projected model center (not top-left) so the frame stays
	// centered on the mesh when size changes.
	if (SelectionMarkerBox)
	{
		SelectionMarkerBox->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (UCanvasPanelSlot* BoxSlot = Cast<UCanvasPanelSlot>(SelectionMarkerBox->Slot))
		{
			BoxSlot->SetAnchors(FAnchors(0.f, 0.f));
			BoxSlot->SetAlignment(FVector2D(0.5f, 0.5f));
			BoxSlot->SetAutoSize(false);
			BoxSlot->SetPosition(FVector2D(CenterX, CenterY));
			BoxSlot->SetSize(FVector2D(W, H));
			Canvas->SetOverlayOrder(SelectionMarkerBox, Manager->FindElementByName(TEXT("RootGameplay_SmartBox_Field")), Z);
		}
	}

	PlaceSelectionCorner(SelectionMarkerCorners[0], DidNW, 0.f, 0.f, Tint, 1);
	PlaceSelectionCorner(SelectionMarkerCorners[1], DidNE, W - Corner.X, 0.f, Tint, 1);
	PlaceSelectionCorner(SelectionMarkerCorners[2], DidSW, 0.f, H - Corner.Y, Tint, 1);
	PlaceSelectionCorner(SelectionMarkerCorners[3], DidSE, W - Corner.X, H - Corner.Y, Tint, 1);
}

void UACEUIGameplayBinder::EnsureSelectionMarkers()
{
	if (!Canvas || !Canvas->WidgetTree || !Canvas->GetElementLayer())
	{
		return;
	}
	UCanvasPanel* Layer = Canvas->GetElementLayer();
    if(!SelectionDirectionArrow)
    {
        SelectionDirectionArrow=Canvas->WidgetTree->ConstructWidget<UBorder>();
        SelectionDirectionArrow->SetPadding(FMargin(0));SelectionDirectionArrow->SetVisibility(ESlateVisibility::Collapsed);
        Layer->AddChild(SelectionDirectionArrow);
    }
    else if(SelectionDirectionArrow->GetParent()!=Layer) Layer->AddChild(SelectionDirectionArrow);
	if (!SelectionMarkerBox)
	{
		SelectionMarkerBox = Canvas->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		SelectionMarkerBox->SetVisibility(ESlateVisibility::Collapsed);
		Layer->AddChild(SelectionMarkerBox);
	}
	else if (SelectionMarkerBox->GetParent() != Layer)
	{
		Layer->AddChild(SelectionMarkerBox);
	}

	while (SelectionMarkerCorners.Num() < 4)
	{
		UBorder* Corner = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Corner->SetPadding(FMargin(0.f));
		Corner->SetVisibility(ESlateVisibility::Collapsed);
		SelectionMarkerBox->AddChild(Corner);
		SelectionMarkerCorners.Add(Corner);
	}
	for (UBorder* Corner : SelectionMarkerCorners)
	{
		if (Corner && Corner->GetParent() != SelectionMarkerBox)
		{
			SelectionMarkerBox->AddChild(Corner);
		}
	}
}

void UACEUIGameplayBinder::HideSelectionMarkers()
{
	if (SelectionDirectionArrow) SelectionDirectionArrow->SetVisibility(ESlateVisibility::Collapsed);
	if (SelectionMarkerBox)
	{
		SelectionMarkerBox->SetVisibility(ESlateVisibility::Collapsed);
	}
	for (UBorder* Corner : SelectionMarkerCorners)
	{
		if (Corner)
		{
			Corner->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UACEUIGameplayBinder::PlaceSelectionCorner(UBorder* Corner, int32 TexDid, float LocalX, float LocalY,
	const FLinearColor& Tint, int32 ZOrder)
{
	if (!Corner || !SelectionMarkerBox)
	{
		return;
	}
	if (Corner->GetParent() != SelectionMarkerBox)
	{
		SelectionMarkerBox->AddChild(Corner);
	}
	// Resolve DAT corner art (12×12 grayscale). Tint multiplies after brush assign.
	if (Canvas && Canvas->GetResourceResolver())
	{
		if (UTexture2D* Tex = Canvas->GetResourceResolver()->ResolveTexture(static_cast<uint32>(TexDid)))
		{
			FSlateBrush Brush;
			Brush.SetResourceObject(Tex);
			Brush.DrawAs = ESlateBrushDrawType::Image;
			Brush.Tiling = ESlateBrushTileType::NoTile;
			Brush.ImageSize = FVector2D(12.f, 12.f) * Canvas->GetLastScale2D();
			Corner->SetBrush(Brush);
		}
		else
		{
			FSlateBrush Clear;
			Clear.DrawAs = ESlateBrushDrawType::NoDrawType;
			Corner->SetBrush(Clear);
		}
	}
	// Grayscale DAT art is tinted via brush color (retail VividTargetIndicator::SetOnScreenColor).
	Corner->SetBrushColor(Tint);
	Corner->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Corner->Slot))
	{
		Slot->SetAnchors(FAnchors(0.f, 0.f));
		Slot->SetAlignment(FVector2D(0.f, 0.f));
		Slot->SetAutoSize(false);
		Slot->SetPosition(FVector2D(LocalX, LocalY));
		Slot->SetSize(FVector2D(12.f, 12.f) * Canvas->GetLastScale2D());
		Slot->SetZOrder(ZOrder);
	}
}

FLinearColor UACEUIGameplayBinder::ColorFromSelectionMarker(uint8 RadarColor)
{
    return ColorFromRadarBlip(RadarColor);
}

void UACEUIGameplayBinder::RefreshExaminationOverlay()
{
	if (!Manager || !Canvas)
	{
		return;
	}
	TSharedPtr<FACEUIElement> ExamRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
	const bool bShow = ExamRoot.IsValid() && ExamRoot->bVisible;
	if (ExamInscriptionScroll) ExamInscriptionScroll->SetVisibility(ESlateVisibility::Collapsed);
	Manager->SetElementVisibleByName(TEXT("ItemInscriptionScrollbar"),false);
	auto HideExamExtras = [this]()
	{
		for (UTextBlock* Label : ExamSpellLabels) if (Label) Label->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamTitle) { ExamTitle->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamLevel) { ExamLevel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamCreatureDetailsScroll) ExamCreatureDetailsScroll->SetVisibility(ESlateVisibility::Collapsed);
		for (UTextBlock* Text : ExamCreatureHeadings) if (Text) Text->SetVisibility(ESlateVisibility::Collapsed);
		for (UTextBlock* Text : ExamAttributeLabels) if (Text) Text->SetVisibility(ESlateVisibility::Collapsed);
		for (UTextBlock* Text : ExamAttributeValues) if (Text) Text->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamBody) { ExamBody->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamScroll) { ExamScroll->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamValueLabel) { ExamValueLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamBurdenLabel) { ExamBurdenLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamInscriptionLabel) { ExamInscriptionLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamInscriptionSignature) ExamInscriptionSignature->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamInscriptionEditor) ExamInscriptionEditor->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamIconBorder) { ExamIconBorder->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamPaperDollModelImage) { ExamPaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamPaperDollPreviewActor)
			if (auto* Appearance = ExamPaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>())
				Appearance->SetComponentTickEnabled(false);
	};
	if (!bShow)
	{
		HideExamExtras();
		return;
	}
	SyncExamineBodyVisibility(LastAppraisal.bIsCreature);
	UACEUIElementManager::ApplyFloatyResizeLayout(ExamRoot);
	if (ExaminedSpellId) { HideExamExtras(); RefreshSpellExamination(); return; }
	for (UTextBlock* Label : ExamSpellLabels) if (Label) Label->SetVisibility(ESlateVisibility::Collapsed);
	constexpr int32 ExamOverlayZ = 100000;
	PlaceTextOnElement(ExamTitle, TEXT("DisplayedNameText"), ACEAppraisalFormatting::ExaminationName(LastAppraisal), 10, TextGold, ExamOverlayZ);
	if (LastAppraisal.bIsCreature)
	{
		if (ExamInscriptionSignature) ExamInscriptionSignature->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamInscriptionEditor) ExamInscriptionEditor->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamBody) ExamBody->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamScroll) ExamScroll->SetVisibility(ESlateVisibility::Collapsed);
		for (UTextBlock* Text : {ExamValueLabel.Get(), ExamBurdenLabel.Get(), ExamInscriptionLabel.Get()})
			if (Text) Text->SetVisibility(ESlateVisibility::Collapsed);
		if (ExamIconBorder) ExamIconBorder->SetVisibility(ESlateVisibility::Collapsed);
		RefreshCreatureExamination();
		RefreshExamPaperDollPreview();
		return;
	}
	if (ExamLevel) ExamLevel->SetVisibility(ESlateVisibility::Collapsed);
	if (ExamCreatureDetailsScroll) ExamCreatureDetailsScroll->SetVisibility(ESlateVisibility::Collapsed);
	for (UTextBlock* Text : ExamCreatureHeadings) if (Text) Text->SetVisibility(ESlateVisibility::Collapsed);
	for (UTextBlock* Text : ExamAttributeLabels) if (Text) Text->SetVisibility(ESlateVisibility::Collapsed);
	for (UTextBlock* Text : ExamAttributeValues) if (Text) Text->SetVisibility(ESlateVisibility::Collapsed);
	const FACEPlayerVitals Viewer = Client ? Client->GetPlayerVitals() : FACEPlayerVitals();
	const FString Body = ACEAppraisalFormatting::ItemExaminationText(LastAppraisal,
        Canvas->GetResourceResolver() ? Canvas->GetResourceResolver()->GetDatSubsystem() : nullptr,
        !Manager->FindElementUnder(TEXT("ItemExamineUI"), TEXT("ItemValueText")),
        !Manager->FindElementUnder(TEXT("ItemExamineUI"), TEXT("ItemBurdenText")), &Viewer);
	const FString BodyEl = LastAppraisal.bIsCreature
		? TEXT("BasicCreatureExam_Attributes")
		: TEXT("ItemDisplayText");
	TSharedPtr<FACEUIElement> BodyTarget = Manager->FindElementUnder(
		TEXT("RootGameplay_FloatyExamination_Field"), BodyEl);
	if (!BodyTarget.IsValid())
	{
		BodyTarget = Manager->FindElementUnder(
			TEXT("RootGameplay_FloatyExamination_Field"),
			LastAppraisal.bIsCreature ? TEXT("BasicCreatureExamineUI") : TEXT("ItemExamineUI"));
	}
	if (ExamBody && BodyTarget.IsValid())
	{
		// Bind the DAT glyph/style without moving the text out of its scroll box.
		ExamBody->SetText(FText::FromString(Body));
		ExamBody->SetColorAndOpacity(FSlateColor(BodyTarget->TextColor.Get(TextWhite)));
		ExamBody->SetFont(ACEUIFontStyles::MakeSlateFont(ACEUIFontStyles::ResolveForElement(BodyTarget, 9)));
		ExamBody->SetJustification(ETextJustify::Left);
		ExamBody->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (auto* Retail = Cast<UACERetailTextBlock>(ExamBody))
		{
			Retail->SetRetailElement(Canvas->GetResourceResolver(), BodyTarget, Canvas->GetLastScale2D(), BodyTarget->Width, false);
			Retail->SetTextColors(ACEAppraisalFormatting::ItemTextColors(LastAppraisal, Body));
		}
		ExamBody->SetAutoWrapText(true);
		ExamBody->SetClipping(EWidgetClipping::Inherit);
		if (!ExamScroll && Canvas->WidgetTree)
		{
			ExamScroll = Canvas->WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
			ExamScroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
			ExamScroll->SetClipping(EWidgetClipping::ClipToBounds);
			ExamScroll->SetAnimateWheelScrolling(false);
		}
		if (ExamScroll)
		{
			// Spell and item examinations reuse this widget. Never retain a
			// Slate scrollbar beside the retail chain/thumb from an earlier view.
			ExamScroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
			if (ExamBody->GetParent() != ExamScroll) ExamScroll->AddChild(ExamBody);
			if (ExamScrolledObjectGuid != LastAppraisal.ObjectGuid)
			{
				ExamScroll->SetScrollOffset(0.f);
				ExamScrolledObjectGuid = LastAppraisal.ObjectGuid;
			}
			ExamScroll->SetVisibility(ESlateVisibility::Visible);
			Canvas->PlaceWidgetAtElement(ExamScroll, BodyTarget, ExamOverlayZ + 1);
		}
	}

	if (ExamScroll && BodyTarget)
	{
		const float MaxOffset = ExamScroll->GetScrollOffsetOfEnd();
		const auto Bar=Manager->FindElementUnder(TEXT("ItemExamineUI"), TEXT("ItemDisplayTextScrollbar"));
		if (Bar) Bar->bVisible=MaxOffset>.5f;
		SyncDatScrollbar(Bar,
			MaxOffset > 0.f ? ExamScroll->GetScrollOffset() / MaxOffset : 0.f,
			BodyTarget->Height / FMath::Max(1.f, MaxOffset + BodyTarget->Height));
	}

	if (!LastAppraisal.bIsCreature)
	{
		auto EnsureExamText = [&](TObjectPtr<UTextBlock>& Label)
		{
			if (!Label && Canvas->WidgetTree)
			{
				Label = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
				FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
				Label->SetFont(Font);
			}
		};
		EnsureExamText(ExamValueLabel);
		EnsureExamText(ExamBurdenLabel);
		EnsureExamText(ExamInscriptionLabel);
		const FString ValueStr = LastAppraisal.bSuccess
			? FString::Printf(TEXT("Value: %d"), LastAppraisal.Value) : FString();
		const FString BurdenStr = LastAppraisal.bSuccess
			? FString::Printf(TEXT("Burden: %d"), LastAppraisal.Burden) : FString();
		if (!ValueStr.IsEmpty())
		{
			PlaceTextUnder(ExamValueLabel, TEXT("RootGameplay_FloatyExamination_Field"),
				TEXT("ItemValueText"), ValueStr, 9, TextWhite, ExamOverlayZ + 2);
		}
		else if (ExamValueLabel)
		{
			ExamValueLabel->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (!BurdenStr.IsEmpty())
		{
			PlaceTextUnder(ExamBurdenLabel, TEXT("RootGameplay_FloatyExamination_Field"),
				TEXT("ItemBurdenText"), BurdenStr, 9, TextWhite, ExamOverlayZ + 3);
		}
		else if (ExamBurdenLabel)
		{
			ExamBurdenLabel->SetVisibility(ESlateVisibility::Collapsed);
		}
        const auto InscriptionElement=Manager->FindElementUnder(TEXT("RootGameplay_FloatyExamination_Field"),TEXT("ItemInscriptionText"));
        FACEWorldObject InspectedObject;
        const bool bInscribable=LastAppraisal.BoolProperties.FindRef(22)
            || (Client->GetWorldObject(LastAppraisal.ObjectGuid,InspectedObject) && (InspectedObject.ObjectDescriptionFlags & 0x2));
        if (InscriptionElement && (bInscribable || !LastAppraisal.Inscription.IsEmpty()))
        {
            // ItemExamineUI::SetInscription selects the DAT uninscribed state.
            const bool bPrompt=LastAppraisal.Inscription.IsEmpty();
            InscriptionElement->DefaultState=bPrompt ? 0x10000050u : 1u;
            InscriptionElement->bUseExplicitState=true;
            InscriptionElement->TextHorizontalJustification=bPrompt ? 1 : 2;
            InscriptionElement->TextVerticalJustification=bPrompt ? 1 : 4;
            InscriptionElement->ResolvePaintState(false,false,false);
            if(bPrompt)
                PlaceTextOnElement(ExamInscriptionLabel,InscriptionElement,TEXT("<Inscribe here>"),8,FLinearColor::Black,ExamOverlayZ+4);
            else
            {
                if(!ExamInscriptionScroll)
                {
                    ExamInscriptionScroll=Canvas->WidgetTree->ConstructWidget<UScrollBox>();
                    ExamInscriptionScroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
                    ExamInscriptionScroll->SetClipping(EWidgetClipping::ClipToBounds);
                    ExamInscriptionScroll->SetAnimateWheelScrolling(false);
                }
                ExamInscriptionLabel->SetText(FText::FromString(LastAppraisal.Inscription));
                ExamInscriptionLabel->SetJustification(ETextJustify::Left);
                ExamInscriptionLabel->SetAutoWrapText(true);
                ExamInscriptionLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
                if(auto* Retail=Cast<UACERetailTextBlock>(ExamInscriptionLabel))
                    Retail->SetRetailElement(Canvas->GetResourceResolver(),InscriptionElement,Canvas->GetLastScale2D(),InscriptionElement->Width,false);
                if(ExamInscriptionLabel->GetParent()!=ExamInscriptionScroll) ExamInscriptionScroll->AddChild(ExamInscriptionLabel);
                if(ExamInscriptionScrolledGuid!=LastAppraisal.ObjectGuid)
                {ExamInscriptionScrolledGuid=LastAppraisal.ObjectGuid;ExamInscriptionScroll->SetScrollOffset(0);}
                Canvas->PlaceWidgetAtElement(ExamInscriptionScroll,InscriptionElement,ExamOverlayZ+4);
                const bool Editing=EditingInscriptionGuid==LastAppraisal.ObjectGuid;
                ExamInscriptionScroll->SetVisibility(Editing?ESlateVisibility::Collapsed:ESlateVisibility::Visible);
                const auto Bar=Manager->FindElementUnder(TEXT("ItemExamineUI"),TEXT("ItemInscriptionScrollbar"));
                const float Max=ExamInscriptionScroll->GetScrollOffsetOfEnd();
                if(Bar)Bar->bVisible=!Editing && Max>.5f;
                SyncDatScrollbar(Bar,Max>0?ExamInscriptionScroll->GetScrollOffset()/Max:0,
                    InscriptionElement->Height/FMath::Max(1.f,Max+InscriptionElement->Height));
            }
            ExamInscriptionLabel->SetColorAndOpacity(FLinearColor::Black);
			EnsureExamText(ExamInscriptionSignature);
			const FString Scribe = LastAppraisal.StringProperties.FindRef(8);
			PlaceTextUnder(ExamInscriptionSignature, TEXT("RootGameplay_FloatyExamination_Field"),
				TEXT("ItemInscriptionSignatureText"), bPrompt || Scribe.IsEmpty() ? FString() : TEXT("--")+Scribe,
				8, FLinearColor::Black, ExamOverlayZ+4);
			ExamInscriptionSignature->SetJustification(ETextJustify::Right);
			if (EditingInscriptionGuid == LastAppraisal.ObjectGuid && ExamInscriptionEditor)
			{
				ExamInscriptionLabel->SetVisibility(ESlateVisibility::Collapsed);
				Canvas->PlaceWidgetAtElement(ExamInscriptionEditor, InscriptionElement, ExamOverlayZ+5, FMargin(2));
				ExamInscriptionEditor->SetVisibility(ESlateVisibility::Visible);
			}
        }
        else
		{
			if (ExamInscriptionLabel) ExamInscriptionLabel->SetVisibility(ESlateVisibility::Collapsed);
			if (ExamInscriptionSignature) ExamInscriptionSignature->SetVisibility(ESlateVisibility::Collapsed);
			if (ExamInscriptionEditor) ExamInscriptionEditor->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (LastAppraisal.IconDid != 0)
		{
			if (!ExamIconBorder && Canvas->WidgetTree)
			{
				ExamIconBorder = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
				ExamIconBorder->SetPadding(FMargin(0.f));
				ExamIconBorder->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
			if (ExamIconBorder)
			{
				SetIconDid(ExamIconBorder, LastAppraisal.IconDid);
				if (TSharedPtr<FACEUIElement> IconEl = Manager->FindElementUnder(
						TEXT("RootGameplay_FloatyExamination_Field"), TEXT("ItemIcon")))
				{
					ExamIconBorder->SetVisibility(ESlateVisibility::HitTestInvisible);
					Canvas->PlaceWidgetAtElement(ExamIconBorder, IconEl, ExamOverlayZ + 5, FMargin(0.f));
					if (IconEl->ImageFileId != static_cast<uint32>(LastAppraisal.IconDid))
					{
						IconEl->ImageFileId = static_cast<uint32>(LastAppraisal.IconDid);
					}
				}
			}
		}
		else if (ExamIconBorder)
		{
			ExamIconBorder->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	else
	{
		if (ExamValueLabel) { ExamValueLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamBurdenLabel) { ExamBurdenLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamInscriptionLabel) { ExamInscriptionLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamIconBorder) { ExamIconBorder->SetVisibility(ESlateVisibility::Collapsed); }
	}
	RefreshExamPaperDollPreview();
}

void UACEUIGameplayBinder::RefreshPanelBodyText()
{
	if (CharacterInfoScroll && ActivePanelPage != TEXT("CharacterInfoPanel_Field")) CharacterInfoScroll->SetVisibility(ESlateVisibility::Collapsed);
	if (CharacterInfoTitle && ActivePanelPage != TEXT("CharacterInfoPanel_Field")) CharacterInfoTitle->SetVisibility(ESlateVisibility::Collapsed);
	if (!PanelBodyText || !Manager || !Canvas || ActivePanelPage.IsEmpty())
	{
		if (PanelBodyText) { PanelBodyText->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	if (ActivePanelPage == TEXT("InventoryPanel_Field")
		|| ActivePanelPage == TEXT("SkillManagementPanel_Field")
		|| ActivePanelPage == TEXT("SpellManagementPanel_Field")
		|| ActivePanelPage == TEXT("OptionsPanel_Field")
		|| ActivePanelPage == TEXT("PositiveEffectsPanel_Field")
		|| ActivePanelPage == TEXT("NegativeEffectsPanel_Field")
		|| ActivePanelPage == TEXT("VitaePanel_Field")
		|| ActivePanelPage == TEXT("LinkStatusPanel_Field")
		|| ActivePanelPage == TEXT("SocialPanel_Field")
		|| ActivePanelPage == TEXT("QuestManagementPanel_Field")
		|| ActivePanelPage == TEXT("WorldPanel_Field")
		|| ActivePanelPage == TEXT("AbusePanel_Field")
		|| ActivePanelPage == TEXT("UrgentAssistancePanel_Field")
		|| ActivePanelPage == TEXT("BookPanel_Field")
		|| ActivePanelPage == TEXT("MiniGamePanel_Field"))
	{
		// These pages use DAT chrome + dedicated overlays — no full-page text slab.
		PanelBodyText->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	TSharedPtr<FACEUIElement> Page = Manager->FindElementByName(ActivePanelPage);
	if (!Page.IsValid() || !Page->bVisible)
	{
		PanelBodyText->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	FString Body;
	if (ActivePanelPage == TEXT("CharacterInfoPanel_Field"))
	{
		Body = BuildCharacterInformation();
	}
	else
	{
		Body = ActivePanelPage;
	}
	PanelBodyText->SetVisibility(ESlateVisibility::HitTestInvisible);
	PanelBodyText->SetText(FText::FromString(Body));
	// CharacterInfo has an authored text region (CharacterInfoText, 8px inset under
	// the 25px title bar) — bind to it when present instead of the whole page.
	TSharedPtr<FACEUIElement> TextEl = ActivePanelPage == TEXT("CharacterInfoPanel_Field")
		? Manager->FindElementUnder(ActivePanelPage, TEXT("CharacterInfoText"))
		: nullptr;
	if (TextEl.IsValid())
	{
		if (const auto Background = Manager->FindElementUnder(ActivePanelPage,TEXT("CharacterInfoBackground")))
		{
			Background->Width=Page->Width;
			Background->Height=FMath::Max(1,Page->Height-Background->Y);
			TextEl->Width=FMath::Max(1,Page->Width-38);
			TextEl->Height=Background->Height;
			if (const auto Bar=Manager->FindElementUnder(ActivePanelPage,TEXT("CharacterInfoText_Scrollbar")))
			{
				Bar->X=Page->Width-20;
				Bar->Height=Background->Height;
			}
		}
		if (!CharacterInfoTitle) CharacterInfoTitle=Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		PlaceTextOnElement(CharacterInfoTitle,Manager->FindElementUnder(ActivePanelPage,TEXT("TitleText")),
			TEXT("Character Information"),10,TextWhite,551,true);
		PanelBodyText->SetJustification(ETextJustify::Center);
		if (auto* Retail = Cast<UACERetailTextBlock>(PanelBodyText))
			Retail->SetRetailElement(Canvas->GetResourceResolver(),TextEl,Canvas->GetLastScale2D(),TextEl->Width,false);
		if (!CharacterInfoScroll)
		{
			CharacterInfoScroll=Canvas->WidgetTree->ConstructWidget<UScrollBox>();
			CharacterInfoScroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
			CharacterInfoScroll->SetClipping(EWidgetClipping::ClipToBounds);
			CharacterInfoScroll->SetAnimateWheelScrolling(false);
		}
		if (PanelBodyText->GetParent()!=CharacterInfoScroll) CharacterInfoScroll->AddChild(PanelBodyText);
		CharacterInfoScroll->SetVisibility(ESlateVisibility::Visible);
		Canvas->PlaceWidgetAtElement(CharacterInfoScroll,TextEl,550);
		const float Max=CharacterInfoScroll->GetScrollOffsetOfEnd();
		SyncDatScrollbar(Manager->FindElementUnder(ActivePanelPage,TEXT("CharacterInfoText_Scrollbar")),
			Max>0 ? CharacterInfoScroll->GetScrollOffset()/Max : 0, TextEl->Height/FMath::Max(1.f,Max+TextEl->Height));
	}
	else
	{
		Canvas->PlaceWidgetAtElement(PanelBodyText, Page, 550, FMargin(10.f, 28.f, 10.f, 10.f));
	}
}

UBorder* UACEUIGameplayBinder::EnsureIconBorder(TArray<TObjectPtr<UBorder>>& Array, int32 Index)
{
	if (!Canvas || !Canvas->WidgetTree || Index < 0)
	{
		return nullptr;
	}
	while (Array.Num() <= Index)
	{
		Array.Add(nullptr);
	}
	if (!IsValid(Array[Index]))
	{
		UBorder* B = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		B->SetPadding(FMargin(0.f));
		// HitTestInvisible so clicks reach the canvas → manager / TryHandleOverlayClick.
		B->SetVisibility(ESlateVisibility::HitTestInvisible);
		FSlateBrush Clear;
		Clear.DrawAs = ESlateBrushDrawType::NoDrawType;
		B->SetBrush(Clear);
		B->SetBrushColor(FLinearColor::Transparent);
		Array[Index] = B;
	}
	return Array[Index];
}

void UACEUIGameplayBinder::SetIconDid(UBorder* Border, int32 IconDid, FLinearColor Tint)
{
	if (!IsValid(Border))
	{
		return;
	}
	FAceIconPaintCache& Cached = IconPaintCache.FindOrAdd(Border);
	constexpr int32 IconStyleRev = 4;
	const bool bWantClear = (IconDid == 0);
	if (!bWantClear && Cached.Did == IconDid && Cached.Tint.Equals(Tint, 0.002f)
		&& !Cached.bCleared && Cached.Style == IconStyleRev)
	{
		return;
	}
	if (bWantClear && Cached.bCleared && Cached.Style == IconStyleRev)
	{
		return;
	}
	if (IconDid != 0 && Canvas && Canvas->GetResourceResolver())
	{
		if (UTexture2D* Tex = Canvas->GetResourceResolver()->ResolveIconTexture(static_cast<uint32>(IconDid)))
		{
			FSlateBrush Brush;
			Brush.SetResourceObject(Tex);
			// Image honors texture alpha so parchment slot chrome shows through cutouts.
			// ImageSize = cell size stretches the 32×32 DAT icon (letterboxing was a white frame).
			Brush.DrawAs = ESlateBrushDrawType::Image;
			Brush.Tiling = ESlateBrushTileType::NoTile;
			FVector2D ImageSize(
				static_cast<float>(FMath::Max(1, Tex->GetSizeX())),
				static_cast<float>(FMath::Max(1, Tex->GetSizeY())));
			if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Border->Slot))
			{
				const FVector2D Sz = Slot->GetSize();
				if (Sz.X >= 1.f && Sz.Y >= 1.f)
				{
					ImageSize = Sz;
				}
			}
			Brush.ImageSize = ImageSize;
			Border->SetBrush(Brush);
			Border->SetBrushColor(Tint);
			Border->SetPadding(FMargin(0.f));
			Cached.Did = IconDid;
			Cached.Tint = Tint;
			Cached.bCleared = false;
			Cached.Style = IconStyleRev;
			return;
		}
	}
	// Retail empty slots are open paper — no dark placeholder slab.
	FSlateBrush Clear;
	Clear.DrawAs = ESlateBrushDrawType::NoDrawType;
	Border->SetBrush(Clear);
	Border->SetBrushColor(FLinearColor::Transparent);
	Cached.Did = 0;
	Cached.Tint = FLinearColor::Transparent;
	Cached.bCleared = true;
	Cached.Style = IconStyleRev;
}

void UACEUIGameplayBinder::SetSpellIcon(UBorder* Border, int32 SpellId)
{
	if (!Border || !Canvas || !Canvas->GetResourceResolver()) return;
	UTexture2D* Texture = Canvas->GetResourceResolver()->ResolveSpellIcon(SpellId);
	if (Texture && Border->Background.GetResourceObject() == Texture) return;
	FSlateBrush Brush; Brush.SetResourceObject(Texture); Brush.ImageSize = FVector2D(32, 32);
	Brush.DrawAs = Texture ? ESlateBrushDrawType::Image : ESlateBrushDrawType::NoDrawType;
	Border->SetBrush(Brush); Border->SetBrushColor(FLinearColor::White); Border->SetPadding(FMargin(0));
	IconPaintCache.Remove(Border);
}

void UACEUIGameplayBinder::SetItemSlotBackground(UBorder* Border, const FACEWorldObject* Item)
{
    if (!Border || !Canvas || !Canvas->GetResourceResolver()) return;
    if (!Item) { SetIconDid(Border, DidInvSlotBg); return; }
    UTexture2D* Texture = Canvas->GetResourceResolver()->ResolveItemBackground(Item->ItemType, Item->IconUnderlayId);
    if (Border->Background.GetResourceObject() == Texture && Texture) return;
    FSlateBrush Brush;
    Brush.DrawAs = Texture ? ESlateBrushDrawType::Image : ESlateBrushDrawType::NoDrawType;
    Brush.SetResourceObject(Texture); Brush.ImageSize = FVector2D(32,32);
    Border->SetBrush(Brush); Border->SetBrushColor(FLinearColor::White);
    IconPaintCache.Remove(Border);
}

void UACEUIGameplayBinder::SetItemSlotForeground(UBorder* Border, const FACEWorldObject* Item)
{
    if (!Border || !Canvas || !Canvas->GetResourceResolver()) return;
    if (!Item) { SetIconDid(Border,0); return; }
    // Retail marks salvage offers with the same trade-state overlay as sale offers.
    const bool bOffered = SalvageQueueGuids.Contains(Item->Guid)
        || (Client && Client->GetTradeSelfItems().Contains(Item->Guid))
        || (OpenVendorGuid != 0 && VendorSellCart.ContainsByPredicate(
            [Item](const TPair<int32,int32>& Entry) { return Entry.Value == Item->Guid; }));
    UTexture2D* Texture = Canvas->GetResourceResolver()->ResolveItemForeground(Item->IconId,Item->IconOverlayId,Item->UiEffects,bOffered,
        Item->Structure,Item->MaxStructure);
    const float Flash = LastSelection.bValid && Item->Guid == LastSelection.Guid
        ? FMath::Clamp(float((SelectionFlashUntil - FPlatformTime::Seconds()) / .4),0.f,1.f) : 0.f;
    Border->SetBrushColor(FLinearColor(1.f+Flash,1.f+Flash,1.f+Flash,1));
    if (Border->Background.GetResourceObject() == Texture && Texture) return;
    FSlateBrush Brush;
    Brush.DrawAs = Texture ? ESlateBrushDrawType::Image : ESlateBrushDrawType::NoDrawType;
    Brush.SetResourceObject(Texture); Brush.ImageSize = FVector2D(32,32);
    Border->SetBrush(Brush); Border->SetBrushColor(FLinearColor::White);
    IconPaintCache.Remove(Border);
}

bool UACEUIGameplayBinder::EnsurePaperDollPreviewRig()
{
	// Binder is a plain UObject — take the world from the widget / controller that owns the HUD.
	UWorld* World = Canvas ? Canvas->GetWorld() : nullptr;
	if (!World && PlayerController)
	{
		World = PlayerController->GetWorld();
	}
	if (!World)
	{
		World = GetWorld();
	}
	if (!World)
	{
		return false;
	}
	// LayoutDesc PaperDoll is 224x214; render at 2x so the doll stays crisp on large viewports.
	constexpr int32 RtWidth = 448;
	constexpr int32 RtHeight = 428;
	if (!PaperDollRenderTarget)
	{
		PaperDollRenderTarget = NewObject<UTextureRenderTarget2D>(this);
		PaperDollRenderTarget->ClearColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
        PaperDollRenderTarget->InitCustomFormat(RtWidth, RtHeight, PF_FloatRGBA, true);
		PaperDollRenderTarget->UpdateResourceImmediate(true);
	}
	if (!PaperDollPreviewActor)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		// Far off the playfield so the rig's own light can't spill onto the world.
		PaperDollPreviewActor = World->SpawnActor<AActor>(AActor::StaticClass(),
			FVector(-50000.f, -46000.f, -50000.f), FRotator::ZeroRotator, Params);
		if (!PaperDollPreviewActor)
		{
			return false;
		}
		// The actor must stay game-visible: SetActorHiddenInGame also hides it from captures.
		// Parts are flagged VisibleInSceneCaptureOnly in RefreshPaperDollPreview instead.
		USceneComponent* Root = NewObject<USceneComponent>(PaperDollPreviewActor, TEXT("PaperDollRoot"));
		PaperDollPreviewActor->SetRootComponent(Root);
		Root->RegisterComponent();
		PaperDollPreviewActor->SetActorLocation(FVector(-50000.f,-46000.f,-50000.f));
		UACECharacterAppearanceComponent* Appearance = NewObject<UACECharacterAppearanceComponent>(
			PaperDollPreviewActor, TEXT("PaperDollAppearance"));
		Appearance->RegisterComponent();
		UPointLightComponent* Light = NewObject<UPointLightComponent>(PaperDollPreviewActor, TEXT("PaperDollLight"));
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetIntensity(3000.f);
		Light->SetAttenuationRadius(1200.f);
		Light->SetCastShadows(false);
		Light->SetRelativeLocation(FVector(-150.f, -80.f, 150.f));
		PaperDollCapture = NewObject<USceneCaptureComponent2D>(PaperDollPreviewActor, TEXT("PaperDollCapture"));
		PaperDollCapture->SetupAttachment(Root);
		PaperDollCapture->RegisterComponent();
		PaperDollCapture->SetAbsolute(true, true, true);
		PaperDollCapture->TextureTarget = PaperDollRenderTarget;
		PaperDollCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
		PaperDollCapture->bCaptureEveryFrame = false;
		PaperDollCapture->bCaptureOnMovement = false;
		PaperDollCapture->ShowFlags.SetAtmosphere(false);
		PaperDollCapture->ShowFlags.SetFog(false);
		PaperDollCapture->ShowFlags.SetMotionBlur(false);
		PaperDollCapture->ShowFlags.SetAntiAliasing(false);
		PaperDollCapture->ShowFlags.SetEyeAdaptation(false);
		PaperDollCapture->ShowFlags.SetBloom(false);
		PaperDollCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		PaperDollCapture->FOVAngle = ACERetailPaperDoll::HorizontalFov(224.f / 214.f);
	}
	return PaperDollPreviewActor != nullptr && PaperDollCapture != nullptr;
}

void UACEUIGameplayBinder::RefreshPaperDollPreview()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree || !Client)
	{
		return;
	}
	TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("PaperDoll"));
	if (!El.IsValid())
	{
		El = Manager->FindElementByName(TEXT("PaperDoll"));
	}
	const int32 Self = Client->GetPlayerGuid();
	FACEWorldObject Obj;
	if (!El.IsValid() || Self == 0 || !Client->GetWorldObject(Self, Obj) || Obj.SetupId == 0
		|| !EnsurePaperDollPreviewRig())
	{
		if (PaperDollModelImage) { PaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	if (!PaperDollModelImage)
	{
		PaperDollModelImage = Canvas->WidgetTree->ConstructWidget<UACECaptureImage>(UACECaptureImage::StaticClass());
	}
	if (!PaperDollModelImage)
	{
		return;
	}
	if (PaperDollModelImage->GetBrush().GetResourceObject() != PaperDollRenderTarget)
	{
		FSlateBrush Brush;
		Brush.SetResourceObject(PaperDollRenderTarget);
		Brush.ImageSize = FVector2D(El->Width, El->Height);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		PaperDollModelImage->SetBrush(Brush);
	}
	PaperDollModelImage->SetVisibility(ESlateVisibility::HitTestInvisible);
	// Show Equipment ON = slot chrome view (hide 3D). OFF = 3D model view.
	if (bShowPaperdollSlots)
	{
		PaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed);
	}
	else
	{
		// Below the equipment icon overlays (OverlayZ) so slot icons still read on top when shown.
		Canvas->PlaceWidgetAtElement(PaperDollModelImage, El, 120000 - 3);
	}

	// Clothing fingerprint only — jewelry/weapon guid churn recooked the paperdoll on every
	// wield even when ObjDesc was unchanged (multi-hundred-ms hitch).
	uint32 Hash = HashCombine(GetTypeHash(Obj.SetupId), GetTypeHash(Obj.Appearance.GetContentHash()));
	Hash = HashCombine(Hash, GetTypeHash(LastVitals.HeritageGroup));
	Hash = HashCombine(Hash, GetTypeHash(Obj.Scale));
	// Retail turns the inventory creature independently of its attached camera.
	if (PaperDollPreviewActor)
	{
		PaperDollPreviewActor->SetActorRotation(FRotator::ZeroRotator);
		if (UACECharacterAppearanceComponent* PreviewApp =
			PaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			PreviewApp->MeshFacingYawDegrees = ACERetailPaperDoll::HeadingDegrees;
		}
	}
	if (PaperDollAppliedGuid == Self && PaperDollAppliedHash == Hash)
	{
		// This is a fixed authored pose. Changes rebuild it; selection flash
		// explicitly captures its changed materials. Reuse the completed texture.
		return;
	}
	UACECharacterAppearanceComponent* Appearance =
		PaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>();
	if (!Appearance)
	{
		return;
	}
	FACEWorldObject Preview = Obj;
	Preview.bIsSelf = false; // never treat the rig as the hidden local player
	Appearance->MeshFacingYawDegrees = ACERetailPaperDoll::HeadingDegrees;
	Appearance->ClearAppearance();
	if (!Appearance->ApplyWorldObject(Preview, 100.f, false))
	{
		return;
	}
	Appearance->SetPreviewAnimation(Canvas->GetResourceResolver()->ResolvePaperDollAnimation(LastVitals.HeritageGroup), false, 1);
	PaperDollPreviewActor->SetActorRotation(FRotator::ZeroRotator);
	PaperDollCapture->ClearShowOnlyComponents();
	TArray<UPrimitiveComponent*> Prims;
	PaperDollPreviewActor->GetComponents<UPrimitiveComponent>(Prims);
	for (UPrimitiveComponent* Prim : Prims)
	{
		if (!Prim || Prim->ComponentHasTag(TEXT("ACECollisionOnly")) || Prim->IsA<UCapsuleComponent>() || Prim->IsA<UTextRenderComponent>())
		{
			if (Prim)
			{
				Prim->SetVisibility(false);
				Prim->SetHiddenInGame(true);
			}
			continue;
		}
		Prim->SetHiddenInGame(false);
		Prim->SetVisibility(true, false);
		Prim->SetVisibleInSceneCaptureOnly(true);
		PaperDollCapture->ShowOnlyComponent(Prim);
	}
	PaperDollCapture->FOVAngle = ACERetailPaperDoll::HorizontalFov(224.f / 214.f);
	PaperDollCapture->SetWorldLocation(PaperDollPreviewActor->GetActorLocation()
		+ ACERetailPaperDoll::CameraPositionCm(LastVitals.HeritageGroup));
	PaperDollCapture->SetWorldRotation(FRotator(0.f, 90.f, 0.f));
	PaperDollAppliedGuid = Self;
	PaperDollAppliedHash = Hash;
	if (PaperDollCapture)
	{
		PaperDollCapture->CaptureScene();
	}
}

void UACEUIGameplayBinder::ReleasePaperDollPreview()
{
	PaperDollAppliedGuid = 0;
	PaperDollAppliedHash = 0;
	if (PaperDollModelImage)
	{
		PaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (PaperDollPreviewActor)
	{
		PaperDollPreviewActor->Destroy();
		PaperDollPreviewActor = nullptr;
	}
	PaperDollCapture = nullptr;
	PaperDollRenderTarget = nullptr;
}

bool UACEUIGameplayBinder::ApplyPreviewCaptureAppearance(AActor* PreviewActor, USceneCaptureComponent2D* Capture,
	const FACEWorldObject& Obj, float InWorldScale)
{
	if (!PreviewActor || !Capture)
	{
		return false;
	}
	UACECharacterAppearanceComponent* Appearance =
		PreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>();
	if (!Appearance)
	{
		return false;
	}
	FACEWorldObject Preview = Obj;
	Preview.bIsSelf = false;
	Appearance->SetPreviewCapture(true);
	Appearance->MeshFacingYawDegrees = ACERetailPaperDoll::HeadingDegrees;
	Appearance->ClearAppearance();
	if (!Appearance->ApplyWorldObject(Preview, InWorldScale, false))
	{
		return false;
	}
	Appearance->SetLocomotionInput(0.f, 0.f, false, 1.f);
	PreviewActor->SetActorRotation(FRotator::ZeroRotator);
	// Setup placements are not the creature's Ready pose (notably Undead heads).
	// Fit after posing, and keep UI captures out of world-distance throttling.
	Appearance->TickComponent(0.f, LEVELTICK_All, nullptr);
	Capture->ClearShowOnlyComponents();
	TArray<UPrimitiveComponent*> Prims;
	PreviewActor->GetComponents<UPrimitiveComponent>(Prims);
	for (UPrimitiveComponent* Prim : Prims)
	{
		// Reusing the rig after a creature with fewer/null parts leaves those
		// components hidden in game. ApplySetupParts restores section visibility,
		// so clear the previous capture's hide flag before testing IsVisible().
		if (Prim && Prim->IsA<UMeshComponent>() && Prim->IsAttachedTo(Appearance->GetMeshRoot()))
			Prim->SetHiddenInGame(false);
		// The capture owns an editor camera visualization mesh too. Only the
		// appearance's visible DAT parts belong in the portrait or its bounds.
		if (!Prim || !Prim->IsA<UMeshComponent>() || !Prim->IsAttachedTo(Appearance->GetMeshRoot()) || !Prim->IsVisible())
		{
			if (Prim)
			{
				Prim->SetVisibility(false);
				Prim->SetHiddenInGame(true);
			}
			continue;
		}
		Prim->SetHiddenInGame(false);
		Prim->SetVisibility(true, false);
		Prim->SetVisibleInSceneCaptureOnly(true);
		Capture->ShowOnlyComponent(Prim);
	}
	// BasicCreatureExamineUI::Init fits the rotated physics object's bounding
	// box using a 45-degree vertical field of view. It is not the inventory's
	// fixed heritage camera: long, wide and short creatures must fit as well.
	FBox Bounds(ForceInit);
	for (UPrimitiveComponent* Prim : Prims)
	{
		if (Prim && Prim->IsA<UMeshComponent>() && Prim->IsAttachedTo(Appearance->GetMeshRoot()) && Prim->IsVisible())
		{
			Prim->UpdateBounds();
			Bounds += Prim->Bounds.GetBox();
		}
	}
	if (!Bounds.IsValid) return false;
	const float Aspect = Capture->TextureTarget ? float(Capture->TextureTarget->SizeX) / Capture->TextureTarget->SizeY : 300.f / 265.f;
	const FVector Size = Bounds.GetSize();
	const float Distance = FMath::Max(Size.Z, Size.X / Aspect) * 1.2071068f + Size.Y * .5f;
	Capture->FOVAngle = ACERetailPaperDoll::HorizontalFov(Aspect);
	Capture->SetWorldLocation(Bounds.GetCenter() - FVector(0.f, FMath::Max(1.f, Distance), 0.f));
	Capture->SetWorldRotation(FRotator(0.f, 90.f, 0.f));
	Capture->CaptureScene();
	return true;
}

bool UACEUIGameplayBinder::EnsureExamPaperDollPreviewRig()
{
	UWorld* World = Canvas ? Canvas->GetWorld() : nullptr;
	if (!World && PlayerController)
	{
		World = PlayerController->GetWorld();
	}
	if (!World)
	{
		World = GetWorld();
	}
	if (!World)
	{
		return false;
	}
	constexpr int32 RtWidth = 600;
	constexpr int32 RtHeight = 530;
	if (!ExamPaperDollRenderTarget)
	{
		ExamPaperDollRenderTarget = NewObject<UTextureRenderTarget2D>(this);
		ExamPaperDollRenderTarget->ClearColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
		ExamPaperDollRenderTarget->InitCustomFormat(RtWidth, RtHeight, PF_FloatRGBA, true);
		ExamPaperDollRenderTarget->UpdateResourceImmediate(true);
	}
	if (!ExamPaperDollPreviewActor)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		ExamPaperDollPreviewActor = World->SpawnActor<AActor>(AActor::StaticClass(),
			FVector(-50000.f, -48000.f, -50000.f), FRotator::ZeroRotator, Params);
		if (!ExamPaperDollPreviewActor)
		{
			return false;
		}
		USceneComponent* Root = NewObject<USceneComponent>(ExamPaperDollPreviewActor, TEXT("ExamPaperDollRoot"));
		ExamPaperDollPreviewActor->SetRootComponent(Root);
		Root->RegisterComponent();
		ExamPaperDollPreviewActor->SetActorLocation(FVector(-50000.f, -48000.f, -50000.f));
		UACECharacterAppearanceComponent* Appearance = NewObject<UACECharacterAppearanceComponent>(
			ExamPaperDollPreviewActor, TEXT("ExamPaperDollAppearance"));
		Appearance->RegisterComponent();
		UPointLightComponent* Light = NewObject<UPointLightComponent>(ExamPaperDollPreviewActor, TEXT("ExamPaperDollLight"));
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetIntensity(3000.f);
		Light->SetAttenuationRadius(1200.f);
		Light->SetCastShadows(false);
		Light->SetRelativeLocation(FVector(-150.f, -80.f, 150.f));
		ExamPaperDollCapture = NewObject<USceneCaptureComponent2D>(ExamPaperDollPreviewActor, TEXT("ExamPaperDollCapture"));
		ExamPaperDollCapture->SetupAttachment(Root);
		ExamPaperDollCapture->RegisterComponent();
		ExamPaperDollCapture->SetAbsolute(true, true, true);
		ExamPaperDollCapture->TextureTarget = ExamPaperDollRenderTarget;
		ExamPaperDollCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
		ExamPaperDollCapture->bCaptureEveryFrame = false;
		ExamPaperDollCapture->bCaptureOnMovement = false;
		ExamPaperDollCapture->ShowFlags.SetAtmosphere(false);
		ExamPaperDollCapture->ShowFlags.SetFog(false);
		ExamPaperDollCapture->ShowFlags.SetMotionBlur(false);
		ExamPaperDollCapture->ShowFlags.SetAntiAliasing(false);
		ExamPaperDollCapture->ShowFlags.SetEyeAdaptation(false);
		ExamPaperDollCapture->ShowFlags.SetBloom(false);
		ExamPaperDollCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		ExamPaperDollCapture->FOVAngle = 42.f;
	}
	return ExamPaperDollPreviewActor != nullptr && ExamPaperDollCapture != nullptr;
}

void UACEUIGameplayBinder::RefreshExamPaperDollPreview()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree || !Client)
	{
		if (ExamPaperDollModelImage) { ExamPaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	TSharedPtr<FACEUIElement> ExamRoot = Manager->FindElementByName(TEXT("RootGameplay_FloatyExamination_Field"));
	TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(TEXT("RootGameplay_FloatyExamination_Field"), TEXT("Exam_PaperDoll"));
	if (!El.IsValid())
	{
		El = Manager->FindElementByName(TEXT("Exam_PaperDoll"));
	}
	FACEWorldObject Obj;
	const bool bHaveObj = LastAppraisal.bIsCreature && LastAppraisal.ObjectGuid != 0
		&& Client->GetWorldObject(LastAppraisal.ObjectGuid, Obj) && Obj.SetupId != 0;
	if (!ExamRoot.IsValid() || !ExamRoot->bVisible || !El.IsValid() || !El->bVisible || !bHaveObj
		|| !EnsureExamPaperDollPreviewRig())
	{
		if (ExamPaperDollModelImage) { ExamPaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed); }
		if (ExamPaperDollPreviewActor)
			if (auto* Appearance = ExamPaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>())
				Appearance->SetComponentTickEnabled(false);
		return;
	}
	if (auto* Appearance = ExamPaperDollPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>())
		Appearance->SetComponentTickEnabled(true);
	if (!ExamPaperDollModelImage)
	{
		ExamPaperDollModelImage = Canvas->WidgetTree->ConstructWidget<UACECaptureImage>(UACECaptureImage::StaticClass());
	}
	if (!ExamPaperDollModelImage)
	{
		return;
	}
	if (ExamPaperDollModelImage->GetBrush().GetResourceObject() != ExamPaperDollRenderTarget)
	{
		FSlateBrush Brush;
		Brush.SetResourceObject(ExamPaperDollRenderTarget);
		Brush.ImageSize = FVector2D(El->Width, El->Height);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		ExamPaperDollModelImage->SetBrush(Brush);
	}
	ExamPaperDollModelImage->SetVisibility(ESlateVisibility::HitTestInvisible);
	Canvas->PlaceWidgetAtElement(ExamPaperDollModelImage, El, 100000 - 2);

	uint32 Hash = HashCombine(GetTypeHash(Obj.SetupId), GetTypeHash(Obj.Appearance.GetContentHash()));
	Hash = HashCombine(Hash, GetTypeHash(Obj.Scale));
	Hash = HashCombine(Hash, GetTypeHash(FIntPoint(El->Width, El->Height)));
	if (ExamPaperDollAppliedGuid == LastAppraisal.ObjectGuid && ExamPaperDollAppliedHash == Hash)
	{
		if (ExamPaperDollCapture)
		{
			static double LastExamPaperDollCapSec = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - LastExamPaperDollCapSec >= 0.125)
			{
				LastExamPaperDollCapSec = Now;
				ExamPaperDollCapture->CaptureScene();
			}
		}
		return;
	}
	const float Scale = PlayerController ? PlayerController->WorldScale : 100.f;
	const float ResolutionScale = FMath::Min(2.f, 2048.f / FMath::Max(El->Width, El->Height));
	const FIntPoint CaptureSize(FMath::Max(1, FMath::RoundToInt(El->Width * ResolutionScale)),
		FMath::Max(1, FMath::RoundToInt(El->Height * ResolutionScale)));
	if (ExamPaperDollRenderTarget->SizeX != CaptureSize.X || ExamPaperDollRenderTarget->SizeY != CaptureSize.Y)
		ExamPaperDollRenderTarget->ResizeTarget(CaptureSize.X, CaptureSize.Y);
	if (!ApplyPreviewCaptureAppearance(ExamPaperDollPreviewActor, ExamPaperDollCapture, Obj, Scale))
	{
		// Streaming can make a setup temporarily unavailable. Retry it on the
		// next refresh instead of caching an empty or previous subject's model.
		ExamPaperDollAppliedGuid = 0;
		ExamPaperDollAppliedHash = 0;
		ExamPaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	ExamPaperDollAppliedGuid = LastAppraisal.ObjectGuid;
	ExamPaperDollAppliedHash = Hash;
}

void UACEUIGameplayBinder::ReleaseExamPaperDollPreview()
{
	ExamPaperDollAppliedGuid = 0;
	ExamPaperDollAppliedHash = 0;
	if (ExamPaperDollModelImage)
	{
		ExamPaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (ExamPaperDollPreviewActor)
	{
		ExamPaperDollPreviewActor->Destroy();
		ExamPaperDollPreviewActor = nullptr;
	}
	ExamPaperDollCapture = nullptr;
	ExamPaperDollRenderTarget = nullptr;
}

void UACEUIGameplayBinder::RefreshInventoryBurdenOverlays()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree || !Client
		|| ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		if (InvBurdenBeginLabel) { InvBurdenBeginLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (InvBurdenValueLabel) { InvBurdenValueLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (InvTitleLabel) { InvTitleLabel->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}

	FACEWorldObject SelfObj;
	Client->GetWorldObject(Client->GetPlayerGuid(), SelfObj);
	const FACEPlayerVitals Vitals = Client->GetPlayerVitals();
	// Retail / ACE.Server: capacity = buffed Strength * (150 + 30 * carrying augs).
	const int32 Strength = FMath::Max(1, Vitals.bValid ? Vitals.GetBuffedStrength() : 100);
	const int32 NumAugs = Vitals.bValid ? FMath::Clamp(Vitals.CarryingCapacityAugs, 0, 5) : 0;
	const int32 BonusBurden = FMath::Clamp(30 * NumAugs, 0, 150);
	const int32 Capacity = FMath::Max(1, 150 * Strength + Strength * BonusBurden);
	// Prefer PropertyInt.EncumbranceVal; fall back to ObjectCreate Burden on the player weenie.
	int32 Enc = 0;
	if (!Client->TryGetPlayerEncumbrance(Enc))
	{
		if (SelfObj.Burden > 0)
		{
			Enc = SelfObj.Burden;
		}
		else
		{
			auto AddBurden = [&](const TArray<FACEWorldObject>& Items)
			{
				for (const FACEWorldObject& It : Items)
				{
					Enc += FMath::Max(0, It.Burden);
				}
			};
			AddBurden(Client->GetEquippedItems());
			AddBurden(Client->GetPackItems(Client->GetPlayerGuid()));
			for (const FACEWorldObject& Pack : Client->GetPlayerPacks())
			{
				Enc += FMath::Max(0, Pack.Burden);
				AddBurden(Client->GetPackItems(Pack.Guid));
			}
		}
	}
	Enc = FMath::Max(0, Enc);
	const float BurdenRatio = static_cast<float>(Enc) / static_cast<float>(Capacity);
	Client->SetBurden(BurdenRatio);

	// Green while under capacity, yellow from 100%, red when overburdened (≥200% immobile ramp).
	const FLinearColor BurdenTint = FLinearColor::White;
	if (TSharedPtr<FACEUIElement> Meter = Manager->FindElementUnder(
		TEXT("InventoryPanel_Field"), TEXT("Inv_BurdenBar")))
	{
		Meter->bVisible = true;
		// Retail gmBackpackUI::SetLoadLevel: meter attribute = clamp(load / 3, 0..1)
		// — the bar saturates at 300% burden, not at capacity.
		Meter->MeterFillFraction = FMath::Clamp(BurdenRatio / 3.f, 0.f, 1.f);
		Meter->ImageTint = BurdenTint;
		// Tint fill children as well (retail meter art is nested).
		for (const TSharedPtr<FACEUIElement>& Child : Meter->Children)
		{
			if (Child.IsValid())
			{
				Child->ImageTint = BurdenTint;
			}
		}
	}

	if (!InvBurdenBeginLabel)
	{
		InvBurdenBeginLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		InvBurdenBeginLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		InvBurdenBeginLabel->SetJustification(ETextJustify::Center);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 8);
		InvBurdenBeginLabel->SetFont(Font);
		InvBurdenBeginLabel->SetColorAndOpacity(FSlateColor(TextGold));
	}
	if (!InvBurdenValueLabel)
	{
		InvBurdenValueLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		InvBurdenValueLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		InvBurdenValueLabel->SetJustification(ETextJustify::Center);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 8);
		InvBurdenValueLabel->SetFont(Font);
	}

	// Retail displays floor(load * 100) as "%d%%".
	const int32 Pct = FMath::FloorToInt(BurdenRatio * 100.f);
	InvBurdenValueLabel->SetColorAndOpacity(FSlateColor(BurdenTint));
	PlaceTextOnElement(InvBurdenBeginLabel, TEXT("Inv_BurdenTextBegin"), TEXT("Burden"), 8, TextWhite, 10020);
	PlaceTextOnElement(InvBurdenValueLabel, TEXT("Inv_BurdenText"),
		FString::Printf(TEXT("%d%%"), Pct), 8, BurdenTint, 10021);

	// Retail gmInventoryUI title: "Inventory of %s" (acclient.c:222477).
	if (!InvTitleLabel)
	{
		InvTitleLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		InvTitleLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		InvTitleLabel->SetJustification(ETextJustify::Center);
	}
	if (!SelfObj.Name.IsEmpty())
	{
		const auto Title = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("InvTitleText"));
		const FString FullTitle = FString::Printf(TEXT("Inventory of %s"), *SelfObj.Name);
		FString DisplayTitle = FullTitle;
		FACEDatFont Font;
		if (Title && Canvas->GetResourceResolver()->ResolveFont(Title->FontId,Font))
			DisplayTitle=ACEDatText::Ellipsize(Font,FullTitle,
				FMath::Max(1,Title->Width-int32(Title->TextMargins.Left+Title->TextMargins.Right)));
		// Preserve the authored 18px white DAT title, 5px text insets, gold frame
		// and separate close-button well. Long names truncate instead of clipping
		// both ends of the centered title (UIElement_Text::SetTruncateTextToFit).
		PlaceTextOnElement(InvTitleLabel, Title, DisplayTitle, 9, TextWhite, 10023);
		SetRetailTooltip(InvTitleLabel,FText::FromString(FullTitle));
	}
	else
	{
		InvTitleLabel->SetVisibility(ESlateVisibility::Collapsed);
	}
}

namespace
{
uint32 HashOverlayGeometry(const UACEUICanvasWidget* Canvas, UACEUIElementManager* Manager, const TCHAR* WindowName)
{
	uint32 Hash = Canvas ? GetTypeHash(Canvas->GetCachedGeometry().GetLocalSize()) : 0;
	if (Canvas) Hash = HashCombine(Hash, GetTypeHash(Canvas->GetLastScale2D()));
	const auto Window = Manager ? Manager->FindElementByName(WindowName) : nullptr;
	if (Window)
	{
		Hash = HashCombine(Hash, GetTypeHash(Window->GetScreenOrigin()));
		Hash = HashCombine(Hash, GetTypeHash(FIntPoint(Window->Width, Window->Height)));
	}
	return Hash;
}
}

uint64 UACEUIGameplayBinder::HashInventoryOverlayState() const
{
	uint64 H = HashCombine(GetTypeHash(SelectedPackGuid), GetTypeHash(InventoryScrollOffset));
	H = HashCombine(H, HashOverlayGeometry(Canvas, Manager, TEXT("RootGameplay_FloatyPanel_Field")));
	H = HashCombine(H, GetTypeHash(PackScrollOffset));
	H = HashCombine(H, GetTypeHash(LastSelection.bValid ? LastSelection.Guid : 0));
	H = HashCombine(H, GetTypeHash(bShowPaperdollSlots));
	H = HashCombine(H, GetTypeHash(SalvageQueueGuids.Num()));
	for (int32 Guid : SalvageQueueGuids) H = HashCombine(H, GetTypeHash(Guid));
	if (!Client)
	{
		return H;
	}
	const auto& TradeItems = Client->GetTradeSelfItems();
	H = HashCombine(H, GetTypeHash(TradeItems.Num()));
	for (int32 Guid : TradeItems) H = HashCombine(H, GetTypeHash(Guid));
	for (int32 S = 0; S < 18; ++S) H = HashCombine(H, GetTypeHash(Client->GetShortcutObject(S)));
	const FACEPlayerVitals Vitals = Client->GetPlayerVitals();
	int32 Encumbrance = 0;
	Client->TryGetPlayerEncumbrance(Encumbrance);
	H = HashCombine(H, GetTypeHash(Encumbrance));
	H = HashCombine(H, GetTypeHash(Vitals.GetBuffedStrength()));
	H = HashCombine(H, GetTypeHash(Vitals.CarryingCapacityAugs));
	H = HashCombine(H, GetTypeHash(Vitals.Level));
	H = HashCombine(H, GetTypeHash(Vitals.AetheriaUnlocked));
	if (Manager)
	{
		if (TSharedPtr<FACEUIElement> Page = Manager->FindElementByName(TEXT("InventoryPanel_Field")))
		{
			H = HashCombine(H, HashCombine(GetTypeHash(Page->Width), GetTypeHash(Page->Height)));
		}
	}
	const auto Session = Client->GetSession();
	if (!Session) return H;
	const auto& Objects = Session->GetWorldObjects();
	TArray<int32> Items;
	Session->GetPackItemGuids(SelectedPackGuid, Items);
	H = HashCombine(H, GetTypeHash(Items.Num()));
	const FACEWorldObject PendingItem;
	for (int32 Guid : Items)
	{
		const auto* Item = Objects.Find(Guid);
		const auto& O = Item ? *Item : PendingItem;
		H = HashCombine(H, GetTypeHash(Guid));
		H = HashCombine(H, GetTypeHash(O.IconId));
		H = HashCombine(H, GetTypeHash(O.IconOverlayId));
		H = HashCombine(H, GetTypeHash(O.IconUnderlayId));
		H = HashCombine(H, GetTypeHash(O.StackSize));
		H = HashCombine(H, GetTypeHash(O.Structure));
		H = HashCombine(H, GetTypeHash(O.MaxStructure));
		H = HashCombine(H, GetTypeHash(O.MaterialType));
		H = HashCombine(H, GetTypeHash(O.Name));
		H = HashCombine(H, GetTypeHash(O.PluralName));
		H = HashCombine(H, GetTypeHash(O.UiEffects));
		H = HashCombine(H, GetTypeHash(O.PlacementPosition));
	}
	TArray<const FACEWorldObject*, TInlineAllocator<32>> Equipped;
	for (const auto& Pair : Objects)
	{
		const auto& O = Pair.Value;
		if (O.CurrentWieldedLocation != 0
			&& (O.WielderId == Client->GetPlayerGuid() || O.ParentGuid == Client->GetPlayerGuid()))
			Equipped.Add(&O);
	}
	H = HashCombine(H, GetTypeHash(Equipped.Num()));
	for (const auto* Item : Equipped)
	{
		const auto& O = *Item;
		H = HashCombine(H, GetTypeHash(O.Guid));
		H = HashCombine(H, GetTypeHash(O.CurrentWieldedLocation));
		H = HashCombine(H, GetTypeHash(O.Structure));
		H = HashCombine(H, GetTypeHash(O.MaxStructure));
		H = HashCombine(H, GetTypeHash(O.MaterialType));
		H = HashCombine(H, GetTypeHash(O.Name));
		H = HashCombine(H, GetTypeHash(O.IconId));
		H = HashCombine(H, GetTypeHash(O.IconOverlayId));
	}
    TArray<int32> Packs;
    Session->GetPlayerPackGuids(Packs);
    TMap<int32, int32> PackCounts;
    PackCounts.Add(Client->GetPlayerGuid(), 0);
    for (int32 Guid : Packs) PackCounts.Add(Guid, 0);
    Session->GetPackItemCounts(PackCounts);
    H = HashCombine(H, GetTypeHash(PackCounts.FindRef(Client->GetPlayerGuid())));
    if (const auto* Player = Objects.Find(Client->GetPlayerGuid()))
        H = HashCombine(H, GetTypeHash(Player->ContainersCapacity));
    H = HashCombine(H, GetTypeHash(Packs.Num()));
    for (int32 Guid : Packs)
    {
        const auto& Pack = Objects.FindChecked(Guid);
        H = HashCombine(H, GetTypeHash(Pack.Guid));
        H = HashCombine(H, GetTypeHash(Pack.IconId));
        H = HashCombine(H, GetTypeHash(Pack.IconOverlayId));
        H = HashCombine(H, GetTypeHash(Pack.IconUnderlayId));
        H = HashCombine(H, GetTypeHash(Pack.ItemType));
        H = HashCombine(H, GetTypeHash(Pack.Structure));
        H = HashCombine(H, GetTypeHash(Pack.MaxStructure));
        H = HashCombine(H, GetTypeHash(Pack.MaterialType));
        H = HashCombine(H, GetTypeHash(Pack.Name));
        H = HashCombine(H, GetTypeHash(Pack.UiEffects));
        H = HashCombine(H, GetTypeHash(Pack.ItemsCapacity));
        H = HashCombine(H, GetTypeHash(Pack.PlacementPosition));
        H = HashCombine(H, GetTypeHash(PackCounts.FindRef(Pack.Guid)));
    }

	return H;
}

uint64 UACEUIGameplayBinder::HashVendorOverlayState() const
{
	uint64 H = HashCombine(GetTypeHash(OpenVendorGuid), GetTypeHash(ActiveVendorPage));
	H = HashCombine(H, HashOverlayGeometry(Canvas, Manager, TEXT("RootGameplay_FloatyEnvPanel_Field")));
	H = HashCombine(H, GetTypeHash(VendorItemTypeFilter));
	H = HashCombine(H, GetTypeHash(bVendorFilterDropdownOpen));
	H = HashCombine(H, GetTypeHash(VendorItemsScrollOffset));
	H = HashCombine(H, GetTypeHash(VendorBuyScrollOffset));
	H = HashCombine(H, GetTypeHash(VendorSellScrollOffset));
	H = HashCombine(H, GetTypeHash(VendorSelectedGuid));
	H = HashCombine(H, GetTypeHash(VendorSellSelectedGuid));
	H = HashCombine(H, GetTypeHash(SelectedStackAmount));
	H = HashCombine(H, GetTypeHash(VendorBuyCart.Num()));
	H = HashCombine(H, GetTypeHash(VendorSellCart.Num()));
	for (const TPair<int32, int32>& Pair : VendorBuyCart)
	{
		H = HashCombine(H, HashCombine(GetTypeHash(Pair.Key), GetTypeHash(Pair.Value)));
	}
	for (const TPair<int32, int32>& Pair : VendorSellCart)
	{
		H = HashCombine(H, HashCombine(GetTypeHash(Pair.Key), GetTypeHash(Pair.Value)));
	}
	if (!Client)
	{
		return H;
	}
	const auto Session = Client->GetSession();
	if (Session)
	{
		H = HashCombine(H, GetTypeHash(Session->GetVendorPurchaseFunds()));
		H = HashCombine(H, GetTypeHash(Session->VendorUsesPyreals()));
		H = HashCombine(H, GetTypeHash(Session->GetVendorCurrencyName()));
	}
	const TConstArrayView<FACEWorldObject> Merch = Session
		? TConstArrayView<FACEWorldObject>(Session->GetVendorMerchandise()) : TConstArrayView<FACEWorldObject>();
	H = HashCombine(H, GetTypeHash(Merch.Num()));
	for (const FACEWorldObject& O : Merch)
	{
		H = HashCombine(H, GetTypeHash(O.Guid));
		H = HashCombine(H, GetTypeHash(O.IconId));
		H = HashCombine(H, GetTypeHash(O.StackSize));
		H = HashCombine(H, GetTypeHash(O.Value));
		H = HashCombine(H, GetTypeHash(O.Structure));
		H = HashCombine(H, GetTypeHash(O.MaxStructure));
		H = HashCombine(H, GetTypeHash(O.MaterialType));
		H = HashCombine(H, GetTypeHash(O.Name));
	}
	return H;
}

uint64 UACEUIGameplayBinder::HashLootOverlayState() const
{
	uint64 H = HashCombine(GetTypeHash(OpenLootContainerGuid), GetTypeHash(OpenLootSelectedPackGuid));
	H = HashCombine(H, HashOverlayGeometry(Canvas, Manager, TEXT("RootGameplay_FloatyEnvPanel_Field")));
	H = HashCombine(H, GetTypeHash(ExtItemScrollOffset));
	H = HashCombine(H, GetTypeHash(ExtPackScrollOffset));
	H = HashCombine(H, GetTypeHash(LastSelection.bValid ? LastSelection.Guid : 0));
	if (!Client || OpenLootContainerGuid == 0)
	{
		return H;
	}
	const int32 ContGuid = OpenLootSelectedPackGuid != 0 ? OpenLootSelectedPackGuid : OpenLootContainerGuid;
	TArray<FACEWorldObject> Items = Client->GetPackItems(ContGuid);
	// Pack-slot items are deliberately absent from GetPackItems' ordinary grid.
	TArray<int32> Packs;
	if (const auto Session = Client->GetSession()) Session->GetContainerPackGuids(OpenLootContainerGuid, Packs);
	for (int32 Guid : Packs)
	{
		FACEWorldObject Pack;
		if (Client->GetWorldObject(Guid, Pack)) Items.Add(Pack);
	}
	FACEWorldObject Root;
	if (Client->GetWorldObject(OpenLootContainerGuid, Root))
		H = HashCombine(H, GetTypeHash(Root.ContainersCapacity));
	H = HashCombine(H, GetTypeHash(Items.Num()));
	for (const FACEWorldObject& O : Items)
	{
		H = HashCombine(H, GetTypeHash(O.Guid));
		H = HashCombine(H, GetTypeHash(O.IconId));
		H = HashCombine(H, GetTypeHash(O.IconOverlayId));
		H = HashCombine(H, GetTypeHash(O.IconUnderlayId));
		H = HashCombine(H, GetTypeHash(O.ItemType));
		H = HashCombine(H, GetTypeHash(O.UiEffects));
		H = HashCombine(H, GetTypeHash(O.StackSize));
		H = HashCombine(H, GetTypeHash(O.Structure));
		H = HashCombine(H, GetTypeHash(O.MaxStructure));
		H = HashCombine(H, GetTypeHash(O.MaterialType));
		H = HashCombine(H, GetTypeHash(O.Name));
	}
	return H;
}

void UACEUIGameplayBinder::RefreshInventoryOverlays()
{
	if (!Client || !Manager || !Canvas || ActivePanelPage != TEXT("InventoryPanel_Field"))
	{
		if (!bInventoryOverlaysCollapsed)
		{
		bInventoryOverlaysCollapsed = true;
		LastInventoryOverlayHash = ~uint64(0);
		for (UBorder* B : InventorySlots)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : InventorySlotBgs)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : InventorySlotOverlays)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : InventorySlotSelected)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UTextBlock* T : InventorySlotCounts)
		{
			if (T) { T->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : PackSlots)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : PackSlotBgs)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : PackSlotSelected)
		{
			if (B) { B->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (UBorder* B : PackShortcutIcons) if (B) B->SetVisibility(ESlateVisibility::Collapsed);
        for (UBorder* B : PackItemSelected) if (B) B->SetVisibility(ESlateVisibility::Collapsed);
        if (MainPackCapacityMeter) MainPackCapacityMeter->SetVisibility(ESlateVisibility::Collapsed);
        for (UProgressBar* B : PackCapacityMeters) if (B) B->SetVisibility(ESlateVisibility::Collapsed);
		if (MainPackShortcutIcon) MainPackShortcutIcon->SetVisibility(ESlateVisibility::Collapsed);
		for (auto& Pair : PaperDollIcons)
		{
			if (Pair.Value) { Pair.Value->SetVisibility(ESlateVisibility::Collapsed); }
		}
        for (auto& Pair : PaperDollShortcutIcons)
            if (Pair.Value) Pair.Value->SetVisibility(ESlateVisibility::Collapsed);
		for (auto& Pair : PaperDollSelectedIcons) if (Pair.Value) Pair.Value->SetVisibility(ESlateVisibility::Collapsed);
		if (PaperDollDragTargetIcon) PaperDollDragTargetIcon->SetVisibility(ESlateVisibility::Collapsed);
		for (auto& Pair : PaperDollSlotBgs)
		{
			if (Pair.Value) { Pair.Value->SetVisibility(ESlateVisibility::Collapsed); }
		}
		if (PaperDollModelImage) { PaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed); }
		if (PaperdollSlotsCheckboxIcon) { PaperdollSlotsCheckboxIcon->SetVisibility(ESlateVisibility::Collapsed); }
		if (PaperdollSlotsCheckboxLabel) { PaperdollSlotsCheckboxLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (InvBurdenBeginLabel) { InvBurdenBeginLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (InvBurdenValueLabel) { InvBurdenValueLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (InvTitleLabel) { InvTitleLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (InvContentsLabel) { InvContentsLabel->SetVisibility(ESlateVisibility::Collapsed); }
		if (ChatLog)
		{
			// keep chat always — place below
		}
		// Still place chat overlays every tick
		}
	}
	else
	{
		bInventoryOverlaysCollapsed = false;
		// The opaque scene capture covers ordinary DAT widgets. Repaint the authored
		// checkbox at its original child rectangle above the model, every frame so
		// capture/hover changes do not depend on an inventory mutation.
		if (auto Cb = Manager->FindElementByName(TEXT("Paperdoll_Slots_Checkbox")))
		{
			Cb->bHighlighted = bShowPaperdollSlots;
			Cb->bActivatable = true;
			Cb->ResolvePaintState(Manager->GetCaptureElement()==Cb, Manager->GetHoverElement()==Cb, Manager->GetFocusElement()==Cb);
			for (const auto& Child : Cb->Children)
			{
				if (!Child) continue;
				Child->bActivatable = false;
				const auto* State = Child->ResolvePaintState(false, false, false);
				const uint32 Did = State && State->ImageFileId ? State->ImageFileId : Child->ImageFileId;
				if (!Did) continue;
				if (!PaperdollSlotsCheckboxIcon && Canvas->WidgetTree)
				{
					PaperdollSlotsCheckboxIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
					PaperdollSlotsCheckboxIcon->SetPadding(FMargin(0));
				}
				SetIconDid(PaperdollSlotsCheckboxIcon, Did);
				PaperdollSlotsCheckboxIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
				Canvas->PlaceWidgetAtElement(PaperdollSlotsCheckboxIcon, Child, 120001);
				break;
			}
			if (!PaperdollSlotsCheckboxLabel && Canvas->WidgetTree)
				PaperdollSlotsCheckboxLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			PlaceTextOnElement(PaperdollSlotsCheckboxLabel, Cb, TEXT("Slots"), 8, TextWhite, 120001);
		}
		const uint64 OverlayHash = HashInventoryOverlayState();
		if (OverlayHash == LastInventoryOverlayHash)
		{
			RefreshPaperDollPreview();
		}
		else
		{
		LastInventoryOverlayHash = OverlayHash;
		ReflowInventoryPanelGeometry();
		RefreshInventoryBurdenOverlays();

		const int32 Self = Client->GetPlayerGuid();
		if (SelectedPackGuid == 0)
		{
			SelectedPackGuid = Self;
		}
		// Above SyncElementWidgets DAT chrome (climbs past 10k on dense layouts).
		constexpr int32 OverlayZ = 120000;

		// Retail draws the live character in the PaperDoll viewport; equipment icons sit on top.
		RefreshPaperDollPreview();

		// Paper doll — resolve under InventoryPanel so merged layout hits the right nodes.
		TArray<FACEWorldObject> Equipped = Client->GetEquippedItems();
		for (const FDollSlotMap& Slot : GDollSlots)
		{
			TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), Slot.Name);
			if (!El.IsValid())
			{
				El = Manager->FindElementByName(Slot.Name);
			}
			if (!El.IsValid())
			{
				continue;
			}
			UBorder* Bg = PaperDollSlotBgs.FindRef(Slot.Name);
			if (!Bg && Canvas->WidgetTree)
			{
				Bg = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
				Bg->SetPadding(FMargin(0.f));
				PaperDollSlotBgs.Add(Slot.Name, Bg);
			}
			UBorder* Icon = PaperDollIcons.FindRef(Slot.Name);
			if (!Icon && Canvas->WidgetTree)
			{
				Icon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
				Icon->SetPadding(FMargin(0.f));
				PaperDollIcons.Add(Slot.Name, Icon);
			}
			if (!Icon)
			{
				continue;
			}
			const FACEWorldObject* Item = nullptr;
			for (const FACEWorldObject& Obj : Equipped)
			{
				if (ItemMatchesDollSlot(Obj, Slot.Mask))
				{
					Item = &Obj;
					break;
				}
			}
			// Show Equipment ON: main-body armor icons. OFF: 3D model + peripheral slots
			// (jewelry, underclothes, weapons/shield/ammo) stay visible.
			const bool bMainBody = IsMainBodyDollSlot(Slot.Name);
			const int32 AetheriaBit = Slot.Mask == ACEEquipMask::SigilOne ? 1 : Slot.Mask == ACEEquipMask::SigilTwo ? 2 : Slot.Mask == ACEEquipMask::SigilThree ? 4 : 0;
            const bool bUnlocked = !AetheriaBit || (Client->GetPlayerVitals().AetheriaUnlocked & AetheriaBit) != 0;
            if (AetheriaBit) El->bVisible = bUnlocked;
            const bool bShowThisSlot = bUnlocked && (bShowPaperdollSlots || !bMainBody);
			const bool bHasItem = Item != nullptr;
			UBorder* SelectedIcon = PaperDollSelectedIcons.FindRef(Slot.Name);
			if (!SelectedIcon && Canvas->WidgetTree)
			{
				SelectedIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
				SelectedIcon->SetPadding(FMargin(0)); PaperDollSelectedIcons.Add(Slot.Name, SelectedIcon);
			}
			if (SelectedIcon)
			{
				const bool bSelected = bShowThisSlot && Item && LastSelection.bValid && LastSelection.Guid == Item->Guid;
				SelectedIcon->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
				if (bSelected)
				{
					SetIconDid(SelectedIcon, DidInvSlotSelected);
					Canvas->PlaceWidgetAtElement(SelectedIcon, El, OverlayZ+3);
				}
			}
            UBorder* ShortcutIcon=PaperDollShortcutIcons.FindRef(Slot.Name);
            if (!ShortcutIcon && Canvas->WidgetTree)
            {
                ShortcutIcon=Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
                ShortcutIcon->SetPadding(FMargin(0)); PaperDollShortcutIcons.Add(Slot.Name,ShortcutIcon);
            }
            int32 Shortcut=INDEX_NONE;
            for (int32 S=0; Item && S<18; ++S)
                if (Client->GetShortcutObject(S)==Item->Guid) { Shortcut=S; break; }
            if (ShortcutIcon)
            {
                ShortcutIcon->SetVisibility(bShowThisSlot && Shortcut!=INDEX_NONE ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
                if (bShowThisSlot && Shortcut!=INDEX_NONE)
                {
                    SetIconDid(ShortcutIcon,Shortcut<9 ? 0x0600109Eu+Shortcut : 0x060074D3u);
                    Canvas->PlaceWidgetAtElement(ShortcutIcon,El,OverlayZ+2);
                }
            }

			if (Bg)
			{
				if (bShowThisSlot)
				{
					Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
					if (bHasItem)
					{
						SetItemSlotBackground(Bg, Item);
					}
					else
					{
						SetIconDid(Bg, Slot.EmptyIconDid);
					}
					Canvas->PlaceWidgetAtElement(Bg, El, OverlayZ - 1);
				}
				else
				{
					Bg->SetVisibility(ESlateVisibility::Collapsed);
				}
			}
			if (bShowThisSlot && bHasItem)
			{
				Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
				SetItemSlotForeground(Icon, Item);
				SetRetailTooltip(Icon, FText::FromString(ACERetailObjectNames::Name(*Item)));
				Canvas->PlaceWidgetAtElement(Icon, El, OverlayZ);
			}
			else
			{
				Icon->SetVisibility(ESlateVisibility::Collapsed);
				SetIconDid(Icon, 0);
			}
		}

		// Pack tabs in Inv_ContainerList — retail: main pack + ContainersCapacity side slots (usually 7).
		TArray<FACEWorldObject> Packs = Client->GetPlayerPacks();
		TMap<int32, int32> PackCounts;
		PackCounts.Add(Self, 0);
		for (const auto& Pack : Packs) PackCounts.Add(Pack.Guid, 0);
		if (const auto Session = Client->GetSession()) Session->GetPackItemCounts(PackCounts);
		FACEWorldObject SelfObj;
		Client->GetWorldObject(Self, SelfObj);
		// Retail: ContainersCapacity side-pack slots (augs can raise this above 7).
		int32 SidePackCapacity = SelfObj.ContainersCapacity > 0 ? SelfObj.ContainersCapacity : 7;
		SidePackCapacity = FMath::Clamp(SidePackCapacity, 1, 24);
		// Always show at least as many slots as packs we actually have.
		SidePackCapacity = FMath::Max(SidePackCapacity, Packs.Num());
		SidePackCapacity = FMath::Clamp(SidePackCapacity, 1, 24);
		TSharedPtr<FACEUIElement> ListEl = Manager->FindElementByName(TEXT("Inv_ContainerList"));
        auto MakeCapacityMeter = [&]()
        {
                    auto* Meter = Canvas->WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
                    Meter->SetBarFillType(EProgressBarFillType::BottomToTop);
                    Meter->SetBarFillStyle(EProgressBarFillStyle::Mask);
                    Meter->SetBorderPadding(FVector2D::ZeroVector);
                    Meter->SetFillColorAndOpacity(FLinearColor::White);
                    FProgressBarStyle Style;
                    Style.BackgroundImage.SetResourceObject(Canvas->GetResourceResolver()->ResolveTexture(0x06004D22));
                    Style.BackgroundImage.DrawAs=ESlateBrushDrawType::Image;
                    Style.BackgroundImage.ImageSize=FVector2D(5,30);
                    Style.FillImage.SetResourceObject(Canvas->GetResourceResolver()->ResolveTexture(0x06004D23));
                    Style.FillImage.DrawAs=ESlateBrushDrawType::Image;
                    Style.FillImage.ImageSize=FVector2D(5,30);
                    Meter->SetWidgetStyle(Style);
            return Meter;
        };
		TSharedPtr<FACEUIElement> MainPackEl = Manager->FindElementByName(TEXT("Inv_MainPackSlot"));
		if (MainPackEl.IsValid())
		{
            if (!MainPackCapacityMeter) MainPackCapacityMeter=MakeCapacityMeter();
            const int32 MainCount=PackCounts.FindRef(Self);
            const int32 MainCapacity=SelfObj.ItemsCapacity > 0 ? SelfObj.ItemsCapacity : 102;
            MainPackCapacityMeter->SetVisibility(MainCount > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
            MainPackCapacityMeter->SetPercent(FMath::Clamp(float(MainCount)/MainCapacity,0.f,1.f));
            Canvas->PlaceWidgetAtElement(MainPackCapacityMeter,MainPackEl,OverlayZ+4);
            if (auto* MeterSlot=Cast<UCanvasPanelSlot>(MainPackCapacityMeter->Slot))
            {
                MeterSlot->SetPosition((FVector2D(MainPackEl->GetScreenOrigin())+FVector2D(28,3))*Canvas->GetLastScale2D());
                MeterSlot->SetSize(FVector2D(5,30)*Canvas->GetLastScale2D());
            }
			if (!MainPackShortcutIcon)
				MainPackShortcutIcon = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			int32 MainBound = INDEX_NONE;
			for (int32 S=0; S<18; ++S)
				if (Client->GetShortcutObject(S)==Self) { MainBound=S; break; }
			MainPackShortcutIcon->SetVisibility(MainBound!=INDEX_NONE ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (MainBound!=INDEX_NONE)
			{
				SetIconDid(MainPackShortcutIcon,MainBound<9 ? 0x0600109Eu+MainBound : 0x060074D3u);
				Canvas->PlaceWidgetAtElement(MainPackShortcutIcon, MainPackEl, OverlayZ+2, FMargin(2));
			}
			UBorder* MainIcon = EnsureIconBorder(PackSlots, 0);
			UBorder* MainBg = EnsureIconBorder(PackSlotBgs, 0);
			UBorder* MainSel = EnsureIconBorder(PackSlotSelected, 0);
			if (MainBg)
			{
				// IconData::RenderIcons treats the player's main pack as Container (512).
				FACEWorldObject MainPackStyle;
				MainPackStyle.ItemType = ACEItemType::Container;
				SetItemSlotBackground(MainBg, &MainPackStyle);
				MainBg->SetVisibility(ESlateVisibility::HitTestInvisible);
				Canvas->PlaceWidgetAtElement(MainBg, MainPackEl, OverlayZ - 1, FMargin(2));
			}
			if (MainIcon)
			{
				PackSlotGuids.SetNum(1 + SidePackCapacity);
				PackSlotGuids[0] = Self;
				MainIcon->SetVisibility(ESlateVisibility::HitTestInvisible);
				// Retail: closed bag idle, open bag when this pack is selected.
				const bool bMainSelected = SelectedPackGuid == Self;
				SetIconDid(MainIcon, bMainSelected ? 0x06004CF8 : 0x06004CF7);
				MainIcon->SetRenderOpacity(1.f);
				SetRetailTooltip(MainIcon, FText::FromString(TEXT("Main Pack")));
				Canvas->PlaceWidgetAtElement(MainIcon, MainPackEl, OverlayZ, FMargin(2));
			}
			if (MainSel)
			{
				const bool bMainSelected = SelectedPackGuid == Self;
				MainSel->SetVisibility(bMainSelected
					? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
				if (bMainSelected)
				{
					SetIconDid(MainSel, 0x06005D9C);
					Canvas->PlaceWidgetAtElement(MainSel, MainPackEl, OverlayZ + 1);
				}
			}
		}
		if (ListEl.IsValid())
		{
			const FIntPoint Origin = ListEl->GetScreenOrigin();
			const float SX = Canvas->GetLastScaleX();
			const float SY = Canvas->GetLastScaleY();
			constexpr int32 Cell = 36;
			const int32 MaxVisible = FMath::Max(1, ListEl->Height / Cell);
			PackScrollOffset = FMath::Clamp(PackScrollOffset, 0, FMath::Max(0, SidePackCapacity - MaxVisible));
			PackSlotGuids.SetNum(1 + MaxVisible);
			for (int32 i = 0; i < MaxVisible; ++i)
			{
				UBorder* Icon = EnsureIconBorder(PackSlots, i + 1);
				UBorder* Bg = EnsureIconBorder(PackSlotBgs, i + 1);
				UBorder* Sel = EnsureIconBorder(PackSlotSelected, i + 1);
				if (!Icon) { continue; }
				const int32 PackIndex = i + PackScrollOffset;
				if (PackIndex >= SidePackCapacity)
				{
					Icon->SetVisibility(ESlateVisibility::Collapsed);
					if (Bg) { Bg->SetVisibility(ESlateVisibility::Collapsed); }
					if (Sel) { Sel->SetVisibility(ESlateVisibility::Collapsed); }
                    if (PackItemSelected.IsValidIndex(i) && PackItemSelected[i]) PackItemSelected[i]->SetVisibility(ESlateVisibility::Collapsed);
                    if (PackCapacityMeters.IsValidIndex(i) && PackCapacityMeters[i]) PackCapacityMeters[i]->SetVisibility(ESlateVisibility::Collapsed);
					PackSlotGuids[i + 1] = 0;
					continue;
				}
				Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
				const bool bSelected = Packs.IsValidIndex(PackIndex)
					&& SelectedPackGuid == Packs[PackIndex].Guid;
				if (Packs.IsValidIndex(PackIndex))
				{
					PackSlotGuids[i + 1] = Packs[PackIndex].Guid;
					SetItemSlotForeground(Icon, &Packs[PackIndex]);
					SetItemSlotBackground(Bg, &Packs[PackIndex]);
					if (Bg) { Bg->SetVisibility(ESlateVisibility::HitTestInvisible); }
					SetRetailTooltip(Icon, FText::FromString(FString::Printf(
						TEXT("%s (%d/%d items)"), *ACERetailObjectNames::Name(Packs[PackIndex]), PackCounts.FindRef(Packs[PackIndex].Guid), Packs[PackIndex].ItemsCapacity)));
					Icon->SetRenderOpacity(1.f);
				}
				else
				{
					PackSlotGuids[i + 1] = 0;
					// ItemSlot_Backpack/ItemSlot_Icon's authored ItemSlot_Empty state.
					SetIconDid(Icon, DidEmptyPackSlot);
					if (Bg) { Bg->SetVisibility(ESlateVisibility::Collapsed); }
					SetRetailTooltip(Icon, FText::FromString(FString::Printf(
						TEXT("Empty pack slot (%d/%d)"), PackIndex + 1, SidePackCapacity)));
					Icon->SetRenderOpacity(1.f);
				}
				auto PlacePack = [&](UWidget* B, int32 Z, int32 X = 2, int32 Y = 2, int32 W = 32, int32 H = 32)
				{
					if (!B) { return; }
					if (B->GetParent() != Canvas->GetElementLayer())
					{
						Canvas->GetElementLayer()->AddChild(B);
					}
					if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(B->Slot))
					{
						Slot->SetAnchors(FAnchors(0.f, 0.f));
						Slot->SetPosition(FVector2D(
							static_cast<float>(Origin.X + X) * SX,
							static_cast<float>(Origin.Y + i * Cell + Y) * SY));
						Slot->SetSize(FVector2D(static_cast<float>(W) * SX, static_cast<float>(H) * SY));
						Canvas->SetOverlayOrder(B, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), Z);
					}
				};
				PlacePack(Bg, OverlayZ - 1);
				PlacePack(Icon, OverlayZ);
                UBorder* ItemSel = EnsureIconBorder(PackItemSelected, i);
                ItemSel->SetVisibility(Packs.IsValidIndex(PackIndex) && LastSelection.bValid && LastSelection.Guid == Packs[PackIndex].Guid
                    ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
                SetIconDid(ItemSel, DidInvSlotSelected);
                PlacePack(ItemSel, OverlayZ+3);
                while (PackCapacityMeters.Num() <= i)
                {
                    PackCapacityMeters.Add(MakeCapacityMeter());
                }
                auto* Meter = PackCapacityMeters[i].Get();
                const int32 Capacity = Packs.IsValidIndex(PackIndex) ? Packs[PackIndex].ItemsCapacity : 0;
                const int32 Count = Capacity > 0 ? PackCounts.FindRef(Packs[PackIndex].Guid) : 0;
                Meter->SetVisibility(Count > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
                Meter->SetPercent(Capacity > 0 ? FMath::Clamp(float(Count)/Capacity,0.f,1.f) : 0.f);
                PlacePack(Meter,OverlayZ+4,28,3,5,30);

				UBorder* Shortcut = EnsureIconBorder(PackShortcutIcons,i);
				int32 Bound = INDEX_NONE;
				for (int32 S=0; Packs.IsValidIndex(PackIndex) && S<18; ++S)
					if (Client->GetShortcutObject(S)==Packs[PackIndex].Guid) { Bound=S; break; }
				if (Shortcut)
				{
					Shortcut->SetVisibility(Bound!=INDEX_NONE ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
					if (Bound!=INDEX_NONE) SetIconDid(Shortcut,Bound<9 ? 0x0600109Eu+Bound : 0x060074D3u);
					PlacePack(Shortcut,OverlayZ+2);
				}
				if (Sel)
				{
					Sel->SetVisibility(bSelected
						? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
					if (bSelected)
					{
						SetIconDid(Sel, 0x06005D9C);
					}
					PlacePack(Sel, OverlayZ + 1, 0, 0, 36, 36);
				}
			}
			for (int32 i = MaxVisible + 1; i < PackSlots.Num(); ++i)
			{
				if (PackSlots[i]) { PackSlots[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
			for (int32 i = MaxVisible + 1; i < PackSlotBgs.Num(); ++i)
			{
				if (PackSlotBgs[i]) { PackSlotBgs[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
			for (int32 i=MaxVisible; i<PackCapacityMeters.Num(); ++i)
                if (PackCapacityMeters[i]) PackCapacityMeters[i]->SetVisibility(ESlateVisibility::Collapsed);
            for (int32 i=MaxVisible; i<PackItemSelected.Num(); ++i)
                if (PackItemSelected[i]) PackItemSelected[i]->SetVisibility(ESlateVisibility::Collapsed);
            for (int32 i=MaxVisible; i<PackShortcutIcons.Num(); ++i)
				if (PackShortcutIcons[i]) PackShortcutIcons[i]->SetVisibility(ESlateVisibility::Collapsed);
			for (int32 i = MaxVisible + 1; i < PackSlotSelected.Num(); ++i)
			{
				if (PackSlotSelected[i]) { PackSlotSelected[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
		}

		// Item grid over Inv_3DItemList (DAT 192×96 → 6×3 cells of 32×32).
		// Capacity is ItemsCapacity (main 102 / side packs 24); only PageSize cells are on-screen.
		// When the panel is resized taller, ReflowInventoryPanelGeometry grows the list height.
		TSharedPtr<FACEUIElement> GridEl = Manager->FindElementByName(TEXT("Inv_3DItemList"));
		if (GridEl.IsValid())
		{
			FACEWorldObject PackObj;
			Client->GetWorldObject(SelectedPackGuid, PackObj);
			TArray<FACEWorldObject> Items = Client->GetPackItems(SelectedPackGuid);
			{
				// Retail gmInventoryUI: "Contents of Backpack" for the main pack,
				// "Contents of %s" for sub-packs (acclient.c:222519/222536).
				const FString ContentsText = (SelectedPackGuid == Self || PackObj.Name.IsEmpty())
					? FString(TEXT("Contents of Backpack"))
					: FString::Printf(TEXT("Contents of %s"), *ACERetailObjectNames::Name(PackObj));
				if (!InvContentsLabel && Canvas->WidgetTree)
				{
					InvContentsLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(
						UACERetailTextBlock::StaticClass());
					InvContentsLabel->SetJustification(ETextJustify::Left);
				}
				PlaceTextOnElement(InvContentsLabel, TEXT("Inv_ContentsText"),
					ContentsText, 8, TextWhite, 10022);
			}
			constexpr int32 Cell = 32;
			const int32 Cols = FMath::Max(1, GridEl->Width / Cell);
			const int32 Rows = FMath::Max(1, GridEl->Height / Cell);
			const int32 PageSize = Cols * Rows;
			int32 Capacity = PackObj.ItemsCapacity;
			if (Capacity <= 0)
			{
				Capacity = (SelectedPackGuid == Self) ? 102 : 24;
			}
			// Show the pack's real capacity (filled + empty). Do not invent extra cells.
			Capacity = FMath::Clamp(Capacity, 1, 120);
			InventoryScrollOffset = FMath::Clamp(InventoryScrollOffset, 0, FMath::Max(0, Capacity - PageSize));
			InventoryScrollOffset -= InventoryScrollOffset % Cols;
			const FIntPoint Origin = GridEl->GetScreenOrigin();
			const float SX = Canvas->GetLastScaleX();
			const float SY = Canvas->GetLastScaleY();
			InventorySlotGuids.SetNum(PageSize);
			TArray<int32> SlotGuids;
			TArray<FACEWorldObject> SlotObjs;
			BuildPackSlotGuids(Items, Capacity, SlotGuids, SlotObjs);
			const int32 SelectedGuid = LastSelection.bValid ? LastSelection.Guid : 0;
			for (int32 i = 0; i < PageSize; ++i)
			{
				UBorder* Bg = EnsureIconBorder(InventorySlotBgs, i);
				UBorder* Icon = EnsureIconBorder(InventorySlots, i);
				UBorder* Overlay = EnsureIconBorder(InventorySlotOverlays, i);
				UBorder* Sel = EnsureIconBorder(InventorySlotSelected, i);
				if (!Bg || !Icon || !Overlay || !Sel) { continue; }
				const int32 SlotIndex = i + InventoryScrollOffset;
				const bool bInCapacity = SlotIndex < Capacity;
				const int32 Guid = (bInCapacity && SlotGuids.IsValidIndex(SlotIndex))
					? SlotGuids[SlotIndex] : 0;
				InventorySlotGuids[i] = Guid;
				// Visible (not HitTestInvisible) so slot geometry stays in the hit path for drag/reorder.
				Bg->SetVisibility(bInCapacity ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
				Icon->SetVisibility(bInCapacity ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
				if (!bInCapacity)
				{
					Overlay->SetVisibility(ESlateVisibility::Collapsed);
					Sel->SetVisibility(ESlateVisibility::Collapsed);
					while (InventorySlotCounts.Num() <= i)
					{
						InventorySlotCounts.Add(nullptr);
					}
					if (InventorySlotCounts[i])
					{
						InventorySlotCounts[i]->SetVisibility(ESlateVisibility::Collapsed);
					}
					continue;
				}
				const FACEWorldObject* SlotObj = (Guid != 0 && SlotObjs.IsValidIndex(SlotIndex))
					? &SlotObjs[SlotIndex] : nullptr;
				SetItemSlotBackground(Bg, SlotObj);
				SetItemSlotForeground(Icon, SlotObj);
				// ItemSlot (0x21000037), UI_ItemList_ShortcutOverlayArray;
				// UIElement_UIItem::SetShortcutNum uses the first bound shortcut.
				int32 Shortcut = INDEX_NONE;
				if (Guid != 0) for (int32 S = 0; S < 18; ++S)
				{
					if (Client->GetShortcutObject(S) == Guid) { Shortcut = S; break; }
				}
				const bool bHasOverlay = Shortcut != INDEX_NONE;
				Overlay->SetVisibility(bHasOverlay
					? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
				if (bHasOverlay)
				{
					SetIconDid(Overlay, Shortcut < 9 ? 0x0600109Eu + Shortcut : 0x060074D3u);
				}
				const bool bSelected = Guid != 0 && Guid == SelectedGuid;
				Sel->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
				if (bSelected)
				{
					SetIconDid(Sel, DidInvSlotSelected);
				}
				if (SlotObj)
				{
					SetRetailTooltip(Icon, FText::FromString(FormatItemStackName(*SlotObj)));
				}
				else
				{
					SetRetailTooltip(Icon, FText());
				}
				const int32 Col = i % Cols;
				const int32 Row = i / Cols;
				const FVector2D Pos(
					static_cast<float>(Origin.X + Col * Cell) * SX,
					static_cast<float>(Origin.Y + Row * Cell) * SY);
				const FVector2D Size(static_cast<float>(Cell) * SX, static_cast<float>(Cell) * SY);
				auto Place = [&](UBorder* B, int32 Z)
				{
					if (B->GetParent() != Canvas->GetElementLayer())
					{
						Canvas->GetElementLayer()->AddChild(B);
					}
					if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(B->Slot))
					{
						Slot->SetAnchors(FAnchors(0.f, 0.f));
						Slot->SetPosition(Pos);
						Slot->SetSize(Size);
						Canvas->SetOverlayOrder(B, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), Z);
					}
				};
				Place(Bg, OverlayZ);
				Place(Icon, OverlayZ + 1);
				Place(Overlay, OverlayZ + 2);
				Place(Sel, OverlayZ + 3);

				// Retail UIItem quantity overlay (UpdateQuantityDisplay: L"%d"):
				// stack counts render as a numeral on the icon's lower-right corner.
				while (InventorySlotCounts.Num() <= i)
				{
					InventorySlotCounts.Add(nullptr);
				}
				const bool bShowCount = SlotObj && SlotObj->StackSize > 1;
				if (bShowCount && !InventorySlotCounts[i] && Canvas->WidgetTree)
				{
					UTextBlock* NewCount = Canvas->WidgetTree->ConstructWidget<UTextBlock>(
						UACERetailTextBlock::StaticClass());
					NewCount->SetJustification(ETextJustify::Right);
					NewCount->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 7));
					NewCount->SetColorAndOpacity(FSlateColor(FLinearColor::White));
					NewCount->SetShadowOffset(FVector2D(1.f, 1.f));
					NewCount->SetShadowColorAndOpacity(FLinearColor::Black);
					InventorySlotCounts[i] = NewCount;
				}
				UTextBlock* Count = InventorySlotCounts[i];
				if (Count)
				{
					if (bShowCount)
					{
						Count->SetText(FText::AsNumber(SlotObj->StackSize));
						Count->SetVisibility(ESlateVisibility::HitTestInvisible);
						if (Count->GetParent() != Canvas->GetElementLayer())
						{
							Canvas->GetElementLayer()->AddChild(Count);
						}
						if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Count->Slot))
						{
							Slot->SetAnchors(FAnchors(0.f, 0.f));
							Slot->SetPosition(FVector2D(Pos.X + 1.f, Pos.Y + Size.Y - 11.f * SY));
							Slot->SetSize(FVector2D(Size.X - 3.f, 11.f * SY));
							Canvas->SetOverlayOrder(Count, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), OverlayZ + 4);
						}
					}
					else
					{
						Count->SetVisibility(ESlateVisibility::Collapsed);
					}
				}
			}
			for (int32 i = PageSize; i < InventorySlots.Num(); ++i)
			{
				if (InventorySlots[i]) { InventorySlots[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
			for (int32 i = PageSize; i < InventorySlotBgs.Num(); ++i)
			{
				if (InventorySlotBgs[i]) { InventorySlotBgs[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
			for (int32 i = PageSize; i < InventorySlotOverlays.Num(); ++i)
			{
				if (InventorySlotOverlays[i]) { InventorySlotOverlays[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
			for (int32 i = PageSize; i < InventorySlotSelected.Num(); ++i)
			{
				if (InventorySlotSelected[i]) { InventorySlotSelected[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
			for (int32 i = PageSize; i < InventorySlotCounts.Num(); ++i)
			{
				if (InventorySlotCounts[i]) { InventorySlotCounts[i]->SetVisibility(ESlateVisibility::Collapsed); }
			}
		}
		SyncInventoryScrollbars();
		}
	}

	// Chat overlays — ONLY MainChat (Chat1–4 also contain ChatLog / ChatPanelTextEntry).
	if (ChatLog && ChatEntry && Manager && Canvas)
	{
		constexpr TCHAR MainChat[] = TEXT("RootGameplay_FloatyMainChat_Field");
		TSharedPtr<FACEUIElement> ChatRoot = Manager->FindElementByName(MainChat);
		TSharedPtr<FACEUIElement> LogEl = Manager->FindElementUnder(MainChat, TEXT("ChatLog"));
		TSharedPtr<FACEUIElement> EntryEl = Manager->FindElementUnder(MainChat, TEXT("ChatPanelTextEntry"));
		const bool bChatOn = ChatRoot.IsValid() && ChatRoot->bVisible;
		if (bChatOn && LogEl.IsValid())
		{
			ChatLog->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
			// DAT already reserves space for the numbered window controls.
			Canvas->PlaceWidgetAtElement(ChatLog, LogEl, 530, LogEl->TextMargins);
			RefreshChatRowLayout(0);
		}
		else if (ChatLog)
		{
			ChatLog->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (bChatOn && EntryEl.IsValid())
		{
			ChatEntry->SetVisibility(ESlateVisibility::Visible);
			Canvas->PlaceWidgetAtElement(ChatEntry, EntryEl, 531);
		}
		else if (ChatEntry)
		{
			ChatEntry->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (bChatOn)
		{
			SyncChatScrollbar();
		}
	}

	// FloatyChat1-4 logs / entries / titles follow their own DAT windows.
	PlaceFloatyChatOverlays();
}

bool UACEUIGameplayBinder::IsScrollbarDragActive() const
{
	return ScrollDragTarget != EACEUIScrollTarget::None;
}

bool UACEUIGameplayBinder::TryBeginScrollbarDrag(FVector2D ViewportPos)
{
	const FVector2D CanvasLocalPos = Canvas ? Canvas->ViewportToLayout(ViewportPos) : ViewportPos;
	// Selected-item stack amount slider (toolbar SelectedObjectField).
	if (Manager && SelectedStackMax > 1)
	{
		if (TSharedPtr<FACEUIElement> Slider = Manager->FindElementByName(TEXT("StackSizeSlider")))
		{
			if (Slider->bVisible && Canvas && Canvas->IsElementExposedAt(Slider, ViewportPos))
			{
				const FIntPoint O = Slider->GetScreenOrigin();
				if (CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
					&& CanvasLocalPos.X < O.X + Slider->Width
					&& CanvasLocalPos.Y < O.Y + Slider->Height)
				{
					ScrollDragBar.Reset();
					const auto Thumb = Slider->Children.Num() > 1 ? Slider->Children[1] : nullptr;
					const int32 ThumbWidth = Thumb ? Thumb->Width : 16;
					const float LocalX = CanvasLocalPos.X - O.X;
					const bool bOnThumb = Thumb && LocalX >= Thumb->X && LocalX < Thumb->X + ThumbWidth;
					ScrollDragLastOffset = INDEX_NONE;
					ScrollDragTarget = EACEUIScrollTarget::StackSize;
					ScrollDragMaxOff = SelectedStackMax;
					bScrollDragHorizontal = true;
					ScrollDragTrackOrigin = O.X;
					ScrollDragTravel = FMath::Max(1, Slider->Width - ThumbWidth);
					// Retail moves the touched point to the thumb center; grabbing the thumb
					// preserves its fractional position and the selected amount until dragged.
					ScrollDragFraction = bOnThumb ? float(SelectedStackAmount) / SelectedStackMax
						: FMath::Clamp((LocalX - ThumbWidth * .5f) / FMath::Max(1, ScrollDragTravel - 1), 0.f, 1.f);
					ScrollDragGrabOffset = LocalX - ScrollDragFraction * ScrollDragTravel;
					if (bOnThumb) ScrollDragLastOffset = SelectedStackAmount;
					else UpdateScrollbarDrag(ViewportPos);
					return true;
				}
			}
		}
	}

	auto TryBar = [&](const TSharedPtr<FACEUIElement>& Bar, EACEUIScrollTarget Target,
		int32 MaxOff, bool bHorizontal) -> bool
	{
		if (!Bar.IsValid() || !Bar->bVisible || MaxOff <= 0
			|| !Canvas || !Canvas->IsElementExposedAt(Bar, ViewportPos))
		{
			return false;
		}
		TSharedPtr<FACEUIElement> Thumb;
		for (const TSharedPtr<FACEUIElement>& Child : Bar->Children)
		{
			if (!Child.IsValid()) { continue; }
			if (Child->Type == ACEUI::ElementType::Scrollbar || Child->ElementName.IsEmpty()
				|| Child->ElementName.Contains(TEXT("widget_")))
			{
				Thumb = Child;
				if (Child->Type == ACEUI::ElementType::Scrollbar || Child->ElementName.IsEmpty())
				{
					break;
				}
			}
		}
		const FIntPoint BarO = Bar->GetScreenOrigin();
		constexpr int32 Btn = 16;
		const bool bOverBar = CanvasLocalPos.X >= BarO.X && CanvasLocalPos.Y >= BarO.Y
			&& CanvasLocalPos.X < BarO.X + Bar->Width && CanvasLocalPos.Y < BarO.Y + Bar->Height;
		if (!bOverBar)
		{
			return false;
		}
		// Arrow buttons use the element manager's DAT hot-button repeat behavior.
		if (bHorizontal)
		{
			if (CanvasLocalPos.X < BarO.X + Btn || CanvasLocalPos.X >= BarO.X + Bar->Width - Btn)
			{
				return false;
			}
		}
		else if (CanvasLocalPos.Y < BarO.Y + Btn || CanvasLocalPos.Y >= BarO.Y + Bar->Height - Btn)
		{
			return false;
		}

		ScrollDragTarget = Target;
		ScrollDragMaxOff = MaxOff;
		bScrollDragHorizontal = bHorizontal;
		if (bHorizontal)
		{
			const int32 ThumbW = Thumb.IsValid() ? Thumb->Width : 8;
			ScrollDragTrackOrigin = BarO.X + Btn;
			ScrollDragTravel = FMath::Max(1, Bar->Width - Btn * 2 - ThumbW);
		}
		else
		{
			const int32 ThumbH = Thumb.IsValid() ? Thumb->Height : 8;
			ScrollDragTrackOrigin = BarO.Y + Btn;
			ScrollDragTravel = FMath::Max(1, Bar->Height - Btn * 2 - ThumbH);
		}
		ScrollDragBar = Bar;
		ScrollDragLastOffset = INDEX_NONE;
		const float Cursor = bHorizontal ? CanvasLocalPos.X : CanvasLocalPos.Y;
		const float ThumbStart = Thumb ? (bHorizontal ? Thumb->GetScreenOrigin().X : Thumb->GetScreenOrigin().Y) : Cursor;
		const float ThumbSize = Thumb ? (bHorizontal ? Thumb->Width : Thumb->Height) : 8.f;
		ScrollDragGrabOffset = Cursor >= ThumbStart && Cursor <= ThumbStart + ThumbSize ? Cursor - ThumbStart : ThumbSize * .5f;
		ScrollDragVisibleFraction = ThumbSize / FMath::Max(1.f, float((bHorizontal ? Bar->Width : Bar->Height) - Btn*2));
		UpdateScrollbarDrag(ViewportPos);
		return true;
	};

	if(OpenHouseLord && Client && Client->GetSession())
	{
		const FString Prefix=bHouseRentTab?TEXT("HouseMaintenance"):TEXT("HouseBuy");
		const auto List=Manager->FindElementByName(Prefix+TEXT("ItemsList"));
		if(List && TryBar(Manager->FindElementByName(Prefix+TEXT("_ItemListScroll")),EACEUIScrollTarget::HousePayment,
			FMath::Max(0,Client->GetSession()->GetHousePaymentItems(bHouseRentTab).Num()-FMath::Max(1,List->Width/32)),true)) return true;
	}
	if(OpenSalvageToolGuid && TryBar(Manager->FindElementByName(TEXT("Salvage_ItemListScroll")),EACEUIScrollTarget::Salvage,
		FMath::Max(0,SalvageQueueGuids.Num()+1-SalvageVisibleSlots),true))return true;
	if (ActivePanelPage==TEXT("SocialPanel_Field") && ActiveSocialTab==TEXT("AllegiancePage") && Client)
		if (TryBar(Manager->FindElementUnder(TEXT("AllegiancePage"),TEXT("VassalsListBoxScrollbar")),
			EACEUIScrollTarget::Allegiance,FMath::Max(0,Client->GetAllegiance().Vassals.Num()-VassalVisibleRows),false)) return true;
	if (ActivePanelPage == TEXT("SocialPanel_Field") && ActiveSocialTab == TEXT("FellowshipPage") && Client)
		if (TryBar(Manager->FindElementUnder(TEXT("FellowshipPage"),TEXT("FellowsListBoxScrollbar")),
			EACEUIScrollTarget::Fellowship,FMath::Max(0,Client->GetFellowship().Members.Num()-FellowVisibleRows),false)) return true;
	if (ActivePanelPage == TEXT("SocialPanel_Field") && ActiveSocialTab == TEXT("FriendsPage") && Client)
		if (TryBar(Manager->FindElementUnder(TEXT("FriendsPage"),TEXT("FreindsListBoxScrollbar")),
			EACEUIScrollTarget::Friends,FMath::Max(0,Client->GetFriends().Num()-FriendVisibleRows),false)) return true;
	if (ActivePanelPage == TEXT("SocialPanel_Field") && ActiveSocialTab == TEXT("SquelchPage") && Client)
		if (TryBar(Manager->FindElementUnder(TEXT("SquelchPage"),TEXT("SquelchListBoxScrollbar")),
			EACEUIScrollTarget::Squelch,FMath::Max(0,Client->GetSquelches().Num()-SquelchVisibleRows),false)) return true;
	if (const auto KeyboardRoot=Manager->FindElementByName(TEXT("RootGameplay_Keyboard_Field"));KeyboardRoot && KeyboardRoot->bVisible && !bKeymapImportOpen)
		if(TryBar(Manager->FindElementUnder(ActiveKeyboardPage+TEXT("Page"),TEXT("KeyboardMappingScrollbar")),
			EACEUIScrollTarget::Keyboard,KeyboardMaxOffset,false))return true;
	if (ActivePanelPage == TEXT("QuestManagementPanel_Field") && ActiveQuestTab == TEXT("PageListPage"))
	{
		auto List=Manager->FindElementUnder(ActiveQuestTab,TEXT("PageListBox"));
		if (List && TryBar(Manager->FindElementUnder(ActiveQuestTab,TEXT("PageListBoxScrollbar")),
			EACEUIScrollTarget::JournalList,FMath::Max(0,JournalFilteredCount-List->Height/20),false)) return true;
	}
	if (ActivePanelPage == TEXT("QuestManagementPanel_Field") && ActiveQuestTab == TEXT("JournalPage") && JournalNotes)
		if(TryBar(Manager->FindElementUnder(ActiveQuestTab,TEXT("NotesScrollbar")),EACEUIScrollTarget::JournalNotes,
			FMath::CeilToInt(JournalNotes->GetScrollOffsetOfEnd()),false))return true;
	if (ActivePanelPage == TEXT("SpellManagementPanel_Field") && ActiveSpellPanelTab == TEXT("SpellComponentPage"))
	{
		const auto List = Manager->FindElementUnder(ActiveSpellPanelTab, TEXT("SpellComponents_ComponentList"));
		TArray<FACEComponentRow> Rows; BuildComponentRowList(Rows);
		if (List && TryBar(Manager->FindElementUnder(ActiveSpellPanelTab, TEXT("SpellComponents_ComponentList_Scrollbar")),
			EACEUIScrollTarget::Components, FMath::Max(0, Rows.Num() - List->Height/32), false)) return true;
	}
	if (bTradeOpen && Client)
	{
		for (bool bSelf : {true,false})
		{
			const auto List=Manager->FindElementByName(bSelf ? TEXT("TradeSelfItemsList") : TEXT("TradeOtherItemsList"));
			const auto Bar=Manager->FindElementByName(bSelf ? TEXT("TradeSelf_ItemListScroll") : TEXT("TradeOther_ItemListScroll"));
			const int32 Capacity=bSelf ? Client->GetTradeSelfItems().Num()+1 : Client->GetTradePartnerItems().Num();
			if (List && TryBar(Bar,bSelf ? EACEUIScrollTarget::TradeSelf : EACEUIScrollTarget::TradeOther,
				FMath::Max(0,Capacity-FMath::Max(1,List->Width/32)),true)) return true;
		}
	}
	if (ActivePanelPage == TEXT("BookPanel_Field") && BookScroll)
		if (TryBar(GetBookScrollbar(), EACEUIScrollTarget::Book,
			FMath::CeilToInt(BookPageEditor && BookPageEditor->IsVisible() ? BookPageEditor->GetScrollOffsetOfEnd()
				: BookScroll->GetScrollOffsetOfEnd()), false)) return true;
	if (ActivePanelPage == TEXT("CharacterInfoPanel_Field") && CharacterInfoScroll)
		if (TryBar(Manager->FindElementUnder(ActivePanelPage,TEXT("CharacterInfoText_Scrollbar")),
			EACEUIScrollTarget::CharacterInfo,FMath::CeilToInt(CharacterInfoScroll->GetScrollOffsetOfEnd()),false)) return true;
	if (ExamScroll && ExamScroll->GetVisibility() != ESlateVisibility::Collapsed)
	{
		if (TryBar(Manager->FindElementUnder(ExaminedSpellId ? TEXT("SpellExamineUI") : TEXT("ItemExamineUI"),
			ExaminedSpellId ? TEXT("SpellDisplayTextScrollbar") : TEXT("ItemDisplayTextScrollbar")),
			EACEUIScrollTarget::Examination, FMath::CeilToInt(ExamScroll->GetScrollOffsetOfEnd()), false)) return true;
	if (ExamInscriptionScroll && ExamInscriptionScroll->GetVisibility()!=ESlateVisibility::Collapsed)
		if(TryBar(Manager->FindElementUnder(TEXT("ItemExamineUI"),TEXT("ItemInscriptionScrollbar")),
			EACEUIScrollTarget::Inscription,FMath::CeilToInt(ExamInscriptionScroll->GetScrollOffsetOfEnd()),false))return true;
	}

	if (ActivePanelPage == TEXT("SpellManagementPanel_Field") && ActiveSpellPanelTab == TEXT("SpellbookPage"))
	{
		TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(TEXT("SpellbookPage"), TEXT("SpellBook_SpellList"));
		TSharedPtr<FACEUIElement> Bar = Manager->FindElementUnder(TEXT("SpellbookPage"), TEXT("SpellBook_SpellList_Scrollbar"));
		const int32 PageSize = ListEl.IsValid() ? FMath::Max(1, ListEl->Height / SpellbookTemplate().Height()) : 1;
		if (TryBar(Bar, EACEUIScrollTarget::Spellbook, FMath::Max(0, SpellbookFilteredCount - PageSize), false))
		{
			return true;
		}
	}
	if (ActivePanelPage == TEXT("InventoryPanel_Field"))
	{
		TSharedPtr<FACEUIElement> GridEl = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList"));
		TSharedPtr<FACEUIElement> ItemBar = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList_Scrollbar"));
		if (GridEl.IsValid())
		{
			constexpr int32 Cell = 32;
			const int32 PageSize = FMath::Max(1, GridEl->Width / Cell) * FMath::Max(1, GridEl->Height / Cell);
			FACEWorldObject PackObj;
			int32 Capacity = PageSize;
			if (Client && Client->GetWorldObject(SelectedPackGuid, PackObj) && PackObj.ItemsCapacity > 0)
			{
				Capacity = PackObj.ItemsCapacity;
			}
			if (TryBar(ItemBar, EACEUIScrollTarget::InvGrid, FMath::Max(0, Capacity - PageSize), false))
			{
				return true;
			}
		}
		TSharedPtr<FACEUIElement> PackList = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("Inv_ContainerList"));
		TSharedPtr<FACEUIElement> PackBar = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("Inv_ContainerList_Scrollbar"));
		if (PackList.IsValid() && Client)
		{
			FACEWorldObject SelfObj;
			Client->GetWorldObject(Client->GetPlayerGuid(), SelfObj);
			const int32 Side = FMath::Clamp(SelfObj.ContainersCapacity > 0 ? SelfObj.ContainersCapacity : 7, 1, 24);
			const int32 MaxVis = FMath::Max(1, PackList->Height / 36);
			if (TryBar(PackBar, EACEUIScrollTarget::PackList, FMath::Max(0, Side - MaxVis), false))
			{
				return true;
			}
		}
	}
	if (OpenLootContainerGuid != 0)
	{
		TSharedPtr<FACEUIElement> ItemBar = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Ext_ItemListScroll"));
		TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Ext_Container_ItemList"));
		const int32 PageSize = ListEl.IsValid() ? FMath::Max(1, ListEl->Width / 32) : 12;
		if (TryBar(ItemBar, EACEUIScrollTarget::ExtItems, FMath::Max(0, GetExternalContainerItemCapacity() - PageSize), true))
		{
			return true;
		}
	}
	if (ActivePanelPage == TEXT("SkillManagementPanel_Field") && ActiveSkillTab == TEXT("CharacterTitlePage"))
	{
		const auto List = Manager->FindElementUnder(ActiveSkillTab, TEXT("CharacterTitle_ListBox"));
		const auto Bar = Manager->FindElementUnder(ActiveSkillTab, TEXT("CharacterTitle_ListBox_Scrollbar"));
		const int32 Rows = List ? FMath::Max(1, List->Height / 24) : 1;
		if (TryBar(Bar, EACEUIScrollTarget::TitleList, FMath::Max(0, SortedTitleIds.Num() - Rows), false)) return true;
	}
	if (ActivePanelPage == TEXT("SkillManagementPanel_Field") && ActiveSkillTab == TEXT("SkillPage"))
	{
		TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(TEXT("SkillPage"), TEXT("StatManagement_List"));
		TSharedPtr<FACEUIElement> Bar = Manager->FindElementUnder(TEXT("SkillPage"), TEXT("StatManagement_List_Scrollbar"));
		const int32 PageSize = ListEl.IsValid() ? FMath::Max(1, ListEl->Height / 18) : 1;
		if (TryBar(Bar, EACEUIScrollTarget::StatList, FMath::Max(0, SkillListContentCount - PageSize), false))
		{
			return true;
		}
	}
	if (ActivePanelPage == TEXT("SkillManagementPanel_Field") && ActiveSkillTab == TEXT("AttributePage"))
	{
		const auto List = Manager->FindElementUnder(ActiveSkillTab, TEXT("StatManagement_List"));
		if (List && TryBar(Manager->FindElementUnder(ActiveSkillTab, TEXT("StatManagement_List_Scrollbar")),
			EACEUIScrollTarget::Attributes, FMath::Max(0, 9-FMath::Max(1,List->Height/18)), false)) return true;
	}
	if (ActivePanelPage == TEXT("PositiveEffectsPanel_Field")
		|| ActivePanelPage == TEXT("NegativeEffectsPanel_Field"))
	{
		TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(ActivePanelPage, TEXT("Effects_SpellList"));
		TSharedPtr<FACEUIElement> Bar = Manager->FindElementUnder(ActivePanelPage, TEXT("Effects_SpellList_Scrollbar"));
		const int32 PageSize = ListEl.IsValid() ? FMath::Max(1, ListEl->Height / 32) : 10;
		if (TryBar(Bar, EACEUIScrollTarget::Effects, FMath::Max(0, EffectsContentCount - PageSize), false))
		{
			return true;
		}
		if (EffectsInfoScroll && TryBar(Manager->FindElementUnder(ActivePanelPage,TEXT("Effects_InfoText_Scrollbar")),
			EACEUIScrollTarget::EffectsInfo,FMath::CeilToInt(EffectsInfoScroll->GetScrollOffsetOfEnd()),false)) return true;
	}
	if (ActivePanelPage == TEXT("OptionsPanel_Field") && ActiveOptionsPage() != INDEX_NONE)
	{
		TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(ActiveOptionsTab, ActiveOptionsListName());
		const TCHAR* BarName = (ActiveOptionsTab == TEXT("CharacterSettingsPage"))
			? TEXT("CharacterOptionsListBoxScrollbar") : TEXT("OptionsListBoxScrollbar");
		TSharedPtr<FACEUIElement> Bar = Manager->FindElementUnder(ActiveOptionsTab, BarName);
		TArray<const FACECharacterOptionDesc*> PageOptions;
		ACECharacterOptions::GetPageOptions(static_cast<uint8>(ActiveOptionsPage()), PageOptions);
		const int32 PageSize = ListEl.IsValid() ? FMath::Max(1, ListEl->Height / 16) : 16;
		if (TryBar(Bar, EACEUIScrollTarget::OptionsList,
			ActiveOptionsTab == TEXT("ConfigPage") && VideoSettings
				? FMath::CeilToInt(CastChecked<UACEVideoSettingsWidget>(VideoSettings)->GetScrollEnd())
				: FMath::Max(0, GetOptionsRowCount() - PageSize), false))
		{
			return true;
		}
	}
    for (int32 W=0; W<=NumFloatyChats; ++W)
    {
        auto* Log=GetChatLogWidget(W);
        if (!Log || Log->GetVisibility()==ESlateVisibility::Collapsed) continue;
        const FString Root=W ? FString::Printf(TEXT("RootGameplay_FloatyChat%d_Field"),W) : TEXT("RootGameplay_FloatyMainChat_Field");
        ScrollDragChatWindow=W;
        if (TryBar(Manager->FindElementUnder(Root,TEXT("ChatLogScrollbar")),EACEUIScrollTarget::Chat,
            FMath::Max(0,FMath::CeilToInt(Log->GetScrollOffsetOfEnd())),false)) return true;
    }
	if (OpenVendorGuid != 0 && Manager)
	{
		const TCHAR* ScrollName = TEXT("Vendor_ItemListScroll");
		EACEUIScrollTarget VendorTarget = EACEUIScrollTarget::VendorItems;
		int32 Offset = VendorItemsScrollOffset;
		if (ActiveVendorPage == 1)
		{
			ScrollName = TEXT("Vendor_BuyListScroll");
			VendorTarget = EACEUIScrollTarget::VendorBuy;
			Offset = VendorBuyScrollOffset;
		}
		else if (ActiveVendorPage == 2)
		{
			ScrollName = TEXT("Vendor_SellListScroll");
			VendorTarget = EACEUIScrollTarget::VendorSell;
			Offset = VendorSellScrollOffset;
		}
		(void)Offset;
		TSharedPtr<FACEUIElement> VendorBar = Manager->FindElementByName(ScrollName);
		constexpr int32 Cell = 32;
		TSharedPtr<FACEUIElement> ListEl;
		if (ActiveVendorPage == 0)
		{
			ListEl = Manager->FindElementUnder(TEXT("VendorItemsPage"), TEXT("VendorItemsList"));
		}
		else if (ActiveVendorPage == 1)
		{
			ListEl = Manager->FindElementUnder(TEXT("VendorBuyPage"), TEXT("VendorBuyList"));
		}
		else
		{
			ListEl = Manager->FindElementUnder(TEXT("VendorSellPage"), TEXT("VendorSellList"));
		}
		int32 Capacity = 0;
		if (Client)
		{
			if (ActiveVendorPage == 2) { Capacity = VendorSellCart.Num(); }
			else if (ActiveVendorPage == 1) { Capacity = VendorBuyCart.Num(); }
			else
			{
				Capacity = Client->GetVendorMerchandise().Num();
				if (Capacity == 0) { Capacity = Client->GetPackItems(OpenVendorGuid).Num(); }
			}
		}
		const int32 PageSize = ListEl.IsValid()
			? FMath::Max(1, (ListEl->Width / Cell) * FMath::Max(1, ListEl->Height / Cell))
			: 1;
		if (TryBar(VendorBar, VendorTarget, FMath::Max(0, Capacity - PageSize), true))
		{
			return true;
		}
	}
	return false;
}

void UACEUIGameplayBinder::UpdateScrollbarDrag(FVector2D ViewportPos)
{
	const FVector2D CanvasLocalPos = Canvas ? Canvas->ViewportToLayout(ViewportPos) : ViewportPos;
	if (ScrollDragTarget == EACEUIScrollTarget::None || ScrollDragTravel <= 0)
	{
		return;
	}
	const float Pos = bScrollDragHorizontal ? CanvasLocalPos.X : CanvasLocalPos.Y;
	const float T = FMath::Clamp((Pos - ScrollDragGrabOffset - static_cast<float>(ScrollDragTrackOrigin))
		/ static_cast<float>(ScrollDragTravel), 0.f, 1.f);
	if (ScrollDragTarget == EACEUIScrollTarget::StackSize && ScrollDragLastOffset != INDEX_NONE
		&& FMath::IsNearlyEqual(T, ScrollDragFraction)) return;
	ScrollDragFraction = T;
	int32 Off = FMath::RoundToInt(T * static_cast<float>(ScrollDragMaxOff));
	if (ScrollDragTarget == EACEUIScrollTarget::StackSize)
	{
		// UIElement_Scrollbar sends integer thousandths; gmToolbarUI maps these to 1..max.
		const int32 Thousandths = FMath::FloorToInt(double(T) * 1000.0);
		Off = int32(FMath::Clamp<int64>(1 + int64(Thousandths) * SelectedStackMax / 1000, 1, FMath::Max(1, SelectedStackMax)));
	}
	if (ScrollDragTarget == EACEUIScrollTarget::InvGrid)
		if (const auto Grid = Manager->FindElementUnder(TEXT("InventoryPanel_Field"), TEXT("Inv_3DItemList")))
			Off -= Off % FMath::Max(1, Grid->Width/32);
	if (const auto Bar = ScrollDragBar.Pin()) SyncDatScrollbar(Bar, T, ScrollDragVisibleFraction);
	if (Off == ScrollDragLastOffset && ScrollDragTarget != EACEUIScrollTarget::Chat) return;
	ScrollDragLastOffset = Off;
	switch (ScrollDragTarget)
	{
	case EACEUIScrollTarget::Components:
		ComponentScrollOffset = Off; RefreshComponentOverlays(); break;
	case EACEUIScrollTarget::Allegiance:
		VassalScrollOffset=Off; RefreshAllegianceOverlays(); break;
	case EACEUIScrollTarget::Fellowship:
		FellowScrollOffset=Off;RefreshFellowshipOverlays();break;
	case EACEUIScrollTarget::Friends:
		FriendScrollOffset=Off;RefreshFriendsOverlays();break;
	case EACEUIScrollTarget::Squelch:
		SquelchScrollOffset=Off;RefreshSquelchOverlays();break;
	case EACEUIScrollTarget::Keyboard:
		KeyboardScrollOffset=Off;RefreshKeyboardOverlays();break;
	case EACEUIScrollTarget::JournalList:
		JournalScrollOffset=Off; RefreshQuestOverlays(); break;
	case EACEUIScrollTarget::JournalNotes:
		if(JournalNotes)JournalNotes->SetScrollOffset(Off);
		break;
	case EACEUIScrollTarget::CharacterInfo:
		if (CharacterInfoScroll) CharacterInfoScroll->SetScrollOffset(Off);
		break;
	case EACEUIScrollTarget::Salvage:
		SetSalvageScrollOffset(Off);
		break;
	case EACEUIScrollTarget::HousePayment:
		HouseItemOffset=Off; RefreshHouseOffer(); break;
	case EACEUIScrollTarget::Book:
		if (BookPageEditor && BookPageEditor->IsVisible()) BookPageEditor->SetScrollOffset(Off);
		else if (BookScroll) BookScroll->SetScrollOffset(Off);
		break;
	case EACEUIScrollTarget::Examination:
		if (ExamScroll) ExamScroll->SetScrollOffset(Off);
		break;
	case EACEUIScrollTarget::Inscription:
		if (ExamInscriptionScroll) ExamInscriptionScroll->SetScrollOffset(Off);
		break;

	case EACEUIScrollTarget::Spellbook:
		SpellbookScrollOffset = Off;
		RefreshSpellbookOverlays();
		SyncSpellbookScrollbar();
		break;
	case EACEUIScrollTarget::InvGrid:
		InventoryScrollOffset = Off;
		RefreshInventoryOverlays();
		SyncInventoryScrollbars();
		break;
	case EACEUIScrollTarget::PackList:
		PackScrollOffset = Off;
		RefreshInventoryOverlays();
		SyncInventoryScrollbars();
		break;
	case EACEUIScrollTarget::ExtItems:
		ExtItemScrollOffset = Off;
		RefreshExternalContainerOverlays();
		SyncExternalContainerScrollbars();
		break;
	case EACEUIScrollTarget::ExtPacks:
		ExtPackScrollOffset = Off;
		RefreshExternalContainerOverlays();
		SyncExternalContainerScrollbars();
		break;
	case EACEUIScrollTarget::StatList:
		SkillListScrollOffset = Off;
		RefreshSkillOverlays();
		SyncStatListScrollbar();
		break;
	case EACEUIScrollTarget::Attributes:
		AttributeScrollOffset = Off;
		RefreshAttributeOverlays();
		break;
	case EACEUIScrollTarget::TitleList:
		TitleListScrollOffset = Off;
		RefreshTitleOverlays();
		break;
	case EACEUIScrollTarget::Effects:
		EffectsScrollOffset = Off;
		RefreshEffectsOverlays(ActivePanelPage == TEXT("PositiveEffectsPanel_Field"));
		break;
	case EACEUIScrollTarget::EffectsInfo:
		if (EffectsInfoScroll) EffectsInfoScroll->SetScrollOffset(Off);
		RefreshEffectsOverlays(ActivePanelPage == TEXT("PositiveEffectsPanel_Field"));
		break;
	case EACEUIScrollTarget::OptionsList:
		if (ActiveOptionsTab == TEXT("ConfigPage"))
		{
			if (auto* Video = Cast<UACEVideoSettingsWidget>(VideoSettings)) Video->SetScrollOffset(T*ScrollDragMaxOff);
			SyncOptionsScrollbar();
			break;
		}
		OptionsScrollOffset = Off;
		RefreshOptionsOverlays();
		break;
	case EACEUIScrollTarget::StackSize:
		{
			SelectedStackAmount = Off;
			RefreshSelectionOverlay();
			RefreshVendorOverlays();
			break;
		}
    case EACEUIScrollTarget::Chat:
        if (auto* Log=GetChatLogWidget(ScrollDragChatWindow))
            ScrollChatLog(T*Log->GetScrollOffsetOfEnd()-Log->GetScrollOffset(),ScrollDragChatWindow);
        break;
	case EACEUIScrollTarget::TradeSelf:
		TradeSelfOffset=Off; RefreshTradeOverlays(); break;
	case EACEUIScrollTarget::TradeOther:
		TradeOtherOffset=Off; RefreshTradeOverlays(); break;
	case EACEUIScrollTarget::VendorItems:
		VendorItemsScrollOffset = Off;
		RefreshVendorOverlays();
		SyncVendorScrollbars();
		break;
	case EACEUIScrollTarget::VendorBuy:
		VendorBuyScrollOffset = Off;
		RefreshVendorOverlays();
		SyncVendorScrollbars();
		break;
	case EACEUIScrollTarget::VendorSell:
		VendorSellScrollOffset = Off;
		RefreshVendorOverlays();
		SyncVendorScrollbars();
		break;
	default:
		break;
	}
}

bool UACEUIGameplayBinder::TryFinishScrollbarDrag()
{
	if (ScrollDragTarget == EACEUIScrollTarget::None)
	{
		return false;
	}
	ScrollDragTarget = EACEUIScrollTarget::None;
	ScrollDragBar.Reset();
	ScrollDragGrabOffset = 0.f;
	ScrollDragLastOffset = INDEX_NONE;
	return true;
}

bool UACEUIGameplayBinder::IsBeyondContainerUseRadius(int32 ContainerGuid) const
{
	if (!Client || ContainerGuid == 0)
	{
		return true;
	}
	FACEWorldObject Cont;
	if (!Client->GetWorldObject(ContainerGuid, Cont) || !Cont.bHasPosition)
	{
		// Missing position — don't force-close (may still be streaming).
		return false;
	}
	const FACEPosition PlayerPos = Client->GetPlayerPosition();
	if (!PlayerPos.IsValid())
	{
		return false;
	}
	// Interact uses Setup cylinder gap ≤ UseRadius. Auto-close must sit strictly outside
	// that band — center Dist2D was overlapping interact range and closed loot on open.
	const float UseR = Cont.UseRadius > 0.f ? Cont.UseRadius : 0.5f;
	constexpr float CloseHysteresisAc = 2.0f;
	const float CloseLimitAc = UseR + CloseHysteresisAc;

	if (AACEPlayerController* PC = PlayerController.Get())
	{
		const float Scale = FMath::Max(1.f, PC->WorldScale);
		const FVector SelfCm = PlayerPos.ToUnrealLocation(Scale);
		const FVector ContCm = Cont.Position.ToUnrealLocation(Scale);
		const float GapAc = PC->GetUseCylinderDistanceCm(Cont, SelfCm, ContCm) / Scale;
		return GapAc > CloseLimitAc;
	}

	const FVector P = PlayerPos.ToUnrealLocation(1.f);
	const FVector C = Cont.Position.ToUnrealLocation(1.f);
	const float Dist2D = FVector2D(P.X - C.X, P.Y - C.Y).Size();
	// Rough radii so fallback still clears interact range.
	return Dist2D > (CloseLimitAc + 1.0f);
}

void UACEUIGameplayBinder::TickEnvPanelRangeChecks()
{
	if (!Client)
	{
		return;
	}
	// Vendor CloseForPlayer only runs the goodbye emote — no CloseGroundContainer packet.
	// Mirror server CheckClose on the client so the panel leaves with the thanks message.
	if (OpenVendorGuid != 0 && IsBeyondContainerUseRadius(OpenVendorGuid))
	{
		HideVendorPanel();
	}
	if (OpenLootContainerGuid != 0)
	{
		if (IsBeyondContainerUseRadius(OpenLootContainerGuid))
		{
			// Debounce — cylinder gap can flicker across the close band while looting.
			if (PendingLootRangeCloseAt <= 0.0)
			{
				PendingLootRangeCloseAt = FPlatformTime::Seconds() + 0.6;
			}
			else if (FPlatformTime::Seconds() >= PendingLootRangeCloseAt)
			{
				PendingLootRangeCloseAt = 0.0;
				HideExternalContainer(true);
			}
		}
		else
		{
			PendingLootRangeCloseAt = 0.0;
		}
	}
	else
	{
		PendingLootRangeCloseAt = 0.0;
	}
}

void UACEUIGameplayBinder::SyncEnvPanelMode()
{
	if (!Manager)
	{
		return;
	}
	const bool bLoot = OpenLootContainerGuid != 0;
	const bool bVendor = OpenVendorGuid != 0;
	const bool bTrade = bTradeOpen;
	const bool bSalvage = OpenSalvageToolGuid != 0;
	const bool bHouse = OpenHouseLord != 0 && !bLoot && !bVendor && !bTrade && !bSalvage;
	const bool bAny = bLoot || bVendor || bTrade || bSalvage || bHouse;
	SetFloatyVisible(TEXT("RootGameplay_FloatyEnvPanel_Field"), bAny);
	// Only one env mode panel at a time — Salvage/Trade ghosts under vendor/loot otherwise.
	Manager->SetElementVisibleByName(TEXT("SalvagePanel"), bSalvage);
	Manager->SetElementVisibleByName(TEXT("SecureTrade"), bTrade);
	Manager->SetElementVisibleByName(TEXT("ExternalContainer"), bLoot);
	Manager->SetElementVisibleByName(TEXT("Vendor"), bVendor);
	// Visibility inherits from the mode root. Preserve authored hidden focus/locked frames.
	auto SetEnvTreeVisible = [this](const TCHAR* RootName, bool bShow)
	{
		if (TSharedPtr<FACEUIElement> Root = Manager->FindElementByName(RootName))
		{
			Root->bVisible = bShow;
		}
	};
	SetEnvTreeVisible(TEXT("SalvagePanel"), bSalvage);
	SetEnvTreeVisible(TEXT("SecureTrade"), bTrade);
	SetEnvTreeVisible(TEXT("ExternalContainer"), bLoot);
	SetEnvTreeVisible(TEXT("Vendor"), bVendor);
	// Mag-nus gmEnvPanelUI::SetupChildren hides Slumlord until house UI — wood/maroon
	// ghost behind Vendor Buy is Slumlord (0x06004CC2 + HouseBuy buttons) left visible.
	SetEnvTreeVisible(TEXT("Slumlord"), bHouse);
	if (!bHouse)
	{
		for (auto* Array : {&HouseOfferSlots,&HouseOfferBackgrounds,&HouseOfferSelections})
			for (auto& B : *Array) if(B) B->SetVisibility(ESlateVisibility::Collapsed);
		for(auto& L : HouseOfferLabels) if(L) L->SetVisibility(ESlateVisibility::Collapsed);
	}
	else
	{
		ExpandEnvFloatyForWideContent();
		Manager->SetElementVisibleByName(TEXT("CloseSlumlordButton"),true);
	}
	// Collapse inactive mode chrome so authored backgrounds cannot ghost through.
	if (!bVendor)
	{
		Manager->SetElementVisibleByName(TEXT("VendorPanel"), false);
		Manager->SetElementVisibleByName(TEXT("VendorItemsPage"), false);
		Manager->SetElementVisibleByName(TEXT("VendorBuyPage"), false);
		Manager->SetElementVisibleByName(TEXT("VendorSellPage"), false);
		for (UBorder* B : VendorItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : VendorItemSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* L : VendorTabLabels) { if (L) L->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* L : VendorButtonLabels) { if (L) L->SetVisibility(ESlateVisibility::Collapsed); }
	}
	if (!bLoot)
	{
		for (UBorder* B : ExtItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : ExtItemSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : ExtPackSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : ExtPackSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	}
	if (!bTrade)
	{
		for (UBorder* B : TradeSelfSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : TradeOtherSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	}
	if (!bSalvage)
	{
		for (UBorder* B : SalvageItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : SalvageItemBackgrounds) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : SalvageItemSelections) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* L : SalvageLabels) { if (L) L->SetVisibility(ESlateVisibility::Collapsed); }
		if (SalvageWarningLabel) { SalvageWarningLabel->SetVisibility(ESlateVisibility::Collapsed); }
	}
	if (bSalvage)
	{
		ExpandEnvFloatyForWideContent();
		Manager->SetElementVisibleByName(TEXT("CloseSalvagePanelButton"), true);
		Manager->SetElementVisibleByName(TEXT("Salvage_Button"), true);
	}
	if (bVendor)
	{
		ExpandEnvFloatyForWideContent();
		SyncVendorPageVisibility();
		RefreshVendorTabLabels();
		RefreshVendorButtonLabels();
		Manager->SetElementVisibleByName(TEXT("CloseVendorPanelButton"), true);
	}
	if (bLoot)
	{
		ExpandEnvFloatyForWideContent();
		if (TSharedPtr<FACEUIElement> CloseBtn = Manager->FindElementUnder(
			TEXT("ExternalContainer"), TEXT("CloseExtContainerPanelButton")))
		{
			CloseBtn->bVisible = true;
		}
		Manager->SetElementVisibleByName(TEXT("CloseExtContainerPanelButton"), true);
	}
	if (bTrade)
	{
		ExpandEnvFloatyForWideContent();
	}
	if (bAny)
	{
		if (TSharedPtr<FACEUIElement> Env = Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field")))
		{
			Manager->BringFloatyToFront(Env);
		}
	}
}

void UACEUIGameplayBinder::ShowExternalContainer(int32 Guid)
{
	if (Guid == 0)
	{
		return;
	}
	OpenLootContainerGuid = Guid;
	OpenLootSelectedPackGuid = Guid;
	ExtItemScrollOffset = 0;
	ExtPackScrollOffset = 0;
	OpenVendorGuid = 0;
	PendingVendorSellGuid = PendingVendorSellTargetGuid = 0;
	VendorSellCart.Reset();
	VendorBuyCart.Reset();
	bTradeOpen = false;
	OpenSalvageToolGuid = 0;
	SalvageQueueGuids.Reset();
	if (PlayerController)
	{
		PlayerController->EndUseApproach();
	}
	SyncEnvPanelMode();
	RefreshExternalContainerOverlays();
}

void UACEUIGameplayBinder::HideExternalContainer(bool bNotifyServer)
{
	const int32 Guid = OpenLootContainerGuid;
	if (Guid && (InvDragSourcePack == Guid || InvDragSourcePack == OpenLootSelectedPackGuid)) CancelInventoryDrag();
	if (Guid && Client && LastSelection.bValid)
	{
		FACEWorldObject Selected;
		if (Client->GetWorldObject(LastSelection.Guid, Selected)
			&& (Selected.ContainerId == Guid || (OpenLootSelectedPackGuid && Selected.ContainerId == OpenLootSelectedPackGuid)))
			Client->SelectObject(0);
	}
	OpenLootContainerGuid = 0;
	OpenLootSelectedPackGuid = 0;
	ExtItemScrollOffset = 0;
	PendingLootRangeCloseAt = 0.0;
	for (UBorder* B : ExtItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UBorder* B : ExtItemSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UBorder* B : ExtPackSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UBorder* B : ExtPackSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	SyncEnvPanelMode();
	if (Guid != 0 && Client)
	{
		Client->ClearContainerContents(Guid);
		if (bNotifyServer)
		{
			Client->SendNoLongerViewingContents(Guid);
		}
	}
}

void UACEUIGameplayBinder::ShowVendorPanel(int32 Guid)
{
	if (Guid == 0)
	{
		return;
	}
	const bool bSameVendor = (Guid == OpenVendorGuid);
	OpenVendorGuid = Guid;
	OpenLootContainerGuid = 0;
	bTradeOpen = false;
	OpenSalvageToolGuid = 0;
	SalvageQueueGuids.Reset();
	if (!bSameVendor)
	{
		VendorSellSplit = {};
		ActiveVendorPage = 0;
		VendorItemsScrollOffset = 0;
		VendorBuyScrollOffset = 0;
		VendorSellScrollOffset = 0;
		VendorItemTypeFilter = 0;
		VendorSelectedGuid = 0;
		VendorSellSelectedGuid = 0;
		VendorBuyCart.Reset();
		VendorSellCart.Reset();
		CloseVendorFilterDropdown();
	}
	if (PlayerController)
	{
		PlayerController->EndUseApproach();
	}
	SyncEnvPanelMode();
	if (!bSameVendor && Client)
	{
		// VendorItemsUI::UpdateItemsList(selectFirst): use the displayed stock
		// order, and initialize the ordinary selection/quantity controls too.
		TArray<FACEWorldObject> Items = Client->GetVendorMerchandise();
		if (Items.IsEmpty()) Items = Client->GetPackItems(Guid);
		if (!Items.IsEmpty())
		{
			VendorSelectedGuid = Items[0].Guid;
			Client->SelectObject(VendorSelectedGuid);
		}
	}
	RefreshVendorOverlays();
}

void UACEUIGameplayBinder::HideVendorPanel()
{
	VendorSellSplit = {};
	const int32 Guid = OpenVendorGuid;
	OpenVendorGuid = 0;
	VendorSelectedGuid = 0;
	VendorSellSelectedGuid = 0;
	PendingVendorSellGuid = PendingVendorSellTargetGuid = 0;
	VendorBuyCart.Reset();
	VendorSellCart.Reset();
	CloseVendorFilterDropdown();
	for (UBorder* B : VendorItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UBorder* B : VendorItemSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	HideVendorTextOverlays();
	SyncEnvPanelMode();
	if (Guid != 0 && Client)
	{
		Client->SendNoLongerViewingContents(Guid);
	}
}

void UACEUIGameplayBinder::SetVendorPage(int32 PageIndex)
{
	ActiveVendorPage = FMath::Clamp(PageIndex, 0, 2);
	CloseVendorFilterDropdown();
	SyncVendorPageVisibility();
	RefreshVendorTabLabels();
	RefreshVendorOverlays();
}

void UACEUIGameplayBinder::SyncVendorPageVisibility()
{
	if (!Manager || OpenVendorGuid == 0)
	{
		return;
	}
	Manager->SetElementVisibleByName(TEXT("VendorPanel"), true);
	// Recursive — SetEnvTreeVisible(Vendor,true) re-shows Items chrome under Buy otherwise.
	auto SetPageTree = [this](const TCHAR* PageName, bool bShow)
	{
		if (TSharedPtr<FACEUIElement> Root = Manager->FindElementByName(PageName))
		{
			TArray<TSharedPtr<FACEUIElement>> Stack;
			Stack.Add(Root);
			while (Stack.Num() > 0)
			{
				TSharedPtr<FACEUIElement> Cur = Stack.Pop(EAllowShrinking::No);
				if (!Cur.IsValid()) { continue; }
				Cur->bVisible = bShow;
				for (const TSharedPtr<FACEUIElement>& Ch : Cur->Children)
				{
					Stack.Add(Ch);
				}
			}
		}
	};
	SetPageTree(TEXT("VendorItemsPage"), ActiveVendorPage == 0);
	SetPageTree(TEXT("VendorBuyPage"), ActiveVendorPage == 1);
	SetPageTree(TEXT("VendorSellPage"), ActiveVendorPage == 2);
}

void UACEUIGameplayBinder::RefreshVendorTabLabels()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree || OpenVendorGuid == 0)
	{
		for (UTextBlock* L : VendorTabLabels)
		{
			if (L) { L->SetVisibility(ESlateVisibility::Collapsed); }
		}
		return;
	}
	EnsureOverlays();
	static const TCHAR* TabNames[] = {
		TEXT("VendorItemsTab"), TEXT("VendorBuyTab"), TEXT("VendorSellTab")
	};
	// Retail-facing labels for the three vendor pages (DAT tabs are image-only).
	static const TCHAR* TabLabels[] = { TEXT("Items"), TEXT("Buying"), TEXT("Selling") };
	while (VendorTabLabels.Num() < 3)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		L->SetJustification(ETextJustify::Center);
		VendorTabLabels.Add(L);
	}
	for (int32 i = 0; i < 3; ++i)
	{
		const FLinearColor Col = (ActiveVendorPage == i) ? TextGold : TextWhite;
		ApplyPanelTabChrome(TabNames[i], ActiveVendorPage == i);
		PlaceTextOnElement(VendorTabLabels[i], TabNames[i], TabLabels[i], 9, Col, 10050);
	}
}

void UACEUIGameplayBinder::RefreshVendorButtonLabels()
{
	if (!Manager || !Canvas || !Canvas->WidgetTree || OpenVendorGuid == 0)
	{
		for (UTextBlock* L : VendorButtonLabels)
		{
			if (L) { L->SetVisibility(ESlateVisibility::Collapsed); }
		}
		return;
	}
	EnsureOverlays();
	struct FVendorBtn
	{
		const TCHAR* Element;
		const TCHAR* Label;
		int32 Page; // -1 = items, 0/1/2 = match ActiveVendorPage, 99 = always
	};
	static const FVendorBtn Specs[] = {
		{ TEXT("VendorItemBuy_Button"), TEXT("Buy"), -1 },
		{ TEXT("VendorItemAdd_Button"), TEXT("Add to List"), -1 },
		{ TEXT("VendorBuyBuyItem_Button"), TEXT("Buy"), 1 },
		{ TEXT("VendorBuyBuyAll_Button"), TEXT("Buy All"), 1 },
		{ TEXT("VendorBuyClearItem_Button"), TEXT("Clear"), 1 },
		{ TEXT("VendorBuyClearAll_Button"), TEXT("Clear All"), 1 },
		{ TEXT("VendorSellSellItem_Button"), TEXT("Sell"), 2 },
		{ TEXT("VendorSellSellAll_Button"), TEXT("Sell All"), 2 },
		{ TEXT("VendorSellClearItem_Button"), TEXT("Clear"), 2 },
		{ TEXT("VendorSellClearAll_Button"), TEXT("Clear All"), 2 },
	};
	constexpr int32 N = UE_ARRAY_COUNT(Specs);
	while (VendorButtonLabels.Num() < N)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		L->SetJustification(ETextJustify::Center);
		VendorButtonLabels.Add(L);
	}
	for (int32 i = 0; i < N; ++i)
	{
		const bool bShow = (Specs[i].Page < 0 && ActiveVendorPage == 0)
			|| Specs[i].Page == ActiveVendorPage;
		if (!bShow)
		{
			if (VendorButtonLabels[i])
			{
				VendorButtonLabels[i]->SetVisibility(ESlateVisibility::Collapsed);
			}
			continue;
		}
		PlaceTextOnElement(VendorButtonLabels[i], Specs[i].Element, Specs[i].Label, 8, TextGold, 10060, true);
	}
}


FString UACEUIGameplayBinder::VendorFilterLabel() const
{
	static_assert(VendorFilterCount == UE_ARRAY_COUNT(ACERetailVendorCategories::Categories));
	return ACERetailVendorCategories::Categories[FMath::Clamp(VendorItemTypeFilter,0,VendorFilterCount-1)].Name;
}

bool UACEUIGameplayBinder::VendorItemMatchesFilter(const FACEWorldObject& O, int32 FilterIndex) const
{
	return FilterIndex >= 0 && FilterIndex < VendorFilterCount
		&& (uint32(O.ItemType) & ACERetailVendorCategories::Categories[FilterIndex].Mask) != 0;
}

void UACEUIGameplayBinder::RebuildVendorVisibleFilters()
{
	VendorVisibleFilterIndices.Reset();
	// Use the live profiles during this query; do not duplicate names and appearance
	// arrays whenever an open dropdown checks which categories are available.
	const auto Session = Client && OpenVendorGuid != 0 ? Client->GetSession() : nullptr;
	TConstArrayView<FACEWorldObject> Items;
	TArray<FACEWorldObject> Fallback;
	if (Session)
	{
		Items = Session->GetVendorMerchandise();
		if (Items.IsEmpty())
		{
			Session->GetPackItems(OpenVendorGuid, Fallback);
			Items = Fallback;
		}
	}
	for (int32 FilterId = 0; FilterId < VendorFilterCount; ++FilterId)
	{
		for (const FACEWorldObject& O : Items)
		{
			if (VendorItemMatchesFilter(O, FilterId))
			{
				VendorVisibleFilterIndices.Add(FilterId);
				break;
			}
		}
	}
	if (!VendorVisibleFilterIndices.Contains(VendorItemTypeFilter))
	{
		VendorItemTypeFilter = VendorVisibleFilterIndices.IsEmpty() ? 0 : VendorVisibleFilterIndices[0];
	}
}

static void UpdateVendorFilterArrow(UACEUIElementManager* Manager, bool bOpen)
{
	if (!Manager) return;
	if (auto Arrow = Manager->FindElementByName(TEXT("Menu_Vendor_SelectionWidget")))
	{
		// Retail's menu uses states 1/2 when closed and 6/7 when open.
		// Select the state as well as the fallback image; state media wins at paint.
		Arrow->bVisible = true;
		Arrow->bUseExplicitState = true;
		Arrow->DefaultState = bOpen ? 6 : 1;
		Arrow->ImageFileId = bOpen ? 0x060012B2 : 0x060012B1;
		Arrow->DrawMode = 3;
	}
}

void UACEUIGameplayBinder::PlaceVendorFilterCaption()
{
	if (!Canvas || !Manager || OpenVendorGuid == 0)
	{
		if (VendorFilterText)
		{
			VendorFilterText->SetVisibility(ESlateVisibility::Collapsed);
		}
		return;
	}
	EnsureOverlays();
	TSharedPtr<FACEUIElement> Display = Manager->FindElementUnder(
		TEXT("VendorItemsPage"), TEXT("Menu_Vendor_SelectionDisplay"));
	if (!Display.IsValid())
	{
		Display = Manager->FindElementUnder(
			TEXT("VendorItemTypeList"), TEXT("Menu_Vendor_SelectionDisplay"));
	}
	if (!Display.IsValid())
	{
		Display = Manager->FindElementByName(TEXT("Menu_Vendor_SelectionDisplay"));
	}
	PlaceTextOnElement(VendorFilterText, Display, VendorFilterLabel(), 9, TextGold, 200010);
	UpdateVendorFilterArrow(Manager, bVendorFilterDropdownOpen);
}

void UACEUIGameplayBinder::ToggleVendorFilterDropdown()
{
	bVendorFilterDropdownOpen = !bVendorFilterDropdownOpen;
	RefreshVendorFilterDropdown();
}

void UACEUIGameplayBinder::SetVendorItemTypeFilter(int32 FilterIndex)
{
	VendorItemTypeFilter = FMath::Clamp(FilterIndex, 0, VendorFilterCount - 1);
	CloseVendorFilterDropdown();
	RefreshVendorOverlays();
}

void UACEUIGameplayBinder::CloseVendorFilterDropdown()
{
	bVendorFilterDropdownOpen = false;
	UpdateVendorFilterArrow(Manager, false);
	if (VendorFilterDropdownBg)
	{
		VendorFilterDropdownBg->SetVisibility(ESlateVisibility::Collapsed);
	}
	for (UTextBlock* L : VendorFilterDropdownLabels)
	{
		if (L) { L->SetVisibility(ESlateVisibility::Collapsed); }
	}
	for (UBorder* Row : VendorFilterDropdownRows)
		if (Row) Row->SetVisibility(ESlateVisibility::Collapsed);
}

void UACEUIGameplayBinder::RefreshVendorFilterDropdown()
{
	if (!Canvas || !Canvas->WidgetTree || !Manager || OpenVendorGuid == 0 || !bVendorFilterDropdownOpen)
	{
		CloseVendorFilterDropdown();
		return;
	}
	TSharedPtr<FACEUIElement> Menu = Manager->FindElementUnder(
		TEXT("VendorItemsPage"), TEXT("VendorItemTypeList"));
	if (!Menu.IsValid())
	{
		Menu = Manager->FindElementByName(TEXT("VendorItemTypeList"));
	}
	if (!Menu.IsValid())
	{
		CloseVendorFilterDropdown();
		return;
	}
	EnsureOverlays();
	RebuildVendorVisibleFilters();
	UpdateVendorFilterArrow(Manager, true);
	constexpr int32 RowH = 18;
	constexpr int32 DropZ = 200000;
	const int32 Rows = FMath::Max(1, VendorVisibleFilterIndices.Num());
	const FIntPoint Origin = Menu->GetScreenOrigin();
	const float SX = Canvas->GetLastScaleX();
	const float SY = Canvas->GetLastScaleY();
	const float W = FMath::Max(100.f, static_cast<float>(Menu->Width)) * SX;
	const float X = static_cast<float>(Origin.X) * SX;
	const float Y = static_cast<float>(Origin.Y + Menu->Height) * SY;

	if (!VendorFilterDropdownBg)
	{
		VendorFilterDropdownBg = Canvas->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		VendorFilterDropdownBg->SetPadding(FMargin(0.f));
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::Box;
		Brush.TintColor = FSlateColor(FLinearColor::Black);
		VendorFilterDropdownBg->SetBrush(Brush);
		Canvas->GetElementLayer()->AddChild(VendorFilterDropdownBg);
	}
	VendorFilterDropdownBg->SetVisibility(ESlateVisibility::Visible);
	if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(VendorFilterDropdownBg->Slot))
	{
		Slot->SetAnchors(FAnchors(0.f, 0.f));
		Slot->SetPosition(FVector2D(X, Y));
		Slot->SetSize(FVector2D(W, static_cast<float>(Rows * RowH) * SY));
		Canvas->SetOverlayOrder(VendorFilterDropdownBg, nullptr, DropZ);
	}

	while (VendorFilterDropdownLabels.Num() < Rows)
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetJustification(ETextJustify::Left);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		L->SetFont(Font);
		Canvas->GetElementLayer()->AddChild(L);
		VendorFilterDropdownLabels.Add(L);
	}
	while (VendorFilterDropdownRows.Num() < Rows)
	{
		auto* Row = Canvas->WidgetTree->ConstructWidget<UBorder>();
		Row->SetPadding(FMargin(0.f));
		Canvas->GetElementLayer()->AddChild(Row);
		VendorFilterDropdownRows.Add(Row);
	}
	for (int32 i = 0; i < Rows; ++i)
	{
		UBorder* Row = VendorFilterDropdownRows[i];
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (auto* Slot = Cast<UCanvasPanelSlot>(Row->Slot))
		{
			Slot->SetAnchors(FAnchors(0.f, 0.f));
			Slot->SetPosition(FVector2D(X, Y + static_cast<float>(i * RowH) * SY));
			Slot->SetSize(FVector2D(W, static_cast<float>(RowH) * SY));
			Canvas->SetOverlayOrder(Row, nullptr, DropZ + 1);
		}
		UTextBlock* L = VendorFilterDropdownLabels[i];
		const int32 FilterId = VendorVisibleFilterIndices.IsValidIndex(i)
			? VendorVisibleFilterIndices[i] : i;
		// Menu_Vendor_PopupItem (0x21000043): green normal/selected retail art.
		// Keep the native one-pixel row border when the menu is stretched.
		if (auto* Resolver = Canvas->GetResourceResolver())
		{
			if (auto* Texture = Resolver->ResolveTexture(FilterId == VendorItemTypeFilter ? 0x060012B4 : 0x060012B3))
			{
				FSlateBrush Brush;
				Brush.SetResourceObject(Texture);
				Brush.ImageSize = FVector2D(Texture->GetSizeX(), Texture->GetSizeY());
				Brush.DrawAs = ESlateBrushDrawType::Box;
				Brush.Margin = FMargin(1.f / Texture->GetSizeX(), 1.f / Texture->GetSizeY());
				Row->SetBrush(Brush);
				Row->SetBrushColor(FLinearColor::White);
			}
		}
		const int32 Prev = VendorItemTypeFilter;
		VendorItemTypeFilter = FilterId;
		const FString Label = VendorFilterLabel();
		VendorItemTypeFilter = Prev;
		L->SetText(FText::FromString(Label));
		L->SetColorAndOpacity(FSlateColor(FilterId == VendorItemTypeFilter ? TextWhite : FLinearColor::FromSRGBColor(FColor(204,204,204))));
		L->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(L->Slot))
		{
			Slot->SetAnchors(FAnchors(0.f, 0.f));
			Slot->SetPosition(FVector2D(X + 4.f * SX, Y + static_cast<float>(i * RowH) * SY));
			Slot->SetSize(FVector2D(W - 8.f * SX, static_cast<float>(RowH) * SY));
			Canvas->SetOverlayOrder(L, nullptr, DropZ + 2);
		}
	}
	for (int32 i = Rows; i < VendorFilterDropdownLabels.Num(); ++i)
	{
		if (VendorFilterDropdownLabels[i])
		{
			VendorFilterDropdownLabels[i]->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	for (int32 i = Rows; i < VendorFilterDropdownRows.Num(); ++i)
		VendorFilterDropdownRows[i]->SetVisibility(ESlateVisibility::Collapsed);
}

bool UACEUIGameplayBinder::TryHandleVendorFilterDropdownClick(FVector2D Absolute)
{
	if (!bVendorFilterDropdownOpen)
	{
		return false;
	}
	if (VendorFilterDropdownBg
		&& VendorFilterDropdownBg->GetVisibility() != ESlateVisibility::Collapsed
		&& Canvas->IsWidgetExposedAt(VendorFilterDropdownBg, Absolute))
	{
		const FGeometry& Geo = VendorFilterDropdownBg->GetCachedGeometry();
		const FVector2D Local = Geo.AbsoluteToLocal(Absolute);
		const FVector2D Size = Geo.GetLocalSize();
		if (Size.Y > 1.f)
		{
			const int32 VisibleRows = FMath::Max(1, VendorVisibleFilterIndices.Num());
			const int32 Row = FMath::Clamp(
				static_cast<int32>(Local.Y * static_cast<float>(VisibleRows) / Size.Y),
				0, VisibleRows - 1);
			const int32 FilterId = VendorVisibleFilterIndices.IsValidIndex(Row)
				? VendorVisibleFilterIndices[Row] : Row;
			SetVendorItemTypeFilter(FilterId);
			return true;
		}
		return true;
	}
	CloseVendorFilterDropdown();
	return false;
}

int32 UACEUIGameplayBinder::GetVendorPurchaseLimit(int32 ItemGuid) const
{
	if (Client && OpenVendorGuid != 0)
		for (const auto& Stock : Client->GetVendorMerchandise())
			if (Stock.Guid == ItemGuid) return ACEInventoryRules::VendorPurchaseLimit(Stock);
	return 0;
}

int32 UACEUIGameplayBinder::GetVendorSelectionLimit(int32 ItemGuid) const
{
	const int32 Limit=GetVendorPurchaseLimit(ItemGuid);
	if (ActiveVendorPage==1)
		if(const auto* P=VendorBuyCart.FindByPredicate([&](const auto& Entry){return Entry.Value==ItemGuid;}))return FMath::Min(Limit,P->Key);
	return Limit;
}

void UACEUIGameplayBinder::BuySelectedVendorItem()
{
	if (!Client || OpenVendorGuid == 0 || VendorSelectedGuid == 0)
	{
		return;
	}
	TArray<TPair<int32, int32>> One;
	const int32 Limit=GetVendorPurchaseLimit(VendorSelectedGuid);
	if (Limit<=0) return;
	One.Emplace(LastSelection.Guid==VendorSelectedGuid ? FMath::Clamp(SelectedStackAmount,1,Limit) : 1, VendorSelectedGuid);
	SubmitVendorPurchase(One);
}

void UACEUIGameplayBinder::AddSelectedVendorItemToBuyCart()
{
	if (!Client || OpenVendorGuid == 0 || VendorSelectedGuid == 0)
	{
		return;
	}
	const int32 Limit=GetVendorPurchaseLimit(VendorSelectedGuid);
	if (Limit<=0) return;
	const int32 Quantity=LastSelection.Guid==VendorSelectedGuid ? FMath::Clamp(SelectedStackAmount,1,Limit) : 1;
	for (TPair<int32, int32>& P : VendorBuyCart)
	{
		if (P.Value == VendorSelectedGuid)
		{
			P.Key = static_cast<int32>(FMath::Min<int64>(Limit,static_cast<int64>(P.Key)+Quantity));
			RefreshVendorOverlays();
			return;
		}
	}
	VendorBuyCart.Emplace(Quantity, VendorSelectedGuid);
	// Retail VendorItemsUI::AddToBuyList leaves the Items page and selection
	// in place so another click adds the same quantity again.
	RefreshVendorOverlays();
}

void UACEUIGameplayBinder::BuyVendorCartItem()
{
	if (!Client || OpenVendorGuid == 0)
	{
		return;
	}
	int32 Guid = VendorSelectedGuid;
	if (Guid == 0 && VendorBuyCart.Num() > 0)
	{
		Guid = VendorBuyCart[0].Value;
	}
	if (Guid == 0)
	{
		return;
	}
	int32 Amt = 0;
	for (const TPair<int32, int32>& P : VendorBuyCart)
	{
		if (P.Value == Guid)
		{
			Amt = P.Key;
			break;
		}
	}
	TArray<TPair<int32, int32>> One;
	const int32 Limit=GetVendorPurchaseLimit(Guid);
	if (Limit<=0 || Amt<=0) return;
	const int32 Quantity=FMath::Clamp(LastSelection.Guid==Guid?SelectedStackAmount:Amt,1,FMath::Min(Amt,Limit));
	One.Emplace(Quantity, Guid);
	if (!SubmitVendorPurchase(One)) return;
	for(auto& P:VendorBuyCart)if(P.Value==Guid)P.Key-=Quantity;
	VendorBuyCart.RemoveAll([](const auto& P){return P.Key<=0;});
	RefreshVendorOverlays();
}

int32 UACEUIGameplayBinder::CountPlayerPyreals() const
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	// gmVendorUI::UpdateTotalValue reads the player's CoinValue quality (20).
	return Session ? FMath::Max(0, Session->GetPlayerVitals().StatQualityInts.FindRef(20)) : 0;
}

void UACEUIGameplayBinder::HideVendorTextOverlays()
{
	auto Hide = [](UTextBlock* T)
	{
		if (T) { T->SetVisibility(ESlateVisibility::Collapsed); }
	};
	for (UTextBlock* L : VendorTabLabels) { Hide(L); }
	for (UTextBlock* L : VendorFilterDropdownLabels) { Hide(L); }
	for (UBorder* Row : VendorFilterDropdownRows)
		if (Row) Row->SetVisibility(ESlateVisibility::Collapsed);
	Hide(VendorItemNameLabel);
	Hide(VendorItemCostLabel);
	Hide(VendorBuyCostLabel);
	Hide(VendorBuyPurseLabel);
	Hide(VendorSellCostLabel);
	Hide(VendorSellPurseLabel);
	Hide(VendorFilterText);
	if (VendorFilterDropdownBg)
	{
		VendorFilterDropdownBg->SetVisibility(ESlateVisibility::Collapsed);
	}
	// Do not touch PanelBodyText / SpellcastSpellNameLabel here — that stole the hotbar name.
}

void UACEUIGameplayBinder::RefreshVendorInfoTexts()
{
	if (!Client || !Manager || OpenVendorGuid == 0)
	{
		HideVendorTextOverlays();
		return;
	}
	EnsureOverlays();
	auto EnsureLabel = [this](TObjectPtr<UTextBlock>& Label) -> UTextBlock*
	{
		if (!Label && Canvas && Canvas->WidgetTree)
		{
			Label = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			Label->SetVisibility(ESlateVisibility::HitTestInvisible);
			Label->SetAutoWrapText(true);
		}
		return Label;
	};
	auto Hide = [](UTextBlock* T) { if (T) { T->SetVisibility(ESlateVisibility::Collapsed); } };

	const auto VendorSession = Client->GetSession();
	const bool AlternateCurrency = VendorSession && !VendorSession->VendorUsesPyreals();
	const FString Currency = VendorSession && !VendorSession->GetVendorCurrencyName().IsEmpty()
		? VendorSession->GetVendorCurrencyName() : TEXT("currency");
	const int32 Purse = VendorSession ? VendorSession->GetVendorPurchaseFunds() : 0;
	const float SellRate = Client->GetVendorSellRate();
	const float BuyRate = Client->GetVendorBuyRate();

	FACEWorldObject Focus;
	const bool bHaveFocus = VendorSelectedGuid != 0 && Client->GetWorldObject(VendorSelectedGuid, Focus);

	// Collapse labels that belong to inactive pages so they cannot linger on-screen.
	if (ActiveVendorPage != 0)
	{
		Hide(VendorItemNameLabel);
		Hide(VendorItemCostLabel);
	}
	if (ActiveVendorPage != 1)
	{
		Hide(VendorBuyCostLabel);
		Hide(VendorBuyPurseLabel);
	}
	if (ActiveVendorPage != 2)
	{
		Hide(VendorSellCostLabel);
		Hide(VendorSellPurseLabel);
	}

	if (ActiveVendorPage == 0)
	{
		const int32 Quantity = bHaveFocus ? (LastSelection.Guid == Focus.Guid ? SelectedStackAmount : 1) : 0;
		const FString ItemName = bHaveFocus ? ACERetailObjectNames::Name(Focus) : FString();
		PlaceTextOnElement(EnsureLabel(VendorItemNameLabel), TEXT("VendorItemName_Text"),
			ItemName, 9, TextWhite, 10040);
		FString CostText;
		if (bHaveFocus)
		{
			const uint32 Cost = VendorSellCost(Focus, Quantity, SellRate);
			CostText = AlternateCurrency
				? FString::Printf(TEXT("This item costs %s %s. You have %s"),*FormatXpNumber(Cost),*Currency,*FormatXpNumber(Purse))
				: FString::Printf(TEXT("costs %sp (you have %sp)"),*FormatXpNumber(Cost),*FormatXpNumber(Purse));
		}
		PlaceTextOnElement(EnsureLabel(VendorItemCostLabel), TEXT("VendorItemCost_Text"),
			CostText, 8, TextGold, 10040);
	}
	else if (ActiveVendorPage == 1)
	{
		int64 ItemCount = 0;
		int64 Worth = 0;
		for (const TPair<int32, int32>& P : VendorBuyCart)
		{
			ItemCount += P.Key;
			FACEWorldObject Obj;
			if (Client->GetWorldObject(P.Value, Obj))
			{
				Worth += VendorSellCost(Obj, P.Key, SellRate);
			}
		}
		PlaceTextOnElement(EnsureLabel(VendorBuyCostLabel), TEXT("VendorBuyCost_Text"),
			FString::Printf(TEXT("Buying %lld items worth %s %s"), ItemCount, *FormatXpNumber(Worth), AlternateCurrency ? *Currency : TEXT("pyreals")),
			8, TextWhite, 10040);
		PlaceTextOnElement(EnsureLabel(VendorBuyPurseLabel), TEXT("VendorBuyPurse_Text"),
			FString::Printf(TEXT("You have %s %s"), *FormatXpNumber(Purse), AlternateCurrency ? *Currency : TEXT("pyreals")),
			8, TextWhite, 10040);
	}
	else if (ActiveVendorPage == 2)
	{
		const int32 ItemCount = VendorSellCart.Num();
		int32 Worth = 0;
		for (const TPair<int32, int32>& P : VendorSellCart)
		{
			FACEWorldObject Obj;
			if (Client->GetWorldObject(P.Value, Obj))
			{
				Worth += VendorBuyPayout(Obj, P.Key, BuyRate);
			}
		}
		PlaceTextOnElement(EnsureLabel(VendorSellCostLabel), TEXT("VendorSellCost_Text"),
			FString::Printf(TEXT("Selling %d items worth %sp"), ItemCount, *FormatXpNumber(Worth)),
			8, TextWhite, 10040);
		PlaceTextOnElement(EnsureLabel(VendorSellPurseLabel), TEXT("VendorSellPurse_Text"),
			FString::Printf(TEXT("You have %sp"), *FormatXpNumber(CountPlayerPyreals())),
			8, TextWhite, 10040);
	}
}

void UACEUIGameplayBinder::AddInventoryGuidToVendorSellCart(int32 Guid, int32 Amount)
{
	if (!Client || Guid == 0 || OpenVendorGuid == 0)
	{
		return;
	}
	if (VendorSellSplit.SourceGuid != 0 || TradeStackSplit.SourceGuid != 0)
	{
		PostInventorySystemMessage(TEXT("Please wait for the stack to finish splitting."));
		return;
	}
	FACEWorldObject Obj;
	if (!Client->GetWorldObject(Guid, Obj))
	{
		return;
	}
	if (!CanStageVendorSellItem(Guid))
	{
		PostInventorySystemMessage(TEXT("That item cannot be sold here."));
		return;
	}
	if (Guid == Client->GetPlayerGuid() || (Obj.ItemType & ACEItemType::Container) != 0 || Obj.ItemsCapacity > 0)
	{
		// A pack is a bulk selection, never the item offered for sale. Use complete
		// stacks and refresh once after the batch; an earlier partial-stack selection
		// must not truncate one of the contents when the pack is dragged.
		for (const FACEWorldObject& Item : Client->GetPackItems(Guid))
		{
			if ((Item.ItemType & ACEItemType::Container) != 0 || Item.ItemsCapacity > 0
				|| Item.Attuned != 0 || Item.WielderId != 0 || Item.CurrentWieldedLocation != 0
				|| !Client->GetSession() || !Client->GetSession()->CanVendorBuyItem(Item)) continue;
			TPair<int32, int32>* Existing = VendorSellCart.FindByPredicate(
				[&Item](const TPair<int32, int32>& Entry) { return Entry.Value == Item.Guid; });
			const int32 FullStackAmount = FMath::Max(1, Item.StackSize);
			if (Existing) Existing->Key = FullStackAmount;
			else VendorSellCart.Emplace(FullStackAmount, Item.Guid);
			VendorSellSelectedGuid = Item.Guid;
		}
		ActiveVendorPage = 2;
		SyncVendorPageVisibility();
		RefreshVendorOverlays();
		return;
	}
	if ((Obj.ItemType & ACEItemType::Container) != 0 || Obj.Attuned != 0)
	{
		PostInventorySystemMessage(TEXT("That item cannot be sold."));
		return;
	}
	const int32 StackSize = FMath::Max(1, Obj.StackSize);
	if (Amount == INDEX_NONE) Amount = LastSelection.Guid == Guid ? SelectedStackAmount : StackSize;
	Amount = FMath::Clamp(Amount, 1, StackSize);
	if (Amount < StackSize)
	{
		// VendorSellUI::AcceptDragObject splits into the item's own container and
		// waits for ItemAttributesChanged to replace the offered source with the new GUID.
		// ACE/GDLE sale processing operates on whole objects, even with a smaller amount.
		if (!Client->IsOwnedInventoryItem(Obj) || !Obj.ContainerId || Obj.WielderId
			|| Obj.CurrentWieldedLocation || !Client->GetSession()
			|| !Client->GetSession()->CanVendorBuyItem(Obj))
		{
			PostInventorySystemMessage(TEXT("That item cannot be split for sale."));
			return;
		}
		VendorSellSplit.VendorGuid = OpenVendorGuid;
		TrackInventoryStackSplit(VendorSellSplit, Obj, Amount);
		Client->SendStackableSplitToContainer(Guid, Obj.ContainerId, 0, Amount);
		VendorSellCart.RemoveAll([Guid](const auto& Entry) { return Entry.Value == Guid; });
		VendorSellSelectedGuid = Guid;
		ActiveVendorPage = 2;
		SyncVendorPageVisibility();
		RefreshVendorOverlays();
		PostInventorySystemMessage(TEXT("Splitting the selected amount before selling it."));
		return;
	}
	for (TPair<int32, int32>& P : VendorSellCart)
	{
		if (P.Value == Guid)
		{
			P.Key = Amount;
			VendorSellSelectedGuid = Guid;
			ActiveVendorPage = 2;
			SyncVendorPageVisibility();
			RefreshVendorOverlays();
			return;
		}
	}
	VendorSellCart.Emplace(Amount, Guid);
	VendorSellSelectedGuid = Guid;
	ActiveVendorPage = 2;
	SyncVendorPageVisibility();
	RefreshVendorOverlays();
}

void UACEUIGameplayBinder::SellVendorCart(bool bSelectedOnly)
{
	if (!Client || OpenVendorGuid == 0)
	{
		return;
	}
	if (VendorSellSplit.SourceGuid != 0 || TradeStackSplit.SourceGuid != 0)
	{
		PostInventorySystemMessage(TEXT("Please wait for the stack to finish splitting."));
		return;
	}
	if (VendorSellCart.Num() == 0)
	{
		if (VendorSellSelectedGuid != 0)
		{
			AddInventoryGuidToVendorSellCart(VendorSellSelectedGuid);
		}
		else if (LastSelection.bValid)
		{
			AddInventoryGuidToVendorSellCart(LastSelection.Guid);
		}
	}
	if (VendorSellCart.Num() == 0)
	{
		return;
	}
	if (bSelectedOnly)
	{
		auto* Entry = VendorSellCart.FindByPredicate([&](const auto& P){return P.Value==VendorSellSelectedGuid;});
		if (!Entry) return;
		const int32 Quantity=FMath::Clamp(LastSelection.Guid==Entry->Value ? SelectedStackAmount : Entry->Key,1,Entry->Key);
		FACEWorldObject Item;
		if (!Client->GetWorldObject(Entry->Value, Item)) return;
		if (Quantity > FMath::Max(1, Item.StackSize))
		{
			PostInventorySystemMessage(TEXT("A stack has changed. Clear it from the sell list and add it again."));
			return;
		}
		if (Quantity < FMath::Max(1, Item.StackSize))
		{
			AddInventoryGuidToVendorSellCart(Entry->Value, Quantity);
			if (VendorSellSplit.SourceGuid) VendorSellSplit.bSellWhenReady = true;
			return;
		}
		Client->SendSellItems(OpenVendorGuid, {{Quantity,Entry->Value}});
		Entry->Key-=Quantity;
		VendorSellCart.RemoveAll([](const auto& P){return P.Key<=0;});
		RefreshVendorOverlays();
		return;
	}
	for (const auto& Entry : VendorSellCart)
	{
		FACEWorldObject Item;
		if (!Client->GetWorldObject(Entry.Value, Item) || Entry.Key != FMath::Max(1, Item.StackSize))
		{
			PostInventorySystemMessage(TEXT("A stack has changed. Clear it from the sell list and add it again."));
			return;
		}
	}
	Client->SendSellItems(OpenVendorGuid, VendorSellCart);
	VendorSellCart.Reset();
	VendorSellSelectedGuid = 0;
	RefreshVendorOverlays();
}

void UACEUIGameplayBinder::ShowTradePanel(int32 PartnerGuid)
{
	if (TradePartnerGuid != PartnerGuid) TradeStackSplit = {};
	VendorSellSplit = {};
	TradeSelfOffset = TradeOtherOffset = 0;
	TradePartnerGuid = PartnerGuid;
	bTradeOpen = true;
	OpenLootContainerGuid = 0;
	OpenVendorGuid = 0;
	OpenSalvageToolGuid = 0;
	SalvageQueueGuids.Reset();
	PendingVendorSellGuid = PendingVendorSellTargetGuid = 0;
	VendorSellCart.Reset();
	VendorBuyCart.Reset();
	SyncEnvPanelMode();
	RefreshTradeOverlays();
	if (PendingTradeItemGuid)
	{
		const int32 Item = PendingTradeItemGuid, Amount = PendingTradeItemAmount, Partner = PendingTradeItemPartner;
		PendingTradeItemGuid = PendingTradeItemAmount = PendingTradeItemPartner = 0;
		if (Partner == PartnerGuid && FPlatformTime::Seconds() < PendingTradeItemDeadline)
			AddInventoryGuidToTrade(Item, Amount);
	}
}

void UACEUIGameplayBinder::HideTradePanel(bool bNotifyServer)
{
	TradeStackSplit = {};
	PendingTradeItemGuid = PendingTradeItemAmount = PendingTradeItemPartner = 0;
	bTradeOpen = false;
	RefreshTradeOverlays();
	TradePartnerGuid = 0;
	for (UBorder* B : TradeSelfSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UBorder* B : TradeOtherSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	SyncEnvPanelMode();
	if (bNotifyServer && Client)
	{
		Client->SendCloseTrade();
	}
}

void UACEUIGameplayBinder::HandleExternalContainerOpened(int32 Guid)
{
	FACEWorldObject Opened;
	if(Client && Client->GetWorldObject(Guid,Opened) && Opened.IsCorpse())KeyboardOpenedCorpses.Add(Guid);
	// Retire entries for objects that have left the session's streamed object set.
	if(Client)for(auto It=KeyboardOpenedCorpses.CreateIterator();It;++It)
	{FACEWorldObject O;if(!Client->GetWorldObject(*It,O))It.RemoveCurrent();}
	// A repeated contents snapshot refreshes the existing root, preserving scroll.
	if (Guid != 0 && Guid == OpenLootContainerGuid)
	{
		RefreshExternalContainerOverlays();
		return;
	}
	ShowExternalContainer(Guid);
}

void UACEUIGameplayBinder::HandleExternalContainerClosed(int32 Guid)
{
	// Closing is authoritative: the server stops permitting appraisal and pickup
	// immediately. Proximity cannot keep that container's inventory accessible.
	// A delayed close for the previous corpse must not close the new one.
	if (Guid != 0 && Guid == OpenLootContainerGuid) HideExternalContainer(false);
}

void UACEUIGameplayBinder::HandleVendorOpened(int32 Guid)
{
	// Same vendor ApproachVendor again (Holding Use retries) — refresh merchandise only;
	// do not wipe an in-progress sell cart that drag-drop already filled.
	if (Guid != 0 && Guid == OpenVendorGuid)
	{
		// An approach reply can update currency balances, prices, or stock limits
		// without changing the merchandise icons. Refresh the captions as well.
		LastVendorOverlayHash = ~uint64(0);
		if (LastSelection.bValid && LastSelection.Guid == VendorSelectedGuid)
		{
			SelectedStackMax=GetVendorSelectionLimit(VendorSelectedGuid);
			SelectedStackAmount=SelectedStackMax>0 ? FMath::Clamp(SelectedStackAmount,1,SelectedStackMax) : 0;
			RefreshSelectionOverlay();
		}
		RefreshVendorOverlays();
	}
	else
	{
		ShowVendorPanel(Guid);
	}
	// ClientUISystem::Handle_VendorInfo consumes an offer only for the vendor
	// originally dropped on; an unrelated/late reply must not populate its cart.
	const int32 SellGuid = PendingVendorSellGuid;
	const int32 SellAmount = PendingVendorSellAmount;
	const int32 SellTarget = PendingVendorSellTargetGuid;
	PendingVendorSellGuid = PendingVendorSellTargetGuid = 0;
	PendingVendorSellAmount = INDEX_NONE;
	if (SellGuid != 0 && Guid == SellTarget)
		AddInventoryGuidToVendorSellCart(SellGuid, SellAmount);
}

void UACEUIGameplayBinder::HandleTradeStateChanged(int32 EventType)
{
	RefreshInventoryOverlays();
	using namespace ACEGameEvent;
	if (EventType == OpenTrade || EventType == RegisterTrade)
	{
		const int32 Partner = Client ? Client->GetTradePartnerGuid() : 0;
		ShowTradePanel(Partner);
	}
	else if (EventType == CloseTrade)
	{
		HideTradePanel(false);
	}
	else if (EventType == ResetTrade)
	{
		TradeStackSplit = {};
		RefreshTradeOverlays();
	}
	else
	{
		RefreshTradeOverlays();
	}
}

void UACEUIGameplayBinder::HandleCharacterTitlesChanged()
{
	RefreshTitleOverlays();
}

int32 UACEUIGameplayBinder::GetExternalContainerItemCapacity() const
{
	if (!Client || !OpenLootContainerGuid) return 1;
	const int32 Container = OpenLootSelectedPackGuid ? OpenLootSelectedPackGuid : OpenLootContainerGuid;
	FACEWorldObject Object;
	Client->GetWorldObject(Container, Object);
	// Drawing and input must include the same empty slots, not just owned items.
	if (Object.ItemsCapacity > 0) return FMath::Clamp(Object.ItemsCapacity, 1, 120);
	TArray<FACEWorldObject> Items = Client->GetPackItems(Container);
	if (Container == OpenLootContainerGuid)
		Items.RemoveAll([](const FACEWorldObject& Item) { return (Item.ItemType & ACEItemType::Container) != 0; });
	return FMath::Clamp(Items.Num(), 1, 120);
}

void UACEUIGameplayBinder::SyncExternalContainerScrollbars()
{
	if (!Manager || OpenLootContainerGuid == 0)
	{
		return;
	}
	// Pack-row Up/Down from ScrollTemplate look like tabs — never show on loot.
	TSharedPtr<FACEUIElement> PackScroll = Manager->FindElementUnder(
		TEXT("ExternalContainer"), TEXT("Ext_ContainerListScroll"));
	if (PackScroll.IsValid())
	{
		for (const TSharedPtr<FACEUIElement>& Child : PackScroll->Children)
		{
			if (!Child.IsValid()) { continue; }
			if (Child->ElementName == TEXT("ScrollBar_Up") || Child->ElementName == TEXT("ScrollBar_Down"))
			{
				Child->bVisible = false;
			}
		}
	}
	// Retail loot close (X) at the right of the pack row.
	if (TSharedPtr<FACEUIElement> CloseBtn = Manager->FindElementUnder(
		TEXT("ExternalContainer"), TEXT("CloseExtContainerPanelButton")))
	{
		CloseBtn->bVisible = true;
	}
	Manager->SetElementVisibleByName(TEXT("CloseExtContainerPanelButton"), true);

	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Ext_Container_ItemList"));
	TSharedPtr<FACEUIElement> Bar = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Ext_ItemListScroll"));
	if (!ListEl.IsValid() || !Bar.IsValid())
	{
		return;
	}
	const int32 PageSize = FMath::Max(1, ListEl->Width / 32);
	const int32 Capacity = GetExternalContainerItemCapacity();
	const int32 MaxOff = FMath::Max(0, Capacity - PageSize);
	ExtItemScrollOffset = FMath::Clamp(ExtItemScrollOffset, 0, MaxOff);
	const float Frac = MaxOff > 0
		? static_cast<float>(ExtItemScrollOffset) / static_cast<float>(MaxOff) : 0.f;
	const float VisibleFrac = Capacity > 0
		? FMath::Clamp(static_cast<float>(PageSize) / static_cast<float>(Capacity), 0.08f, 1.f) : 1.f;
	Bar->bVisible = MaxOff > 0;
	if (MaxOff > 0)
	{
		SyncDatScrollbar(Bar, Frac, VisibleFrac);
	}
}

void UACEUIGameplayBinder::RefreshExternalContainerOverlays()
{
	if (!Client || !Manager || !Canvas || !Canvas->WidgetTree || OpenLootContainerGuid == 0)
	{
		if (bLootOverlaysCollapsed)
		{
			return;
		}
		bLootOverlaysCollapsed = true;
		LastLootOverlayHash = ~uint64(0);
		for (UBorder* B : ExtItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : ExtItemSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : ExtItemSlotSelected) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : ExtPackSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : ExtPackSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	const uint64 LootHash = HashLootOverlayState();
	if (LootHash == LastLootOverlayHash)
	{
		return;
	}
	LastLootOverlayHash = LootHash;
	TArray<int32> PackGuids;
	if (const auto Session = Client->GetSession()) Session->GetContainerPackGuids(OpenLootContainerGuid, PackGuids);
	// gmExternalContainerUI returns to the root when the server confirms that
	// the pack being viewed has left this container. Do not show its stale contents.
	if (OpenLootSelectedPackGuid != 0 && OpenLootSelectedPackGuid != OpenLootContainerGuid
		&& !PackGuids.Contains(OpenLootSelectedPackGuid))
	{
		OpenLootSelectedPackGuid = OpenLootContainerGuid;
		ExtItemScrollOffset = 0;
		LastLootOverlayHash = HashLootOverlayState();
	}
	bLootOverlaysCollapsed = false;
	SyncEnvPanelMode();
	constexpr int32 OverlayZ = 120000;
	constexpr int32 Cell = 32;
	const int32 ContGuid = OpenLootSelectedPackGuid != 0 ? OpenLootSelectedPackGuid : OpenLootContainerGuid;
	const int32 SelectedGuid = LastSelection.bValid ? LastSelection.Guid : 0;

	auto PlaceCell = [&](UBorder* B, float ScreenX, float ScreenY, int32 Z)
	{
		if (!B) { return; }
		const float SX = Canvas->GetLastScaleX();
		const float SY = Canvas->GetLastScaleY();
		if (B->GetParent() != Canvas->GetElementLayer())
		{
			Canvas->GetElementLayer()->AddChild(B);
		}
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(B->Slot))
		{
			Slot->SetAnchors(FAnchors(0.f, 0.f));
			Slot->SetPosition(FVector2D(ScreenX * SX, ScreenY * SY));
			Slot->SetSize(FVector2D(static_cast<float>(Cell) * SX, static_cast<float>(Cell) * SY));
			Canvas->SetOverlayOrder(B, Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field")), Z);
		}
	};

	// Nested packs in ContainerList — corpse root icon + optional side packs (no tab chrome).
	TSharedPtr<FACEUIElement> PackList = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("ContainerList"));
	TSharedPtr<FACEUIElement> RootIconEl = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Container"));
	TArray<FACEWorldObject> Nested;
	for (int32 Guid : PackGuids)
	{
		FACEWorldObject Pack;
		if (Client->GetWorldObject(Guid, Pack)) Nested.Add(Pack);
	}
	FACEWorldObject RootContainer;
	Client->GetWorldObject(OpenLootContainerGuid, RootContainer);
	const int32 PackCapacity = FMath::Max(Nested.Num(), RootContainer.ContainersCapacity < 0
		? Nested.Num() + 1 : RootContainer.ContainersCapacity);
	if (RootIconEl.IsValid())
	{
		UBorder* Bg = EnsureIconBorder(ExtItemSlotBgs, 0); // reuse pool index for root pack bg via pack path
		UBorder* Icon = EnsureIconBorder(ExtPackSlots, 0);
		if (Icon)
		{
			ExtPackGuids.SetNum(1 + Nested.Num());
			ExtPackGuids[0] = OpenLootContainerGuid;
			FACEWorldObject RootObj;
			Client->GetWorldObject(OpenLootContainerGuid, RootObj);
			const FIntPoint O = RootIconEl->GetScreenOrigin();
			if (Bg)
			{
				SetItemSlotBackground(Bg, RootObj.IconId != 0 ? &RootObj : nullptr);
				Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
				PlaceCell(Bg, static_cast<float>(O.X), static_cast<float>(O.Y), OverlayZ);
			}
			if (RootObj.IconId) SetItemSlotForeground(Icon, &RootObj);
			else SetIconDid(Icon, DidEmptyPackSlot);
			Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
			Icon->SetRenderOpacity(ContGuid == OpenLootContainerGuid ? 1.f : 0.55f);
			PlaceCell(Icon, static_cast<float>(O.X), static_cast<float>(O.Y), OverlayZ + 1);
		}
	}
	if (PackList.IsValid())
	{
		const FIntPoint Origin = PackList->GetScreenOrigin();
		const int32 MaxVis = FMath::Max(1, PackList->Width / Cell);
		ExtPackScrollOffset = FMath::Clamp(ExtPackScrollOffset, 0, FMath::Max(0, PackCapacity - MaxVis));
		ExtPackGuids.SetNum(1 + MaxVis);
		for (int32 i = 0; i < MaxVis; ++i)
		{
			UBorder* Icon = EnsureIconBorder(ExtPackSlots, i + 1);
			UBorder* Bg = EnsureIconBorder(ExtPackSlotBgs, i);
			if (!Icon) { continue; }
			const int32 Idx = i + ExtPackScrollOffset;
			if (Idx >= PackCapacity)
			{
				Icon->SetVisibility(ESlateVisibility::Collapsed);
				if (Bg) { Bg->SetVisibility(ESlateVisibility::Collapsed); }
				ExtPackGuids[i + 1] = 0;
				continue;
			}
			const FACEWorldObject* Pack = Nested.IsValidIndex(Idx) ? &Nested[Idx] : nullptr;
			ExtPackGuids[i + 1] = Pack ? Pack->Guid : 0;
			if (Pack) SetItemSlotForeground(Icon, Pack);
			else SetIconDid(Icon, DidEmptyPackSlot);
			SetItemSlotBackground(Bg, Pack);
			if (Bg)
			{
				Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
				PlaceCell(Bg, static_cast<float>(Origin.X + i * Cell), static_cast<float>(Origin.Y), OverlayZ);
			}
			Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
			Icon->SetRenderOpacity(!Pack || ContGuid == Pack->Guid ? 1.f : 0.55f);
			SetRetailTooltip(Icon, Pack ? FText::FromString(ACERetailObjectNames::Name(*Pack)) : FText::FromString(TEXT("Empty pack slot")));
			PlaceCell(Icon,
				static_cast<float>(Origin.X + i * Cell),
				static_cast<float>(Origin.Y),
				OverlayZ + 1);
		}
		for (int32 i = MaxVis + 1; i < ExtPackSlots.Num(); ++i)
		{
			if (ExtPackSlots[i]) { ExtPackSlots[i]->SetVisibility(ESlateVisibility::Collapsed); }
		}
		for (int32 i = MaxVis; i < ExtPackSlotBgs.Num(); ++i)
		{
			if (ExtPackSlotBgs[i]) { ExtPackSlotBgs[i]->SetVisibility(ESlateVisibility::Collapsed); }
		}
	}

	TSharedPtr<FACEUIElement> ItemList = Manager->FindElementUnder(TEXT("ExternalContainer"), TEXT("Ext_Container_ItemList"));
	if (!ItemList.IsValid())
	{
		SyncExternalContainerScrollbars();
		return;
	}
	TArray<FACEWorldObject> Items = Client->GetPackItems(ContGuid);
	if (ContGuid == OpenLootContainerGuid)
	{
		Items.RemoveAll([](const FACEWorldObject& O)
		{
			return (O.ItemType & ACEItemType::Container) != 0;
		});
	}
	const int32 Capacity = GetExternalContainerItemCapacity();
	const int32 PageSize = FMath::Max(1, ItemList->Width / Cell);
	const int32 MaxOff = FMath::Max(0, Capacity - PageSize);
	ExtItemScrollOffset = FMath::Clamp(ExtItemScrollOffset, 0, MaxOff);
	const FIntPoint Origin = ItemList->GetScreenOrigin();
	ExtItemGuids.SetNum(PageSize);
	// Item + empty slot backgrounds (DidInvSlotBg) + selection frame (DidInvSlotSelected).
	constexpr int32 ItemBgBase = 1;
	for (int32 i = 0; i < PageSize; ++i)
	{
		UBorder* Bg = EnsureIconBorder(ExtItemSlotBgs, ItemBgBase + i);
		UBorder* Icon = EnsureIconBorder(ExtItemSlots, i);
		UBorder* Sel = EnsureIconBorder(ExtItemSlotSelected, i);
		if (!Bg || !Icon || !Sel) { continue; }
		const int32 Idx = i + ExtItemScrollOffset;
		const bool bInCapacity = Idx < Capacity;
		ExtItemGuids[i] = (bInCapacity && Items.IsValidIndex(Idx)) ? Items[Idx].Guid : 0;
		if (!bInCapacity)
		{
			Bg->SetVisibility(ESlateVisibility::Collapsed);
			Icon->SetVisibility(ESlateVisibility::Collapsed);
			Sel->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
		PlaceCell(Bg,
			static_cast<float>(Origin.X + i * Cell),
			static_cast<float>(Origin.Y),
			OverlayZ);
		const int32 Guid = ExtItemGuids[i];
		const FACEWorldObject* ExtObj = (Guid != 0 && Items.IsValidIndex(Idx)) ? &Items[Idx] : nullptr;
		SetItemSlotBackground(Bg, ExtObj);
		SetItemSlotForeground(Icon, ExtObj);
		Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (Guid != 0)
		{
			SetRetailTooltip(Icon, FText::FromString(ACERetailObjectNames::Name(Items[Idx])));
		}
		else
		{
			SetRetailTooltip(Icon, FText());
		}
		PlaceCell(Icon,
			static_cast<float>(Origin.X + i * Cell),
			static_cast<float>(Origin.Y),
			OverlayZ + 1);
		const bool bSelected = Guid != 0 && Guid == SelectedGuid;
		Sel->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bSelected)
		{
			SetIconDid(Sel, DidInvSlotSelected);
			PlaceCell(Sel,
				static_cast<float>(Origin.X + i * Cell),
				static_cast<float>(Origin.Y),
				OverlayZ + 2);
		}
	}
	for (int32 i = PageSize; i < ExtItemSlots.Num(); ++i)
	{
		if (ExtItemSlots[i]) { ExtItemSlots[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}
	for (int32 i = ItemBgBase + PageSize; i < ExtItemSlotBgs.Num(); ++i)
	{
		if (ExtItemSlotBgs[i]) { ExtItemSlotBgs[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}
	for (int32 i = PageSize; i < ExtItemSlotSelected.Num(); ++i)
	{
		if (ExtItemSlotSelected[i]) { ExtItemSlotSelected[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}
	SyncExternalContainerScrollbars();
}

void UACEUIGameplayBinder::RefreshVendorOverlays()
{
	if (!Client || !Manager || !Canvas || OpenVendorGuid == 0)
	{
		if (bVendorOverlaysCollapsed)
		{
			return;
		}
		bVendorOverlaysCollapsed = true;
		LastVendorOverlayHash = ~uint64(0);
		for (UBorder* B : VendorItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : VendorItemSlotBgs) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : VendorItemSelections) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		HideVendorTextOverlays();
		return;
	}
	const uint64 VendorHash = HashVendorOverlayState();
	if (VendorHash == LastVendorOverlayHash)
	{
		PlaceVendorFilterCaption();
		if (bVendorFilterDropdownOpen)
		{
			RefreshVendorFilterDropdown();
		}
		return;
	}
	LastVendorOverlayHash = VendorHash;
	bVendorOverlaysCollapsed = false;
	SyncEnvPanelMode();
	constexpr int32 OverlayZ = 120000;
	constexpr int32 Cell = 32;

	RebuildVendorVisibleFilters();

	const TCHAR* ListParent = TEXT("VendorItemsPage");
	const TCHAR* ListName = TEXT("VendorItemsList");
	if (ActiveVendorPage == 1)
	{
		ListParent = TEXT("VendorBuyPage");
		ListName = TEXT("VendorBuyList");
	}
	else if (ActiveVendorPage == 2)
	{
		ListParent = TEXT("VendorSellPage");
		ListName = TEXT("VendorSellList");
	}

	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(ListParent, ListName);
	if (!ListEl.IsValid())
	{
		ListEl = Manager->FindElementByName(ListName);
	}
	if (!ListEl.IsValid() && ActiveVendorPage != 0)
	{
		// Fall back to merchandise list geometry if buy/sell list missing.
		ListEl = Manager->FindElementUnder(TEXT("VendorItemsPage"), TEXT("VendorItemsList"));
	}
	if (!ListEl.IsValid())
	{
		PlaceVendorFilterCaption();
		return;
	}

	TArray<FACEWorldObject> Items;
	if (ActiveVendorPage == 2)
	{
		// Sell page: only items the player dragged into the sell cart.
		for (const TPair<int32, int32>& P : VendorSellCart)
		{
			FACEWorldObject Obj;
			if (Client->GetWorldObject(P.Value, Obj))
			{
				Obj.StackSize=P.Key;
				Items.Add(Obj);
			}
		}
	}
	else if (ActiveVendorPage == 1)
	{
		// Buying page: staged purchase list (not merchandise).
		for (const TPair<int32, int32>& P : VendorBuyCart)
		{
			FACEWorldObject Obj;
			if (Client->GetWorldObject(P.Value, Obj))
			{
				Obj.StackSize=P.Key;
				Items.Add(Obj);
			}
		}
	}
	else
	{
		Items = Client->GetVendorMerchandise();
		if (Items.Num() == 0)
		{
			Items = Client->GetPackItems(OpenVendorGuid);
		}
		RebuildVendorVisibleFilters();
		Items.RemoveAll([&](const FACEWorldObject& O)
		{
			return !VendorItemMatchesFilter(O, VendorItemTypeFilter);
		});
	}

	PlaceVendorFilterCaption();
	if (bVendorFilterDropdownOpen)
	{
		RefreshVendorFilterDropdown();
	}
	RefreshVendorInfoTexts();

	const int32 FocusGuid = (ActiveVendorPage == 2)
		? (VendorSellSelectedGuid != 0 ? VendorSellSelectedGuid
			: (LastSelection.bValid ? LastSelection.Guid : 0))
		: VendorSelectedGuid;

	const int32 Cols = FMath::Max(1, ListEl->Width / Cell);
	const int32 Rows = FMath::Max(1, ListEl->Height / Cell);
	const int32 PageSize = Cols * Rows;
	int32* ScrollOffset = &VendorItemsScrollOffset;
	if (ActiveVendorPage == 1) { ScrollOffset = &VendorBuyScrollOffset; }
	else if (ActiveVendorPage == 2) { ScrollOffset = &VendorSellScrollOffset; }
	const int32 MaxOff = FMath::Max(0, Items.Num() - PageSize);
	*ScrollOffset = FMath::Clamp(*ScrollOffset, 0, MaxOff);
	const FIntPoint Origin = ListEl->GetScreenOrigin();
	const float SX = Canvas->GetLastScaleX();
	const float SY = Canvas->GetLastScaleY();
	VendorItemGuids.SetNum(PageSize);
	auto PlaceCell = [&](UBorder* Border, float X, float Y, int32 Z)
	{
		if (!Border) { return; }
		if (Border->GetParent() != Canvas->GetElementLayer())
		{
			Canvas->GetElementLayer()->AddChild(Border);
		}
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Border->Slot))
		{
			Slot->SetAnchors(FAnchors(0.f, 0.f));
			Slot->SetPosition(FVector2D(X * SX, Y * SY));
			Slot->SetSize(FVector2D(static_cast<float>(Cell) * SX, static_cast<float>(Cell) * SY));
			Canvas->SetOverlayOrder(Border, Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field")), Z);
		}
	};
	for (int32 i = 0; i < PageSize; ++i)
	{
		UBorder* Bg = EnsureIconBorder(VendorItemSlotBgs, i);
		UBorder* Icon = EnsureIconBorder(VendorItemSlots, i);
		UBorder* Selection = EnsureIconBorder(VendorItemSelections, i);
		Selection->SetVisibility(ESlateVisibility::Collapsed);
		if (!Bg || !Icon) { continue; }
		const int32 Idx = i + *ScrollOffset;
		const int32 Col = i % Cols;
		const int32 Row = i / Cols;
		const float CellX = static_cast<float>(Origin.X + Col * Cell);
		const float CellY = static_cast<float>(Origin.Y + Row * Cell);
		if (!Items.IsValidIndex(Idx))
		{
			SetItemSlotBackground(Bg, nullptr);
			Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
			PlaceCell(Bg, CellX, CellY, OverlayZ);
			Icon->SetVisibility(ESlateVisibility::Collapsed);
			VendorItemGuids[i] = 0;
			continue;
		}
		VendorItemGuids[i] = Items[Idx].Guid;
		SetItemSlotBackground(Bg, &Items[Idx]);
		Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
		PlaceCell(Bg, CellX, CellY, OverlayZ);
		SetItemSlotForeground(Icon, &Items[Idx]);
		Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
		SetRetailTooltip(Icon, FText::FromString(ACERetailObjectNames::Name(Items[Idx])));
		Icon->SetRenderOpacity(Items[Idx].Guid == FocusGuid ? 1.f : 0.85f);
		PlaceCell(Icon, CellX, CellY, OverlayZ + 1);
		if (Items[Idx].Guid == FocusGuid)
		{
			SetIconDid(Selection, DidInvSlotSelected);
			Selection->SetVisibility(ESlateVisibility::HitTestInvisible);
			PlaceCell(Selection, CellX, CellY, OverlayZ + 2);
		}
	}
	for (int32 i = PageSize; i < VendorItemSelections.Num(); ++i)
		if (VendorItemSelections[i]) VendorItemSelections[i]->SetVisibility(ESlateVisibility::Collapsed);
	for (int32 i = PageSize; i < VendorItemSlots.Num(); ++i)
	{
		if (VendorItemSlots[i]) { VendorItemSlots[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}
	for (int32 i = PageSize; i < VendorItemSlotBgs.Num(); ++i)
	{
		if (VendorItemSlotBgs[i]) { VendorItemSlotBgs[i]->SetVisibility(ESlateVisibility::Collapsed); }
	}
	SyncVendorScrollbars();
	RefreshVendorTabLabels();
}

void UACEUIGameplayBinder::SyncVendorScrollbars()
{
	if (!Manager || OpenVendorGuid == 0)
	{
		return;
	}
	const TCHAR* ScrollName = TEXT("Vendor_ItemListScroll");
	int32 Offset = VendorItemsScrollOffset;
	if (ActiveVendorPage == 1)
	{
		ScrollName = TEXT("Vendor_BuyListScroll");
		Offset = VendorBuyScrollOffset;
	}
	else if (ActiveVendorPage == 2)
	{
		ScrollName = TEXT("Vendor_SellListScroll");
		Offset = VendorSellScrollOffset;
	}
	TSharedPtr<FACEUIElement> Bar = Manager->FindElementByName(ScrollName);
	if (!Bar.IsValid())
	{
		return;
	}
	constexpr int32 Cell = 32;
	TSharedPtr<FACEUIElement> ListEl;
	if (ActiveVendorPage == 0)
	{
		ListEl = Manager->FindElementUnder(TEXT("VendorItemsPage"), TEXT("VendorItemsList"));
	}
	else if (ActiveVendorPage == 1)
	{
		ListEl = Manager->FindElementUnder(TEXT("VendorBuyPage"), TEXT("VendorBuyList"));
	}
	else
	{
		ListEl = Manager->FindElementUnder(TEXT("VendorSellPage"), TEXT("VendorSellList"));
	}
	int32 Capacity = 0;
	if (Client)
	{
		if (ActiveVendorPage == 2)
		{
			Capacity = VendorSellCart.Num();
		}
		else if (ActiveVendorPage == 1)
		{
			Capacity = VendorBuyCart.Num();
		}
		else
		{
			Capacity = Client->GetVendorMerchandise().Num();
			if (Capacity == 0)
			{
				Capacity = Client->GetPackItems(OpenVendorGuid).Num();
			}
		}
	}
	const int32 PageSize = ListEl.IsValid()
		? FMath::Max(1, (ListEl->Width / Cell) * FMath::Max(1, ListEl->Height / Cell))
		: 1;
	const int32 MaxOff = FMath::Max(0, Capacity - PageSize);
	const float Frac = MaxOff > 0 ? static_cast<float>(Offset) / static_cast<float>(MaxOff) : 0.f;
	const float VisibleFrac = Capacity > 0
		? FMath::Clamp(static_cast<float>(PageSize) / static_cast<float>(Capacity), 0.08f, 1.f) : 1.f;
	Bar->bVisible = true;
	SyncDatScrollbar(Bar, Frac, VisibleFrac);
}

bool UACEUIGameplayBinder::CanEditInscription() const
{
	if (!Client || !Client->GetSession() || !LastAppraisal.bSuccess || LastAppraisal.bIsCreature) return false;
	FACEWorldObject Item;
	if (!Client->GetWorldObject(LastAppraisal.ObjectGuid, Item)) return false;
	if (!LastAppraisal.BoolProperties.FindRef(22) && !(Item.ObjectDescriptionFlags & 2)) return false;
	const FString Scribe = LastAppraisal.StringProperties.FindRef(8);
	const FString Account = LastAppraisal.StringProperties.FindRef(23);
	if (!Scribe.IsEmpty() && !Scribe.Equals(Client->GetSession()->GetPlayerName(), ESearchCase::IgnoreCase)
		&& (Account.IsEmpty() || !Account.Equals(Client->GetSession()->GetAccountName(), ESearchCase::IgnoreCase))) return false;
	// Packs may be nested. Do not allow writing items offered by another player.
	TSet<int32> Seen;
	while (!Seen.Contains(Item.Guid))
	{
		Seen.Add(Item.Guid);
		if (Item.ContainerId == Client->GetPlayerGuid() || Item.WielderId == Client->GetPlayerGuid()) return true;
		if (!Item.ContainerId || !Client->GetWorldObject(Item.ContainerId, Item)) break;
	}
	return false;
}

void UACEUIGameplayBinder::BeginInscriptionEdit()
{
	if (!CanEditInscription() || !Canvas || !Canvas->WidgetTree) return;
	if (EditingInscriptionGuid == LastAppraisal.ObjectGuid) return;
	if (!ExamInscriptionEditor)
	{
		ExamInscriptionEditor = Canvas->WidgetTree->ConstructWidget<UACERetailTextEntry>();
		ExamInscriptionEditor->bMultiline = true;
		ExamInscriptionEditor->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleInscriptionCommitted);
	}
	const auto Element = Manager->FindElementUnder(TEXT("RootGameplay_FloatyExamination_Field"), TEXT("ItemInscriptionText"));
	ExamInscriptionEditor->SetRetailElement(Canvas->GetResourceResolver(), Element);
	ExamInscriptionEditor->TextColor = FLinearColor::Black;
	EditingInscriptionGuid = LastAppraisal.ObjectGuid;
	EditingInscriptionOriginal = LastAppraisal.Inscription;
	ExamInscriptionEditor->SetText(FText::FromString(EditingInscriptionOriginal));
	RefreshExaminationOverlay();
	ExamInscriptionEditor->SetKeyboardFocus();
}

void UACEUIGameplayBinder::HandleInscriptionCommitted(const FText& Text, ETextCommit::Type Method)
{
	const int32 Guid = EditingInscriptionGuid;
	if (!Guid) return;
	EditingInscriptionGuid = 0; // Collapsing the editor can also trigger focus loss.
	if (Method != ETextCommit::OnCleared && Guid == LastAppraisal.ObjectGuid && CanEditInscription()
		&& Text.ToString() != EditingInscriptionOriginal)
	{
		Client->SendSetInscription(Guid, Text.ToString());
		LastAppraisal.Inscription = Text.ToString();
		LastAppraisal.StringProperties.Add(7, Text.ToString());
		LastAppraisal.StringProperties.Add(8, Text.IsEmpty() ? FString() : Client->GetSession()->GetPlayerName());
		// No writing acknowledgement exists; read back the authoritative inscription.
		Client->SendIdentifyObject(Guid);
	}
	if (ExamInscriptionEditor) ExamInscriptionEditor->SetVisibility(ESlateVisibility::Collapsed);
	RefreshExaminationOverlay();
}

void UACEUIGameplayBinder::RefreshTradeOverlays()
{
	if (!Client || !Manager || !Canvas || !bTradeOpen)
	{
		for (auto* Array : {&TradeSelfSlots, &TradeOtherSlots, &TradeSelfBackgrounds, &TradeOtherBackgrounds, &TradeSelfSelections, &TradeOtherSelections})
			for (const auto& Widget : *Array) if (Widget) Widget->SetVisibility(ESlateVisibility::Collapsed);
		for (auto* Array : {&TradeLabels, &TradeSelfCounts, &TradeOtherCounts})
			for (const auto& Widget : *Array) if (Widget) Widget->SetVisibility(ESlateVisibility::Collapsed);
		if (Manager) if (auto Indicator=Manager->FindElementByName(TEXT("TradeOtherTradeIndicator"))) Indicator->bVisible=false;
		return;
	}
	SyncEnvPanelMode();
	constexpr int32 OverlayZ=120000, Cell=32;
	const FVector2D Scale=Canvas->GetLastScale2D();
	const auto Root=Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field"));
	auto Fill=[&](const TCHAR* ListName, const TCHAR* BarName, TArray<TObjectPtr<UBorder>>& Icons,
		TArray<TObjectPtr<UBorder>>& Backgrounds, TArray<TObjectPtr<UBorder>>& Selections, TArray<TObjectPtr<UTextBlock>>& Counts,
		TArray<int32>& Guids, const TArray<int32>& Offers, int32& Offset, bool bSelf)
	{
		const auto List=Manager->FindElementByName(ListName);
		if (!List) return;
		// gmSecureTradeUI: one horizontal item list, including an empty self slot.
		const int32 Page=FMath::Max(1,List->Width/Cell);
		const int32 Capacity=Offers.Num()+(bSelf ? 1 : 0);
		const int32 MaxOffset=FMath::Max(0,Capacity-Page);
		Offset=FMath::Clamp(Offset,0,MaxOffset);
		Guids.SetNumZeroed(Page);
		const FIntPoint Origin=List->GetScreenOrigin();
		for (int32 I=0; I<Page; ++I)
		{
			UBorder* Icon=EnsureIconBorder(Icons,I);
			UBorder* Bg=EnsureIconBorder(Backgrounds,I);
			UBorder* Selection=EnsureIconBorder(Selections,I);
			while (Counts.Num()<=I) Counts.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
			UTextBlock* Count=Counts[I];
			FACEWorldObject Item;
			const bool bItem=Offers.IsValidIndex(Offset+I) && Client->GetWorldObject(Offers[Offset+I],Item);
			Guids[I]=bItem ? Item.Guid : 0;
			SetIconDid(Selection,DidInvSlotSelected);
			Selection->SetVisibility(bItem && Client->GetSelectedObject().Guid==Item.Guid ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			SetItemSlotBackground(Bg,bItem ? &Item : nullptr);
			SetItemSlotForeground(Icon,bItem ? &Item : nullptr);
			Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
			Icon->SetVisibility(bItem ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			SetRetailTooltip(Icon, FText::FromString(ACERetailObjectNames::Name(Item)));
			Count->SetText(FText::AsNumber(Item.StackSize));
			Count->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 7));
			Count->SetColorAndOpacity(TextWhite);
			Count->SetJustification(ETextJustify::Right);
			Count->SetVisibility(bItem && Item.StackSize>1 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			for (UWidget* Widget : {static_cast<UWidget*>(Bg),static_cast<UWidget*>(Icon),static_cast<UWidget*>(Count),static_cast<UWidget*>(Selection)})
			{
				if (Widget->GetParent()!=Canvas->GetElementLayer()) Canvas->GetElementLayer()->AddChild(Widget);
				if (auto Slot=Cast<UCanvasPanelSlot>(Widget->Slot))
				{
					Slot->SetAnchors(FAnchors(0,0));
					Slot->SetPosition(FVector2D(Origin.X+I*Cell,Origin.Y+(Widget==Count ? 20 : 0))*Scale);
					Slot->SetSize(FVector2D(Cell,Widget==Count ? 12 : Cell)*Scale);
					Canvas->SetOverlayOrder(Widget,Root,OverlayZ+(Widget==Bg ? -1 : Widget==Selection ? 2 : Widget==Count ? 1 : 0));
				}
			}
		}
		for (auto* Array : {&Icons,&Backgrounds,&Selections}) for (int32 I=Page; I<Array->Num(); ++I) (*Array)[I]->SetVisibility(ESlateVisibility::Collapsed);
		for (int32 I=Page; I<Counts.Num(); ++I) Counts[I]->SetVisibility(ESlateVisibility::Collapsed);
		if (auto Bar=Manager->FindElementByName(BarName))
		{
			Bar->bVisible=MaxOffset>0;
			SyncDatScrollbar(Bar,MaxOffset ? float(Offset)/MaxOffset : 0.f,Capacity ? FMath::Min(1.f,float(Page)/Capacity) : 1.f);
		}
	};
	Fill(TEXT("TradeSelfItemsList"),TEXT("TradeSelf_ItemListScroll"),TradeSelfSlots,TradeSelfBackgrounds,TradeSelfSelections,TradeSelfCounts,TradeSelfGuids,Client->GetTradeSelfItems(),TradeSelfOffset,true);
	Fill(TEXT("TradeOtherItemsList"),TEXT("TradeOther_ItemListScroll"),TradeOtherSlots,TradeOtherBackgrounds,TradeOtherSelections,TradeOtherCounts,TradeOtherGuids,Client->GetTradePartnerItems(),TradeOtherOffset,false);
	while (TradeLabels.Num()<6) TradeLabels.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	FACEWorldObject Partner; Client->GetWorldObject(TradePartnerGuid,Partner);
	PlaceTextOnElement(TradeLabels[0],TEXT("TradeOtherPlayerName"),Partner.Name,10,TextWhite,OverlayZ+2);
	PlaceTextOnElement(TradeLabels[1],TEXT("TradeSelfPlayerName"),TEXT("Your offer"),10,TextWhite,OverlayZ+2);
	PlaceTextOnElement(TradeLabels[2],TEXT("TradeSelfTotalItemsLabel"),FString::Printf(TEXT("Total items: %d"),Client->GetTradeSelfItems().Num()),10,TextWhite,OverlayZ+2);
	PlaceTextOnElement(TradeLabels[3],TEXT("TradeOtherTotalItemsLabel"),FString::Printf(TEXT("Total items: %d"),Client->GetTradePartnerItems().Num()),10,TextWhite,OverlayZ+2);
	PlaceTextOnElement(TradeLabels[4],TEXT("TradeSelfTradeButton"),TEXT("Trade"),10,TextWhite,OverlayZ+2,true);
	PlaceTextOnElement(TradeLabels[5],TEXT("Trade_ClearAllButon"),TEXT("Clear All"),10,TextWhite,OverlayZ+2,true);
	if (auto Indicator=Manager->FindElementByName(TEXT("TradeOtherTradeIndicator")))
		Indicator->bVisible=Client->HasPartnerAcceptedTrade();
	if (auto Button=Manager->FindElementByName(TEXT("TradeSelfTradeButton")))
		Button->bHighlighted=Client->HasAcceptedTrade();
}

bool UACEUIGameplayBinder::IsPointerOverExternalContainer(FVector2D CanvasLocalPos) const
{
	if (!Manager || OpenLootContainerGuid == 0)
	{
		return false;
	}
	TSharedPtr<FACEUIElement> Root = Manager->FindElementByName(TEXT("ExternalContainer"));
	if (!Root.IsValid() || !Root->bVisible)
	{
		return false;
	}
	const FIntPoint O = Root->GetScreenOrigin();
	return CanvasLocalPos.X >= O.X && CanvasLocalPos.Y >= O.Y
		&& CanvasLocalPos.X < O.X + Root->Width && CanvasLocalPos.Y < O.Y + Root->Height;
}

bool UACEUIGameplayBinder::ScrollExternalContainer(float WheelDelta)
{
	if (OpenLootContainerGuid == 0)
	{
		return false;
	}
	const int32 Dir = (WheelDelta > 0.f) ? -1 : 1;
	ExtItemScrollOffset = FMath::Max(0, ExtItemScrollOffset + Dir * 3);
	RefreshExternalContainerOverlays();
	return true;
}


void UACEUIGameplayBinder::RefreshServerConfirmation()
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	if (!Session || Session->GetState()!=EACESessionState::InWorld)
	{ PendingAllegianceAction.Reset(); PendingAllegiancePrompt.Reset(); PendingAllegianceGuid=0; bHousePaymentConfirm=false; bChessQuitConfirm=false; PendingChatConfirmation=0;ChatConfirmationStage=0; }
	if (!Session || Session->GetState()!=EACESessionState::InWorld || (Session->GetConfirmations().IsEmpty() && !PendingAllegianceGuid && !bHousePaymentConfirm && !bChessQuitConfirm && !PendingChatConfirmation))
	{
		if (ServerConfirmRoot) ServerConfirmRoot->bVisible = false;
		for (UTextBlock* Label : ServerConfirmLabels) if (Label) Label->SetVisibility(ESlateVisibility::Collapsed);
		ServerConfirmPrompt.Reset(); return;
	}
	if (!Manager || !Canvas || !Canvas->WidgetTree) return;
	if (!ServerConfirmRoot)
	{
		ServerConfirmRoot = UACEUILayoutResolver::LoadTemplate(0x2100003C, 0x15);
		if (!ServerConfirmRoot) return;
		ServerConfirmRoot->SetElementName(TEXT("RootGameplay_FloatyServerConfirmation_Field"));
		TFunction<void(TSharedPtr<FACEUIElement>)> NameControls = [&](TSharedPtr<FACEUIElement> Node)
		{
			if (Node->ElementId == 0x3D)
			{
				Node->LeftEdge = Node->RightEdge = Node->TopEdge = Node->BottomEdge = 3;
			}
			if (Node->ElementId == 0x3E) Node->SetElementName(TEXT("ServerConfirmationBody"));
			if (Node->ElementId == 0x17) Node->SetElementName(TEXT("ServerConfirmationYes"));
			if (Node->ElementId == 0x19) Node->SetElementName(TEXT("ServerConfirmationNo"));
			for (const auto& Child : Node->Children) NameControls(Child);
		};
		NameControls(ServerConfirmRoot);
		// Join the gameplay window stack so DAT chrome and text share one paint base.
		if (auto GameplayRoot=Manager->FindElementByName(TEXT("RootGameplay_Field"))) GameplayRoot->AddChild(ServerConfirmRoot);
		else Manager->AddRoot(ServerConfirmRoot);
		for (int32 I=0; I<3; ++I)
			ServerConfirmLabels.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	}

	const bool bLocal=PendingAllegianceGuid!=0 || bHousePaymentConfirm || bChessQuitConfirm || PendingChatConfirmation;
	const uint32 PendingType=bLocal ? 0 : Session->GetConfirmations()[0].Type;
	const uint32 PendingContext=bLocal ? uint32(bChessQuitConfirm?ChessBoardGuid:(bHousePaymentConfirm?OpenHouseLord:PendingAllegianceGuid)) : Session->GetConfirmations()[0].Context;
	const FString ChatPrompt=PendingChatConfirmation==ACEGameAction::Suicide
		? TEXT("Do you really want to kill your character? You may drop items and accrue a vitae penalty.")
		: ChatConfirmationStage==1
		? TEXT("Do you really want to abandon your house? Items on hooks or in storage will stay with the house, and you will lose access to them.")
		: TEXT("Are you absolutely certain you wish to abandon your house? Click Yes only if you are sure.");
	const FString PendingPrompt=PendingChatConfirmation ? ChatPrompt : bLocal ? (bChessQuitConfirm?FString(TEXT("Are you sure you want to resign this game?")):(bHousePaymentConfirm?HouseConfirmPrompt:PendingAllegiancePrompt)) : Session->GetConfirmations()[0].Prompt;
	if (!ServerConfirmRoot->bVisible || ServerConfirmType != PendingType || ServerConfirmContext != PendingContext || ServerConfirmPrompt != PendingPrompt)
	{
		ServerConfirmType = PendingType; ServerConfirmContext = PendingContext; ServerConfirmPrompt = PendingPrompt;
		ServerConfirmRoot->bVisible = true;
		const auto Body = Manager->FindElementByName(TEXT("ServerConfirmationBody"));
		FACEDatFont Font;
		if (Body && Canvas->GetResourceResolver()->ResolveFont(Body->FontId, Font))
		{
			const int32 H = ACEDatText::Layout(Font, ServerConfirmPrompt, Body->Width-Body->TextMargins.Left-Body->TextMargins.Right, false).Num()*Font.MaxCharHeight
				+ Body->TextMargins.Top+Body->TextMargins.Bottom;
			const int32 Delta = H-Body->Height;
			if (auto Box = Body->Parent.Pin())
			{
				Box->Height += Delta; Box->LayoutAuthoredH = Box->LastReflowH = Box->Height;
				for (const auto& Child : Box->Children)
				{
					if (Child->TopEdge == 2) Child->Y += Delta;
					else if (Child->BottomEdge == 1) Child->Height += Delta;
					Child->LayoutAuthoredH = Child->LastReflowH = Child->Height;
				}
			}
		}
		Manager->BringFloatyToFront(ServerConfirmRoot);
		if (PlayerController && PlayerController->GetPawn())
			if (auto* VR = PlayerController->GetPawn()->FindComponentByClass<UACEVRComponent>(); VR && VR->IsActive())
				VR->OpenRetailPanel(NAME_None);
	}
	PlaceTextOnElement(ServerConfirmLabels[0], TEXT("ServerConfirmationBody"), ServerConfirmPrompt, 12, TextWhite, 200020);
	PlaceTextOnElement(ServerConfirmLabels[1], TEXT("ServerConfirmationYes"), TEXT("Yes"), 12, TextWhite, 200020, true);
	PlaceTextOnElement(ServerConfirmLabels[2], TEXT("ServerConfirmationNo"), TEXT("No"), 12, TextWhite, 200020, true);
}

void UACEUIGameplayBinder::FinishServerConfirmation(bool bAccept)
{
	if (PendingChatConfirmation)
	{
		if (bAccept && PendingChatConfirmation==ACEGameAction::AbandonHouse && ChatConfirmationStage==1)
		{
			ChatConfirmationStage=2; RefreshServerConfirmation(); return;
		}
		const uint32 Action=PendingChatConfirmation;
		PendingChatConfirmation=0;ChatConfirmationStage=0;
		if (bAccept && Client) if (auto S=Client->GetSession()) S->SendSimpleGameAction(Action);
	}
	else if (bChessQuitConfirm)
	{
		bChessQuitConfirm=false;
		if (bAccept && Client && bChessActive && !bChessQuitRequested)
		{
			bChessQuitRequested=true;
			Client->SendChessQuit();
		}
	}
	else if (bHousePaymentConfirm)
	{
		bHousePaymentConfirm=false;
		if (bAccept && Client) if (auto S=Client->GetSession()) S->SendHousePayment(bHouseRentTab);
	}
	else if (PendingAllegianceGuid)
	{
		if (bAccept && CanActivateAllegianceControl(PendingAllegianceAction,PendingAllegianceGuid))
		{
			if (PendingAllegianceAction==TEXT("SwearButton")) Client->SendSwearAllegiance(PendingAllegianceGuid);
			else Client->SendBreakAllegiance(PendingAllegianceGuid);
		}
		PendingAllegianceGuid=0; PendingAllegianceAction.Reset(); PendingAllegiancePrompt.Reset();
	}
	else if (Client) if (auto Session = Client->GetSession()) Session->RespondToConfirmation(ServerConfirmType, ServerConfirmContext, bAccept);
	RefreshServerConfirmation();
}
