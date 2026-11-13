#include "Mods/ACEAutoCraft.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEItemTools.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Views/SListView.h"
#include "Styling/CoreStyle.h"

namespace
{
const FLinearColor CraftInk(1.f,.98f,.92f),CraftMuted(.86f,.83f,.75f),CraftGold(1,.76f,.25f);
TSharedRef<STextBlock> CraftLabel(const FString& Text,int32 Size=14,FLinearColor Color=CraftInk)
{return SNew(SACEPluginText).Text(FText::FromString(Text)).Font(FACEPluginStyle::NativeFont("Regular",Size)).ColorAndOpacity(Color).AutoWrapText(true);}
struct FChoice { FString Key,CraftLabel; FChoice(FString K,FString L):Key(MoveTemp(K)),CraftLabel(MoveTemp(L)){} };
using FChoices=TArray<TSharedPtr<FChoice>>;

// Retain both the selector and its virtualized result list while filtering.
// Typing never rebuilds the plugin window or steals focus from the search box.
class SCraftPicker : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCraftPicker){}
        SLATE_ARGUMENT(TFunction<FChoices()>,Choices)
        SLATE_ARGUMENT(TFunction<void(const FString&)>,Choose)
        SLATE_ATTRIBUTE(FText,Text)
        SLATE_ARGUMENT(TSharedPtr<FACEPluginStyle>,Style)
    SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {
        GetChoices=A._Choices;Choose=A._Choose;Style=A._Style;
        ChildSlot[SAssignNew(Combo,SACEPluginComboButton).ComboButtonStyle(&Style->Combo)
            .OnGetMenuContent(this,&SCraftPicker::Menu).ButtonContent()
            [SNew(SACEPluginText).Text(A._Text).Font(FACEPluginStyle::NativeFont("Regular",15)).AutoWrapText(true)]];
    }
private:
    TFunction<FChoices()> GetChoices;TFunction<void(const FString&)> Choose;TSharedPtr<FACEPluginStyle> Style;
    FChoices All,Filtered;TSharedPtr<SComboButton> Combo;TSharedPtr<SListView<TSharedPtr<FChoice>>> List;
    void Select(TSharedPtr<FChoice> Item)
    {if(Item){Choose(Item->Key);Combo->SetIsOpen(false);}}
    void Filter(const FText& Text)
    {
        TArray<FString> Terms;Text.ToString().ParseIntoArrayWS(Terms);Filtered.Reset();
        for(const auto& Choice:All)
        {bool Match=true;for(const auto& Term:Terms)if(!Choice->CraftLabel.Contains(Term,ESearchCase::IgnoreCase)){Match=false;break;}if(Match)Filtered.Add(Choice);}
        if(List)List->RequestListRefresh();
    }
    TSharedRef<SWidget> Menu()
    {
        All=GetChoices();Filtered=All;TSharedPtr<SEditableTextBox> Search;
        auto Content=SNew(SBox).WidthOverride(430).HeightOverride(340)
            [SNew(SBorder).BorderImage(&Style->Background).Padding(8)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)
                    [SAssignNew(Search,SACEPluginEditableTextBox).Tag(TEXT("AutoCraft.Search")).Style(&Style->Edit).HintText(FText::FromString(TEXT("Type to search...")))
                        .OnTextChanged(this,&SCraftPicker::Filter)
                        .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter&&!Filtered.IsEmpty())Select(Filtered[0]);})]
                +SVerticalBox::Slot().FillHeight(1)
                    [SAssignNew(List,SListView<TSharedPtr<FChoice>>).ListItemsSource(&Filtered).SelectionMode(ESelectionMode::Single)
                        .OnGenerateRow_Lambda([Theme=Style](TSharedPtr<FChoice> Item,const TSharedRef<STableViewBase>& Owner)
                            {return SNew(STableRow<TSharedPtr<FChoice>>,Owner).Style(&Theme->ComboRow).Padding(6)[CraftLabel(Item->CraftLabel)];})
                        .OnSelectionChanged_Lambda([this](TSharedPtr<FChoice> Item,ESelectInfo::Type Type){if(Type!=ESelectInfo::Direct)Select(Item);})]
                +SVerticalBox::Slot().AutoHeight().Padding(0,6,0,0)
                    [SNew(SACEPluginText).Tag(TEXT("AutoCraft.Matches")).ColorAndOpacity(CraftMuted).Font(FACEPluginStyle::NativeFont("Regular",12))
                        .Text_Lambda([this](){return FText::FromString(Filtered.IsEmpty()?TEXT("No matching items"):FString::Printf(TEXT("%d matches - Enter selects the first result"),Filtered.Num()));})]]];
        Combo->SetMenuContentWidgetToFocus(Search);return Content;
    }
};

class SAutoCraftPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAutoCraftPanel):_ShowTitle(true){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(bool,ShowTitle) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Host=Args._Host;Style=FACEPluginStyle::Use(Host.Get(),TEXT("autocraft"));
        for(const auto& P:Host->Plugins)if(P->Id==TEXT("autocraft"))Plugin=P;
        if(!Plugin)return;Host->RefreshAutoCraft(true);
        auto Form=SNew(SVerticalBox).IsEnabled_Lambda([this](){return !Plugin->Running;});
        auto Add=[&](const FString& Title,TSharedRef<SWidget> Control)
        {Form->AddSlot().AutoHeight().Padding(0,0,0,4)[CraftLabel(Title,14,CraftMuted)];Form->AddSlot().AutoHeight().Padding(0,0,0,12)[Control];};
        Add(TEXT("Category"),SNew(SCraftPicker).Style(Style)
            .Text_Lambda([this](){return FText::FromString(Category());})
            .Choices([this](){FChoices Rows;Rows.Add(MakeShared<FChoice>(TEXT("All categories"),TEXT("All categories")));for(const auto& C:Data().Catalog.Categories)Rows.Add(MakeShared<FChoice>(C,C));return Rows;})
            .Choose([this](const FString& C){Plugin->Profile->SetStringField(TEXT("category"),C);Plugin->Profile->SetStringField(TEXT("recipe"),TEXT(""));Save();}));
        Add(TEXT("Item"),SNew(SCraftPicker).Style(Style).Tag(TEXT("AutoCraft.Item"))
            .Text_Lambda([this](){const auto* R=Recipe();return FText::FromString(R?Data().Catalog.Name(R->Output):TEXT("Choose a crafted item..."));})
            .Choices([this](){FChoices Rows;TSet<FString> Seen;for(const auto& R:Data().Catalog.Recipes)if(InCategory(R)&&!Seen.Contains(Data().Catalog.Name(R.Output)))
                {const FString Name=Data().Catalog.Name(R.Output);Seen.Add(Name);Rows.Add(MakeShared<FChoice>(Name,Name));}
                Rows.Sort([](const auto& A,const auto& B){return A->CraftLabel<B->CraftLabel;});return Rows;})
            .Choose([this](const FString& Name){int32 Best=INDEX_NONE;int64 Score=MAX_int64;
                for(int32 Index:Data().Catalog.ByName.FindRef(Name)){const auto& R=Data().Catalog.Recipes[Index];if(!InCategory(R))continue;
                    const int64 Missing=FMath::Max<int64>(0,FMath::Max(1,R.SourceCount)-Data().Inventory.Available(R.Source,R.SourceReusable()))+
                        FMath::Max<int64>(0,FMath::Max(1,R.TargetCount)-Data().Inventory.Available(R.Target,R.TargetReusable()));
                    const int64 Rank=Missing*100000+R.Difficulty;if(Rank<Score){Best=Index;Score=Rank;}}
                if(Best!=INDEX_NONE){Plugin->Profile->SetStringField(TEXT("recipe"),Data().Catalog.Recipes[Best].Id);Save();}}));
        Add(TEXT("Recipe / ingredients"),SNew(SCraftPicker).Style(Style).Tag(TEXT("AutoCraft.Recipe"))
            .Text_Lambda([this](){const auto* R=Recipe();return FText::FromString(R?RecipeLabel(*R):TEXT("Choose an item first"));})
            .Choices([this]()
            {
                FChoices Rows;
                if(const auto* Selected=Recipe())
                {
                    for(int32 Index:Data().Catalog.ByName.FindRef(Data().Catalog.Name(Selected->Output)))
                    {
                        const auto& R=Data().Catalog.Recipes[Index];
                        if(InCategory(R))Rows.Add(MakeShared<FChoice>(R.Id,RecipeLabel(R)));
                    }
                }
                return Rows;
            })
            .Choose([this](const FString& Id){Plugin->Profile->SetStringField(TEXT("recipe"),Id);Save();}));
        auto Quantity=SNew(SHorizontalBox);
        Quantity->AddSlot().FillWidth(1).VAlign(VAlign_Center)[Toggle(TEXT("maximum"),TEXT("Max available"),false)];
        Quantity->AddSlot().AutoWidth().Padding(8,0)[SNew(SBox).WidthOverride(140).IsEnabled_Lambda([this](){return !Option(TEXT("maximum"));})
            [SNew(SACEPluginSpinBox<int32>).Tag(TEXT("AutoCraft.Quantity")).Style(&Style->Spin).MinValue(1).MaxValue(ACEAutoCraft::MaxUnits).MinDesiredWidth(130)
                .Value_Lambda([this](){return int32(ACEItemTools::Number(Plugin->Profile,TEXT("quantity"),1));})
                .OnValueChanged_Lambda([this](int32 Value){Plugin->Profile->SetNumberField(TEXT("quantity"),Value);})
                .OnValueCommitted_Lambda([this](int32 Value,ETextCommit::Type){Plugin->Profile->SetNumberField(TEXT("quantity"),Value);Save();})
                .OnEndSliderMovement_Lambda([this](int32){Save();})]];
        Add(TEXT("Quantity to make (additional finished items)"),Quantity);
        Add(TEXT("Server recipe catalog (.json file path)"),SNew(SACEPluginEditableTextBox).Style(&Style->Edit)
            .HintText(FText::FromString(TEXT("Path to a verified recipe catalog for this server")))
            .OnTextChanged_Lambda([this](const FText& T){CatalogPath=T.ToString().TrimStartAndEnd();}));
        Form->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[Button(TEXT("Import for this server"),[this](){Host->ImportAutoCraftCatalog(CatalogPath);},[this](){return !CatalogPath.IsEmpty();},TEXT("AutoCraft.ImportCatalog"))]
            +SHorizontalBox::Slot().FillWidth(1).Padding(6,0,0,0)[Button(TEXT("Use bundled recipes"),[this](){Host->RemoveAutoCraftCatalog();},[](){return true;},TEXT("AutoCraft.RemoveCatalog"))]];
        Form->AddSlot().AutoHeight().Padding(0,0,0,8)[CraftLabel(TEXT("Server recipes also apply to UCM automatic crafting. Import replaces this server's previous custom catalog; bundled recipes remain unless a recipe ID is overridden."),12,CraftMuted)];
        Form->AddSlot().AutoHeight().Padding(0,0,0,8)[Toggle(TEXT("ingredients"),TEXT("Make missing ingredients from items in my packs"),true)];
        Form->AddSlot().AutoHeight().Padding(0,0,0,8)[Toggle(TEXT("confirm"),TEXT("Confirm this job's crafting prompts automatically"),true)];
        Form->AddSlot().AutoHeight().Padding(0,0,0,12)[Toggle(TEXT("stop_on_failure"),TEXT("Stop after a failed attempt"),false)];
        Form->AddSlot().AutoHeight().Padding(0,0,0,10)[CraftLabel(TEXT("Whole batches may exceed the requested quantity. Max assumes successful crafts. Equipped, traded and retained ingredients are protected; reusable retained tools are allowed."),12,CraftMuted)];
        Form->AddSlot().AutoHeight()[SNew(SACEPluginText).ColorAndOpacity(CraftMuted).AutoWrapText(true).Font(FACEPluginStyle::NativeFont("Regular",12))
            .Text_Lambda([this](){const auto* R=Recipe();return FText::FromString(R?FString::Printf(TEXT("Yield: %d per craft | Difficulty: %d%s"),R->Yield,R->Difficulty,R->Conditional||R->Modified?TEXT(" | Server requirements / item changes apply"):TEXT("")):TEXT(""));})];
        auto Controls=SNew(SHorizontalBox);
        Controls->AddSlot().FillWidth(1)[Button(TEXT("Make"),[this](){Host->StartAutoCraft();},[this](){return !Plugin->Running&&Plugin->Enabled&&Data().Preview.Valid;},TEXT("AutoCraft.Make"))];
        Controls->AddSlot().FillWidth(1).Padding(6,0)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&Style->Button).IsEnabled_Lambda([this](){return Plugin->Running;})
            .OnClicked_Lambda([this](){Host->PauseAutoCraft();return FReply::Handled();})
            [SNew(SACEPluginText).Justification(ETextJustify::Center).Font(FACEPluginStyle::NativeFont("Regular",15)).Text_Lambda([this](){return FText::FromString(Data().Paused?TEXT("Resume"):TEXT("Pause"));})]];
        Controls->AddSlot().FillWidth(1)[Button(TEXT("Stop"),[this](){Host->Stop(Plugin->Id,TEXT("Stopped by player; an in-flight craft may finish."));},[this](){return Plugin->Running;},TEXT("AutoCraft.Stop"))];
        ChildSlot[SNew(SBorder).BorderImage(&Style->Background).Padding(14)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Visibility(Args._ShowTitle?EVisibility::Visible:EVisibility::Collapsed)[CraftLabel(TEXT("Auto Craft"),24)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[CraftLabel(TEXT("Choose a recipe. Review the materials. Craft across all your packs."),14,CraftMuted)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Controls]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0,0,8,0)
                    [SNew(SACEPluginText).Tag(TEXT("AutoCraft.InventorySummary")).AutoWrapText(true)
                        .Font(FACEPluginStyle::NativeFont("Regular",13))
                        .ColorAndOpacity_Lambda([this](){return FSlateColor(Data().Preview.Valid?FLinearColor(.55f,.9f,.5f):CraftGold);})
                        .Text_Lambda([this](){return FText::FromString(Data().InventorySummary);})]
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [Button(TEXT("Check inventory"),[this](){Host->RefreshAutoCraft(true);},[](){return true;},TEXT("AutoCraft.InventoryRefresh"))]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(SACEPluginText).AutoWrapText(true).ColorAndOpacity(CraftGold)
                .Font(FACEPluginStyle::NativeFont("Regular",14)).Text_Lambda([this](){return FText::FromString(Plugin->Status);})]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[Form]
                +SVerticalBox::Slot().AutoHeight().Padding(0,12)[SNew(SBorder).BorderImage(&Style->SectionFrame).Padding(12)
                    [SNew(SACEPluginText).Tag(TEXT("AutoCraft.Materials")).AutoWrapText(true).Font(FACEPluginStyle::NativeFont("Regular",14)).Text_Lambda([this](){return FText::FromString(Data().Details);})]]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[SNew(SCheckBox).Style(&Style->Check)
                .IsChecked_Lambda([this](){return Plugin->Enabled?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this](ECheckBoxState State){Host->SetEnabled(Plugin->Id,State==ECheckBoxState::Checked);})[CraftLabel(TEXT("Enable Auto Craft"))]]]];
    }
    virtual void Tick(const FGeometry& G,double T,float D)override
    {SCompoundWidget::Tick(G,T,D);if(Host.IsValid()){Style->Refresh();Host->RefreshAutoCraft();}}
private:
    FString CatalogPath;
    TWeakObjectPtr<UACEPluginSubsystem> Host;TSharedPtr<FACEClientPlugin> Plugin;TSharedPtr<FACEPluginStyle> Style;
    FACEAutoCraftState& Data()const{return *Host->AutoCraft;}
    bool Option(const TCHAR* Key,bool Default=false)const{bool B=Default;Plugin->Profile->TryGetBoolField(Key,B);return B;}
    FString Category()const{const FString C=ACEItemTools::String(Plugin->Profile,TEXT("category"));return C.IsEmpty()?TEXT("All categories"):C;}
    bool InCategory(const ACEAutoCraft::FRecipe& R)const{return Category()==TEXT("All categories")||Category()==R.Category;}
    const ACEAutoCraft::FRecipe* Recipe()const{return Data().Catalog.Recipes.IsValidIndex(Data().Selected)?&Data().Catalog.Recipes[Data().Selected]:nullptr;}
    FString RecipeLabel(const ACEAutoCraft::FRecipe& R)const{return FString::Printf(TEXT("%s + %s -> %d"),*Data().Catalog.Name(R.Source),*Data().Catalog.Name(R.Target),R.Yield);}
    void Save(){Host->SaveCurrentProfile(Plugin->Id,false);Host->RefreshAutoCraft(true);}
    TSharedRef<SWidget> Toggle(const TCHAR* Key,const FString& Caption,bool Default)
    {return SNew(SCheckBox).Style(&Style->Check).IsChecked_Lambda([this,Key,Default](){return Option(Key,Default)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this,Key](ECheckBoxState State){Plugin->Profile->SetBoolField(Key,State==ECheckBoxState::Checked);Save();})[CraftLabel(Caption)];}
    TSharedRef<SWidget> Button(const FString& Caption,TFunction<void()> Action,TFunction<bool()> Enabled,const TCHAR* ControlTag)
    {return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Tag(FName(ControlTag)).ButtonStyle(&Style->Button).IsEnabled_Lambda(MoveTemp(Enabled))
        .OnClicked_Lambda([Action](){Action();return FReply::Handled();})[CraftLabel(Caption,15)];}
};
}
TSharedRef<SWidget> UACEPluginSubsystem::MakeAutoCraftPanel(bool bShowTitle)
{return SNew(SAutoCraftPanel).Host(this).ShowTitle(bShowTitle);}
