#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEWaypoint.h"
#include "Mods/ACEWaypointAtlas.h"
#include "Mods/ACEWaypointFilePicker.h"
#include "Mods/SACEWaypointAtlas.h"
#include "ACEDungeonMapContours.h"
#include "Mods/ACEWaypointTiles.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "ACEPlayerController.h"
#include "ACEDatSubsystem.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUIResourceResolver.h"
#include "Mods/ACEWaypointTowns.h"
#include "UI/ACERetailMap.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif
#include "Widgets/SLeafWidget.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SCanvas.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"

bool UACEPluginSubsystem::IsWaypointEnabled() const
{auto P=Find(TEXT("waypoint"));return P && P->Enabled;}
bool UACEPluginSubsystem::IsWaypointUnlocked() const
{
    auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const auto* Manager=Client?Client->GetUIElementManager():nullptr;
    return !Manager || !Manager->IsUiLocked();
}
bool UACEPluginSubsystem::IsWaypointMapUnlocked() const
{
    const auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const auto* Manager=Client?Client->GetUIElementManager():nullptr;
    // Use the same lock icon as the rest of the HUD. Ignore the retired
    // map_locked preference so existing profiles cannot leave the map stuck.
    return !Manager||!Manager->IsUiLocked();
}
FACEPosition UACEPluginSubsystem::WaypointPlayerPosition() const
{
    FACEPosition P;auto* GI=GetGameInstance();
    if(auto* PC=Cast<AACEPlayerController>(GI->GetFirstLocalPlayerController()))if(PC->TryGetLocallyPredictedPosition(P))return P;
    if(auto* Client=GI->GetSubsystem<UACEClientSubsystem>())P=Client->GetPlayerPosition();return P;
}
TArray<ACEWaypoint::FRoutePoint> UACEPluginSubsystem::GetWaypointMapRoute(bool Dungeon) const
{
    const auto P=PolicyPlugin();const auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!P||!P->Enabled||!Client||Client->GetSessionState()!=EACESessionState::InWorld)return {};
    return ACEWaypoint::MapRoute(RuntimeRoute?RuntimeRoute:P->Profile,WaypointPlayerPosition(),Dungeon,RoutePoint);
}
namespace
{
FString ArrowPreferenceFile(const UACEPluginSubsystem& Host)
{
    return Host.UserDirectory()/TEXT("Waypoint/floating-arrow.txt");
}
}
void UACEPluginSubsystem::LoadWaypointArrowPreference()
{
    const FString File=ArrowPreferenceFile(*this);
    FString Choice;
    if(IFileManager::Get().FileExists(*File))
    {
        bShowWaypointArrow=FFileHelper::LoadFileToString(Choice,*File)&&Choice.TrimStartAndEnd()==TEXT("visible");
        return;
    }
    auto P=Find(TEXT("waypoint"));
    bShowWaypointArrow=false;
    if(!P)return;
    // Before the dedicated preference, the profile and desktop window could
    // disagree. Honor an existing opt-out from either source.
    bool ProfileVisible=true,WindowVisible=true;
    P->Profile->TryGetBoolField(TEXT("arrow"),ProfileVisible);
    const TSharedPtr<FJsonObject>* Windows=nullptr;
    if(Settings && Settings->TryGetObjectField(TEXT("_window_visibility"),Windows))
        (*Windows)->TryGetBoolField(TEXT("waypoint.arrow"),WindowVisible);
    bShowWaypointArrow=ProfileVisible&&WindowVisible;
    if(!bShowWaypointArrow)
    {
        // A concurrent client might already have saved a deliberate choice.
        FFileHelper::SaveStringToFile(TEXT("hidden"),*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &IFileManager::Get(),FILEWRITE_NoReplaceExisting);
        if(FFileHelper::LoadFileToString(Choice,*File))bShowWaypointArrow=Choice.TrimStartAndEnd()==TEXT("visible");
    }
}
bool UACEPluginSubsystem::WaypointOption(const FString& Key,bool Default) const
{
    if(Key==TEXT("arrow"))return bShowWaypointArrow;
    auto P=Find(TEXT("waypoint"));bool B;return P && P->Profile->TryGetBoolField(Key,B)?B:Default;
}
bool UACEPluginSubsystem::SetWaypointOption(const FString& Key,bool Value)
{
    auto P=Find(TEXT("waypoint"));if(!P)return false;
    if(Key==TEXT("arrow"))
    {
        bShowWaypointArrow=Value;
        if(FFileHelper::SaveStringToFile(Value?TEXT("visible"):TEXT("hidden"),*ArrowPreferenceFile(*this),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return true;
        Notice=TEXT("Could not save floating arrow visibility for the next login.");
        return false;
    }
    P->Profile->SetBoolField(Key,Value);
    return SaveProfile(P->Id,P->ProfileName,ProfileJson(P->Id),false);
}
bool UACEPluginSubsystem::SetWaypoint(const FString& Text)
{
    FVector2D Point;if(!ACEWaypoint::Parse(Text,Point)){Notice=TEXT("Enter coordinates such as 42.0N, 33.6E (N/S first, then E/W).");return false;}
    SetWaypoint(Point);return true;
}
float UACEPluginSubsystem::WaypointMapOpacity() const
{return GetPluginWindowOpacity(TEXT("waypoint.map"));}
void UACEPluginSubsystem::SetWaypointMapOpacity(float Value)
{SetPluginWindowOpacity(TEXT("waypoint.map"),Value);}
namespace
{
FString WaypointZoomKey(bool Overlay,bool Dungeon)
{return FString(TEXT("waypoint.zoom."))+(Overlay?TEXT("pinned."):TEXT("window."))+(Dungeon?TEXT("dungeon"):TEXT("world"));}
double DefaultWaypointZoom(bool Overlay,bool Dungeon){return Dungeon?3.5:(Overlay?8.:1.);}
double MaximumWaypointZoom(bool Dungeon){return Dungeon?48.:384.;}
}
double UACEPluginSubsystem::WaypointMapZoom(bool Overlay,bool Dungeon) const
{
    const double Default=DefaultWaypointZoom(Overlay,Dungeon);double Value=Default;
    if(Settings)Settings->TryGetNumberField(WaypointZoomKey(Overlay,Dungeon),Value);
    return FMath::IsFinite(Value)?FMath::Clamp(Value,.6,MaximumWaypointZoom(Dungeon)):Default;
}
void UACEPluginSubsystem::SetWaypointMapZoom(bool Overlay,bool Dungeon,double Value)
{
    if(!FMath::IsFinite(Value))return;
    Value=FMath::Clamp(Value,.6,MaximumWaypointZoom(Dungeon));
    if(Value==WaypointMapZoom(Overlay,Dungeon))return;
    if(!Settings)Settings=MakeShared<FJsonObject>();
    Settings->SetNumberField(WaypointZoomKey(Overlay,Dungeon),Value);SaveSettings();
}
void UACEPluginSubsystem::SetWaypoint(FVector2D Point,uint32 Dungeon)
{
    if(!IsWaypointEnabled() || !FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y) || FMath::Abs(Point.X)>102 || FMath::Abs(Point.Y)>102)return;
    if(auto P=Find(TEXT("waypoint")))
    {
        P->Profile->SetNumberField(TEXT("east"),Point.X);P->Profile->SetNumberField(TEXT("north"),Point.Y);
        P->Profile->SetNumberField(TEXT("dungeon_landblock"),Dungeon);
        // The arrow/map already display the destination. Do not broadcast it
        // through Notice, which also appears in unrelated plugin windows.
        const FString PreviousNotice=Notice;
        if(SaveProfile(P->Id,P->ProfileName,ProfileJson(P->Id),false))Notice=PreviousNotice;
    }
}
bool UACEPluginSubsystem::GetWaypoint(FVector2D& Point,uint32& Dungeon) const
{
    auto P=Find(TEXT("waypoint"));double X,Y,D=0;
    if(!P || !P->Profile->TryGetNumberField(TEXT("east"),X) || !P->Profile->TryGetNumberField(TEXT("north"),Y) || !FMath::IsFinite(X) || !FMath::IsFinite(Y) || FMath::Abs(X)>102 || FMath::Abs(Y)>102)return false;
    P->Profile->TryGetNumberField(TEXT("dungeon_landblock"),D);Dungeon=FMath::IsFinite(D) && D>=0 && D<=MAX_uint32?uint32(D):0;
    // Older object actions (including Third Eye's Arrow button) saved the
    // short landblock ID. Map clicks and destination comparisons use the high
    // word of the cell ID; repair old targets on read for both arrow and map.
    if(Dungeon && Dungeon<=0xFFFF)Dungeon<<=16;
    Point=FVector2D(X,Y);return true;
}
void UACEPluginSubsystem::ClearWaypoint()
{if(auto P=Find(TEXT("waypoint"))){P->Profile->RemoveField(TEXT("east"));P->Profile->RemoveField(TEXT("north"));SaveProfile(P->Id,P->ProfileName,ProfileJson(P->Id),false);}}

bool UACEPluginSubsystem::ImportWaypointLocations(const FString& FilePath)
{
    FString Path=FilePath.TrimStartAndEnd().TrimQuotes();
    if(Path.IsEmpty()){Notice=TEXT("Enter the full path to a GoArrow or CoD atlas XML file.");return false;}
    FPaths::NormalizeFilename(Path);
    // An imported file lives on the player's filesystem, outside the cooked content mount.
    TUniquePtr<IFileHandle> Input(FPlatformFileManager::Get().GetPlatformPhysical().OpenRead(*Path));
    if(!Input){Notice=FString::Printf(TEXT("Could not open %s. Check the file path and read permissions."),*Path);return false;}
    const int64 Size=Input->Size();
    if(Size<=0 || Size>16*1024*1024){Notice=TEXT("Choose a non-empty GoArrow or CoD atlas XML file of up to 16 MB.");return false;}
    TArray<uint8> Bytes;Bytes.SetNumUninitialized(int32(Size));
    if(!Input->Read(Bytes.GetData(),Size)){Notice=TEXT("Could not finish reading the atlas. Try importing again.");return false;}
    FString Source;FFileHelper::BufferToString(Source,Bytes.GetData(),Bytes.Num());
    ACEWaypointAtlas::FImportResult Parsed;
    if(!ACEWaypointAtlas::ParseLocations(Source,Parsed,Notice))return false;
    TArray<TSharedPtr<FJsonValue>> Records;
    for(const auto& Place:Parsed.Locations)
    {
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("name"),Place.Name);O->SetStringField(TEXT("type"),Place.Type);
        O->SetNumberField(TEXT("east"),Place.Coordinates.X);O->SetNumberField(TEXT("north"),Place.Coordinates.Y);
        if(Place.TravelExit){O->SetNumberField(TEXT("exitEast"),Place.TravelExit->X);O->SetNumberField(TEXT("exitNorth"),Place.TravelExit->Y);}
        Records.Add(MakeShared<FJsonValueObject>(O));
    }
    auto Data=MakeShared<FJsonObject>();Data->SetBoolField(TEXT("includesRouting"),true);Data->SetArrayField(TEXT("locations"),Records);
    FString Json;FJsonSerializer::Serialize(Data,TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    if(FTCHARToUTF8(*Json).Length()>16*1024*1024){Notice=TEXT("The imported atlas exceeds the 16 MB storage limit.");return false;}
    const FString Directory=UserDirectory()/TEXT("Waypoint");IFileManager::Get().MakeDirectory(*Directory,true);
    FString File=FPaths::ConvertRelativePathToFull(Directory/TEXT("locations.json"));
    FPaths::MakePlatformFilename(File);
    const FString Temp=File+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".tmp");
    ON_SCOPE_EXIT{IFileManager::Get().Delete(*Temp,false,true);};
    if(!FFileHelper::SaveStringToFile(Json,*Temp,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {Notice=TEXT("Could not save the imported atlas. Existing locations were preserved.");return false;}
    // IFileManager::Move deletes an existing destination before renaming. Use
    // a same-directory replacement so a failed rename keeps the previous atlas.
#if PLATFORM_WINDOWS
    // Match the physical writer's extended-length handling for long UserDir
    // paths, including when only the temporary filename exceeds MAX_PATH.
    auto WindowsPath=[](const FString& Path)
    {
        if(Path.Len()<MAX_PATH||Path.StartsWith(TEXT("\\\\?\\")))return Path;
        return Path.StartsWith(TEXT("\\\\"))?TEXT("\\\\?\\UNC\\")+Path.RightChop(2):TEXT("\\\\?\\")+Path;
    };
    const bool Saved=!!::MoveFileExW(*WindowsPath(Temp),*WindowsPath(File),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
#else
    const bool Saved=FPlatformFileManager::Get().GetPlatformPhysical().MoveFile(*File,*Temp);
#endif
    if(!Saved){Notice=TEXT("Could not replace the saved atlas. Existing locations were preserved.");return false;}
    WaypointItinerary.Reset();bWaypointLocationsLoaded=false;++WaypointLocationsRevision;GetWaypointLocations();
    Notice=FString::Printf(TEXT("Imported %d locations."),Records.Num());
    if(Parsed.RetiredLocations)Notice+=FString::Printf(TEXT(" Skipped %d retired locations."),Parsed.RetiredLocations);
    if(Parsed.InvalidLocations)Notice+=FString::Printf(TEXT(" Skipped %d locations with invalid names or coordinates."),Parsed.InvalidLocations);
    if(Parsed.InvalidExits)Notice+=FString::Printf(TEXT(" Ignored %d invalid travel exits."),Parsed.InvalidExits);
    Notice+=TEXT(" Atlas data may differ on custom servers.");return true;
}
const TArray<FACEWaypointLandmark>& UACEPluginSubsystem::GetWaypointLocations()
{
    if(bWaypointLocationsLoaded)return WaypointLocations;bWaypointLocationsLoaded=true;WaypointLocations.Reset();bWaypointAtlasHasRoutingData=false;
    const FString File=UserDirectory()/TEXT("Waypoint/locations.json");FString Json;TSharedPtr<FJsonObject> Data;
    if(IFileManager::Get().FileSize(*File)>16*1024*1024 || !FFileHelper::LoadFileToString(Json,*File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Data)||!Data)return WaypointLocations;
    const TArray<TSharedPtr<FJsonValue>>* Records=nullptr;if(!Data->TryGetArrayField(TEXT("locations"),Records)||Records->Num()>20000)return WaypointLocations;
    Data->TryGetBoolField(TEXT("includesRouting"),bWaypointAtlasHasRoutingData);
    for(const auto& V:*Records)
    {
        const TSharedPtr<FJsonObject>* O=nullptr;if(!V->TryGetObject(O))continue;FACEWaypointLandmark M;double EW,NS;
        if((*O)->TryGetStringField(TEXT("name"),M.Name)&&(*O)->TryGetStringField(TEXT("type"),M.Type)&&(*O)->TryGetNumberField(TEXT("east"),EW)&&(*O)->TryGetNumberField(TEXT("north"),NS)&&FMath::IsFinite(EW)&&FMath::IsFinite(NS)&&FMath::Abs(EW)<=102&&FMath::Abs(NS)<=102)
        {
            M.Name=M.Name.Left(256);M.Coordinates=FVector2D(EW,NS);
            if((*O)->TryGetNumberField(TEXT("exitEast"),EW)&&(*O)->TryGetNumberField(TEXT("exitNorth"),NS)&&FMath::IsFinite(EW)&&FMath::IsFinite(NS)&&FMath::Abs(EW)<=102&&FMath::Abs(NS)<=102)
                M.TravelExit=FVector2D(EW,NS);
            WaypointLocations.Add(MoveTemp(M));
        }
    }
    return WaypointLocations;
}

namespace
{
const FLinearColor Accent(1,.76f,.25f),Ink(1.f,.98f,.92f),Muted(.86f,.83f,.75f);
TSharedRef<STextBlock> WaypointLabel(const FString& S,int Size=16,bool Wrap=true){return SNew(SACEPluginText).Text(FText::FromString(S)).Font(FACEPluginStyle::NativeFont("Regular",Size)).ColorAndOpacity(Ink).AutoWrapText(Wrap);}
const FButtonStyle& ButtonStyle() {return FACEPluginStyle::Current()->Button;}
TSharedRef<SButton> WaypointButton(const FString& S,TFunction<void()> Click)
{return SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).IsFocusable(false).ButtonStyle(&ButtonStyle()).OnClicked_Lambda([Click]{Click();return FReply::Handled();})[WaypointLabel(S,16,false)];}
void Text(FSlateWindowElementList& Out,int L,const FGeometry& G,FVector2D At,const FString& S,int Size,FLinearColor Color,bool Center=false)
{
    const auto Font=FACEPluginStyle::NativeFont("Regular",Size);
    if(Center)At.X-=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(S,Font).X*.5;
    FSlateDrawElement::MakeText(Out,L,G.ToPaintGeometry(G.GetLocalSize(),FSlateLayoutTransform(At)),S,Font,ESlateDrawEffect::None,Color);
}
void Line(FSlateWindowElementList& Out,int L,const FGeometry& G,const TArray<FVector2D>& Points,FLinearColor Color,float Width=1)
{FSlateDrawElement::MakeLines(Out,L,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Width);}
void Triangle(FSlateWindowElementList& Out,int L,const FGeometry& G,FVector2D A,FVector2D B,FVector2D C,FLinearColor Color)
{
    TArray<FSlateVertex> V;
    for(auto P:{A,B,C})V.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),FVector2f(P),FVector2f::ZeroVector,Color.ToFColor(false)));
    FSlateDrawElement::MakeCustomVerts(Out,L,FSlateResourceHandle(),V,TArray<SlateIndex>{0,1,2},nullptr,0,0);
}
class SWaypointMapIcon : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SWaypointMapIcon){} SLATE_END_ARGS()
    void Construct(const FArguments&){SetVisibility(EVisibility::HitTestInvisible);}
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(34,34);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const override
    {
        const FVector2D C=G.GetLocalSize()*.5;const double R=FMath::Min(C.X,C.Y)-3;
        for(double Width:{1.,.45})
        {TArray<FVector2D> Points;for(int I=0;I<=32;++I){const double A=I*UE_DOUBLE_PI/16;Points.Add(C+FVector2D(FMath::Cos(A)*R*Width,FMath::Sin(A)*R));}Line(Out,L,G,Points,Accent,1.4f);}
        Line(Out,L,G,{C+FVector2D(-R,0),C+FVector2D(R,0)},Accent,1.4f);
        return L;
    }
};
class SWaypointArrow : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SWaypointArrow){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {
        Host=A._Host;FACEPluginStyle::Use(A._Host,TEXT("waypoint.arrow"));
        SetVisibility(TAttribute<EVisibility>::CreateLambda([H=Host]
        {return H.IsValid()&&H->ShouldShowWaypointArrow()?EVisibility::HitTestInvisible:EVisibility::Collapsed;}));
    }
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(260,190);}
    void Tick(const FGeometry& G,double T,float D)override{SLeafWidget::Tick(G,T,D);Invalidate(EInvalidateWidgetReason::Paint);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const override
    {
        auto* H=Host.Get();if(!H)return L;const auto P=H->WaypointPlayerPosition();
        const float W=G.GetLocalSize().X;const FVector2D Center(W*.5,79);
        if(!P.IsValid()){Text(Out,L+1,G,{W*.5,74},TEXT("Enter the world to navigate"),14,Muted,true);return L+1;}
        const auto Current=ACEWaypoint::Coordinates(P);FVector2D Target;uint32 Dungeon=0;
        Text(Out,L+1,G,{W*.5,10},ACEWaypoint::Format(Current),16,Ink,true);
        bool Active=H->GetWaypoint(Target,Dungeon);
        const bool OtherDungeon=Active && Dungeon && Dungeon!=(uint32(P.CellId)&0xFFFF0000);
        const double Distance=Active?(Target-Current).Size()*240.:0;
        const FVector F=P.GetAceForwardInAcSpace();
        const double Angle=Active&&!OtherDungeon?ACEWaypoint::RelativeBearing(Current,Target,{F.X,F.Y}):ACEWaypoint::NorthBearing({F.X,F.Y});
        auto Rotate=[&](FVector2D V){return Center+FVector2D(V.X*FMath::Cos(Angle)-V.Y*FMath::Sin(Angle),(V.X*FMath::Sin(Angle)+V.Y*FMath::Cos(Angle))*.8);};
        const auto Tip=Rotate({0,-48}),Left=Rotate({-28,30}),Right=Rotate({28,30}),Ridge=Rotate({0,13});
        const auto Color=Active&&!OtherDungeon?Accent:Muted;
        Triangle(Out,L+1,G,Tip,Left,Ridge,Color*.55f);Triangle(Out,L+1,G,Tip,Ridge,Right,Color);
        Line(Out,L+2,G,{Tip,Left,Ridge,Right,Tip},Color,1.5f);
        FString Caption=!Active?TEXT("North"):OtherDungeon?TEXT("Destination in another dungeon"):Distance<3?TEXT("Arrived"):TEXT("To ")+ACEWaypoint::Format(Target);
        Text(Out,L+2,G,{W*.5,132},Caption,13,Ink,true);
        if(Active && !OtherDungeon && H->WaypointOption(TEXT("distance")))Text(Out,L+2,G,{W*.5,154},FString::Printf(TEXT("%.0f m remaining"),Distance),14,Accent,true);
        return L+2;
    }
private:TWeakObjectPtr<UACEPluginSubsystem> Host;
};

class SWaypointMap : public SCompoundWidget
{
    friend class FACEWaypointOverlayTest;
public:
    SLATE_BEGIN_ARGS(SWaypointMap):_Host(nullptr),_Overlay(false){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(bool,Overlay) SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {
        Host=A._Host;Retail=FACEPluginStyle::Use(A._Host,A._Overlay?TEXT("waypoint.dungeon"):TEXT("waypoint.map"));Overlay=A._Overlay;SetClipping(EWidgetClipping::ClipToBounds);SetTag(TEXT("Waypoint.MapSurface"));
        if(Host.IsValid())
        {Resources.Reset(NewObject<UACEUIResourceResolver>());Resources->Initialize(Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>());MapTexture.Reset(Resources->ResolveTexture(0x0600127D));MapBrush.SetResourceObject(MapTexture.Get());MapBrush.DrawAs=ESlateBrushDrawType::Image;}
        if(Host.IsValid())
        {Player=Host->WaypointPlayerPosition();Dungeon=Overlay?((uint32(Player.CellId)&65535)>=256):Host->WaypointOption(TEXT("dungeon"),false);}
        Zoom=Host.IsValid()?Host->WaypointMapZoom(Overlay,Dungeon):DefaultWaypointZoom(Overlay,Dungeon);
        SetToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateSP(this,&SWaypointMap::MarkerTooltip)));
        BuildResultsPanel();
    }
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(560,450);}
    void SetSearch(const FString& Value){Search=Value.Left(128);NextMarkers=0;}
    void ResetView()
    {
        Zoom=DefaultWaypointZoom(Overlay,Dungeon);Pan=FVector2D::ZeroVector;
        if(Host.IsValid())Host->SetWaypointMapZoom(Overlay,Dungeon,Zoom);
        if(Overlay)CenterOnPlayer();Invalidate(EInvalidateWidgetReason::Paint);
    }
    void CenterOnPlayer()
    {
        FollowPlayer=true;
        if(Host.IsValid()){const auto P=Host->WaypointPlayerPosition();if(P.IsValid()){const auto C=ACEWaypoint::Coordinates(P);Pan=-(Project(C,LastSize)-LastSize*.5-Pan);}}
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    void CancelPan()
    {
        PointerDown=false;PressResults.Reset();
        if(FSlateApplication::IsInitialized())if(const auto User=FSlateApplication::Get().GetUser(PointerUser))
            if(User->GetPointerCaptor(PointerIndex).Get()==this)User->ReleaseCapture(PointerIndex);
    }
    void ChangeZoom(double Factor,FVector2D Cursor)
    {
        if(!FMath::IsFinite(Factor)||Factor<=0)return;
        const double Old=Zoom;Zoom=FMath::Clamp(Zoom*Factor,.6,MaximumWaypointZoom(Dungeon));
        Pan=Cursor-LastSize*.5-(Cursor-LastSize*.5-Pan)*(Zoom/Old);
        if(Host.IsValid())Host->SetWaypointMapZoom(Overlay,Dungeon,Zoom);
        if(Overlay&&FollowPlayer)CenterOnPlayer();Invalidate(EInvalidateWidgetReason::Paint);
    }
    FReply OnMouseWheel(const FGeometry& G,const FPointerEvent& E)override
    {LastSize=G.GetLocalSize();ChangeZoom(FMath::Pow(1.25,E.GetWheelDelta()),G.AbsoluteToLocal(E.GetScreenSpacePosition()));return FReply::Handled();}
    FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)override
    {
        if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
        // One controller owns the whole gesture. Consume another press so it
        // cannot steal capture or bubble into the floating window's move handler.
        if(PointerDown)return FReply::Handled();
        Press=Last=G.AbsoluteToLocal(E.GetScreenSpacePosition());CloseResults();
        // Moving the floating window has its own grip. Leave its bottom-right
        // resize handle to the frame while content drags pan in either lock state.
        if(Overlay&&Host.IsValid()&&Host->CanManipulatePluginWindow(TEXT("waypoint.dungeon"))&&Press.X>G.GetLocalSize().X-24&&Press.Y>G.GetLocalSize().Y-24)return FReply::Unhandled();
        PressResults=ClusterResults(HitCluster(Press));
        LastSize=G.GetLocalSize();PointerUser=E.GetUserIndex();PointerIndex=E.GetPointerIndex();PointerDown=true;Dragged=false;return FReply::Handled().CaptureMouse(SharedThis(this));
    }
    FReply OnPreviewMouseButtonDown(const FGeometry& G,const FPointerEvent& E)override
    {
        // A press on a marker must still become a pan if the pointer moves.
        // The result list remains an ordinary independently clickable control.
        if(!Overlay||(ResultsBox&&ResultsBox->GetVisibility().IsVisible()&&ResultsBox->GetCachedGeometry().IsUnderLocation(E.GetScreenSpacePosition())))return FReply::Unhandled();
        return OnMouseButtonDown(G,E);
    }
    FReply OnMouseMove(const FGeometry& G,const FPointerEvent& E)override
    {
        if(PointerDown&&!OwnsPointer(E))return FReply::Handled();
        auto P=G.AbsoluteToLocal(E.GetScreenSpacePosition());Hover=P;
        if(PointerDown){PanPointer(P);Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();}
        return FReply::Unhandled();
    }
    void OnMouseLeave(const FPointerEvent& E)override
    {SCompoundWidget::OnMouseLeave(E);Hover=FVector2D(-100,-100);Invalidate(EInvalidateWidgetReason::Paint);}
    FReply OnMouseButtonUp(const FGeometry& G,const FPointerEvent& E)override
    {
        if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
        if(!PointerDown)return OwnsPointer(E)&&HasMouseCapture()?FReply::Handled().ReleaseMouseCapture():FReply::Unhandled();
        if(!OwnsPointer(E))return FReply::Handled();
        PanPointer(G.AbsoluteToLocal(E.GetScreenSpacePosition()));
        if(!Dragged && Host.IsValid())
        {
            if(PressResults.Num()>1)ShowResults(PressResults);
            else if(PressResults.Num()==1)ChooseResult(PressResults[0]);
            else if(!Overlay)Host->SetWaypoint(Unproject(G.AbsoluteToLocal(E.GetScreenSpacePosition()),G.GetLocalSize()),Dungeon?Landblock:0);
        }
        PointerDown=false;PressResults.Reset();
        return FReply::Handled().ReleaseMouseCapture();
    }
    void OnMouseCaptureLost(const FCaptureLostEvent& E)override
    {PointerDown=false;PressResults.Reset();SCompoundWidget::OnMouseCaptureLost(E);}
    void Tick(const FGeometry& G,double T,float D)override
    {
        SCompoundWidget::Tick(G,T,D);
        const auto PreviousCenter=Unproject(LastSize*.5,LastSize);
        LastSize=G.GetLocalSize();auto* H=Host.Get();if(!H)return;Player=H->WaypointPlayerPosition();
        // Reused/hidden panels can outlive another view changing the saved zoom.
        const double SavedZoom=H->WaypointMapZoom(Overlay,Dungeon);
        Zoom=SavedZoom;
        const auto Facing=Player.GetAceForwardInAcSpace();
        const auto NextUp=H->WaypointOption(TEXT("heading_up"),false)&&Player.IsValid()?FVector2D(Facing.X,Facing.Y).GetSafeNormal():FVector2D(0,1);
        if(!NextUp.IsNearlyZero())MapUp=NextUp;
        Pan=-ACEWaypoint::ToMap((PreviousCenter-Center())*PixelsPerUnit(LastSize),MapUp);
        const uint32 LB=uint32(Player.CellId)&0xFFFF0000;
        const bool Inside=(uint32(Player.CellId)&65535)>=256;
        const bool NextDungeon=Overlay?Inside:H->WaypointOption(TEXT("dungeon"),false);
        if(Dungeon!=NextDungeon || (NextDungeon && (LB!=Landblock || Inside!=WasInside)))
        {CloseResults();CancelPan();Dungeon=NextDungeon;Landblock=LB;WasInside=Inside;NextMarkers=0;NextCellRetry=0;NextRoute=0;Floors.Reset();Fill.Reset();Route.Reset();Contours=FACEDungeonMapContours();Environments.Reset();CellIndex=0;CellCount=0;
         Zoom=H->WaypointMapZoom(Overlay,Dungeon);Pan=FVector2D::ZeroVector;FollowPlayer=true;PointerDown=false;Invalidate(EInvalidateWidgetReason::Paint);
         if(Dungeon)CenterOnPlayer();}
        // The overlay may first open indoors or before the portal DAT is ready.
        if(!Dungeon&&!MapTexture.IsValid()&&Resources.IsValid()&&T>=NextMapRetry)
        {NextMapRetry=T+1;MapTexture.Reset(Resources->ResolveTexture(0x0600127D));MapBrush.SetResourceObject(MapTexture.Get());}
        // A pinned map can reopen before the asynchronous cell DAT is ready.
        // Retry without forcing disk initialization on the rendering thread.
        if(Dungeon&&WasInside&&!CellCount&&T>=NextCellRetry)
        {
            NextCellRetry=T+.5;
            if(auto* Dat=H->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();Dat&&Dat->IsCellReady())
            {FACEDatLandblockInfo Info;if(Dat->LoadLandblockInfo(LB,Info))CellCount=FMath::Min(Info.NumCells,4096u);}
        }
        if(Dungeon && CellIndex<CellCount)LoadDungeonCells();
        if(Dungeon && CellCount && CellIndex==CellCount && !Contours.IsComplete())
        {
            const int32 First=Contours.Lines.Num();Contours.BuildStep();
            for(int32 I=First;I<Contours.Lines.Num();++I)
            {
                const auto& Edge=Contours.Lines[I];FFloor Floor;Floor.Z=(Edge.A.Z+Edge.B.Z)*.5;
                for(auto V:{Edge.A,Edge.B}){FACEPosition P;P.CellId=int32(Landblock|0x100);P.Location=V;Floor.Points.Add(ACEWaypoint::Coordinates(P));}
                Floors.Add(MoveTemp(Floor));
            }
        }
        if(Overlay&&FollowPlayer)CenterOnPlayer();
        if(!Dungeon)if(auto* Dat=H->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();Dat&&Dat->IsCellReady())
        {
            FBox2D Visible(ForceInit);for(const auto Corner:{FVector2D::ZeroVector,FVector2D(LastSize.X,0),LastSize,FVector2D(0,LastSize.Y)})Visible+=Unproject(Corner,LastSize);
            const FVector2D Origin=G.LocalToAbsolute(FVector2D::ZeroVector);
            const double DisplayScale=FMath::Max((FVector2D(G.LocalToAbsolute(FVector2D(1,0)))-Origin).Size(),(FVector2D(G.LocalToAbsolute(FVector2D(0,1)))-Origin).Size());
            Tiles.Update(Dat->GetDatDirectory(),Visible,PixelsPerUnit(LastSize)*DisplayScale);
        }
        if(T>NextMarkers){NextMarkers=T+1;RefreshMarkers();}
        if(!H->WaypointOption(TEXT("show_route"),true)){Route.Reset();NextRoute=0;}
        else if(T>=NextRoute){NextRoute=T+.25;Route=H->GetWaypointMapRoute(Dungeon);}
        UpdateClusters(LastSize);
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    int32 OnPaint(const FPaintArgs& Args,const FGeometry& G,const FSlateRect& Cull,FSlateWindowElementList& Out,int32 L,const FWidgetStyle& Style,bool Enabled)const override
    {
        const float Opacity=Overlay&&Host.IsValid()?Host->WaypointMapOpacity():1.f;
        const FVector2D Size=G.GetLocalSize();
        // Frameless maps still need a backdrop. Fade only the background;
        // outlines, labels and position markers must remain readable.
        if(Overlay)FSlateDrawElement::MakeBox(Out,L,G.ToPaintGeometry(),FCoreStyle::Get().GetBrush("WhiteBrush"),ESlateDrawEffect::None,FLinearColor(0,0,0,Opacity));
        else FSlateDrawElement::MakeBox(Out,L,G.ToPaintGeometry(),&Retail->Background,ESlateDrawEffect::None,FLinearColor(.015f,.024f,.04f)*Retail->Background.TintColor.GetSpecifiedColor());
        Out.PushClip(FSlateClippingZone(G));
        if(!Dungeon && MapTexture.IsValid())
        {
            PaintTile(Out,L+1,G,FBox2D({-101.95,-101.95},{102.05,102.05}),MapBrush);
        }
        if(!Dungeon)
        {
            for(const auto& Tile:Tiles.GetTiles())
            {
                const auto Bounds=FACEWaypointTiles::Bounds(Tile->Key);
                PaintTile(Out,L+1,G,Bounds,Tile->Brush);
            }
        }
        if(Dungeon)
        {
            if(Host.IsValid()&&Host->WaypointOption(TEXT("fill_dungeon"),false))PaintFill(Out,L+1,G);
            for(const auto& Floor:Floors)
            {
                const bool Current=FMath::Abs(Floor.Z-Player.Location.Z)<4;
                TArray<FVector2D> Points;for(auto V:Floor.Points)Points.Add(Project(V,Size));if(Points.Num()>1){Line(Out,L+2,G,Points,Current?FLinearColor(.25f,.72f,.82f):FLinearColor(.12f,.2f,.27f),Current?1.5f:1.f);}
            }
            if(!CellCount)Text(Out,L+2,G,{16,16},TEXT("Enter a dungeon or building to see its layout."),16,Muted);
            else if(CellIndex<CellCount || !Contours.IsComplete())Text(Out,L+2,G,{16,16},FString::Printf(TEXT("Loading layout: %u / %u"),CellIndex,CellCount),14,Muted);
        }
        PaintRoute(Out,L+3,G);
        for(const auto& Cluster:PaintedClusters)
        {
            const auto& M=Markers[Cluster.Indices[0]];const auto P=Cluster.Point;
            if(Cluster.Indices.Num()>1)continue;
            const FLinearColor Color=M.Portal?FLinearColor(.85f,.5f,1):FLinearColor(1,.8f,.4f);
            Line(Out,L+2,G,{P+FVector2D(0,-5),P+FVector2D(5,0),P+FVector2D(0,5),P+FVector2D(-5,0),P+FVector2D(0,-5)},Color,2);
        }
        const int32 Hovered=HoveredMarker(Size);
        const auto Font=FACEPluginStyle::NativeFont("Regular",13);
        const auto Measure=FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
        const FVector2D Padding(2,1);
        const double MaxTextWidth=FMath::Max(0.,Size.X-8);
        TArray<ACEWaypoint::FMapLabelCandidate> Labels;
        for(const auto& Cluster:PaintedClusters)
        {
            if(Cluster.Indices.Num()>1)continue;
            const int32 Index=Cluster.Indices[0];
            const auto& M=Markers[Index];
            if(!M.Portal&&Zoom<3&&Index!=Hovered)continue;
            if(M.LabelSize.X<0||M.LabelWidth!=MaxTextWidth)
            {
                M.LabelWidth=MaxTextWidth;M.LabelText=ACEWaypoint::MarkerLabel(M.Name);
                M.LabelSize=Measure->Measure(M.LabelText,Font);
                if(M.LabelSize.X>MaxTextWidth)
                {
                    const FString Original=M.LabelText,Ellipsis=TEXT("…");
                    int32 Low=0,High=Original.Len();
                    while(Low<High)
                    {
                        const int32 Mid=(Low+High+1)/2;
                        if(Measure->Measure(Original.Left(Mid)+Ellipsis,Font).X<=MaxTextWidth)Low=Mid;else High=Mid-1;
                    }
                    M.LabelText=Low>0?Original.Left(Low)+Ellipsis:FString();
                    M.LabelSize=Measure->Measure(M.LabelText,Font);
                }
            }
            if(!M.LabelText.IsEmpty())Labels.Add({Index,Project(M.Point,Size),M.LabelSize+Padding*2,M.Name,
                Index==Hovered?3:!M.Detailed&&!M.Portal?2:M.Portal?1:0});
        }
        TArray<FBox2D> Reserved={FBox2D({Size.X-(Overlay?140:72),0},{Size.X,72})};
        if(Overlay&&Host.IsValid()&&Host->CanManipulatePluginWindow(TEXT("waypoint.dungeon")))Reserved.Add(FBox2D({0,0},{28,28}));
        for(const auto& Cluster:PaintedClusters)if(Cluster.Indices.Num()>1)
        {const auto Half=ClusterSize(Cluster)*.5;Reserved.Add(FBox2D(Cluster.Point-Half,Cluster.Point+Half));}
        if(Dungeon&&(!CellCount||CellIndex<CellCount||!Contours.IsComplete()))
            Reserved.Add(FBox2D({12,12},{Size.X-12,40}));
        if(!Dungeon&&Tiles.IsLoading())Reserved.Add(FBox2D({8,Size.Y-25},{Size.X-8,Size.Y-2}));
        if(Player.IsValid())
        {const auto P=Project(ACEWaypoint::Coordinates(Player),Size);Reserved.Add(FBox2D(P-FVector2D(12,12),P+FVector2D(12,12)));}
        FVector2D LabelDestination;uint32 LabelTargetLB=0;
        if(Host.IsValid()&&Host->GetWaypoint(LabelDestination,LabelTargetLB)&&(!LabelTargetLB||(Dungeon&&LabelTargetLB==Landblock)))
        {const auto P=Project(LabelDestination,Size);Reserved.Add(FBox2D(P-FVector2D(10,10),P+FVector2D(10,10)));}
        PaintedLabels=ACEWaypoint::LayoutLabels(MoveTemp(Labels),Size,Reserved);
        for(const auto& Label:PaintedLabels)
        {
            // Keep the admitted names readable over terrain, room outlines and
            // dense marker diamonds, including on a transparent pinned map.
            FSlateDrawElement::MakeBox(Out,L+3,G.ToPaintGeometry(Label.Size,FSlateLayoutTransform(Label.Origin)),
                Overlay?FCoreStyle::Get().GetBrush("WhiteBrush"):&Retail->Background,ESlateDrawEffect::None,
                Overlay?FLinearColor(0,0,0,.72f):FLinearColor(0,0,0,.85f)*Retail->Background.TintColor.GetSpecifiedColor());
            Text(Out,L+4,G,Label.Origin+Padding,Markers[Label.Index].LabelText,13,Ink);
        }
        for(const auto& Cluster:PaintedClusters)if(Cluster.Indices.Num()>1)
        {
            const auto BadgeSize=ClusterSize(Cluster);
            FSlateDrawElement::MakeBox(Out,L+4,G.ToPaintGeometry(BadgeSize,FSlateLayoutTransform(Cluster.Point-BadgeSize*.5)),&ClusterBrush,
                ESlateDrawEffect::None,ClusterBrush.TintColor.GetSpecifiedColor());
            Text(Out,L+5,G,Cluster.Point-FVector2D(0,8),FString::FromInt(Cluster.Indices.Num()),12,Ink,true);
        }
        // Companion annotations use the same projection/orientation as the map.
        if(Host.IsValid())for(const auto& M:Host->GetPluginMarkers())if(M.Map)
        {
            FACEWorldObject Object;auto* Client=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            if(!Client->GetWorldObject(M.Id,Object))continue;
            const bool Interior=(uint32(Object.Position.CellId)&65535)>=256;
            if(Interior!=Dungeon||(Dungeon&&(uint32(Object.Position.CellId)&0xffff0000)!=Landblock))continue;
            const auto At=Project(ACEWaypoint::Coordinates(Object.Position),Size);
            if(At.X<0||At.Y<0||At.X>Size.X||At.Y>Size.Y)continue;
            if(M.Shape==TEXT("!"))Text(Out,L+4,G,At,TEXT("!"),18,M.Color,true);
            else if(M.Shape==TEXT("arrow"))Triangle(Out,L+4,G,At+FVector2D(-6,-7),At+FVector2D(6,-7),At+FVector2D(0,5),M.Color);
            else Line(Out,L+4,G,{At+FVector2D(0,-6),At+FVector2D(6,0),At+FVector2D(0,6),At+FVector2D(-6,0),At+FVector2D(0,-6)},M.Color,2);
        }
        if(Player.IsValid())
        {
            const auto P=Project(ACEWaypoint::Coordinates(Player),Size);const auto F=Player.GetAceForwardInAcSpace();
            const auto Forward=ACEWaypoint::ToMap({F.X,F.Y},MapUp);const FVector2D Right(-Forward.Y,Forward.X);
            Triangle(Out,L+4,G,P+Forward*10,P-Forward*7+Right*6,P-Forward*7-Right*6,Accent);
        }
        FVector2D Destination;uint32 TargetLB=0;
        if(Host.IsValid()&&Host->GetWaypoint(Destination,TargetLB)&&(!TargetLB || (Dungeon&&TargetLB==Landblock)))
        {
            const auto P=Project(Destination,Size);Line(Out,L+4,G,{P+FVector2D(-8,-8),P+FVector2D(8,8)},FLinearColor::White,2);Line(Out,L+4,G,{P+FVector2D(-8,8),P+FVector2D(8,-8)},FLinearColor::White,2);
        }
        const auto North=ACEWaypoint::ToMap({0,1},MapUp);const FVector2D Compass(Size.X-36,36);
        Line(Out,L+4,G,{Compass,Compass+North*15},Ink,2);Text(Out,L+4,G,Compass+North*23,TEXT("N"),12,Ink,true);
        if(!Dungeon&&Tiles.IsLoading())Text(Out,L+4,G,{12,Size.Y-22},TEXT("Loading terrain detail…"),13,Ink);
        if(Overlay)
        {
            Line(Out,L+5,G,{{1,Size.Y-1},{1,1},{Size.X-1,1}},FLinearColor(.78f,.65f,.36f),1);
            Line(Out,L+5,G,{{Size.X-1,1},{Size.X-1,Size.Y-1},{1,Size.Y-1}},FLinearColor(.32f,.23f,.10f),1);
        }
        Out.PopClip();return SCompoundWidget::OnPaint(Args,G,Cull,Out,L+6,Style,Enabled);
    }
private:
    struct FMarker
    {
        FVector2D Point;FString Name;bool Portal=false,Detailed=false;
        mutable FVector2D LabelSize=FVector2D(-1,-1);
        mutable double LabelWidth=-1;
        mutable FString LabelText;
    };
    struct FFloor{TArray<FVector2D> Points;double Z=0;};
    struct FFill{FVector2D A,B,C;double Z=0;};
    struct FMapResult { FVector2D Point; FString Name; uint32 Landblock=0; };
    using FResult=TSharedPtr<FMapResult>;
    TWeakObjectPtr<UACEPluginSubsystem> Host;
    TSharedPtr<FACEPluginStyle> Retail;
    TStrongObjectPtr<UACEUIResourceResolver> Resources;TStrongObjectPtr<UTexture2D> MapTexture;FSlateBrush MapBrush;
    FACEWaypointTiles Tiles;
    TArray<FMarker> Markers;mutable TArray<int32> PaintedMarkers;TArray<FFloor> Floors;FACEDungeonMapContours Contours;TMap<uint32,FACEDatEnvironment> Environments;
    mutable TArray<ACEWaypoint::FMapLabel> PaintedLabels;
    TArray<ACEWaypoint::FMapCluster> PaintedClusters;
    TSharedPtr<SCanvas> ClusterButtons;
    int32 ClusterButtonCount=-1;
    FSlateRoundedBoxBrush ClusterBrush=FSlateRoundedBoxBrush(FLinearColor(.035f,.018f,.05f,.96f),12.f,FLinearColor(.85f,.5f,1),1.5f);
    TArray<FResult> Results,PressResults;
    TSharedPtr<SListView<FResult>> ResultsList;
    TSharedPtr<SBox> ResultsBox;
    TArray<FFill> Fill;TArray<ACEWaypoint::FRoutePoint> Route;double NextRoute=0;
    FACEPosition Player;FVector2D Pan=FVector2D::ZeroVector,Press,Last,Hover=FVector2D(-100,-100),LastSize=FVector2D(560,450);
    FVector2D MapUp=FVector2D(0,1);
    int32 PointerUser=0;uint32 PointerIndex=0;
    bool OwnsPointer(const FPointerEvent& E)const{return PointerUser==E.GetUserIndex()&&PointerIndex==E.GetPointerIndex();}
    double Zoom=1,NextMarkers=0,NextCellRetry=0,NextMapRetry=0;bool Dragged=false,PointerDown=false,FollowPlayer=true,Dungeon=false,WasInside=false,Overlay=false;FString Search;uint32 Landblock=0,CellIndex=0,CellCount=0;
    void PanPointer(FVector2D At)
    {
        if(Dragged||(At-Press).Size()>3)
        {Pan+=At-(Dragged?Last:Press);Dragged=true;FollowPlayer=false;}
        Last=At;
    }
    FVector2D Center()const{return Dungeon?ACEWaypoint::Coordinates(FACEPositionForCenter()):FVector2D::ZeroVector;}
    FACEPosition FACEPositionForCenter()const{FACEPosition P;P.CellId=int32(Landblock|0x100);P.Location=FVector(96,96,0);return P;}
    double PixelsPerUnit(FVector2D Size)const{return FMath::Min(Size.X,Size.Y)*Zoom/(Dungeon?1.5:204.);}
    FVector2D Project(FVector2D P,FVector2D Size)const{return Size*.5+Pan+ACEWaypoint::ToMap((P-Center())*PixelsPerUnit(Size),MapUp);}
    FVector2D Unproject(FVector2D P,FVector2D Size)const{return Center()+ACEWaypoint::FromMap((P-Size*.5-Pan)/PixelsPerUnit(Size),MapUp);}
    static FVector2D ClusterSize(const ACEWaypoint::FMapCluster& Cluster)
    {return {FMath::Max(24.,8.+FString::FromInt(Cluster.Indices.Num()).Len()*8.),24.};}
    int32 HitCluster(FVector2D At)const
    {
        int32 Best=INDEX_NONE;double Distance=MAX_dbl;
        for(int32 I=0;I<PaintedClusters.Num();++I)
        {
            const auto& Cluster=PaintedClusters[I];const auto Delta=At-Cluster.Point,Half=ClusterSize(Cluster)*.5;
            if(FMath::Abs(Delta.X)<=Half.X&&FMath::Abs(Delta.Y)<=Half.Y&&Delta.SizeSquared()<Distance)
            {Best=I;Distance=Delta.SizeSquared();}
        }
        return Best;
    }
    TArray<FResult> ClusterResults(int32 Index)const
    {
        TArray<FResult> Found;
        if(!PaintedClusters.IsValidIndex(Index))return Found;
        for(int32 Member:PaintedClusters[Index].Indices)if(Markers.IsValidIndex(Member))
        {const auto& M=Markers[Member];Found.Add(MakeShared<FMapResult>(FMapResult{M.Point,M.Name,Dungeon?Landblock:0}));}
        Found.Sort([](const FResult& A,const FResult& B)
        {const int32 Name=A->Name.Compare(B->Name);return Name?Name<0:A->Point.X!=B->Point.X?A->Point.X<B->Point.X:A->Point.Y<B->Point.Y;});
        return Found;
    }
    void CloseResults()
    {if(ResultsBox)ResultsBox->SetVisibility(EVisibility::Collapsed);}
    void ChooseResult(FResult Result)
    {if(Result&&Host.IsValid())Host->SetWaypoint(Result->Point,Result->Landblock);CloseResults();}
    void ShowResults(const TArray<FResult>& Found)
    {
        // Snapshot values, never indices into the live marker array, which is
        // refreshed while the player moves or the server sends new objects.
        Results=Found;ResultsList->RequestListRefresh();ResultsList->ScrollToTop();
        ResultsBox->SetVisibility(EVisibility::Visible);
    }
    void BuildResultsPanel()
    {
        auto Body=SNew(SVerticalBox);
        Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
            [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Bold",13)).AutoWrapText(true)
                .Text_Lambda([this]{return FText::FromString(FString::Printf(TEXT("(%d results returned)"),Results.Num()));})]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).ButtonStyle(&Retail->Button).ContentPadding(FMargin(6,3)).IsFocusable(false)
                .OnClicked_Lambda([this]{CloseResults();return FReply::Handled();})[WaypointLabel(TEXT("Close"),12)]]];
        Body->AddSlot().AutoHeight().Padding(0,3)[WaypointLabel(TEXT("Select a location to point the arrow."),12)];
        Body->AddSlot().FillHeight(1)[SAssignNew(ResultsList,SListView<FResult>).Tag(TEXT("Waypoint.ClusterResults"))
            .ListItemsSource(&Results).SelectionMode(ESelectionMode::None).ConsumeMouseWheel(EConsumeMouseWheel::Always)
            .OnGenerateRow_Lambda([this](FResult Result,const TSharedRef<STableViewBase>& Owner)
            {
                return SNew(STableRow<FResult>,Owner).Style(&Retail->ComboRow).Padding(FMargin(0,2))
                    [SNew(SButton).ButtonStyle(&Retail->Button).ContentPadding(FMargin(5,4)).IsFocusable(false)
                        .OnClicked_Lambda([this,Result]{ChooseResult(Result);return FReply::Handled();})
                        [SNew(SVerticalBox)
                            +SVerticalBox::Slot().AutoHeight()[WaypointLabel(Result->Name,13)]
                            +SVerticalBox::Slot().AutoHeight()[WaypointLabel(ACEWaypoint::Format(Result->Point),11)]]];
            })];
        ChildSlot[SNew(SOverlay).Visibility(EVisibility::SelfHitTestInvisible)
            // An actual input control keeps world selection/camera actions from
            // leaking through the map while the user pans its contents.
            +SOverlay::Slot()[SNew(SBorder).Tag(TEXT("Waypoint.PanSurface")).BorderImage(FCoreStyle::Get().GetBrush("NoBrush")).Padding(0)
                .Visibility(Overlay?EVisibility::Visible:EVisibility::HitTestInvisible)]
            +SOverlay::Slot()[SAssignNew(ClusterButtons,SCanvas).Visibility(EVisibility::SelfHitTestInvisible)]
            +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(8)
            [SAssignNew(ResultsBox,SBox).Visibility(EVisibility::Collapsed)
                .WidthOverride_Lambda([this]{return FMath::Max(0.,FMath::Min(360.,LastSize.X-16));})
                .HeightOverride_Lambda([this]{return FMath::Max(0.,FMath::Min(280.,LastSize.Y-16));})
                [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.025f,.02f,.015f,.98f)).Padding(8)[Body]]]];
    }
    void UpdateClusters(FVector2D Size)
    {
        PaintedMarkers.Reset();TArray<ACEWaypoint::FMapClusterPoint> Points;
        for(int32 I=0;I<Markers.Num();++I)
        {
            const auto& M=Markers[I];if(M.Detailed&&!M.Portal&&Zoom<3&&Search.IsEmpty())continue;
            const auto P=Project(M.Point,Size);if(P.X<0||P.Y<0||P.X>Size.X||P.Y>Size.Y)continue;
            PaintedMarkers.Add(I);Points.Add({I,P,M.Name});
        }
        PaintedClusters=ACEWaypoint::ClusterMarkers(MoveTemp(Points),28.);
        if(ClusterButtonCount==PaintedClusters.Num())return;
        ClusterButtonCount=PaintedClusters.Num();ClusterButtons->ClearChildren();
        // Real controls keep the dots independently accessible and preserve
        // their tooltips. Pinned-map preview handling distinguishes click/drag.
        for(int32 I=0;I<ClusterButtonCount;++I)
        {
            const TWeakPtr<SWaypointMap> WeakThis=SharedThis(this);
            ClusterButtons->AddSlot()
                .Position_Lambda([WeakThis,I]{const auto Map=WeakThis.Pin();return Map&&Map->PaintedClusters.IsValidIndex(I)?Map->PaintedClusters[I].Point-ClusterSize(Map->PaintedClusters[I])*.5:FVector2D::ZeroVector;})
                .Size_Lambda([WeakThis,I]{const auto Map=WeakThis.Pin();return Map&&Map->PaintedClusters.IsValidIndex(I)?ClusterSize(Map->PaintedClusters[I]):FVector2D::ZeroVector;})
                [SNew(SButton).Tag(TEXT("Waypoint.ClusterDot")).ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder"))
                    .ContentPadding(0).IsFocusable(false)
                    .Visibility_Lambda([WeakThis,I]
                    {const auto Map=WeakThis.Pin();if(!Map||!Map->PaintedClusters.IsValidIndex(I))return EVisibility::Collapsed;const auto P=Map->PaintedClusters[I].Point;return Map->Overlay&&Map->Host.IsValid()&&Map->Host->IsWaypointMapUnlocked()&&P.X>Map->LastSize.X-36&&P.Y>Map->LastSize.Y-36?EVisibility::HitTestInvisible:EVisibility::Visible;})
                    .ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([WeakThis,I]{const auto Map=WeakThis.Pin();return Map?Map->ClusterTooltip(I):FText::GetEmpty();})))
                    .OnHovered_Lambda([WeakThis,I]{if(const auto Map=WeakThis.Pin()){if(Map->PaintedClusters.IsValidIndex(I))Map->Hover=Map->PaintedClusters[I].Point;Map->Invalidate(EInvalidateWidgetReason::Paint);}})
                    .OnUnhovered_Lambda([WeakThis]{if(const auto Map=WeakThis.Pin()){Map->Hover=FVector2D(-100,-100);Map->Invalidate(EInvalidateWidgetReason::Paint);}})
                    .OnPressed_Lambda([WeakThis,I]{if(const auto Map=WeakThis.Pin())Map->PressResults=Map->ClusterResults(I);})
                    .OnClicked_Lambda([WeakThis]
                    {if(const auto Map=WeakThis.Pin()){if(Map->PressResults.Num()>1)Map->ShowResults(Map->PressResults);else if(Map->PressResults.Num()==1)Map->ChooseResult(Map->PressResults[0]);Map->PressResults.Reset();}return FReply::Handled();})];
        }
    }
    int32 HoveredMarker(FVector2D Size)const
    {
        int32 Best=INDEX_NONE;double Distance=14*14;
        for(int32 Index:PaintedMarkers)
        {
            if(!Markers.IsValidIndex(Index))continue;
            const double Next=(Project(Markers[Index].Point,Size)-Hover).SizeSquared();
            if(Next<Distance||(Next==Distance&&Best!=INDEX_NONE&&Markers[Index].Name<Markers[Best].Name))
            {Best=Index;Distance=Next;}
        }
        return Best;
    }
    FText MarkerTooltip()const
    {
        const int32 Cluster=HitCluster(Hover);
        if(Cluster!=INDEX_NONE)return ClusterTooltip(Cluster);
        const int32 Hovered=HoveredMarker(LastSize);
        if(Markers.IsValidIndex(Hovered))return FText::FromString(Markers[Hovered].Name);
        return FText::FromString(Overlay?TEXT("Drag to pan • Wheel to zoom • Center player resumes following • Unlock the UI to move with the top-left grip or resize from the bottom-right corner"):TEXT("Drag to pan • Wheel to zoom • Click a marker or map position to navigate"));
    }
    FText ClusterTooltip(int32 Index)const
    {
        if(!PaintedClusters.IsValidIndex(Index))return FText::GetEmpty();
        const auto& Cluster=PaintedClusters[Index];
        if(Cluster.Indices.Num()>1)return FText::FromString(FString::Printf(TEXT("%d results returned • Click to choose a location"),Cluster.Indices.Num()));
        return FText::FromString(Markers[Cluster.Indices[0]].Name);
    }
    void PaintFill(FSlateWindowElementList& Out,int32 L,const FGeometry& G)const
    {
        const auto Size=G.GetLocalSize();const FBox2D View(FVector2D::ZeroVector,Size);
        TArray<FSlateVertex> Vertices;TArray<SlateIndex> Indices;
        const auto Resource=FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FCoreStyle::Get().GetBrush("WhiteBrush"));
        auto Flush=[&]{if(Vertices.IsEmpty())return;FSlateDrawElement::MakeCustomVerts(Out,L,Resource,Vertices,Indices,nullptr,0,0);Vertices.Reset();Indices.Reset();};
        // Distant floors first; the current elevation stays legible where rooms overlap.
        for(bool Current:{false,true})
        {
            const FColor Color=(Current?FLinearColor(.055f,.24f,.34f):FLinearColor(.025f,.065f,.095f)).ToFColor(false);
            for(const auto& F:Fill)
            {
                if((FMath::Abs(F.Z-Player.Location.Z)<4)!=Current)continue;
                const FVector2D Points[]={Project(F.A,Size),Project(F.B,Size),Project(F.C,Size)};
                FBox2D Bounds(ForceInit);for(const auto& P:Points)Bounds+=P;if(!Bounds.Intersect(View))continue;
                if(Vertices.Num()+3>60000)Flush();
                for(const auto& P:Points){Indices.Add(SlateIndex(Vertices.Num()));Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),FVector2f(P),FVector2f(.5f,.5f),Color));}
            }
            Flush();
        }
    }
    void PaintRoute(FSlateWindowElementList& Out,int32 L,const FGeometry& G)const
    {
        const auto Size=G.GetLocalSize();TArray<FVector2D> Path;
        auto Flush=[&]{if(Path.Num()>1)Line(Out,L,G,Path,Accent,2);Path.Reset();};
        for(const auto& Point:Route)
        {
            const auto At=Project(ACEWaypoint::Coordinates(Point.Position),Size);
            if(!Point.Connect)Flush();Path.Add(At);
        }
        Flush();
        const FLinearColor Active(.3f,1.f,.45f);
        for(int32 I=0;I<Route.Num();++I)if(Route[I].Current)
        {
            const auto At=Project(ACEWaypoint::Coordinates(Route[I].Position),Size);
            if(I>0&&Route[I].Connect)Line(Out,L,G,{Project(ACEWaypoint::Coordinates(Route[I-1].Position),Size),At},Active,2.5f);
            TArray<FVector2D> Ring;for(int32 J=0;J<=20;++J){const double Angle=J*UE_DOUBLE_PI/10;Ring.Add(At+FVector2D(FMath::Cos(Angle),FMath::Sin(Angle))*7);}
            Line(Out,L,G,Ring,Active,2);Text(Out,L,G,At+FVector2D(10,6),FString::FromInt(Route[I].Index),12,Active);
        }
    }
    void PaintTile(FSlateWindowElementList& Out,int32 L,const FGeometry& G,const FBox2D& Bounds,const FSlateBrush& Brush)const
    {
        const auto Size=G.GetLocalSize();
        const FVector2D Corners[]={Project({Bounds.Min.X,Bounds.Max.Y},Size),Project(Bounds.Max,Size),Project({Bounds.Max.X,Bounds.Min.Y},Size),Project(Bounds.Min,Size)};
        FBox2D Screen(ForceInit);for(const auto& V:Corners)Screen+=V;
        if(!Screen.Intersect(FBox2D(FVector2D::ZeroVector,Size)))return;
        const FVector2f UV[]={{0,0},{1,0},{1,1},{0,1}};TArray<FSlateVertex> Vertices;Vertices.Reserve(4);
        for(int32 I=0;I<4;++I)Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),FVector2f(Corners[I]),UV[I],FColor(255,255,255,FMath::RoundToInt(255*(Overlay&&Host.IsValid()?Host->WaypointMapOpacity():1.f)))));
        const auto Resource=FSlateApplication::Get().GetRenderer()->GetResourceHandle(Brush);
        FSlateDrawElement::MakeCustomVerts(Out,L,Resource,Vertices,TArray<SlateIndex>{0,1,2,0,2,3},nullptr,0,0);
    }
    void RefreshMarkers()
    {
        Markers.Reset();auto* H=Host.Get();if(!H)return;
        if(!Dungeon && H->WaypointOption(TEXT("towns")))for(const auto& Town:ACEWaypointTowns::Locations)
            Markers.Add({{Town.EastWest,Town.NorthSouth},Town.Name,false});
        // Preserve the three regional island labels; these artwork anchors
        // describe whole islands, unlike the precise town destinations above.
        if(!Dungeon && H->WaypointOption(TEXT("towns")))for(const auto& Region:ACERetailMap::Locations)
            if(FString(Region.Name).EndsWith(TEXT("Island")))
                Markers.Add({{(Region.X+Region.Width*.5)/257.*204.-101.95,102.05-(Region.Y+Region.Height*.5)/267.*204.},Region.Name,false});
        if(!Dungeon)for(const auto& M:H->GetWaypointLocations())
        {
            const bool Portal=M.Type.Contains(TEXT("Portal"))||M.Type==TEXT("Dungeon");
            const bool Town=M.Type==TEXT("Town");
            if(H->WaypointOption(Portal?TEXT("portals"):Town?TEXT("towns"):TEXT("pois")))
            {
                // Imported atlases may override a town (including custom-server locations).
                if(Town)Markers.RemoveAll([&](const FMarker& Existing){return Existing.Name.Equals(M.Name,ESearchCase::IgnoreCase);});
                Markers.Add({M.Coordinates,M.Name,Portal,!Town});
            }
        }
        if(H->WaypointOption(TEXT("portals")))if(auto* C=H->GetGameInstance()->GetSubsystem<UACEClientSubsystem>())if(auto Session=C->GetSession())for(const auto& Pair:Session->GetWorldObjects())
        {
            const auto& O=Pair.Value;
            if(!O.Position.IsValid() || !(O.ItemType&ACEItemType::Portal))continue;
            const bool Indoor=(uint32(O.Position.CellId)&65535)>=256;
            if(Dungeon ? !Indoor||(uint32(O.Position.CellId)&0xFFFF0000)!=Landblock : Indoor)continue;
            Markers.Add({ACEWaypoint::Coordinates(O.Position),O.Name,true});
        }
        if(!Search.IsEmpty())Markers.RemoveAll([this](const FMarker& M){return !M.Name.Contains(Search,ESearchCase::IgnoreCase);});
    }
    void LoadDungeonCells()
    {
        auto* Dat=Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>();if(!Dat || !Dat->GetPortalDat())return;
        // Only read geometry. Do not spawn world meshes, textures, collision or actors for the map.
        const double Until=FPlatformTime::Seconds()+.002;
        for(int N=0;N<2 && CellIndex<CellCount && FPlatformTime::Seconds()<Until;++N)
        {
            FACEDatEnvCell Cell;if(!Dat->LoadEnvCell(Landblock|(0x100+CellIndex++),Cell))continue;
            if(!Environments.Contains(Cell.EnvironmentId))
            {
                TArray<uint8> Blob;FACEDatEnvironment Env;
                if(!Dat->GetPortalDat()->ReadFile(Cell.EnvironmentId,Blob))continue;FACEDatCursor Cursor(Blob);
                if(!ACEDatUnpack::UnpackEnvironment(Cursor,Env))continue;Environments.Add(Cell.EnvironmentId,MoveTemp(Env));
            }
            const auto* Structure=Environments[Cell.EnvironmentId].Cells.Find(Cell.CellStructure);if(!Structure)continue;
            // Match the actual collision mesh: unused physics faces and draw-only
            // portal planes are not walls and must not close walkable connections.
            for(uint16 Id:Structure->PhysicsCollisionPolyIds)
            {
                const auto* Poly=Structure->PhysicsPolygons.Find(Id);if(!Poly)continue;
                TArray<FVector> Points;
                for(auto Vertex:Poly->VertexIds)if(const auto* V=Structure->Vertices.Find(uint16(Vertex)))Points.Add(FVector(Cell.Orientation.RotateVector(V->Origin)+Cell.Origin));
                if(Points.Num()!=Poly->VertexIds.Num())continue;
                const int32 First=Contours.Triangles.Num();Contours.AddPolygon(Points);
                auto Coordinate=[&](FVector V){FACEPosition P;P.CellId=int32(Landblock|0x100);P.Location=V;return ACEWaypoint::Coordinates(P);};
                for(int32 I=First;I<Contours.Triangles.Num();++I)
                {const auto& T=Contours.Triangles[I];Fill.Add({Coordinate(T.A),Coordinate(T.B),Coordinate(T.C),(T.A.Z+T.B.Z+T.C.Z)/3});}
            }
        }
    }
};

class SWaypointWheelArea : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWaypointWheelArea){}
        SLATE_EVENT(FPointerEventHandler,OnMouseWheel)
        SLATE_DEFAULT_SLOT(FArguments,Content)
    SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {WheelHandler=A._OnMouseWheel;SetVisibility(EVisibility::SelfHitTestInvisible);ChildSlot[A._Content.Widget];}
    FReply OnMouseWheel(const FGeometry& G,const FPointerEvent& E)override
    {return WheelHandler.IsBound()?WheelHandler.Execute(G,E):FReply::Unhandled();}
private:FPointerEventHandler WheelHandler;
};

class SWaypointControlIcon : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SWaypointControlIcon):_Grip(false){} SLATE_ARGUMENT(bool,Grip) SLATE_END_ARGS()
    void Construct(const FArguments& A){Grip=A._Grip;SetVisibility(EVisibility::HitTestInvisible);}
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(14,14);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& Style,bool Enabled)const override
    {
        const FVector2D Center=G.GetLocalSize()*.5;
        const auto Color=Ink*Style.GetColorAndOpacityTint();
        if(Grip)
        {
            for(double X:{-3.,3.})for(double Y:{-4.,0.,4.})
                FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(FVector2D(2,2),FSlateLayoutTransform(Center+FVector2D(X,Y)-FVector2D(1,1))),
                    FCoreStyle::Get().GetBrush("WhiteBrush"),Enabled?ESlateDrawEffect::None:ESlateDrawEffect::DisabledEffect,Color);
        }
        else
        {
            const double Radius=FMath::Min(G.GetLocalSize().X,G.GetLocalSize().Y)*.3;
            TArray<FVector2D> Ring;for(int32 I=0;I<=20;++I){const double A=I*UE_DOUBLE_PI/10;Ring.Add(Center+FVector2D(FMath::Cos(A),FMath::Sin(A))*Radius);}
            Line(Out,Layer,G,Ring,Color,1.25f);
            for(const FVector2D Axis:{FVector2D(1,0),FVector2D(-1,0),FVector2D(0,1),FVector2D(0,-1)})
                Line(Out,Layer,G,{Center+Axis*Radius*.65,Center+Axis*Radius*1.6},Color,1.25f);
        }
        return Layer;
    }
private:bool Grip=false;
};

class SWaypointPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWaypointPanel):_ShowTitle(true){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(bool,Map) SLATE_ARGUMENT(bool,ShowTitle) SLATE_END_ARGS()
    void Construct(const FArguments& A)
    {
        Host=A._Host;const auto PanelStyle=FACEPluginStyle::Use(A._Host,A._Map?TEXT("waypoint.map"):TEXT("waypoint"));
        ShowTitle=A._ShowTitle;
        auto Body=SNew(SVerticalBox);if(ShowTitle)Body->AddSlot().AutoHeight()[WaypointLabel(A._Map?TEXT("Explore Dereth"):TEXT("Waypoint"),26)];
        auto Input=SNew(SHorizontalBox);
        Input->AddSlot().FillWidth(1)[SAssignNew(Entry,SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",18)).HintText(FText::FromString(TEXT("42.0N, 33.6E")))
            .OnTextCommitted_Lambda([this](const FText& T,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter)Navigate(T.ToString());})];
        Input->AddSlot().AutoWidth().Padding(8,0)[WaypointButton(TEXT("Go"),[this]{Navigate(Entry->GetText().ToString());})];
        Input->AddSlot().AutoWidth().Padding(4,0)[WaypointButton(TEXT("Clear"),[this]{if(Host.IsValid())Host->ClearWaypoint();})];
        Body->AddSlot().AutoHeight().Padding(0,12)[Input];
        if(A._Map)
        {
            auto Controls=SNew(SHorizontalBox);
            Controls->AddSlot().AutoWidth()[Check(TEXT("dungeon"),TEXT("Dungeon"),false)];
            Controls->AddSlot().AutoWidth()[Check(TEXT("towns"),TEXT("Towns"),true)];
            Controls->AddSlot().AutoWidth()[Check(TEXT("portals"),TEXT("Portals"),true)];
            Controls->AddSlot().AutoWidth()[Check(TEXT("pois"),TEXT("POIs"),true)];
            Body->AddSlot().AutoHeight()[Controls];
            Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[Check(TEXT("fill_dungeon"),TEXT("Fill walkable areas"),false)]
                +SHorizontalBox::Slot().AutoWidth()[Check(TEXT("show_route"),TEXT("Show active route"),true)]];
            Body->AddSlot().AutoHeight().Padding(0,6)[Check(TEXT("heading_up"),TEXT("Player facing up (off: north up)"),false)];
            Body->AddSlot().AutoHeight()[Check(TEXT("dungeon_overlay"),TEXT("Pin map to interface"),false)];
            Body->AddSlot().AutoHeight()[Check(TEXT("map_minimized"),TEXT("Minimize pinned map (keep direction arrow)"),false)];
            Body->AddSlot().AutoHeight().Padding(0,6)[WaypointLabel(TEXT("Map background opacity"),14)];
            Body->AddSlot().AutoHeight()[SNew(SSlider).Style(&PanelStyle->Slider).MinValue(0.f).MaxValue(1.f)
                .IsEnabled_Lambda([this](){return Host.IsValid()&&!Host->IsPluginOpacityGlobal();})
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Shared with Waypoint Map background opacity in Plugin Manager. The global plugin opacity overrides this setting."))))
                .Value_Lambda([this](){return Host.IsValid()?Host->WaypointMapOpacity():1.f;})
                .OnValueChanged_Lambda([this](float Value){if(Host.IsValid())Host->SetWaypointMapOpacity(Value);})];
            Body->AddSlot().AutoHeight()[WaypointLabel(TEXT("The pinned map follows you and switches between overworld and interior layouts. Drag to pan or use the wheel to zoom, even while locked; the center-player icon resumes following. Click a numbered dot to choose a location. Unlock the UI to move using the top-left grip or resize. Uncheck Pin to hide it."),13)];
            Body->AddSlot().AutoHeight().Padding(0,6)[SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",16)).HintText(FText::FromString(TEXT("Filter locations by name"))).OnTextChanged_Lambda([this](const FText& T){if(Map)Map->SetSearch(T.ToString());})];
            Body->AddSlot().FillHeight(1).Padding(0,10)[SAssignNew(Map,SWaypointMap).Host(Host.Get())];
            Body->AddSlot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[WaypointButton(TEXT("+"),[this]{Map->ChangeZoom(1.5,Map->GetCachedGeometry().GetLocalSize()*.5);})]
                +SHorizontalBox::Slot().AutoWidth().Padding(5,0)[WaypointButton(TEXT("−"),[this]{Map->ChangeZoom(1/1.5,Map->GetCachedGeometry().GetLocalSize()*.5);})]
                +SHorizontalBox::Slot().AutoWidth()[WaypointButton(TEXT("Center on me"),[this]{Map->CenterOnPlayer();})]
                +SHorizontalBox::Slot().AutoWidth().Padding(5,0)[WaypointButton(TEXT("Reset view"),[this]{Map->ResetView();})]];
            Body->AddSlot().AutoHeight().Padding(0,8)[WaypointButton(TEXT("Waypoint settings"),[this]{if(Host.IsValid())ChildSlot[Host->MakeWaypointPanel(false,false,ShowTitle)];})];
            Body->AddSlot().AutoHeight().Padding(0,8)[WaypointLabel(TEXT("Drag to pan · Scroll to zoom · Click to navigate\nTowns and nearby portals. Atlas detail appears when zoomed in or searched. Dungeon floors near your elevation are brighter."),13)];
        }
        else
        {
            Body->AddSlot().AutoHeight()[WaypointLabel(TEXT("GoArrow / CoD atlas"),18)];
            Body->AddSlot().AutoHeight()[SAssignNew(AtlasPath,SACEPluginEditableTextBox).Tag(TEXT("Waypoint.AtlasPath")).Style(&PanelStyle->Edit).Font(FACEPluginStyle::NativeFont("Regular",16)).HintText(FText::FromString(TEXT("Full path to GoArrow or CoD XML")))];
            Body->AddSlot().AutoHeight().Padding(0,3)[WaypointLabel(TEXT("Browse for a GoArrow or CoD XML file, then choose Import locations."),13)];
            auto Browse=WaypointButton(TEXT("Browse locations"),[this]{BrowseLocations();});
            Browse->SetTag(TEXT("Waypoint.BrowseLocations"));
            Browse->SetEnabled(TAttribute<bool>::CreateLambda([](){return !ACEWaypointFilePicker::IsOpen();}));
            Browse->SetToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Choose an XML file from your computer. Import locations loads the selected file."))));
            auto Import=WaypointButton(TEXT("Import locations"),[this]
            {
                if(!Host.IsValid())return;
                const bool Imported=Host->ImportWaypointLocations(AtlasPath->GetText().ToString());
                Status=Host->Notice;
                if(Imported)OpenAtlas(Status);
            });
            Import->SetTag(TEXT("Waypoint.ImportLocations"));
            Body->AddSlot().AutoHeight().Padding(0,4)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[Import]
                +SHorizontalBox::Slot().AutoWidth().Padding(4,0)[Browse]];
            auto ViewImported=WaypointButton(TEXT("View imported locations"),[this]{OpenAtlas(FString());});
            ViewImported->SetTag(TEXT("Waypoint.ViewImportedLocations"));
            Body->AddSlot().AutoHeight().HAlign(HAlign_Left).Padding(0,4)
                [ViewImported];
            Body->AddSlot().AutoHeight()[SNew(SACEPluginText).Tag(TEXT("Waypoint.ImportStatus")).AutoWrapText(true).ColorAndOpacity(Accent)
                .Text_Lambda([this]{return FText::FromString(Status);})];
            Body->AddSlot().AutoHeight()[SNew(SWaypointArrow).Host(Host.Get())];
            Body->AddSlot().AutoHeight().Padding(0,8)[Check(TEXT("distance"),TEXT("Show distance to destination"),true)];
            Body->AddSlot().AutoHeight()[Check(TEXT("arrow"),TEXT("Show floating arrow"),true)];
            Body->AddSlot().AutoHeight().Padding(0,8)[WaypointLabel(TEXT("Use the game UI lock or hold Quick-show pinned windows to move the arrow."),13)];
            Body->AddSlot().AutoHeight()[WaypointButton(TEXT("Open map"),[this]{if(Host.IsValid())ChildSlot[Host->MakeWaypointPanel(true,false,ShowTitle)];})];
            Body->AddSlot().AutoHeight().Padding(0,12)[WaypointLabel(TEXT("Click coordinates in chat or enter them above. The arrow shows your current coordinates. Unlock the game UI and drag its title to move it; its position is saved."),15)];
        }
        if(A._Map)Body->AddSlot().AutoHeight()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",14)).ColorAndOpacity(Accent).AutoWrapText(true)
            .Text_Lambda([this]{return FText::FromString(Status);})];
        TSharedRef<SWidget> Content=Body;
        if(!A._Map)Content=SNew(SScrollBox)+SScrollBox::Slot()[Body];
        SettingsContent=SNew(SBorder).BorderImage(&PanelStyle->Background).Padding(18)[Content];
        ChildSlot[SettingsContent.ToSharedRef()];
    }
private:
    TWeakObjectPtr<UACEPluginSubsystem> Host;TSharedPtr<SEditableTextBox> Entry,AtlasPath;TSharedPtr<SWaypointMap> Map;FString Status;bool ShowTitle=true;
    TSharedPtr<SWidget> SettingsContent;
    void BrowseLocations()
    {
        if(!Host.IsValid()||!AtlasPath)return;
        const TWeakPtr<SWaypointPanel> Weak=SharedThis(this);
        if(!ACEWaypointFilePicker::Open(AtlasPath->GetText().ToString(),[Weak](const FString& Path,const FString& Error,bool Cancelled)
        {
            const auto Panel=Weak.Pin();
            if(!Panel||!Panel->Host.IsValid()||!Panel->AtlasPath||Cancelled)return;
            if(!Error.IsEmpty()){Panel->Status=Error;return;}
            Panel->AtlasPath->SetText(FText::FromString(Path));
            Panel->Status=TEXT("XML file selected. Choose Import locations to load it.");
        }))Status=TEXT("An XML file picker is already open.");
    }
    void OpenAtlas(const FString& Message)
    {
        if(Host.IsValid())ChildSlot[MakeWaypointAtlas(Host.Get(),Message,FSimpleDelegate::CreateSP(this,&SWaypointPanel::BackToSettings))];
    }
    void BackToSettings(){ChildSlot[SettingsContent.ToSharedRef()];}
    void Navigate(const FString& S){if(Host.IsValid())Status=Host->SetWaypoint(S)?TEXT("Destination set"):Host->Notice;}
    TSharedRef<SWidget> Check(const FString& Key,const FString& Caption,bool Default)
    {return SNew(SCheckBox).Tag(Key==TEXT("arrow")?FName(TEXT("Waypoint.ShowFloatingArrow")):NAME_None).Style(&FACEPluginStyle::Current()->Check).Padding(6).IsChecked_Lambda([this,Key,Default]{return Host.IsValid()&&Host->WaypointOption(Key,Default)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this,Key](ECheckBoxState V){if(Host.IsValid()&&!Host->SetWaypointOption(Key,V==ECheckBoxState::Checked))Status=Host->Notice;})[WaypointLabel(Caption)];}
};
}
TSharedRef<SWidget> UACEPluginSubsystem::MakeWaypointPanel(bool bMap,bool bArrowOnly,bool bShowTitle)
{if(bArrowOnly)return SNew(SWaypointArrow).Host(this);return SNew(SWaypointPanel).Host(this).Map(bMap).ShowTitle(bShowTitle);}
TSharedRef<SWidget> UACEPluginSubsystem::MakeWaypointMapIcon(){return SNew(SWaypointMapIcon);}
TSharedRef<SWidget> UACEPluginSubsystem::MakeWaypointDungeonOverlay()
{
    const TWeakObjectPtr<UACEPluginSubsystem> H(this);
    const auto Map=SNew(SWaypointMap).Host(this).Overlay(true)
        .Visibility_Lambda([H](){return !H.IsValid()||H->WaypointOption(TEXT("map_minimized"),false)?EVisibility::Collapsed:H->IsWaypointMapUnlocked()?EVisibility::Visible:EVisibility::SelfHitTestInvisible;});
    const auto Style=FACEPluginStyle::Current();
    const auto ZoomWithWheel=[H,Map](const FGeometry&,const FPointerEvent& Event)
    {
        if(H.IsValid()&&!H->WaypointOption(TEXT("map_minimized"),false))
            return Map->OnMouseWheel(Map->GetCachedGeometry(),Event);
        return FReply::Handled();
    };
    auto ZoomButton=[H,Map,Style](const TCHAR* Symbol,const TCHAR* Tooltip,double Factor)->TSharedRef<SWidget>
    {
        return SNew(SBox).WidthOverride(26).HeightOverride(26)
            .Visibility_Lambda([H](){return !H.IsValid()||H->WaypointOption(TEXT("map_minimized"),false)?EVisibility::Collapsed:EVisibility::Visible;})
            [SNew(SACEPluginButton).ContentPadding(FMargin(0)).IsFocusable(false).ButtonStyle(&Style->DockButton)
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Tooltip)))
                .OnClicked_Lambda([Map,Factor](){Map->ChangeZoom(Factor,Map->GetCachedGeometry().GetLocalSize()*.5);return FReply::Handled();})
                [SNew(STextBlock).Font(FACEPluginStyle::NativeFont("Bold",16)).ColorAndOpacity(Ink).Text(FText::FromString(Symbol))]];
    };
    return SNew(SWaypointWheelArea).OnMouseWheel_Lambda(ZoomWithWheel)[SNew(SOverlay)
        +SOverlay::Slot()[Map]
        +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top)
        [SNew(SBox).WidthOverride(26).HeightOverride(26)
            .Visibility_Lambda([H](){return H.IsValid()&&!H->WaypointOption(TEXT("map_minimized"),false)&&H->CanManipulatePluginWindow(TEXT("waypoint.dungeon"))?EVisibility::Visible:EVisibility::Collapsed;})
            [SNew(SBorder).Tag(TEXT("Waypoint.MoveGrip")).BorderImage(&Style->DockButton.Normal).Padding(5).Cursor(EMouseCursor::CardinalCross)
                .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Drag to move minimap"))))
                [SNew(SWaypointControlIcon).Grip(true)]]]
        +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top)
        [SNew(SBorder).Tag(TEXT("Waypoint.ZoomControls")).BorderImage(FCoreStyle::Get().GetBrush("NoBrush")).Padding(0)
        [SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[ZoomButton(TEXT("+"),TEXT("Zoom in"),1.25)]
            +SHorizontalBox::Slot().AutoWidth()[ZoomButton(TEXT("−"),TEXT("Zoom out"),1/1.25)]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(26).HeightOverride(26)
                .Visibility_Lambda([H](){return !H.IsValid()||H->WaypointOption(TEXT("map_minimized"),false)?EVisibility::Collapsed:EVisibility::Visible;})
                [SNew(SACEPluginButton).Tag(TEXT("Waypoint.CenterPlayer")).ContentPadding(FMargin(FACEPluginStyle::IconButtonPadding)).IsFocusable(false).ButtonStyle(&Style->DockButton)
                    .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("center player"))))
                    .OnClicked_Lambda([Map](){Map->CenterOnPlayer();return FReply::Handled();})
                    [SNew(SWaypointControlIcon)]]]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).HeightOverride(26)
                [SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).IsFocusable(false).ButtonStyle(&Style->DockButton)
                    .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Minimize / restore map; direction arrow stays visible"))))
                    .OnClicked_Lambda([H,Map](){Map->CancelPan();if(H.IsValid())H->SetWaypointOption(TEXT("map_minimized"),!H->WaypointOption(TEXT("map_minimized"),false));return FReply::Handled();})
                    [SNew(SACEPluginText).Text_Lambda([H](){return FText::FromString(H.IsValid()&&H->WaypointOption(TEXT("map_minimized"),false)?TEXT("Map +"):TEXT("Map −"));})]]]]]];
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Widgets/SVirtualWindow.h"
#include "Input/HittestGrid.h"
#include "Types/PaintArgs.h"
#include "Widgets/IToolTip.h"
#include "Widgets/Layout/SDPIScaler.h"
namespace
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEWaypointOverlayTest,"ACE.Plugins.Waypoint.PinnedDungeon",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FACEWaypointOverlayTest::RunTest(const FString&)
{
    auto* GI=NewObject<UGameInstance>();GI->Init();
    ON_SCOPE_EXIT{GI->Shutdown();};
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    auto* C=GI->GetSubsystem<UACEClientSubsystem>();
    const bool OriginalGlobal=Host->IsPluginOpacityGlobal();
    Host->SetPluginOpacityGlobal(false);
    const bool HadMapOpacity=Host->HasPluginWindowOpacity(TEXT("waypoint.map"));
    const float OriginalOpacity=Host->WaypointMapOpacity(),OriginalGlobalOpacity=Host->GetPluginWindowOpacity(TEXT(""));
    ON_SCOPE_EXIT
    {
        Host->SetPluginOpacityGlobal(false);
        if(HadMapOpacity)Host->SetWaypointMapOpacity(OriginalOpacity);else Host->ResetPluginWindowOpacity(TEXT("waypoint.map"));
        Host->SetPluginWindowOpacity(TEXT(""),OriginalGlobalOpacity);
        Host->SetPluginOpacityGlobal(OriginalGlobal);
    };
    Host->SetWaypointMapOpacity(1.f);
    double OriginalZoom[2][2];
    for(int O=0;O<2;++O)for(int D=0;D<2;++D)
    {OriginalZoom[O][D]=Host->WaypointMapZoom(O!=0,D!=0);Host->SetWaypointMapZoom(O!=0,D!=0,DefaultWaypointZoom(O!=0,D!=0));}
    ON_SCOPE_EXIT{for(int O=0;O<2;++O)for(int D=0;D<2;++D)Host->SetWaypointMapZoom(O!=0,D!=0,OriginalZoom[O][D]);};
    TSharedPtr<FACEClientPlugin> Plugin;
    for(const auto& Entry:Host->Plugins)if(Entry->Id==TEXT("waypoint"))Plugin=Entry;
    if(!Plugin||!C->GetSession())return false;
    Plugin->Profile=MakeShared<FJsonObject>();
    auto Map=SNew(SWaypointMap).Host(Host).Overlay(true);
    Plugin->Profile->SetBoolField(TEXT("towns"),true);
    for(const auto& Town: {ACEWaypointTowns::FTown{TEXT("Mayoi"),82.5,-61.9},ACEWaypointTowns::FTown{TEXT("Wai Jhou"),-51.4,-62.0}})
    {
        const double X=(Town.EastWest+101.95)*240.,Y=(Town.NorthSouth+101.95)*240.;
        FACEPosition P;P.CellId=int32((uint32(X/192)<<24)|(uint32(Y/192)<<16)|1);
        P.Location=FVector(FMath::Fmod(X,192.),FMath::Fmod(Y,192.),0);
        C->GetSession()->SetLocalPosition(P);
        const FVector2D Size(420,420);
        Map->Tick(FGeometry::MakeRoot(Size,FSlateLayoutTransform()),0,.016f);Map->RefreshMarkers();
        const auto* Marker=Map->Markers.FindByPredicate([&](const auto& M){return M.Name==Town.Name;});
        TestTrue(FString::Printf(TEXT("%s marker meets the player at the geographic town center"),Town.Name),
            Marker && Map->Project(Marker->Point,Size).Equals(Size*.5,.01));
    }
    double Time=1;
    for(bool Facing:{false,true})for(int Width:{260,560})for(uint32 Cell:{0x7D640014u,0x01430171u,0x01430172u,0x7D640100u,0x7D64001Au})
    {
        FACEPosition P;P.CellId=int32(Cell);P.Location=FVector(49+Time,-75+Time,0);
        P.RotationW=FMath::Cos(.4);P.RotationXYZ=FVector(0,0,FMath::Sin(.4));
        C->GetSession()->SetLocalPosition(P);Plugin->Profile->SetBoolField(TEXT("heading_up"),Facing);
        const FVector2D Size(Width,320);const auto Geometry=FGeometry::MakeRoot(Size,FSlateLayoutTransform());
        Map->Tick(Geometry,Time++,.016f);
        Plugin->Profile->SetBoolField(TEXT("dungeon"),!((Cell&65535)>=256));
        Map->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Pinned map automatically chooses world or interior independently of normal map"),Map->Dungeon,(Cell&65535)>=256);
        TestTrue(TEXT("Player stays at center after movement, turn, resize and landblock change"),Map->Project(ACEWaypoint::Coordinates(P),Size).Equals(Size*.5,.001));
        const auto Forward=P.GetAceForwardInAcSpace();
        TestTrue(TEXT("Pinned orientation obeys the shared setting"),Map->MapUp.Equals(Facing?FVector2D(Forward.X,Forward.Y).GetSafeNormal():FVector2D(0,1)));
        Map->ChangeZoom(1.25,{25,50});
        TestTrue(TEXT("Off-center wheel zoom cannot displace the tracked player"),Map->Project(ACEWaypoint::Coordinates(P),Size).Equals(Size*.5,.001));
    }
    {
        const double WorldZoom=Map->Zoom;
        auto P=C->GetPlayerPosition();P.CellId=0x01430171;C->GetSession()->SetLocalPosition(P);
        const auto Geometry=FGeometry::MakeRoot(FVector2D(420,420),FSlateLayoutTransform());
        Map->Tick(Geometry,Time++,.016f);Map->ChangeZoom(.8,{40,40});const double InteriorZoom=Map->Zoom;
        P.CellId=0x7D640014;C->GetSession()->SetLocalPosition(P);Map->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Returning outside restores overworld zoom"),Map->Zoom,WorldZoom);
        TestTrue(TEXT("Leaving interior discards stale floor geometry"),Map->Floors.IsEmpty()&&Map->CellCount==0);
        P.CellId=0x01430171;C->GetSession()->SetLocalPosition(P);Map->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Returning inside restores interior zoom"),Map->Zoom,InteriorZoom);
        auto Reopened=SNew(SWaypointMap).Host(Host).Overlay(true);
        TestEqual(TEXT("Opening indoors uses saved zoom even before the first tick"),Reopened->Zoom,InteriorZoom);
        Reopened->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Reopening pinned map indoors restores zoom"),Reopened->Zoom,InteriorZoom);
        P.CellId=0x7D640014;C->GetSession()->SetLocalPosition(P);Reopened->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Reopened map retains separate outdoor zoom"),Reopened->Zoom,WorldZoom);
        Plugin->Profile->SetBoolField(TEXT("dungeon"),false);
        auto Window=SNew(SWaypointMap).Host(Host);Window->Tick(Geometry,Time++,.016f);Window->ChangeZoom(2,{40,40});
        auto ReopenedWindow=SNew(SWaypointMap).Host(Host);ReopenedWindow->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Normal map also retains its own zoom"),ReopenedWindow->Zoom,2.);
        Window->Tick(Geometry,Time++,.016f);const auto Center=Window->Unproject(Geometry.GetLocalSize()*.5,Geometry.GetLocalSize());
        ReopenedWindow->ChangeZoom(1.25,{40,40});Window->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("An already-open view follows the latest saved zoom"),Window->Zoom,2.5);
        TestTrue(TEXT("Restoring a changed zoom keeps the panned center"),Window->Unproject(Geometry.GetLocalSize()*.5,Geometry.GetLocalSize()).Equals(Center,.0001));
        TestEqual(TEXT("Normal map does not overwrite pinned zoom"),Host->WaypointMapZoom(true,false),WorldZoom);
        ReopenedWindow->ResetView();
        TestEqual(TEXT("Explicit Reset view saves the default zoom"),Host->WaypointMapZoom(false,false),1.);
        TestEqual(TEXT("Reset view preserves the other map's zoom"),Host->WaypointMapZoom(true,false),WorldZoom);
    }
    for(bool Pinned:{false,true})
    {
        const auto Geometry=FGeometry::MakeRoot(FVector2D(420,420),FSlateLayoutTransform());
        auto P=C->GetPlayerPosition();P.CellId=0x7D640014;C->GetSession()->SetLocalPosition(P);
        Plugin->Profile->SetBoolField(TEXT("dungeon"),false);
        auto ZoomMap=SNew(SWaypointMap).Host(Host).Overlay(Pinned);ZoomMap->Tick(Geometry,Time++,.016f);
        ZoomMap->ChangeZoom(10000,{210,210});
        TestEqual(TEXT("Overworld allows eight times the previous maximum zoom"),ZoomMap->Zoom,384.);
        P.CellId=0x01430171;C->GetSession()->SetLocalPosition(P);Plugin->Profile->SetBoolField(TEXT("dungeon"),true);
        ZoomMap->Tick(Geometry,Time++,.016f);ZoomMap->ChangeZoom(10000,{210,210});
        TestEqual(TEXT("Interior zoom keeps its separate 48x limit"),ZoomMap->Zoom,48.);
        auto Reopened=SNew(SWaypointMap).Host(Host).Overlay(Pinned);Reopened->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Reopening indoors preserves the interior maximum"),Reopened->Zoom,48.);
        P.CellId=0x7D640014;C->GetSession()->SetLocalPosition(P);Plugin->Profile->SetBoolField(TEXT("dungeon"),false);
        Reopened->Tick(Geometry,Time++,.016f);
        TestEqual(TEXT("Reopening then returning outside restores deep overworld zoom"),Reopened->Zoom,384.);
        for(bool Interior:{false,true})Host->SetWaypointMapZoom(Pinned,Interior,DefaultWaypointZoom(Pinned,Interior));
    }
    if(FApp::CanEverRender())
    {
        const FVector2D ArrowSize(290,190);FWidgetRenderer Renderer(false,true);
        auto Arrow=SNew(SWaypointArrow).Host(Host);auto* Target=FWidgetRenderer::CreateTargetFor(ArrowSize,TF_Bilinear,true);
        TArray<FColor> First;
        for(int32 Turn=0;Turn<2;++Turn)
        {
            FACEPosition P;P.CellId=0x01430171;P.Location=FVector(49,-75,0);
            P.RotationW=FMath::Cos(Turn*UE_DOUBLE_PI/4);P.RotationXYZ=FVector(0,0,FMath::Sin(Turn*UE_DOUBLE_PI/4));
            C->GetSession()->SetLocalPosition(P);
            ++GFrameCounter;Renderer.DrawWidget(Target,Arrow,ArrowSize,0);FlushRenderingCommands();
            TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            if(Turn==0)First=Pixels;
            else
            {
                int Changed=0;for(int Y=25;Y<110;++Y)for(int X=90;X<200;++X)if(Pixels[Y*290+X]!=First[Y*290+X])++Changed;
                TestTrue(TEXT("Floating north arrow visibly rotates when turning in a dungeon without a destination"),Changed>100);
            }
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(290,190,Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-Compass-%d.png"),Turn)));
        }
        Target->ReleaseResource();
    }
    if(FApp::CanEverRender())
    {
        // Check the text actually painted by the arrow, including destinations
        // persisted by the old Third Eye action with a short landblock ID.
        const auto OriginalProfile=Plugin->Profile;const auto OriginalPosition=C->GetPlayerPosition();
        Plugin->Profile=MakeShared<FJsonObject>(*OriginalProfile);
        ON_SCOPE_EXIT{Plugin->Profile=OriginalProfile;C->GetSession()->SetLocalPosition(OriginalPosition);};
        Plugin->Profile->SetBoolField(TEXT("distance"),true);
        const FVector2D Size(290,190);const auto Geometry=FGeometry::MakeRoot(Size,FSlateLayoutTransform());
        auto Arrow=SNew(SWaypointArrow).Host(Host);auto Window=SNew(SVirtualWindow).Size(Size);FHittestGrid Grid;
        auto DrawText=[&]
        {
            FSlateWindowElementList Elements(Window);
            Arrow->OnPaint(FPaintArgs(&Arrow.Get(),Grid,FVector2D::ZeroVector,0,0),Geometry,FSlateRect(0,0,Size.X,Size.Y),Elements,0,FWidgetStyle(),true);
            TArray<FString> Drawn;
            for(const auto& Element:Elements.GetUncachedDrawElements().Get<uint8(EElementType::ET_Text)>())Drawn.Add(Element.GetText());
            return Drawn;
        };
        for(uint32 Landblock:{0x01430000u,0xF1430000u})for(bool Legacy:{false,true})
        {
            FACEPosition Player;Player.CellId=int32(Landblock|0x100);Player.Location=FVector(49,75,0);
            C->GetSession()->SetLocalPosition(Player);
            const FVector2D Destination=ACEWaypoint::Coordinates(Player)+FVector2D(10./240.,0);
            Plugin->Profile->SetNumberField(TEXT("east"),Destination.X);Plugin->Profile->SetNumberField(TEXT("north"),Destination.Y);
            Plugin->Profile->SetNumberField(TEXT("dungeon_landblock"),Legacy?Landblock>>16:Landblock);
            for(uint32 Room:{0x100u,0x171u})
            {
                Player.CellId=int32(Landblock|Room);C->GetSession()->SetLocalPosition(Player);
                const auto Drawn=DrawText();
                TestFalse(TEXT("Same-dungeon target never paints the other-dungeon warning, including legacy saves"),Drawn.Contains(TEXT("Destination in another dungeon")));
                TestTrue(TEXT("Same-dungeon target paints destination and remaining distance across room changes"),Drawn.Contains(TEXT("To ")+ACEWaypoint::Format(Destination))&&Drawn.Contains(TEXT("10 m remaining")));
            }
            Player.CellId=int32((Landblock+0x10000)|0x100);C->GetSession()->SetLocalPosition(Player);
            auto Drawn=DrawText();
            TestTrue(TEXT("Entering a different dungeon still paints the warning"),Drawn.Contains(TEXT("Destination in another dungeon")));
            TestFalse(TEXT("Other-dungeon targets do not show a misleading distance"),Drawn.ContainsByPredicate([](const FString& Text){return Text.EndsWith(TEXT("m remaining"));}));
            Plugin->Profile->SetNumberField(TEXT("dungeon_landblock"),0);
            TestFalse(TEXT("Unrestricted overworld targets are not mistaken for another dungeon"),DrawText().Contains(TEXT("Destination in another dungeon")));
        }
    }
    if(FApp::CanEverRender())
    {
        // Crowded markers become count dots; singleton names must not obscure
        // one another, and all original results remain selectable.
        const FString Names[]={TEXT("Portal to Holtburg"),TEXT("Portal to Yaraq"),TEXT("Portal to a dungeon with a very long name")};
        for(bool Pinned:{false,true})for(double Zoom:{.6,3.5,48.})for(bool HeadingUp:{false,true})for(const auto Size:{FVector2D(260,320),FVector2D(90,48)})
        {
            auto View=SNew(SWaypointMap).Host(Host).Overlay(Pinned);
            View->Dungeon=true;View->Landblock=0x01430000;View->CellCount=View->CellIndex=1;
            View->Player=View->FACEPositionForCenter();View->Zoom=Zoom;View->MapUp=HeadingUp?FVector2D(.6,.8):FVector2D(0,1);
            View->LastSize=Size;
            View->Markers={{View->Unproject(Size*.5,Size),Names[0],true},
                {View->Unproject(Size*.5+FVector2D(1,1),Size),Names[1],true,true},
                {View->Unproject(Size*FVector2D(.5,.8),Size),Names[2],true},
                {View->Unproject({-100,-100},Size),TEXT("Off-map portal"),true}};
            auto Window=SNew(SVirtualWindow).Size(Size);FHittestGrid Grid;
            const auto Geometry=FGeometry::MakeRoot(Size,FSlateLayoutTransform());
            TArray<FString> OriginalNames;
            for(int32 Pass=0;Pass<3;++Pass)
            {
                if(Pass==1)View->Hover=Size*.5;
                if(Pass==2){View->Markers.Swap(0,2);View->Hover=FVector2D(-100,-100);}
                View->UpdateClusters(Size);
                FSlateWindowElementList Elements(Window);
                View->OnPaint(FPaintArgs(&View.Get(),Grid,FVector2D::ZeroVector,0,0),Geometry,FSlateRect(0,0,Size.X,Size.Y),Elements,0,FWidgetStyle(),true);
                TArray<FString> Drawn;
                for(const auto& Element:Elements.GetUncachedDrawElements().Get<uint8(EElementType::ET_Text)>())Drawn.Add(Element.GetText());
                TestFalse(TEXT("Off-map portal labels remain clipped to the visible map"),Drawn.Contains(TEXT("Off-map portal")));
                TestEqual(TEXT("All on-map portals remain selectable"),View->PaintedMarkers.Num(),3);
                TestEqual(TEXT("Each visible cluster has a native click target"),View->ClusterButtons->GetChildren()->Num(),View->PaintedClusters.Num());
                int32 Members=0;for(const auto& Cluster:View->PaintedClusters)Members+=Cluster.Indices.Num();
                TestEqual(TEXT("Cluster result lists retain all on-map destinations"),Members,3);
                TArray<FString> VisibleNames;
                for(int32 I=0;I<View->PaintedLabels.Num();++I)
                {
                    const auto& Label=View->PaintedLabels[I];
                    if(!TestTrue(TEXT("Accepted labels refer to a visible marker"),View->PaintedMarkers.Contains(Label.Index)))continue;
                    TestTrue(TEXT("Only singleton markers emit map names"),View->PaintedClusters.ContainsByPredicate([&](const auto& Cluster){return Cluster.Indices.Num()==1&&Cluster.Indices[0]==Label.Index;}));
                    VisibleNames.Add(View->Markers[Label.Index].Name);
                    const FBox2D Box(Label.Origin,Label.Origin+Label.Size);
                    TestTrue(TEXT("Labels stay within the map at every tested size"),Box.Min.X>=0&&Box.Min.Y>=0&&Box.Max.X<=Size.X+.01&&Box.Max.Y<=Size.Y+.01);
                    for(int32 J=0;J<I;++J)
                    {
                        const auto& Other=View->PaintedLabels[J];
                        const FBox2D B(Other.Origin,Other.Origin+Other.Size);
                        TestFalse(TEXT("Accepted label rectangles do not overlap"),Box.Min.X<B.Max.X&&Box.Max.X>B.Min.X&&Box.Min.Y<B.Max.Y&&Box.Max.Y>B.Min.Y);
                    }
                }
                VisibleNames.Sort();
                if(Pass==0)OriginalNames=VisibleNames;
                if(Pass==2)TestTrue(TEXT("Marker iteration order does not choose different names"),OriginalNames==VisibleNames);
                if(Size.X>=260)
                {
                    TestFalse(TEXT("Ordinary minimap retains readable labels when room exists"),VisibleNames.IsEmpty());
                    TestFalse(TEXT("Grouped portal names never pile up on the map"),VisibleNames.Contains(Names[0])||VisibleNames.Contains(Names[1]));
                }
                if(Pass==1)
                {
                    const auto Found=View->ClusterResults(View->HitCluster(Size*.5));
                    TestTrue(TEXT("Hovering a group describes the number of selectable results"),Found.Num()>1&&View->MarkerTooltip().ToString().Contains(FString::Printf(TEXT("%d results returned"),Found.Num())));
                    View->OnMouseLeave(FPointerEvent());
                    TestEqual(TEXT("Leaving the minimap clears stale hover priority"),View->HoveredMarker(Size),INDEX_NONE);
                }
            }
        }
    }
    if(FApp::CanEverRender()&&FSlateApplication::IsInitialized())
    {
        // Exercise actual pointer routing: a handler-only test cannot detect a
        // pinned map that remains click-through or a marker stealing its drag.
        auto& App=FSlateApplication::Get();auto* Manager=C->GetUIElementManager();
        const bool WasLocked=Manager&&Manager->IsUiLocked();
        const auto OriginalProfile=Plugin->Profile;const auto OriginalPosition=C->GetPlayerPosition();
        Plugin->Profile=MakeShared<FJsonObject>(*OriginalProfile);
        Plugin->Profile->SetBoolField(TEXT("map_minimized"),false);
        Plugin->Profile->SetBoolField(TEXT("heading_up"),true);
        ON_SCOPE_EXIT{if(Manager)Manager->SetUiLocked(WasLocked);Plugin->Profile=OriginalProfile;C->GetSession()->SetLocalPosition(OriginalPosition);};
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName,bool)> FindWidget;
        FindWidget=[&](TSharedRef<SWidget> W,FName Name,bool Tag)->TSharedPtr<SWidget>
        {
            if(Tag?W->GetTag()==Name:W->GetType()==Name)return W;
            auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
                if(auto Found=FindWidget(Children->GetChildAt(I),Name,Tag))return Found;
            return nullptr;
        };
        for(bool Interior:{false,true})for(bool Locked:{false,true})
        {
            if(Manager)Manager->SetUiLocked(Locked);
            FACEPosition P;P.CellId=Interior?0x01430171:0x7D640014;P.Location=FVector(49,75,0);C->GetSession()->SetLocalPosition(P);
            auto Overlay=Host->MakeWaypointDungeonOverlay();
            auto View=StaticCastSharedPtr<SWaypointMap>(FindWidget(Overlay,TEXT("SWaypointMap"),false));
            auto Center=StaticCastSharedPtr<SButton>(FindWidget(Overlay,TEXT("Waypoint.CenterPlayer"),true));
            auto Grip=FindWidget(Overlay,TEXT("Waypoint.MoveGrip"),true);
            auto Controls=FindWidget(Overlay,TEXT("Waypoint.ZoomControls"),true);
            if(!TestTrue(TEXT("Pinned map exposes distinct pan, center, move and wheel controls"),View&&Center&&Grip&&Controls))return false;
            TestEqual(TEXT("Captured map has the controller world-input blocker tag"),View->GetTag(),FName(TEXT("Waypoint.MapSurface")));
            const FVector2D Size(260,320);constexpr float Scale=1.35f;
            // Virtual windows have no native DPI state. Put the DPI transform
            // in the widget tree so hit testing and rebuilt capture paths agree.
            auto Window=SNew(SVirtualWindow).Size(Size);
            Window->SetContent(SNew(SDPIScaler).DPIScale(Scale)[Overlay]);App.RegisterVirtualWindow(Window);
            FWidgetRenderer Renderer(false,true);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            ON_SCOPE_EXIT{App.ReleaseAllPointerCapture();App.UnregisterVirtualWindow(Window);Target->ReleaseResource();};
            auto Frame=[&]{++GFrameCounter;Renderer.DrawWindow(Target,Window->GetHittestGrid(),Window,1.f,Size,.016f);FlushRenderingCommands();};
            Frame();Frame();View->NextMarkers=View->NextRoute=TNumericLimits<double>::Max();View->Markers.Reset();View->UpdateClusters(View->LastSize);Frame();
            const auto Geometry=View->GetCachedGeometry();const auto LocalSize=Geometry.GetLocalSize();
            auto Path=[&](const FVector2D& At){return App.LocateWindowUnderMouse(At,{Window},false,0);};
            FVector2D PointerAt(-100,-100);
            auto Down=[&](const FVector2D& At)
            {
                // Real buttons need the pointer-enter event before a click;
                // their mouse-up handlers deliberately require hover state.
                App.RoutePointerMoveEvent(Path(At),FPointerEvent(0,At,PointerAt,{},EKeys::Invalid,0,FModifierKeysState()),false);
                PointerAt=At;
                return App.RoutePointerDownEvent(Path(At),FPointerEvent(0,At,At,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
            };
            auto Move=[&](const FVector2D& From,const FVector2D& To){PointerAt=To;return App.RoutePointerMoveEvent(Path(To),FPointerEvent(0,To,From,{EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()),false);};
            auto Up=[&](const FVector2D& At){PointerAt=At;return App.RoutePointerUpEvent(Path(At),FPointerEvent(0,At,At,{},EKeys::LeftMouseButton,0,FModifierKeysState()));};
            auto Wheel=[&](const FVector2D& At,float Delta)
            {
                App.RoutePointerMoveEvent(Path(At),FPointerEvent(0,At,PointerAt,{},EKeys::Invalid,0,FModifierKeysState()),false);PointerAt=At;
                return App.RouteMouseWheelOrGestureEvent(Path(At),FPointerEvent(0,At,At,{},EKeys::Invalid,Delta,FModifierKeysState()));
            };
            const FVector2D Start=Geometry.LocalToAbsolute(FVector2D(45,75)),End=Geometry.LocalToAbsolute(FVector2D(79,91));
            TestTrue(TEXT("Empty pinned map content participates in hit testing while locked or unlocked"),Path(Start).ContainsWidget(View.Get()));
            TestEqual(TEXT("Separate move grip is available only while unlocked"),Path(Geometry.LocalToAbsolute(FVector2D(13,13))).ContainsWidget(Grip.Get()),!Locked);
            if(!Locked)
            {
                const auto ResizeAt=Geometry.LocalToAbsolute(LocalSize-FVector2D(2,2));
                TestFalse(TEXT("Unlocked map keeps the frame resize corner available"),View->OnMouseButtonDown(Geometry,
                    FPointerEvent(0,ResizeAt,ResizeAt,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState())).IsEventHandled());
            }
            const auto& CenterGeometry=Center->GetCachedGeometry();
            const FVector2D IconStart=Geometry.AbsoluteToLocal(CenterGeometry.LocalToAbsolute(FVector2D::ZeroVector));
            const FVector2D IconEnd=Geometry.AbsoluteToLocal(CenterGeometry.LocalToAbsolute(CenterGeometry.GetLocalSize()));
            TestTrue(TEXT("Center icon fits a narrow minimap at 135 percent DPI"),IconStart.X>=0&&IconStart.Y>=0&&IconEnd.X<=LocalSize.X+.1&&IconEnd.Y<=LocalSize.Y+.1&&CenterGeometry.GetLocalSize().X>0);
            const auto Tip=Center->GetToolTip();
            if(TestTrue(TEXT("Center icon has a themed tooltip"),Tip.IsValid()))
            {
                Tip->OnOpening();auto Text=StaticCastSharedPtr<STextBlock>(FindWidget(Tip->AsWidget(),TEXT("SACERetailTextBlock"),false));
                if(TestTrue(TEXT("Center tooltip uses retail text"),Text.IsValid()))TestEqual(TEXT("Center tooltip matches requested wording"),Text->GetText().ToString(),FString(TEXT("center player")));
            }
            Host->SetWaypoint(FVector2D(33.6,42));
            auto DestinationUnchanged=[&]{FVector2D Destination;uint32 Landblock=0;return Host->GetWaypoint(Destination,Landblock)&&Destination==FVector2D(33.6,42)&&Landblock==0;};
            Down(Start);Up(Start);
            TestTrue(TEXT("Clicking empty minimap space does not navigate or disable follow"),DestinationUnchanged()&&View->FollowPlayer);
            const double InitialZoom=View->Zoom;
            TestTrue(TEXT("Minimap handles wheel zoom in while locked or unlocked"),Wheel(Start,1).IsEventHandled());
            TestTrue(TEXT("Wheel zoom in keeps a tracked player centered"),View->Zoom>InitialZoom&&View->FollowPlayer
                &&View->Project(ACEWaypoint::Coordinates(P),View->LastSize).Equals(View->LastSize*.5,.001));
            TestTrue(TEXT("Minimap handles wheel zoom out while locked or unlocked"),Wheel(Start,-1).IsEventHandled());
            TestTrue(TEXT("Opposite wheel steps restore the initial zoom"),FMath::IsNearlyEqual(View->Zoom,InitialZoom,1.e-9));
            const auto Before=View->Unproject(LocalSize*.5,LocalSize);
            Down(Start);TestTrue(TEXT("Content drag captures the actual map"),View->HasMouseCapture()&&View->PointerDown);
            Move(Start,End);
            // VR hands can share a Slate user or belong to separate virtual
            // users. Neither may replace or finish the first pointer's pan.
            const auto OwnerPan=View->Pan,OwnerPress=View->Press,OwnerLast=View->Last,OwnerHover=View->Hover;
            const int32 OwnerUser=View->PointerUser;const uint32 OwnerPointer=View->PointerIndex;
            for(const FIntPoint Other:{FIntPoint(0,1),FIntPoint(1,0)})
            {
                const auto OtherAt=End+FVector2D(20,35),OtherEnd=OtherAt+FVector2D(45,20);
                const auto OtherDown=View->OnPreviewMouseButtonDown(Geometry,FPointerEvent(uint32(Other.X),uint32(Other.Y),OtherAt,OtherAt,
                    {EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
                const auto OtherMove=View->OnMouseMove(Geometry,FPointerEvent(uint32(Other.X),uint32(Other.Y),OtherEnd,OtherAt,
                    {EKeys::LeftMouseButton},EKeys::Invalid,0,FModifierKeysState()));
                const auto OtherUp=View->OnMouseButtonUp(Geometry,FPointerEvent(uint32(Other.X),uint32(Other.Y),OtherEnd,OtherEnd,
                    {},EKeys::LeftMouseButton,0,FModifierKeysState()));
                TestTrue(TEXT("A second VR pointer cannot steal or release map capture"),OtherDown.IsEventHandled()&&OtherMove.IsEventHandled()&&OtherUp.IsEventHandled()
                    &&!OtherDown.GetMouseCaptor().IsValid()&&!OtherUp.ShouldReleaseMouse()&&View->HasMouseCapture()&&View->PointerDown
                    &&View->PointerUser==OwnerUser&&View->PointerIndex==OwnerPointer);
                TestTrue(TEXT("A second VR pointer cannot alter the owner's pan or destination"),View->Pan==OwnerPan&&View->Press==OwnerPress
                    &&View->Last==OwnerLast&&View->Hover==OwnerHover&&View->Dragged&&!View->FollowPlayer&&DestinationUnchanged());
            }
            Up(End);
            const auto Panned=View->Unproject(LocalSize*.5,LocalSize);
            TestTrue(TEXT("Dragging pans instead of moving the destination"),!Panned.Equals(Before,.000001)&&!View->FollowPlayer&&DestinationUnchanged());
            TestFalse(TEXT("Completing a map drag releases capture"),View->HasMouseCapture()||View->PointerDown);
            P.Location+=FVector(12,8,0);P.RotationW=FMath::Cos(.5);P.RotationXYZ=FVector(0,0,FMath::Sin(.5));C->GetSession()->SetLocalPosition(P);
            View->Tick(Geometry,Time++,.016f);Frame();
            TestTrue(TEXT("Panned position survives player movement and heading changes"),View->Unproject(View->LastSize*.5,View->LastSize).Equals(Panned,.000001));
            Wheel(Geometry.LocalToAbsolute(LocalSize*.5),1);View->Tick(Geometry,Time++,.016f);
            TestTrue(TEXT("Zooming a panned map does not resume following"),!View->FollowPlayer&&View->Unproject(LocalSize*.5,LocalSize).Equals(Panned,.000001));
            const FVector2D CursorLocal=Geometry.AbsoluteToLocal(Start),UnderCursor=View->Unproject(CursorLocal,LocalSize);
            Wheel(Start,1);View->Tick(Geometry,Time++,.016f);
            TestTrue(TEXT("Panned wheel zoom preserves the world location beneath the cursor"),!View->FollowPlayer&&View->Unproject(CursorLocal,LocalSize).Equals(UnderCursor,.000001));
            Wheel(Start,-1);View->Tick(Geometry,Time++,.016f);
            TestTrue(TEXT("Reverse cursor zoom restores the previous panned center"),View->Unproject(LocalSize*.5,LocalSize).Equals(Panned,.000001));
            const FVector2D Resized=LocalSize+FVector2D(50,30);
            View->Tick(FGeometry::MakeRoot(Resized,FSlateLayoutTransform()),Time++,.016f);
            TestTrue(TEXT("Resizing preserves the chosen map center"),View->Unproject(Resized*.5,Resized).Equals(Panned,.000001));
            View->Tick(Geometry,Time++,.016f);Frame();
            const double ZoomBeforeCenter=View->Zoom;
            const FVector2D CenterAt=Center->GetCachedGeometry().LocalToAbsolute(Center->GetCachedGeometry().GetLocalSize()*.5);
            Down(CenterAt);
            TestTrue(TEXT("Center icon receives real hover and button capture"),Center->IsHovered()&&Center->HasMouseCapture());
            Up(CenterAt);Frame();
            TestTrue(TEXT("Center icon restores player tracking"),View->FollowPlayer&&View->Project(ACEWaypoint::Coordinates(P),View->LastSize).Equals(View->LastSize*.5,.001));
            TestEqual(TEXT("Centering keeps the chosen zoom"),View->Zoom,ZoomBeforeCenter);
            TestTrue(TEXT("Centering leaves the navigation destination unchanged"),DestinationUnchanged());
            TestTrue(TEXT("Minimap toolbar has a world-input-blocking border in its hit path"),Controls->GetType()==TEXT("SBorder")&&Path(CenterAt).ContainsWidget(Controls.Get()));
            TestTrue(TEXT("Wheel over the toolbar forwards to minimap zoom"),Wheel(CenterAt,1).IsEventHandled()&&View->Zoom>ZoomBeforeCenter);
            Wheel(CenterAt,-1);
            TestTrue(TEXT("Toolbar wheel steps preserve tracking and restore zoom"),View->FollowPlayer&&FMath::IsNearlyEqual(View->Zoom,ZoomBeforeCenter,1.e-9));
            TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
            TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(int32(Size.X),int32(Size.Y),Pixels,PNG);
            FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(
                TEXT("Waypoint-Controls-%s-%s-135.png"),Interior?TEXT("Dungeon"):TEXT("World"),Locked?TEXT("Locked"):TEXT("Unlocked"))));
            // Pressing a count dot can still become a drag; a stationary click
            // must retain its list behavior, without navigating to an anchor.
            auto PlaceCluster=[&]
            {
                View->Markers={{View->Unproject({70,110},View->LastSize),TEXT("Portal one"),true},
                    {View->Unproject({71,111},View->LastSize),TEXT("Portal two"),true}};
                View->UpdateClusters(View->LastSize);Frame();
            };PlaceCluster();
            if(!TestEqual(TEXT("Panning fixture has one selectable count dot"),View->PaintedClusters.Num(),1))return false;
            const FVector2D Dot=View->GetCachedGeometry().LocalToAbsolute(View->PaintedClusters[0].Point),DotEnd=Dot+FVector2D(30,20);
            const double BeforeDotWheel=View->Zoom;
            TestTrue(TEXT("Wheel over a marker also reaches minimap zoom"),Wheel(Dot,1).IsEventHandled()&&View->Zoom>BeforeDotWheel);
            Wheel(Dot,-1);Frame();
            Down(Dot);Move(Dot,DotEnd);Up(DotEnd);
            TestTrue(TEXT("Dragging from a count dot pans without selecting or opening results"),!View->FollowPlayer&&DestinationUnchanged()&&!View->ResultsBox->GetVisibility().IsVisible());
            View->CenterOnPlayer();PlaceCluster();
            if(!TestEqual(TEXT("Centered fixture retains its count dot"),View->PaintedClusters.Num(),1))return false;
            const FVector2D ClickDot=View->GetCachedGeometry().LocalToAbsolute(View->PaintedClusters[0].Point);
            Down(ClickDot);Up(ClickDot);Frame();
            TestTrue(TEXT("A count-dot click still opens all matching locations"),View->ResultsBox->GetVisibility().IsVisible()&&View->Results.Num()==2&&DestinationUnchanged());
            const auto& ListGeometry=View->ResultsList->GetCachedGeometry();
            const FVector2D ListAt=ListGeometry.LocalToAbsolute(ListGeometry.GetLocalSize()*.5);const double BeforeListWheel=View->Zoom;
            TestTrue(TEXT("Result-list scroll targets the actual list"),Path(ListAt).ContainsWidget(View->ResultsList.Get()));
            TestTrue(TEXT("Result-list wheel is consumed instead of reaching the map"),Wheel(ListAt,-1).IsEventHandled());
            TestEqual(TEXT("Scrolling results leaves minimap zoom unchanged"),View->Zoom,BeforeListWheel);
            View->CloseResults();View->Markers.Reset();View->UpdateClusters(View->LastSize);Frame();
            Down(Start);Move(Start,End);App.ReleaseAllPointerCapture();
            const auto AfterLost=View->Pan;
            View->OnMouseMove(Geometry,FPointerEvent(0,End+FVector2D(80,20),End,{},EKeys::Invalid,0,FModifierKeysState()));
            TestTrue(TEXT("Capture loss ends panning without a later pointer move changing the map"),!View->PointerDown&&!View->HasMouseCapture()&&View->Pan==AfterLost);
            if(!Interior)
            {
                P.CellId=0x7E640014;C->GetSession()->SetLocalPosition(P);View->Tick(Geometry,Time++,.016f);
                TestFalse(TEXT("Ordinary overworld landblock travel does not reset a panned map"),View->FollowPlayer);
            }
            PlaceCluster();
            if(!TestEqual(TEXT("Context-change fixture has a pending count dot"),View->PaintedClusters.Num(),1))return false;
            const FVector2D StaleDot=View->GetCachedGeometry().LocalToAbsolute(View->PaintedClusters[0].Point);
            Down(StaleDot);TestTrue(TEXT("Count-dot press is pending before the dungeon transition"),View->PointerDown&&View->HasMouseCapture());
            P.CellId=Interior?0x01440171:0x01430171;C->GetSession()->SetLocalPosition(P);View->Tick(Geometry,Time++,.016f);
            TestTrue(TEXT("Entering a different map resets follow to the player"),View->FollowPlayer&&View->Project(ACEWaypoint::Coordinates(P),View->LastSize).Equals(View->LastSize*.5,.001));
            TestTrue(TEXT("Map-context transition cancels the gesture and releases its capture"),!View->PointerDown&&!View->HasMouseCapture()&&View->PressResults.IsEmpty());
            Up(StaleDot);Frame();
            TestTrue(TEXT("Releasing a cancelled gesture cannot choose an old dungeon result"),DestinationUnchanged()&&!View->ResultsBox->GetVisibility().IsVisible());
            Host->SetWaypointOption(TEXT("map_minimized"),true);Frame();
            const auto& MinimizedGeometry=Controls->GetCachedGeometry();
            const FVector2D MinimizedAt=MinimizedGeometry.LocalToAbsolute(MinimizedGeometry.GetLocalSize()*.5);const double BeforeMinimizedWheel=View->Zoom;
            TestTrue(TEXT("Minimized map toolbar consumes wheel input"),Wheel(MinimizedAt,1).IsEventHandled());
            TestEqual(TEXT("Minimized map does not change zoom from wheel input"),View->Zoom,BeforeMinimizedWheel);
            Host->SetWaypointOption(TEXT("map_minimized"),false);Frame();
            Host->ClearWaypoint();
        }
    }
    if(FApp::CanEverRender())
    {
        auto* Manager=C->GetUIElementManager();const bool WasLocked=Manager&&Manager->IsUiLocked();
        if(Manager)Manager->SetUiLocked(true);
        ON_SCOPE_EXIT{if(Manager)Manager->SetUiLocked(WasLocked);};
        for(bool Interior:{false,true})
        {
            FACEPosition P;P.CellId=Interior?0x01430171:0x7D640014;P.Location=FVector(49,75,0);C->GetSession()->SetLocalPosition(P);
            const FVector2D Size(260,320);const auto Geometry=FGeometry::MakeRoot(Size,FSlateLayoutTransform());
            auto View=SNew(SWaypointMap).Host(Host).Overlay(true);View->SetVisibility(EVisibility::SelfHitTestInvisible);
            View->Tick(Geometry,Time++,.016f);View->NextMarkers=View->NextRoute=TNumericLimits<double>::Max();
            View->Markers={{View->Unproject({130,120},Size),TEXT("Portal to Arwic"),true},
                {View->Unproject({131,121},Size),TEXT("Portal to Holtburg"),true},
                {View->Unproject({132,122},Size),TEXT("Portal to Yaraq"),true}};
            View->UpdateClusters(Size);const auto Snapshot=View->ClusterResults(0);
            if(!TestEqual(TEXT("A crowded dot exposes every original result"),Snapshot.Num(),3))continue;
            auto Window=SNew(SVirtualWindow).Size(Size);Window->SetContent(View);
            FWidgetRenderer Renderer(false,true);auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);FHittestGrid Grid;
            ON_SCOPE_EXIT{Target->ReleaseResource();};
            auto Frame=[&]{++GFrameCounter;Renderer.DrawWindow(Target,Grid,Window,1.f,Size,.016f);FlushRenderingCommands();};Frame();Frame();
            if(!TestEqual(TEXT("Nearby destinations share one real button"),View->ClusterButtons->GetChildren()->Num(),1))continue;
            auto Button=StaticCastSharedRef<SButton>(View->ClusterButtons->GetChildren()->GetChildAt(0));
            const auto& BG=Button->GetCachedGeometry();const auto At=BG.LocalToAbsolute(BG.GetLocalSize()*.5);
            TestFalse(TEXT("Cluster interaction does not require unlocking the minimap"),Host->IsWaypointMapUnlocked());
            TestTrue(TEXT("A locked minimap dot participates in Slate hit testing"),Grid.GetBubblePath(At,0,false).ContainsByPredicate([&](const auto& Hit){return Hit.Widget==Button;}));
            const auto Empty=View->GetCachedGeometry().LocalToAbsolute(FVector2D(10,60));
            TestFalse(TEXT("Empty locked-map space has no cluster click target"),Grid.GetBubblePath(Empty,0,false).ContainsByPredicate([](const auto& Hit){return Hit.Widget->GetTag()==TEXT("Waypoint.ClusterDot");}));
            TestTrue(TEXT("Pressing the actual dot snapshots its results"),Button->OnMouseButtonDown(BG,FPointerEvent(0,At,At,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState())).IsEventHandled());
            // Simulate a server marker refresh between press and click. The
            // open list must not silently point to unrelated replacement IDs.
            View->Markers={{View->Unproject({210,100},Size),TEXT("Replacement portal"),true}};View->UpdateClusters(Size);
            Button->SimulateClick();Button->OnMouseLeave(FPointerEvent());Frame();Frame();
            TestTrue(TEXT("Clicking a group opens its result list"),View->ResultsBox->GetVisibility().IsVisible());
            if(TestEqual(TEXT("Marker refresh preserves the clicked result count"),View->Results.Num(),Snapshot.Num()))
                for(int32 I=0;I<Snapshot.Num();++I)
                    TestTrue(TEXT("Result names and exact coordinates survive marker refresh"),View->Results[I]->Name==Snapshot[I]->Name&&View->Results[I]->Point==Snapshot[I]->Point&&View->Results[I]->Landblock==Snapshot[I]->Landblock);
            if(View->Results.Num()<2)continue;
            TestTrue(TEXT("Scrolling a short results list cannot zoom the map underneath"),View->ResultsList->OnMouseWheel(
                View->ResultsList->GetCachedGeometry(),FPointerEvent(0,At,At,{},EKeys::Invalid,-1,FModifierKeysState())).IsEventHandled());
            const auto Selected=Snapshot[1];
            const auto Row=View->ResultsList->WidgetFromItem(View->Results[1]);
            TFunction<TSharedPtr<SButton>(TSharedRef<SWidget>)> FindButton;
            FindButton=[&](TSharedRef<SWidget> Widget)->TSharedPtr<SButton>
            {if(Widget->GetType()==TEXT("SButton"))return StaticCastSharedRef<SButton>(Widget);auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=FindButton(Children->GetChildAt(I)))return Found;return nullptr;};
            const auto ResultButton=Row?FindButton(Row->AsWidget()):nullptr;
            if(TestTrue(TEXT("The scrollable result list builds real destination buttons"),ResultButton.IsValid()))ResultButton->SimulateClick();
            FVector2D Destination;uint32 Landblock=0;
            TestTrue(TEXT("Selecting a result uses its exact location, not the cluster anchor"),Host->GetWaypoint(Destination,Landblock)&&Destination==Selected->Point);
            TestEqual(TEXT("A result preserves its original world or dungeon binding"),Landblock,Interior?0x01430000u:0u);
            TestTrue(TEXT("Selecting a result closes the chooser"),View->ResultsBox->GetVisibility()==EVisibility::Collapsed);
            Host->ClearWaypoint();
        }
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("WaypointRender")))
    {
        auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
        Dat->SetWorldStreamingAllowed(true);
        const double Deadline=FPlatformTime::Seconds()+15;
        while(Dat->IsDatLoading()&&FPlatformTime::Seconds()<Deadline)
        {FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);FPlatformProcess::Sleep(.005f);}
        if(!TestTrue(TEXT("Load real dungeon geometry"),Dat->LoadDatDirectory(TEXT("C:/Turbine/Asheron's Call"))))return false;
        Host->SetWaypointMapZoom(true,true,3.5);
        FACEPosition P;P.CellId=0x01430171;P.Location=FVector(49,-75,0);
        P.RotationW=FMath::Cos(.4);P.RotationXYZ=FVector(0,0,FMath::Sin(.4));C->GetSession()->SetLocalPosition(P);
        const FVector2D Size(420,420);FWidgetRenderer Renderer(false,true);
        auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        Target->ClearColor=FLinearColor::Transparent;
        for(bool Facing:{false,true})
        {
            Plugin->Profile->SetBoolField(TEXT("heading_up"),Facing);
            auto Overlay=SNew(SWaypointMap).Host(Host).Overlay(true);
            // Multiple DrawWidget calls in one engine frame do not necessarily
            // tick Slate again. Advance streaming explicitly for this fixture.
            const auto Geometry=FGeometry::MakeRoot(Size,FSlateLayoutTransform());
            for(int Pass=0;Pass<2100;++Pass)
            {
                Overlay->Tick(Geometry,Time+=.016,.016f);
                if(Overlay->CellCount&&Overlay->CellIndex==Overlay->CellCount&&Overlay->Contours.IsComplete())break;
            }
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();}
            TestTrue(TEXT("Pinned fixture contains dungeon floor outlines"),Overlay->Floors.Num()>0);
            TestTrue(TEXT("Real dungeon removes internal floor seams"),Overlay->Contours.Lines.Num()<Overlay->Contours.InputEdgeCount());
            AddInfo(FString::Printf(TEXT("Dungeon %08X: %d floor edges -> %d contour segments"),uint32(P.CellId),Overlay->Contours.InputEdgeCount(),Overlay->Contours.Lines.Num()));
            TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            int32 Black=0;for(const auto& Pixel:Pixels)if(Pixel==FColor::Black)++Black;
            TestTrue(TEXT("Pinned dungeon has an opaque black backdrop at full opacity"),Black>Pixels.Num()*3/4);
            const auto FullPixels=Pixels;
            Host->SetWaypointMapOpacity(.4f);
            TestEqual(TEXT("Pinned map shares the normal map background setting"),Host->GetPluginWindowOpacity(TEXT("waypoint.dungeon")),.4f);
            Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            int32 FadedBlack=0,Foreground=0,StableForeground=0;
            for(int32 I=0;I<Pixels.Num();++I)
            {
                const auto Pixel=Pixels[I];
                if(Pixel.R==0&&Pixel.G==0&&Pixel.B==0&&FMath::Abs(int32(Pixel.A)-102)<=2)++FadedBlack;
                if(FullPixels[I].G>80&&FullPixels[I].A==255)
                {++Foreground;if(Pixel.R==FullPixels[I].R&&Pixel.G==FullPixels[I].G&&Pixel.B==FullPixels[I].B)++StableForeground;}
            }
            TestTrue(TEXT("Opacity slider fades the black backdrop"),FadedBlack>Pixels.Num()*3/4);
            TestTrue(TEXT("Wireframes and labels retain their brightness"),Foreground>20&&StableForeground>Foreground*.98);
            Host->SetPluginWindowOpacity(TEXT(""),0.f,false);Host->SetPluginOpacityGlobal(true);
            Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            int32 Transparent=0;for(const auto& Pixel:Pixels)if(Pixel.A==0)++Transparent;
            TestTrue(TEXT("Global zero opacity removes the pinned backdrop"),Transparent>Pixels.Num()*3/4);
            Host->SetPluginOpacityGlobal(false);Host->SetWaypointMapOpacity(1.f);
            Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(420,420,Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-Pinned-%s.png"),Facing?TEXT("Facing"):TEXT("North"))));
            Plugin->Profile->SetBoolField(TEXT("fill_dungeon"),true);
            Overlay->NextRoute=TNumericLimits<double>::Max();
            // Use the same route projection as live UCM without running automation.
            Overlay->Route.Reset();
            for(int32 I=0;I<4;++I)
            {ACEWaypoint::FRoutePoint Point;Point.Position=P;Point.Position.Location+=FVector((I%2)*12-6,(I/2)*12-6,0);Point.Index=I+1;Point.Connect=I>0;Point.Current=I==2;Overlay->Route.Add(Point);}
            Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            int32 Blue=0,Green=0;for(const auto& Pixel:Pixels)
            {if(Pixel.B>Pixel.G&&Pixel.G>Pixel.R*2&&Pixel.B>20)++Blue;if(Pixel.G>180&&Pixel.R<140&&Pixel.B<160)++Green;}
            TestTrue(TEXT("Optional blue fill visibly covers walkable dungeon floors"),Blue>Pixels.Num()/10);
            TestTrue(TEXT("Route's active waypoint visibly highlights green"),Green>20);
            FImageUtils::PNGCompressImageArray(420,420,Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-Filled-Route-%s.png"),Facing?TEXT("Facing"):TEXT("North"))));
            TestTrue(TEXT("Current route waypoint is inside the rendered fixture"),FBox2D(FVector2D::ZeroVector,Size).IsInside(Overlay->Project(ACEWaypoint::Coordinates(Overlay->Route[2].Position),Size)));
            Plugin->Profile->SetBoolField(TEXT("show_route"),false);Overlay->Tick(Geometry,Time+=.016,.016f);
            TestTrue(TEXT("Hiding routes clears the overlay immediately"),Overlay->Route.IsEmpty());
            Plugin->Profile->SetBoolField(TEXT("show_route"),true);Plugin->Profile->SetBoolField(TEXT("fill_dungeon"),false);
            P.CellId=0x7D640014;P.Location=FVector(60,78,12);C->GetSession()->SetLocalPosition(P);
            Overlay->Tick(Geometry,Time+=.016,.016f);
            TestTrue(TEXT("Pinned overworld resolves retail overview after leaving dungeon"),!Overlay->Dungeon&&Overlay->MapTexture.IsValid());
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();}
            Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            int32 Opaque=0;for(const auto& Pixel:Pixels)if(Pixel.A>0)++Opaque;
            TestTrue(TEXT("Pinned overworld renders terrain across the overlay"),Opaque>Pixels.Num()*3/4);
            FImageUtils::PNGCompressImageArray(420,420,Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-Pinned-World-%s.png"),Facing?TEXT("Facing"):TEXT("North"))));
            P.CellId=0x01430171;P.Location=FVector(49,-75,0);C->GetSession()->SetLocalPosition(P);
        }
        // Nearby portals must all remain available in the grouped results.
        auto Overlay=SNew(SWaypointMap).Host(Host).Overlay(true);
        P.CellId=0x7D640014;P.Location=FVector(60,78,12);C->GetSession()->SetLocalPosition(P);
        Overlay->Tick(FGeometry::MakeRoot(Size,FSlateLayoutTransform()),Time+=.016,.016f);
        Overlay->NextMarkers=TNumericLimits<double>::Max(); // Keep the synthetic cluster during Slate's render tick.
        const auto Here=ACEWaypoint::Coordinates(Overlay->Player);
        Overlay->Markers={{Here,TEXT("Portal to Holtburg"),true},{Here+FVector2D(.1,0),TEXT("Portal to Yaraq"),true},{Here+FVector2D(0,.1),TEXT("Portal to Arwic"),true}};
        Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();
        TestEqual(TEXT("All closely spaced portals remain available for selection"),Overlay->PaintedMarkers.Num(),3);
        TArray<FColor> OpaquePixels;FReadSurfaceDataFlags OpacityFlags;OpacityFlags.SetLinearToGamma(false);
        Target->GameThread_GetRenderTargetResource()->ReadPixels(OpaquePixels,OpacityFlags);
        uint64 FullAlpha=0;for(const auto& Pixel:OpaquePixels)FullAlpha+=Pixel.A;
        Host->SetWaypointMapOpacity(.4f);
        Renderer.DrawWidget(Target,Overlay,Size,.016f);FlushRenderingCommands();
        Target->GameThread_GetRenderTargetResource()->ReadPixels(OpaquePixels,OpacityFlags);
        uint64 FadedAlpha=0;for(const auto& Pixel:OpaquePixels)FadedAlpha+=Pixel.A;
        TestTrue(TEXT("Background opacity also fades the overworld terrain"),FullAlpha>0&&FadedAlpha<FullAlpha*.8);
        Host->SetWaypointMapOpacity(1.f);
        Target->ReleaseResource();

        // Render actual Slate glyphs for the reported crowded-town shape and
        // an interior cluster. Keep the geography constant when resizing;
        // the 135% pass changes physical pixels, not the logical map size.
        for(bool Interior:{false,true})for(int32 Width:{260,560})
        {
            FACEPosition DensePlayer;
            if(Interior){DensePlayer.CellId=0x01430171;DensePlayer.Location=FVector(49,-75,0);}
            else
            {
                const double X=(56.8+101.95)*240.,Y=(33.6+101.95)*240.;
                DensePlayer.CellId=int32((uint32(X/192)<<24)|(uint32(Y/192)<<16)|1);
                DensePlayer.Location=FVector(FMath::Fmod(X,192.),FMath::Fmod(Y,192.),0);
            }
            C->GetSession()->SetLocalPosition(DensePlayer);
            Plugin->Profile->SetBoolField(TEXT("heading_up"),false);
            Host->SetWaypointMapZoom(true,Interior,DefaultWaypointZoom(true,Interior));
            auto DenseMap=SNew(SWaypointMap).Host(Host).Overlay(true);
            const FVector2D BaseSize(260,320),LogicalSize(Width,Width==260?320:560);
            DenseMap->Tick(FGeometry::MakeRoot(BaseSize,FSlateLayoutTransform()),Time+=.016,.016f);
            const double LayoutDeadline=FPlatformTime::Seconds()+3;
            while(Interior&&DenseMap->CellCount&&(DenseMap->CellIndex<DenseMap->CellCount||!DenseMap->Contours.IsComplete())&&FPlatformTime::Seconds()<LayoutDeadline)
                DenseMap->Tick(FGeometry::MakeRoot(BaseSize,FSlateLayoutTransform()),Time+=.016,.016f);
            DenseMap->Markers.Reset();DenseMap->NextMarkers=TNumericLimits<double>::Max();
            DenseMap->NextRoute=TNumericLimits<double>::Max();DenseMap->Route.Reset();
            for(int32 I=0;I<120;++I)
            {
                const FVector2D At=BaseSize*.5+FVector2D((I%12-5.5)*14,(I/12-4.5)*12);
                DenseMap->Markers.Add({DenseMap->Unproject(At,BaseSize),FString::Printf(TEXT("%s %03d"),Interior?TEXT("Dungeon gateway"):TEXT("Arwic atlas destination"),I),I%5!=0,false});
            }
            for(float Scale:{1.f,1.35f})
            {
                const FVector2D PhysicalSize(FMath::RoundToInt(LogicalSize.X*Scale),FMath::RoundToInt(LogicalSize.Y*Scale));
                auto* DenseTarget=FWidgetRenderer::CreateTargetFor(PhysicalSize,TF_Bilinear,true);
                for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(DenseTarget,DenseMap,Scale,PhysicalSize,.016f);FlushRenderingCommands();}
                TestEqual(TEXT("Crowded render retains every on-map destination"),DenseMap->PaintedMarkers.Num(),120);
                TestTrue(TEXT("Crowded render replaces close names with count dots"),DenseMap->PaintedClusters.Num()>0&&DenseMap->PaintedClusters.Num()<120);
                int32 Members=0;for(const auto& Cluster:DenseMap->PaintedClusters)Members+=Cluster.Indices.Num();
                TestEqual(TEXT("Rendered count dots account for every destination"),Members,120);
                for(int32 I=0;I<DenseMap->PaintedLabels.Num();++I)
                {
                    const auto& A=DenseMap->PaintedLabels[I];const FBox2D Box(A.Origin,A.Origin+A.Size);
                    TestTrue(TEXT("DPI-scaled labels stay within the logical minimap"),Box.Min.X>=0&&Box.Min.Y>=0&&Box.Max.X<=LogicalSize.X+.01&&Box.Max.Y<=LogicalSize.Y+.01);
                    for(int32 J=0;J<I;++J){const auto& B=DenseMap->PaintedLabels[J];TestFalse(TEXT("Rendered dense label bounds do not intersect"),Box.Min.X<B.Origin.X+B.Size.X&&Box.Max.X>B.Origin.X&&Box.Min.Y<B.Origin.Y+B.Size.Y&&Box.Max.Y>B.Origin.Y);}
                }
                TArray<FColor> DensePixels;DenseTarget->GameThread_GetRenderTargetResource()->ReadPixels(DensePixels,OpacityFlags);
                if(const auto* Badge=DenseMap->PaintedClusters.FindByPredicate([](const auto& Group){return Group.Indices.Num()>1;}))
                {
                    const FVector2D Center=Badge->Point*Scale;int32 Dark=0,Samples=0;
                    for(int32 Y=int32(Center.Y-7*Scale);Y<int32(Center.Y+7*Scale);++Y)
                        for(int32 X=int32(Center.X-7*Scale);X<int32(Center.X+7*Scale);++X)
                        {
                            if(X<0||Y<0||X>=int32(PhysicalSize.X)||Y>=int32(PhysicalSize.Y))continue;
                            const auto& Pixel=DensePixels[Y*int32(PhysicalSize.X)+X];++Samples;
                            if(Pixel.R<90&&Pixel.G<80&&Pixel.B<95)++Dark;
                        }
                    TestTrue(TEXT("Count badges retain a dark contrasting fill behind their text"),Samples>0&&Dark>Samples/3);
                }
                int32 White=0;for(const auto& Pixel:DensePixels)if(Pixel.R>210&&Pixel.G>210&&Pixel.B>210&&Pixel.A>0)++White;
                TestTrue(TEXT("Dense map paints readable text without becoming a white block"),White>30&&White<DensePixels.Num()/4);
                AddInfo(FString::Printf(TEXT("Crowded %s %dx%d at %.0f%%: 120 results, %d dots, %d labels, %d bright pixels of %d"),Interior?TEXT("dungeon"):TEXT("Arwic"),int32(LogicalSize.X),int32(LogicalSize.Y),Scale*100,DenseMap->PaintedClusters.Num(),DenseMap->PaintedLabels.Num(),White,DensePixels.Num()));
                TArray64<uint8> DensePng;FImageUtils::PNGCompressImageArray(int32(PhysicalSize.X),int32(PhysicalSize.Y),DensePixels,DensePng);
                FFileHelper::SaveArrayToFile(DensePng,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-Crowded-%s-%d-%d.png"),Interior?TEXT("Dungeon"):TEXT("Arwic"),Width,FMath::RoundToInt(Scale*100))));
                if(Scale>1.f)
                {
                    int32 Largest=0;for(int32 I=1;I<DenseMap->PaintedClusters.Num();++I)
                        if(DenseMap->PaintedClusters[I].Indices.Num()>DenseMap->PaintedClusters[Largest].Indices.Num())Largest=I;
                    const auto Found=DenseMap->ClusterResults(Largest);DenseMap->ShowResults(Found);
                    for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(DenseTarget,DenseMap,Scale,PhysicalSize,.016f);FlushRenderingCommands();}
                    TestTrue(TEXT("The crowded 135% fixture opens a multi-result chooser"),Found.Num()>1&&DenseMap->ResultsBox->GetVisibility().IsVisible());
                    const auto Bounds=DenseMap->ResultsBox->GetCachedGeometry().GetRenderBoundingRect();
                    TestTrue(TEXT("Result chooser stays inside its DPI-scaled map"),Bounds.Left>=-.01&&Bounds.Top>=-.01&&Bounds.Right<=PhysicalSize.X+.01&&Bounds.Bottom<=PhysicalSize.Y+.01);
                    DenseTarget->GameThread_GetRenderTargetResource()->ReadPixels(DensePixels,OpacityFlags);
                    FImageUtils::PNGCompressImageArray(int32(PhysicalSize.X),int32(PhysicalSize.Y),DensePixels,DensePng);
                    FFileHelper::SaveArrayToFile(DensePng,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-Results-%s-%d-135.png"),Interior?TEXT("Dungeon"):TEXT("Arwic"),Width)));
                    DenseMap->CloseResults();
                }
                DenseTarget->ReleaseResource();
            }
        }
    }
    return true;
}
}
#endif
