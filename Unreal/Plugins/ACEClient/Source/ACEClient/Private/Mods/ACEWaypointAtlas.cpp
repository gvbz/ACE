#include "Mods/ACEWaypointAtlas.h"
#include "Interfaces/IPluginManager.h"
#include "XmlFile.h"
#include "Algo/Reverse.h"
#include "Misc/DefaultValueHelper.h"
#define XML_STATIC
THIRD_PARTY_INCLUDES_START
#include "expat.h"
THIRD_PARTY_INCLUDES_END
#undef XML_STATIC

namespace ACEWaypointAtlas
{
namespace
{
// FXmlFile's lightweight tokenizer rejects legal '>' characters in CoD notes
// and stops after the first root. Expat validates the complete document while
// keeping only the small set of fields needed for map markers and routing.
struct FAtlasXmlReader
{
    using FFields=TMap<FString,FString>;
    XML_Parser Parser=nullptr;
    TFunction<void(const FFields&,bool)> Consume;
    FFields Fields;
    FString Field,Failure;
    int32 Depth=0,Records=0;
    bool CoD=false,InRecord=false;
    static constexpr int32 MaxFieldText=64*1024;
    ~FAtlasXmlReader(){if(Parser)XML_ParserFree(Parser);}
    void Fail(const TCHAR* Message)
    {Failure=Message;XML_StopParser(Parser,XML_FALSE);}
    static FString FieldName(const FString& Name,bool Raw)
    {
        static const TCHAR* Normal[]={TEXT("name"),TEXT("type"),TEXT("retired"),TEXT("NS"),TEXT("EW"),TEXT("exitNS"),TEXT("exitEW"),TEXT("use")};
        static const TCHAR* Legacy[]={TEXT("name"),TEXT("type"),TEXT("retired"),TEXT("latitude"),TEXT("longitude"),TEXT("arrival_latitude"),TEXT("arrival_longitude"),TEXT("use")};
        for(int32 I=0;I<UE_ARRAY_COUNT(Normal);++I)if(Name==(Raw?Legacy[I]:Normal[I]))return Normal[I];
        return FString();
    }
    static void XMLCALL Start(void* User,const XML_Char* Element,const XML_Char** Attributes)
    {
        auto& R=*static_cast<FAtlasXmlReader*>(User);if(!R.Failure.IsEmpty())return;
        if(++R.Depth>64){R.Fail(TEXT("The atlas exceeds the XML nesting limit. Existing locations were preserved."));return;}
        const FString Name=UTF8_TO_TCHAR(Element);
        if(R.Depth==1)
        {
            R.CoD=Name==TEXT("atlas");
            if(!R.CoD&&Name!=TEXT("locations"))R.Fail(TEXT("Choose a GoArrow locations export or a Crossroads of Dereth atlas XML. Existing locations were preserved."));
        }
        else if(R.Depth==2&&Name==(R.CoD?TEXT("location"):TEXT("loc")))
        {
            if(++R.Records>20000){R.Fail(TEXT("The atlas exceeds the 20,000-location limit. Existing locations were preserved."));return;}
            R.InRecord=true;R.Fields.Reset();
            if(!R.CoD)for(int32 I=0;Attributes[I];I+=2)
            {
                const FString Key=FieldName(UTF8_TO_TCHAR(Attributes[I]),false);if(Key.IsEmpty())continue;
                FString Value=UTF8_TO_TCHAR(Attributes[I+1]);
                if(Value.Len()>MaxFieldText){R.Fail(TEXT("An atlas field exceeds the text limit. Existing locations were preserved."));return;}
                R.Fields.Add(Key,MoveTemp(Value));
            }
        }
        else if(R.CoD&&R.InRecord&&R.Depth==3)
        {
            R.Field=FieldName(Name,true);
            if(!R.Field.IsEmpty())R.Fields.FindOrAdd(R.Field);
        }
    }
    static void XMLCALL End(void* User,const XML_Char*)
    {
        auto& R=*static_cast<FAtlasXmlReader*>(User);if(!R.Failure.IsEmpty())return;
        if(R.Depth==3)R.Field.Reset();
        if(R.Depth==2&&R.InRecord){R.Consume(R.Fields,R.CoD);R.InRecord=false;R.Fields.Reset();}
        --R.Depth;
    }
    static void XMLCALL Text(void* User,const XML_Char* Data,int Length)
    {
        auto& R=*static_cast<FAtlasXmlReader*>(User);
        if(!R.Failure.IsEmpty()||!R.CoD||!R.InRecord||R.Depth!=3||R.Field.IsEmpty())return;
        const FUTF8ToTCHAR Converted(Data,Length);FString& Value=R.Fields.FindChecked(R.Field);
        if(Converted.Length()>MaxFieldText-Value.Len())
        {R.Fail(TEXT("An atlas field exceeds the text limit. Existing locations were preserved."));return;}
        Value.AppendChars(Converted.Get(),Converted.Length());
    }
    static void XMLCALL Doctype(void* User,const XML_Char*,const XML_Char*,const XML_Char*,int)
    {static_cast<FAtlasXmlReader*>(User)->Fail(TEXT("Atlas XML document type declarations are unsupported. Existing locations were preserved."));}
};
}
bool ParseLocations(const FString& Source,FImportResult& Out,FString& Error)
{
    Error.Reset();
    if(Source.Len()>16*1024*1024){Error=TEXT("The atlas exceeds the 16 MB input limit. Existing locations were preserved.");return false;}
    const FTCHARToUTF8 Utf8(*Source,Source.Len());
    if(Utf8.Length()>16*1024*1024){Error=TEXT("The atlas exceeds the 16 MB input limit. Existing locations were preserved.");return false;}
    FImportResult Parsed;FAtlasXmlReader Reader;
    auto Coordinate=[](const FString& Text,double& Value)
    {
        // ParseDouble alone accepts incomplete literals such as '.' and '1e'.
        // Require a complete decimal mantissa and, when present, exponent.
        int32 At=0,Digits=0;
        auto IsDigit=[&]{return At<Text.Len()&&Text[At]>=TEXT('0')&&Text[At]<=TEXT('9');};
        if(At<Text.Len()&&(Text[At]==TEXT('+')||Text[At]==TEXT('-')))++At;
        while(IsDigit()){++At;++Digits;}
        if(At<Text.Len()&&Text[At]==TEXT('.'))
        {++At;while(IsDigit()){++At;++Digits;}}
        if(Digits==0)return false;
        if(At<Text.Len()&&(Text[At]==TEXT('e')||Text[At]==TEXT('E')))
        {
            ++At;if(At<Text.Len()&&(Text[At]==TEXT('+')||Text[At]==TEXT('-')))++At;
            const int32 ExponentStart=At;while(IsDigit())++At;
            if(At==ExponentStart)return false;
        }
        return At==Text.Len()&&FDefaultValueHelper::ParseDouble(Text,Value)&&FMath::IsFinite(Value)&&FMath::Abs(Value)<=102.;
    };
    Reader.Consume=[&](const FAtlasXmlReader::FFields& Fields,bool CoD)
    {
        auto Field=[&](const TCHAR* Key){return Fields.FindRef(Key).TrimStartAndEnd();};
        const FString Name=Field(TEXT("name"));
        if(Name.Equals(TEXT("GAVersion"),ESearchCase::IgnoreCase))return;
        const FString Retired=Field(TEXT("retired"));
        if(Retired.Equals(TEXT("Y"),ESearchCase::IgnoreCase)||Retired.Equals(TEXT("True"),ESearchCase::IgnoreCase)||Retired==TEXT("1"))
        {++Parsed.RetiredLocations;return;}
        double NS=0,EW=0;
        if(Name.IsEmpty()||Name.Len()>256||!Coordinate(Field(TEXT("NS")),NS)||!Coordinate(Field(TEXT("EW")),EW))
        {++Parsed.InvalidLocations;return;}
        // CoD stores south-positive latitude; normalized GoArrow NS is north-positive.
        FACEWaypointLandmark Place;Place.Name=Name;Place.Type=Field(TEXT("type")).Left(64);
        if(CoD)
        {
            const FString Compact=Place.Type.Replace(TEXT(" "),TEXT(""));
            static const TCHAR* KnownTypes[]={TEXT("AllegianceHall"),TEXT("Bindstone"),TEXT("Dungeon"),TEXT("Landmark"),
                TEXT("Lifestone"),TEXT("NPC"),TEXT("Outpost"),TEXT("Portal"),TEXT("PortalHub"),TEXT("SettlementPortal"),
                TEXT("Village"),TEXT("Town"),TEXT("TownPortal"),TEXT("UndergroundPortal"),TEXT("Vendor"),TEXT("WildernessPortal")};
            for(const TCHAR* Type:KnownTypes)if(Compact.Equals(Type,ESearchCase::IgnoreCase)){Place.Type=Type;break;}
        }
        Place.Coordinates={EW,CoD?-NS:NS};
        const FString ExitNS=Field(TEXT("exitNS")),ExitEW=Field(TEXT("exitEW"));
        const FString Use=Field(TEXT("use"));
        if((!ExitNS.IsEmpty()||!ExitEW.IsEmpty())&&!Use.Equals(TEXT("False"),ESearchCase::IgnoreCase)&&Use!=TEXT("0"))
        {
            if(!Coordinate(ExitNS,NS)||!Coordinate(ExitEW,EW))++Parsed.InvalidExits;
            // GoArrow treats a raw CoD arrival with either axis zero as unknown.
            // Explicit axis-zero values in normalized exports are real exits.
            else if(!CoD||(NS!=0&&EW!=0))Place.TravelExit=FVector2D(EW,CoD?-NS:NS);
        }
        Parsed.Locations.Add(MoveTemp(Place));
    };
    // Input has already been decoded to FString by UE; override an original
    // UTF-16 declaration because this parser receives the converted UTF-8.
    Reader.Parser=XML_ParserCreate("UTF-8");
    if(!Reader.Parser){Error=TEXT("Could not initialize the atlas XML reader.");return false;}
    XML_SetUserData(Reader.Parser,&Reader);
    XML_SetElementHandler(Reader.Parser,&FAtlasXmlReader::Start,&FAtlasXmlReader::End);
    XML_SetCharacterDataHandler(Reader.Parser,&FAtlasXmlReader::Text);
    XML_SetStartDoctypeDeclHandler(Reader.Parser,&FAtlasXmlReader::Doctype);
    XML_SetParamEntityParsing(Reader.Parser,XML_PARAM_ENTITY_PARSING_NEVER);
    if(XML_Parse(Reader.Parser,Utf8.Get(),Utf8.Length(),XML_TRUE)!=XML_STATUS_OK)
    {
        Error=Reader.Failure.IsEmpty()?FString::Printf(TEXT("Could not read atlas XML at line %llu. Existing locations were preserved."),
            static_cast<uint64>(XML_GetCurrentLineNumber(Reader.Parser))):Reader.Failure;
        return false;
    }
    if(Parsed.Locations.IsEmpty())
    {Error=TEXT("No usable locations found in the atlas. Existing locations were preserved.");return false;}
    Out=MoveTemp(Parsed);return true;
}
const TArray<FRegion>& MapRegions()
{
    static const TArray<FRegion> Regions=[]
    {
        TArray<FRegion> Result;
        const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
        if(!Plugin)return Result;
        FXmlFile Xml(Plugin->GetBaseDir()/TEXT("ClientMods/waypoint/MapRegions.xml"));
        if(!Xml.IsValid() || !Xml.GetRootNode())return Result;
        // Nested areas and rectangular islands take precedence over the enclosing polygon.
        for(const TCHAR* Tag:{TEXT("innerRect"),TEXT("innerPoly"),TEXT("rect"),TEXT("poly")})
        {
            for(const FXmlNode* Node:Xml.GetRootNode()->GetChildrenNodes())
            {
                if(Node->GetTag()!=Tag)continue;
                FRegion Region;
                for(const FXmlNode* Vertex:Node->GetChildrenNodes())
                {
                    FVector2D Point(FCString::Atod(*Vertex->GetAttribute(TEXT("EW"))),FCString::Atod(*Vertex->GetAttribute(TEXT("NS"))));
                    Region.Bounds+=Point;
                    Region.Polygon.Add(Point);
                }
                if(Region.Polygon.Num()<2)continue;
                if(Region.Polygon.Num()==2)Region.Polygon.Reset();
                Result.Add(MoveTemp(Region));
            }
        }
        return Result;
    }();
    return Regions;
}

static int32 RegionAt(const FVector2D Point,TConstArrayView<FRegion> Regions)
{
    for(int32 Index=0;Index<Regions.Num();++Index)
    {
        const FRegion& Region=Regions[Index];
        if(!Region.Bounds.IsInsideOrOn(Point))continue;
        const auto& Vertices=Region.Polygon;
        if(Vertices.IsEmpty())return Index;
        bool Inside=false;
        for(int32 I=0,J=Vertices.Num()-1;I<Vertices.Num();J=I++)
        {
            const auto& A=Vertices[I];const auto& B=Vertices[J];
            if((A.Y>Point.Y)!=(B.Y>Point.Y) && Point.X<(B.X-A.X)*(Point.Y-A.Y)/(B.Y-A.Y)+A.X)
                Inside=!Inside;
        }
        if(Inside)return Index;
    }
    return INDEX_NONE; // Mainland.
}

TArray<FACEWaypointLandmark> FindRoute(FVector2D Start,const FACEWaypointLandmark& Destination,
    TConstArrayView<FACEWaypointLandmark> Transports,TConstArrayView<FRegion> Regions,TFunctionRef<bool()> Cancelled)
{
    struct FStop
    {
        FVector2D Entrance,Exit;
        int32 EntranceRegion,ExitRegion;
        double Distance=TNumericLimits<double>::Max();
        int32 Hops=MAX_int32,Previous=INDEX_NONE;
        bool Visited=false;
    };
    TArray<FStop> Stops;
    auto Add=[&](FVector2D Entrance,FVector2D Exit)
    {
        Stops.Add({Entrance,Exit,RegionAt(Entrance,Regions),RegionAt(Exit,Regions)});
    };
    for(const auto& Link:Transports)
    {
        check(Link.TravelExit.IsSet());
        Add(Link.Coordinates,Link.TravelExit.GetValue());
    }
    const int32 Goal=Stops.Num();Add(Destination.Coordinates,Destination.Coordinates);
    const int32 Origin=Stops.Num();Add(Start,Start);
    int32 Current=Origin;
    Stops[Origin].Distance=0;Stops[Origin].Hops=0;
    // Dijkstra over directed transports. Generate walking connections on demand so a
    // dense atlas never allocates a portal-by-portal adjacency matrix.
    while(Current!=Goal)
    {
        if(Cancelled())return {};
        FStop& From=Stops[Current];From.Visited=true;
        int32 Next=INDEX_NONE;
        for(int32 I=0;I<=Goal;++I)
        {
            FStop& To=Stops[I];
            if(To.Visited)continue;
            if(From.ExitRegion==To.EntranceRegion)
            {
                const double Walk=FVector2D::Distance(From.Exit,To.Entrance);
                const double Total=From.Distance+Walk;
                const int32 Hops=From.Hops+(I!=Goal);
                if(Walk<50. && (Total<To.Distance || (Total==To.Distance && Hops<To.Hops)))
                {
                    To.Distance=Total;To.Hops=Hops;To.Previous=Current;
                }
            }
            if(To.Previous!=INDEX_NONE && (Next==INDEX_NONE || To.Distance<Stops[Next].Distance ||
               (To.Distance==Stops[Next].Distance && To.Hops<Stops[Next].Hops)))Next=I;
        }
        if(Next==INDEX_NONE)return {};
        Current=Next;
    }
    TArray<FACEWaypointLandmark> Steps{Destination};
    Steps[0].TravelExit.Reset(); // The destination is an arrival, even if it is a portal.
    for(int32 I=Stops[Goal].Previous;I!=Origin;I=Stops[I].Previous)Steps.Add(Transports[I]);
    Algo::Reverse(Steps);
    return Steps;
}
}
