#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Mods/ACEWaypoint.h"
#include "Mods/ACEWaypointAtlas.h"
#include "UI/ACERetailTextBlock.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointImportFormatsTest,"ACE.Plugins.Waypoint.ImportFormats",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointImportFormatsTest::RunTest(const FString&)
{
    using namespace ACEWaypointAtlas;
    FImportResult Parsed;FString Error;
    const FString Normalized=TEXT(R"(<locations type="CrossroadsOfDereth">
        <loc name="Al-Arqas" type="Town" NS="-31.1" EW="13.7"/>
        <loc name="Portal" type="WildernessPortal" NS="1" EW="2" exitNS="3" exitEW="4"/>
        <loc name="Axis exit" type="NPC" NS="1" EW="2" exitNS="0" exitEW="5"/>
        <loc name="Origin exit" type="Portal" NS="1" EW="2" exitNS="0" exitEW="0"/>
        <loc name="Disabled" type="NPC" NS="1" EW="2" exitNS="3" exitEW="4" use="False"/>
        <loc name="Disabled zero" type="Portal" NS="1" EW="2" exitNS="3" exitEW="4" use="0"/>
        <loc name="Broken exit" type="Portal" NS="1" EW="2" exitNS="broken" exitEW="4"/>
        <loc name="Partial exit" type="Portal" NS="1" EW="2" exitNS="3"/>
        <loc name="Broken location" type="NPC" NS="NaN" EW="2"/>
        <loc name="Retired" type="Town" NS="1" EW="2" retired="True"/>
    </locations>)");
    if(!TestTrue(TEXT("Normalized GoArrow records remain usable despite a bad row"),ParseLocations(Normalized,Parsed,Error)))return false;
    TestEqual(TEXT("Only invalid or retired normalized locations are skipped"),Parsed.Locations.Num(),8);
    TestEqual(TEXT("Bad location coordinates are reported"),Parsed.InvalidLocations,1);
    TestEqual(TEXT("Malformed and incomplete exits are reported separately"),Parsed.InvalidExits,2);
    TestEqual(TEXT("Retired normalized records are excluded"),Parsed.RetiredLocations,1);
    auto Find=[&](const TCHAR* Name){return Parsed.Locations.FindByPredicate([&](const auto& Place){return Place.Name==Name;});};
    auto HasExit=[&](const TCHAR* Name,FVector2D Exit){const auto* Place=Find(Name);return Place&&Place->TravelExit.IsSet()&&Place->TravelExit.GetValue()==Exit;};
    TestTrue(TEXT("Normalized north/south coordinates retain their sign"),Find(TEXT("Al-Arqas"))&&Find(TEXT("Al-Arqas"))->Coordinates==FVector2D(13.7,-31.1));
    TestTrue(TEXT("Normalized portal exits retain their sign and axes"),HasExit(TEXT("Portal"),{4,3}));
    TestTrue(TEXT("Explicit normalized zero-axis exits are real destinations"),HasExit(TEXT("Axis exit"),{5,0})&&HasExit(TEXT("Origin exit"),{0,0}));
    for(const TCHAR* Name:{TEXT("Disabled"),TEXT("Disabled zero"),TEXT("Broken exit"),TEXT("Partial exit")})
        TestTrue(TEXT("Unusable travel links retain their searchable entrance"),Find(Name)&&!Find(Name)->TravelExit.IsSet());

    FString Numeric=TEXT("<locations><loc name='Scientific' NS='-3.11e+1' EW='+1.37E1'/><loc name='Fraction' NS='-.5' EW='+.25'/><loc name='Whole' NS='1.' EW='-0'/><loc name='Dot exit' NS='1' EW='2' exitNS='.' exitEW='5'/>");
    const TCHAR* InvalidNumbers[]={TEXT("."),TEXT("e"),TEXT("f"),TEXT("+."),TEXT("1e"),TEXT("1e+"),TEXT("-3E-"),TEXT("1f"),TEXT("1.2x"),TEXT("1 2")};
    for(const TCHAR* Number:InvalidNumbers)Numeric+=FString::Printf(TEXT("<loc name='Bad %s' NS='%s' EW='2'/>"),Number,Number);
    Numeric+=TEXT("</locations>");
    if(!TestTrue(TEXT("Valid decimal coordinates survive malformed neighboring records"),ParseLocations(Numeric,Parsed,Error)))return false;
    TestEqual(TEXT("Incomplete and nondecimal coordinate strings are skipped"),Parsed.InvalidLocations,int32(UE_ARRAY_COUNT(InvalidNumbers)));
    TestEqual(TEXT("All valid decimal forms and the bad-exit entrance remain"),Parsed.Locations.Num(),4);
    TestTrue(TEXT("Signed scientific notation retains its value"),Find(TEXT("Scientific"))&&Find(TEXT("Scientific"))->Coordinates.Equals({13.7,-31.1},.00001));
    TestTrue(TEXT("Signed fractions without leading zero remain valid"),Find(TEXT("Fraction"))&&Find(TEXT("Fraction"))->Coordinates==FVector2D(.25,-.5));
    TestTrue(TEXT("Decimal trailing points and signed zero remain valid"),Find(TEXT("Whole"))&&Find(TEXT("Whole"))->Coordinates==FVector2D(0,1));
    TestEqual(TEXT("A bare decimal point is reported as an invalid exit"),Parsed.InvalidExits,1);
    TestTrue(TEXT("Malformed zero-like exits cannot become false routes"),Find(TEXT("Dot exit"))&&!Find(TEXT("Dot exit"))->TravelExit.IsSet());

    const FString Legacy=TEXT(R"(<atlas><headers><header>name</header></headers><num_location>999</num_location>
        <location><name>GAVersion</name><latitude>0</latitude><longitude>0</longitude></location>
        <location><name>Al-Arqas</name><type>Town</type><latitude>31.1</latitude><longitude>13.7</longitude><retired>N</retired></location>
        <location><name>Portal</name><type>Town Portal</type><latitude>-42.9</latitude><longitude>34.5</longitude><arrival_latitude>33.5</arrival_latitude><arrival_longitude>72.8</arrival_longitude></location>
        <location><name>Axis north</name><type>Landmark</type><latitude>1</latitude><longitude>2</longitude><arrival_latitude>0</arrival_latitude><arrival_longitude>5</arrival_longitude></location>
        <location><name>Axis east</name><type>Landmark</type><latitude>1</latitude><longitude>2</longitude><arrival_latitude>5</arrival_latitude><arrival_longitude>0</arrival_longitude></location>
        <location><name>Zero pair</name><type>Landmark</type><latitude>1</latitude><longitude>2</longitude><arrival_latitude>0</arrival_latitude><arrival_longitude>0</arrival_longitude></location>
        <location><name>Broken exit</name><type>NPC</type><latitude>1</latitude><longitude>2</longitude><arrival_latitude>434</arrival_latitude><arrival_longitude>5</arrival_longitude></location>
        <location><name>Broken location</name><type>NPC</type><latitude>745</latitude><longitude>85</longitude></location>
        <location><name>Old portal</name><latitude>1</latitude><longitude>2</longitude><retired>Y</retired></location>
        <location><name>Other old portal</name><latitude>1</latitude><longitude>2</longitude><retired>y</retired></location>
    </atlas>)");
    if(!TestTrue(TEXT("Raw CoD atlases are accepted without conversion"),ParseLocations(Legacy,Parsed,Error)))return false;
    TestEqual(TEXT("Legacy metadata and retired rows are not map destinations"),Parsed.Locations.Num(),6);
    TestEqual(TEXT("Legacy retired flags are case insensitive"),Parsed.RetiredLocations,2);
    TestEqual(TEXT("Legacy invalid coordinates are skipped and counted"),Parsed.InvalidLocations,1);
    TestEqual(TEXT("A bad legacy exit does not discard its entrance"),Parsed.InvalidExits,1);
    TestTrue(TEXT("CoD latitude is converted from south-positive to north-positive"),Find(TEXT("Al-Arqas"))&&Find(TEXT("Al-Arqas"))->Coordinates==FVector2D(13.7,-31.1));
    TestTrue(TEXT("CoD arrival latitude receives the same sign conversion"),HasExit(TEXT("Portal"),{72.8,-33.5}));
    for(const TCHAR* Name:{TEXT("Axis north"),TEXT("Axis east"),TEXT("Zero pair"),TEXT("Broken exit")})
        TestTrue(TEXT("Raw CoD zero components and malformed exits never create false transport links"),Find(Name)&&!Find(Name)->TravelExit.IsSet());
    const auto Before=Parsed.Locations;const int32 BeforeInvalid=Parsed.InvalidLocations;
    for(const TCHAR* Broken:{TEXT("<unexpected/>"),TEXT("<locations><loc"),TEXT("<locations><loc name=\"Bad\" NS=\"745\" EW=\"5\"/></locations>"),
        TEXT("<locations><loc name=\"First\" NS=\"1\" EW=\"2\"/></locations><broken"),
        TEXT("<locations><loc name=\"First\" NS=\"1\" EW=\"2\"/></locations><other/>"),
        TEXT("<locations><loc name=\"First\" NS=\"1\" EW=\"2\"/></locations>trailing text"),
        TEXT("<locations><loc name=\"First\" NS=\"1\" EW=\"2\"/></locations></extra>")})
    {
        TestFalse(TEXT("Malformed or wholly unusable input is rejected"),ParseLocations(Broken,Parsed,Error));
        TestTrue(TEXT("A rejected import returns an explanation and preserves previous results"),!Error.IsEmpty()&&Parsed.Locations.Num()==Before.Num()&&Parsed.InvalidLocations==BeforeInvalid&&Parsed.Locations[0].Name==Before[0].Name);
    }
    FString Excess=TEXT("<locations>");
    for(int32 I=0;I<20001;++I)Excess+=TEXT("<loc name=\"Place\" NS=\"1\" EW=\"2\"/>");
    Excess+=TEXT("</locations>");
    TestFalse(TEXT("Oversized record counts are rejected transactionally"),ParseLocations(Excess,Parsed,Error));
    TestEqual(TEXT("The record limit preserves the last usable atlas"),Parsed.Locations.Num(),Before.Num());
    TestTrue(TEXT("A declaration and comments around one complete root remain supported"),ParseLocations(
        TEXT("<?xml version=\"1.0\"?>\n<!--before-->\n<locations><loc name=\"Complete\" NS=\"1\" EW=\"2\"/></locations>\n<!--after-->"),Parsed,Error));
    TestEqual(TEXT("Document validation keeps the intended atlas root"),Parsed.Locations.Num(),1);
    const FString RichLegacy=TEXT(R"(<?xml version="1.0" encoding="UTF-16"?>
        <atlas><location><name>North &amp; South <![CDATA[>]]> &#x00E9;</name>
        <type>Landmark</type><latitude>
        31.1
        </latitude><longitude>13.7</longitude>
        <description>Aerlinthe > Watcher > Temple.
        The description can contain "quotes", &#62; and <![CDATA[<directions>]]>.</description>
        </location></atlas>)");
    if(TestTrue(TEXT("Real CoD prose, CDATA, entities and decoded Unicode XML are accepted"),ParseLocations(RichLegacy,Parsed,Error)))
    {
        TestEqual(TEXT("SAX text chunks concatenate and XML entities decode once"),Parsed.Locations[0].Name,FString(TEXT("North & South > é")));
        TestTrue(TEXT("Multiline coordinate fields preserve the expected sign"),Parsed.Locations[0].Coordinates==FVector2D(13.7,-31.1));
    }
    TestTrue(TEXT("Single-quoted normalized attributes and entities are valid XML"),ParseLocations(
        TEXT("<locations><loc name='Rock &amp; River &#233;' NS='1' EW='2'/></locations>"),Parsed,Error));
    TestEqual(TEXT("Normalized attribute entities decode without changing names"),Parsed.Locations[0].Name,FString(TEXT("Rock & River é")));
    const int32 RetainedCount=Parsed.Locations.Num();const FString RetainedName=Parsed.Locations[0].Name;
    FString EmbeddedNull=TEXT("<locations><loc name='First' NS='1' EW='2'/></locations>");
    EmbeddedNull.AppendChar(TCHAR(0));EmbeddedNull+=TEXT("<other/>");
    TestFalse(TEXT("Embedded NUL bytes cannot hide trailing XML"),ParseLocations(EmbeddedNull,Parsed,Error));
    TestTrue(TEXT("Embedded NUL rejection preserves the prior atlas"),Parsed.Locations.Num()==RetainedCount&&Parsed.Locations[0].Name==RetainedName&&!Error.IsEmpty());
    FString Nested=TEXT("<locations>");for(int32 I=0;I<65;++I)Nested+=TEXT("<nested>");for(int32 I=0;I<65;++I)Nested+=TEXT("</nested>");Nested+=TEXT("</locations>");
    const FString LargeField=TEXT("<locations><loc name=\"")+FString::ChrN(65537,TEXT('a'))+TEXT("\" NS=\"1\" EW=\"2\"/></locations>");
    for(const FString& Rejected:{Nested,LargeField,FString(TEXT("<!DOCTYPE locations [<!ENTITY example 'expanded'>]><locations><loc name='&example;' NS='1' EW='2'/></locations>"))})
    {
        TestFalse(TEXT("XML nesting, field text and document declarations are bounded"),ParseLocations(Rejected,Parsed,Error));
        TestTrue(TEXT("XML validation limits preserve the previous atlas"),Parsed.Locations.Num()==RetainedCount&&Parsed.Locations[0].Name==RetainedName&&!Error.IsEmpty());
    }

    FString Directory;
    if(FParse::Value(FCommandLine::Get(),TEXT("WaypointAtlasDirectory="),Directory))
    {
        struct FFixture{const TCHAR* File;int32 Locations,Retired,Invalid,Exits;};
        const FFixture Fixtures[]={{TEXT("2023GAlocations.xml"),5066,191,0,869},{TEXT("locations.xml"),5066,191,0,869},
            {TEXT("cod_locations.xml"),5202,195,3,870},{TEXT("OLD_locations.xml"),5203,0,3,870},{TEXT("locations - Copy.xml"),5203,0,3,870}};
        for(const auto& Fixture:Fixtures)
        {
            FString Source;
            if(!TestTrue(FString::Printf(TEXT("Read user-supplied fixture %s"),Fixture.File),FFileHelper::LoadFileToString(Source,*(Directory/Fixture.File))))continue;
            if(!TestTrue(FString::Printf(TEXT("Import user-supplied fixture %s: %s"),Fixture.File,*Error),ParseLocations(Source,Parsed,Error)))continue;
            TestEqual(TEXT("Actual atlas retains every active usable record"),Parsed.Locations.Num(),Fixture.Locations);
            TestEqual(TEXT("Actual atlas reports all retired locations"),Parsed.RetiredLocations,Fixture.Retired);
            TestEqual(TEXT("Actual atlas skips only known invalid locations"),Parsed.InvalidLocations,Fixture.Invalid);
            TestEqual(TEXT("Active records in supplied atlases have valid exits"),Parsed.InvalidExits,0);
            TArray<FACEWaypointLandmark> Links;for(const auto& Place:Parsed.Locations)if(Place.TravelExit.IsSet())Links.Add(Place);
            TestEqual(TEXT("Actual atlas preserves all eligible transport exits"),Links.Num(),Fixture.Exits);
            const auto* Mayoi=Find(TEXT("Mayoi"));
            TestTrue(TEXT("Both formats place Mayoi south and east"),Mayoi&&Mayoi->Coordinates.Equals({82,-61.6},.00001));
            const auto* From=Find(TEXT("Holtburg"));const auto* To=Find(TEXT("Ayan Baqur"));
            if(TestTrue(TEXT("Supplied atlas includes known distant towns"),From&&To))
            {
                const auto Trip=FindRoute(From->Coordinates,*To,Links,MapRegions(),[]{return false;});
                TestTrue(TEXT("Actual imported exits connect distant towns with a usable route"),Trip.Num()>1&&Trip.Last().Name==To->Name);
            }
            AddInfo(FString::Printf(TEXT("%s: %d destinations, %d retired, %d invalid, %d transport exits"),Fixture.File,Parsed.Locations.Num(),Parsed.RetiredLocations,Parsed.InvalidLocations,Links.Num()));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointMarkerClusterTest,"ACE.Plugins.Waypoint.MarkerClusters",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointMarkerClusterTest::RunTest(const FString&)
{
    using namespace ACEWaypoint;
    auto Check=[&](const TArray<FMapClusterPoint>& Points,const TArray<FMapCluster>& Clusters,double Radius)
    {
        TMap<int32,FVector2D> Positions;for(const auto& Point:Points)Positions.Add(Point.Index,Point.Point);
        TSet<int32> Seen;
        for(const auto& Cluster:Clusters)
        {
            TestFalse(TEXT("Every visible dot has at least one result"),Cluster.Indices.IsEmpty());
            for(int32 Index:Cluster.Indices)
            {
                const auto* Position=Positions.Find(Index);
                TestTrue(TEXT("Every cluster result refers to a source marker"),Position!=nullptr);
                TestFalse(TEXT("Each source marker appears exactly once"),Seen.Contains(Index));Seen.Add(Index);
                if(Position)TestTrue(TEXT("Every result stays within the dot's fixed screen radius"),(*Position-Cluster.Point).SizeSquared()<=Radius*Radius+.0001);
            }
        }
        TestEqual(TEXT("Clustering preserves every selectable marker"),Seen.Num(),Points.Num());
    };
    const TArray<FMapClusterPoint> Chain={{0,{0,0},TEXT("A")},{1,{17,0},TEXT("B")},
        {2,{34,0},TEXT("C")},{3,{51,0},TEXT("D")},{4,{140,90},TEXT("Single")}};
    auto Groups=ClusterMarkers(Chain);Check(Chain,Groups,18.);
    if(TestEqual(TEXT("Adjacent chains cannot swallow the whole town"),Groups.Num(),3))
    {
        TestTrue(TEXT("The first anchor retains only nearby results"),Groups[0].Point==FVector2D(0,0)&&Groups[0].Indices==TArray<int32>({0,1}));
        TestTrue(TEXT("A separate anchor starts beyond the fixed radius"),Groups[1].Point==FVector2D(34,0)&&Groups[1].Indices==TArray<int32>({2,3}));
        TestTrue(TEXT("An isolated marker remains a single selectable result"),Groups[2].Indices==TArray<int32>({4}));
    }
    const TArray<FMapClusterPoint> Boundary={{0,{-1,17},TEXT("A")},{1,{1,19},TEXT("B")},
        {2,{100,0},TEXT("C")},{3,{118,0},TEXT("D")},{4,{118.01,0},TEXT("E")}};
    Groups=ClusterMarkers(Boundary);Check(Boundary,Groups,18.);
    if(TestEqual(TEXT("Grid boundaries and exact radius do not split neighbors"),Groups.Num(),3))
    {
        TestTrue(TEXT("Nearby negative and positive screen cells can share a dot"),Groups[0].Indices==TArray<int32>({0,1}));
        TestTrue(TEXT("The radius boundary is inclusive"),Groups[1].Indices==TArray<int32>({2,3}));
        TestTrue(TEXT("A point beyond the anchor radius starts another dot"),Groups[2].Indices==TArray<int32>({4}));
    }
    const TArray<FMapClusterPoint> Nearest={{0,{0,0},TEXT("A")},{1,{30,0},TEXT("B")},
        {2,{17,0},TEXT("C")},{3,{15,0},TEXT("D")}};
    Groups=ClusterMarkers(Nearest);Check(Nearest,Groups,18.);
    if(TestEqual(TEXT("Overlapping catchment areas retain separate anchors"),Groups.Num(),2))
    {
        TestTrue(TEXT("An equal-distance result uses a stable anchor"),Groups[0].Indices==TArray<int32>({0,3}));
        TestTrue(TEXT("A result chooses the closest existing dot"),Groups[1].Indices==TArray<int32>({1,2}));
    }
    TArray<FMapClusterPoint> Dense,Sparse;
    for(int32 I=0;I<5219;++I)
    {
        const FString Key=FString::Printf(TEXT("Imported portal %05d"),I);
        Dense.Add({I,{double((I%73)*2),double((I/73)*2)},Key});
        Sparse.Add({I,{double((I%73)*40),double((I/73)*40)},Key});
    }
    const double Started=FPlatformTime::Seconds();
    const auto DenseGroups=ClusterMarkers(Dense),SparseGroups=ClusterMarkers(Sparse);
    AddInfo(FString::Printf(TEXT("Clustered two 5,219-marker atlases in %.2f ms; dense dots=%d, sparse dots=%d"),
        (FPlatformTime::Seconds()-Started)*1000,DenseGroups.Num(),SparseGroups.Num()));
    Check(Dense,DenseGroups,18.);Check(Sparse,SparseGroups,18.);
    TestTrue(TEXT("A town-sized dense atlas becomes a bounded set of dots"),DenseGroups.Num()>1&&DenseGroups.Num()<100);
    TestEqual(TEXT("An atlas spread across the screen retains every singleton"),SparseGroups.Num(),Sparse.Num());
    auto Reordered=Dense;for(int32 I=0;I<Reordered.Num()/2;++I)Reordered.Swap(I,Reordered.Num()-1-I);
    const auto Again=ClusterMarkers(Reordered);
    if(TestEqual(TEXT("Import ordering cannot change the number of dots"),Again.Num(),DenseGroups.Num()))
        for(int32 I=0;I<Again.Num();++I)
            TestTrue(TEXT("Import ordering preserves anchors and ordered result lists"),Again[I].Point==DenseGroups[I].Point&&Again[I].Indices==DenseGroups[I].Indices);
    TArray<FMapClusterPoint> Zoomed={{0,{0,0},TEXT("A")},{1,{8,0},TEXT("B")},{2,{0,8},TEXT("C")}};
    TestEqual(TEXT("Nearby destinations initially share a dot"),ClusterMarkers(Zoomed).Num(),1);
    for(auto& Point:Zoomed)Point.Point*=3;
    TestEqual(TEXT("Zooming in separates nearby destinations"),ClusterMarkers(Zoomed).Num(),3);
    TestEqual(TEXT("A zero radius disables grouping"),ClusterMarkers(Chain,0).Num(),Chain.Num());
    TestEqual(TEXT("An empty map has no dots"),ClusterMarkers({}).Num(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointLabelLayoutTest,"ACE.Plugins.Waypoint.LabelLayout",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointLabelLayoutTest::RunTest(const FString&)
{
    using namespace ACEWaypoint;
    auto Check=[&](const TArray<FMapLabel>& Labels,FVector2D View,const TArray<FBox2D>& Reserved)
    {
        TSet<int32> Indices;
        for(int32 I=0;I<Labels.Num();++I)
        {
            const auto& Label=Labels[I];const FBox2D Box(Label.Origin,Label.Origin+Label.Size);
            TestFalse(TEXT("Each marker gets at most one label"),Indices.Contains(Label.Index));Indices.Add(Label.Index);
            TestTrue(TEXT("Accepted text is fully inside the map"),Box.Min.X>=0&&Box.Min.Y>=0&&Box.Max.X<=View.X&&Box.Max.Y<=View.Y);
            auto Overlaps=[&](const FBox2D& Other){return Box.Min.X<Other.Max.X&&Box.Max.X>Other.Min.X&&Box.Min.Y<Other.Max.Y&&Box.Max.Y>Other.Min.Y;};
            for(const auto& Exclusion:Reserved)TestFalse(TEXT("Labels leave compass and player areas clear"),Overlaps(Exclusion));
            for(int32 J=0;J<I;++J)TestFalse(TEXT("Dense atlas text never overlaps other accepted text"),Overlaps(FBox2D(Labels[J].Origin,Labels[J].Origin+Labels[J].Size)));
        }
    };
    const FVector2D Small(320,360);
    const TArray<FBox2D> Reserved={FBox2D({248,0},{320,72}),FBox2D({152,152},{168,168})};
    TArray<FMapLabelCandidate> Dense;
    // Match the reported atlas size, concentrated around a town-sized cluster.
    // Text width varies as it does in the imported portal/dungeon data.
    for(int32 I=0;I<5219;++I)Dense.Add({I,{130.+(I%17)*2,180.+((I/17)%19)*2},{70.+I%50,18.},FString::Printf(TEXT("Portal %05d"),I),0});
    auto Labels=LayoutLabels(Dense,Small,Reserved);Check(Labels,Small,Reserved);
    TestTrue(TEXT("A crowded imported atlas retains a useful subset"),Labels.Num()>0&&Labels.Num()<Dense.Num());
    auto Reordered=Dense;for(int32 I=0;I<Reordered.Num()/2;++I)Reordered.Swap(I,Reordered.Num()-1-I);
    auto Repeat=LayoutLabels(Reordered,Small,Reserved);
    TestEqual(TEXT("Reordering the atlas preserves the label count"),Repeat.Num(),Labels.Num());
    for(const auto& Label:Labels)
    {
        const auto* Same=Repeat.FindByPredicate([&](const auto& Other){return Other.Index==Label.Index;});
        TestTrue(TEXT("Reordering preserves accepted markers and positions"),Same&&Same->Origin.Equals(Label.Origin,.001)&&Same->Size==Label.Size);
    }
    Dense.Add({6000,{160,210},{90,18},TEXT("Hovered portal"),100});
    Labels=LayoutLabels(Dense,Small,Reserved);Check(Labels,Small,Reserved);
    TestTrue(TEXT("Hover priority reveals a name in a dense town"),Labels.ContainsByPredicate([](const auto& Label){return Label.Index==6000;}));
    TestTrue(TEXT("An oversized name is not emitted outside the map"),LayoutLabels({{0,{160,180},{1000,18},TEXT("Oversized"),0}},Small,{}).IsEmpty());
    TestTrue(TEXT("A collapsed map has no text placements"),LayoutLabels(Dense,FVector2D::ZeroVector,{}).IsEmpty());
    TArray<FMapLabelCandidate> SmallTown,LargeTown;
    for(int32 I=0;I<100;++I)
    {
        FMapLabelCandidate Candidate{I,{20.+(I%10)*28,20.+(I/10)*28},{80,18},FString::Printf(TEXT("Town place %03d"),I),0};
        SmallTown.Add(Candidate);Candidate.Point*=3;LargeTown.Add(Candidate);
    }
    const auto SmallLabels=LayoutLabels(SmallTown,{320,320},{}),LargeLabels=LayoutLabels(LargeTown,{960,960},{});
    Check(SmallLabels,{320,320},{});Check(LargeLabels,{960,960},{});
    TestTrue(TEXT("Enlarging the same map reveals more names"),LargeLabels.Num()>SmallLabels.Num());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointAtlasRouteTest,"ACE.Plugins.Waypoint.AtlasRoutes",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointAtlasRouteTest::RunTest(const FString&)
{
    const auto& Regions=ACEWaypointAtlas::MapRegions();
    if(!TestTrue(TEXT("Bundled GoArrow region boundaries loaded"),!Regions.IsEmpty()))return false;
    FACEWaypointLandmark Goal{{52,84},TEXT("Dark Isle"),TEXT("Town")};
    auto Route=[&](FVector2D From,const TArray<FACEWaypointLandmark>& Links)
    {return ACEWaypointAtlas::FindRoute(From,Goal,Links,Regions,[]{return false;});};
    const FVector2D Mainland(52,82);
    TestTrue(TEXT("A short walk cannot cross the sea"),Route(Mainland,{}).IsEmpty());
    FACEWaypointLandmark Link{Mainland,TEXT("Ferry"),TEXT("NPC"),Goal.Coordinates};
    auto Steps=Route(Mainland,{Link});
    TestEqual(TEXT("Non-portal transports with exits can reach an island"),Steps.Num(),2);
    Goal.Coordinates=Mainland;
    TestTrue(TEXT("An atlas exit cannot be taken in reverse"),Route({52,84},{Link}).IsEmpty());
    Goal.Coordinates={52,81};
    TestEqual(TEXT("Nearby mainland destination uses a single walking leg"),Route(Mainland,{Link}).Num(),1);
    Goal.Coordinates={60,0};
    const TArray<FACEWaypointLandmark> Chain={{{1,0},TEXT("First"),TEXT("Portal"),FVector2D(25,0)},
        {{26,0},TEXT("Second"),TEXT("Portal"),FVector2D(59,0)}};
    Steps=Route({0,0},Chain);
    TestTrue(TEXT("Planner minimizes walking distance over multiple portals"),
        Steps.Num()==3 && Steps[0].Name==TEXT("First") && Steps[1].Name==TEXT("Second") && Steps[2].Name==Goal.Name);
    TestTrue(TEXT("Canceled routes never return partial directions"),ACEWaypointAtlas::FindRoute({0,0},Goal,Chain,Regions,[]{return true;}).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointRouteTest,"ACE.Plugins.Waypoint.Route",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointRouteTest::RunTest(const FString&)
{
    auto Profile=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Points;
    auto Add=[&](uint32 Cell,double X,double Y,const FString& Kind=TEXT("walk"))
    {auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("cell"),Cell);O->SetNumberField(TEXT("x"),X);O->SetNumberField(TEXT("y"),Y);O->SetNumberField(TEXT("z"),0);O->SetStringField(TEXT("kind"),Kind);Points.Add(MakeShared<FJsonValueObject>(O));return O;};
    FACEPosition Player;Player.CellId=0x01430171;Player.Location=FVector(49,-75,0);
    Add(0x01430171,49,-75);Add(0x01430172,55,-75,TEXT("jump"));Add(0x01430173,55,-85,TEXT("portal"));Add(0x01430174,90,0);
    Add(0x021B0146,50,50);Add(0x01430171,60,60,TEXT("recall"));Add(0x01430171,70,70);
    Points.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));Add(0x01430171,80,80);
    Profile->SetArrayField(TEXT("route"),Points);
    auto Route=ACEWaypoint::MapRoute(Profile,Player,true,2);
    if(!TestEqual(TEXT("Only this dungeon's positioned waypoints are drawn"),Route.Num(),7))return false;
    TestTrue(TEXT("Current destination is highlighted and preserves route numbering"),Route[1].Current&&Route[1].Index==2);
    TestTrue(TEXT("Adjacent rooms and jumps connect"),Route[1].Connect&&Route[2].Connect);
    TestFalse(TEXT("Portal breaks the line"),Route[3].Connect);
    TestFalse(TEXT("Other dungeon breaks the line"),Route[4].Connect);
    TestFalse(TEXT("Recall breaks the line"),Route[5].Connect);
    TestFalse(TEXT("Unpositioned actions never draw a line through the map origin"),Route[6].Connect);
    Profile->SetBoolField(TEXT("loop_route"),true);Route=ACEWaypoint::MapRoute(Profile,Player,true,2);
    TestTrue(TEXT("Loop links the last point back to the first"),Route.Num()==8&&Route.Last().Connect&&Route.Last().Index==1&&!Route.Last().Current);
    Profile->SetBoolField(TEXT("reverse_route"),true);
    TestEqual(TEXT("Back-and-forth routes do not add a false closing leg"),ACEWaypoint::MapRoute(Profile,Player,true,2).Num(),7);
    Points.Reset();auto Legacy=Add(0x02430001,-143,-75);Legacy->SetBoolField(TEXT("legacy"),true);Profile->SetArrayField(TEXT("route"),Points);
    Route=ACEWaypoint::MapRoute(Profile,Player,true,1);
    TestTrue(TEXT("Imported NAV rebases across nominal indoor landblock edges"),Route.Num()==1&&Route[0].Position.CellId==Player.CellId&&Route[0].Position.Location.Equals(FVector(49,-75,0)));
    Points.Reset();Add(0x7D640001,191,80);Add(0x7E640001,1,80);Add(0x7D640100,10,10);Add(0x80640001,1,80);Profile->SetArrayField(TEXT("route"),Points);
    Player.CellId=0x7D640001;Route=ACEWaypoint::MapRoute(Profile,Player,false,4);
    TestTrue(TEXT("World map shows distant route legs, including across landblocks"),Route.Num()==3&&Route[1].Connect&&Route[2].Current);
    TestTrue(TEXT("World route crosses a landblock boundary continuously"),FMath::IsNearlyEqual((ACEWaypoint::Coordinates(Route[1].Position)-ACEWaypoint::Coordinates(Route[0].Position)).Size()*240,2.,.001));
    TestFalse(TEXT("World map excludes interior legs without bridging them"),Route[2].Connect);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointCoordinatesTest,"ACE.Plugins.Waypoint.CoordinatesAndHeading",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointCoordinatesTest::RunTest(const FString&)
{
    FVector2D P;
    TestEqual(TEXT("Short portal labels preserve destination"),ACEWaypoint::MarkerLabel(TEXT("Portal to Holtburg")),FString(TEXT("Holtburg")));
    TestEqual(TEXT("Ordinary portal names are unchanged"),ACEWaypoint::MarkerLabel(TEXT("Holtburg Portal")),FString(TEXT("Holtburg Portal")));
    TestTrue(TEXT("Retail chat syntax"),ACEWaypoint::Parse(TEXT("42.0N, 33.6E"),P));
    TestEqual(TEXT("EW is X"),P,FVector2D(33.6,42));
    TestTrue(TEXT("Lowercase and whitespace"),ACEWaypoint::Parse(TEXT(" 12.5s 7.25w "),P));
    TestEqual(TEXT("Southern/western hemispheres"),P,FVector2D(-7.25,-12.5));
    for(const FString Bad:{TEXT("103N, 20E"),TEXT("-1N, 2E"),TEXT("1.2.3N, 4E"),TEXT("1N, 2Evil"),TEXT("NaNN, 2E"),TEXT("1E, 2N"),TEXT("1N, 2E extra")})
        TestFalse(*(TEXT("Reject invalid coordinate: ")+Bad),ACEWaypoint::Parse(Bad,P));
    const FString Chat=TEXT("Alice says, meet at 42.0N, 33.6E; then 12.5S, 7.25W.");
    const auto Links=ACEWaypoint::FindCoordinates(Chat);
    TestEqual(TEXT("Two independent clickable destinations"),Links.Num(),2);
    if(Links.Num()==2)
    {
        TestEqual(TEXT("Span preserves message text"),Chat.Mid(Links[0].Begin,Links[0].End-Links[0].Begin),FString(TEXT("42.0N, 33.6E")));
        auto* Row=NewObject<UACERetailTextBlock>();Row->TextLinks.Add({Links[0].Begin,Links[0].End});
        TestFalse(TEXT("Sender remains ordinary text"),Row->IsLinkAt(0));TestTrue(TEXT("Coordinate gets link treatment"),Row->IsLinkAt(Links[0].Begin));
        TestFalse(TEXT("Trailing separator is not clickable"),Row->IsLinkAt(Links[0].End));
        Row->AreLinksEnabled=[]{return false;};TestFalse(TEXT("Disabling plugin disables existing links"),Row->IsLinkAt(Links[0].Begin));
    }
    const FVector2D Origin(0,0),North(0,1);
    TestTrue(TEXT("Forward arrow points up"),FMath::IsNearlyZero(ACEWaypoint::RelativeBearing(Origin,{0,1},North)));
    TestTrue(TEXT("East is right when facing north"),FMath::IsNearlyEqual(ACEWaypoint::RelativeBearing(Origin,{1,0},North),UE_DOUBLE_PI/2));
    TestTrue(TEXT("West is left when facing north"),FMath::IsNearlyEqual(ACEWaypoint::RelativeBearing(Origin,{-1,0},North),-UE_DOUBLE_PI/2));
    TestTrue(TEXT("Behind points down"),FMath::IsNearlyEqual(FMath::Abs(ACEWaypoint::RelativeBearing(Origin,{0,-1},North)),UE_DOUBLE_PI));
    TestTrue(TEXT("Facing east makes north left"),ACEWaypoint::RelativeBearing(Origin,{0,1},{1,0})<0);
    TestTrue(TEXT("No destination: north-facing compass points up"),FMath::IsNearlyZero(ACEWaypoint::NorthBearing({0,1})));
    TestTrue(TEXT("No destination: turning east rotates north left"),FMath::IsNearlyEqual(ACEWaypoint::NorthBearing({1,0}),-UE_DOUBLE_PI/2));
    TestTrue(TEXT("No destination: turning south rotates north behind"),FMath::IsNearlyEqual(FMath::Abs(ACEWaypoint::NorthBearing({0,-1})),UE_DOUBLE_PI));
    TestTrue(TEXT("No destination: turning west rotates north right"),FMath::IsNearlyEqual(ACEWaypoint::NorthBearing({-1,0}),UE_DOUBLE_PI/2));
    FACEPosition A;A.CellId=int32(0x7D640001);A.Location=FVector(191,80,0);
    FACEPosition B=A;B.CellId=int32(0x7E640001);B.Location.X=1;
    TestTrue(TEXT("Landblock transition remains two metres"),FMath::IsNearlyEqual((ACEWaypoint::Coordinates(B)-ACEWaypoint::Coordinates(A)).Size()*240.,2.,.0001));
    B=A;B.CellId=int32(0x7D640100);TestEqual(TEXT("Buildings preserve map coordinates"),ACEWaypoint::Coordinates(A),ACEWaypoint::Coordinates(B));
    for(const FVector2D Up:{FVector2D(0,1),FVector2D(1,0),FVector2D(0,-1),FVector2D(-1,0),FVector2D(.6,.8)})
    {
        TestTrue(TEXT("Player facing goes up at every heading"),ACEWaypoint::ToMap(Up,Up).Equals({0,-1}));
        const FVector2D Point(33.6,-42);
        TestTrue(TEXT("Rotated map clicks round trip to world coordinates"),ACEWaypoint::FromMap(ACEWaypoint::ToMap(Point,Up),Up).Equals(Point));
        TestTrue(TEXT("Rotation preserves distances"),FMath::IsNearlyEqual(ACEWaypoint::ToMap(Point,Up).Size(),Point.Size()));
    }
    return true;
}

#include "Mods/ACEWaypointTiles.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Slate/WidgetRenderer.h"
#include "Widgets/Images/SImage.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointTilesTest,"ACE.Plugins.Waypoint.TerrainTiles",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointTilesTest::RunTest(const FString&)
{
    const auto A=FACEWaypointTiles::Bounds({124,100,2}),B=FACEWaypointTiles::Bounds({126,100,2});
    TestEqual(TEXT("Adjacent terrain tiles share exact edge"),A.Max.X,B.Min.X);
    FACEPosition Position;Position.CellId=0x7C640001;Position.Location=FVector::ZeroVector;
    TestEqual(TEXT("Terrain tile origin matches world coordinates"),A.Min,ACEWaypoint::Coordinates(Position));
    for(double Scale:{2.,9.,10.,11.,20.,40.,64.,100.,160.,320.,640.,1000.})
    {
        const auto Detail=FACEWaypointTiles::Detail(Scale);
        TestTrue(TEXT("LOD has at least one texel per display pixel"),Detail.X*.8*Scale<=Detail.Y);
        TestTrue(TEXT("LOD stays within source detail and upload budget"),Detail.X>=1&&Detail.X<=32&&Detail.Y<=1024);
    }
    if(!FParse::Param(FCommandLine::Get(),TEXT("WaypointRender")))return true;
    FACEWaypointTiles Tiles;const auto View=FBox2D(A.Min+FVector2D(.01,.01),A.Max-FVector2D(.01,.01));
    const double Begin=FPlatformTime::Seconds();double MaxUpdate=0;
    do {const double Before=FPlatformTime::Seconds();Tiles.Update(TEXT("C:/Turbine/Asheron's Call"),View,100);MaxUpdate=FMath::Max(MaxUpdate,FPlatformTime::Seconds()-Before);FPlatformProcess::Sleep(.01f);}
    while(Tiles.GetTiles().IsEmpty()&&FPlatformTime::Seconds()-Begin<20);
    TestEqual(TEXT("Real DAT produces one visible high-detail tile"),Tiles.GetTiles().Num(),1);
    AddInfo(FString::Printf(TEXT("Terrain tile ready in %.2f sec; maximum game-thread update %.2f ms"),FPlatformTime::Seconds()-Begin,MaxUpdate*1000));
    if(Tiles.GetTiles().IsEmpty())return false;
    for(int I=0;I<10;++I)Tiles.Update(TEXT("C:/Turbine/Asheron's Call"),View,100);
    TestEqual(TEXT("Unchanged map reuses texture"),Tiles.GetTiles().Num(),1);
    auto Widget=SNew(SImage).Image(&Tiles.GetTiles()[0]->Brush);FWidgetRenderer Renderer(false,true);
    auto* Target=FWidgetRenderer::CreateTargetFor({512,512},TF_Bilinear,true);
    for(int I=0;I<3;++I){Renderer.DrawWidget(Target,Widget,{512,512},0);FlushRenderingCommands();}
    TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
    TSet<uint32> Colors;for(auto C:Pixels)Colors.Add(C.DWColor());TestTrue(TEXT("Tile contains detailed terrain rather than blank fallback"),Colors.Num()>100);
    TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(512,512,Pixels,Png);FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/Waypoint-TerrainTile.png")));Target->ReleaseResource();
    const auto FineBounds=FACEWaypointTiles::Bounds({124,100,1});
    const FBox2D FineView(FineBounds.Min+FVector2D(.01,.01),FineBounds.Max-FVector2D(.01,.01));
    const double FineBegin=FPlatformTime::Seconds();
    do {Tiles.Update(TEXT("C:/Turbine/Asheron's Call"),FineView,1000);FPlatformProcess::Sleep(.01f);}
    while(Tiles.IsLoading()&&FPlatformTime::Seconds()-FineBegin<20);
    TestFalse(TEXT("High-DPI terrain request completes"),Tiles.IsLoading());
    TestTrue(TEXT("High-DPI zoom receives real 1024-pixel DAT tile"),Tiles.GetTiles().ContainsByPredicate([](const auto& T){return T->Resolution==1024&&T->Texture.IsValid()&&T->Texture->GetSizeX()==1024;}));
    return true;
}
#endif
