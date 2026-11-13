#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginColor.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACESession.h"
#include "ACEVTProfile.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Audio.h"
#include "Sound/SoundWaveProcedural.h"
#include "Kismet/GameplayStatics.h"
#include "Components/AudioComponent.h"
#include "TimerManager.h"

namespace
{
namespace ACEPluginPublicAPIPrivate
{
    // Explicit local S/N declarations avoid overload collisions with the
    // automation host when Unreal combines these sources in a unity build.
    FString S(const TSharedPtr<FJsonObject>& O,const TCHAR* K,const FString& D=FString())
    {FString V;return O&&O->TryGetStringField(K,V)?V:D;}
    double N(const TSharedPtr<FJsonObject>& O,const TCHAR* K,double D=0)
    {double V;return O&&O->TryGetNumberField(K,V)&&FMath::IsFinite(V)?V:D;}
    bool B(const TSharedPtr<FJsonObject>& O,const TCHAR* K,bool D=true)
    {bool V;return O&&O->TryGetBoolField(K,V)?V:D;}
    FString Encode(const TSharedPtr<FJsonObject>& O)
    {FString R;FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&R));return R;}
    bool IsValidPluginStorageName(const FString& V)
    {if(V.IsEmpty()||V.Len()>64)return false;for(TCHAR C:V)if(!FChar::IsAlnum(C)&&C!='_'&&C!='-')return false;return true;}
    bool Read(const FString& Path,TSharedPtr<FJsonObject>& O)
    {FString T;return IFileManager::Get().FileSize(*Path)<=8*1024*1024&&FFileHelper::LoadFileToString(T,*Path)&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(T),O)&&O.IsValid();}
    bool Write(const FString& Path,const TSharedPtr<FJsonObject>& O)
    {const FString T=Encode(O);if(FTCHARToUTF8(*T).Length()>8*1024*1024)return false;IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);return FFileHelper::SaveStringToFile(T,*(Path+TEXT(".tmp")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)&&IFileManager::Get().Move(*Path,*(Path+TEXT(".tmp")),true,true);}
    TSharedPtr<FJsonObject> Control(const TArray<TSharedPtr<FJsonValue>>& Rows,const FString& Id)
    {
        for(const auto& V:Rows)if(V&&V->Type==EJson::Object)
        {auto R=V->AsObject();if(!B(R,TEXT("enabled")))continue;if(S(R,TEXT("id"))==Id)return R;const TArray<TSharedPtr<FJsonValue>>* Children=nullptr;if(R->TryGetArrayField(TEXT("children"),Children))if(auto Found=Control(*Children,Id))return Found;}
        return nullptr;
    }
}
} // anonymous namespace

TSharedPtr<FACEClientPlugin> UACEPluginSubsystem::PolicyPlugin() const
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    // There is one gameplay driver. Observers never change its profile context.
    for(const auto& P:Plugins)if(P->Running&&P->Enabled&&P->IsPolicy())return P;
    if(auto Idle=Find(IdleManaOwner);Idle&&Idle->Enabled&&Idle->IsPolicy())return Idle;
    return Find(TEXT("ucm")); // Preserve stopped UCM route preview/settings.
}

void UACEPluginSubsystem::QueuePluginEvent(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& E)
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    E->SetNumberField(TEXT("serial"),++PluginEventSerial);
    int32 Bytes=FTCHARToUTF8(*Encode(E)).Length();
    for(const auto& V:P.PluginEvents)Bytes+=FTCHARToUTF8(*Encode(V->AsObject())).Length();
    while(!P.PluginEvents.IsEmpty()&&(P.PluginEvents.Num()>=64||Bytes>384*1024))
    {Bytes-=FTCHARToUTF8(*Encode(P.PluginEvents[0]->AsObject())).Length();P.PluginEvents.RemoveAt(0);}
    P.PluginEvents.Add(MakeShared<FJsonValueObject>(E));P.NextDecision=0;
}

TSharedPtr<FJsonObject> UACEPluginSubsystem::PublicSnapshot(FACEClientPlugin& P)
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    auto Common=CompanionSnapshot(P);
    const bool DataAllowed=!P.Companion||P.Permissions.Contains(TEXT("data"));
    // Character-only observers should never build inventory, spell tables,
    // navigation visibility, or vendor transaction data just to discard it.
    static const TSet<FString> Lightweight={TEXT("char_strings"),TEXT("char_ints"),TEXT("char_quads"),TEXT("char_doubles"),TEXT("char_bools"),
        TEXT("inventory"),TEXT("inventory_revision"),TEXT("appraisal_revision"),TEXT("window_chrome_hidden"),TEXT("level"),TEXT("character_name"),TEXT("action_serial"),TEXT("action_error"),TEXT("last_spell"),TEXT("last_spell_confirmed"),
        TEXT("profile_name"),TEXT("waiting_action"),TEXT("log"),TEXT("panel"),TEXT("activity_failure"),TEXT("capabilities"),TEXT("automation_owner"),TEXT("action_events"),
        TEXT("inventory_failure_serial"),TEXT("inventory_failure_item"),TEXT("inventory_failure_error"),
        TEXT("objects"),TEXT("objects_truncated"),TEXT("selection"),TEXT("tracked_targets"),TEXT("target_effects")};
    bool FullData=!P.SelectiveSnapshot;
    if(P.SelectiveSnapshot)for(const auto& Field:P.SnapshotFields)if(!Common->HasField(Field)&&!Lightweight.Contains(Field)){FullData=true;break;}
    auto Result=(DataAllowed&&FullData)?ItemToolsSnapshot(&P):MakeShared<FJsonObject>();
    if(DataAllowed && !FullData)
    {
        auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();const auto& V=C->GetPlayerVitalsView();
        auto Qualities=[&](const TCHAR* Field,const auto& Map)
        {
            if(!P.WantsSnapshotField(Field))return;
            auto J=MakeShared<FJsonObject>();for(const auto& Pair:Map)J->SetNumberField(FString::FromInt(Pair.Key),double(Pair.Value));Result->SetObjectField(Field,J);
        };
        Qualities(TEXT("char_ints"),V.StatQualityInts);Qualities(TEXT("char_quads"),V.QualityInt64s);
        Qualities(TEXT("char_doubles"),V.QualityDoubles);Qualities(TEXT("char_bools"),V.QualityBools);
        if(P.WantsSnapshotField(TEXT("char_strings")))
        {
            auto J=MakeShared<FJsonObject>();for(const auto& Pair:V.QualityStrings)J->SetStringField(FString::FromInt(Pair.Key),Pair.Value);
            if(!J->HasField(TEXT("1"))&&C->GetSession())J->SetStringField(TEXT("1"),C->GetSession()->GetPlayerName());Result->SetObjectField(TEXT("char_strings"),J);
        }
        if(P.WantsSnapshotField(TEXT("inventory"))||P.WantsSnapshotField(TEXT("inventory_revision"))||P.WantsSnapshotField(TEXT("appraisal_revision")))ExtendItemInventorySnapshot(Result);
        Result->SetNumberField(TEXT("level"),V.Level);Result->SetNumberField(TEXT("action_serial"),ActionSerial);
        Result->SetNumberField(TEXT("action_error"),LastActionError);Result->SetNumberField(TEXT("last_spell"),LastCompletedSpell);
        Result->SetBoolField(TEXT("last_spell_confirmed"),LastSpellConfirmed);
        if(P.WantsSnapshotField(TEXT("action_events")))Result->SetArrayField(TEXT("action_events"),ActionOutcomes);
        if(auto Session=C->GetSession())
        {
            Result->SetNumberField(TEXT("inventory_failure_serial"),Session->GetInventoryFailureSerial());
            Result->SetNumberField(TEXT("inventory_failure_item"),uint32(Session->GetInventoryFailureItem()));
            Result->SetNumberField(TEXT("inventory_failure_error"),Session->GetInventoryFailureError());
        }
    }
    if(DataAllowed && P.WantsSnapshotField(TEXT("character_name")))
        if(auto Session=GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->GetSession())Result->SetStringField(TEXT("character_name"),Session->GetPlayerName());
    // Preserve the extended position (global coordinates) and merge observer data.
    for(const auto& Pair:Common->Values)if(Pair.Key!=TEXT("position")||!Result->HasField(Pair.Key))Result->SetField(Pair.Key,Pair.Value);
    if(!P.Permissions.Contains(TEXT("chat")))Result->RemoveField(TEXT("meta_chat"));
    Result->SetBoolField(TEXT("window_chrome_hidden"),IsPluginWindowChromeHidden(P.Id));
    Result->SetArrayField(TEXT("log"),P.LogEntries);
    Result->SetStringField(TEXT("profile_name"),P.ProfileName);
    Result->SetStringField(TEXT("waiting_action"),P.WaitAction);
    if(!P.ScrollPanel()&&P.PanelSize.X>0&&P.PanelSize.Y>0)
    {
        auto Panel=MakeShared<FJsonObject>();Panel->SetNumberField(TEXT("width"),P.PanelSize.X);Panel->SetNumberField(TEXT("height"),P.PanelSize.Y);
        Result->SetObjectField(TEXT("panel"),Panel);
    }
    if(P.ActivityFailure){Result->SetObjectField(TEXT("activity_failure"),P.ActivityFailure);P.ActivityFailure.Reset();}
    auto Capabilities=MakeShared<FJsonObject>();for(const auto& Permission:P.Permissions)Capabilities->SetBoolField(Permission,true);
    Result->SetObjectField(TEXT("capabilities"),Capabilities);
    auto Owner=PolicyPlugin();Result->SetStringField(TEXT("automation_owner"),Owner&&Owner->Running?Owner->Id:FString());
    if(P.SelectiveSnapshot)
    {
        static const TSet<FString> Required={TEXT("api"),TEXT("time"),TEXT("unix_time"),TEXT("player"),TEXT("server"),TEXT("state"),TEXT("error"),TEXT("events"),TEXT("capabilities")};
        for(auto It=Result->Values.CreateIterator();It;++It)
        {
            const FString Key(It.Key());
            if(!Required.Contains(Key)&&!P.SnapshotFields.Contains(Key))It.RemoveCurrent();
        }
    }
    return Result;
}

bool UACEPluginSubsystem::ApplyPublicOutput(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& O,FString& Error)
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    if(O->HasField(TEXT("runtime_route"))&&!P.Permissions.Contains(TEXT("navigation")))
    {Error=TEXT("runtime_route requires navigation permission");return false;}
    if(B(O,TEXT("helper_updates"),false)&&!P.Permissions.Contains(TEXT("fellowship")))
    {Error=TEXT("helper_updates requires fellowship permission");return false;}
    const FString Action=S(O,TEXT("action"));
    const FString Permission=Action==TEXT("profile_load")?TEXT("profiles"):
        Action==TEXT("buff_request_start")||Action==TEXT("buff_request_done")?TEXT("say"):
        Action==TEXT("force_buff_done")?TEXT("cast"):Action==TEXT("pause_navigation")?TEXT("navigation"):FString();
    if(!Permission.IsEmpty()&&!P.Permissions.Contains(Permission)){Error=TEXT("Missing permission: ")+Permission;return false;}
    if(!ApplyCompanionOutput(P,O,Error))return false;
    if(!P.Running)return true;
    const TSharedPtr<FJsonObject>* Patch=nullptr;
    if(O->HasField(TEXT("profile_patch")))
    {
        if(!P.Permissions.Contains(TEXT("profiles"))||!O->TryGetObjectField(TEXT("profile_patch"),Patch)){Error=TEXT("profile_patch requires profiles permission and an object");return false;}
        auto Updated=MakeShared<FJsonObject>(*P.Profile);
        bool Changed=false;
        for(const auto& Pair:(*Patch)->Values)
        {if(!Updated->HasField(Pair.Key)||!FJsonValue::CompareEqual(*Updated->Values[Pair.Key],*Pair.Value))Changed=true;Updated->SetField(Pair.Key,Pair.Value);}
        if(!ValidatePublicProfile(Updated)){Error=TEXT("Invalid native profile structure");return false;}
        // One patch updates runtime configuration. Persistence is an explicit save request.
        if(Changed){if(P.IsPolicy())Updated->SetNumberField(TEXT("loot_revision"),N(P.Profile,TEXT("loot_revision"))+1);P.Profile=Updated;P.ResumeMetaPending=true;if(P.IsPolicy())RouteRebuiltAt=0;}
    }
    return ApplyPluginServices(P,O,Error);
}

bool UACEPluginSubsystem::ValidatePluginUI(const TArray<TSharedPtr<FJsonValue>>& Rows,FString& Error) const
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    TSet<FString> Ids;int Count=0;
    TFunction<bool(const TArray<TSharedPtr<FJsonValue>>&,int)> Visit;
    Visit=[&](const TArray<TSharedPtr<FJsonValue>>& Children,int Depth)
    {
        if(Depth>8){Error=TEXT("UI nesting exceeds eight levels");return false;}
        for(const auto& V:Children)
        {
            if(!V||V->Type!=EJson::Object||++Count>256){Error=TEXT("UI must contain at most 256 control objects");return false;}
            auto R=V->AsObject();const FString Id=S(R,TEXT("id")),Type=S(R,TEXT("type"),S(R,TEXT("action")).IsEmpty()?TEXT("text"):TEXT("button"));
            if(Id.IsEmpty()||Id.Len()>64||Ids.Contains(Id)||S(R,TEXT("text")).Len()>4096||S(R,TEXT("tooltip")).Len()>1024)
            {Error=TEXT("UI controls need unique IDs and bounded text");return false;}Ids.Add(Id);
            if(!TArray<FString>{TEXT("text"),TEXT("button"),TEXT("checkbox"),TEXT("slider"),TEXT("number"),TEXT("input"),TEXT("select"),TEXT("color"),TEXT("progress"),TEXT("icon"),TEXT("item_icon"),TEXT("image"),TEXT("row"),TEXT("column"),TEXT("section"),TEXT("tabs"),TEXT("overlay"),TEXT("frame")}.Contains(Type))
            {Error=TEXT("Unknown UI control: ")+Type;return false;}
            for(const TCHAR* Field:{TEXT("compact"),TEXT("auto_width"),TEXT("wrap"),TEXT("clickable"),TEXT("hide_when_pinned")})
                if(R->HasField(Field)&&!R->HasTypedField<EJson::Boolean>(Field))
                {Error=FString(Field)+TEXT(" must be a boolean");return false;}
            for(const TCHAR* Field:{TEXT("width"),TEXT("height"),TEXT("opacity"),TEXT("min_opacity"),TEXT("period"),TEXT("rotation"),TEXT("outline"),TEXT("glow"),TEXT("scale"),TEXT("offset_x"),TEXT("offset_y")})
            {
                if(!R->HasField(Field))continue;
                double Value=0;const FString Key(Field);
                const double Min=Key==TEXT("period")?.1:Key==TEXT("rotation")?-360.:Key==TEXT("scale")?.25:Key.StartsWith(TEXT("offset_"))?-2048.:0.;
                const double Max=Key==TEXT("opacity")||Key==TEXT("min_opacity")?1.:Key==TEXT("period")?60.:Key==TEXT("outline")||Key==TEXT("glow")?8.:Key==TEXT("rotation")?360.:Key==TEXT("scale")?4.:2048.;
                if(!R->HasTypedField<EJson::Number>(Field)||!R->TryGetNumberField(Field,Value)||!FMath::IsFinite(Value)||Value<Min||Value>Max)
                {Error=TEXT("Invalid visual field: ")+Key;return false;}
            }
            if(!TArray<FString>{TEXT(""),TEXT("blink"),TEXT("pulse"),TEXT("rotate")}.Contains(S(R,TEXT("effect")))){Error=TEXT("Unknown visual effect");return false;}
            if(!TArray<FString>{TEXT(""),TEXT("left"),TEXT("center"),TEXT("right")}.Contains(S(R,TEXT("align")))){Error=TEXT("Unknown text alignment");return false;}
            const FString Action=S(R,TEXT("action"));
            if(!Action.IsEmpty()&&!TArray<FString>{TEXT("event"),TEXT("select"),TEXT("select_item"),TEXT("waypoint"),TEXT("command"),TEXT("content_folder")}.Contains(Action))
            {Error=TEXT("Unknown UI button action");return false;}
            if(Type==TEXT("color"))
            {
                FString Value; FLinearColor Parsed;
                if(!R->TryGetStringField(TEXT("value"),Value)||!ACEPluginColor::Parse(Value,Parsed))
                {Error=TEXT("Color controls need a named color, #RRGGBB or #RRGGBBAA value");return false;}
            }
            if(Type==TEXT("slider")||Type==TEXT("number"))
            {
                if(N(R,TEXT("min"))>N(R,TEXT("max"),100)||FMath::Abs(N(R,TEXT("min")))>1.e9||FMath::Abs(N(R,TEXT("max"),100))>1.e9||N(R,TEXT("step"),1)<=0)
                {Error=TEXT("Invalid numeric control bounds");return false;}
            }
            if(Type==TEXT("icon"))
            {const double Icon=N(R,TEXT("icon"));if(Icon<0||Icon>MAX_uint32||Icon!=FMath::FloorToDouble(Icon)){Error=TEXT("Invalid DAT icon ID");return false;}}
            if(Type==TEXT("item_icon"))
            {
                for(const TCHAR* Field:{TEXT("icon"),TEXT("overlay"),TEXT("underlay"),TEXT("effects"),TEXT("item_type")})
                {
                    if(!R->HasField(Field))continue;
                    double Value=0;
                    if(!R->HasTypedField<EJson::Number>(Field)||!R->TryGetNumberField(Field,Value)||!FMath::IsFinite(Value)||Value<0||Value>MAX_uint32||Value!=FMath::FloorToDouble(Value))
                    {Error=TEXT("Item icon fields must be unsigned 32-bit integers");return false;}
                }
            }
            if(Type==TEXT("image"))
            {const FString Asset=S(R,TEXT("asset"));if(Asset.IsEmpty()||Asset.Len()>128||Asset!=FPaths::GetCleanFilename(Asset)||!Asset.EndsWith(TEXT(".png"))){Error=TEXT("Images must name a PNG in this plugin's images folder");return false;}}
            if(Type==TEXT("select"))
            {
                const TArray<TSharedPtr<FJsonValue>>* Options=nullptr;
                if(!R->TryGetArrayField(TEXT("options"),Options)||Options->Num()>256){Error=TEXT("Select needs at most 256 string options");return false;}
                for(const auto& Option:*Options)if(Option->Type!=EJson::String||Option->AsString().Len()>256){Error=TEXT("Invalid select option");return false;}
            }
            const TArray<TSharedPtr<FJsonValue>>* Nested=nullptr;
            if(R->HasField(TEXT("children")))
            {
                if(!TArray<FString>{TEXT("row"),TEXT("column"),TEXT("section"),TEXT("tabs"),TEXT("overlay"),TEXT("frame")}.Contains(Type)||!R->TryGetArrayField(TEXT("children"),Nested)||!Visit(*Nested,Depth+1))return false;
            }
        }
        return true;
    };
    return Visit(Rows,0);
}

bool UACEPluginSubsystem::ClickPluginRow(const FString& Plugin,const FString& Id)
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    auto P=Find(Plugin);if(!P||!P->Running||!P->Enabled||!P->HasPresentation()||!P->Permissions.Contains(TEXT("ui")))return false;
    auto R=Control(P->PanelRows,Id);if(!R||!B(R,TEXT("enabled")))return false;
    const FString Type=S(R,TEXT("type"),S(R,TEXT("action")).IsEmpty()?TEXT("text"):TEXT("button"));
    if(Type!=TEXT("button")&&!B(R,TEXT("clickable"),false)&&!R->HasField(TEXT("action")))return false;
    if(B(R,TEXT("hide_when_pinned"),false)&&IsPluginWindowChromeHidden(Plugin))return false;
    FString Error;bool Ok=true;const FString Action=S(R,TEXT("action"),TEXT("event"));
    if(Action==TEXT("content_folder"))Ok=ChooseContentFolder(*P,Error);
    else if(Action!=TEXT("event"))
    {
        auto A=MakeShared<FJsonObject>(*R);if(Action==TEXT("command"))A->SetStringField(TEXT("text"),S(R,TEXT("command")));
        Ok=CompanionAction(*P,A,Error);
    }
    auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("type"),TEXT("click"));E->SetStringField(TEXT("id"),Id);E->SetBoolField(TEXT("ok"),Ok);E->SetStringField(TEXT("error"),Error);QueuePluginEvent(*P,E);
    if(!Ok)P->CompanionError=Error;return Ok;
}

bool UACEPluginSubsystem::ChangePluginControl(const FString& Plugin,const FString& Id,const TSharedPtr<FJsonValue>& Value)
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    auto P=Find(Plugin);if(!P||!P->Running||!P->Enabled||!P->Permissions.Contains(TEXT("ui"))||!Value)return false;
    auto R=Control(P->PanelRows,Id);if(!R||!B(R,TEXT("enabled")))return false;
    auto Accepted=Value;const FString Type=S(R,TEXT("type"));
    if(Type==TEXT("checkbox")){if(Value->Type!=EJson::Boolean)return false;}
    else if(Type==TEXT("color"))
    {
        FLinearColor Parsed;
        if(Value->Type!=EJson::String||!ACEPluginColor::Parse(Value->AsString(),Parsed))return false;
        Accepted=MakeShared<FJsonValueString>(ACEPluginColor::Encode(Parsed));
    }
    else if(Type==TEXT("input")||Type==TEXT("select"))
    {
        if(Value->Type!=EJson::String||Value->AsString().Len()>1024)return false;
        if(Type==TEXT("select"))
        {const TArray<TSharedPtr<FJsonValue>>* Options=nullptr;if(!R->TryGetArrayField(TEXT("options"),Options)||!Options->ContainsByPredicate([&](const auto& V){return V->AsString()==Value->AsString();}))return false;}
    }
    else if(Type==TEXT("slider")||Type==TEXT("number"))
    {
        if(Value->Type!=EJson::Number||!FMath::IsFinite(Value->AsNumber()))return false;
        const double Lo=N(R,TEXT("min")),Hi=N(R,TEXT("max"),100),Step=N(R,TEXT("step"),1);
        Accepted=MakeShared<FJsonValueNumber>(FMath::Clamp(Lo+FMath::RoundToDouble((FMath::Clamp(Value->AsNumber(),Lo,Hi)-Lo)/Step)*Step,Lo,Hi));
    }
    else return false;
    R->SetField(TEXT("value"),Accepted);
    // All open views should see the accepted (possibly snapped/clamped) value.
    // A revision updates retained controls; it no longer replaces the panel.
    ++P->PanelRevision;
    auto E=MakeShared<FJsonObject>();E->SetStringField(TEXT("type"),TEXT("change"));E->SetStringField(TEXT("id"),Id);E->SetField(TEXT("value"),Accepted);QueuePluginEvent(*P,E);return true;
}

bool UACEPluginSubsystem::ApplyPluginServices(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& O,FString& Error)
{
    using namespace ACEPluginPublicAPIPrivate; using ACEPluginPublicAPIPrivate::S; using ACEPluginPublicAPIPrivate::N;
    const TArray<TSharedPtr<FJsonValue>>* Requests=nullptr;
    if(!O->HasField(TEXT("requests")))return true;
    if(!O->TryGetArrayField(TEXT("requests"),Requests)||Requests->Num()>4){Error=TEXT("At most four service requests per callback");return false;}
    for(const auto& V:*Requests)if(V->Type!=EJson::Object||S(V->AsObject(),TEXT("id")).IsEmpty()||S(V->AsObject(),TEXT("id")).Len()>64)
    {Error=TEXT("Service requests need a short correlation ID");return false;}
    const double Now=FPlatformTime::Seconds();const bool Limited=Now<P.NextService;
    if(!Requests->IsEmpty()&&!Limited)P.NextService=Now+.5;
    for(const auto& V:*Requests)
    {
        auto R=V->AsObject();auto Reply=MakeShared<FJsonObject>();const FString Service=S(R,TEXT("service")),Op=S(R,TEXT("operation")),Key=S(R,TEXT("name"));
        Reply->SetStringField(TEXT("type"),TEXT("result"));Reply->SetStringField(TEXT("id"),S(R,TEXT("id")));bool Ok=false;FString Failure;
        const FString Permission=Service==TEXT("sound")?TEXT("sound"):Service==TEXT("content")?TEXT("admin_content"):Service==TEXT("profiles")||Service==TEXT("import")?TEXT("profiles"):
            Service==TEXT("waypoint")?TEXT("waypoint"):Service==TEXT("route")?TEXT("navigation"):
            Service==TEXT("force_buff")?TEXT("cast"):(Service==TEXT("window")||Service==TEXT("notice"))?TEXT("ui"):Service==TEXT("policy")?TEXT("commands"):Service==TEXT("chat")?TEXT("say"):FString();
        if(Limited)Failure=TEXT("Service rate limit; retry after 0.5 seconds");
        else if(!Permission.IsEmpty()&&!P.Permissions.Contains(Permission))Failure=TEXT("Missing permission: ")+Permission;
        else if(Service==TEXT("content"))Ok=ContentService(P,R,Reply,Failure);
        else if(Service==TEXT("sound")&&Op==TEXT("play"))
        {
            const FString Asset=S(R,TEXT("asset"));const FString Path=P.Directory/TEXT("sounds")/Asset;
            const int64 Bytes=IFileManager::Get().FileSize(*Path);TArray<uint8> Data;FWaveModInfo Wave;
            if(Asset.IsEmpty()||Asset.Len()>128||Asset!=FPaths::GetCleanFilename(Asset)||Asset.Contains(TEXT(":"))||!Asset.EndsWith(TEXT(".wav"))
                ||Bytes<=0||Bytes>1024*1024||!FFileHelper::LoadFileToArray(Data,*Path)||!Wave.ReadWaveInfo(Data.GetData(),Data.Num())
                ||!Wave.pFormatTag||*Wave.pFormatTag!=1||!Wave.pChannels||*Wave.pChannels<1||*Wave.pChannels>2
                ||!Wave.pBitsPerSample||*Wave.pBitsPerSample!=16||!Wave.pSamplesPerSec||*Wave.pSamplesPerSec<8000||*Wave.pSamplesPerSec>48000
                ||!Wave.SampleDataSize||Wave.SampleDataSize%(*Wave.pChannels*2u)!=0
                ||Wave.SampleDataSize>10u*(*Wave.pSamplesPerSec)*(*Wave.pChannels)*2u)
                Failure=TEXT("Sound must be a bundled PCM16 mono/stereo WAV, 8-48 kHz, at most 10 seconds and 1 MiB");
            else
            {
                auto* Sound=NewObject<USoundWaveProcedural>(this);Sound->SetSampleRate(*Wave.pSamplesPerSec);Sound->NumChannels=*Wave.pChannels;
                Sound->Duration=float(Wave.SampleDataSize)/(*Wave.pSamplesPerSec*Sound->NumChannels*2);Sound->bLooping=false;
                Sound->QueueAudio(Wave.SampleDataStart,Wave.SampleDataSize);
                if(auto* Audio=UGameplayStatics::SpawnSound2D(GetWorld(),Sound,FMath::Clamp(N(R,TEXT("volume"),1),0.,1.)))
                {
                    // Procedural sources otherwise remain alive generating silence.
                    FTimerHandle Stop;GetWorld()->GetTimerManager().SetTimer(Stop,FTimerDelegate::CreateWeakLambda(Audio,[Audio](){Audio->Stop();}),Sound->Duration+.1f,false);
                }
                Ok=true;
            }
        }
        else if(Service==TEXT("profiles"))
        {
            const FString Group=S(R,TEXT("group"),TEXT("Default"));
            if(!IsValidPluginStorageName(Group)||(Op!=TEXT("list")&&!IsValidPluginStorageName(Key)))Failure=TEXT("Profile group/name must use letters, digits, _ or - (1-64 characters)");
            else
            {
                const FString Root=UserDirectory()/TEXT("Profiles")/P.Id/TEXT("Library")/Group,Path=Root/(Key+TEXT(".json"));
                if(Op==TEXT("list"))
                {TArray<FString> Files;IFileManager::Get().FindFiles(Files,*(Root/TEXT("*.json")),true,false);Files.Sort();TArray<TSharedPtr<FJsonValue>> Names;for(const auto& F:Files){Names.Add(MakeShared<FJsonValueString>(FPaths::GetBaseFilename(F)));if(Names.Num()>=256)break;}Reply->SetArrayField(TEXT("names"),Names);Ok=true;}
                else if(Op==TEXT("save"))
                {const TSharedPtr<FJsonObject>* Data=nullptr;auto Saved=R->TryGetObjectField(TEXT("data"),Data)?*Data:P.Profile;Ok=ValidatePublicProfile(Saved)&&Write(Path,Saved);}
                else if(Op==TEXT("read")||Op==TEXT("load"))
                {TSharedPtr<FJsonObject> Data;Ok=Read(Path,Data)&&ValidatePublicProfile(Data);if(Ok){if(Op==TEXT("read")){const FString Field=S(R,TEXT("field"));if(Field.IsEmpty())Reply->SetObjectField(TEXT("data"),Data);else if(auto Value=Data->TryGetField(Field))Reply->SetField(TEXT("data"),Value);else Ok=false;}else{Ok=SaveProfile(P.Id,Key,Encode(Data),false);if(Ok){Reply->SetStringField(TEXT("profile_name"),Key);P.ResumeMetaPending=true;if(P.IsPolicy()){RuntimeRoute.Reset();RouteRebuiltAt=0;}}}}}
                else if(Op==TEXT("delete"))
                {const FString Archive=UserDirectory()/TEXT("Profiles")/P.Id/TEXT("Deleted")/Group;IFileManager::Get().MakeDirectory(*Archive,true);Ok=IFileManager::Get().Move(*(Archive/(Key+TEXT("_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".json"))),*Path,false,true);}
                else Failure=TEXT("Unknown profile operation");
            }
        }
        else if(Service==TEXT("import"))
        {
            if(Op==TEXT("list"))
            {TArray<TSharedPtr<FJsonValue>> Names;for(const TCHAR* Ext:{TEXT("nav"),TEXT("utl"),TEXT("met"),TEXT("usd")}){for(const auto& F:GetProfileFiles(Ext)){if(Names.Num()>=256)break;Names.Add(MakeShared<FJsonValueString>(F));}if(Names.Num()>=256)break;}Reply->SetArrayField(TEXT("files"),Names);Ok=true;}
            else if((Op==TEXT("convert")||Op==TEXT("inspect")||Op==TEXT("activate"))&&Key==FPaths::GetCleanFilename(Key)&&GetProfileFiles(FPaths::GetExtension(Key).ToLower()).Contains(Key))
            {
                TArray<FString> Issues;TSharedPtr<FJsonObject> Converted;
                if(Op==TEXT("inspect")){FString Text;const FString Path=GetProfileFolder()/Key;if(IFileManager::Get().FileSize(*Path)>8*1024*1024)Failure=TEXT("Import exceeds 8 MiB");else if(FFileHelper::LoadFileToString(Text,*Path))Converted=ACEVTProfile::Read(Text,FPaths::GetExtension(Key).ToLower(),Failure);}
                else Converted=ACEVTProfile::ConvertFile(GetProfileFolder()/Key,Issues);
                TArray<TSharedPtr<FJsonValue>> Messages;for(const auto& Issue:Issues)Messages.Add(MakeShared<FJsonValueString>(Issue));Reply->SetArrayField(TEXT("issues"),Messages);
                Ok=Converted.IsValid()&&Issues.IsEmpty();
                if(Op==TEXT("activate"))
                {
                    const FString Destination=S(R,TEXT("profile_name"));
                    if(!IsValidPluginStorageName(Destination)){Ok=false;Failure=TEXT("Activate requires a safe profile_name");}
                    else if(Ok)
                    {
                        auto Updated=MakeShared<FJsonObject>(*P.Profile);for(const auto& Pair:Converted->Values)Updated->SetField(Pair.Key,Pair.Value);
                        Ok=SaveProfile(P.Id,Destination,Encode(Updated),false);
                        if(Ok){Reply->SetStringField(TEXT("profile_name"),Destination);P.ResumeMetaPending=true;if(P.IsPolicy()){RuntimeRoute.Reset();RouteRebuiltAt=0;}}
                        else Failure=Notice;
                    }
                }
                else if(Converted)Reply->SetObjectField(TEXT("data"),Converted);
            }
            else if(Op==TEXT("export_loot")&&IsValidPluginStorageName(Key))
            {
                const TSharedPtr<FJsonObject>* Document=nullptr;
                if(R->TryGetObjectField(TEXT("data"),Document))
                {
                    const FString Text=ACEVTProfile::WriteLoot(*Document,Failure),Root=UserDirectory()/TEXT("Exports")/P.Id;
                    FString Filename=Key+TEXT(".utl");if(IFileManager::Get().FileExists(*(Root/Filename)))Filename=Key+TEXT("_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".utl");
                    IFileManager::Get().MakeDirectory(*Root,true);Ok=Failure.IsEmpty()&&FFileHelper::SaveStringToFile(Text,*(Root/Filename),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
                    if(Ok)Reply->SetStringField(TEXT("file"),Filename);
                }
            }
            else Failure=TEXT("Choose a supported file in the configured import folder");
        }
        else if(Service==TEXT("log"))
        {
            if(Op==TEXT("clear")){P.LogEntries.Reset();Ok=true;}
            else if(Op==TEXT("append"))
            {auto Line=MakeShared<FJsonObject>();Line->SetNumberField(TEXT("time"),FDateTime::UtcNow().ToUnixTimestamp());Line->SetStringField(TEXT("level"),S(R,TEXT("level"),TEXT("info")).Left(16));Line->SetStringField(TEXT("text"),S(R,TEXT("text")).Left(1024));if(P.LogEntries.Num()>=200)P.LogEntries.RemoveAt(0);P.LogEntries.Add(MakeShared<FJsonValueObject>(Line));Ok=true;}
        }
        else if(Service==TEXT("policy")&&P.IsPolicy()&&Op==TEXT("command"))
        {
            TArray<FString> Issues;auto Command=ACEVTProfile::CompileCommand(S(R,TEXT("text")),Issues);
            Ok=Command.IsValid()&&Issues.IsEmpty()&&UCMCommands.Num()<32;
            if(Ok)UCMCommands.Add(MakeShared<FJsonValueObject>(Command));else Failure=FString::Join(Issues,TEXT("; "));
        }
        else if(Service==TEXT("window"))
        {
            if(Op==TEXT("show")||Op==TEXT("hide")||Op==TEXT("toggle"))
            {const bool Open=IsPluginWindowOpen(P.Id);if(Op==TEXT("toggle")||Open!=(Op==TEXT("show")))TogglePluginWindow(P.Id);Ok=true;}
        }
        else if(Service==TEXT("notice")&&Op==TEXT("show"))
        {
            const FString Text=S(R,TEXT("text"));
            if(!Text.IsEmpty()&&Text.Len()<=500&&!Text.Contains(TEXT("\n"))&&!Text.Contains(TEXT("\r")))
            {
                auto Rule=MakeShared<FJsonObject>();Rule->SetStringField(TEXT("id"),TEXT("notice"));Rule->SetStringField(TEXT("exact"),Text);
                Rule->SetStringField(TEXT("action"),TEXT("show"));
                if(R->HasField(TEXT("window")))Rule->SetField(TEXT("window"),R->TryGetField(TEXT("window")));
                if(R->HasField(TEXT("color")))Rule->SetField(TEXT("color"),R->TryGetField(TEXT("color")));
                TArray<FACEPluginChatFilter> Parsed;
                if(!ValidatePluginChatFilters({MakeShared<FJsonValueObject>(Rule)},Parsed,Failure))
                {Reply->SetBoolField(TEXT("ok"),false);Reply->SetStringField(TEXT("error"),Failure);QueuePluginEvent(P,Reply);continue;}
                // Display-only notification. Never use the server chat path or
                // the host's shared Notice string for plugin-owned messages.
                auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
                C->ReceiveChatMessage(TEXT("[")+P.Name.Left(80)+TEXT("] ")+Text,TEXT(""),ACEChatMessageType::Broadcast,ACEChatMessageType::Plugin,
                    R->HasField(TEXT("window"))?Parsed[0].Windows:0,Parsed[0].Color);
                Ok=true;
            }
            else Failure=TEXT("A local notice requires one line, 1-500 characters");
        }
        else if(Service==TEXT("chat")&&Op==TEXT("send"))
        {
            auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();const FString Text=S(R,TEXT("text")),Recipient=S(R,TEXT("recipient"));
            if(!Text.IsEmpty()&&Text.Len()<=500&&!Text.Contains(TEXT("\n"))&&!Text.Contains(TEXT("\r"))&&Recipient.Len()<=80&&!Recipient.Contains(TEXT("\n"))&&!Recipient.Contains(TEXT("\r"))&&Now>=P.NextCommand)
            {P.NextCommand=Now+1;if(!Recipient.IsEmpty())C->SendTell(Recipient,Text);else if(N(R,TEXT("channel"))==2048)C->SendChatChannel(2048,Text);else if(auto Session=C->GetSession())Session->SendTalk(Text);Ok=true;}
            else Failure=TEXT("Chat requires one line, 1-500 characters, at most one send per second");
        }
        else if(Service==TEXT("waypoint"))
        {
            if(Op==TEXT("clear")){ClearWaypoint();Ok=true;}
            else if(Op==TEXT("set")){Ok=IsWaypointEnabled()&&SetWaypoint(S(R,TEXT("coordinates")));}
        }
        else if(Service==TEXT("route")&&!P.Companion)
        {
            if(Op==TEXT("record"))
            {
                auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEPosition Pos=C->GetPlayerPosition();
                if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))PC->TryGetLocallyPredictedPosition(Pos);
                const TArray<TSharedPtr<FJsonValue>>* Current=nullptr;TArray<TSharedPtr<FJsonValue>> Points;if(P.Profile->TryGetArrayField(TEXT("route"),Current))Points=*Current;
                const FString Kind=S(R,TEXT("kind"),TEXT("walk"));
                if(Pos.IsValid()&&Points.Num()<2048&&TArray<FString>{TEXT("walk"),TEXT("jump"),TEXT("pause"),TEXT("use"),TEXT("portal")}.Contains(Kind))
                {
                    auto Point=MakeShared<FJsonObject>();Point->SetNumberField(TEXT("cell"),uint32(Pos.CellId));Point->SetNumberField(TEXT("x"),Pos.Location.X);Point->SetNumberField(TEXT("y"),Pos.Location.Y);Point->SetNumberField(TEXT("z"),Pos.Location.Z);Point->SetStringField(TEXT("kind"),Kind);
                    Point->SetNumberField(TEXT("charge"),FMath::Clamp(N(R,TEXT("charge"),.5),0.,1.));Point->SetNumberField(TEXT("seconds"),FMath::Clamp(N(R,TEXT("seconds"),5),0.,3600.));
                    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))Point->SetNumberField(TEXT("heading"),PC->GetPawn()?PC->GetPawn()->GetActorRotation().Yaw:0);
                    FACEWorldObject Target;const bool Use=Kind==TEXT("use")||Kind==TEXT("portal");
                    if(!Use||(C->GetWorldObject(C->GetSelectedObject().Guid,Target)&&Target.IsSelectableWorldObject()))
                    {
                        if(Use){Point->SetStringField(TEXT("object_name"),Target.Name);Point->SetNumberField(TEXT("wcid"),Target.WeenieClassId);}
                        Points.Add(MakeShared<FJsonValueObject>(Point));P.Profile->SetArrayField(TEXT("route"),Points);RuntimeRoute.Reset();RouteRebuiltAt=0;Ok=true;Reply->SetObjectField(TEXT("point"),Point);
                    }
                }
            }
        }
        else if(Service==TEXT("force_buff")&&P.IsPolicy())
        {if(Op==TEXT("start")){ForceBuffRequest=++ForceBuffSerial;bForceBuffOnly=B(R,TEXT("only"),false);Ok=true;}else if(Op==TEXT("cancel")){const bool Only=bForceBuffOnly;ForceBuffRequest=0;bForceBuffOnly=false;if(Only)Stop(P.Id,TEXT("Force Buff cancelled"),true);Ok=true;}}
        else Failure=TEXT("Unknown or unavailable service");
        if(!Ok&&Failure.IsEmpty())Failure=TEXT("Request failed; data/file may be missing, invalid, or too large");
        Reply->SetBoolField(TEXT("ok"),Ok);Reply->SetStringField(TEXT("error"),Failure);
        // Never allow imported files or profile libraries to exhaust a VM on its next callback.
        if(FTCHARToUTF8(*Encode(Reply)).Length()>256*1024){Reply->RemoveField(TEXT("data"));Reply->SetBoolField(TEXT("ok"),false);Reply->SetStringField(TEXT("error"),TEXT("Result exceeds 256 KiB; split the profile"));}
        QueuePluginEvent(P,Reply);
    }
    return true;
}
