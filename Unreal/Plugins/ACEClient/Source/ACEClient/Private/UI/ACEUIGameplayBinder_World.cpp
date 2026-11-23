#include "UI/ACERetailTextBlock.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACERetailMap.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUILayoutResolver.h"
#include "Dat/ACEDatTextLayout.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "Dat/ACEDatFileTypes.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "Styling/CoreStyle.h"

namespace
{
	static const FLinearColor WorldGold(0.95f, 0.82f, 0.35f, 1.f);
	static const FLinearColor WorldInk(0.16f, 0.12f, 0.06f, 1.f);
	static const FLinearColor WorldDim(0.38f, 0.30f, 0.18f, 1.f);
	constexpr int32 WorldTabHeight = 25;
	constexpr int32 WorldZ = 100000;
	/**
	 * The Dereth map art covers the full 255-landblock grid: 204 map units of
	 * 240m each (255 * 192m = 48960m), so a global metre position maps linearly
	 * onto the Map element rect.
	 */
	constexpr float DerethExtentMeters = 48960.f;
	/** Retail maintenance period (House.RentInterval), tripled for apartments. */
	constexpr int64 RentIntervalSeconds = 30 * 24 * 60 * 60;

	FVector2D GlobalMetersFrom(const FACEPosition& Pos)
	{
		const uint32 Cell = static_cast<uint32>(Pos.CellId);
		return FVector2D(
			((Cell >> 24) & 0xFF) * 192.f + Pos.Location.X,
			((Cell >> 16) & 0xFF) * 192.f + Pos.Location.Y);
	}

	bool IsOutdoorCell(const FACEPosition& Pos)
	{
		return Pos.IsValid() && (static_cast<uint32>(Pos.CellId) & 0xFFFF) < 0x100;
	}

	bool HouseMapLocation(const FACEHouseInfo& House, FIntPoint& Cell)
	{
		// gmHouseUI::GetHouseLocation resolves Position::get_outside_cell_id
		// before displaying coordinates. A landscape house can be in a building
		// cell; rejecting all interiors hides both its location and its map pin.
		const auto& Pos = House.Position;
		const uint32 Local = static_cast<uint32>(Pos.CellId) & 0xFFFF;
		if (!House.bOwned || House.HouseType == 4 || !Pos.IsValid()
			|| !((Local >= 1 && Local <= 64) || (Local >= 0x100 && Local <= 0xFFFD) || Local == 0xFFFF)
			|| !FMath::IsFinite(Pos.Location.X) || !FMath::IsFinite(Pos.Location.Y)) return false;
		FACEPosition Outside = Pos;
		if (FMath::Abs(Outside.Location.X) < 0.0002) Outside.Location.X = 0;
		if (FMath::Abs(Outside.Location.Y) < 0.0002) Outside.Location.Y = 0;
		const FVector2D Global = GlobalMetersFrom(Outside);
		if (Global.X < 0 || Global.Y < 0 || Global.X >= DerethExtentMeters || Global.Y >= DerethExtentMeters) return false;
		Cell = FIntPoint(FMath::FloorToInt(Global.X / 24.), FMath::FloorToInt(Global.Y / 24.));
		return true;
	}

	FString FormatUnixDate(int64 UnixSeconds)
	{
		if (UnixSeconds <= 0)
		{
			return TEXT("N/A");
		}
		const FDateTime Time = FDateTime::FromUnixTimestamp(UnixSeconds);
		return FText::AsDateTime(Time, EDateTimeStyle::Short, EDateTimeStyle::Medium).ToString();
	}
}

FString UACEUIGameplayBinder::FormatMapCoords(const FACEPosition& Pos, bool bIncludeInteriors)
{
	if (!Pos.IsValid() || (!bIncludeInteriors && !IsOutdoorCell(Pos)))
	{
		return FString();
	}
	// PositionExtensions.GetMapCoords: global metres / 240, centred on 102 map units.
	const FVector2D Global = GlobalMetersFrom(Pos);
	const float NS = Global.Y / 240.f - 102.f;
	const float EW = Global.X / 240.f - 102.f;
	return FString::Printf(TEXT("%.1f%s, %.1f%s"),
		FMath::Abs(NS) - 0.05f, NS >= 0.f ? TEXT("N") : TEXT("S"),
		FMath::Abs(EW) - 0.05f, EW >= 0.f ? TEXT("E") : TEXT("W"));
}

FString UACEUIGameplayBinder::FormatDerethDateTime(double GameTicks) const
{
	if (GameTicks <= 0.0)
	{
		return FString();
	}
	// Align hour with GetSky: Fmod(Ticks - ZeroTimeOfYear, DayLength).
	// Darktide = hour 0 at fraction 0 (not the tick-0 Morntide-and-Half calendar epoch).
	double DayLength = 7620.0;
	double ZeroTimeOfYear = 3600.0;
	if (Client)
	{
		if (UWorld* World = Client->GetWorld())
		{
			if (UGameInstance* GI = World->GetGameInstance())
			{
				if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
				{
					if (const FACEDatRegionSky* Sky = Dat->GetRegionSkyInfo())
					{
						if (Sky->DayLengthSeconds > 1.f)
						{
							DayLength = static_cast<double>(Sky->DayLengthSeconds);
						}
						ZeroTimeOfYear = Sky->ZeroTimeOfYear;
					}
				}
			}
		}
	}
	static const TCHAR* MonthNames[] = {
		TEXT("Morningthaw"), TEXT("Solclaim"), TEXT("Seedsow"), TEXT("Leafdawning"),
		TEXT("Verdantine"), TEXT("Thistledown"), TEXT("Harvestgain"), TEXT("Leafcull"),
		TEXT("Frostfell"), TEXT("Snowreap"), TEXT("Coldeve"), TEXT("Wintersebb"),
	};
	static const TCHAR* HourNames[] = {
		TEXT("Darktide"), TEXT("Darktide-and-Half"), TEXT("Foredawn"), TEXT("Foredawn-and-Half"),
		TEXT("Dawnsong"), TEXT("Dawnsong-and-Half"), TEXT("Morntide"), TEXT("Morntide-and-Half"),
		TEXT("Midsong"), TEXT("Midsong-and-Half"), TEXT("Warmtide"), TEXT("Warmtide-and-Half"),
		TEXT("Evensong"), TEXT("Evensong-and-Half"), TEXT("Gloaming"), TEXT("Gloaming-and-Half"),
	};
	double Phase = FMath::Fmod(GameTicks - ZeroTimeOfYear, DayLength);
	if (Phase < 0.0)
	{
		Phase += DayLength;
	}
	const double HourTicks = DayLength / 16.0;
	const int32 HourIndex = FMath::Clamp(static_cast<int32>(Phase / HourTicks), 0, 15);
	// Date still advances from tick 0 = Morningthaw 1, 10 P.Y. (DerethDateTime epoch).
	const int64 HoursElapsed = static_cast<int64>(GameTicks / HourTicks);
	const int64 AbsHour = 7 + HoursElapsed;
	const int64 DayOffset = AbsHour / 16;
	const int32 Day = static_cast<int32>(DayOffset % 30) + 1;
	const int64 MonthsElapsed = DayOffset / 30;
	const int32 Month = static_cast<int32>(MonthsElapsed % 12);
	const int32 Year = 10 + static_cast<int32>(MonthsElapsed / 12);
	return FString::Printf(TEXT("%s %d, %d P.Y.  %s"),
		MonthNames[Month], Day, Year, HourNames[HourIndex]);
}

void UACEUIGameplayBinder::SyncWorldPanelTab(const FString& PageName)
{
	if (!Manager)
	{
		return;
	}
	ActiveWorldTab = PageName;
	HouseScrollOffset = 0;
	static const TCHAR* Pages[] = { TEXT("MapPage"), TEXT("HousePage") };
	for (const TCHAR* Page : Pages)
	{
		if (TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(TEXT("WorldPanel_Field"), Page))
		{
			El->bVisible = (PageName == Page);
		}
	}
	ApplyPanelTabChrome(TEXT("MapTab"), PageName == TEXT("MapPage"));
	ApplyPanelTabChrome(TEXT("HouseTab"), PageName == TEXT("HousePage"));
	if (PageName == TEXT("HousePage") && Client)
	{
		// Housing state is push-only after login; ask again whenever the tab opens.
		Client->SendHouseQuery();
	}
	RefreshWorldOverlays();
}

void UACEUIGameplayBinder::HideWorldOverlays()
{
	for (UTextBlock* T : WorldTabLabels) { if (T) { T->SetVisibility(ESlateVisibility::Collapsed); } }
	for (UTextBlock* T : HouseTextRows) { if (T) { T->SetVisibility(ESlateVisibility::Collapsed); } }
	if (MapCoordinateLabel) { MapCoordinateLabel->SetVisibility(ESlateVisibility::Collapsed); }
	if (MapDateTimeLabel) { MapDateTimeLabel->SetVisibility(ESlateVisibility::Collapsed); }
}

void UACEUIGameplayBinder::RefreshWorldOverlays()
{
	if (ActivePanelPage != TEXT("WorldPanel_Field") || !Manager || !Canvas || !Canvas->WidgetTree
		|| !Client)
	{
		HideWorldOverlays();
		return;
	}
	TSharedPtr<FACEUIElement> Panel = Manager->FindElementByName(TEXT("WorldPanel_Field"));
	if (!Panel.IsValid() || !Panel->bVisible)
	{
		HideWorldOverlays();
		return;
	}
	if (ActiveWorldTab.IsEmpty())
	{
		ActiveWorldTab = TEXT("MapPage");
	}

	const int32 PanelW = FMath::Max(160, Panel->Width);
	if (TSharedPtr<FACEUIElement> Close = Manager->FindElementUnder(TEXT("WorldPanel_Field"),
		TEXT("CloseWorldPanelButton")))
	{
		Close->X = FMath::Max(0, PanelW - Close->Width);
	}

	// Tab captions — the DAT tab art carries no text layer.
	static const TPair<const TCHAR*, const TCHAR*> Tabs[] = {
		{ TEXT("MapTab"), TEXT("Map") },
		{ TEXT("HouseTab"), TEXT("Housing") },
	};
	static const TCHAR* TabPages[] = { TEXT("MapPage"), TEXT("HousePage") };
	while (WorldTabLabels.Num() < UE_ARRAY_COUNT(Tabs))
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetJustification(ETextJustify::Center);
		WorldTabLabels.Add(L);
	}
	for (int32 i = 0; i < UE_ARRAY_COUNT(Tabs); ++i)
	{
		if (UTextBlock* Label = WorldTabLabels[i])
		{
			PlaceTextOnElement(Label, Tabs[i].Key, Tabs[i].Value, 8,
				ActiveWorldTab == TabPages[i] ? WorldGold : FLinearColor(0.92f, 0.92f, 0.88f, 1.f),
				WorldZ, true);
		}
	}

	TSharedPtr<FACEUIElement> Page = Manager->FindElementUnder(TEXT("WorldPanel_Field"), ActiveWorldTab);
	if (!Page.IsValid())
	{
		HideWorldOverlays();
		return;
	}

	if (ActiveWorldTab == TEXT("MapPage"))
	{
		for (UTextBlock* T : HouseTextRows) { if (T) { T->SetVisibility(ESlateVisibility::Collapsed); } }
		RefreshWorldMapPage(Page);
		return;
	}
	if (MapCoordinateLabel) { MapCoordinateLabel->SetVisibility(ESlateVisibility::Collapsed); }
	if (MapDateTimeLabel) { MapDateTimeLabel->SetVisibility(ESlateVisibility::Collapsed); }
	RefreshWorldHousePage(Page);
}

void UACEUIGameplayBinder::RefreshWorldMapPage(const TSharedPtr<FACEUIElement>& Page)
{
	const auto Map = Manager->FindElementUnder(TEXT("MapPage"), TEXT("Map"));
	if (!Map) return;
	// Retail keeps the map at its native size and centers its complete area.
	// The parent-size reflow also anchors the paper and labels.
	const FACEPosition Self = Client->GetPlayerPosition();
	// Player pin — retail hides it when the character is off the surface map.
	if (TSharedPtr<FACEUIElement> Pin = Manager->FindElementUnder(TEXT("MapPage"),
		TEXT("Map_PlayerPosition_Icon")))
	{
		if (IsOutdoorCell(Self))
		{
			const FVector2D G = GlobalMetersFrom(Self);
			Pin->bVisible = true;
			Pin->X = FMath::RoundToInt((6.f + G.X / DerethExtentMeters * 242.f) * Map->Width / 257.f) - Pin->Width / 2;
			Pin->Y = FMath::RoundToInt((8.f + (1.f - G.Y / DerethExtentMeters) * 251.f) * Map->Height / 267.f) - Pin->Height / 2;
		}
		else
		{
			Pin->bVisible = false;
		}
	}
	const FACEHouseInfo HouseInfo = Client->GetHouseInfo();
	if (TSharedPtr<FACEUIElement> Pin = Manager->FindElementUnder(TEXT("MapPage"),
		TEXT("Map_HousePosition_Icon")))
	{
		FIntPoint HouseCell;
		if (HouseMapLocation(HouseInfo, HouseCell))
		{
			const FVector2D G((HouseCell.X + .5) * 24., (HouseCell.Y + .5) * 24.);
			Pin->bVisible = true;
			Pin->X = FMath::RoundToInt((6.f + G.X / DerethExtentMeters * 242.f) * Map->Width / 257.f) - Pin->Width / 2;
			Pin->Y = FMath::RoundToInt((8.f + (1.f - G.Y / DerethExtentMeters) * 251.f) * Map->Height / 267.f) - Pin->Height / 2;
		}
		else
		{
			Pin->bVisible = false;
		}
	}

	if (!MapDateTimeLabel)
	{
		MapDateTimeLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		MapDateTimeLabel->SetJustification(ETextJustify::Center);
	}
	if (!MapCoordinateLabel)
	{
		MapCoordinateLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		MapCoordinateLabel->SetJustification(ETextJustify::Center);
	}
	const FString DateLine = FormatDerethDateTime(Client->GetGameTimeTicks());
	PlaceTextOnElement(MapDateTimeLabel,
		Manager->FindElementUnder(TEXT("MapPage"), TEXT("Map_DateTimeLabel")),
		DateLine.IsEmpty() ? TEXT("Dereth") : DateLine, 8, WorldInk, WorldZ + 2, true);
	const FString Coords = FormatMapCoords(Self);
	PlaceTextOnElement(MapCoordinateLabel,
		Manager->FindElementUnder(TEXT("MapPage"), TEXT("Map_CoordinateLabel")),
		Coords.IsEmpty() ? TEXT("Location unknown") : Coords, 9,
		Coords.IsEmpty() ? WorldDim : WorldInk, WorldZ + 2, true);
}

void UACEUIGameplayBinder::RefreshWorldHousePage(const TSharedPtr<FACEUIElement>& Page)
{
	const auto List = Manager->FindElementUnder(TEXT("HousePage"), TEXT("HouseTextListBox"));
	if (!List || !Canvas->GetResourceResolver()) return;
	// The retail list's bottom edge is fixed to its original 600px parent.
	// Fit the viewport, but keep the paragraph template's font, alignment and gaps.
	List->Width = FMath::Max(1, Page->Width - List->X - 5);
	List->Height = FMath::Max(1, Page->Height - List->Y - 5);
	const FACEHouseInfo Info = Client->GetHouseInfo();
	TArray<TPair<FString,FLinearColor>> Paragraphs;
	auto Add = [&](FString Text, FLinearColor Color = FLinearColor::White)
	{ Paragraphs.Emplace(MoveTemp(Text), Color); };
	auto Payments = [](const TArray<FACEHousePayment>& Costs, bool bRent)
	{
		TArray<FString> Parts;
		for (const auto& Cost : Costs)
		{
			const FString Name = Cost.Required == 1 ? Cost.Name : Cost.PluralName.IsEmpty() ? Cost.Name + TEXT("s") : Cost.PluralName;
			Parts.Add(bRent ? FString::Printf(TEXT("%d/%d %s"), Cost.Paid, Cost.Required, *Name)
				: FString::Printf(TEXT("%d %s"), Cost.Required, *Name));
		}
		return FString::Join(Parts, TEXT(", "));
	};
	// gmHouseUI::DisplayHouseData: each call adds a centered, auto-height
	// HouseTextEntryTemplate (white/green/yellow, with an 8px paragraph gap).
	if (!Info.bQueried) Add(TEXT("Querying dwelling status..."));
	else if (!Info.bOwned) Add(TEXT("You do not currently own a house."));
	else
	{
		Add(TEXT("The purchase price for this dwelling is:\n") + Payments(Info.Buy, false));
		Add(TEXT("Rent:\n") + Payments(Info.Rent, true));
		Add(TEXT("Bought: ") + FormatUnixDate(Info.BuyTime));
		const int64 Interval = Info.HouseType == 4 ? RentIntervalSeconds * 3 : RentIntervalSeconds;
		const bool Paid = Info.bMaintenanceFree || !Info.Rent.ContainsByPredicate(
			[](const FACEHousePayment& Cost) { return Cost.Paid < Cost.Required; });
		Add(TEXT("This maintenance period ends: ") + FormatUnixDate(Info.RentTime + Interval));
		Add(TEXT("Maintenance is next due: ") + FormatUnixDate(Info.RentTime + Interval * (Paid ? 2 : 1)));
		FIntPoint HouseCell;
		if (HouseMapLocation(Info, HouseCell))
		{
			// Retail displays the resolved outdoor cell, not the player's exact
			// floating-point map coordinates (which use a different rounding rule).
			Add(FString::Printf(TEXT("Location: %.1f%s, %.1f%s"),
				FMath::Abs((HouseCell.Y - 1024) * .1 + .5), HouseCell.Y < 1024 ? TEXT("S") : TEXT("N"),
				FMath::Abs((HouseCell.X - 1024) * .1 + .5), HouseCell.X < 1024 ? TEXT("W") : TEXT("E")));
		}
		Add(Paid ? TEXT("The maintenance has already been paid for this period. You may not prepay next period's maintenance.")
			: FString::Printf(TEXT("Warning!  You have not paid your maintenance costs for the last %d day maintenance period.  Please pay these costs by this deadline or you will lose your house, and all your items within it."), Info.HouseType == 4 ? 90 : 30),
			Paid ? FLinearColor::Green : FLinearColor::Yellow);
	}
	if (Info.bQueried)
	{
		const int64 Purchased = Client->GetPlayerVitalsView().StatQualityInts.FindRef(199); // HousePurchaseTimestamp
		const int64 NextPurchase = Purchased + RentIntervalSeconds;
		if (NextPurchase <= FDateTime::UtcNow().ToUnixTimestamp())
			Add(Info.bOwned ? TEXT("You may buy another house immediately after you abandon this one.")
				: TEXT("You may buy another house immediately."));
		else Add(TEXT("You may buy another landscape house at ") + FormatUnixDate(NextPurchase)
			+ TEXT(". This restriction does not apply to apartments."));
	}

	while (HouseTextRows.Num() < Paragraphs.Num())
	{
		HouseTextRows.Add(Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>());
		HouseTextElements.Add(UACEUILayoutResolver::LoadTemplate(0x21000025, 0x100001E7));
	}
	int32 Height = 0;
	for (int32 I = 0; I < Paragraphs.Num(); ++I)
	{
		const auto Row = HouseTextElements[I];
		if (!Row) continue;
		Row->Parent = List;
		Row->X = 0; Row->Y = Height;
		Row->Width = FMath::Max(1, FMath::Min(280, List->Width));
		FACEDatFont Font;
		Canvas->GetResourceResolver()->ResolveFont(Row->FontId, Font);
		Row->Height = FMath::Max(1, ACEDatText::Layout(Font, Paragraphs[I].Key,
			Row->Width - Row->TextMargins.Left - Row->TextMargins.Right, false).Num()) * FMath::Max(1, int32(Font.MaxCharHeight))
			+ Row->TextMargins.Top + Row->TextMargins.Bottom;
		Height += Row->Height;
	}
	HouseMaxScrollOffset = FMath::Max(0, Height - List->Height);
	HouseScrollOffset = FMath::Clamp(HouseScrollOffset, 0, HouseMaxScrollOffset);
	for (int32 I = 0; I < HouseTextRows.Num(); ++I)
	{
		auto* Text = HouseTextRows[I].Get();
		const auto Row = HouseTextElements[I];
		if (!Paragraphs.IsValidIndex(I) || !Row)
		{ Text->SetVisibility(ESlateVisibility::Collapsed); continue; }
		Row->Y -= HouseScrollOffset;
		Row->bVisible = Row->Y + Row->Height > 0 && Row->Y < List->Height;
		PlaceTextOnElement(Text, Row, Paragraphs[I].Key, 10, Paragraphs[I].Value, WorldZ + 2);
		// The row template supplies multiple indexed colors; SetTextWithFont in
		// retail selects the color per paragraph, not the template's first color.
		Text->SetColorAndOpacity(Paragraphs[I].Value);
	}
}

bool UACEUIGameplayBinder::IsPointerOverHouseList(FVector2D CanvasLocalPos) const
{
	return ActivePanelPage == TEXT("WorldPanel_Field") && ActiveWorldTab == TEXT("HousePage") && Manager && Canvas
		&& Canvas->IsElementExposedAt(Manager->FindElementUnder(TEXT("HousePage"), TEXT("HouseTextListBox")), CanvasLocalPos);
}

bool UACEUIGameplayBinder::ScrollHouseList(float WheelDelta)
{
	if (ActivePanelPage != TEXT("WorldPanel_Field") || ActiveWorldTab != TEXT("HousePage")) return false;
	HouseScrollOffset = FMath::Clamp(HouseScrollOffset + (WheelDelta > 0.f ? -48 : 48), 0, HouseMaxScrollOffset);
	RefreshWorldOverlays();
	return true;
}
bool UACEUIGameplayBinder::HandleWorldNamedClick(const FString& Name)
{
	if (Name == TEXT("MapTab") && ActivePanelPage == TEXT("WorldPanel_Field"))
	{
		SyncWorldPanelTab(TEXT("MapPage"));
		return true;
	}
	if (Name == TEXT("HouseTab") && ActivePanelPage == TEXT("WorldPanel_Field"))
	{
		SyncWorldPanelTab(TEXT("HousePage"));
		return true;
	}
	if (ActivePanelPage != TEXT("WorldPanel_Field"))
	{
		return false;
	}
	if (Name == TEXT("CloseWorldPanelButton"))
	{
		HidePanel();
		return true;
	}
	return false;
}

bool UACEUIGameplayBinder::GetMapTooltipAt(FVector2D Absolute, FString& OutText) const
{
    if (!Canvas || !Manager || ActivePanelPage != TEXT("WorldPanel_Field") || ActiveWorldTab != TEXT("MapPage")) return false;
    const auto Map = Manager->FindElementUnder(TEXT("MapPage"), TEXT("Map"));
    const FVector2D Local = Canvas->GetCachedGeometry().AbsoluteToLocal(Absolute);
    if (!Map || !Canvas->IsElementExposedAt(Map, Local) || Map->Width <= 0 || Map->Height <= 0) return false;
    const FVector2D P = Local / Canvas->GetLastScale2D() - FVector2D(Map->GetScreenOrigin());
    const FVector2D Native(P.X * 257.f / Map->Width, P.Y * 267.f / Map->Height);
    for (const auto& Town : ACERetailMap::Locations)
        if (Native.X >= Town.X && Native.X < Town.X + Town.Width && Native.Y >= Town.Y && Native.Y < Town.Y + Town.Height)
        { OutText = Town.Name; return true; }
    return false;
}
