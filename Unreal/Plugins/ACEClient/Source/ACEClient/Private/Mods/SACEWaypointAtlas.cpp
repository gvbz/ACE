#include "Mods/SACEWaypointAtlas.h"
#include "Mods/ACEWaypointAtlas.h"
#include "Mods/ACEWaypoint.h"
#include "Mods/ACEPluginStyle.h"
#include "Async/Async.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"

namespace
{
using FLocation=TSharedPtr<FACEWaypointLandmark>;
FString Describe(const FACEWaypointLandmark& Location)
{
    return FString::Printf(TEXT("%s  ·  %s  ·  %s"),*Location.Name,*Location.Type,*ACEWaypoint::Format(Location.Coordinates));
}

class SWaypointAtlas : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWaypointAtlas){}
        SLATE_ARGUMENT(UACEPluginSubsystem*,Host)
        SLATE_ARGUMENT(FString,Status)
        SLATE_EVENT(FSimpleDelegate,Back)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Host=Args._Host;Status=Args._Status;
        auto Style=FACEPluginStyle::Use(Args._Host,TEXT("waypoint"));
        auto Body=SNew(SVerticalBox);
        Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(SACEPluginText).Text(FText::FromString(TEXT("Atlas locations"))).Font(FACEPluginStyle::NativeFont("Bold",18))]
            +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Back"),Args._Back)]];
        Body->AddSlot().AutoHeight().Padding(0,4)[SNew(SACEPluginText).Tag(TEXT("Waypoint.AtlasStatus")).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Status);})];
        Body->AddSlot().AutoHeight()[SNew(SACEPluginEditableTextBox).Tag(TEXT("Waypoint.AtlasSearch")).Style(&Style->Edit).HintText(FText::FromString(TEXT("Search by name or type")))
            .OnTextChanged_Lambda([this](const FText& Value){Filter=Value.ToString().TrimStartAndEnd();FilterRows();})];
        Body->AddSlot().AutoHeight().Padding(0,3)[SNew(SACEPluginText).Text_Lambda([this]
            {return FText::FromString(FString::Printf(TEXT("%d of %d locations"),Rows.Num(),Locations.Num()));})];
        Body->AddSlot().FillHeight(1)[SAssignNew(List,SListView<FLocation>).Tag(TEXT("Waypoint.AtlasResults")).ListItemsSource(&Rows).SelectionMode(ESelectionMode::Single)
            .OnSelectionChanged_Lambda([this](FLocation Value,ESelectInfo::Type){Selected=Value;CancelSearch();})
            .OnGenerateRow_Lambda([Style](FLocation Value,const TSharedRef<STableViewBase>& Owner)
            {return SNew(STableRow<FLocation>,Owner).Style(&Style->ComboRow).Padding(FMargin(4,2))
                [SNew(SACEPluginText).Text(FText::FromString(Describe(*Value))).AutoWrapText(true)];})];
        auto Actions=SNew(SHorizontalBox).IsEnabled_Lambda([this]{return Selected.IsValid()&&Host.IsValid()&&Host->IsWaypointEnabled();});
        Actions->AddSlot().AutoWidth()[Button(TEXT("Point arrow"),FSimpleDelegate::CreateLambda([this]
        {
            if(!Selected||!Host.IsValid())return;
            CancelSearch();
            Host->SetWaypoint(Selected->Coordinates);
            Status=TEXT("Arrow set to ")+Selected->Name+TEXT(" at ")+ACEWaypoint::Format(Selected->Coordinates);
        }))];
        Actions->AddSlot().AutoWidth().Padding(4,0)[Button(TEXT("Find route"),FSimpleDelegate::CreateSP(this,&SWaypointAtlas::Plan))];
        Body->AddSlot().AutoHeight().Padding(0,4)[Actions];
        Body->AddSlot().AutoHeight().MaxHeight(160)[SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(Directions,SVerticalBox)]];
        ChildSlot[SNew(SBorder).BorderImage(&Style->Background).Padding(10)[Body]];
        RefreshAtlas();
    }
    ~SWaypointAtlas(){CancelSearch();}
    virtual void Tick(const FGeometry& Geometry,double Time,float Delta)override
    {
        SCompoundWidget::Tick(Geometry,Time,Delta);
        if(Host.IsValid()&&Revision!=Host->WaypointLocationsRevision){Status.Reset();RefreshAtlas();}
    }
private:
    TWeakObjectPtr<UACEPluginSubsystem> Host;
    TArray<FLocation> Locations,Rows;
    FLocation Selected;
    FString Filter,Status;
    TSharedPtr<SListView<FLocation>> List;
    TSharedPtr<SVerticalBox> Directions;
    TSharedPtr<TAtomic<bool>> Cancel;
    uint64 Revision=0;
    static TSharedRef<SButton> Button(const FString& Label,FSimpleDelegate Action)
    {
        return SNew(SACEPluginButton).ButtonStyle(&FACEPluginStyle::Current()->Button)
            .ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).IsFocusable(false)
            .OnClicked_Lambda([Action]{Action.ExecuteIfBound();return FReply::Handled();})
            [SNew(SACEPluginText).AutoWrapText(true).Text(FText::FromString(Label))];
    }
    void RefreshAtlas()
    {
        CancelSearch();Locations.Reset();
        for(const auto& Place:Host->GetWaypointLocations())Locations.Add(MakeShared<FACEWaypointLandmark>(Place));
        Locations.Sort([](const FLocation& A,const FLocation& B)
        {
            const int32 Names=A->Name.Compare(B->Name,ESearchCase::IgnoreCase);
            if(Names)return Names<0;
            if(A->Coordinates.Y!=B->Coordinates.Y)return A->Coordinates.Y<B->Coordinates.Y;
            return A->Coordinates.X<B->Coordinates.X;
        });
        Revision=Host->WaypointLocationsRevision;
        if(Locations.IsEmpty())
            Status=TEXT("No locations imported. Select Back, use Browse locations to choose a GoArrow or CoD XML file, then choose Import locations.");
        else if(!Host->bWaypointAtlasHasRoutingData)
            Status=TEXT("Reimport your GoArrow or CoD XML file to include portal exits in routes.");
        FilterRows();ShowDirections();
    }
    void FilterRows()
    {
        List->ClearSelection();Selected.Reset();Rows.Reset();
        for(const auto& Place:Locations)
            if(Filter.IsEmpty()||Place->Name.Contains(Filter)||Place->Type.Contains(Filter))Rows.Add(Place);
        List->RequestListRefresh();List->ScrollToTop();
    }
    void ShowDirections()
    {
        Directions->ClearChildren();
        const auto& Steps=Host->WaypointItinerary;
        if(Steps.IsEmpty())return;
        Directions->AddSlot().AutoHeight()[SNew(SACEPluginText).AutoWrapText(true)
            .Text(FText::FromString(TEXT("Route to ")+Steps.Last().Name+TEXT(" · Select a step to point the arrow")))];
        for(int32 I=0;I<Steps.Num();++I)
        {
            const auto& Step=Steps[I];
            FString Label=FString::Printf(TEXT("%d. %s %s at %s"),I+1,Step.TravelExit?TEXT("Use"):TEXT("Reach"),*Step.Name,*ACEWaypoint::Format(Step.Coordinates));
            if(Step.TravelExit)Label+=TEXT(" → ")+ACEWaypoint::Format(Step.TravelExit.GetValue());
            Directions->AddSlot().AutoHeight()[Button(Label,FSimpleDelegate::CreateLambda([this,Step]
            {
                if(!Host.IsValid()||!Host->IsWaypointEnabled())return;
                CancelSearch();Host->SetWaypoint(Step.Coordinates);
                Status=TEXT("Arrow set to ")+Step.Name+TEXT(" at ")+ACEWaypoint::Format(Step.Coordinates);
            }))];
        }
    }
    void CancelSearch()
    {
        if(Cancel){Cancel->Store(true);Cancel.Reset();Status.Reset();}
    }
    void Plan()
    {
        if(!Selected||!Host.IsValid())return;
        CancelSearch();
        const FACEPosition Player=Host->WaypointPlayerPosition();
        if(!Player.IsValid() || (uint32(Player.CellId)&0xffff)>=0x100)
        {Status=TEXT("Go outdoors to find a route from your current position.");return;}
        if(!Host->bWaypointAtlasHasRoutingData){Status=TEXT("Reimport your GoArrow or CoD XML file to enable atlas routes.");return;}
        const auto& Regions=ACEWaypointAtlas::MapRegions();
        if(Regions.IsEmpty()){Status=TEXT("The Waypoint map regions could not be loaded.");return;}
        TArray<FACEWaypointLandmark> Transports;
        for(const auto& Place:Locations)if(Place->TravelExit)Transports.Add(*Place);
        auto Token=MakeShared<TAtomic<bool>>(false);Cancel=Token;
        Status=TEXT("Finding a route to ")+Selected->Name+TEXT("…");
        TWeakPtr<SWaypointAtlas> Weak=SharedThis(this);
        Async(EAsyncExecution::ThreadPool,[Weak,Token,Start=ACEWaypoint::Coordinates(Player),Target=*Selected,
            Links=MoveTemp(Transports),&Regions,ExpectedRevision=Revision]()
        {
            auto Steps=ACEWaypointAtlas::FindRoute(Start,Target,Links,Regions,[&Token]{return Token->Load();});
            if(Token->Load())return;
            AsyncTask(ENamedThreads::GameThread,[Weak,Token,ExpectedRevision,Steps=MoveTemp(Steps)]()mutable
            {
                auto Self=Weak.Pin();
                if(!Self||Token->Load()||!Self->Host.IsValid()||!Self->Host->IsWaypointEnabled()||
                    Self->Host->WaypointLocationsRevision!=ExpectedRevision)return;
                Self->Cancel.Reset();
                Self->Host->WaypointItinerary=MoveTemp(Steps);Self->ShowDirections();
                const auto& Route=Self->Host->WaypointItinerary;
                if(Route.IsEmpty())Self->Status=TEXT("No route found using this atlas. Point arrow gives the direct bearing.");
                else
                {
                    Self->Host->SetWaypoint(Route[0].Coordinates);
                    Self->Status=TEXT("Head to ")+Route[0].Name+TEXT(" at ")+ACEWaypoint::Format(Route[0].Coordinates);
                }
            });
        });
    }
};
}
TSharedRef<SWidget> MakeWaypointAtlas(UACEPluginSubsystem* Host,FString ImportStatus,FSimpleDelegate Back)
{
    return SNew(SWaypointAtlas).Host(Host).Status(ImportStatus).Back(Back);
}
