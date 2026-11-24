#include "UI/ACEGameHUDWidget.h"
#include "ACERuntimeOptions.h"
#include "ACERetailChat.h"
#include "UI/ACERadarColors.h"
#include "ACEHoverTooltipWidget.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACERetailObjectNames.h"
#include "UI/ACEItemDragIcon.h"
#include "UI/ACEAppraisalFormatting.h"
#include "ACEClientSubsystem.h"
#include "ACEInventoryRules.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "ACEOpcodes.h"
#include "ACETypes.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACECombatStance.h"
#include "ACEPlayerController.h"
#include "Dat/ACEDatTextureResolver.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateColor.h"
#include "Misc/ConfigCacheIni.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	// Fallback chrome for elements whose art fails to decode (warm retail browns).
	const FLinearColor PanelColor(0.075f, 0.065f, 0.055f, 0.92f);
	const FLinearColor SideBarColor(0.16f, 0.13f, 0.09f, 0.95f);
	const FLinearColor ButtonColor(0.22f, 0.18f, 0.11f, 1.f);
	const FLinearColor ButtonHoverColor(0.34f, 0.28f, 0.16f, 1.f);
	const FLinearColor BarBackColor(0.04f, 0.035f, 0.03f, 0.95f);
	const FLinearColor HealthColor(0.62f, 0.08f, 0.06f, 1.f);
	const FLinearColor StaminaColor(0.78f, 0.58f, 0.10f, 1.f);
	const FLinearColor ManaColor(0.12f, 0.28f, 0.72f, 1.f);
	const FLinearColor HUDTextColor(0.92f, 0.89f, 0.80f, 1.f);
	const FLinearColor GoldColor(0.87f, 0.74f, 0.35f, 1.f);
	const FLinearColor LabelColor(0.72f, 0.68f, 0.56f, 1.f);
	const FLinearColor ChatSystemColor = ACERetailChat::Color(ACEChatMessageType::Broadcast);
	const FLinearColor ChatSpeechColor = ACERetailChat::Color(ACEChatMessageType::Speech);
	const FLinearColor RadarBackColor(0.03f, 0.05f, 0.03f, 0.92f);

	// --- Retail texture DIDs (Docs/UI/UIAssetManifest.csv) ---
	// classic_statusbar / classic_vitals
	constexpr uint32 DidStatusBar        = 0x06004CE4;
	constexpr uint32 DidHealthFillL      = 0x06001131, DidHealthFillM = 0x06001132, DidHealthFillR = 0x06001133;
	constexpr uint32 DidHealthBackL      = 0x06001141, DidHealthBackM = 0x06001140, DidHealthBackR = 0x0600113F;
	constexpr uint32 DidStamFillL        = 0x06001137, DidStamFillM = 0x06001138, DidStamFillR = 0x06001139;
	constexpr uint32 DidStamBackL        = 0x06001147, DidStamBackM = 0x06001146, DidStamBackR = 0x06001145;
	constexpr uint32 DidManaFillL        = 0x06001134, DidManaFillM = 0x06001135, DidManaFillR = 0x06001136;
	constexpr uint32 DidManaBackL        = 0x06001144, DidManaBackM = 0x06001143, DidManaBackR = 0x06001142;
	// classic_indicators
	constexpr uint32 DidIndPortalStorm   = 0x060016D0;
	constexpr uint32 DidIndMiniGame      = 0x06002817;
	constexpr uint32 DidIndVitae         = 0x06001106;
	constexpr uint32 DidIndPositive      = 0x06001111;
	constexpr uint32 DidIndNegative      = 0x06002629;
	constexpr uint32 DidIndBurden        = 0x06001103;
	constexpr uint32 DidIndLink          = 0x06001360;
	constexpr uint32 DidWorldMap         = 0x0600127D;
	constexpr uint32 DidMapPlayerIcon    = 0x06004D10;
	constexpr uint32 DidHelpButton       = 0x06001148, DidHelpButtonDown = 0x0600114A;
	constexpr uint32 DidLogoffButton     = 0x06001932, DidLogoffButtonDown = 0x06001933;
	// classic_panel
	constexpr uint32 DidPanelSideBar     = 0x0600114C;
	constexpr uint32 DidPanelBackground  = 0x0600114D;
	constexpr uint32 DidRadarBackground  = 0x06004CC1; // classic_floatyradar 120x120 (NOT panel 300x120)
	constexpr uint32 DidRadarCoords      = 0x06004CC0;
	constexpr uint32 DidRadarNorth       = 0x060011FB;
	constexpr uint32 DidRadarEast        = 0x06001938;
	constexpr uint32 DidRadarSouth       = 0x0600193A;
	constexpr uint32 DidRadarWest        = 0x0600193C;
	constexpr uint32 HUDDidLockUi           = 0x060074B7;
	constexpr uint32 DidLockUiDown       = 0x060074B8;
	constexpr uint32 DidOpenContainerOverlay = 0x06005D9C;
	// classic_toolbar
	constexpr uint32 DidPeaceMode        = 0x06004CEC, DidPeaceModeDown = 0x06004CED;
	constexpr uint32 DidMeleeMode        = 0x06004CEE, DidMeleeModeDown = 0x06004CEF;
	constexpr uint32 DidMissileMode      = 0x06004CF0, DidMissileModeDown = 0x06004CF1;
	constexpr uint32 DidMagicMode        = 0x06004CF2, DidMagicModeDown = 0x06004CF3;
	constexpr uint32 DidSocialButton     = 0x0600111F, DidSocialButtonDown = 0x06001121;
	constexpr uint32 DidMagicButton      = 0x06001119, DidMagicButtonDown = 0x0600111B;
	constexpr uint32 DidSkillButton      = 0x06001122, DidSkillButtonDown = 0x06001124;
	constexpr uint32 DidQuestButton      = 0x060069AE, DidQuestButtonDown = 0x060069AF;
	constexpr uint32 DidWorldButton      = 0x06001116, DidWorldButtonDown = 0x06001118;
	constexpr uint32 DidOptionsButton    = 0x0600111C, DidOptionsButtonDown = 0x0600111E;
	constexpr uint32 DidInventoryButton  = 0x06004CF7, DidInventoryButtonDown = 0x06004CF8;
	constexpr uint32 DidExamineButton    = 0x06001127, DidExamineButtonDown = 0x06001128;
	constexpr uint32 DidUseButton        = 0x06001129, DidUseButtonDown = 0x0600112A;
	constexpr uint32 DidSelectedField    = 0x06001126;
	constexpr uint32 DidShortcutSpacer   = 0x06004CC2;
	// classic_chat
	constexpr uint32 DidChatTopBorder    = 0x06001125;
	constexpr uint32 DidChatSmoke        = 0x0600114D;
	constexpr uint32 DidChatEntryField   = 0x0600113A;
	constexpr uint32 DidChatTarget       = 0x06004D65;
	constexpr uint32 DidChatSend         = 0x06001915, DidChatSendDown = 0x06001916;
	// classic_spellcasting / classic_combat
	constexpr uint32 DidSpellSlotBg      = 0x06004CC2;
	constexpr uint32 DidSpellSelected    = 0x06004D09;
	constexpr uint32 DidSpellTabInactive = 0x06005EB8;
	constexpr uint32 DidSpellTabActive   = 0x06005EB9;
	constexpr uint32 DidCastBtn          = 0x06004D1C, DidCastBtnDown = 0x06004D1D;

	// classic_attribute / StatManagement_Template
	constexpr uint32 DidAttrIconStrength = 0x060002C8;
	constexpr uint32 DidAttrIconEndurance = 0x060002C4;
	constexpr uint32 DidAttrIconQuickness = 0x060002C6;
	constexpr uint32 DidAttrIconCoordination = 0x060002C9;
	constexpr uint32 DidAttrIconFocus = 0x060002C5;
	constexpr uint32 DidAttrIconSelf = 0x060002C7;
	constexpr uint32 DidAttrIconHealth = 0x06004C3B;
	constexpr uint32 DidAttrIconStamina = 0x06004C3C;
	constexpr uint32 DidAttrIconMana = 0x06004C3D;
	constexpr uint32 DidAttrHeaderDivider = 0x06004CB8;
	constexpr uint32 DidAttrXpMeterBack = 0x060011A6;
	constexpr uint32 DidAttrXpMeterFill = 0x060011A5;
	constexpr uint32 DidAttrRaiseMeterFill = 0x06000F8A;
	constexpr uint32 DidAttrRaise1 = 0x06004CB6, DidAttrRaise1Down = 0x06004CB7;
	constexpr uint32 DidAttrRaise10 = 0x0600712B, DidAttrRaise10Down = 0x0600712C;
	constexpr uint32 DidAttrRowBg = 0x06000F93;

	struct FAttrRowDef
	{
		const TCHAR* Name;
		const TCHAR* Hover;
		uint32 IconDid;
		int32 AttributeId; // 1..6 primary, 0 = vital (use VitalId)
		int32 VitalId;     // 1/3/5 for Health/Stamina/Mana Max*
	};
	const FAttrRowDef AttrRows[9] =
	{
		{ TEXT("Strength"), TEXT("Strength raises melee damage and how much you can carry."), DidAttrIconStrength, 1, 0 },
		{ TEXT("Endurance"), TEXT("Endurance raises Health and Stamina pools."), DidAttrIconEndurance, 2, 0 },
		{ TEXT("Quickness"), TEXT("Quickness raises run speed and melee defense."), DidAttrIconQuickness, 3, 0 },
		{ TEXT("Coordination"), TEXT("Coordination raises missile accuracy and many craft skills."), DidAttrIconCoordination, 4, 0 },
		{ TEXT("Focus"), TEXT("Focus raises magic accuracy and mana conversion."), DidAttrIconFocus, 5, 0 },
		{ TEXT("Self"), TEXT("Self raises Mana and resistance to mental attacks."), DidAttrIconSelf, 6, 0 },
		{ TEXT("Health"), TEXT("Health is your life. Raised with Endurance and Health XP."), DidAttrIconHealth, 0, 1 },
		{ TEXT("Stamina"), TEXT("Stamina fuels attacks and movement. Raised with Endurance and Stamina XP."), DidAttrIconStamina, 0, 3 },
		{ TEXT("Mana"), TEXT("Mana powers spells. Raised with Self and Mana XP."), DidAttrIconMana, 0, 5 },
	};

	// Meter geometry: classic_vitals meters (260 wide) squeezed into the 580px statusbar
	// vitals field (x' = 10 + x * 0.725) → three 188px meters. Caps from the retail art.
	struct FVitalGeom { int32 X, Y, W, H, CapL, CapR; };
	const FVitalGeom GVitals[3] =
	{
		{ 10, 1, 188, 26, 10, 8 },   // Health
		{ 206, 1, 188, 26, 10, 8 },  // Stamina
		{ 402, 1, 188, 26, 9, 10 },  // Mana
	};

	// classic_panel radar area — local to the radar window.
	constexpr int32 RadarLocalCenterX = 60;
	constexpr int32 RadarLocalCenterY = 60;
	constexpr int32 RadarRadiusPx = 52;
	constexpr float RadarRangeAc = 60.f;
	constexpr int32 MaxRadarBlips = 32;
	constexpr int32 MaxChatLines = 120;

	// Paper doll slot → EquipMask, shared by the inventory refresh and drag & drop.
	const int64 GDollMasks[8] =
	{
		ACEEquipMask::HeadWear,
		ACEEquipMask::ChestArmor | ACEEquipMask::ChestWear,
		ACEEquipMask::UpperArmArmor | ACEEquipMask::LowerArmArmor | ACEEquipMask::UpperArmWear | ACEEquipMask::LowerArmWear,
		ACEEquipMask::HandWear,
		ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded | ACEEquipMask::Held | ACEEquipMask::MissileWeapon,
		ACEEquipMask::Shield,
		ACEEquipMask::UpperLegArmor | ACEEquipMask::LowerLegArmor | ACEEquipMask::UpperLegWear | ACEEquipMask::LowerLegWear,
		ACEEquipMask::FootWear,
	};

	FSlateBrush MakeRoundedBrush(const FLinearColor& Color, float Radius)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		return Brush;
	}
}

TSharedRef<SWidget> UACEGameHUDWidget::RebuildWidget()
{
	BuildLayout();
	return Super::RebuildWidget();
}

void UACEGameHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (UGameInstance* GI = GetGameInstance())
	{
		Client = GI->GetSubsystem<UACEClientSubsystem>();
		Dat = GI->GetSubsystem<UACEDatSubsystem>();
	}
	if (Client && !bChatDelegateBound)
	{
		Client->OnRoutedChatMessage.AddDynamic(this, &UACEGameHUDWidget::HandleChatMessage);
		Client->OnSelectionChanged.AddDynamic(this, &UACEGameHUDWidget::HandleSelectionChanged);
		Client->OnAppraisal.AddDynamic(this, &UACEGameHUDWidget::HandleAppraisal);
		bChatDelegateBound = true;
	}

	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	if (Client) for (const auto& Message : Client->GetChatHistory())
		HandleChatMessage(Message.Text, Message.Sender, Message.Type, Message.FilterType);
	EnsureArtApplied();
}

void UACEGameHUDWidget::NativeDestruct()
{
	ClearInspectPreview();
	if (InspectPreviewActor)
	{
		InspectPreviewActor->Destroy();
		InspectPreviewActor = nullptr;
		InspectCapture = nullptr;
	}
	InspectRenderTarget = nullptr;
	if (PaperDollPreviewActor)
	{
		PaperDollPreviewActor->Destroy();
		PaperDollPreviewActor = nullptr;
		PaperDollCapture = nullptr;
	}
	PaperDollRenderTarget = nullptr;
	PaperDollAppliedGuid = 0;
	PaperDollAppliedHash = 0;
	SaveWindowLayout();
	if (Client && bChatDelegateBound)
	{
		Client->OnRoutedChatMessage.RemoveDynamic(this, &UACEGameHUDWidget::HandleChatMessage);
		Client->OnSelectionChanged.RemoveDynamic(this, &UACEGameHUDWidget::HandleSelectionChanged);
		Client->OnAppraisal.RemoveDynamic(this, &UACEGameHUDWidget::HandleAppraisal);
		bChatDelegateBound = false;
	}
	Super::NativeDestruct();
}

UTexture2D* UACEGameHUDWidget::UiTex(uint32 TextureDid)
{
	if (!Dat)
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			Dat = GI->GetSubsystem<UACEDatSubsystem>();
		}
	}
	if (Dat && Dat->IsDatReady())
	{
		if (FACEDatTextureResolver* Resolver = Dat->GetTextureResolver())
		{
			return Resolver->GetOrCreateUiTexture(TextureDid);
		}
	}
	return nullptr;
}

FSlateBrush UACEGameHUDWidget::MakeFallbackBrush(const FLinearColor& Color, float Radius) const
{
	return MakeRoundedBrush(Color, Radius);
}

FSlateBrush UACEGameHUDWidget::MakeTexBrush(UTexture2D* Texture, EACEHudArtTile Tile) const
{
	FSlateBrush Brush;
	// Box (not Image) fills the widget rect the way retail UI frames do; ImageSize drives tiling.
	Brush.DrawAs = ESlateBrushDrawType::Box;
	Brush.TintColor = FSlateColor(FLinearColor::White);
	Brush.Margin = FMargin(0.f);
	Brush.SetResourceObject(Texture);
	if (Texture)
	{
		Brush.ImageSize = FVector2D(static_cast<float>(Texture->GetSizeX()), static_cast<float>(Texture->GetSizeY()));
	}
	switch (Tile)
	{
	case EACEHudArtTile::Horizontal:
		Brush.Tiling = ESlateBrushTileType::Horizontal;
		break;
	case EACEHudArtTile::Vertical:
		Brush.Tiling = ESlateBrushTileType::Vertical;
		break;
	case EACEHudArtTile::Both:
		Brush.Tiling = ESlateBrushTileType::Both;
		break;
	default:
		Brush.Tiling = ESlateBrushTileType::NoTile;
		break;
	}
	return Brush;
}

void UACEGameHUDWidget::BindArt(UWidget* Widget, uint32 NormalDid, uint32 PressedDid, EACEHudArtTile Tile,
	const FLinearColor& Fallback, bool bIsButton)
{
	if (!Widget || NormalDid == 0)
	{
		return;
	}
	FHudArtBinding Binding;
	Binding.Widget = Widget;
	Binding.NormalDid = NormalDid;
	Binding.PressedDid = PressedDid;
	Binding.Tile = Tile;
	Binding.Fallback = Fallback;
	Binding.bIsButton = bIsButton;
	Binding.bApplied = false;
	ArtBindings.Add(Binding);
	bAllArtApplied = false;
}

bool UACEGameHUDWidget::ApplyArtBinding(FHudArtBinding& Binding)
{
	UWidget* Widget = Binding.Widget.Get();
	if (!Widget)
	{
		return true;
	}
	UTexture2D* NormalTex = UiTex(Binding.NormalDid);
	if (!NormalTex)
	{
		return false;
	}

	if (Binding.bIsButton)
	{
		if (UButton* Button = Cast<UButton>(Widget))
		{
			FButtonStyle Style = Button->GetStyle();
			const FSlateBrush Normal = MakeTexBrush(NormalTex, Binding.Tile);
			FSlateBrush Hovered = Normal;
			Hovered.TintColor = FSlateColor(FLinearColor(1.12f, 1.12f, 1.12f, 1.f));
			FSlateBrush Pressed = Normal;
			if (UTexture2D* PressedTex = Binding.PressedDid ? UiTex(Binding.PressedDid) : nullptr)
			{
				Pressed = MakeTexBrush(PressedTex, Binding.Tile);
			}
			Style.Normal = Normal;
			Style.Hovered = Hovered;
			Style.Pressed = Pressed;
			Style.NormalPadding = FMargin(0.f);
			Style.PressedPadding = FMargin(0.f);
			Button->SetStyle(Style);
			return true;
		}
		return false;
	}

	if (UBorder* Border = Cast<UBorder>(Widget))
	{
		Border->SetBrush(MakeTexBrush(NormalTex, Binding.Tile));
		Border->SetBrushColor(FLinearColor::White);
		return true;
	}
	return false;
}

void UACEGameHUDWidget::EnsureArtApplied()
{
	if (bAllArtApplied || ArtBindings.Num() == 0)
	{
		return;
	}
	if (!Dat || !Dat->IsDatReady())
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			Dat = GI->GetSubsystem<UACEDatSubsystem>();
		}
		if (!Dat || !Dat->IsDatReady())
		{
			return;
		}
	}

	bool bAll = true;
	int32 Applied = 0;
	for (FHudArtBinding& Binding : ArtBindings)
	{
		if (Binding.bApplied)
		{
			continue;
		}
		if (ApplyArtBinding(Binding))
		{
			Binding.bApplied = true;
			++Applied;
		}
		else
		{
			bAll = false;
		}
	}
	if (Applied > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("ACEHUD: applied %d UI textures (pending=%d)"),
			Applied, ArtBindings.Num() - Applied);
	}
	bAllArtApplied = bAll;
}

UBorder* UACEGameHUDWidget::AddPanel(int32 X, int32 Y, int32 W, int32 H, const FLinearColor& Color)
{
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Panel->SetBrush(MakeFallbackBrush(Color));
	Panel->SetBrushColor(Color);
	Panel->SetPadding(FMargin(0.f));
	Panel->OnMouseButtonDownEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseDown);
	Panel->OnMouseMoveEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseMove);
	Panel->OnMouseButtonUpEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseUp);
	RootCanvas->AddChild(Panel);
	Track(Panel, X, Y, W, H);
	return Panel;
}

UBorder* UACEGameHUDWidget::AddArt(int32 X, int32 Y, int32 W, int32 H, uint32 TextureDid, const FLinearColor& Fallback,
	EACEHudArtTile Tile)
{
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Panel->SetBrush(MakeFallbackBrush(Fallback));
	Panel->SetBrushColor(Fallback);
	Panel->SetPadding(FMargin(0.f));
	Panel->OnMouseButtonDownEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseDown);
	Panel->OnMouseMoveEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseMove);
	Panel->OnMouseButtonUpEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseUp);
	RootCanvas->AddChild(Panel);
	Track(Panel, X, Y, W, H);
	BindArt(Panel, TextureDid, 0, Tile, Fallback, false);
	// Try immediately in case DAT is already open.
	if (FHudArtBinding* Last = ArtBindings.Num() > 0 ? &ArtBindings.Last() : nullptr)
	{
		if (ApplyArtBinding(*Last))
		{
			Last->bApplied = true;
		}
	}
	return Panel;
}

UTextBlock* UACEGameHUDWidget::AddCenteredLabel(UWidget* Parent, const FString& Text, const FLinearColor& Color, int32 FontSize)
{
	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
	Label->SetText(FText::FromString(Text));
	Label->SetColorAndOpacity(FSlateColor(Color));
	Label->SetJustification(ETextJustify::Center);
	FSlateFontInfo Font = Label->GetFont();
	Font.Size = FontSize;
	Label->SetFont(Font);
	if (UBorder* Border = Cast<UBorder>(Parent))
	{
		Border->SetContent(Label);
		Border->SetVerticalAlignment(VAlign_Center);
		Border->SetHorizontalAlignment(HAlign_Center);
	}
	return Label;
}

UTextBlock* UACEGameHUDWidget::AddCanvasLabel(int32 X, int32 Y, int32 W, int32 H, const FString& Text, const FLinearColor& Color, int32 FontSize, ETextJustify::Type Justify)
{
	UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
	Label->SetText(FText::FromString(Text));
	Label->SetColorAndOpacity(FSlateColor(Color));
	Label->SetJustification(Justify);
	FSlateFontInfo Font = Label->GetFont();
	Font.Size = FontSize;
	Label->SetFont(Font);
	Label->SetVisibility(ESlateVisibility::HitTestInvisible);
	RootCanvas->AddChild(Label);
	Track(Label, X, Y, W, H);
	return Label;
}

UButton* UACEGameHUDWidget::AddToolButton(int32 X, int32 Y, int32 W, int32 H, const FString& Text, int32 FontSize,
	uint32 NormalDid, uint32 PressedDid)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	FButtonStyle Style = Button->GetStyle();
	Style.Normal = MakeFallbackBrush(ButtonColor, 3.f);
	Style.Hovered = MakeFallbackBrush(ButtonHoverColor, 3.f);
	Style.Pressed = MakeFallbackBrush(SideBarColor, 3.f);
	Style.NormalPadding = FMargin(0.f);
	Style.PressedPadding = FMargin(0.f);
	Button->SetStyle(Style);

	if (!Text.IsEmpty())
	{
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Label->SetText(FText::FromString(Text));
		Label->SetColorAndOpacity(FSlateColor(HUDTextColor));
		Label->SetJustification(ETextJustify::Center);
		// Labels must not steal clicks — the entire button texture is the hit target.
		Label->SetVisibility(ESlateVisibility::HitTestInvisible);
		FSlateFontInfo Font = Label->GetFont();
		Font.Size = FontSize;
		Label->SetFont(Font);
		Button->AddChild(Label);
	}

	RootCanvas->AddChild(Button);
	Track(Button, X, Y, W, H);
	if (NormalDid != 0)
	{
		BindArt(Button, NormalDid, PressedDid, EACEHudArtTile::None, ButtonColor, true);
		if (FHudArtBinding* Last = ArtBindings.Num() > 0 ? &ArtBindings.Last() : nullptr)
		{
			if (ApplyArtBinding(*Last))
			{
				Last->bApplied = true;
			}
		}
	}
	return Button;
}

void UACEGameHUDWidget::Track(UWidget* Widget, int32 X, int32 Y, int32 W, int32 H)
{
	FHudPiece Piece;
	Piece.Widget = Widget;
	Piece.VirtualRect = FVector4(X, Y, W, H);
	Piece.WindowIndex = BuildWindowIndex;
	Pieces.Add(Piece);
}

void UACEGameHUDWidget::TrackPinnedTopLeft(UWidget* Widget, int32 X, int32 Y, int32 W, int32 H)
{
	Track(Widget, X, Y, W, H);
	if (Pieces.Num() > 0)
	{
		Pieces.Last().bPinScreenTopLeft = true;
		Pieces.Last().WindowIndex = INDEX_NONE;
	}
}

void UACEGameHUDWidget::MarkLastPiece(bool bStretchHeight, bool bAnchorBottom)
{
	if (Pieces.Num() > 0)
	{
		Pieces.Last().bPanelStretchHeight = bStretchHeight;
		Pieces.Last().bPanelAnchorBottom = bAnchorBottom;
	}
}

void UACEGameHUDWidget::MarkLastChatPiece(bool bStretchHeight, bool bShiftUp)
{
	if (Pieces.Num() > 0)
	{
		Pieces.Last().bChatStretchHeight = bStretchHeight;
		Pieces.Last().bChatShiftUp = bShiftUp;
	}
}

void UACEGameHUDWidget::ApplyChatExtraHeight()
{
	ChatExtraHeight = FMath::Clamp(ChatExtraHeight, 0.f, 280.f);
	if (Windows.IsValidIndex(WindowChat))
	{
		// Grow upward: window origin rises and height increases so drag hit-tests stay correct.
		Windows[WindowChat].DefaultTopLeft.Y = 500.f - ChatExtraHeight;
		Windows[WindowChat].Size.Y = 100.f + ChatExtraHeight;
	}
}

void UACEGameHUDWidget::BuildLayout()
{
	if (bLayoutBuilt || !WidgetTree)
	{
		return;
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		if (!Client) { Client = GI->GetSubsystem<UACEClientSubsystem>(); }
		if (!Dat) { Dat = GI->GetSubsystem<UACEDatSubsystem>(); }
	}

	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("HUDRoot"));
	WidgetTree->RootWidget = RootCanvas;
	Pieces.Reset();
	ArtBindings.Reset();
	bAllArtApplied = false;

	Windows.SetNum(WindowCount);
	auto InitWin = [&](int32 Idx, FName Key, float X, float Y, float W, float H)
	{
		Windows[Idx].PersistKey = Key;
		Windows[Idx].DefaultTopLeft = FVector2D(X, Y);
		Windows[Idx].Size = FVector2D(W, H);
		Windows[Idx].Offset = FVector2D::ZeroVector;
	};
	InitWin(WindowVitals, TEXT("Vitals"), 160, 2, 580, 28);
	InitWin(WindowRadar, TEXT("Radar"), 8, 36, 120, 140);
	InitWin(WindowPanel, TEXT("Panel"), 500, 36, 300, 430);
	InitWin(WindowToolbar, TEXT("Toolbar"), 500, 510, 300, 90);
	InitWin(WindowChat, TEXT("Chat"), 0, 500, 490, 100);
	InitWin(WindowInspect, TEXT("Inspect"), 180, 80, 360, 360);
	InitWin(WindowCombat, TEXT("Combat"), 0, 440, 800, 80);

	BuildIndicators();
	BuildVitalsWindow();
	BuildRadarWindow();
	BuildRightPanel();
	BuildToolbar();
	BuildChat();
	BuildCombatHotbar();
	BuildWindowIndex = INDEX_NONE;
	LoadWindowLayout();
	UpdateUiLockVisual();

	bLayoutBuilt = true;
}

void UACEGameHUDWidget::BuildIndicators()
{
	// True viewport top-left (not letterboxed 800×600). Clickable retail indicator icons.
	BuildWindowIndex = INDEX_NONE;
	auto PinLast = [this]()
	{
		if (Pieces.Num() > 0)
		{
			Pieces.Last().bPinScreenTopLeft = true;
			Pieces.Last().WindowIndex = INDEX_NONE;
		}
	};
	UButton* B0 = AddToolButton(4, 2, 20, 28, TEXT(""), 8, DidIndPortalStorm, DidIndPortalStorm);
	PinLast(); B0->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnIndicatorPortalStorm);
	UButton* B1 = AddToolButton(24, 2, 20, 28, TEXT(""), 8, DidIndMiniGame, DidIndMiniGame);
	PinLast(); B1->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnIndicatorMiniGame);
	UButton* B2 = AddToolButton(44, 2, 20, 28, TEXT(""), 8, DidIndVitae, DidIndVitae);
	PinLast(); B2->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnIndicatorVitae);
	UButton* B3 = AddToolButton(64, 2, 20, 28, TEXT(""), 8, DidIndPositive, DidIndPositive);
	PinLast(); B3->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnIndicatorPositive);
	UButton* B4 = AddToolButton(84, 2, 20, 28, TEXT(""), 8, DidIndNegative, DidIndNegative);
	PinLast(); B4->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnIndicatorNegative);
	UButton* B5 = AddToolButton(104, 2, 20, 28, TEXT(""), 8, DidIndBurden, DidIndBurden);
	PinLast(); B5->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnIndicatorBurden);
	UButton* B6 = AddToolButton(124, 2, 20, 28, TEXT(""), 8, DidIndLink, DidIndLink);
	PinLast(); B6->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnIndicatorLink);
	UButton* HelpButton = AddToolButton(148, 2, 32, 28, TEXT(""), 9, DidHelpButton, DidHelpButtonDown);
	PinLast();
	HelpButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnHelpClicked);
	UButton* LogoffButton = AddToolButton(180, 2, 32, 28, TEXT(""), 9, DidLogoffButton, DidLogoffButtonDown);
	PinLast();
	LogoffButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnLogoffClicked);
}

void UACEGameHUDWidget::BuildVitalsWindow()
{
	BuildWindowIndex = WindowVitals;
	const int32 OX = 160;
	const int32 OY = 2;

	UBorder* Root = AddArt(OX, OY, 580, 28, DidStatusBar, PanelColor, EACEHudArtTile::Horizontal);
	Windows[WindowVitals].DragSurface = Root;

	struct FMeterArt { uint32 BackL, BackM, BackR, FillL, FillM, FillR; FLinearColor Fallback; };
	const FMeterArt Meters[3] =
	{
		{ DidHealthBackL, DidHealthBackM, DidHealthBackR, DidHealthFillL, DidHealthFillM, DidHealthFillR, HealthColor },
		{ DidStamBackL, DidStamBackM, DidStamBackR, DidStamFillL, DidStamFillM, DidStamFillR, StaminaColor },
		{ DidManaBackL, DidManaBackM, DidManaBackR, DidManaFillL, DidManaFillM, DidManaFillR, ManaColor },
	};
	TArray<TObjectPtr<UBorder>>* FillArrays[3] = { &HealthFillPieces, &StaminaFillPieces, &ManaFillPieces };

	for (int32 i = 0; i < 3; ++i)
	{
		const FVitalGeom& G = GVitals[i];
		const int32 X = OX + (G.X - 10); // GVitals authored for statusbar x=10 origin
		const int32 Y = OY + G.Y;
		AddArt(X, Y, G.CapL, G.H, Meters[i].BackL, BarBackColor);
		AddArt(X + G.CapL, Y, G.W - G.CapL - G.CapR, G.H, Meters[i].BackM, BarBackColor, EACEHudArtTile::Horizontal);
		AddArt(X + G.W - G.CapR, Y, G.CapR, G.H, Meters[i].BackR, BarBackColor);

		FillArrays[i]->Reset();
		const uint32 FillDids[3] = { Meters[i].FillL, Meters[i].FillM, Meters[i].FillR };
		const EACEHudArtTile FillTile[3] = { EACEHudArtTile::None, EACEHudArtTile::Horizontal, EACEHudArtTile::None };
		for (int32 p = 0; p < 3; ++p)
		{
			UBorder* Fill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Fill->SetBrush(MakeFallbackBrush(Meters[i].Fallback, 8.f));
			Fill->SetBrushColor(Meters[i].Fallback);
			Fill->SetPadding(FMargin(0.f));
			Fill->SetVisibility(ESlateVisibility::HitTestInvisible);
			RootCanvas->AddChild(Fill);
			FillArrays[i]->Add(Fill);
			BindArt(Fill, FillDids[p], 0, FillTile[p], Meters[i].Fallback, false);
			if (FHudArtBinding* Last = ArtBindings.Num() > 0 ? &ArtBindings.Last() : nullptr)
			{
				if (ApplyArtBinding(*Last)) { Last->bApplied = true; }
			}
		}
	}

	HealthLabel = AddCanvasLabel(OX, OY + 6, 188, 16, TEXT("Health"), HUDTextColor, 9, ETextJustify::Center);
	StaminaLabel = AddCanvasLabel(OX + 196, OY + 6, 188, 16, TEXT("Stamina"), HUDTextColor, 9, ETextJustify::Center);
	ManaLabel = AddCanvasLabel(OX + 392, OY + 6, 188, 16, TEXT("Mana"), HUDTextColor, 9, ETextJustify::Center);
	BuildWindowIndex = INDEX_NONE;
}

void UACEGameHUDWidget::BuildRadarWindow()
{
	BuildWindowIndex = WindowRadar;
	const int32 OX = 8;
	const int32 OY = 36;

	UBorder* Root = AddArt(OX, OY, 120, 120, DidRadarBackground, PanelColor);
	Windows[WindowRadar].DragSurface = Root;

	// Compass ring labels (retail NESW art).
	AddArt(OX + 52, OY + 2, 16, 16, DidRadarNorth, GoldColor);
	AddArt(OX + 102, OY + 52, 16, 16, DidRadarEast, GoldColor);
	AddArt(OX + 52, OY + 102, 16, 16, DidRadarSouth, GoldColor);
	AddArt(OX + 2, OY + 52, 16, 16, DidRadarWest, GoldColor);

	UBorder* PlayerDot = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	PlayerDot->SetBrush(MakeRoundedBrush(FLinearColor(0.2f, 0.9f, 0.2f, 1.f), 4.f));
	PlayerDot->SetVisibility(ESlateVisibility::HitTestInvisible);
	RootCanvas->AddChild(PlayerDot);
	Track(PlayerDot, OX + RadarLocalCenterX - 2, OY + RadarLocalCenterY - 2, 4, 4);

	RadarBlips.Reset();
	BlipVirtualRects.Reset();
	for (int32 i = 0; i < MaxRadarBlips; ++i)
	{
		UBorder* Blip = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Blip->SetBrush(MakeRoundedBrush(FLinearColor::White, 3.f));
		Blip->SetPadding(FMargin(0.f));
		Blip->SetVisibility(ESlateVisibility::Collapsed);
		RootCanvas->AddChild(Blip);
		RadarBlips.Add(Blip);
		BlipVirtualRects.Add(FVector4(0, 0, 0, 0));
	}

	RadarCoordsText = AddCanvasLabel(OX, OY + 122, 120, 14, TEXT("0.0N, 0.0E"), LabelColor, 8, ETextJustify::Center);

	// Retail LockUI button (27x27 art) — global floaty lock for all windows.
	UiLockButton = AddToolButton(OX + 93, OY + 0, 27, 27, TEXT(""), 8, HUDDidLockUi, DidLockUiDown);
	UiLockButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnUiLockClicked);

	BuildWindowIndex = INDEX_NONE;
}

void UACEGameHUDWidget::BuildRightPanel()
{
	BuildWindowIndex = WindowPanel;
	const int32 OX = 500;
	const int32 OY = 36;

	// classic_panel: sidebar + body (radar lives in its own floaty window now).
	// Leave 8px at the bottom for the resize grip so stretch chrome does not cover it.
	AddArt(OX - 9, OY, 9, 422, DidPanelSideBar, SideBarColor, EACEHudArtTile::Vertical);
	MarkLastPiece(true, false);
	UBorder* Body = AddArt(OX, OY, 300, 422, DidPanelBackground, PanelColor, EACEHudArtTile::Vertical);
	MarkLastPiece(true, false);
	Windows[WindowPanel].DragSurface = Body;

	// Panel pages — hidden until a toolbar button opens one.
	PanelPageRoot = AddPanel(OX, OY + 4, 300, 414, PanelColor);
	MarkLastPiece(true, false);
	PanelPageRoot->SetVisibility(ESlateVisibility::Collapsed);
	PanelPageTitle = AddCanvasLabel(OX, OY + 10, 300, 20, TEXT(""), GoldColor, 12, ETextJustify::Center);
	PanelPageTitle->SetVisibility(ESlateVisibility::Collapsed);

	// Character sheet tabs (Attributes / Skills / Titles) — shown with Character page.
	CharTabAttributes = AddToolButton(OX + 8, OY + 34, 90, 18, TEXT("Attributes"), 8);
	CharTabAttributes->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCharTabAttributes);
	CharTabAttributes->SetVisibility(ESlateVisibility::Collapsed);
	CharTabSkills = AddToolButton(OX + 104, OY + 34, 90, 18, TEXT("Skills"), 8);
	CharTabSkills->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCharTabSkills);
	CharTabSkills->SetVisibility(ESlateVisibility::Collapsed);
	CharTabTitles = AddToolButton(OX + 200, OY + 34, 90, 18, TEXT("Titles"), 8);
	CharTabTitles->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCharTabTitles);
	CharTabTitles->SetVisibility(ESlateVisibility::Collapsed);

	PanelPageBody = AddCanvasLabel(OX + 14, OY + 36, 272, 380, TEXT(""), LabelColor, 9, ETextJustify::Left);
	PanelPageBody->SetVisibility(ESlateVisibility::Collapsed);
	PanelPageBody->SetAutoWrapText(true);
	PanelStatsLabels = AddCanvasLabel(OX + 14, OY + 58, 180, 360, TEXT(""), LabelColor, 10, ETextJustify::Left);
	PanelStatsLabels->SetVisibility(ESlateVisibility::Collapsed);
	PanelStatsValues = AddCanvasLabel(OX + 194, OY + 58, 92, 360, TEXT(""), HUDTextColor, 10, ETextJustify::Right);
	PanelStatsValues->SetVisibility(ESlateVisibility::Collapsed);

	// Scrollable skill list (retail classic_skills) — leaves room for the XP raise footer.
	SkillsScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	SkillsScroll->SetVisibility(ESlateVisibility::Collapsed);
	RootCanvas->AddChild(SkillsScroll);
	Track(SkillsScroll, OX + 8, OY + 56, 284, 280);
	MarkLastPiece(true, false);

	// Scrollable spellbook list (retail classic_spellbook).
	SpellbookScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	SpellbookScroll->SetVisibility(ESlateVisibility::Collapsed);
	RootCanvas->AddChild(SpellbookScroll);
	Track(SpellbookScroll, OX + 8, OY + 34, 284, 376);
	MarkLastPiece(true, false);

	EnsureAttributeWidgets();
	EnsureWorldMapWidgets();
	BuildInventoryWidgets(OX, OY);

	// Bottom-edge resize grip: drag to make the panel shorter/taller.
	PanelResizeGrip = AddArt(OX, OY + 422, 300, 8, DidPanelSideBar, SideBarColor, EACEHudArtTile::Horizontal);
	MarkLastPiece(false, true);
	UACEHoverTooltipWidget::SetWidgetTooltip(PanelResizeGrip, FText::FromString(TEXT("Drag to resize the panel")));
	if (UCanvasPanelSlot* GripSlot = Cast<UCanvasPanelSlot>(PanelResizeGrip->Slot))
	{
		GripSlot->SetZOrder(50);
	}

	BuildWindowIndex = INDEX_NONE;

	// Floating inspect panel — model viewport + text (retail examination layout).
	BuildWindowIndex = WindowInspect;
	InspectEdgeChrome = AddArt(171, 80, 9, 360, DidPanelSideBar, SideBarColor, EACEHudArtTile::Vertical);
	InspectEdgeChrome->SetVisibility(ESlateVisibility::Collapsed);
	InspectPanel = AddArt(180, 80, 360, 360, DidPanelBackground, PanelColor, EACEHudArtTile::Vertical);
	Windows[WindowInspect].DragSurface = InspectPanel;
	InspectPanel->SetVisibility(ESlateVisibility::Collapsed);
	// Retail examination header strip.
	InspectHeaderArt = AddArt(180, 80, 360, 24, DidChatTopBorder, SideBarColor, EACEHudArtTile::Horizontal);
	InspectHeaderArt->SetVisibility(ESlateVisibility::Collapsed);
	InspectTitle = AddCanvasLabel(188, 84, 320, 18, TEXT(""), GoldColor, 11, ETextJustify::Center);
	InspectTitle->SetVisibility(ESlateVisibility::Collapsed);
	// Model viewport well (dark inset behind the 3D capture).
	InspectModelFrame = AddPanel(194, 112, 144, 184, FLinearColor(0.015f, 0.015f, 0.02f, 1.f));
	InspectModelFrame->SetVisibility(ESlateVisibility::Collapsed);
	InspectModelImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	InspectModelImage->SetVisibility(ESlateVisibility::Collapsed);
	RootCanvas->AddChild(InspectModelImage);
	Track(InspectModelImage, 196, 114, 140, 180);
	InspectBody = AddCanvasLabel(348, 112, 180, 296, TEXT(""), LabelColor, 9, ETextJustify::Left);
	InspectBody->SetVisibility(ESlateVisibility::Collapsed);
	InspectBody->SetAutoWrapText(true);
	UButton* CloseInspect = AddToolButton(518, 83, 18, 18, TEXT("X"), 8);
	CloseInspect->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCloseInspectClicked);
	BuildWindowIndex = INDEX_NONE;
}

void UACEGameHUDWidget::BuildInventoryWidgets(int32 PanelOX, int32 PanelOY)
{
	// Inventory page chrome lives inside the right panel (hidden until backpack is opened).
	InventoryRoot = AddPanel(PanelOX + 4, PanelOY + 4, 292, 414, FLinearColor(0.05f, 0.045f, 0.04f, 0.55f));
	MarkLastPiece(true, false);
	InventoryRoot->SetVisibility(ESlateVisibility::Collapsed);

	InventoryHint = AddCanvasLabel(PanelOX + 8, PanelOY + 8, 200, 16, TEXT("Inventory"), GoldColor, 11, ETextJustify::Left);
	InventoryHint->SetVisibility(ESlateVisibility::Collapsed);

	// Retail paper doll: 3D character in the middle, equipment slots down both sides,
	// bags in a column on the RIGHT edge, item grid along the bottom.
	PaperDollModelFrame = AddPanel(PanelOX + 90, PanelOY + 30, 116, 204, FLinearColor(0.015f, 0.015f, 0.02f, 1.f));
	PaperDollModelFrame->SetVisibility(ESlateVisibility::Collapsed);
	PaperDollModelImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	PaperDollModelImage->SetVisibility(ESlateVisibility::Collapsed);
	RootCanvas->AddChild(PaperDollModelImage);
	Track(PaperDollModelImage, PanelOX + 92, PanelOY + 32, 112, 200);

	PaperDollSlots.Reset();
	PaperDollGuids.Reset();
	struct FDollSlot { int32 X, Y; const TCHAR* Tip; };
	const FDollSlot Doll[] =
	{
		{ PanelOX + 16, PanelOY + 30, TEXT("Head") },
		{ PanelOX + 16, PanelOY + 66, TEXT("Chest") },
		{ PanelOX + 16, PanelOY + 102, TEXT("Arms") },
		{ PanelOX + 16, PanelOY + 138, TEXT("Hands") },
		{ PanelOX + 16, PanelOY + 174, TEXT("Weapon") },
		{ PanelOX + 210, PanelOY + 30, TEXT("Shield") },
		{ PanelOX + 210, PanelOY + 66, TEXT("Legs") },
		{ PanelOX + 210, PanelOY + 102, TEXT("Feet") },
	};
	for (const FDollSlot& S : Doll)
	{
		UBorder* Cell = AddPanel(S.X, S.Y, 32, 32, BarBackColor);
		Cell->SetVisibility(ESlateVisibility::Collapsed);
		UACEHoverTooltipWidget::SetWidgetTooltip(Cell, FText::FromString(S.Tip));
		PaperDollSlots.Add(Cell);
		PaperDollGuids.Add(0);
	}

	// Scrollable pack column (count = ContainersCapacity + main pack).
	PackTabsScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	PackTabsScroll->SetVisibility(ESlateVisibility::Collapsed);
	PackTabsScroll->SetScrollBarVisibility(ESlateVisibility::Visible);
	PackTabsScroll->SetConsumePointerInput(true);
	RootCanvas->AddChild(PackTabsScroll);
	Track(PackTabsScroll, PanelOX + 250, PanelOY + 30, 40, 204);
	MarkLastPiece(true, false);
	PackTabsList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	PackTabsScroll->AddChild(PackTabsList);
	PackTabButtons.Reset();
	PackTabGuids.Reset();
	BuiltPackTabCount = 0;

	// Scrollable item grid — slot count comes from the viewed bag's ItemsCapacity.
	InventoryGridScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	InventoryGridScroll->SetVisibility(ESlateVisibility::Collapsed);
	InventoryGridScroll->SetScrollBarVisibility(ESlateVisibility::Visible);
	InventoryGridScroll->SetConsumePointerInput(true);
	RootCanvas->AddChild(InventoryGridScroll);
	Track(InventoryGridScroll, PanelOX + 12, PanelOY + 240, 230, 168);
	MarkLastPiece(true, true);
	InventoryGridRows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	InventoryGridScroll->AddChild(InventoryGridRows);
	InventoryIconSlots.Reset();
	InventoryCellGuids.Reset();
	BuiltInventorySlotCount = 0;
}

void UACEGameHUDWidget::EnsurePackTabCount(int32 TabCount)
{
	TabCount = FMath::Clamp(TabCount, 1, 32);
	if (!PackTabsList || !WidgetTree)
	{
		return;
	}
	if (BuiltPackTabCount == TabCount && PackTabButtons.Num() == TabCount)
	{
		return;
	}

	PackTabsList->ClearChildren();
	PackTabButtons.Reset();
	PackTabGuids.Reset();
	for (int32 i = 0; i < TabCount; ++i)
	{
		UButton* Tab = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		FButtonStyle Style = Tab->GetStyle();
		Style.Normal = MakeFallbackBrush(BarBackColor, 2.f);
		Style.Hovered = MakeFallbackBrush(ButtonHoverColor, 2.f);
		Style.Pressed = MakeFallbackBrush(SideBarColor, 2.f);
		Style.NormalPadding = FMargin(0.f);
		Style.PressedPadding = FMargin(0.f);
		Tab->SetStyle(Style);
		Tab->SetVisibility(ESlateVisibility::Visible);
		if (USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass()))
		{
			Size->SetWidthOverride(32.f);
			Size->SetHeightOverride(32.f);
			Size->AddChild(Tab);
			if (UVerticalBoxSlot* BoxSlot = PackTabsList->AddChildToVerticalBox(Size))
			{
				BoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
			}
		}
		switch (i)
		{
		case 0: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab0); break;
		case 1: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab1); break;
		case 2: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab2); break;
		case 3: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab3); break;
		case 4: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab4); break;
		case 5: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab5); break;
		case 6: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab6); break;
		case 7: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnPackTab7); break;
		default:
			// Extra packs: resolve via geometry in HandlePackTabMouseDown (button still Visible).
			break;
		}
		PackTabButtons.Add(Tab);
		PackTabGuids.Add(0);
	}
	BuiltPackTabCount = TabCount;
}

void UACEGameHUDWidget::EnsureInventorySlotCount(int32 SlotCount)
{
	SlotCount = FMath::Clamp(SlotCount, 1, 200);
	if (!InventoryGridRows || !WidgetTree)
	{
		return;
	}
	if (BuiltInventorySlotCount == SlotCount && InventoryIconSlots.Num() == SlotCount)
	{
		return;
	}

	InventoryGridRows->ClearChildren();
	InventoryIconSlots.Reset();
	InventoryCellGuids.Reset();
	constexpr int32 Cols = 7;
	constexpr float CellSize = 28.f;
	constexpr float Gap = 2.f;
	const int32 Rows = FMath::DivideAndRoundUp(SlotCount, Cols);
	for (int32 r = 0; r < Rows; ++r)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		for (int32 c = 0; c < Cols; ++c)
		{
			const int32 Idx = r * Cols + c;
			if (Idx >= SlotCount)
			{
				break;
			}
			UBorder* IconCell = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			IconCell->SetBrush(MakeFallbackBrush(BarBackColor));
			IconCell->SetBrushColor(BarBackColor);
			IconCell->SetPadding(FMargin(0.f));
			IconCell->SetVisibility(ESlateVisibility::Visible);
			if (USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass()))
			{
				Size->SetWidthOverride(CellSize);
				Size->SetHeightOverride(CellSize);
				Size->AddChild(IconCell);
				if (UHorizontalBoxSlot* HSlot = Row->AddChildToHorizontalBox(Size))
				{
					HSlot->SetPadding(FMargin(0.f, 0.f, Gap, 0.f));
				}
			}
			InventoryIconSlots.Add(IconCell);
			InventoryCellGuids.Add(0);
		}
		if (UVerticalBoxSlot* VSlot = InventoryGridRows->AddChildToVerticalBox(Row))
		{
			VSlot->SetPadding(FMargin(0.f, 0.f, 0.f, Gap));
		}
	}
	BuiltInventorySlotCount = SlotCount;
}

FEventReply UACEGameHUDWidget::HandlePackTabMouseDown(FGeometry MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		return UWidgetBlueprintLibrary::Unhandled();
	}
	const FVector2D ScreenPos = MouseEvent.GetScreenSpacePosition();
	for (int32 i = 0; i < PackTabButtons.Num(); ++i)
	{
		if (PackTabGuids.IsValidIndex(i) && PackTabGuids[i] != 0
			&& WidgetContains(PackTabButtons[i].Get(), ScreenPos))
		{
			OnInventoryPackClicked(i);
			return UWidgetBlueprintLibrary::Handled();
		}
	}
	return UWidgetBlueprintLibrary::Unhandled();
}

void UACEGameHUDWidget::SetInventorySlotIcon(UBorder* IconCell, int32 IconDid, const FLinearColor& EmptyColor)
{
	if (!IconCell)
	{
		return;
	}
	if (IconDid != 0)
	{
		if (UTexture2D* Tex = UiTex(static_cast<uint32>(IconDid)))
		{
			IconCell->SetBrush(MakeTexBrush(Tex, EACEHudArtTile::None));
			IconCell->SetBrushColor(FLinearColor::White);
			return;
		}
	}
	IconCell->SetBrush(MakeFallbackBrush(EmptyColor));
	IconCell->SetBrushColor(EmptyColor);
}

void UACEGameHUDWidget::RefreshInventoryUI()
{
	if (!Client || !InventoryRoot)
	{
		return;
	}
	const int32 Self = Client->GetPlayerGuid();
	if (Self == 0)
	{
		return;
	}

	TArray<FACEWorldObject> Equipped = Client->GetEquippedItems();
	auto FindEquipped = [&](int64 Mask) -> const FACEWorldObject*
	{
		for (const FACEWorldObject& Obj : Equipped)
		{
			if ((Obj.CurrentWieldedLocation & Mask) != 0)
			{
				return &Obj;
			}
		}
		return nullptr;
	};

	PaperDollGuids.SetNum(PaperDollSlots.Num());
	for (int32 i = 0; i < PaperDollSlots.Num() && i < 8; ++i)
	{
		const FACEWorldObject* Item = FindEquipped(GDollMasks[i]);
		SetInventorySlotIcon(PaperDollSlots[i].Get(), Item ? Item->IconId : 0, BarBackColor);
		PaperDollGuids[i] = Item ? Item->Guid : 0;
		if (UBorder* Cell = PaperDollSlots[i].Get())
		{
			if (Item && !Item->Name.IsEmpty())
			{
				UACEHoverTooltipWidget::SetWidgetTooltip(Cell, FText::FromString(ACERetailObjectNames::Name(*Item)));
			}
		}
	}

	FACEWorldObject SelfObj;
	Client->GetWorldObject(Self, SelfObj);
	const int32 MaxPackTabs = FMath::Clamp(1 + FMath::Max(SelfObj.ContainersCapacity, 0), 1, 32);
	TArray<FACEWorldObject> Packs = Client->GetPlayerPacks();
	const int32 TabCount = FMath::Clamp(1 + Packs.Num(), 1, MaxPackTabs);
	EnsurePackTabCount(TabCount);
	PackTabGuids.SetNum(TabCount);
	PackTabGuids[0] = Self;
	for (int32 i = 1; i < TabCount; ++i)
	{
		PackTabGuids[i] = (i - 1 < Packs.Num()) ? Packs[i - 1].Guid : 0;
	}
	if (SelectedPackGuid == 0 || !PackTabGuids.Contains(SelectedPackGuid))
	{
		SelectedPackGuid = Self;
	}
	for (int32 i = 0; i < PackTabButtons.Num(); ++i)
	{
		UButton* Tab = PackTabButtons[i].Get();
		if (!Tab)
		{
			continue;
		}
		const bool bHas = PackTabGuids.IsValidIndex(i) && PackTabGuids[i] != 0;
		Tab->SetVisibility(bHas ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (!bHas)
		{
			continue;
		}
		uint32 IconDid = (i == 0) ? DidInventoryButton : 0;
		FString TabName = (i == 0) ? TEXT("Main Pack") : FString::Printf(TEXT("Pack %d"), i);
		if (i > 0)
		{
			FACEWorldObject PackObj;
			if (Client->GetWorldObject(PackTabGuids[i], PackObj))
			{
				IconDid = static_cast<uint32>(PackObj.IconId);
				if (!PackObj.Name.IsEmpty())
				{
					TabName = ACERetailObjectNames::Name(PackObj);
				}
			}
		}
		UACEHoverTooltipWidget::SetWidgetTooltip(Tab, FText::FromString(TabName));
		if (UTexture2D* Tex = UiTex(IconDid))
		{
			FButtonStyle Style = Tab->GetStyle();
			Style.Normal = MakeTexBrush(Tex, EACEHudArtTile::None);
			Style.Hovered = Style.Normal;
			Style.Hovered.TintColor = FSlateColor(FLinearColor(1.2f, 1.2f, 1.2f, 1.f));
			Style.Pressed = Style.Normal;
			Tab->SetStyle(Style);
		}
		Tab->SetRenderOpacity(PackTabGuids[i] == SelectedPackGuid ? 1.f : 0.65f);
	}
	if (PackTabsScroll)
	{
		PackTabsScroll->SetVisibility(ESlateVisibility::Visible);
	}

	FACEWorldObject PackObj;
	int32 SlotCount = 35; // retail-ish fallback when capacity missing
	if (Client->GetWorldObject(SelectedPackGuid, PackObj) && PackObj.ItemsCapacity > 0)
	{
		SlotCount = PackObj.ItemsCapacity;
	}
	else if (SelectedPackGuid == Self && SelfObj.ItemsCapacity > 0)
	{
		SlotCount = SelfObj.ItemsCapacity;
	}
	EnsureInventorySlotCount(SlotCount);

	TArray<FACEWorldObject> PackItems = Client->GetPackItems(SelectedPackGuid);
	InventoryCellGuids.SetNum(InventoryIconSlots.Num());
	for (int32 i = 0; i < InventoryIconSlots.Num(); ++i)
	{
		UBorder* IconCell = InventoryIconSlots[i].Get();
		if (!IconCell)
		{
			continue;
		}
		IconCell->SetVisibility(ESlateVisibility::Visible);
		if (i < PackItems.Num())
		{
			SetInventorySlotIcon(IconCell, PackItems[i].IconId, BarBackColor);
			UACEHoverTooltipWidget::SetWidgetTooltip(IconCell, FText::FromString(ACERetailObjectNames::Name(PackItems[i])));
			InventoryCellGuids[i] = PackItems[i].Guid;
		}
		else
		{
			SetInventorySlotIcon(IconCell, 0, BarBackColor);
			UACEHoverTooltipWidget::SetWidgetTooltip(IconCell, FText::GetEmpty());
			InventoryCellGuids[i] = 0;
		}
	}
	if (InventoryGridScroll)
	{
		InventoryGridScroll->SetVisibility(ESlateVisibility::Visible);
	}

	if (InventoryHint)
	{
		InventoryHint->SetText(FText::FromString(
			FString::Printf(TEXT("Inventory   Burden %d   Slots %d"), SelfObj.Burden, SlotCount)));
	}

	UpdatePaperDollPreview();
}

void UACEGameHUDWidget::OnInventoryPackClicked(int32 PackIndex)
{
	if (PackTabGuids.IsValidIndex(PackIndex) && PackTabGuids[PackIndex] != 0)
	{
		SelectedPackGuid = PackTabGuids[PackIndex];
		RefreshInventoryUI();
	}
}

void UACEGameHUDWidget::OnPackTab0() { OnInventoryPackClicked(0); }
void UACEGameHUDWidget::OnPackTab1() { OnInventoryPackClicked(1); }
void UACEGameHUDWidget::OnPackTab2() { OnInventoryPackClicked(2); }
void UACEGameHUDWidget::OnPackTab3() { OnInventoryPackClicked(3); }
void UACEGameHUDWidget::OnPackTab4() { OnInventoryPackClicked(4); }
void UACEGameHUDWidget::OnPackTab5() { OnInventoryPackClicked(5); }
void UACEGameHUDWidget::OnPackTab6() { OnInventoryPackClicked(6); }
void UACEGameHUDWidget::OnPackTab7() { OnInventoryPackClicked(7); }

void UACEGameHUDWidget::BuildToolbar()
{
	BuildWindowIndex = WindowToolbar;
	const int32 OX = 500;
	const int32 OY = 510;

	UBorder* Root = AddPanel(OX, OY, 300, 90, FLinearColor(0.05f, 0.045f, 0.04f, 0.85f));
	Windows[WindowToolbar].DragSurface = Root;

	// Combat mode button (art swaps with stance) at (0,0,55,58).
	CombatModeButton = AddToolButton(OX + 0, OY + 0, 55, 58, TEXT(""), 9, DidPeaceMode, DidPeaceModeDown);
	CombatModeButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatModeClicked);

	// Panel buttons row (y=0..27): Social, Magic, Skills, Quests, World, Options.
	UButton* B = nullptr;
	B = AddToolButton(OX + 55, OY, 35, 27, TEXT(""), 8, DidSocialButton, DidSocialButtonDown);
	B->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSocialClicked);
	B = AddToolButton(OX + 85, OY, 34, 27, TEXT(""), 8, DidMagicButton, DidMagicButtonDown);
	B->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellbookClicked);
	B = AddToolButton(OX + 115, OY, 34, 27, TEXT(""), 8, DidSkillButton, DidSkillButtonDown);
	B->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCharacterClicked);
	B = AddToolButton(OX + 145, OY, 34, 27, TEXT(""), 8, DidQuestButton, DidQuestButtonDown);
	B->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnQuestsClicked);
	B = AddToolButton(OX + 175, OY, 34, 27, TEXT(""), 8, DidWorldButton, DidWorldButtonDown);
	B->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnMapClicked);
	B = AddToolButton(OX + 204, OY, 34, 27, TEXT(""), 8, DidOptionsButton, DidOptionsButtonDown);
	B->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnOptionsClicked);

	// Big inventory backpack button (238,0,63,58).
	InventoryButton = AddToolButton(OX + 238, OY, 62, 58, TEXT(""), 8, DidInventoryButton, DidInventoryButtonDown);
	InventoryButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnInventoryClicked);

	// Selected-object row (y=27): Use, selection field + name/health, Examine.
	UButton* UseBtn = AddToolButton(OX + 55, OY + 27, 23, 31, TEXT(""), 8, DidUseButton, DidUseButtonDown);
	UseBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnUseSelectedClicked);
	SelectedFieldArt = AddArt(OX + 78, OY + 27, 140, 31, DidSelectedField, BarBackColor);
	SelectedNameLabel = AddCanvasLabel(OX + 90, OY + 28, 116, 14, TEXT(""), HUDTextColor, 8, ETextJustify::Left);
	SelectedHealthFill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	SelectedHealthFill->SetBrush(MakeFallbackBrush(HealthColor));
	SelectedHealthFill->SetBrushColor(HealthColor);
	SelectedHealthFill->SetVisibility(ESlateVisibility::Collapsed);
	RootCanvas->AddChild(SelectedHealthFill);
	Track(SelectedHealthFill, OX + 90, OY + 44, 116, 8);
	UButton* ExamineBtn = AddToolButton(OX + 218, OY + 27, 22, 31, TEXT(""), 8, DidExamineButton, DidExamineButtonDown);
	ExamineBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnExamineClicked);

	// Shortcut bar: retail shows one row of 1-9 under the backpack.
	AddArt(OX, OY + 58, 6, 32, DidShortcutSpacer, SideBarColor);
	for (int32 i = 0; i < 9; ++i)
	{
		AddToolButton(OX + 6 + i * 32, OY + 58, 32, 32, FString::FromInt(i + 1), 7);
	}
	AddArt(OX + 294, OY + 58, 6, 32, DidShortcutSpacer, SideBarColor);

	BuildWindowIndex = INDEX_NONE;
}

void UACEGameHUDWidget::BuildCombatHotbar()
{
	BuildWindowIndex = WindowCombat;
	const int32 OX = 0;
	const int32 OY = 440;

	// Retail classic_spellcasting RootSpellcasting_Field: 800x80.
	CombatHotbarRoot = AddPanel(OX, OY, 800, 80, FLinearColor(0.05f, 0.04f, 0.03f, 0.92f));
	Windows[WindowCombat].DragSurface = CombatHotbarRoot;
	CombatHotbarRoot->SetVisibility(ESlateVisibility::Collapsed);
	AddArt(OX, OY + 20, 800, 60, DidSpellSlotBg, BarBackColor, EACEHudArtTile::Both);
	AddArt(OX, OY, 800, 20, DidSpellSlotBg, SideBarColor, EACEHudArtTile::Horizontal);

	static const TCHAR* TabLabels[] = {
		TEXT("I"), TEXT("II"), TEXT("III"), TEXT("IV"),
		TEXT("V"), TEXT("VI"), TEXT("VII"), TEXT("VIII")
	};
	CombatSpellTabButtons.Reset();
	for (int32 i = 0; i < 8; ++i)
	{
		UButton* Tab = AddToolButton(OX + i * 47, OY, 47, 20, TabLabels[i], 8,
			DidSpellTabInactive, DidSpellTabActive);
		switch (i)
		{
		case 0: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab0); break;
		case 1: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab1); break;
		case 2: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab2); break;
		case 3: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab3); break;
		case 4: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab4); break;
		case 5: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab5); break;
		case 6: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab6); break;
		case 7: Tab->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSpellTab7); break;
		default: break;
		}
		CombatSpellTabButtons.Add(Tab);
	}

	CombatSpellButtons.Reset();
	CombatSpellIconCells.Reset();
	CombatSpellSelectedOverlays.Reset();
	CombatSpellSlotNumbers.Reset();
	SelectedCombatSpellSlot = 0;
	LastSpellSlotClickIndex = INDEX_NONE;
	LastSpellSlotClickTime = 0.0;

	for (int32 i = 0; i < 12; ++i)
	{
		const int32 SX = OX + 4 + i * 36;
		const int32 SY = OY + 24;
		UBorder* IconCell = AddArt(SX, SY, 32, 32, 0, BarBackColor);
		// Icons are decorative — buttons above them own the click / drag-drop target.
		IconCell->SetVisibility(ESlateVisibility::HitTestInvisible);
		CombatSpellIconCells.Add(IconCell);

		UButton* SpellSlot = AddToolButton(SX, SY, 32, 32, TEXT(""), 7);
		{
			FButtonStyle Style = SpellSlot->GetStyle();
			FSlateBrush Clear;
			Clear.DrawAs = ESlateBrushDrawType::NoDrawType;
			Style.Normal = Clear;
			Style.Hovered = Clear;
			Style.Pressed = Clear;
			Style.Disabled = Clear;
			SpellSlot->SetStyle(Style);
		}
		switch (i)
		{
		case 0: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell0); break;
		case 1: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell1); break;
		case 2: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell2); break;
		case 3: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell3); break;
		case 4: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell4); break;
		case 5: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell5); break;
		case 6: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell6); break;
		case 7: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell7); break;
		case 8: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell8); break;
		case 9: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell9); break;
		case 10: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell10); break;
		case 11: SpellSlot->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCombatSpell11); break;
		default: break;
		}
		CombatSpellButtons.Add(SpellSlot);

		UBorder* Selected = AddArt(SX, SY, 32, 32, DidSpellSelected, FLinearColor(0.9f, 0.75f, 0.2f, 0.0f));
		Selected->SetVisibility(ESlateVisibility::HitTestInvisible);
		CombatSpellSelectedOverlays.Add(Selected);

		if (i < 9)
		{
			UTextBlock* Num = AddCanvasLabel(SX + 1, SY + 1, 10, 10,
				FString::FromInt(i + 1), HUDTextColor, 7, ETextJustify::Left);
			Num->SetVisibility(ESlateVisibility::HitTestInvisible);
			CombatSpellSlotNumbers.Add(Num);
		}
	}

	CombatSpellNameLabel = AddCanvasLabel(OX + 40, OY + 58, 450, 18, TEXT(""), HUDTextColor, 10, ETextJustify::Left);
	CastSpellButton = AddToolButton(OX + 725, OY + 24, 75, 32, TEXT("Cast"), 11, DidCastBtn, DidCastBtnDown);
	CastSpellButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnCastSpellClicked);

	BuildWindowIndex = INDEX_NONE;
	SetCombatHotbarVisible(false);
}

void UACEGameHUDWidget::BuildChat()
{
	BuildWindowIndex = WindowChat;

	// classic_chat: TopBorder is 276x10 — tile across the chat width; smoke is 300x150 tiled.
	// History grows upward when resized; the entry row stays at y=583.
	UBorder* TopBorder = AddArt(0, 500, 490, 9, DidChatTopBorder, SideBarColor, EACEHudArtTile::Horizontal);
	MarkLastChatPiece(false, true);
	Windows[WindowChat].DragSurface = TopBorder;
	AddArt(0, 509, 490, 74, DidChatSmoke, PanelColor, EACEHudArtTile::Both);
	MarkLastChatPiece(true, true);

	ChatScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	ChatScroll->SetAnimateWheelScrolling(true);
	ChatScroll->SetScrollBarVisibility(ESlateVisibility::Visible);
	RootCanvas->AddChild(ChatScroll);
	Track(ChatScroll, 4, 511, 482, 70);
	MarkLastChatPiece(true, true);

	// Top-edge resize grip (drag up to grow history).
	ChatResizeGrip = AddArt(0, 500, 490, 8, DidChatTopBorder, SideBarColor, EACEHudArtTile::Horizontal);
	MarkLastChatPiece(false, true);
	UACEHoverTooltipWidget::SetWidgetTooltip(ChatResizeGrip, FText::FromString(TEXT("Drag up/down to resize chat")));
	if (UCanvasPanelSlot* GripSlot = Cast<UCanvasPanelSlot>(ChatResizeGrip->Slot))
	{
		GripSlot->SetZOrder(50);
	}

	// Entry line: target/filter button (46) + text entry (398) + send button (46).
	AddArt(0, 583, 490, 17, DidChatEntryField, SideBarColor);
	ChatTargetButton = AddToolButton(0, 583, 46, 17, TEXT("All"), 7, DidChatTarget);
	ChatTargetButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnChatTargetClicked);
	UACEHoverTooltipWidget::SetWidgetTooltip(ChatTargetButton, FText::FromString(TEXT("Cycle chat filter: All / Speech / Combat / System")));
	UpdateChatTargetButton();

	ChatEntry = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
	ChatEntry->SetHintText(FText::FromString(TEXT("Press Enter to chat")));
	FEditableTextBoxStyle EntryStyle = ChatEntry->GetWidgetStyle();
	EntryStyle.SetBackgroundImageNormal(MakeRoundedBrush(BarBackColor, 2.f));
	EntryStyle.SetBackgroundImageHovered(MakeRoundedBrush(BarBackColor, 2.f));
	EntryStyle.SetBackgroundImageFocused(MakeRoundedBrush(FLinearColor(0.09f, 0.08f, 0.06f, 1.f), 2.f));
	EntryStyle.SetForegroundColor(FSlateColor(HUDTextColor));
	FSlateFontInfo EntryFont = EntryStyle.TextStyle.Font;
	EntryFont.Size = 9;
	EntryStyle.TextStyle.SetFont(EntryFont);
	ChatEntry->SetWidgetStyle(EntryStyle);
	ChatEntry->SetClearKeyboardFocusOnCommit(true);
	ChatEntry->OnTextCommitted.AddDynamic(this, &UACEGameHUDWidget::HandleChatCommitted);
	RootCanvas->AddChild(ChatEntry);
	Track(ChatEntry, 46, 583, 398, 17);

	UButton* Send = AddToolButton(444, 583, 46, 17, TEXT("Send"), 7, DidChatSend, DidChatSendDown);
	Send->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSendClicked);

	ApplyChatExtraHeight();
	BuildWindowIndex = INDEX_NONE;
}

void UACEGameHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	EnsureArtApplied();
	TickPointerCapture(InDeltaTime);
	RefreshVitals();
	RefreshRadar();
	RefreshPanelPage();
	RefreshSelectionUI();
	RefreshCombatHotbar();
	ApplyLetterboxLayout();
}

void UACEGameHUDWidget::RefreshVitals()
{
	if (!Client)
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			Client = GI->GetSubsystem<UACEClientSubsystem>();
		}
		if (!Client)
		{
			return;
		}
	}

	LastVitals = Client->GetPlayerVitals();
	if (!LastVitals.bValid)
	{
		return;
	}

	if (HealthLabel)
	{
		HealthLabel->SetText(FText::FromString(FString::Printf(TEXT("Health  %d/%d"), LastVitals.Health, LastVitals.MaxHealth)));
	}
	if (StaminaLabel)
	{
		StaminaLabel->SetText(FText::FromString(FString::Printf(TEXT("Stamina  %d/%d"), LastVitals.Stamina, LastVitals.MaxStamina)));
	}
	if (ManaLabel)
	{
		ManaLabel->SetText(FText::FromString(FString::Printf(TEXT("Mana  %d/%d"), LastVitals.Mana, LastVitals.MaxMana)));
	}
}

void UACEGameHUDWidget::RefreshRadar()
{
	if (!Client || RadarBlips.Num() == 0)
	{
		return;
	}
	TSharedPtr<FACESession> Session = Client->GetSession();
	if (!Session)
	{
		return;
	}

	const FACEPosition PlayerPos = Session->GetPlayerPosition();
	const int32 SelfGuid = Session->GetPlayerGuid();

	if (RadarCoordsText && PlayerPos.IsValid())
	{
		const bool bShowCoords = Client && Client->IsCharacterOptionSet(0x14);
		RadarCoordsText->SetVisibility(bShowCoords ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowCoords)
		{
			const float NS = (((static_cast<uint32>(PlayerPos.CellId) >> 16) & 0xFF) * 192.f + PlayerPos.Location.Y) / 240.f - 101.95f;
			const float EW = (((static_cast<uint32>(PlayerPos.CellId) >> 24) & 0xFF) * 192.f + PlayerPos.Location.X) / 240.f - 101.95f;
			RadarCoordsText->SetText(FText::FromString(
				FString::Printf(TEXT("%.1f%s, %.1f%s"),
					FMath::Abs(NS), NS >= 0.f ? TEXT("N") : TEXT("S"),
					FMath::Abs(EW), EW >= 0.f ? TEXT("E") : TEXT("W"))));
		}
	}

	auto GlobalXY = [](const FACEPosition& Pos) -> FVector2D
	{
		const uint32 Cell = static_cast<uint32>(Pos.CellId);
		return FVector2D(
			((Cell >> 24) & 0xFF) * 192.f + Pos.Location.X,
			((Cell >> 16) & 0xFF) * 192.f + Pos.Location.Y);
	};

	const FVector2D SelfXY = GlobalXY(PlayerPos);
	// AC heading about +Z; radar rotates so the facing direction points up.
	const float Heading = 2.f * FMath::Atan2(PlayerPos.RotationXYZ.Z, PlayerPos.RotationW);
	const float CosH = FMath::Cos(Heading);
	const float SinH = FMath::Sin(Heading);

	const float RadarCenterX = Windows.IsValidIndex(WindowRadar)
		? Windows[WindowRadar].DefaultTopLeft.X + RadarLocalCenterX
		: static_cast<float>(8 + RadarLocalCenterX);
	const float RadarCenterY = Windows.IsValidIndex(WindowRadar)
		? Windows[WindowRadar].DefaultTopLeft.Y + RadarLocalCenterY
		: static_cast<float>(36 + RadarLocalCenterY);

	// Blip 0: rotating North marker on the ring rim.
	int32 BlipIndex = 0;
	{
		const float RelX = SinH;
		const float RelY = CosH;
		const float ScreenX = RadarCenterX + RelX * (RadarRadiusPx - 3);
		const float ScreenY = RadarCenterY - RelY * (RadarRadiusPx - 3);
		UBorder* North = RadarBlips[BlipIndex];
		North->SetBrush(MakeRoundedBrush(GoldColor, 2.f));
		North->SetVisibility(ESlateVisibility::HitTestInvisible);
		BlipVirtualRects[BlipIndex] = FVector4(ScreenX - 2.f, ScreenY - 2.f, 4.f, 4.f);
		++BlipIndex;
	}

	for (const TPair<int32, FACEWorldObject>& Pair : Session->GetWorldObjects())
	{
		if (BlipIndex >= RadarBlips.Num())
		{
			break;
		}
		const FACEWorldObject& Obj = Pair.Value;
		if (Obj.Guid == SelfGuid || !Obj.bHasPosition || Obj.ParentGuid != 0
			|| Obj.bDying)
		{
			continue;
		}
		const bool bCreature = (Obj.ItemType & ACEItemType::Creature) != 0;
		const bool bPortal = (Obj.ItemType & ACEItemType::Portal) != 0;
		if (!Obj.bIsPlayer && !bCreature && !bPortal)
		{
			continue;
		}

		const FVector2D Delta = GlobalXY(Obj.Position) - SelfXY;
		if (Delta.SizeSquared() > RadarRangeAc * RadarRangeAc)
		{
			continue;
		}

		// Rotate world delta by -heading so facing = screen up; screen Y grows down.
		const float RelX = Delta.X * CosH + Delta.Y * SinH;
		const float RelY = -Delta.X * SinH + Delta.Y * CosH;
		const float ScreenX = RadarCenterX + RelX / RadarRangeAc * (RadarRadiusPx - 4);
		const float ScreenY = RadarCenterY - RelY / RadarRangeAc * (RadarRadiusPx - 4);

		UBorder* Blip = RadarBlips[BlipIndex];
        const auto& Fellowship = Client->GetFellowship();
        const FLinearColor Color = ACERadarColors::Tint(ACERadarColors::Resolve(Obj, &Fellowship));
		Blip->SetBrush(MakeRoundedBrush(Color, 3.f));
		Blip->SetVisibility(ESlateVisibility::Visible);
		BlipVirtualRects[BlipIndex] = FVector4(ScreenX - 2.5f, ScreenY - 2.5f, 5.f, 5.f);
		++BlipIndex;
	}
	for (int32 i = BlipIndex; i < RadarBlips.Num(); ++i)
	{
		RadarBlips[i]->SetVisibility(ESlateVisibility::Collapsed);
		BlipVirtualRects[i] = FVector4(0, 0, 0, 0);
	}
}

void UACEGameHUDWidget::RefreshPanelPage()
{
	if (CurrentPage == EACEHudPanelPage::None || !PanelPageBody || !Client)
	{
		return;
	}

	if (CurrentPage == EACEHudPanelPage::Character)
	{
		// Retail character sheet: name + level header, Attributes / Skills / Titles tabs.
		FACEWorldObject Self;
		FString Name = TEXT("Adventurer");
		if (Client->GetWorldObject(Client->GetPlayerGuid(), Self) && !Self.Name.IsEmpty())
		{
			Name = Self.Name;
		}
		if (PanelPageTitle)
		{
			PanelPageTitle->SetText(FText::FromString(
				LastVitals.bValid && LastVitals.Level > 0
					? FString::Printf(TEXT("%s  (Level %d)"), *Name, LastVitals.Level)
					: Name));
		}
		if (!LastVitals.bValid)
		{
			PanelStatsLabels->SetText(FText::FromString(TEXT("Waiting for the server\ncharacter description...")));
			PanelStatsValues->SetText(FText::GetEmpty());
			return;
		}
		switch (CharSheetTab)
		{
		case EACECharSheetTab::Skills:
		{
			SetAttributePanelVisible(false);
			if (PanelStatsLabels) { PanelStatsLabels->SetVisibility(ESlateVisibility::Collapsed); }
			if (PanelStatsValues) { PanelStatsValues->SetVisibility(ESlateVisibility::Collapsed); }
			if (SkillsScroll) { SkillsScroll->SetVisibility(ESlateVisibility::Visible); }
			SetRaiseFooterVisible(true);
			RefreshSkillsUI();
			break;
		}
		case EACECharSheetTab::Titles:
			SetAttributePanelVisible(false);
			SetRaiseFooterVisible(false);
			if (SkillsScroll) { SkillsScroll->SetVisibility(ESlateVisibility::Collapsed); }
			if (PanelStatsLabels) { PanelStatsLabels->SetVisibility(ESlateVisibility::HitTestInvisible); }
			if (PanelStatsValues) { PanelStatsValues->SetVisibility(ESlateVisibility::HitTestInvisible); }
			PanelStatsLabels->SetText(FText::FromString(TEXT("Titles\n\n(No titles received yet.)")));
			PanelStatsValues->SetText(FText::GetEmpty());
			break;
		case EACECharSheetTab::Attributes:
		default:
			if (SkillsScroll) { SkillsScroll->SetVisibility(ESlateVisibility::Collapsed); }
			if (PanelStatsLabels) { PanelStatsLabels->SetVisibility(ESlateVisibility::Collapsed); }
			if (PanelStatsValues) { PanelStatsValues->SetVisibility(ESlateVisibility::Collapsed); }
			SetAttributePanelVisible(true);
			RefreshAttributePanel();
			break;
		}
		return;
	}

	FString Body;
	switch (CurrentPage)
	{
	case EACEHudPanelPage::Inventory:
		RefreshInventoryUI();
		return;
	case EACEHudPanelPage::Spellbook:
		RefreshSpellbookUI();
		return;
	case EACEHudPanelPage::Social:
		Body = TEXT("Allegiance, Fellowship, and Friends\n\nUse the DAT Social panel (PanelButton_Social) for full controls.");
		break;
	case EACEHudPanelPage::Quests:
		Body = TEXT("Quest contracts\n\nUse the DAT Quests panel for the active contract list.");
		break;
	case EACEHudPanelPage::Map:
	{
		EnsureWorldMapWidgets();
		RefreshWorldMap();
		if (WorldMapRoot) { WorldMapRoot->SetVisibility(ESlateVisibility::Visible); }
		if (PanelPageBody) { PanelPageBody->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	case EACEHudPanelPage::Options:
		Body = TEXT("Options\n\nUI scale adapts to your resolution.\n\nUse the radar lock icon to freeze\nor free floaty window positions.\n\nType @commands in chat for\nserver-side options.");
		break;
	default:
		break;
	}
	PanelPageBody->SetText(FText::FromString(Body));
}

void UACEGameHUDWidget::SetPanelPage(EACEHudPanelPage Page)
{
	CurrentPage = (CurrentPage == Page) ? EACEHudPanelPage::None : Page;
	const bool bVisible = CurrentPage != EACEHudPanelPage::None;
	const bool bCharSheet = CurrentPage == EACEHudPanelPage::Character;
	const bool bInventory = CurrentPage == EACEHudPanelPage::Inventory;
	const bool bMap = CurrentPage == EACEHudPanelPage::Map;
	const bool bSpellbook = CurrentPage == EACEHudPanelPage::Spellbook;
	const ESlateVisibility PanelVis = bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	const ESlateVisibility TextVis = bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (PanelPageRoot) { PanelPageRoot->SetVisibility(PanelVis); }
	if (PanelPageTitle)
	{
		// Attributes tab owns the StatManagement header (name/level) — hide the generic title.
		PanelPageTitle->SetVisibility(
			(bCharSheet && CharSheetTab == EACECharSheetTab::Attributes)
				? ESlateVisibility::Collapsed
				: ((bVisible && !bInventory && !bMap) ? TextVis : ESlateVisibility::Collapsed));
	}
	if (PanelPageBody) { PanelPageBody->SetVisibility(bVisible && !bCharSheet && !bInventory && !bMap && !bSpellbook ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }
	if (SpellbookScroll) { SpellbookScroll->SetVisibility(bSpellbook ? ESlateVisibility::Visible : ESlateVisibility::Collapsed); }
	if (SkillsScroll) { SkillsScroll->SetVisibility(bCharSheet && CharSheetTab == EACECharSheetTab::Skills ? ESlateVisibility::Visible : ESlateVisibility::Collapsed); }
	if (WorldMapRoot) { WorldMapRoot->SetVisibility(bMap ? ESlateVisibility::Visible : ESlateVisibility::Collapsed); }
	if (!bMap)
	{
		if (WorldMapImage) { WorldMapImage->SetVisibility(ESlateVisibility::Collapsed); }
		if (WorldMapPlayerMarker) { WorldMapPlayerMarker->SetVisibility(ESlateVisibility::Collapsed); }
		if (WorldMapHoverLabel) { WorldMapHoverLabel->SetVisibility(ESlateVisibility::Collapsed); }
		for (UButton* TownBtn : WorldMapTownButtons)
		{
			if (TownBtn) { TownBtn->SetVisibility(ESlateVisibility::Collapsed); }
		}
	}
	else
	{
		RefreshWorldMap();
	}
	if (PanelStatsLabels) { PanelStatsLabels->SetVisibility(bCharSheet && CharSheetTab != EACECharSheetTab::Attributes ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }
	if (PanelStatsValues) { PanelStatsValues->SetVisibility(bCharSheet && CharSheetTab != EACECharSheetTab::Attributes ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed); }
	SetAttributePanelVisible(bCharSheet && CharSheetTab == EACECharSheetTab::Attributes);
	if (bCharSheet && CharSheetTab == EACECharSheetTab::Skills)
	{
		SetRaiseFooterVisible(true);
	}
	else if (!bCharSheet || CharSheetTab == EACECharSheetTab::Titles)
	{
		SetRaiseFooterVisible(false);
	}
	const ESlateVisibility TabVis = bCharSheet ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	if (CharTabAttributes) { CharTabAttributes->SetVisibility(TabVis); }
	if (CharTabSkills) { CharTabSkills->SetVisibility(TabVis); }
	if (CharTabTitles) { CharTabTitles->SetVisibility(TabVis); }

	const ESlateVisibility InvVis = bInventory ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	const ESlateVisibility InvHit = bInventory ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (InventoryRoot) { InventoryRoot->SetVisibility(InvVis); }
	if (InventoryHint) { InventoryHint->SetVisibility(InvHit); }
	if (InventoryGridScroll) { InventoryGridScroll->SetVisibility(InvVis); }
	if (PackTabsScroll) { PackTabsScroll->SetVisibility(InvVis); }
	if (PaperDollModelFrame) { PaperDollModelFrame->SetVisibility(InvHit); }
	if (PaperDollModelImage) { PaperDollModelImage->SetVisibility(InvHit); }
	for (UBorder* IconCell : PaperDollSlots)
	{
		// Visible (not HitTestInvisible): doll slots are drag & drop sources/targets.
		if (IconCell) { IconCell->SetVisibility(InvVis); }
	}
	for (UBorder* IconCell : InventoryIconSlots)
	{
		if (IconCell) { IconCell->SetVisibility(InvVis); }
	}
	// Pack tabs refreshed in RefreshInventoryUI (hide extras without packs).
	if (!bInventory)
	{
		for (UButton* Tab : PackTabButtons)
		{
			if (Tab) { Tab->SetVisibility(ESlateVisibility::Collapsed); }
		}
	}
	else
	{
		RefreshInventoryUI();
	}
	UpdateInventoryButtonVisual();

	if (PanelPageTitle && bVisible && !bCharSheet && !bInventory)
	{
		const TCHAR* Title = TEXT("");
		switch (CurrentPage)
		{
		case EACEHudPanelPage::Spellbook: Title = TEXT("Spellbook"); break;
		case EACEHudPanelPage::Map:       Title = TEXT("Map"); break;
		case EACEHudPanelPage::Social:    Title = TEXT("Social"); break;
		case EACEHudPanelPage::Quests:    Title = TEXT("Quests"); break;
		case EACEHudPanelPage::Options:   Title = TEXT("Options"); break;
		default: break;
		}
		PanelPageTitle->SetText(FText::FromString(Title));
	}
	if (bVisible && CurrentPage == EACEHudPanelPage::Spellbook)
	{
		RefreshSpellbookUI();
	}
}

void UACEGameHUDWidget::OnInventoryClicked() { SetPanelPage(EACEHudPanelPage::Inventory); }
void UACEGameHUDWidget::OnCharacterClicked() { SetPanelPage(EACEHudPanelPage::Character); }
void UACEGameHUDWidget::OnSpellbookClicked() { SetPanelPage(EACEHudPanelPage::Spellbook); }
void UACEGameHUDWidget::OnMapClicked() { SetPanelPage(EACEHudPanelPage::Map); }
void UACEGameHUDWidget::OnSocialClicked() { SetPanelPage(EACEHudPanelPage::Social); }
void UACEGameHUDWidget::OnQuestsClicked() { SetPanelPage(EACEHudPanelPage::Quests); }
void UACEGameHUDWidget::OnOptionsClicked() { SetPanelPage(EACEHudPanelPage::Options); }
void UACEGameHUDWidget::OnHelpClicked()
{
	AppendChatLine(TEXT("Help: use the toolbar icons for Character, Spellbook, Map, Inventory, and Options."), ChatSystemColor);
	SetPanelPage(EACEHudPanelPage::Options);
}

void UACEGameHUDWidget::ShowIndicatorPanel(int32 IndicatorIndex)
{
	FString Body;
	switch (IndicatorIndex)
	{
	case 0: // Portal storm
		Body = TEXT("Portal Storm\n\nWhen a portal storm is active, portals may shift or become unstable. Watch for storm warnings in chat.");
		SetPanelPage(EACEHudPanelPage::Options);
		if (PanelPageTitle) { PanelPageTitle->SetText(FText::FromString(TEXT("Portal Storm"))); }
		if (PanelPageBody) { PanelPageBody->SetText(FText::FromString(Body)); PanelPageBody->SetVisibility(ESlateVisibility::HitTestInvisible); }
		return;
	case 1: // Mini-game
		AppendChatLine(TEXT("No mini-game is active."), ChatSystemColor);
		return;
	case 2: // Vitae
	{
		SetPanelPage(EACEHudPanelPage::Character);
		CharSheetTab = EACECharSheetTab::Attributes;
		RefreshPanelPage();
		AppendChatLine(TEXT("Vitae: death penalty reduces earned XP until it fades. Check Character > Attributes."), ChatSystemColor);
		return;
	}
	case 3: // Positive effects
	case 4: // Negative effects
	{
		Body = (IndicatorIndex == 3)
			? TEXT("Enchantments (Beneficial)\n\nSpells and effects currently boosting you appear here in retail. Open Spellbook for known spells; appraisal shows active enchantments on targets.")
			: TEXT("Enchantments (Harmful)\n\nCurses and debuffs currently affecting you appear here in retail. Check chat combat log and target appraisal for active harmful spells.");
		CurrentPage = EACEHudPanelPage::None;
		SetPanelPage(EACEHudPanelPage::Options);
		if (PanelPageTitle) { PanelPageTitle->SetText(FText::FromString(IndicatorIndex == 3 ? TEXT("Effects (+)") : TEXT("Effects (-)"))); }
		if (PanelPageBody)
		{
			PanelPageBody->SetText(FText::FromString(Body));
			PanelPageBody->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		if (PanelStatsLabels) { PanelStatsLabels->SetVisibility(ESlateVisibility::Collapsed); }
		if (PanelStatsValues) { PanelStatsValues->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	case 5: // Burden
	{
		int32 Burden = 0;
		if (Client)
		{
			FACEWorldObject Self;
			if (Client->GetWorldObject(Client->GetPlayerGuid(), Self))
			{
				Burden = Self.Burden;
			}
		}
		Body = FString::Printf(TEXT("Burden\n\nEncumbrance: %d\n\nHigh burden slows run speed. Drop or store items in your pack to reduce burden."), Burden);
		SetPanelPage(EACEHudPanelPage::Inventory);
		AppendChatLine(FString::Printf(TEXT("Burden: %d — open Inventory to manage encumbrance."), Burden), ChatSystemColor);
		return;
	}
	case 6: // Link
		Body = TEXT("Connection\n\nLink status shows your connection to the world server. If movement freezes after a portal, the client now re-syncs MoveToState automatically.");
		SetPanelPage(EACEHudPanelPage::Options);
		if (PanelPageTitle) { PanelPageTitle->SetText(FText::FromString(TEXT("Link Status"))); }
		if (PanelPageBody) { PanelPageBody->SetText(FText::FromString(Body)); PanelPageBody->SetVisibility(ESlateVisibility::HitTestInvisible); }
		return;
	default:
		return;
	}
}

void UACEGameHUDWidget::OnIndicatorPortalStorm() { ShowIndicatorPanel(0); }
void UACEGameHUDWidget::OnIndicatorMiniGame() { ShowIndicatorPanel(1); }
void UACEGameHUDWidget::OnIndicatorVitae() { ShowIndicatorPanel(2); }
void UACEGameHUDWidget::OnIndicatorPositive() { ShowIndicatorPanel(3); }
void UACEGameHUDWidget::OnIndicatorNegative() { ShowIndicatorPanel(4); }
void UACEGameHUDWidget::OnIndicatorBurden() { ShowIndicatorPanel(5); }
void UACEGameHUDWidget::OnIndicatorLink() { ShowIndicatorPanel(6); }

void UACEGameHUDWidget::EnsureAttributeWidgets()
{
	if (AttrPanelRoot || !RootCanvas)
	{
		return;
	}
	// classic_panel body origin. StatManagement_Template (0x21000045) is 300x337.
	// Character tabs occupy the first ~24px; template content starts below them.
	const int32 OX = 500;
	const int32 OY = 36;
	const int32 TX = OX;
	const int32 TY = OY + 24;

	AttrPanelRoot = AddArt(TX, TY, 300, 337, DidShortcutSpacer, FLinearColor(0.04f, 0.035f, 0.03f, 0.92f));
	AttrPanelRoot->SetVisibility(ESlateVisibility::Collapsed);

	// Header (0,0)-(300,110)
	AttrHeaderBg = AddArt(TX, TY, 300, 110, DidShortcutSpacer, FLinearColor(0.05f, 0.045f, 0.035f, 0.5f));
	AttrHeaderBg->SetVisibility(ESlateVisibility::Collapsed);
	AttrHeaderName = AddCanvasLabel(TX, TY, 230, 20, TEXT(""), GoldColor, 12, ETextJustify::Left);
	AttrHeaderName->SetVisibility(ESlateVisibility::Collapsed);
	AttrHeaderTitle = AddCanvasLabel(TX, TY + 20, 230, 15, TEXT(""), LabelColor, 9, ETextJustify::Left);
	AttrHeaderTitle->SetVisibility(ESlateVisibility::Collapsed);
	AttrHeaderTotalXpLabel = AddCanvasLabel(TX, TY + 70, 130, 18, TEXT("Total XP"), LabelColor, 9, ETextJustify::Left);
	AttrHeaderTotalXpLabel->SetVisibility(ESlateVisibility::Collapsed);
	AttrHeaderTotalXpValue = AddCanvasLabel(TX + 130, TY + 70, 100, 18, TEXT("0"), HUDTextColor, 9, ETextJustify::Right);
	AttrHeaderTotalXpValue->SetVisibility(ESlateVisibility::Collapsed);
	AttrHeaderDivider = AddArt(TX + 230, TY, 5, 105, DidAttrHeaderDivider, SideBarColor);
	AttrHeaderDivider->SetVisibility(ESlateVisibility::Collapsed);
	AttrLevelLabel = AddCanvasLabel(TX + 235, TY, 65, 35, TEXT("Level"), LabelColor, 10, ETextJustify::Center);
	AttrLevelLabel->SetVisibility(ESlateVisibility::Collapsed);
	AttrLevelValue = AddCanvasLabel(TX + 235, TY + 35, 65, 50, TEXT("1"), GoldColor, 22, ETextJustify::Center);
	AttrLevelValue->SetVisibility(ESlateVisibility::Collapsed);

	AttrXpToLevelMeterBack = AddArt(TX, TY + 88, 230, 17, DidAttrXpMeterBack, BarBackColor);
	AttrXpToLevelMeterBack->SetVisibility(ESlateVisibility::Collapsed);
	AttrXpToLevelMeterFill = AddArt(TX, TY + 89, 1, 15, DidAttrXpMeterFill, GoldColor);
	AttrXpToLevelMeterFill->SetVisibility(ESlateVisibility::Collapsed);
	AttrXpToLevelLabel = AddCanvasLabel(TX, TY + 88, 130, 17, TEXT("XP to Level"), LabelColor, 8, ETextJustify::Left);
	AttrXpToLevelLabel->SetVisibility(ESlateVisibility::Collapsed);
	AttrXpToLevelValue = AddCanvasLabel(TX + 130, TY + 88, 100, 17, TEXT(""), HUDTextColor, 8, ETextJustify::Right);
	AttrXpToLevelValue->SetVisibility(ESlateVisibility::Collapsed);

	AttrDividerTop = AddArt(TX, TY + 105, 300, 7, DidAttrHeaderDivider, SideBarColor, EACEHudArtTile::Horizontal);
	AttrDividerTop->SetVisibility(ESlateVisibility::Collapsed);

	AttrRowButtons.Reset();
	AttrRowIcons.Reset();
	AttrRowNames.Reset();
	AttrRowValues.Reset();
	for (int32 i = 0; i < 9; ++i)
	{
		// List region starts at y=112; rows are 20px (ListInfo_Template).
		const int32 RY = TY + 112 + i * 20;
		UButton* RowBtn = AddToolButton(TX, RY, 282, 20, TEXT(""), 8, DidAttrRowBg, DidAttrRowBg);
		RowBtn->SetVisibility(ESlateVisibility::Collapsed);
		UACEHoverTooltipWidget::SetWidgetTooltip(RowBtn, FText::FromString(AttrRows[i].Hover));
		switch (i)
		{
		case 0: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow0); break;
		case 1: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow1); break;
		case 2: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow2); break;
		case 3: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow3); break;
		case 4: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow4); break;
		case 5: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow5); break;
		case 6: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow6); break;
		case 7: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow7); break;
		case 8: RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRow8); break;
		default: break;
		}
		AttrRowButtons.Add(RowBtn);

		UBorder* Icon = AddArt(TX, RY, 20, 20, AttrRows[i].IconDid, BarBackColor);
		Icon->SetVisibility(ESlateVisibility::Collapsed);
		AttrRowIcons.Add(Icon);

		UTextBlock* Name = AddCanvasLabel(TX + 25, RY, 150, 20, AttrRows[i].Name, LabelColor, 9, ETextJustify::Left);
		Name->SetVisibility(ESlateVisibility::Collapsed);
		AttrRowNames.Add(Name);

		UTextBlock* Value = AddCanvasLabel(TX + 175, RY, 100, 20, TEXT("0"), HUDTextColor, 9, ETextJustify::Right);
		Value->SetVisibility(ESlateVisibility::Collapsed);
		AttrRowValues.Add(Value);
	}

	AttrDividerBottom = AddArt(TX, TY + 272, 300, 7, DidAttrHeaderDivider, SideBarColor, EACEHudArtTile::Horizontal);
	AttrDividerBottom->SetVisibility(ESlateVisibility::Collapsed);

	// Footer_Meter (y=282, h=55) — shared with Skills tab.
	AttrSelectedName = AddCanvasLabel(TX + 5, TY + 282, 250, 18, TEXT(""), GoldColor, 10, ETextJustify::Left);
	AttrSelectedName->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
	AttrRaiseMeterBack = AddArt(TX + 5, TY + 302, 240, 17, DidAttrXpMeterBack, BarBackColor);
	AttrRaiseMeterBack->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
	AttrRaiseMeterFill = AddArt(TX + 5, TY + 303, 1, 15, DidAttrRaiseMeterFill, GoldColor);
	AttrRaiseMeterFill->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
	AttrRaiseXpText = AddCanvasLabel(TX + 5, TY + 302, 145, 17, TEXT(""), LabelColor, 8, ETextJustify::Left);
	AttrRaiseXpText->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
	AttrUnassignedXpText = AddCanvasLabel(TX + 150, TY + 302, 95, 17, TEXT(""), HUDTextColor, 8, ETextJustify::Right);
	AttrUnassignedXpText->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
	AttrSelectedHint = AddCanvasLabel(TX + 5, TY + 319, 240, 18, TEXT(""), LabelColor, 8, ETextJustify::Left);
	AttrSelectedHint->SetVisibility(ESlateVisibility::Collapsed);
	AttrSelectedHint->SetAutoWrapText(true);
	MarkLastPiece(false, true);

	AttrRaise10Button = AddToolButton(TX + 260, TY + 282, 30, 26, TEXT(""), 8, DidAttrRaise10, DidAttrRaise10Down);
	AttrRaise10Button->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRaise10);
	UACEHoverTooltipWidget::SetWidgetTooltip(AttrRaise10Button, FText::FromString(TEXT("Spend XP to raise ten times.")));
	AttrRaise10Button->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
	AttrRaise1Button = AddToolButton(TX + 260, TY + 308, 30, 26, TEXT(""), 8, DidAttrRaise1, DidAttrRaise1Down);
	AttrRaise1Button->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRaise1);
	UACEHoverTooltipWidget::SetWidgetTooltip(AttrRaise1Button, FText::FromString(TEXT("Spend XP to raise once.")));
	AttrRaise1Button->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
	// Extra "Max" control (not in retail footer art) — keep for power users, tucked under raise10.
	AttrRaiseAllButton = AddToolButton(TX + 260, TY + 334, 30, 16, TEXT("Max"), 7);
	AttrRaiseAllButton->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnAttrRaiseAll);
	UACEHoverTooltipWidget::SetWidgetTooltip(AttrRaiseAllButton, FText::FromString(TEXT("Spend as much unassigned XP as possible.")));
	AttrRaiseAllButton->SetVisibility(ESlateVisibility::Collapsed);
	MarkLastPiece(false, true);
}

void UACEGameHUDWidget::SetAttributePanelVisible(bool bVisible)
{
	EnsureAttributeWidgets();
	const ESlateVisibility Vis = bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	const ESlateVisibility Hit = bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	if (AttrPanelRoot) { AttrPanelRoot->SetVisibility(Vis); }
	if (AttrHeaderBg) { AttrHeaderBg->SetVisibility(Hit); }
	if (AttrHeaderName) { AttrHeaderName->SetVisibility(Hit); }
	if (AttrHeaderTitle) { AttrHeaderTitle->SetVisibility(Hit); }
	if (AttrHeaderTotalXpLabel) { AttrHeaderTotalXpLabel->SetVisibility(Hit); }
	if (AttrHeaderTotalXpValue) { AttrHeaderTotalXpValue->SetVisibility(Hit); }
	if (AttrHeaderDivider) { AttrHeaderDivider->SetVisibility(Hit); }
	if (AttrLevelLabel) { AttrLevelLabel->SetVisibility(Hit); }
	if (AttrLevelValue) { AttrLevelValue->SetVisibility(Hit); }
	if (AttrXpToLevelMeterBack) { AttrXpToLevelMeterBack->SetVisibility(Hit); }
	if (AttrXpToLevelMeterFill) { AttrXpToLevelMeterFill->SetVisibility(Hit); }
	if (AttrXpToLevelLabel) { AttrXpToLevelLabel->SetVisibility(Hit); }
	if (AttrXpToLevelValue) { AttrXpToLevelValue->SetVisibility(Hit); }
	if (AttrDividerTop) { AttrDividerTop->SetVisibility(Hit); }
	if (AttrDividerBottom) { AttrDividerBottom->SetVisibility(Hit); }
	for (int32 i = 0; i < AttrRowButtons.Num(); ++i)
	{
		if (AttrRowButtons[i]) { AttrRowButtons[i]->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed); }
		if (AttrRowIcons.IsValidIndex(i) && AttrRowIcons[i]) { AttrRowIcons[i]->SetVisibility(Hit); }
		if (AttrRowNames.IsValidIndex(i) && AttrRowNames[i]) { AttrRowNames[i]->SetVisibility(Hit); }
		if (AttrRowValues.IsValidIndex(i) && AttrRowValues[i]) { AttrRowValues[i]->SetVisibility(Hit); }
	}
	// Raise footer is shared with the Skills tab — leave it alone when hiding attributes.
	if (bVisible)
	{
		SetRaiseFooterVisible(true);
	}
}

void UACEGameHUDWidget::RefreshAttributePanel()
{
	EnsureAttributeWidgets();
	if (!LastVitals.bValid)
	{
		return;
	}

	FACEWorldObject Self;
	FString Name = TEXT("Adventurer");
	if (Client && Client->GetWorldObject(Client->GetPlayerGuid(), Self) && !Self.Name.IsEmpty())
	{
		Name = Self.Name;
	}
	if (AttrHeaderName) { AttrHeaderName->SetText(FText::FromString(Name)); }
	if (AttrHeaderTitle)
	{
		AttrHeaderTitle->SetText(FText::FromString(
			LastVitals.Level > 0 ? FString::Printf(TEXT("Level %d Adventurer"), LastVitals.Level) : TEXT("Adventurer")));
	}
	if (AttrLevelValue) { AttrLevelValue->SetText(FText::AsNumber(LastVitals.Level)); }
	if (AttrHeaderTotalXpValue)
	{
		AttrHeaderTotalXpValue->SetText(FText::AsNumber(LastVitals.TotalExperience));
	}

	int64 XpNeeded = 0;
	float Progress = 0.f;
	if (Dat)
	{
		Dat->TryGetXpToNextLevel(LastVitals.TotalExperience, LastVitals.Level, XpNeeded, &Progress);
	}
	if (AttrXpToLevelValue)
	{
		AttrXpToLevelValue->SetText(FText::FromString(
			XpNeeded > 0 ? FString::Printf(TEXT("%lld"), XpNeeded) : TEXT("Max")));
	}
	if (AttrXpToLevelMeterFill)
	{
		const float MeterW = FMath::Clamp(Progress, 0.f, 1.f) * 230.f;
		const int32 PieceIdx = Pieces.IndexOfByPredicate([this](const FHudPiece& P)
		{
			return P.Widget.Get() == AttrXpToLevelMeterFill;
		});
		if (PieceIdx != INDEX_NONE)
		{
			Pieces[PieceIdx].VirtualRect.Z = FMath::Max(1.f, MeterW);
		}
	}

	auto FormatVital = [](int32 Cur, int32 Max) -> FString
	{
		return FString::Printf(TEXT("%d / %d"), Cur, Max);
	};
	for (int32 i = 0; i < 9 && i < AttrRowValues.Num(); ++i)
	{
		FString ValueText;
		switch (i)
		{
		case 0: ValueText = FString::FromInt(LastVitals.Strength); break;
		case 1: ValueText = FString::FromInt(LastVitals.Endurance); break;
		case 2: ValueText = FString::FromInt(LastVitals.Quickness); break;
		case 3: ValueText = FString::FromInt(LastVitals.Coordination); break;
		case 4: ValueText = FString::FromInt(LastVitals.Focus); break;
		case 5: ValueText = FString::FromInt(LastVitals.Self); break;
		case 6: ValueText = FormatVital(LastVitals.Health, LastVitals.MaxHealth); break;
		case 7: ValueText = FormatVital(LastVitals.Stamina, LastVitals.MaxStamina); break;
		case 8: ValueText = FormatVital(LastVitals.Mana, LastVitals.MaxMana); break;
		default: break;
		}
		if (AttrRowValues[i]) { AttrRowValues[i]->SetText(FText::FromString(ValueText)); }
		if (AttrRowButtons.IsValidIndex(i) && AttrRowButtons[i])
		{
			const bool bSel = (i == SelectedAttributeRow);
			AttrRowButtons[i]->SetBackgroundColor(bSel
				? FLinearColor(0.35f, 0.28f, 0.12f, 0.85f)
				: FLinearColor(1.f, 1.f, 1.f, 0.15f));
		}
	}
	SelectedAttributeRow = FMath::Clamp(SelectedAttributeRow, 0, 8);
	const FAttrRowDef& Sel = AttrRows[SelectedAttributeRow];
	if (AttrSelectedName) { AttrSelectedName->SetText(FText::FromString(Sel.Name)); }
	if (AttrSelectedHint) { AttrSelectedHint->SetText(FText::FromString(Sel.Hover)); }

	int32 XpSpent = 0;
	int64 XpToNext = 0;
	int64 MaxSpend = 0;
	if (Sel.AttributeId != 0)
	{
		XpSpent = LastVitals.GetAttributeXpSpent(Sel.AttributeId);
		if (Dat) { Dat->TryGetAttributeXpToNextRank(XpSpent, XpToNext, &MaxSpend); }
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
		if (Dat) { Dat->TryGetVitalXpToNextRank(XpSpent, XpToNext, &MaxSpend); }
	}
	const int64 CanSpend = FMath::Min<int64>(LastVitals.AvailableExperience, MaxSpend);
	if (AttrRaiseXpText)
	{
		AttrRaiseXpText->SetText(FText::FromString(
			XpToNext > 0
				? FString::Printf(TEXT("XP to raise: %d"), XpToNext)
				: TEXT("Fully raised")));
	}
	if (AttrUnassignedXpText)
	{
		AttrUnassignedXpText->SetText(FText::AsNumber(LastVitals.AvailableExperience));
	}
	if (AttrRaiseMeterFill)
	{
		float RaiseProgress = 0.f;
		if (XpToNext > 0 && MaxSpend > 0)
		{
			RaiseProgress = 1.f - (static_cast<float>(XpToNext) / static_cast<float>(MaxSpend + XpToNext));
		}
		const float MeterW = FMath::Clamp(RaiseProgress, 0.f, 1.f) * 240.f;
		const int32 PieceIdx = Pieces.IndexOfByPredicate([this](const FHudPiece& P)
		{
			return P.Widget.Get() == AttrRaiseMeterFill;
		});
		if (PieceIdx != INDEX_NONE)
		{
			Pieces[PieceIdx].VirtualRect.Z = FMath::Max(1.f, MeterW);
		}
	}
	(void)CanSpend;
}

void UACEGameHUDWidget::SelectAttributeRow(int32 RowIndex)
{
	SelectedAttributeRow = FMath::Clamp(RowIndex, 0, 8);
	RefreshAttributePanel();
}

void UACEGameHUDWidget::RaiseSelectedAttribute(int32 Multiplier)
{
	if (!Client || !LastVitals.bValid || !Dat)
	{
		return;
	}
	SelectedAttributeRow = FMath::Clamp(SelectedAttributeRow, 0, 8);
	const FAttrRowDef& Sel = AttrRows[SelectedAttributeRow];
	int32 XpSpent = 0;
	int64 XpToNext = 0;
	int64 MaxSpend = 0;
	if (Sel.AttributeId != 0)
	{
		XpSpent = LastVitals.GetAttributeXpSpent(Sel.AttributeId);
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
	if (XpToNext <= 0 || MaxSpend <= 0 || LastVitals.AvailableExperience <= 0)
	{
		return;
	}

	int64 Wanted = 0;
	if (Multiplier <= 0)
	{
		Wanted = MaxSpend;
	}
	else
	{
		Wanted = XpToNext;
		Wanted = FMath::Min<int64>(Wanted, MaxSpend);
	}
	if (Multiplier > 0 && LastVitals.AvailableExperience < Wanted) return;
	Wanted = FMath::Min<int64>(Wanted, LastVitals.AvailableExperience);
	Wanted = FMath::Max<int64>(1, Wanted);
	if (Sel.AttributeId != 0)
	{
		Client->SendRaiseAttribute(Sel.AttributeId, static_cast<int32>(FMath::Min<int64>(Wanted, MAX_int32)));
	}
	else
	{
		Client->SendRaiseVital(Sel.VitalId, static_cast<int32>(FMath::Min<int64>(Wanted, MAX_int32)));
	}
}

void UACEGameHUDWidget::OnAttrRow0() { SelectAttributeRow(0); }
void UACEGameHUDWidget::OnAttrRow1() { SelectAttributeRow(1); }
void UACEGameHUDWidget::OnAttrRow2() { SelectAttributeRow(2); }
void UACEGameHUDWidget::OnAttrRow3() { SelectAttributeRow(3); }
void UACEGameHUDWidget::OnAttrRow4() { SelectAttributeRow(4); }
void UACEGameHUDWidget::OnAttrRow5() { SelectAttributeRow(5); }
void UACEGameHUDWidget::OnAttrRow6() { SelectAttributeRow(6); }
void UACEGameHUDWidget::OnAttrRow7() { SelectAttributeRow(7); }
void UACEGameHUDWidget::OnAttrRow8() { SelectAttributeRow(8); }
void UACEGameHUDWidget::OnAttrRaise1()
{
	if (CharSheetTab == EACECharSheetTab::Skills) { RaiseSelectedSkill(1); }
	else { RaiseSelectedAttribute(1); }
}
void UACEGameHUDWidget::OnAttrRaise10()
{
	if (CharSheetTab == EACECharSheetTab::Skills) { RaiseSelectedSkill(10); }
	else { RaiseSelectedAttribute(10); }
}
void UACEGameHUDWidget::OnAttrRaiseAll()
{
	if (CharSheetTab == EACECharSheetTab::Skills) { RaiseSelectedSkill(0); }
	else { RaiseSelectedAttribute(0); }
}

void UACEGameHUDWidget::EnsureWorldMapWidgets()
{
	if (WorldMapRoot || !RootCanvas)
	{
		return;
	}
	const int32 OX = 500;
	const int32 OY = 36;
	WorldMapRoot = AddPanel(OX + 8, OY + 28, 284, 390, FLinearColor(0.02f, 0.02f, 0.03f, 0.92f));
	WorldMapRoot->SetVisibility(ESlateVisibility::Collapsed);

	WorldMapImage = AddArt(OX + 14, OY + 56, 257, 267, DidWorldMap, FLinearColor(0.1f, 0.12f, 0.08f, 1.f));
	WorldMapImage->SetVisibility(ESlateVisibility::Collapsed);

	WorldMapPlayerMarker = AddArt(OX + 14, OY + 56, 17, 16, DidMapPlayerIcon, GoldColor);
	WorldMapPlayerMarker->SetVisibility(ESlateVisibility::Collapsed);

	WorldMapHoverLabel = AddCanvasLabel(OX + 14, OY + 330, 272, 40, TEXT(""), GoldColor, 10, ETextJustify::Center);
	WorldMapHoverLabel->SetVisibility(ESlateVisibility::Collapsed);
	WorldMapHoverLabel->SetAutoWrapText(true);

	struct FTown { const TCHAR* Name; uint8 LbX; uint8 LbY; };
	const FTown Towns[] =
	{
		{ TEXT("Holtburg"), 0xA9, 0xB4 },
		{ TEXT("Yaraq"), 0x7D, 0x64 },
		{ TEXT("Shoushi"), 0xDE, 0x51 },
		{ TEXT("Sanamar"), 0x72, 0xB4 },
		{ TEXT("Rithwic"), 0xC9, 0x95 },
		{ TEXT("Glenden Wood"), 0xA1, 0xA1 },
		{ TEXT("Cragstone"), 0xB4, 0xA9 },
		{ TEXT("Zaikhal"), 0x80, 0x96 },
		{ TEXT("Hebian-To"), 0xE6, 0x4E },
	};
	WorldMapTownButtons.Reset();
	for (const FTown& Town : Towns)
	{
		const float U = (static_cast<float>(Town.LbX) + 0.5f) / 256.f;
		const float V = 1.f - (static_cast<float>(Town.LbY) + 0.5f) / 256.f;
		const int32 TX = OX + 14 + static_cast<int32>(U * 257.f) - 6;
		const int32 TY = OY + 56 + static_cast<int32>(V * 267.f) - 6;
		UButton* TownBtn = AddToolButton(TX, TY, 12, 12, TEXT(""), 7);
		UACEHoverTooltipWidget::SetWidgetTooltip(TownBtn, FText::FromString(Town.Name));
		TownBtn->SetVisibility(ESlateVisibility::Collapsed);
		WorldMapTownButtons.Add(TownBtn);
	}
}

void UACEGameHUDWidget::RefreshWorldMap()
{
	EnsureWorldMapWidgets();
	const ESlateVisibility Vis = ESlateVisibility::Visible;
	const ESlateVisibility Hit = ESlateVisibility::HitTestInvisible;
	if (WorldMapRoot) { WorldMapRoot->SetVisibility(Vis); }
	if (WorldMapImage) { WorldMapImage->SetVisibility(Hit); }
	for (UButton* TownBtn : WorldMapTownButtons)
	{
		if (TownBtn) { TownBtn->SetVisibility(ESlateVisibility::Visible); }
	}

	FString Hover = TEXT("Dereth — hover a town marker");
	if (Client)
	{
		const FACEPosition Pos = Client->GetPlayerPosition();
		if (Pos.IsValid())
		{
			const uint32 Cell = static_cast<uint32>(Pos.CellId);
			const float LbX = static_cast<float>((Cell >> 24) & 0xFF) + Pos.Location.X / 192.f;
			const float LbY = static_cast<float>((Cell >> 16) & 0xFF) + Pos.Location.Y / 192.f;
			const float U = FMath::Clamp(LbX / 256.f, 0.f, 1.f);
			const float V = FMath::Clamp(1.f - LbY / 256.f, 0.f, 1.f);
			const int32 OX = 500;
			const int32 OY = 36;
			const int32 PX = OX + 14 + static_cast<int32>(U * 257.f) - 8;
			const int32 PY = OY + 56 + static_cast<int32>(V * 267.f) - 8;
			if (WorldMapPlayerMarker)
			{
				WorldMapPlayerMarker->SetVisibility(Hit);
				for (FHudPiece& Piece : Pieces)
				{
					if (Piece.Widget.Get() == WorldMapPlayerMarker)
					{
						Piece.VirtualRect = FVector4(static_cast<float>(PX), static_cast<float>(PY), 17.f, 16.f);
						break;
					}
				}
			}
			Hover = FString::Printf(TEXT("You — LB 0x%04X  (%.1f, %.1f)"),
				(Cell >> 16) & 0xFFFF, Pos.Location.X, Pos.Location.Y);
		}
	}
	if (WorldMapHoverLabel)
	{
		WorldMapHoverLabel->SetText(FText::FromString(Hover));
		WorldMapHoverLabel->SetVisibility(Hit);
	}
	ApplyLetterboxLayout();
}

void UACEGameHUDWidget::OnCombatModeClicked()
{
	// Retail: peace ↔ combat for the currently equipped weapon class.
	// Wand/caster → Magic, missile weapon → Missile, melee/unarmed → Melee.
	const int32 Equipped = ResolveEquippedCombatMode();
	const int32 Next = (CombatMode == ACECombatMode::NonCombat) ? Equipped : static_cast<int32>(ACECombatMode::NonCombat);
	CombatMode = Next;
	if (Client)
	{
		Client->SendChangeCombatMode(CombatMode);
	}
	ApplyCombatModeVisual(CombatMode);
	ApplyPreferredStance(CombatMode);
	RefreshCombatHotbar();
}

int32 UACEGameHUDWidget::ResolveEquippedCombatMode() const
{
	if (!Client)
	{
		return ACECombatMode::Melee;
	}
	TSharedPtr<FACESession> Session = Client->GetSession();
	if (!Session)
	{
		return ACECombatMode::Melee;
	}
	const int32 Self = Session->GetPlayerGuid();
	bool bCaster = false;
	bool bMissile = false;
	bool bMelee = false;
	for (const TPair<int32, FACEWorldObject>& Pair : Session->GetWorldObjects())
	{
		const FACEWorldObject& Obj = Pair.Value;
		const bool bOurs = (Obj.ParentGuid == Self) || (Obj.WielderId == Self) || (Obj.ContainerId == 0 && Obj.ParentGuid == Self);
		if (!bOurs)
		{
			continue;
		}
		const int64 Loc = Obj.CurrentWieldedLocation;
		if ((Obj.ItemType & ACEItemType::Caster) != 0 || (Loc & ACEEquipMask::Held) != 0)
		{
			bCaster = true;
		}
		else if ((Obj.ItemType & ACEItemType::MissileWeapon) != 0 || (Loc & ACEEquipMask::MissileWeapon) != 0)
		{
			bMissile = true;
		}
		else if ((Obj.ItemType & ACEItemType::MeleeWeapon) != 0
			|| (Loc & (ACEEquipMask::MeleeWeapon | ACEEquipMask::TwoHanded)) != 0)
		{
			bMelee = true;
		}
	}
	// Wand priority matches ACE GetCombatStance / GetEquippedWand.
	if (bCaster) { return ACECombatMode::Magic; }
	if (bMissile) { return ACECombatMode::Missile; }
	if (bMelee) { return ACECombatMode::Melee; }
	return ACECombatMode::Melee; // unarmed HandCombat
}

void UACEGameHUDWidget::ApplyCombatModeVisual(int32 Mode)
{
	uint32 NormalDid = DidPeaceMode;
	uint32 PressedDid = DidPeaceModeDown;
	switch (Mode)
	{
	case ACECombatMode::Melee:
		NormalDid = DidMeleeMode; PressedDid = DidMeleeModeDown; break;
	case ACECombatMode::Missile:
		NormalDid = DidMissileMode; PressedDid = DidMissileModeDown; break;
	case ACECombatMode::Magic:
		NormalDid = DidMagicMode; PressedDid = DidMagicModeDown; break;
	default:
		break;
	}
	if (!CombatModeButton)
	{
		return;
	}
	FButtonStyle Style = CombatModeButton->GetStyle();
	if (UTexture2D* Tex = UiTex(NormalDid))
	{
		Style.Normal = MakeTexBrush(Tex, EACEHudArtTile::None);
		FSlateBrush Hovered = Style.Normal;
		Hovered.TintColor = FSlateColor(FLinearColor(1.12f, 1.12f, 1.12f, 1.f));
		Style.Hovered = Hovered;
	}
	if (UTexture2D* Tex = UiTex(PressedDid))
	{
		Style.Pressed = MakeTexBrush(Tex, EACEHudArtTile::None);
	}
	CombatModeButton->SetStyle(Style);
}

void UACEGameHUDWidget::ApplyPreferredStance(int32 Mode)
{
	APlayerController* PC = GetOwningPlayer();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UACECharacterAppearanceComponent* App = Pawn
		? Pawn->FindComponentByClass<UACECharacterAppearanceComponent>()
		: nullptr;
	if (!App)
	{
		return;
	}
	uint32 Stance = ACEMotion::StanceNonCombat;
	if (Client)
	{
		TArray<FACEWorldObject> Equipped = Client->GetEquippedItems();
		Stance = ACECombatStance::ResolveForCombatMode(Equipped, static_cast<uint32>(Mode));
	}
	App->SetPreferredStyle(static_cast<int32>(Stance));
}

void UACEGameHUDWidget::OnExamineClicked()
{
	if (!Client)
	{
		return;
	}
	const FACESelectedObject Sel = Client->GetSelectedObject();
	if (Sel.bValid && Sel.Guid != 0)
	{
		Client->SendIdentifyObject(Sel.Guid);
	}
	else
	{
		AppendChatLine(TEXT("Nothing selected to examine."), ChatSystemColor);
	}
}

void UACEGameHUDWidget::OnUseSelectedClicked()
{
	if (!Client)
	{
		return;
	}
	const FACESelectedObject Sel = Client->GetSelectedObject();
	if (!Sel.bValid || Sel.Guid == 0)
	{
		AppendChatLine(TEXT("Select an item first (left-click), then use the hand to pick it up."), ChatSystemColor);
		return;
	}

	if (AACEPlayerController* PC = Cast<AACEPlayerController>(GetOwningPlayer()))
	{
		PC->InteractWithObject(Sel.Guid);
		return;
	}

	FACEWorldObject Obj;
	if (Client->GetWorldObject(Sel.Guid, Obj) && ACEInventoryRules::IsWorldPickupOnUse(Obj))
	{
		Client->SendInventoryPickup(Sel.Guid,SelectedPackGuid);
	}
	else
	{
		Client->SendUseItem(Sel.Guid);
	}
}

void UACEGameHUDWidget::HandleSelectionChanged(const FACESelectedObject& Selection)
{
	LastSelection = Selection;
	RefreshSelectionUI();
	// Refresh open inspect panel when selection changes.
	if (Client && Selection.bValid && Selection.Guid != 0
		&& InspectPanel && InspectPanel->GetVisibility() != ESlateVisibility::Collapsed)
	{
		Client->SendIdentifyObject(Selection.Guid);
	}
}

void UACEGameHUDWidget::HandleAppraisal(const FACEAppraisalInfo& Appraisal)
{
	// Retail shows appraisal only in the examination window — never in chat.
	if (InspectTitle)
	{
		InspectTitle->SetText(FText::FromString(ACEAppraisalFormatting::ExaminationName(Appraisal)));
		InspectTitle->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	if (InspectBody)
	{
		InspectBody->SetText(FText::FromString(Appraisal.Summary.IsEmpty()
			? (Appraisal.bSuccess ? TEXT("") : TEXT("You fail to appraise the item."))
			: Appraisal.Summary));
		InspectBody->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	if (InspectPanel)
	{
		InspectPanel->SetVisibility(ESlateVisibility::Visible);
	}
	if (InspectEdgeChrome) { InspectEdgeChrome->SetVisibility(ESlateVisibility::HitTestInvisible); }
	if (InspectHeaderArt) { InspectHeaderArt->SetVisibility(ESlateVisibility::HitTestInvisible); }
	if (InspectModelFrame) { InspectModelFrame->SetVisibility(ESlateVisibility::HitTestInvisible); }
	UpdateInspectPreview(Appraisal.ObjectGuid);
}

bool UACEGameHUDWidget::EnsurePreviewRig(TObjectPtr<UTextureRenderTarget2D>& RenderTarget, TObjectPtr<AActor>& PreviewActor,
	TObjectPtr<USceneCaptureComponent2D>& Capture, int32 Width, int32 Height, const FVector& SpawnLocation)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (!RenderTarget)
	{
		RenderTarget = NewObject<UTextureRenderTarget2D>(this);
		RenderTarget->InitCustomFormat(Width, Height, PF_B8G8R8A8, false);
		RenderTarget->ClearColor = FLinearColor(0.015f, 0.015f, 0.02f, 1.f);
		RenderTarget->UpdateResourceImmediate(true);
	}
	if (!PreviewActor)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transient;
		PreviewActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnLocation, FRotator::ZeroRotator, Params);
		if (!PreviewActor)
		{
			return false;
		}
		// NOTE: the actor must stay game-visible — SetActorHiddenInGame(true) also hides it
		// from scene captures. Individual meshes are flagged VisibleInSceneCaptureOnly instead.
		USceneComponent* Root = NewObject<USceneComponent>(PreviewActor, TEXT("PreviewRoot"));
		PreviewActor->SetRootComponent(Root);
		Root->RegisterComponent();
		UACECharacterAppearanceComponent* Appearance = NewObject<UACECharacterAppearanceComponent>(PreviewActor, TEXT("PreviewAppearance"));
		Appearance->RegisterComponent();
		// The rig lives far below the map — bring its own light so the model isn't black.
		UPointLightComponent* Light = NewObject<UPointLightComponent>(PreviewActor, TEXT("PreviewLight"));
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetIntensity(3000.f);
		Light->SetAttenuationRadius(1200.f);
		Light->SetCastShadows(false);
		Light->SetRelativeLocation(FVector(-150.f, -80.f, 150.f));
		Capture = NewObject<USceneCaptureComponent2D>(PreviewActor, TEXT("PreviewCapture"));
		Capture->SetupAttachment(Root);
		Capture->RegisterComponent();
		Capture->TextureTarget = RenderTarget;
		Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->ShowFlags.SetAtmosphere(false);
		Capture->ShowFlags.SetFog(false);
		Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		Capture->SetRelativeLocation(FVector(-180.f, 0.f, 90.f));
		Capture->SetRelativeRotation(FRotator(-10.f, 0.f, 0.f));
		Capture->FOVAngle = 35.f;
	}
	return PreviewActor != nullptr && Capture != nullptr;
}

bool UACEGameHUDWidget::ApplyPreviewObject(AActor* PreviewActor, USceneCaptureComponent2D* Capture, const FACEWorldObject& Obj, float YawDegrees)
{
	if (!PreviewActor || !Capture || Obj.SetupId == 0)
	{
		return false;
	}
	UACECharacterAppearanceComponent* Appearance = PreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>();
	if (!Appearance)
	{
		return false;
	}
	FACEWorldObject Preview = Obj;
	Preview.bIsSelf = false; // never treat the rig as the hidden local player
	Appearance->ClearAppearance();
	if (!Appearance->ApplyWorldObject(Preview, 100.f, false))
	{
		return false;
	}
	Appearance->SetLocomotionInput(0.f, 0.f, false, 1.f);
	PreviewActor->SetActorRotation(FRotator(0.f, YawDegrees, 0.f));

	Capture->ClearShowOnlyComponents();
	TArray<UPrimitiveComponent*> Prims;
	PreviewActor->GetComponents<UPrimitiveComponent>(Prims);
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
		// Keep the rig out of the main view but inside the capture.
		Prim->SetHiddenInGame(false);
		Prim->SetVisibility(true, false);
		Prim->SetVisibleInSceneCaptureOnly(true);
		Capture->ShowOnlyComponent(Prim);
	}
	const float Height = FMath::Max(40.f, Appearance->GetVisualHeightCm());
	// The capture is attached to the actor root, so counter-rotate to keep the camera in front.
	const FVector CamOffset = FRotator(0.f, -YawDegrees, 0.f).RotateVector(FVector(-Height * 1.8f, 0.f, Height * 0.55f));
	Capture->SetRelativeLocation(CamOffset);
	Capture->SetRelativeRotation(FRotator(-8.f, -YawDegrees, 0.f));
	return true;
}

void UACEGameHUDWidget::EnsureInspectPreview()
{
	if (!EnsurePreviewRig(InspectRenderTarget, InspectPreviewActor, InspectCapture, 256, 320,
		FVector(-50000.f, -50000.f, -50000.f)))
	{
		return;
	}
	if (InspectModelImage && InspectRenderTarget && InspectModelImage->GetBrush().GetResourceObject() != InspectRenderTarget)
	{
		FSlateBrush Brush;
		Brush.SetResourceObject(InspectRenderTarget);
		Brush.ImageSize = FVector2D(256.f, 320.f);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		InspectModelImage->SetBrush(Brush);
	}
}

void UACEGameHUDWidget::UpdateInspectPreview(int32 ObjectGuid)
{
	EnsureInspectPreview();
	if (!Client || !InspectPreviewActor || ObjectGuid == 0)
	{
		ClearInspectPreview();
		return;
	}
	FACEWorldObject Obj;
	if (!Client->GetWorldObject(ObjectGuid, Obj)
		|| !ApplyPreviewObject(InspectPreviewActor, InspectCapture, Obj, 180.f))
	{
		if (InspectModelImage) { InspectModelImage->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	InspectPreviewGuid = ObjectGuid;
	if (InspectModelImage) { InspectModelImage->SetVisibility(ESlateVisibility::HitTestInvisible); }
}

void UACEGameHUDWidget::UpdatePaperDollPreview()
{
	if (!Client)
	{
		return;
	}
	const int32 Self = Client->GetPlayerGuid();
	if (Self == 0 || !PaperDollModelImage)
	{
		return;
	}
	if (!EnsurePreviewRig(PaperDollRenderTarget, PaperDollPreviewActor, PaperDollCapture, 192, 288,
		FVector(-50000.f, -48000.f, -50000.f)))
	{
		return;
	}
	if (PaperDollRenderTarget && PaperDollModelImage->GetBrush().GetResourceObject() != PaperDollRenderTarget)
	{
		FSlateBrush Brush;
		Brush.SetResourceObject(PaperDollRenderTarget);
		Brush.ImageSize = FVector2D(192.f, 288.f);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		PaperDollModelImage->SetBrush(Brush);
	}
	FACEWorldObject Obj;
	if (!Client->GetWorldObject(Self, Obj))
	{
		return;
	}
	// Only redo the (expensive) appearance rebuild when clothing ObjDesc changes.
	uint32 Hash = HashCombine(GetTypeHash(Obj.SetupId), GetTypeHash(Obj.Appearance.GetContentHash()));
	if (PaperDollAppliedGuid == Self && PaperDollAppliedHash == Hash)
	{
		return;
	}
	if (ApplyPreviewObject(PaperDollPreviewActor, PaperDollCapture, Obj, 180.f))
	{
		PaperDollAppliedGuid = Self;
		PaperDollAppliedHash = Hash;
		if (PaperDollCapture)
		{
			PaperDollCapture->CaptureScene();
		}
	}
}

void UACEGameHUDWidget::ClearInspectPreview()
{
	InspectPreviewGuid = 0;
	if (InspectModelImage) { InspectModelImage->SetVisibility(ESlateVisibility::Collapsed); }
	if (InspectPreviewActor)
	{
		if (UACECharacterAppearanceComponent* Appearance = InspectPreviewActor->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			Appearance->ClearAppearance();
		}
	}
}

void UACEGameHUDWidget::RefreshSelectionUI()
{
	if (SelectedNameLabel)
	{
		FACEWorldObject Object;
		const FString Name = LastSelection.bValid
			? Client && Client->GetWorldObject(LastSelection.Guid, Object) ? ACERetailObjectNames::Name(Object) : LastSelection.Name
			: FString();
		SelectedNameLabel->SetText(FText::FromString(Name));
	}
	if (SelectedHealthFill)
	{
		const bool bShow = LastSelection.bValid && LastSelection.bShowHealth;
		SelectedHealthFill->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UACEGameHUDWidget::OnLogoffClicked()
{
	if (Client)
	{
		Client->Logout();
	}
}

void UACEGameHUDWidget::OnCloseInspectClicked()
{
	if (InspectPanel) { InspectPanel->SetVisibility(ESlateVisibility::Collapsed); }
	if (InspectEdgeChrome) { InspectEdgeChrome->SetVisibility(ESlateVisibility::Collapsed); }
	if (InspectHeaderArt) { InspectHeaderArt->SetVisibility(ESlateVisibility::Collapsed); }
	if (InspectModelFrame) { InspectModelFrame->SetVisibility(ESlateVisibility::Collapsed); }
	if (InspectTitle) { InspectTitle->SetVisibility(ESlateVisibility::Collapsed); }
	if (InspectBody) { InspectBody->SetVisibility(ESlateVisibility::Collapsed); }
	ClearInspectPreview();
}

void UACEGameHUDWidget::OnCharTabAttributes()
{
	CharSheetTab = EACECharSheetTab::Attributes;
	RefreshPanelPage();
}

void UACEGameHUDWidget::OnCharTabSkills()
{
	CharSheetTab = EACECharSheetTab::Skills;
	RefreshPanelPage();
}

void UACEGameHUDWidget::OnCharTabTitles()
{
	CharSheetTab = EACECharSheetTab::Titles;
	RefreshPanelPage();
}

void UACEGameHUDWidget::OnUiLockClicked()
{
	ToggleUiLock();
}

void UACEGameHUDWidget::ToggleUiLock()
{
	bUiLocked = !bUiLocked;
	if (bUiLocked)
	{
		SaveWindowLayout();
	}
	UpdateUiLockVisual();
}

void UACEGameHUDWidget::UpdateUiLockVisual()
{
	if (!UiLockButton)
	{
		return;
	}
	// Locked uses normal lock art; unlocked uses the pressed/alternate state.
	const uint32 Did = bUiLocked ? HUDDidLockUi : DidLockUiDown;
	if (UTexture2D* Tex = UiTex(Did))
	{
		FButtonStyle Style = UiLockButton->GetStyle();
		Style.Normal = MakeTexBrush(Tex, EACEHudArtTile::None);
		Style.Hovered = Style.Normal;
		Style.Pressed = MakeTexBrush(UiTex(DidLockUiDown), EACEHudArtTile::None);
		UiLockButton->SetStyle(Style);
	}
}

void UACEGameHUDWidget::UpdateInventoryButtonVisual()
{
	if (!InventoryButton)
	{
		return;
	}
	const bool bOpen = CurrentPage == EACEHudPanelPage::Inventory;
	const uint32 Did = bOpen ? DidInventoryButtonDown : DidInventoryButton;
	if (UTexture2D* Tex = UiTex(Did))
	{
		FButtonStyle Style = InventoryButton->GetStyle();
		Style.Normal = MakeTexBrush(Tex, EACEHudArtTile::None);
		Style.Hovered = Style.Normal;
		Style.Pressed = MakeTexBrush(UiTex(DidInventoryButtonDown), EACEHudArtTile::None);
		InventoryButton->SetStyle(Style);
	}
}

namespace
{
	/** Header row for grouped panel lists (retail gold section captions). */
	UTextBlock* MakeListHeader(UWidgetTree* Tree, const FString& Text)
	{
		UTextBlock* Header = Tree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Header->SetText(FText::FromString(Text));
		Header->SetColorAndOpacity(FSlateColor(GoldColor));
		FSlateFontInfo Font = Header->GetFont();
		Font.Size = 10;
		Font.TypefaceFontName = FName(TEXT("Bold"));
		Header->SetFont(Font);
		Header->SetMargin(FMargin(2.f, 6.f, 0.f, 2.f));
		return Header;
	}
}

void UACEGameHUDWidget::RefreshSkillsUI()
{
	if (!SkillsScroll || !Client || !WidgetTree)
	{
		return;
	}
	uint32 Signature = GetTypeHash(LastVitals.Skills.Num()) ^ GetTypeHash(SelectedSkillId);
	for (const FACESkillInfo& Sk : LastVitals.Skills)
	{
		Signature = HashCombine(Signature, GetTypeHash(Sk.SkillId));
		Signature = HashCombine(Signature, GetTypeHash(Sk.Current));
		Signature = HashCombine(Signature, GetTypeHash(Sk.GetDisplayBase()));
		Signature = HashCombine(Signature, GetTypeHash(Sk.Ranks));
		Signature = HashCombine(Signature, GetTypeHash(Sk.XpSpent));
		Signature = HashCombine(Signature, GetTypeHash(Sk.AdvancementClass));
	}
	if (Signature == SkillsUiSignature && SkillsScroll->GetChildrenCount() > 0)
	{
		RefreshRaiseFooter();
		return;
	}
	SkillsUiSignature = Signature;
	SkillsScroll->ClearChildren();
	SkillRowButtons.Reset();
	SkillRowIds.Reset();

	auto AddSkillRow = [&](const FACESkillInfo& Sk)
	{
		UButton* RowBtn = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		FButtonStyle Style = RowBtn->GetStyle();
		const bool bSel = (Sk.SkillId == SelectedSkillId);
		Style.Normal = MakeRoundedBrush(bSel ? FLinearColor(0.28f, 0.22f, 0.10f, 0.85f) : FLinearColor(1.f, 1.f, 1.f, 0.04f), 2.f);
		Style.Hovered = MakeRoundedBrush(FLinearColor(0.34f, 0.28f, 0.14f, 0.9f), 2.f);
		Style.Pressed = Style.Hovered;
		Style.NormalPadding = FMargin(0.f);
		Style.PressedPadding = FMargin(0.f);
		RowBtn->SetStyle(Style);
		RowBtn->OnClicked.AddDynamic(this, &UACEGameHUDWidget::OnSkillRowClicked);

		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		RowBtn->AddChild(Box);

		FString DatName;
		uint32 IconDid = 0;
		if (Dat)
		{
			Dat->TryGetSkillInfo(static_cast<uint32>(Sk.SkillId), DatName, IconDid);
		}
		UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		FSlateBrush IconBrush;
		if (UTexture2D* Tex = UiTex(IconDid))
		{
			IconBrush.SetResourceObject(Tex);
		}
		else
		{
			IconBrush.TintColor = FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.35f));
		}
		IconBrush.ImageSize = FVector2D(18.f, 18.f);
		IconBrush.DrawAs = ESlateBrushDrawType::Image;
		Icon->SetBrush(IconBrush);
		Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (UHorizontalBoxSlot* IconSlot = Cast<UHorizontalBoxSlot>(Box->AddChild(Icon)))
		{
			IconSlot->SetPadding(FMargin(4.f, 2.f, 6.f, 2.f));
			IconSlot->SetVerticalAlignment(VAlign_Center);
		}

		UTextBlock* NameText = WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		NameText->SetText(FText::FromString(!Sk.Name.IsEmpty() ? Sk.Name : DatName));
		NameText->SetColorAndOpacity(FSlateColor(bSel ? GoldColor : HUDTextColor));
		NameText->SetVisibility(ESlateVisibility::HitTestInvisible);
		FSlateFontInfo NameFont = NameText->GetFont();
		NameFont.Size = 9;
		NameText->SetFont(NameFont);
		if (UHorizontalBoxSlot* NameSlot = Cast<UHorizontalBoxSlot>(Box->AddChild(NameText)))
		{
			NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			NameSlot->SetVerticalAlignment(VAlign_Center);
		}

		UTextBlock* ValueText = WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		ValueText->SetText(FText::AsNumber(Sk.Current));
		const int32 Modifier = Sk.GetDisplayModifier();
		ValueText->SetColorAndOpacity(FSlateColor(Modifier > 0 ? FLinearColor::Green : Modifier < 0 ? FLinearColor::Red : HUDTextColor));
		ValueText->SetVisibility(ESlateVisibility::HitTestInvisible);
		FSlateFontInfo ValueFont = ValueText->GetFont();
		ValueFont.Size = 10;
		ValueFont.TypefaceFontName = FName(TEXT("Bold"));
		ValueText->SetFont(ValueFont);
		if (UHorizontalBoxSlot* ValueSlot = Cast<UHorizontalBoxSlot>(Box->AddChild(ValueText)))
		{
			ValueSlot->SetVerticalAlignment(VAlign_Center);
			ValueSlot->SetPadding(FMargin(6.f, 0.f, 6.f, 0.f));
		}

		UACEHoverTooltipWidget::SetWidgetTooltip(RowBtn, FText::FromString(FString::Printf(
			TEXT("%s — base %d, %d ranks, %d XP spent\nClick to select, then raise with unassigned XP."),
			!Sk.Name.IsEmpty() ? *Sk.Name : *DatName, Sk.GetDisplayBase(), Sk.Ranks, Sk.XpSpent)));
		if (UScrollBoxSlot* RowSlot = Cast<UScrollBoxSlot>(SkillsScroll->AddChild(RowBtn)))
		{
			RowSlot->SetPadding(FMargin(0.f, 1.f));
		}
		SkillRowButtons.Add(RowBtn);
		SkillRowIds.Add(Sk.SkillId);
	};

	struct FGroup { int32 Sac; const TCHAR* Caption; bool bUnusable; };
	const FGroup Groups[] =
	{
		{ 3, TEXT("Specialized"), false },
		{ 2, TEXT("Trained"), false },
		{ 1, TEXT("Untrained"), false },
		{ 1, TEXT("Unusable"), true },
	};
	bool bAny = false;
	for (const FGroup& Group : Groups)
	{
		bool bHeaderAdded = false;
		for (const FACESkillInfo& Sk : LastVitals.Skills)
		{
			if (Sk.SkillId <= 0 || Sk.AdvancementClass != Group.Sac)
			{
				continue;
			}
			if (Group.Sac == 1)
			{
				uint32 MinLevel = 0;
				if (Dat)
				{
					Dat->TryGetSkillMinLevel(static_cast<uint32>(Sk.SkillId), MinLevel);
				}
				const bool bUnusable = (MinLevel != 0 && MinLevel != 1);
				if (bUnusable != Group.bUnusable)
				{
					continue;
				}
			}
			if (!bHeaderAdded)
			{
				SkillsScroll->AddChild(MakeListHeader(WidgetTree, Group.Caption));
				bHeaderAdded = true;
			}
			AddSkillRow(Sk);
			bAny = true;
		}
	}
	if (!bAny)
	{
		SkillsScroll->AddChild(MakeListHeader(WidgetTree, TEXT("No skills received yet.")));
	}
	if (SelectedSkillId == 0)
	{
		for (int32 Id : SkillRowIds)
		{
			// Prefer the first trained/specialized skill for the raise footer.
			for (const FACESkillInfo& Sk : LastVitals.Skills)
			{
				if (Sk.SkillId == Id && Sk.AdvancementClass >= 2)
				{
					SelectedSkillId = Id;
					break;
				}
			}
			if (SelectedSkillId != 0) { break; }
		}
		if (SelectedSkillId == 0 && SkillRowIds.Num() > 0)
		{
			SelectedSkillId = SkillRowIds[0];
		}
	}
	RefreshRaiseFooter();
}

void UACEGameHUDWidget::OnSkillRowClicked()
{
	for (int32 i = 0; i < SkillRowButtons.Num() && i < SkillRowIds.Num(); ++i)
	{
		if (SkillRowButtons[i] && SkillRowButtons[i]->IsHovered())
		{
			SelectSkillRow(i);
			return;
		}
	}
}

void UACEGameHUDWidget::SelectSkillRow(int32 SkillIndex)
{
	if (!SkillRowIds.IsValidIndex(SkillIndex))
	{
		return;
	}
	SelectedSkillId = SkillRowIds[SkillIndex];
	SkillsUiSignature = 0; // force row highlight rebuild
	RefreshSkillsUI();
}

void UACEGameHUDWidget::SetRaiseFooterVisible(bool bVisible)
{
	const ESlateVisibility Hit = bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	const ESlateVisibility Btn = bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	if (AttrSelectedName) { AttrSelectedName->SetVisibility(Hit); }
	if (AttrSelectedHint) { AttrSelectedHint->SetVisibility(Hit); }
	if (AttrRaiseMeterBack) { AttrRaiseMeterBack->SetVisibility(Hit); }
	if (AttrRaiseMeterFill) { AttrRaiseMeterFill->SetVisibility(Hit); }
	if (AttrRaiseXpText) { AttrRaiseXpText->SetVisibility(Hit); }
	if (AttrRaise1Button) { AttrRaise1Button->SetVisibility(Btn); }
	if (AttrRaise10Button) { AttrRaise10Button->SetVisibility(Btn); }
	if (AttrRaiseAllButton) { AttrRaiseAllButton->SetVisibility(Btn); }
}

void UACEGameHUDWidget::RefreshRaiseFooter()
{
	if (!LastVitals.bValid)
	{
		return;
	}
	if (AttrUnassignedXpText)
	{
		AttrUnassignedXpText->SetText(FText::FromString(
			FString::Printf(TEXT("Unassigned XP: %lld"), LastVitals.AvailableExperience)));
	}

	if (CharSheetTab == EACECharSheetTab::Skills)
	{
		const FACESkillInfo* Sel = nullptr;
		for (const FACESkillInfo& Sk : LastVitals.Skills)
		{
			if (Sk.SkillId == SelectedSkillId)
			{
				Sel = &Sk;
				break;
			}
		}
		if (!Sel)
		{
			if (AttrSelectedName) { AttrSelectedName->SetText(FText::FromString(TEXT("Select a skill"))); }
			if (AttrSelectedHint) { AttrSelectedHint->SetText(FText::FromString(TEXT("Trained and Specialized skills can be raised with unassigned XP."))); }
			if (AttrRaiseXpText) { AttrRaiseXpText->SetText(FText::GetEmpty()); }
			return;
		}
		if (AttrSelectedName) { AttrSelectedName->SetText(FText::FromString(Sel->Name)); }
		if (AttrSelectedHint)
		{
			AttrSelectedHint->SetText(FText::FromString(
				Sel->AdvancementClass >= 2
					? TEXT("Click +1 / +10 / Max to spend unassigned XP on this skill.")
					: TEXT("Click +1 to spend skill credits and train this skill.")));
		}
		int64 XpToNext = 0;
		int64 MaxSpend = 0;
		int32 TrainCost = 0;
		if (Dat)
		{
			if (Sel->AdvancementClass > 0 && Sel->AdvancementClass < 2)
			{
				Dat->TryGetSkillTrainedCost(static_cast<uint32>(Sel->SkillId), TrainCost);
			}
			else
			{
				Dat->TryGetSkillXpToNextRank(Sel->AdvancementClass, Sel->XpSpent, XpToNext, &MaxSpend);
			}
		}
		const int64 CanSpend = FMath::Min<int64>(LastVitals.AvailableExperience, MaxSpend);
		if (AttrRaiseXpText)
		{
			if (Sel->AdvancementClass > 0 && Sel->AdvancementClass < 2)
			{
				AttrRaiseXpText->SetText(FText::FromString(FString::Printf(
					TEXT("Credits to train: %d  (have %d)"), TrainCost, LastVitals.AvailableSkillCredits)));
			}
			else
			{
				AttrRaiseXpText->SetText(FText::FromString(
					XpToNext > 0
						? FString::Printf(TEXT("XP to raise: %lld  (can spend %lld)"), XpToNext, CanSpend)
						: (Sel->AdvancementClass >= 2 ? TEXT("Fully raised") : TEXT("Cannot raise"))));
			}
		}
		if (AttrRaiseMeterFill)
		{
			float Progress = 0.f;
			if (XpToNext > 0 && MaxSpend > 0)
			{
				Progress = 1.f - (static_cast<float>(XpToNext) / static_cast<float>(MaxSpend + XpToNext));
			}
			const int32 PieceIdx = Pieces.IndexOfByPredicate([this](const FHudPiece& P)
			{
				return P.Widget.Get() == AttrRaiseMeterFill;
			});
			if (PieceIdx != INDEX_NONE)
			{
				Pieces[PieceIdx].VirtualRect.Z = FMath::Max(1.f, 200.f * FMath::Clamp(Progress, 0.f, 1.f));
			}
		}
		return;
	}

	// Attributes tab — RefreshAttributePanel already fills the footer; keep unassigned XP fresh.
}

void UACEGameHUDWidget::RaiseSelectedSkill(int32 Multiplier)
{
	if (!Client || !Dat || SelectedSkillId == 0)
	{
		return;
	}
	const FACESkillInfo* Sel = nullptr;
	for (const FACESkillInfo& Sk : LastVitals.Skills)
	{
		if (Sk.SkillId == SelectedSkillId)
		{
			Sel = &Sk;
			break;
		}
	}
	if (!Sel || Sel->AdvancementClass <= 0)
	{
		return;
	}
	if (Sel->AdvancementClass < 2)
	{
		int32 Cost = 0;
		if (!Dat->TryGetSkillTrainedCost(static_cast<uint32>(Sel->SkillId), Cost))
		{
			Cost = 1;
		}
		if (LastVitals.AvailableSkillCredits < Cost)
		{
			return;
		}
		Client->SendTrainSkill(Sel->SkillId, Cost);
		return;
	}
	int64 XpToNext = 0;
	int64 MaxSpend = 0;
	Dat->TryGetSkillXpToNextRank(Sel->AdvancementClass, Sel->XpSpent, XpToNext, &MaxSpend, FMath::Max(1, Multiplier));
	if (XpToNext <= 0 || MaxSpend <= 0 || LastVitals.AvailableExperience <= 0)
	{
		return;
	}
	int64 Wanted = (Multiplier <= 0)
		? MaxSpend
		: FMath::Min<int64>(XpToNext, MaxSpend);
	if (Multiplier > 0 && LastVitals.AvailableExperience < Wanted) return;
	Wanted = FMath::Min<int64>(Wanted, LastVitals.AvailableExperience);
	Wanted = FMath::Max<int64>(1, Wanted);
	Client->SendRaiseSkill(Sel->SkillId, static_cast<int32>(FMath::Min<int64>(Wanted, MAX_int32)));
}

void UACEGameHUDWidget::RefreshSpellbookUI()
{
	if (!SpellbookScroll || !Client || !WidgetTree)
	{
		return;
	}
	const TArray<int32> RawSpells = Client->GetKnownSpells();
	TArray<int32> Spells;
	Spells.Reserve(RawSpells.Num());
	for (int32 SpellId : RawSpells)
	{
		if (SpellId <= 0)
		{
			continue;
		}
		// Only list spells the character actually knows (server spellbook), with a DAT name when possible.
		FString Name;
		uint32 IconDid = 0;
		if (Dat && !Dat->TryGetSpellInfo(static_cast<uint32>(SpellId), Name, IconDid) && Name.IsEmpty() && IconDid == 0)
		{
			// Still show server-known spells even if the DAT row is missing.
		}
		Spells.Add(SpellId);
	}
	// Retail spellbook sorts by SpellTable.DisplayOrder (not raw SpellId / learn order).
	if (UACEDatSubsystem* SpellDat = Dat.Get())
	{
		Spells.Sort([SpellDat](int32 A, int32 B)
		{
			uint32 OrderA = 0, OrderB = 0;
			SpellDat->TryGetSpellDisplayOrder(static_cast<uint32>(A), OrderA);
			SpellDat->TryGetSpellDisplayOrder(static_cast<uint32>(B), OrderB);
			if (OrderA != OrderB)
			{
				return OrderA < OrderB;
			}
			return A < B;
		});
	}
	uint32 Signature = GetTypeHash(Spells.Num());
	for (int32 SpellId : Spells)
	{
		Signature = HashCombine(Signature, GetTypeHash(SpellId));
	}
	if (PanelPageTitle && CurrentPage == EACEHudPanelPage::Spellbook)
	{
		PanelPageTitle->SetText(FText::FromString(TEXT("Spellbook")));
		PanelPageTitle->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	if (Signature == SpellbookUiSignature && SpellbookScroll->GetChildrenCount() > 0)
	{
		return;
	}
	SpellbookUiSignature = Signature;
	SpellbookScroll->ClearChildren();
	SpellRowBorders.Reset();
	SpellRowIds.Reset();
	SpellRowIconDids.Reset();

	SpellbookScroll->AddChild(MakeListHeader(WidgetTree,
		FString::Printf(TEXT("Known Spells (%d) — drag onto the spell bar"), Spells.Num())));

	for (int32 SpellId : Spells)
	{
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

		UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Row->SetBrush(MakeRoundedBrush(FLinearColor(1.f, 1.f, 1.f, 0.04f), 2.f));
		Row->SetPadding(FMargin(4.f, 2.f));
		Row->OnMouseButtonDownEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseDown);
		Row->OnMouseMoveEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseMove);
		Row->OnMouseButtonUpEvent.BindDynamic(this, &UACEGameHUDWidget::HandleWindowMouseUp);
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Row->SetContent(Box);

		UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		FSlateBrush IconBrush;
		if (UTexture2D* Tex = UiTex(IconDid))
		{
			IconBrush.SetResourceObject(Tex);
		}
		else
		{
			IconBrush.TintColor = FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.35f));
		}
		IconBrush.ImageSize = FVector2D(22.f, 22.f);
		IconBrush.DrawAs = ESlateBrushDrawType::Image;
		Icon->SetBrush(IconBrush);
		Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (UHorizontalBoxSlot* IconSlot = Cast<UHorizontalBoxSlot>(Box->AddChild(Icon)))
		{
			IconSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
			IconSlot->SetVerticalAlignment(VAlign_Center);
		}

		UTextBlock* NameText = WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		NameText->SetText(FText::FromString(SpellName));
		NameText->SetColorAndOpacity(FSlateColor(HUDTextColor));
		FSlateFontInfo NameFont = NameText->GetFont();
		NameFont.Size = 9;
		NameText->SetFont(NameFont);
		NameText->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (UHorizontalBoxSlot* NameSlot = Cast<UHorizontalBoxSlot>(Box->AddChild(NameText)))
		{
			NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			NameSlot->SetVerticalAlignment(VAlign_Center);
		}

		UACEHoverTooltipWidget::SetWidgetTooltip(Row, FText::FromString(FString::Printf(TEXT("%s\nDrag onto the combat spell bar to memorize."), *SpellName)));
		if (UScrollBoxSlot* RowSlot = Cast<UScrollBoxSlot>(SpellbookScroll->AddChild(Row)))
		{
			RowSlot->SetPadding(FMargin(0.f, 1.f));
		}
		SpellRowBorders.Add(Row);
		SpellRowIds.Add(SpellId);
		SpellRowIconDids.Add(static_cast<int32>(IconDid));
	}
	if (Spells.Num() == 0)
	{
		SpellbookScroll->AddChild(MakeListHeader(WidgetTree, TEXT("No spells known yet.")));
	}
}

void UACEGameHUDWidget::RefreshCombatHotbar()
{
	if (!CombatHotbarRoot || !Client)
	{
		return;
	}
	const bool bInCombat = CombatMode != static_cast<int32>(ACECombatMode::NonCombat);
	SetCombatHotbarVisible(bInCombat);
	if (!bInCombat)
	{
		return;
	}

	const int32 ActiveBar = FMath::Clamp(Client->GetActiveSpellBar(), 0, 7);
	const TArray<int32> Bar = Client->GetSpellBar(ActiveBar);

	for (int32 i = 0; i < CombatSpellTabButtons.Num(); ++i)
	{
		if (UButton* Tab = CombatSpellTabButtons[i].Get())
		{
			const uint32 Did = (i == ActiveBar) ? DidSpellTabActive : DidSpellTabInactive;
			if (UTexture2D* Tex = UiTex(Did))
			{
				FButtonStyle Style = Tab->GetStyle();
				Style.Normal = MakeTexBrush(Tex, EACEHudArtTile::None);
				Style.Hovered = Style.Normal;
				Style.Pressed = Style.Normal;
				Tab->SetStyle(Style);
			}
		}
	}

	for (int32 i = 0; i < CombatSpellButtons.Num(); ++i)
	{
		const int32 SpellId = Bar.IsValidIndex(i) ? Bar[i] : 0;
		FString SpellName;
		uint32 IconDid = 0;
		if (SpellId != 0 && Dat)
		{
			Dat->TryGetSpellInfo(static_cast<uint32>(SpellId), SpellName, IconDid);
		}
		if (UBorder* IconCell = CombatSpellIconCells.IsValidIndex(i) ? CombatSpellIconCells[i].Get() : nullptr)
		{
			SetInventorySlotIcon(IconCell, static_cast<int32>(IconDid),
				i < 9 ? FLinearColor(0.05f, 0.08f, 0.16f, 0.85f) : BarBackColor);
		}
		if (UBorder* Overlay = CombatSpellSelectedOverlays.IsValidIndex(i) ? CombatSpellSelectedOverlays[i].Get() : nullptr)
		{
			const bool bSel = (i == SelectedCombatSpellSlot && SpellId != 0);
			Overlay->SetVisibility(bSel ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			if (bSel)
			{
				if (UTexture2D* SelTex = UiTex(DidSpellSelected))
				{
					Overlay->SetBrush(MakeTexBrush(SelTex, EACEHudArtTile::None));
					Overlay->SetBrushColor(FLinearColor::White);
				}
			}
		}
		if (UButton* Btn = CombatSpellButtons[i].Get())
		{
			Btn->SetIsEnabled(SpellId != 0 || i < 9);
		}
	}

	const int32 SelSpell = Bar.IsValidIndex(SelectedCombatSpellSlot) ? Bar[SelectedCombatSpellSlot] : 0;
	FString SelName;
	uint32 SelIcon = 0;
	if (SelSpell != 0 && Dat)
	{
		Dat->TryGetSpellInfo(static_cast<uint32>(SelSpell), SelName, SelIcon);
	}
	if (CombatSpellNameLabel)
	{
		CombatSpellNameLabel->SetText(FText::FromString(SelName));
	}
	if (CastSpellButton)
	{
		CastSpellButton->SetIsEnabled(SelSpell != 0);
	}
}

void UACEGameHUDWidget::SetCombatHotbarVisible(bool bVisible)
{
	const ESlateVisibility PieceVisibility = bVisible
		? ESlateVisibility::Visible
		: ESlateVisibility::Collapsed;
	for (const FHudPiece& Piece : Pieces)
	{
		if (Piece.WindowIndex == WindowCombat)
		{
			if (UWidget* Widget = Piece.Widget.Get())
			{
				Widget->SetVisibility(PieceVisibility);
			}
		}
	}
	// Decorative chrome must never steal clicks from the spell-slot buttons.
	if (bVisible)
	{
		for (UBorder* IconCell : CombatSpellIconCells)
		{
			if (IconCell) { IconCell->SetVisibility(ESlateVisibility::HitTestInvisible); }
		}
		for (UBorder* Overlay : CombatSpellSelectedOverlays)
		{
			if (Overlay && Overlay->GetVisibility() != ESlateVisibility::Collapsed)
			{
				Overlay->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
		}
		for (UTextBlock* Num : CombatSpellSlotNumbers)
		{
			if (Num) { Num->SetVisibility(ESlateVisibility::HitTestInvisible); }
		}
		if (CombatSpellNameLabel)
		{
			CombatSpellNameLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
	}
}

void UACEGameHUDWidget::ActivateHotbarSlot(int32 SlotIndex)
{
	if (!Client || SlotIndex < 0 || SlotIndex >= 9)
	{
		return;
	}
	const bool bSpellBarOpen = CombatHotbarRoot
		&& CombatHotbarRoot->GetVisibility() != ESlateVisibility::Collapsed
		&& CombatHotbarRoot->GetVisibility() != ESlateVisibility::Hidden;
	if (bSpellBarOpen || CombatMode != static_cast<int32>(ACECombatMode::NonCombat))
	{
		CastSpellFromBar(SlotIndex);
		return;
	}

	const int32 ObjectGuid = Client->GetShortcutObject(SlotIndex);
	if (ObjectGuid == 0)
	{
		return;
	}
	// Mirror DAT binder: wield gear / Use consumables & gems (not bare Use for weapons).
	FACEWorldObject Obj;
	if (!Client->GetWorldObject(ObjectGuid, Obj))
	{
		Client->SendUseItem(ObjectGuid);
		return;
	}
	Client->SelectObject(ObjectGuid);
	if (Obj.CurrentWieldedLocation != 0
		&& (Obj.WielderId == Client->GetPlayerGuid() || Obj.ParentGuid == Client->GetPlayerGuid()))
	{
		Client->SendPutItemInContainer(ObjectGuid, Client->GetPlayerGuid(), 0);
		return;
	}
	const bool bCanWield = Obj.ValidLocations != 0
		&& (Obj.IsWieldOnUse()
			|| ((Obj.ItemType & (ACEItemType::Armor | ACEItemType::Clothing | ACEItemType::Jewelry
				| ACEItemType::MeleeWeapon | ACEItemType::MissileWeapon | ACEItemType::Caster)) != 0));
	const bool bNeverWield = (Obj.ItemType & (ACEItemType::Gem | ACEItemType::Food | ACEItemType::Money
		| ACEItemType::Container | ACEItemType::SpellComponents | ACEItemType::Writable
		| ACEItemType::Key | ACEItemType::Misc | ACEItemType::Useless)) != 0;
	if (bCanWield && !bNeverWield)
	{
		int64 Loc = Obj.ValidLocations;
		static const int64 WieldPriority[] = {
			ACEEquipMask::Held, ACEEquipMask::MeleeWeapon, ACEEquipMask::TwoHanded,
			ACEEquipMask::MissileWeapon, ACEEquipMask::Shield, ACEEquipMask::MissileAmmo,
		};
		for (const int64 Bit : WieldPriority)
		{
			if ((Obj.ValidLocations & Bit) != 0) { Loc = Bit; break; }
		}
		if ((Obj.ItemType & (ACEItemType::Armor | ACEItemType::Clothing | ACEItemType::Jewelry)) != 0)
		{
			Loc = Obj.ValidLocations;
		}
		if (Loc != 0)
		{
			Client->SendGetAndWieldItem(ObjectGuid, Loc);
			return;
		}
	}
	Client->SendUseItem(ObjectGuid);
}

void UACEGameHUDWidget::SelectCombatSpellSlot(int32 SlotIndex)
{
	SelectedCombatSpellSlot = FMath::Clamp(SlotIndex, 0, 11);
	RefreshCombatHotbar();
}

void UACEGameHUDWidget::HandleCombatSpellSlotClicked(int32 SlotIndex)
{
	SelectCombatSpellSlot(SlotIndex);
	// Retail: clicking a memorized spell casts it (Cast button also works for the selection).
	CastSpellFromBar(SlotIndex);
}

void UACEGameHUDWidget::CastSpellFromBar(int32 SlotIndex)
{
	if (!Client)
	{
		return;
	}
	const int32 ActiveBar = FMath::Clamp(Client->GetActiveSpellBar(), 0, 7);
	const TArray<int32> Bar = Client->GetSpellBar(ActiveBar);
	if (!Bar.IsValidIndex(SlotIndex) || Bar[SlotIndex] == 0)
	{
		return;
	}
	if (!Client->SendCastSpell(Bar[SlotIndex]))
	{
		AppendChatLine(TEXT("You must select a target for that spell."), ChatSystemColor);
	}
}

void UACEGameHUDWidget::OnCastSpellClicked()
{
	CastSpellFromBar(SelectedCombatSpellSlot);
}

void UACEGameHUDWidget::OnSpellTab0() { if (Client) { Client->SetActiveSpellBar(0); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }
void UACEGameHUDWidget::OnSpellTab1() { if (Client) { Client->SetActiveSpellBar(1); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }
void UACEGameHUDWidget::OnSpellTab2() { if (Client) { Client->SetActiveSpellBar(2); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }
void UACEGameHUDWidget::OnSpellTab3() { if (Client) { Client->SetActiveSpellBar(3); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }
void UACEGameHUDWidget::OnSpellTab4() { if (Client) { Client->SetActiveSpellBar(4); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }
void UACEGameHUDWidget::OnSpellTab5() { if (Client) { Client->SetActiveSpellBar(5); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }
void UACEGameHUDWidget::OnSpellTab6() { if (Client) { Client->SetActiveSpellBar(6); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }
void UACEGameHUDWidget::OnSpellTab7() { if (Client) { Client->SetActiveSpellBar(7); SelectedCombatSpellSlot = 0; RefreshCombatHotbar(); } }

void UACEGameHUDWidget::OnCombatSpell0() { HandleCombatSpellSlotClicked(0); }
void UACEGameHUDWidget::OnCombatSpell1() { HandleCombatSpellSlotClicked(1); }
void UACEGameHUDWidget::OnCombatSpell2() { HandleCombatSpellSlotClicked(2); }
void UACEGameHUDWidget::OnCombatSpell3() { HandleCombatSpellSlotClicked(3); }
void UACEGameHUDWidget::OnCombatSpell4() { HandleCombatSpellSlotClicked(4); }
void UACEGameHUDWidget::OnCombatSpell5() { HandleCombatSpellSlotClicked(5); }
void UACEGameHUDWidget::OnCombatSpell6() { HandleCombatSpellSlotClicked(6); }
void UACEGameHUDWidget::OnCombatSpell7() { HandleCombatSpellSlotClicked(7); }
void UACEGameHUDWidget::OnCombatSpell8() { HandleCombatSpellSlotClicked(8); }
void UACEGameHUDWidget::OnCombatSpell9() { HandleCombatSpellSlotClicked(9); }
void UACEGameHUDWidget::OnCombatSpell10() { HandleCombatSpellSlotClicked(10); }
void UACEGameHUDWidget::OnCombatSpell11() { HandleCombatSpellSlotClicked(11); }

void UACEGameHUDWidget::LoadWindowLayout()
{
	if (!GConfig)
	{
		return;
	}
	const TCHAR* Section = TEXT("ACEClient.HUD");
	bool bLocked = bUiLocked;
	GConfig->GetBool(Section, TEXT("UiLocked"), bLocked, GGameUserSettingsIni);
	bUiLocked = bLocked;
	for (FHudWindow& Window : Windows)
	{
		if (Window.PersistKey.IsNone())
		{
			continue;
		}
		float OX = 0.f, OY = 0.f;
		const FString KeyX = Window.PersistKey.ToString() + TEXT("_X");
		const FString KeyY = Window.PersistKey.ToString() + TEXT("_Y");
		if (GConfig->GetFloat(Section, *KeyX, OX, GGameUserSettingsIni)
			&& GConfig->GetFloat(Section, *KeyY, OY, GGameUserSettingsIni))
		{
			Window.Offset = FVector2D(OX, OY);
		}
	}
	float ExtraH = PanelExtraHeight;
	if (GConfig->GetFloat(Section, TEXT("PanelExtraHeight"), ExtraH, GGameUserSettingsIni))
	{
		PanelExtraHeight = FMath::Clamp(ExtraH, -200.f, 400.f);
		if (Windows.IsValidIndex(WindowPanel))
		{
			Windows[WindowPanel].Size.Y = 430.f + PanelExtraHeight;
		}
	}
	float ChatExtra = ChatExtraHeight;
	if (GConfig->GetFloat(Section, TEXT("ChatExtraHeight"), ChatExtra, GGameUserSettingsIni))
	{
		ChatExtraHeight = ChatExtra;
		ApplyChatExtraHeight();
	}
}

void UACEGameHUDWidget::SaveWindowLayout() const
{
	if (!GConfig)
	{
		return;
	}
	const TCHAR* Section = TEXT("ACEClient.HUD");
	GConfig->SetBool(Section, TEXT("UiLocked"), bUiLocked, GGameUserSettingsIni);
	for (const FHudWindow& Window : Windows)
	{
		if (Window.PersistKey.IsNone())
		{
			continue;
		}
		const FString KeyX = Window.PersistKey.ToString() + TEXT("_X");
		const FString KeyY = Window.PersistKey.ToString() + TEXT("_Y");
		GConfig->SetFloat(Section, *KeyX, Window.Offset.X, GGameUserSettingsIni);
		GConfig->SetFloat(Section, *KeyY, Window.Offset.Y, GGameUserSettingsIni);
	}
	GConfig->SetFloat(Section, TEXT("PanelExtraHeight"), PanelExtraHeight, GGameUserSettingsIni);
	GConfig->SetFloat(Section, TEXT("ChatExtraHeight"), ChatExtraHeight, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

FVector2D UACEGameHUDWidget::ScreenToVirtual(const FVector2D& ScreenPos) const
{
	const FVector2D Local = GetCachedGeometry().AbsoluteToLocal(ScreenPos);
	if (LastScale <= KINDA_SMALL_NUMBER)
	{
		return FVector2D::ZeroVector;
	}
	return (Local - LastLetterboxOffset) / LastScale;
}

void UACEGameHUDWidget::GetVirtualScreenBounds(FVector2D& OutMin, FVector2D& OutMax) const
{
	// Full viewport in virtual units (includes letterbox margins beyond 800x600).
	if (LastScale <= KINDA_SMALL_NUMBER)
	{
		OutMin = FVector2D::ZeroVector;
		OutMax = FVector2D(ACEUI::ReferenceWidth, ACEUI::ReferenceHeight);
		return;
	}
	OutMin = -LastLetterboxOffset / LastScale;
	OutMax = OutMin + LastViewportSize / LastScale;
}

bool UACEGameHUDWidget::WidgetContains(const UWidget* Widget, const FVector2D& ScreenPos)
{
	return Widget
		&& Widget->GetVisibility() != ESlateVisibility::Collapsed
		&& Widget->GetVisibility() != ESlateVisibility::Hidden
		&& Widget->GetCachedGeometry().IsUnderLocation(ScreenPos);
}

void UACEGameHUDWidget::BeginPendingDrag(EACEDragKind Kind, int32 PayloadId, int32 IconDid, const FVector2D& ScreenPos,
	int32 SourcePackGuid, int32 SourceSlotIndex)
{
	DragKind = Kind;
	DragPayloadId = PayloadId;
	DragIconDid = IconDid;
	DragSourcePackGuid = SourcePackGuid;
	DragSourceSlotIndex = SourceSlotIndex;
	DragStartScreen = ScreenPos;
	bDragActive = false;
}

bool UACEGameHUDWidget::TryStartCellDrag(const FVector2D& ScreenPos)
{
	if (!Client || DragKind != EACEDragKind::None)
	{
		return DragKind != EACEDragKind::None;
	}
	// Inventory grid items.
	for (int32 i = 0; i < InventoryIconSlots.Num() && i < InventoryCellGuids.Num(); ++i)
	{
		if (InventoryCellGuids[i] != 0 && WidgetContains(InventoryIconSlots[i].Get(), ScreenPos))
		{
			FACEWorldObject Obj;
			const int32 IconDid = Client->GetWorldObject(InventoryCellGuids[i], Obj) ? Obj.IconId : 0;
			BeginPendingDrag(EACEDragKind::Item, InventoryCellGuids[i], IconDid, ScreenPos, SelectedPackGuid, i);
			return true;
		}
	}
	// Equipped items on the paper doll.
	for (int32 i = 0; i < PaperDollSlots.Num() && i < PaperDollGuids.Num(); ++i)
	{
		if (PaperDollGuids[i] != 0 && WidgetContains(PaperDollSlots[i].Get(), ScreenPos))
		{
			FACEWorldObject Obj;
			const int32 IconDid = Client->GetWorldObject(PaperDollGuids[i], Obj) ? Obj.IconId : 0;
			BeginPendingDrag(EACEDragKind::Item, PaperDollGuids[i], IconDid, ScreenPos, 0, INDEX_NONE);
			return true;
		}
	}
	// Spellbook rows — pick the closest row under the cursor (scroll geometry can overlap).
	{
		int32 BestIdx = INDEX_NONE;
		float BestDistSq = TNumericLimits<float>::Max();
		for (int32 i = 0; i < SpellRowBorders.Num() && i < SpellRowIds.Num(); ++i)
		{
			UWidget* Row = SpellRowBorders[i].Get();
			if (!Row || Row->GetVisibility() == ESlateVisibility::Collapsed
				|| Row->GetVisibility() == ESlateVisibility::Hidden)
			{
				continue;
			}
			const FGeometry& Geo = Row->GetCachedGeometry();
			if (!Geo.IsUnderLocation(ScreenPos))
			{
				continue;
			}
			const FVector2D Center = Geo.GetAbsolutePosition() + Geo.GetAbsoluteSize() * 0.5f;
			const float DistSq = FVector2D::DistSquared(Center, ScreenPos);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				BestIdx = i;
			}
		}
		if (BestIdx != INDEX_NONE)
		{
			BeginPendingDrag(EACEDragKind::Spell, SpellRowIds[BestIdx],
				SpellRowIconDids.IsValidIndex(BestIdx) ? SpellRowIconDids[BestIdx] : 0,
				ScreenPos, 0, INDEX_NONE);
			return true;
		}
	}
	return false;
}

void UACEGameHUDWidget::UpdateDragVisual(const FVector2D& ScreenPos)
{
	if (DragKind == EACEDragKind::None)
	{
		return;
	}
	if (!bDragActive)
	{
		if (FVector2D::Distance(ScreenPos, DragStartScreen) < 6.f)
		{
			return;
		}
		bDragActive = true;
	}
	if (!DragIconImage && WidgetTree && RootCanvas)
	{
		DragIconImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		DragIconImage->SetVisibility(ESlateVisibility::HitTestInvisible);
		RootCanvas->AddChild(DragIconImage);
	}
	if (!DragIconImage)
	{
		return;
	}
	FSlateBrush Brush;
	UTexture2D* Tex = DragKind == EACEDragKind::Item && Client
		? ACEItemDragIcon::Resolve(Client->GetUIResourceResolver(), Client->GetSession().Get(), DragPayloadId, DragIconDid)
		: nullptr;
	if (!Tex) Tex = UiTex(static_cast<uint32>(DragIconDid));
	if (Tex)
	{
		Brush.SetResourceObject(Tex);
	}
	else
	{
		Brush.TintColor = FSlateColor(FLinearColor(0.9f, 0.85f, 0.4f, 0.8f));
	}
	Brush.ImageSize = FVector2D(32.f, 32.f);
	Brush.DrawAs = ESlateBrushDrawType::Image;
	DragIconImage->SetBrush(Brush);
	DragIconImage->SetRenderOpacity(0.85f);
	DragIconImage->SetVisibility(ESlateVisibility::HitTestInvisible);
	if (UCanvasPanelSlot* IconSlot = Cast<UCanvasPanelSlot>(DragIconImage->Slot))
	{
		const FVector2D Local = GetCachedGeometry().AbsoluteToLocal(ScreenPos);
		IconSlot->SetAnchors(FAnchors(0.f, 0.f));
		IconSlot->SetAlignment(FVector2D(0.f, 0.f));
		IconSlot->SetAutoSize(false);
		IconSlot->SetPosition(Local - FVector2D(16.f, 16.f));
		IconSlot->SetSize(FVector2D(32.f, 32.f));
		IconSlot->SetZOrder(100);
	}
}

void UACEGameHUDWidget::ReleasePointerCapture()
{
	// Tick-finished drags never go through HandleWindowMouseUp's ReleaseMouseCapture reply.
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().ReleaseAllPointerCapture();
	}
}

void UACEGameHUDWidget::CancelDrag()
{
	DragKind = EACEDragKind::None;
	DragPayloadId = 0;
	DragIconDid = 0;
	DragSourcePackGuid = 0;
	DragSourceSlotIndex = INDEX_NONE;
	bDragActive = false;
	if (DragIconImage)
	{
		DragIconImage->SetVisibility(ESlateVisibility::Collapsed);
	}
	ReleasePointerCapture();
}

void UACEGameHUDWidget::FinishDrag(const FVector2D& ScreenPos)
{
	const EACEDragKind Kind = DragKind;
	const int32 Payload = DragPayloadId;
	const bool bWasActive = bDragActive;
	CancelDrag();
	if (!bWasActive || Kind == EACEDragKind::None || Payload == 0 || !Client)
	{
		return;
	}

	if (Kind == EACEDragKind::Item)
	{
		// Paper doll slot → equip.
		for (int32 i = 0; i < PaperDollSlots.Num(); ++i)
		{
			if (WidgetContains(PaperDollSlots[i].Get(), ScreenPos))
			{
				Client->SendGetAndWieldItem(Payload, GDollMasks[i]);
				return;
			}
		}
		// Bag button → move into that pack.
		for (int32 i = 0; i < PackTabButtons.Num() && i < PackTabGuids.Num(); ++i)
		{
			if (PackTabGuids[i] != 0 && WidgetContains(PackTabButtons[i].Get(), ScreenPos))
			{
				Client->SendPutItemInContainer(Payload, PackTabGuids[i], 0);
				return;
			}
		}
		// A grid drop either merges with the item under the cursor or inserts
		// before that slot, taking the source's original slot into account.
		for (int32 i = 0; i < InventoryIconSlots.Num(); ++i)
		{
			if (WidgetContains(InventoryIconSlots[i].Get(), ScreenPos))
			{
				const int32 Container=SelectedPackGuid ? SelectedPackGuid : Client->GetPlayerGuid();
				const int32 Target=InventoryCellGuids.IsValidIndex(i) ? InventoryCellGuids[i] : 0;
				FACEWorldObject Source,Destination;
				if(!Client->GetWorldObject(Payload,Source) || ACEInventoryRules::IsContainer(Source))return;
				if(Client->GetWorldObject(Target,Destination))
					if(const int32 Amount=Client->GetSession()->AvailableMergeAmount(Source,Destination);
						Amount>0 && !Client->GetTradeSelfItems().Contains(Payload) && !Client->GetTradeSelfItems().Contains(Target))
					{
						Client->SendStackableMerge(Payload,Target,Amount);
						Client->SelectObject(Target);
						return;
					}
				TArray<int32> Slots;
				if(const auto Session=Client->GetSession())Session->GetPackItemGuids(Container,Slots);
				const int32 Placement=ACEInventoryRules::DropInsertionIndex(Slots,Payload,Target,true);
				if(Placement!=INDEX_NONE)Client->SendPutItemInContainer(Payload,Container,Placement,false);
				return;
			}
		}
		// Anywhere outside the inventory panel → drop on the ground.
		if (InventoryRoot && !WidgetContains(InventoryRoot, ScreenPos))
		{
			FACEWorldObject Obj;
			if (Client->GetWorldObject(Payload, Obj) && !Obj.CanDropToWorld())
			{
				AppendChatLine(TEXT("You cannot drop that item."), ChatSystemColor);
				return;
			}
			Client->SendDropItem(Payload);
		}
		return;
	}

	// Spell → combat hotbar slot (icons are HitTestInvisible; also accept the button rects).
	for (int32 i = 0; i < CombatSpellButtons.Num(); ++i)
	{
		UWidget* Target = CombatSpellButtons[i].Get();
		if (!Target && CombatSpellIconCells.IsValidIndex(i))
		{
			Target = CombatSpellIconCells[i].Get();
		}
		if (WidgetContains(Target, ScreenPos))
		{
			Client->SendAddSpellToBar(Payload, i, Client->GetActiveSpellBar());
			RefreshCombatHotbar();
			return;
		}
	}
}

bool UACEGameHUDWidget::IsLeftMouseButtonDown() const
{
	return FSlateApplication::IsInitialized()
		&& FSlateApplication::Get().GetPressedMouseButtons().Contains(EKeys::LeftMouseButton);
}

void UACEGameHUDWidget::TickPointerCapture(float /*InDeltaTime*/)
{
	if (DragKind == EACEDragKind::None && !bResizingPanel && !bResizingChat)
	{
		return;
	}
	// PlayerController key state ignores Slate-captured UI clicks — use Slate's pressed set.
	const FVector2D CursorPos = FSlateApplication::Get().GetCursorPos();
	if (!IsLeftMouseButtonDown())
	{
		if (DragKind != EACEDragKind::None)
		{
			FinishDrag(CursorPos);
		}
		if (bResizingPanel || bResizingChat)
		{
			bResizingPanel = false;
			bResizingChat = false;
			ReleasePointerCapture();
			SaveWindowLayout();
		}
		return;
	}
	if (DragKind != EACEDragKind::None)
	{
		UpdateDragVisual(CursorPos);
	}
	if (bResizingPanel && Windows.IsValidIndex(WindowPanel))
	{
		const FVector2D Virtual = ScreenToVirtual(CursorPos);
		const float PanelTop = Windows[WindowPanel].DefaultTopLeft.Y + Windows[WindowPanel].Offset.Y;
		PanelExtraHeight = FMath::Clamp(Virtual.Y - PanelTop - 422.f, -200.f, 400.f);
		Windows[WindowPanel].Size.Y = 430.f + PanelExtraHeight;
	}
	if (bResizingChat && Windows.IsValidIndex(WindowChat))
	{
		const FVector2D Virtual = ScreenToVirtual(CursorPos);
		// Base top is virtual y=500 (+ window offset). Dragging the grip up grows history.
		ChatExtraHeight = FMath::Clamp(500.f + Windows[WindowChat].Offset.Y - Virtual.Y, 0.f, 280.f);
		ApplyChatExtraHeight();
	}
}

FEventReply UACEGameHUDWidget::HandleWindowMouseDown(FGeometry MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		return UWidgetBlueprintLibrary::Unhandled();
	}

	const FVector2D ScreenPos = MouseEvent.GetScreenSpacePosition();
	const FVector2D Virtual = ScreenToVirtual(ScreenPos);

	// Pack tabs beyond the first eight (no UFUNCTION OnClicked) — resolve by geometry.
	if (CurrentPage == EACEHudPanelPage::Inventory)
	{
		for (int32 i = 0; i < PackTabButtons.Num(); ++i)
		{
			if (i < 8)
			{
				continue;
			}
			if (PackTabGuids.IsValidIndex(i) && PackTabGuids[i] != 0
				&& WidgetContains(PackTabButtons[i].Get(), ScreenPos))
			{
				OnInventoryPackClicked(i);
				return UWidgetBlueprintLibrary::Handled();
			}
		}
	}

	// Icon drags come before window drags (an item picked up must not move the panel).
	if (TryStartCellDrag(ScreenPos))
	{
		FEventReply Reply = UWidgetBlueprintLibrary::Handled();
		// Capture on a widget that has our move/up delegates (not the UserWidget itself).
		UWidget* CaptureTarget = InventoryRoot;
		if (!CaptureTarget || CaptureTarget->GetVisibility() == ESlateVisibility::Collapsed)
		{
			CaptureTarget = SpellbookScroll;
		}
		if (!CaptureTarget)
		{
			CaptureTarget = PanelResizeGrip;
		}
		if (CaptureTarget)
		{
			Reply = UWidgetBlueprintLibrary::CaptureMouse(Reply, CaptureTarget);
		}
		return Reply;
	}

	// Bottom-edge grip of the right panel (works even when the UI is locked).
	if (CurrentPage != EACEHudPanelPage::None && Windows.IsValidIndex(WindowPanel))
	{
		const FVector2D TopLeft = Windows[WindowPanel].DefaultTopLeft + Windows[WindowPanel].Offset;
		const float Bottom = TopLeft.Y + 422.f + PanelExtraHeight;
		const bool bOverGrip = Virtual.X >= TopLeft.X && Virtual.X <= TopLeft.X + Windows[WindowPanel].Size.X
			&& Virtual.Y >= Bottom - 8.f && Virtual.Y <= Bottom + 12.f;
		if (bOverGrip || (PanelResizeGrip && WidgetContains(PanelResizeGrip, ScreenPos)))
		{
			bResizingPanel = true;
			FEventReply Reply = UWidgetBlueprintLibrary::Handled();
			if (PanelResizeGrip)
			{
				Reply = UWidgetBlueprintLibrary::CaptureMouse(Reply, PanelResizeGrip);
			}
			return Reply;
		}
	}

	// Top-edge grip of the chat window (works even when the UI is locked).
	if (Windows.IsValidIndex(WindowChat))
	{
		const FVector2D TopLeft = Windows[WindowChat].DefaultTopLeft + Windows[WindowChat].Offset;
		const bool bOverGrip = Virtual.X >= TopLeft.X && Virtual.X <= TopLeft.X + Windows[WindowChat].Size.X
			&& Virtual.Y >= TopLeft.Y - 4.f && Virtual.Y <= TopLeft.Y + 12.f;
		if (bOverGrip || (ChatResizeGrip && WidgetContains(ChatResizeGrip, ScreenPos)))
		{
			bResizingChat = true;
			FEventReply Reply = UWidgetBlueprintLibrary::Handled();
			if (ChatResizeGrip)
			{
				Reply = UWidgetBlueprintLibrary::CaptureMouse(Reply, ChatResizeGrip);
			}
			return Reply;
		}
	}

	if (!CanDragWindows())
	{
		// Do not swallow clicks meant for buttons sitting on top of chrome.
		return UWidgetBlueprintLibrary::Unhandled();
	}

	FEventReply Reply = UWidgetBlueprintLibrary::Handled();
	for (int32 i = Windows.Num() - 1; i >= 0; --i)
	{
		FHudWindow& Window = Windows[i];
		const FVector2D TopLeft = Window.DefaultTopLeft + Window.Offset;
		if (Virtual.X >= TopLeft.X && Virtual.X <= TopLeft.X + Window.Size.X
			&& Virtual.Y >= TopLeft.Y && Virtual.Y <= TopLeft.Y + Window.Size.Y)
		{
			DragWindowIndex = i;
			DragGrabOffset = Virtual - TopLeft;
			if (UWidget* Surface = Window.DragSurface.Get())
			{
				Reply = UWidgetBlueprintLibrary::CaptureMouse(Reply, Surface);
			}
			break;
		}
	}
	return Reply;
}

FEventReply UACEGameHUDWidget::HandleWindowMouseMove(FGeometry MyGeometry, const FPointerEvent& MouseEvent)
{
	if (DragKind != EACEDragKind::None)
	{
		UpdateDragVisual(MouseEvent.GetScreenSpacePosition());
		return UWidgetBlueprintLibrary::Handled();
	}
	if (bResizingPanel && Windows.IsValidIndex(WindowPanel))
	{
		const FVector2D Virtual = ScreenToVirtual(MouseEvent.GetScreenSpacePosition());
		const float PanelTop = Windows[WindowPanel].DefaultTopLeft.Y + Windows[WindowPanel].Offset.Y;
		PanelExtraHeight = FMath::Clamp(Virtual.Y - PanelTop - 422.f, -200.f, 400.f);
		Windows[WindowPanel].Size.Y = 430.f + PanelExtraHeight;
		return UWidgetBlueprintLibrary::Handled();
	}
	if (bResizingChat && Windows.IsValidIndex(WindowChat))
	{
		const FVector2D Virtual = ScreenToVirtual(MouseEvent.GetScreenSpacePosition());
		ChatExtraHeight = FMath::Clamp(500.f + Windows[WindowChat].Offset.Y - Virtual.Y, 0.f, 280.f);
		ApplyChatExtraHeight();
		return UWidgetBlueprintLibrary::Handled();
	}
	if (DragWindowIndex == INDEX_NONE || !Windows.IsValidIndex(DragWindowIndex))
	{
		return UWidgetBlueprintLibrary::Unhandled();
	}
	FHudWindow& Window = Windows[DragWindowIndex];
	const FVector2D Virtual = ScreenToVirtual(MouseEvent.GetScreenSpacePosition());
	FVector2D NewTopLeft = Virtual - DragGrabOffset;
	FVector2D VirtMin, VirtMax;
	GetVirtualScreenBounds(VirtMin, VirtMax);
	if (ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"))>.5f)
	{
		NewTopLeft.X = FMath::Clamp(NewTopLeft.X, VirtMin.X, FMath::Max(VirtMin.X,VirtMax.X-Window.Size.X));
		NewTopLeft.Y = FMath::Clamp(NewTopLeft.Y, VirtMin.Y, FMath::Max(VirtMin.Y,VirtMax.Y-Window.Size.Y));
	}
	Window.Offset = NewTopLeft - Window.DefaultTopLeft;
	return UWidgetBlueprintLibrary::Handled();
}

FEventReply UACEGameHUDWidget::HandleWindowMouseUp(FGeometry MyGeometry, const FPointerEvent& MouseEvent)
{
	FEventReply Reply = UWidgetBlueprintLibrary::Handled();
	if (DragKind != EACEDragKind::None)
	{
		FinishDrag(MouseEvent.GetScreenSpacePosition());
		return UWidgetBlueprintLibrary::ReleaseMouseCapture(Reply);
	}
	if (bResizingPanel)
	{
		bResizingPanel = false;
		SaveWindowLayout();
		return UWidgetBlueprintLibrary::ReleaseMouseCapture(Reply);
	}
	if (bResizingChat)
	{
		bResizingChat = false;
		SaveWindowLayout();
		return UWidgetBlueprintLibrary::ReleaseMouseCapture(Reply);
	}
	if (DragWindowIndex != INDEX_NONE)
	{
		DragWindowIndex = INDEX_NONE;
		SaveWindowLayout();
		Reply = UWidgetBlueprintLibrary::ReleaseMouseCapture(Reply);
	}
	return Reply;
}

void UACEGameHUDWidget::AppendChatLine(const FString& Line, const FLinearColor& Color)
{
	if (!ChatScroll || !WidgetTree)
	{
		return;
	}
	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
	Text->SetText(FText::FromString(Line));
	Text->SetColorAndOpacity(FSlateColor(Color));
	Text->SetAutoWrapText(true);
	FSlateFontInfo Font = Text->GetFont();
	Font.Size = 9;
	Text->SetFont(Font);
	ChatScroll->AddChild(Text);
	++ChatLineCount;

	while (ChatLineCount > MaxChatLines && ChatScroll->GetChildrenCount() > 0)
	{
		ChatScroll->RemoveChildAt(0);
		--ChatLineCount;
	}
	// Wrapped lines grow after layout — snap again next tick.
	ChatScroll->ScrollToEnd();
	if (UWorld* World = GetWorld())
	{
		TWeakObjectPtr<UScrollBox> WeakScroll = ChatScroll;
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakScroll]()
		{
			if (UScrollBox* Scroll = WeakScroll.Get())
			{
				Scroll->ForceLayoutPrepass();
				Scroll->SetScrollOffset(Scroll->GetScrollOffsetOfEnd());
			}
		}));
	}
}

void UACEGameHUDWidget::HandleChatMessage(const FString& Text, const FString& Sender, int32 ChatType, int32 FilterType)
{
	if (!PassesChatFilter(FilterType == INDEX_NONE ? ChatType : FilterType))
	{
		return;
	}
	const FLinearColor Color = ACERetailChat::Color(ChatType);

	// ServerMessage / combat GameEvents / WeenieError — no speaker name.
	if (Sender.IsEmpty() || ACEChatMessageType::IsGlobalChannel(ChatType)
		|| ChatType == ACEChatMessageType::Allegiance || ChatType == ACEChatMessageType::Fellowship
		|| ChatType == ACEChatMessageType::Social || ChatType == ACEChatMessageType::SocialSend
		|| ChatType == ACEChatMessageType::Channel || ChatType == ACEChatMessageType::ChannelSend
		|| ChatType == ACEChatMessageType::Abuse || ChatType == ACEChatMessageType::Help)
	{
		AppendChatLine(Text, Color);
		return;
	}

	// Local SendChatText already appends "You say, …" — ignore the server echo for self.
	if (Client)
	{
		FACEWorldObject Self;
		if (Client->GetWorldObject(Client->GetPlayerGuid(), Self) && !Self.Name.IsEmpty()
			&& Sender.Equals(Self.Name, ESearchCase::IgnoreCase)
			&& ChatType == ACEChatMessageType::Speech)
		{
			return;
		}
	}

	if (ChatType == ACEChatMessageType::Tell || ChatType == ACEChatMessageType::OutgoingTell)
	{
		AppendChatLine(FString::Printf(TEXT("%s tells you, \"%s\""), *Sender, *Text), Color);
		return;
	}

	AppendChatLine(FString::Printf(TEXT("%s says, \"%s\""), *Sender, *Text), Color);
}

void UACEGameHUDWidget::SendChatText()
{
	if (!ChatEntry)
	{
		return;
	}
	const FString Message = ChatEntry->GetText().ToString().TrimStartAndEnd();
	ChatEntry->SetText(FText::GetEmpty());
	if (Message.IsEmpty())
	{
		FSlateApplication::Get().SetAllUserFocusToGameViewport();
		return;
	}
	if (Client)
	{
		// Match Mag-nus OnChatCommand: leading / is rewritten to @ for CommandManager.
		FString Out = Message;
		if (Out.StartsWith(TEXT("/")))
		{
			Out = TEXT("@") + Out.Mid(1);
		}
		Client->SendChatMessage(Out);
		if (!Out.StartsWith(TEXT("@")))
		{
			AppendChatLine(FString::Printf(TEXT("You say, \"%s\""), *Out), ChatSpeechColor);
		}
	}
	FSlateApplication::Get().SetAllUserFocusToGameViewport();
}

void UACEGameHUDWidget::HandleChatCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter)
	{
		SendChatText();
	}
}

void UACEGameHUDWidget::OnSendClicked()
{
	SendChatText();
}

void UACEGameHUDWidget::OnChatTargetClicked()
{
	CycleChatFilter();
}

bool UACEGameHUDWidget::PassesChatFilter(int32 ChatType) const
{
	switch (ChatFilterMode)
	{
	case 1: // Speech / tells
		return ChatType == ACEChatMessageType::Speech
			|| ChatType == ACEChatMessageType::Tell
			|| ChatType == ACEChatMessageType::OutgoingTell
			|| ChatType == ACEChatMessageType::Broadcast;
	case 2: // Combat / magic
		return ChatType == ACEChatMessageType::Combat
			|| ChatType == ACEChatMessageType::CombatEnemy
			|| ChatType == ACEChatMessageType::CombatSelf
			|| ChatType == ACEChatMessageType::Magic
			|| ChatType == ACEChatMessageType::Spellcasting;
	case 3: // System
		return ChatType == ACEChatMessageType::System
			|| ChatType == ACEChatMessageType::Broadcast;
	default: // All
		return true;
	}
}

void UACEGameHUDWidget::CycleChatFilter()
{
	ChatFilterMode = (ChatFilterMode + 1) % 4;
	UpdateChatTargetButton();
	static const TCHAR* Names[] = { TEXT("All"), TEXT("Speech"), TEXT("Combat"), TEXT("System") };
	AppendChatLine(FString::Printf(TEXT("Chat filter: %s"), Names[ChatFilterMode]), ChatSystemColor);
}

void UACEGameHUDWidget::UpdateChatTargetButton()
{
	if (!ChatTargetButton)
	{
		return;
	}
	static const TCHAR* Names[] = { TEXT("All"), TEXT("Spch"), TEXT("Cmbt"), TEXT("Sys") };
	const FString Label = Names[FMath::Clamp(ChatFilterMode, 0, 3)];
	if (UTextBlock* Text = Cast<UTextBlock>(ChatTargetButton->GetContent()))
	{
		Text->SetText(FText::FromString(Label));
	}
	else if (ChatTargetButton->GetChildrenCount() > 0)
	{
		if (UTextBlock* Child = Cast<UTextBlock>(ChatTargetButton->GetChildAt(0)))
		{
			Child->SetText(FText::FromString(Label));
		}
	}
}

void UACEGameHUDWidget::FocusChatEntry()
{
	if (ChatEntry)
	{
		ChatEntry->SetKeyboardFocus();
	}
}

bool UACEGameHUDWidget::IsChatEntryFocused() const
{
	return ChatEntry && ChatEntry->HasKeyboardFocus();
}

void UACEGameHUDWidget::ApplyLetterboxLayout()
{
	if (!RootCanvas)
	{
		return;
	}

	const FVector2D ViewportSize = GetCachedGeometry().GetLocalSize();
	if (ViewportSize.X < 1.f || ViewportSize.Y < 1.f)
	{
		return;
	}

	// Scale = largest integer that fits 800×600 (retail letterbox). Do not stretch/reflow.
	const int32 ScaleI = FMath::Max(1, FMath::Min(
		FMath::FloorToInt(ViewportSize.X / static_cast<float>(ACEUI::ReferenceWidth)),
		FMath::FloorToInt(ViewportSize.Y / static_cast<float>(ACEUI::ReferenceHeight))));
	const float Scale = static_cast<float>(ScaleI);
	const float OffsetX = (ViewportSize.X - ACEUI::ReferenceWidth * Scale) * 0.5f;
	const float OffsetY = (ViewportSize.Y - ACEUI::ReferenceHeight * Scale) * 0.5f;
	LastScale = Scale;
	LastLetterboxOffset = FVector2D(OffsetX, OffsetY);
	LastViewportSize = ViewportSize;
	if (ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"))>.5f)
	{
		FVector2D Min,Max;GetVirtualScreenBounds(Min,Max);
		for(auto& Window:Windows)
		{
			const FVector2D At=Window.DefaultTopLeft+Window.Offset;
			Window.Offset=FVector2D(FMath::Clamp(At.X,Min.X,FMath::Max(Min.X,Max.X-Window.Size.X)),
				FMath::Clamp(At.Y,Min.Y,FMath::Max(Min.Y,Max.Y-Window.Size.Y)))-Window.DefaultTopLeft;
		}
	}

	auto PlaceRect = [&](UWidget* Widget, float VX, float VY, float VW, float VH)
	{
		if (!Widget)
		{
			return;
		}
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
		{
			CanvasSlot->SetAnchors(FAnchors(0.f, 0.f));
			CanvasSlot->SetAlignment(FVector2D(0.f, 0.f));
			CanvasSlot->SetAutoSize(false);
			CanvasSlot->SetPosition(FVector2D(OffsetX + VX * Scale, OffsetY + VY * Scale));
			CanvasSlot->SetSize(FVector2D(VW * Scale, VH * Scale));
		}
	};

	auto WindowOffset = [&](int32 WindowIndex) -> FVector2D
	{
		return Windows.IsValidIndex(WindowIndex) ? Windows[WindowIndex].Offset : FVector2D::ZeroVector;
	};

	for (const FHudPiece& Piece : Pieces)
	{
		if (!Piece.Widget.IsValid())
		{
			continue;
		}
		if (Piece.bPinScreenTopLeft)
		{
			// Screen-absolute top-left — cancel letterbox centering so burden/vitae stay at (0,0).
			if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Piece.Widget->Slot))
			{
				CanvasSlot->SetAnchors(FAnchors(0.f, 0.f));
				CanvasSlot->SetAlignment(FVector2D(0.f, 0.f));
				CanvasSlot->SetAutoSize(false);
				CanvasSlot->SetPosition(FVector2D(Piece.VirtualRect.X * Scale, Piece.VirtualRect.Y * Scale));
				CanvasSlot->SetSize(FVector2D(Piece.VirtualRect.Z * Scale, Piece.VirtualRect.W * Scale));
			}
			continue;
		}
		const FVector2D Off = WindowOffset(Piece.WindowIndex);
		float PieceY = Piece.VirtualRect.Y;
		float PieceH = Piece.VirtualRect.W;
		if (Piece.bPanelStretchHeight)
		{
			PieceH += PanelExtraHeight;
		}
		if (Piece.bPanelAnchorBottom)
		{
			PieceY += PanelExtraHeight;
		}
		if (Piece.bChatStretchHeight)
		{
			PieceH += ChatExtraHeight;
		}
		if (Piece.bChatShiftUp)
		{
			PieceY -= ChatExtraHeight;
		}
		PlaceRect(Piece.Widget.Get(), Piece.VirtualRect.X + Off.X, PieceY + Off.Y,
			Piece.VirtualRect.Z, PieceH);
	}

	// Vitals fills: 3-slice pieces sized to the current vital fraction (vitals window is movable).
	auto Frac = [](int32 Cur, int32 Max) { return Max > 0 ? FMath::Clamp(static_cast<float>(Cur) / Max, 0.f, 1.f) : 1.f; };
	const float Fracs[3] =
	{
		LastVitals.bValid ? Frac(LastVitals.Health, LastVitals.MaxHealth) : 1.f,
		LastVitals.bValid ? Frac(LastVitals.Stamina, LastVitals.MaxStamina) : 1.f,
		LastVitals.bValid ? Frac(LastVitals.Mana, LastVitals.MaxMana) : 1.f,
	};
	const TArray<TObjectPtr<UBorder>>* FillArrays[3] = { &HealthFillPieces, &StaminaFillPieces, &ManaFillPieces };
	const FVector2D VitalsOff = WindowOffset(WindowVitals);
	constexpr float VitalsOX = 160.f;
	constexpr float VitalsOY = 2.f;
	for (int32 i = 0; i < 3; ++i)
	{
		const FVitalGeom& G = GVitals[i];
		if (FillArrays[i]->Num() != 3)
		{
			continue;
		}
		const float InnerW = static_cast<float>(G.W - G.CapL - G.CapR);
		const float FillW = InnerW * Fracs[i];
		UBorder* L = (*FillArrays[i])[0].Get();
		UBorder* M = (*FillArrays[i])[1].Get();
		UBorder* R = (*FillArrays[i])[2].Get();
		const bool bEmpty = Fracs[i] <= KINDA_SMALL_NUMBER;
		for (UBorder* Piece : { L, M, R })
		{
			if (Piece)
			{
				Piece->SetVisibility(bEmpty ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
			}
		}
		if (!bEmpty)
		{
			const float X = VitalsOX + static_cast<float>(G.X - 10) + VitalsOff.X;
			const float Y = VitalsOY + static_cast<float>(G.Y) + VitalsOff.Y;
			PlaceRect(L, X, Y, G.CapL, G.H);
			PlaceRect(M, X + G.CapL, Y, FillW, G.H);
			PlaceRect(R, X + G.CapL + FillW, Y, G.CapR, G.H);
		}
	}

	// Selected target health bar under the name (retail red meter in the selected-object field).
	if (SelectedHealthFill && LastSelection.bValid && LastSelection.bShowHealth)
	{
		const FVector2D Off = WindowOffset(WindowToolbar);
		const float BarW = 116.f * FMath::Clamp(LastSelection.HealthFraction, 0.f, 1.f);
		PlaceRect(SelectedHealthFill, 590.f + Off.X, 554.f + Off.Y, BarW, 8.f);
	}
	const FVector2D RadarOff = WindowOffset(WindowRadar);
	for (int32 i = 0; i < RadarBlips.Num() && i < BlipVirtualRects.Num(); ++i)
	{
		if (BlipVirtualRects[i].Z > 0.f)
		{
			PlaceRect(RadarBlips[i], BlipVirtualRects[i].X + RadarOff.X, BlipVirtualRects[i].Y + RadarOff.Y,
				BlipVirtualRects[i].Z, BlipVirtualRects[i].W);
		}
	}
}
