#include "Mods/SACEWaypointAtlas.h"
#include "Mods/ACEWaypointAtlas.h"
#include "Mods/ACEWaypoint.h"
#include "Mods/ACEPluginStyle.h"
#include "Async/Async.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
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

class SWaypointRoute : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWaypointRoute){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Host=Args._Host;Style=FACEPluginStyle::Use(Args._Host,TEXT("waypoint.route"));
        SetTag(TEXT("Waypoint.RoutePanel"));
        ChildSlot[SNew(SBorder).BorderImage(&Style->Background).Padding(10)
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginText).Tag(TEXT("Waypoint.RouteTitle"))
                    .Font(FACEPluginStyle::NativeFont("Bold",16)).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Title);})]
                +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SACEPluginText).Tag(TEXT("Waypoint.RouteStatus"))
                    .Font(FACEPluginStyle::NativeFont("Regular",13)).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Status);})]
                +SVerticalBox::Slot().FillHeight(1)[SAssignNew(Scroll,SScrollBox)+SScrollBox::Slot()[SAssignNew(Steps,SVerticalBox)]]]];
        RefreshRoute();
    }
    virtual void Tick(const FGeometry& Geometry,double Time,float Delta)override
    {
        SCompoundWidget::Tick(Geometry,Time,Delta);
        if(Host.IsValid()&&Revision!=Host->WaypointItineraryRevision)RefreshRoute();
    }
private:
    TWeakObjectPtr<UACEPluginSubsystem> Host;
    TSharedPtr<FACEPluginStyle> Style;
    TSharedPtr<SVerticalBox> Steps;
    TSharedPtr<SScrollBox> Scroll;
    FString Title,Status;
    uint64 Revision=0;
    void RefreshRoute()
    {
        Steps->ClearChildren();Scroll->ScrollToStart();
        if(!Host.IsValid())return;
        Revision=Host->WaypointItineraryRevision;
        const auto& Route=Host->WaypointItinerary;
        Title=Route.IsEmpty()?TEXT("No active route"):TEXT("Route to ")+Route.Last().Name;
        Status=Route.IsEmpty()?TEXT("Use Find route in Waypoint to choose a destination.")
            :TEXT("Click a step to aim the arrow. Use portals manually.");
        for(int32 Index=0;Index<Route.Num();++Index)
        {
            const auto& Step=Route[Index];
            FString Label=FString::Printf(TEXT("%d. %s %s at %s"),Index+1,Step.TravelExit?TEXT("Use"):TEXT("Reach"),*Step.Name,*ACEWaypoint::Format(Step.Coordinates));
            if(Step.TravelExit)Label+=TEXT(" to ")+ACEWaypoint::Format(Step.TravelExit.GetValue());
            Steps->AddSlot().AutoHeight().Padding(0,2)[SNew(SACEPluginButton).Tag(FName(*FString::Printf(TEXT("Waypoint.RouteStep.%d"),Index)))
                .ButtonStyle(&Style->Button).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).IsFocusable(false)
                .IsEnabled_Lambda([H=Host]{return H.IsValid()&&H->IsWaypointEnabled();})
                .OnClicked_Lambda([this,Index,ExpectedRevision=Revision]
                {
                    // A hidden window can receive a new route before its next
                    // Slate tick. Never navigate using an obsolete step button.
                    if(!Host.IsValid()||!Host->IsWaypointEnabled()||Host->WaypointItineraryRevision!=ExpectedRevision
                        ||!Host->WaypointItinerary.IsValidIndex(Index))return FReply::Handled();
                    const auto& Current=Host->WaypointItinerary[Index];Host->SetWaypoint(Current.Coordinates);
                    Status=TEXT("Arrow set to ")+Current.Name+TEXT(" at ")+ACEWaypoint::Format(Current.Coordinates);
                    return FReply::Handled();
                })[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",16)).AutoWrapText(true).Text(FText::FromString(Label))]];
        }
    }
};

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
        auto Back=Button(TEXT("Back"),Args._Back);
        Back->SetTag(TEXT("Waypoint.AtlasBack"));
        Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(SACEPluginText).Text(FText::FromString(TEXT("Atlas locations"))).Font(FACEPluginStyle::NativeFont("Bold",18))]
            +SHorizontalBox::Slot().AutoWidth()[Back]];
        Body->AddSlot().AutoHeight().Padding(0,4)[SNew(SACEPluginText).Tag(TEXT("Waypoint.AtlasStatus")).Font(FACEPluginStyle::NativeFont("Regular",13)).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(Status);})];
        Body->AddSlot().AutoHeight()[SNew(SACEPluginEditableTextBox).Tag(TEXT("Waypoint.AtlasSearch")).Style(&Style->Edit).HintText(FText::FromString(TEXT("Search by name or type")))
            .OnTextChanged_Lambda([this](const FText& Value){Filter=Value.ToString().TrimStartAndEnd();FilterRows();})];
        Body->AddSlot().AutoHeight().Padding(0,3)[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",13)).Text_Lambda([this]
            {return FText::FromString(FString::Printf(TEXT("%d of %d locations"),Rows.Num(),Locations.Num()));})];
        Body->AddSlot().AutoHeight()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",13)).AutoWrapText(true)
            .Text(FText::FromString(TEXT("Double-click a location to point the arrow.")))];
        Body->AddSlot().FillHeight(1)[SAssignNew(List,SListView<FLocation>).Tag(TEXT("Waypoint.AtlasResults")).ListItemsSource(&Rows).SelectionMode(ESelectionMode::Single)
            .OnSelectionChanged_Lambda([this](FLocation Value,ESelectInfo::Type){Selected=Value;CancelSearch();})
            .OnMouseButtonDoubleClick_Lambda([this](FLocation Value){Selected=Value;PointArrow();})
            .OnGenerateRow_Lambda([Style](FLocation Value,const TSharedRef<STableViewBase>& Owner)
            {return SNew(STableRow<FLocation>,Owner).Style(&Style->ComboRow).Padding(FMargin(4,3))
                [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(Describe(*Value))).AutoWrapText(true)];})];
        auto Actions=SNew(SHorizontalBox).IsEnabled_Lambda([this]{return Selected.IsValid()&&Host.IsValid()&&Host->IsWaypointEnabled();});
        Actions->AddSlot().AutoWidth()[Button(TEXT("Point arrow"),FSimpleDelegate::CreateSP(this,&SWaypointAtlas::PointArrow))];
        auto FindRoute=Button(TEXT("Find route"),FSimpleDelegate::CreateSP(this,&SWaypointAtlas::Plan));
        FindRoute->SetTag(TEXT("Waypoint.FindRoute"));
        Actions->AddSlot().AutoWidth().Padding(4,0)[FindRoute];
        Body->AddSlot().AutoHeight().Padding(0,4)[Actions];
        // Keep an inline view for embedded/VR surfaces that have no desktop dock.
        Body->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(160)
            .Visibility_Lambda([H=Host]{return H.IsValid()&&!H->WaypointItinerary.IsEmpty()?EVisibility::Visible:EVisibility::Collapsed;})
            [Host->MakeWaypointRoutePanel()]];
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
    TSharedPtr<TAtomic<bool>> Cancel;
    uint64 Revision=0;
    static TSharedRef<SButton> Button(const FString& Label,FSimpleDelegate Action,bool Wrap=false)
    {
        return SNew(SACEPluginButton).ButtonStyle(&FACEPluginStyle::Current()->Button)
            .ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).IsFocusable(false)
            .OnClicked_Lambda([Action]{Action.ExecuteIfBound();return FReply::Handled();})
            [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",16)).AutoWrapText(Wrap).Text(FText::FromString(Label))];
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
            Status=TEXT("The atlas could not be loaded. Choose Back to import a GoArrow or CoD XML file.");
        else if(!Host->bWaypointAtlasHasRoutingData)
            Status=TEXT("Reimport your GoArrow or CoD XML file to include portal exits in routes.");
        FilterRows();
    }
    void FilterRows()
    {
        List->ClearSelection();Selected.Reset();Rows.Reset();
        for(const auto& Place:Locations)
            if(Filter.IsEmpty()||Place->Name.Contains(Filter)||Place->Type.Contains(Filter))Rows.Add(Place);
        List->RequestListRefresh();List->ScrollToTop();
    }
    void CancelSearch()
    {
        if(Cancel){Cancel->Store(true);Cancel.Reset();Status.Reset();}
    }
    void PointArrow()
    {
        if(!Selected||!Host.IsValid()||!Host->IsWaypointEnabled())return;
        CancelSearch();
        Host->SetWaypoint(Selected->Coordinates);
        Status=TEXT("Arrow set to ")+Selected->Name+TEXT(" at ")+ACEWaypoint::Format(Selected->Coordinates);
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
                Self->Host->SetWaypointItinerary(MoveTemp(Steps));
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
TSharedRef<SWidget> UACEPluginSubsystem::MakeWaypointRoutePanel()
{
    return SNew(SWaypointRoute).Host(this);
}
void UACEPluginSubsystem::SetWaypointItinerary(TArray<FACEWaypointLandmark> Steps)
{
    WaypointItinerary=MoveTemp(Steps);++WaypointItineraryRevision;
    if(!WaypointItinerary.IsEmpty())OpenPluginWindow(TEXT("waypoint.route"));
}
