#include "Mods/ACEPluginSubsystem.h"
#include "ACEClientSubsystem.h"
#include "Engine/GameInstance.h"
#include "Misc/Paths.h"

namespace
{
    FString Field(const TSharedPtr<FJsonObject>& O,const TCHAR* Key)
    {FString V;if(O)O->TryGetStringField(Key,V);return V;}
}
void UACEPluginSubsystem::FlushUCMLog()
{
    if(!UCMLog.Dirty)return;
    UCMLogSaveError=UCMLog.Save(UserDirectory()/TEXT("ucm-log.json"))?FString():TEXT("Could not save UCM log. History is still available in this session.");
}
void UACEPluginSubsystem::ClearUCMLog() { UCMLog.Clear();FlushUCMLog(); }
void UACEPluginSubsystem::LogUCMEvent(const FACEClientPlugin& P,const FString& Message,EACEUCMLogLevel Level,const TSharedPtr<FJsonObject>& Intent)
{
    if(P.Api>=2)if(auto Current=Find(P.Id))
    {
        auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("time"),FDateTime::UtcNow().ToUnixTimestamp());E->SetStringField(TEXT("text"),Message.Left(1024));E->SetNumberField(TEXT("level"),int32(Level));
        if(Current->LogEntries.Num()>=200)Current->LogEntries.RemoveAt(0);Current->LogEntries.Add(MakeShared<FJsonValueObject>(E));
    }
    if(P.Id!=TEXT("ucm")&&!P.UsesCompactControls())return;
    FString Context=TEXT("Character: ")+Field(P.ProfileLinks,TEXT("Character")).Left(128);
    FString Route=Field(P.ProfileLinks,TEXT("Routes"));if(Route.IsEmpty())Route=Field(P.Profile,TEXT("nav_source"));
    if(!Route.IsEmpty())Context+=TEXT(" | Route: ")+FPaths::GetCleanFilename(Route).Left(128);
    Context+=FString::Printf(TEXT(" | Waypoint %d"),RoutePoint);
    if(!P.MetaState.IsEmpty())Context+=TEXT(" | Meta: ")+P.MetaState.Left(128);
    if(auto* GI=GetGameInstance())if(auto* Client=GI->GetSubsystem<UACEClientSubsystem>())
    {
        FACEWorldObject Player;if(Client->GetWorldObject(Client->GetPlayerGuid(),Player))Context+=TEXT(" | ")+Player.Name.Left(128);
        Context+=TEXT(" | ")+Client->GetServerName().Left(128);
        const auto Pos=Client->GetPlayerPosition();
        if(Pos.IsValid())Context+=FString::Printf(TEXT(" | 0x%08X [%.3f %.3f %.3f]"),uint32(Pos.CellId),Pos.Location.X,Pos.Location.Y,Pos.Location.Z);
    }
    if(Intent)
    {
        const FString Action=Field(Intent,TEXT("action"));if(!Action.IsEmpty())Context+=TEXT(" | Action: ")+Action.Left(48);
        double Item=0;if(Intent->TryGetNumberField(TEXT("item"),Item)&&Item>0&&Item<=MAX_uint32)Context+=FString::Printf(TEXT(" | Item: 0x%08X"),uint32(Item));
    }
    const auto RouteData=RuntimeRoute?RuntimeRoute:P.Profile;
    const TArray<TSharedPtr<FJsonValue>>* Points=nullptr;
    if(RouteData&&RouteData->TryGetArrayField(TEXT("route"),Points)&&Points->IsValidIndex(RoutePoint-1))
    {
        const auto& Value=(*Points)[RoutePoint-1];
        if(Value->Type==EJson::Object)
        {
            double Cell=0,X=0,Y=0,Z=0;const auto O=Value->AsObject();
            if(O->TryGetNumberField(TEXT("cell"),Cell)&&Cell>0&&Cell<=MAX_uint32
                &&O->TryGetNumberField(TEXT("x"),X)&&O->TryGetNumberField(TEXT("y"),Y)&&O->TryGetNumberField(TEXT("z"),Z))
                Context+=FString::Printf(TEXT(" | Destination: 0x%08X [%.3f %.3f %.3f]"),uint32(Cell),X,Y,Z);
        }
    }
    UCMLog.Add(Message,Context,Level);
}
void UACEPluginSubsystem::LogUCMIntent(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& I)
{
    if(!P.IsPolicy())return;
    const FString Action=Field(I,TEXT("action")),Status=Field(I,TEXT("status")),Activity=Field(I,TEXT("activity"));
    // Errors/stops are recorded by their host handlers, with the final reason.
    if(Action==TEXT("activity_failed")||Action==TEXT("pause_navigation")||Action==TEXT("stop"))return;
    const FString State=Field(I,TEXT("meta_state"));
    if(!State.IsEmpty()&&State!=P.MetaState)LogUCMEvent(P,TEXT("Meta state: ")+State,EACEUCMLogLevel::Info,I);
    const bool ChangedActivity=!Activity.IsEmpty()&&Activity!=P.LoggedActivity;
    if(!Activity.IsEmpty())P.LoggedActivity=Activity;
    const bool Route=Activity==TEXT("navigation")||Action==TEXT("use_world")||Action==TEXT("jump")||Action==TEXT("recall");
    const bool RouteChanged=Route&&!Status.IsEmpty()&&Status!=P.LoggedRouteStatus;
    if(Route)P.LoggedRouteStatus=Status;
    const bool Milestone=Action==TEXT("profile_load")||Action==TEXT("force_buff_done")
        ||Action==TEXT("buff_request_start")||Action==TEXT("buff_request_done")||Action==TEXT("notice");
    // Record decisions, not every polling tick, spell, item appraisal, or attack.
    if((ChangedActivity||RouteChanged||Milestone)&&!Status.IsEmpty())LogUCMEvent(P,Status,EACEUCMLogLevel::Info,I);
}
