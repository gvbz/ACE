#include "Mods/ACELedger.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginStyle.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "Misc/SecureHash.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Styling/CoreStyle.h"

bool UACEPluginSubsystem::UpdateLedgerCharacter()
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!C)return false;
    const auto State=C->GetSessionState();
    if(State!=EACESessionState::InWorld&&State!=EACESessionState::EnteringWorld)
    {LedgerCharacter.Empty();return false;}
    const int32 Player=C->GetPlayerGuid();if(!Player)return false;
    const FString Key=FMD5::HashAnsiString(*C->GetServerName())+FString::Printf(TEXT(".%08X"),uint32(Player));
    if(Key==LedgerCharacter)return true;
    LedgerCharacter=Key;LedgerSupplies.Reset();++SupplyRevision;SupplyInventoryRevision=MAX_uint64;
    SupplyNamesVisible=true;
    const bool HasNamePreference=Settings&&Settings->TryGetBoolField(TEXT("supplies.names.")+Key,SupplyNamesVisible);
    const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;
    if(Settings&&Settings->TryGetArrayField(TEXT("supplies.")+Key,Rows))for(const auto& Row:*Rows)
    {
        const auto O=Row->AsObject();if(!O)continue;FACESupplyEntry Entry;
        O->TryGetNumberField(TEXT("class"),Entry.ClassId);O->TryGetNumberField(TEXT("icon"),Entry.Icon);O->TryGetStringField(TEXT("name"),Entry.Name);
        O->TryGetNumberField(TEXT("overlay"),Entry.Overlay);O->TryGetNumberField(TEXT("underlay"),Entry.Underlay);
        O->TryGetNumberField(TEXT("effects"),Entry.Effects);O->TryGetNumberField(TEXT("item_type"),Entry.ItemType);
        // Migrate the old per-row preference to one choice for the entire ledger.
        bool ShowName=true;if(!HasNamePreference&&O->TryGetBoolField(TEXT("show_name"),ShowName)&&!ShowName)SupplyNamesVisible=false;
        if(!Entry.Name.IsEmpty())LedgerSupplies.Add(Entry);
    }
    ResetHuntLedger();return true;
}
void UACEPluginSubsystem::ResetHuntLedger()
{
    HuntLedger.Reset(FPlatformTime::Seconds());
    if(auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>())HuntLedger.Observe(C->GetPlayerVitalsView(),FPlatformTime::Seconds());
}
void UACEPluginSubsystem::ObserveLedgerVitals(const FACEPlayerVitals& V)
{
    if(UpdateLedgerCharacter())HuntLedger.Observe(V,FPlatformTime::Seconds());
}
void UACEPluginSubsystem::UpdateLedgers()
{
    if(!UpdateLedgerCharacter())return;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();const auto S=C->GetSession();if(!S)return;
    if(SupplyInventoryRevision==S->GetInventoryDataRevision())return;
    SupplyInventoryRevision=S->GetInventoryDataRevision();
    ACELedger::Count(LedgerSupplies,C->GetPlayerGuid(),S->GetWorldObjects());
}
void UACEPluginSubsystem::SaveSupplies(bool Rebuild)
{
    if(LedgerCharacter.IsEmpty())return;
    if(!Settings)Settings=MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Rows;
    for(const auto& E:LedgerSupplies)
    {
        auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("class"),E.ClassId);O->SetNumberField(TEXT("icon"),E.Icon);O->SetStringField(TEXT("name"),E.Name);
        O->SetNumberField(TEXT("overlay"),E.Overlay);O->SetNumberField(TEXT("underlay"),E.Underlay);
        O->SetNumberField(TEXT("effects"),E.Effects);O->SetNumberField(TEXT("item_type"),E.ItemType);
        Rows.Add(MakeShared<FJsonValueObject>(O));
    }
    Settings->SetArrayField(TEXT("supplies.")+LedgerCharacter,Rows);
    Settings->SetBoolField(TEXT("supplies.names.")+LedgerCharacter,SupplyNamesVisible);SaveSettings();
    if(!Rebuild)return;
    ++SupplyRevision;SupplyInventoryRevision=MAX_uint64;
    UpdateLedgers();
}
void UACEPluginSubsystem::EditSupplySelection(bool Add)
{
    if(!UpdateLedgerCharacter())return;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Item;
    if(!C->GetWorldObject(C->GetSelectedObject().Guid,Item)||(!C->GetSession()||!ACELedger::IsOwned(Item,C->GetPlayerGuid(),C->GetSession()->GetWorldObjects())))
    {SupplyNotice=TEXT("Select an item in your inventory first.");return;}
    const int32 Index=LedgerSupplies.IndexOfByPredicate([&](const auto& E){return E.Matches(Item);});
    if(Add&&Index==INDEX_NONE){FACESupplyEntry E;E.ClassId=Item.WeenieClassId;E.CopyAppearance(Item);E.Name=Item.Name;LedgerSupplies.Add(E);}
    else if(!Add&&Index!=INDEX_NONE)LedgerSupplies.RemoveAt(Index);
    else return;
    SupplyNotice.Empty();SaveSupplies();
}
void UACEPluginSubsystem::RemoveSupply(int32 Index)
{if(LedgerSupplies.IsValidIndex(Index)){LedgerSupplies.RemoveAt(Index);SaveSupplies();}}
void UACEPluginSubsystem::ToggleSupplyName(int32 Index)
{if(LedgerSupplies.IsValidIndex(Index)){SupplyNamesVisible=!SupplyNamesVisible;SaveSupplies(false);}}
bool UACEPluginSubsystem::LedgerOption(const FString& Id,const FString& Key,bool Default) const
{bool V;return Settings&&Settings->TryGetBoolField(Id+TEXT(".")+Key,V)?V:Default;}
void UACEPluginSubsystem::SetLedgerOption(const FString& Id,const FString& Key,bool Value)
{if(!Settings)Settings=MakeShared<FJsonObject>();Settings->SetBoolField(Id+TEXT(".")+Key,Value);SaveSettings();}
FVector2D UACEPluginSubsystem::LedgerSize(const FString& Id) const
{
    const bool Locked=IsPluginWindowChromeHidden(Id);
    return Id==TEXT("supplyledger")?FVector2D(200,14+FMath::Clamp(LedgerSupplies.Num(),1,15)*26+LedgerChromeHeight(Id,Locked))
        :FVector2D(290,190+LedgerChromeHeight(Id,Locked));
}
double UACEPluginSubsystem::LedgerChromeHeight(const FString& Id,bool Hidden) const
{
    if(Id!=TEXT("supplyledger"))return Hidden?0:ACELedger::HuntEditorHeight;
    if(Hidden&&!LedgerSupplies.IsEmpty())return 0;
    // Empty pinned ledgers retain their add control, but not the title bar.
    return ACELedger::SupplyEditorHeight-(Hidden?ACELedger::HeaderHeight:0)+(SupplyNotice.IsEmpty()?0:28);
}

namespace
{
    class SACELedgerPanel : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SACELedgerPanel){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(FString,Id) SLATE_END_ARGS()
        void Construct(const FArguments& A)
        {Host=A._Host;Id=A._Id;Style=FACEPluginStyle::Use(A._Host,Id);ChildSlot[SAssignNew(Body,SVerticalBox)];Rebuild();}
        virtual void Tick(const FGeometry& G,double T,float D) override
        {SCompoundWidget::Tick(G,T,D);if(Host.IsValid()&&(Revision!=Host->GetSupplyRevision()||Locked!=Host->IsPluginWindowChromeHidden(Id)))Rebuild();UpdateHuntText();}
    private:
        TWeakObjectPtr<UACEPluginSubsystem> Host;FString Id;
        TSharedPtr<FACEPluginStyle> Style;TSharedPtr<SVerticalBox> Body;
        uint32 Revision=0;bool Locked=false;
        FText HuntText[4][2], HuntEstimate, HuntElapsed;
        int64 HuntSecond=-1,HuntXP=-1,HuntLum=-1,CharacterXP=-1;
        int64 AvailableXP=-1,AvailableLum=-1,MaximumLum=-1;
        double HuntStarted=-1;int32 HuntLevel=-1;bool HuntValid=false,LumKnown=false;
        FText EstimateHuntLevel() const
        {
            auto* GI=Host.IsValid()?Host->GetGameInstance():nullptr;
            auto* C=GI?GI->GetSubsystem<UACEClientSubsystem>():nullptr;
            auto* Dat=GI?GI->GetSubsystem<UACEDatSubsystem>():nullptr;
            if(!C||!Dat)return FText::FromString(TEXT("--"));
            const auto& V=C->GetPlayerVitalsView();int64 Needed=0,Threshold=0;
            if(!V.bValid||V.Level<=0||!V.QualityInt64s.Contains(1)||!Dat->TryGetXpToNextLevel(V.TotalExperience,V.Level,Needed))
                return FText::FromString(TEXT("--"));
            if(Needed==0)
            {
                Dat->TryGetXpToNextLevel(0,V.Level,Threshold);
                return FText::FromString(Threshold==0?TEXT("Max level"):TEXT("Leveling..."));
            }
            const double Seconds=Host->GetHuntLedger().SecondsToLevel(Needed,FPlatformTime::Seconds());
            if(Seconds<0)return FText::FromString(TEXT("Waiting for XP"));
            if(Seconds<60)return FText::FromString(TEXT("<1m"));
            if(Seconds>=86400)return FText::FromString(FString::Printf(TEXT("%.0fd %02dh"),FMath::FloorToDouble(Seconds/86400),int32(FMath::Fmod(Seconds/3600.,24.))));
            const int32 Minutes=FMath::CeilToInt(Seconds/60.);
            return FText::FromString(Minutes>=60?FString::Printf(TEXT("%dh %02dm"),Minutes/60,Minutes%60):FString::Printf(TEXT("%dm"),Minutes));
        }
        void UpdateHuntText()
        {
            if(Id!=TEXT("huntledger")||!Host.IsValid())return;
            auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            if(!C)return;
            const auto& V=C->GetPlayerVitalsView();const auto& L=Host->GetHuntLedger();
            const double Now=FPlatformTime::Seconds();const int64 Second=FMath::FloorToInt64(Now);
            const bool Valid=V.bValid&&V.QualityInt64s.Contains(1);
            const bool HasLum=V.bValid&&(V.bHasLuminanceBalance||V.QualityInt64s.Contains(6));
            if(HuntSecond==Second&&HuntXP==L.XP&&HuntLum==L.Lum&&HuntStarted==L.Started
                &&CharacterXP==V.TotalExperience&&HuntLevel==V.Level&&HuntValid==Valid
                &&AvailableXP==V.AvailableExperience&&AvailableLum==V.AvailableLuminance&&MaximumLum==V.MaximumLuminance&&LumKnown==HasLum)return;
            HuntSecond=Second;HuntXP=L.XP;HuntLum=L.Lum;HuntStarted=L.Started;
            CharacterXP=V.TotalExperience;HuntLevel=V.Level;HuntValid=Valid;
            AvailableXP=V.AvailableExperience;AvailableLum=V.AvailableLuminance;MaximumLum=V.MaximumLuminance;LumKnown=HasLum;
            const auto Recent=L.LastFiveMinutes(Now);
            const FText Unknown=FText::FromString(TEXT("--"));
            for(int32 Lum=0;Lum<2;++Lum)
            {
                const int64 Total=Lum?L.Lum:L.XP;
                const bool Known=Lum?L.HasLum:L.HasXP;
                HuntText[0][Lum]=Known?FText::AsNumber(Lum?Recent.Lum:Recent.XP):Unknown;
                HuntText[1][Lum]=Known?FText::AsNumber(L.PerHour(Total,Now)):Unknown;
                HuntText[2][Lum]=Known?FText::AsNumber(Total):Unknown;
            }
            HuntText[3][0]=V.bValid?FText::AsNumber(AvailableXP):Unknown;
            HuntText[3][1]=HasLum?FText::AsNumber(AvailableLum):Unknown;
            if(HasLum&&MaximumLum>0&&AvailableLum>=MaximumLum)
                HuntText[3][1]=FText::Format(FText::FromString(TEXT("{0} (cap)")),HuntText[3][1]);
            HuntEstimate=EstimateHuntLevel();
            const int64 Elapsed=FMath::FloorToInt64(L.Elapsed(Now));
            HuntElapsed=L.Started>0?FText::FromString(FString::Printf(TEXT("%02lld:%02lld:%02lld"),
                Elapsed/3600,(Elapsed/60)%60,Elapsed%60)):Unknown;
        }
        TSharedRef<SWidget> Label(const FString& Text)
        {return SNew(SACEPluginText).Text(FText::FromString(Text)).Font(FCoreStyle::GetDefaultFontStyle("Regular",12));}
        TSharedRef<SWidget> Button(const FString& Text,const FString& Tip,TFunction<void()> Action)
        {return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).Tag(Text==TEXT("Reset run")?TEXT("LedgerReset"):TEXT("LedgerEdit")).IsFocusable(false).ButtonStyle(&Style->Button).ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Tip))).OnClicked_Lambda([Action]{Action();return FReply::Handled();})[Label(Text)];}
        void Rebuild()
        {
            if(!Host.IsValid())return;FACEPluginStyle::Use(Host.Get(),Id);Revision=Host->GetSupplyRevision();Locked=Host->IsPluginWindowChromeHidden(Id);Body->ClearChildren();UpdateHuntText();
            if(Id==TEXT("supplyledger"))
            {
                auto Rows=SNew(SVerticalBox).Tag(TEXT("SupplyRows"));const auto& LedgerSupplies=Host->GetSupplies();
                if(LedgerSupplies.IsEmpty())Rows->AddSlot().AutoHeight()[SNew(SACEPluginText)
                    .Text(FText::FromString(TEXT("Select item\nthen +"))).Font(FCoreStyle::GetDefaultFontStyle("Regular",10))];
                for(int32 I=0;I<LedgerSupplies.Num();++I)
                {
                    auto Row=SNew(SHorizontalBox);
                    const auto& Supply=LedgerSupplies[I];
                    auto Toggle=[H=Host,I]{if(H.IsValid())H->ToggleSupplyName(I);return FReply::Handled();};
                    Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SACEPluginButton).ContentPadding(FMargin(0)).Tag(FName(*FString::Printf(TEXT("SupplyIcon.%d"),I)))
                        .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder")).IsFocusable(false)
                        .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Supply.Name+TEXT("\nClick to show or hide all item names."))))
                        .OnClicked_Lambda(Toggle)[SNew(SBox).WidthOverride(22).HeightOverride(22)
                        [SNew(SScaleBox).Stretch(EStretch::ScaleToFit)[SNew(SOverlay)
                        +SOverlay::Slot()[SNew(SImage).Image(Style->ItemImage(Supply,false))]
                        +SOverlay::Slot()[SNew(SImage).Image(Style->ItemImage(Supply,true))]]]]];
                    Row->AddSlot().FillWidth(1).VAlign(VAlign_Center).Padding(4,0)[SNew(SACEPluginButton).ContentPadding(FMargin(0)).Tag(FName(*FString::Printf(TEXT("SupplyName.%d"),I)))
                        .ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder")).IsFocusable(false)
                        .Clipping(EWidgetClipping::ClipToBounds).OnClicked_Lambda(Toggle)
                        .Visibility_Lambda([H=Host]{return H.IsValid()&&H->AreSupplyNamesVisible()?EVisibility::Visible:EVisibility::Hidden;})
                        [Label(Supply.Name)]];
                    Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SACEPluginText).Font(FCoreStyle::GetDefaultFontStyle("Regular",12))
                        .Tag(FName(*FString::Printf(TEXT("SupplyCount.%d"),I)))
                        .Text_Lambda([H=Host,I]{return FText::AsNumber(H.IsValid()&&H->GetSupplies().IsValidIndex(I)?H->GetSupplies()[I].Count:0);})];
                    if(!Locked)Row->AddSlot().AutoWidth().Padding(4,0)[Button(TEXT("-"),TEXT("Remove this item from the ledger"),[H=Host,I]{if(H.IsValid())H->RemoveSupply(I);})];
                    Rows->AddSlot().AutoHeight().Padding(2,2)[Row];
                }
                Body->AddSlot().FillHeight(1)[SNew(SScrollBox).ScrollBarThickness(FVector2D(4,4)).ScrollBarPadding(0)+SScrollBox::Slot()[Rows]];
                Body->SetToolTip(FACEPluginStyle::Tooltip(FText::FromString(Locked?TEXT("Unlock the game UI or hold Quick-show pinned windows to edit this HUD."):TEXT("Track selected items across your inventory."))));
                if(!Locked||LedgerSupplies.IsEmpty())
                {
                    Body->AddSlot().AutoHeight().HAlign(HAlign_Left)[Button(TEXT("+"),TEXT("Track the selected inventory item"),[H=Host]{if(H.IsValid())H->EditSupplySelection(true);})];
                    Body->AddSlot().AutoHeight()[SNew(SACEPluginText).AutoWrapText(true).Font(FCoreStyle::GetDefaultFontStyle("Regular",10))
                        .Text_Lambda([H=Host]{return FText::FromString(H.IsValid()?H->SupplyNotice:FString());})];
                }
            }
            else
            {
                auto Row=[&](const FString& Caption,int32 Metric)
                {
                    auto R=SNew(SHorizontalBox);R->AddSlot().FillWidth(1)[Label(Caption)];
                    for(bool Lum:{false,true})R->AddSlot().FillWidth(1).HAlign(HAlign_Right)[SNew(SACEPluginText).Font(FCoreStyle::GetDefaultFontStyle("Regular",12))
                        .Tag(Lum&&Metric>=0?FName(*FString::Printf(TEXT("HuntLuminance%d"),Metric)):NAME_None)
                        .Text_Lambda([this,Metric,Lum]
                        {
                            if(Metric<0){static const FText XP=FText::FromString(TEXT("XP")),Luminance=FText::FromString(TEXT("Luminance"));return Lum?Luminance:XP;}
                            return HuntText[Metric][Lum?1:0];
                        })];
                    Body->AddSlot().AutoHeight().Padding(3,3)[R];
                };
                Row(TEXT(""),-1);Row(TEXT("Last 5 min"),0);Row(TEXT("Per hour"),1);Row(TEXT("Total"),2);
                Row(TEXT("Available"),3);
                auto Estimate=SNew(SHorizontalBox);
                Estimate->AddSlot().FillWidth(1)[Label(TEXT("Next level (est.)"))];
                Estimate->AddSlot().AutoWidth().HAlign(HAlign_Right)[SNew(SACEPluginText).Tag(TEXT("HuntNextLevel"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular",12)).Text_Lambda([this]{return HuntEstimate;})];
                Body->AddSlot().AutoHeight().Padding(3,3)[Estimate];
                auto Elapsed=SNew(SHorizontalBox);
                Elapsed->AddSlot().FillWidth(1).Padding(0,0,8,0)[Label(TEXT("Time since last reset"))];
                Elapsed->AddSlot().AutoWidth().HAlign(HAlign_Right)[SNew(SACEPluginText).Tag(TEXT("HuntElapsed"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Regular",12)).Text_Lambda([this]{return HuntElapsed;})];
                Body->AddSlot().AutoHeight().Padding(3,3)[Elapsed];
                Body->SetToolTip(FACEPluginStyle::Tooltip(FText::FromString(FString(TEXT("Rates and level estimates use gains since login or Reset. Spending keeps earned totals intact."))
                    +TEXT(" Luminance counts gains credited to your balance. -- means the balance is pending.")
                    +(Locked?TEXT("\nUnlock the game UI or hold Quick-show pinned windows to edit this HUD."):TEXT("")))));
                if(!Locked)Body->AddSlot().AutoHeight()[Button(TEXT("Reset run"),TEXT("Clear totals and begin a new estimate now"),[H=Host]{if(H.IsValid())H->ResetHuntLedger();})];
            }
        }
    };
}
TSharedRef<SWidget> UACEPluginSubsystem::MakeLedgerPanel(const FString& Id)
{UpdateLedgers();return SNew(SACELedgerPanel).Host(this).Id(Id);}
