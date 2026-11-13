#include "Mods/ACEPluginSubsystem.h"
#include "ACEClientSubsystem.h"
#include "Engine/GameInstance.h"

TArray<TSharedPtr<FJsonValue>> UACEPluginSubsystem::BuffCommands() const
{
    const TArray<TSharedPtr<FJsonValue>>* Commands=nullptr;
    if(auto P=PolicyPlugin();P&&P->Profile->TryGetArrayField(TEXT("buff_commands"),Commands))return *Commands;
    TArray<TSharedPtr<FJsonValue>> Defaults;
    for(const TCHAR* Role:{TEXT("mage"),TEXT("heavy"),TEXT("missile"),TEXT("light"),TEXT("finesse"),TEXT("twohanded"),TEXT("unarmed"),TEXT("melee")})
    {
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("keyword"),Role);O->SetStringField(TEXT("role"),Role);
        Defaults.Add(MakeShared<FJsonValueObject>(O));
    }
    return Defaults;
}
void UACEPluginSubsystem::ClearBuffRequests()
{
    for(const auto& V:BuffRequests){const auto O=V->AsObject();ReplyBuffRequest(int32(uint32(O->GetNumberField(TEXT("player")))),O->GetStringField(TEXT("name")),TEXT("UCM: your buff request was cancelled; the queue was cleared."));}
    BuffRequests.Empty();LastBuffRequest.Empty();BuffRequestStatus.Empty();
}
void UACEPluginSubsystem::ReplyBuffRequest(int32 Player,const FString& Name,const FString& Message,bool RateLimit)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!C||C->GetSessionState()!=EACESessionState::InWorld||Name.IsEmpty())return;
    const double Now=FPlatformTime::Seconds();
    for(auto It=LastBuffReply.CreateIterator();It;++It)if(Now-It.Value()>300)It.RemoveCurrent();
    if(RateLimit){if(const auto* Last=LastBuffReply.Find(Player);Last&&Now-*Last<10)return;if(LastBuffReply.Num()>=256)return;}
    LastBuffReply.Add(Player,Now);C->SendTell(Name,Message);
}
FString UACEPluginSubsystem::BuffQueueSummary() const
{
    FString Summary;
    for(int32 Index=0;Index<BuffRequests.Num();++Index)
    {
        const auto O=BuffRequests[Index]->AsObject();bool Started=false;O->TryGetBoolField(TEXT("started"),Started);
        Summary+=FString::Printf(TEXT("%s%d. %s - %s (%s)"),Index?TEXT("\n"):TEXT(""),Index+1,*O->GetStringField(TEXT("name")),*O->GetStringField(TEXT("role")),Started?TEXT("buffing"):TEXT("waiting"));
    }
    return Summary.IsEmpty()?TEXT("No buff requests queued."):Summary;
}
void UACEPluginSubsystem::NotifyBuffQueuePositions()
{
    for(int32 Index=0;Index<BuffRequests.Num();++Index)
    {
        const auto O=BuffRequests[Index]->AsObject();
        ReplyBuffRequest(int32(uint32(O->GetNumberField(TEXT("player")))),O->GetStringField(TEXT("name")),FString::Printf(TEXT("UCM: your buff request is now #%d in the queue (%d ahead of you)."),Index+1,Index));
    }
}

void UACEPluginSubsystem::ObservePlayerTell(const FString& Text,const FString& Sender,int32 SenderId)
{
    QueueFellowshipTell(Text,Sender,SenderId);
    auto P=PolicyPlugin();auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    bool Enabled=false,Buffing=true;
    if(!P||!P->Enabled||!P->Running||C->GetSessionState()!=EACESessionState::InWorld)return;
    P->Profile->TryGetBoolField(TEXT("buff_others"),Enabled);P->Profile->TryGetBoolField(TEXT("buffing"),Buffing);
    if((P->Api>=2&&!P->Permissions.Contains(TEXT("say")))||!Enabled||!Buffing||SenderId==C->GetPlayerGuid()||uint32(SenderId)<0x50000001u||uint32(SenderId)>0x6fffffffu)return;
    const FString Keyword=Text.TrimStartAndEnd().ToLower();FString Role;
    for(const auto& V:BuffCommands())
    {
        const auto O=V->AsObject();FString K,R;
        if(O&&O->TryGetStringField(TEXT("keyword"),K)&&O->TryGetStringField(TEXT("role"),R)&&K.TrimStartAndEnd().ToLower()==Keyword)
        {Role=R;break;}
    }
    if(!TArray<FString>{TEXT("mage"),TEXT("heavy"),TEXT("missile"),TEXT("light"),TEXT("finesse"),TEXT("twohanded"),TEXT("unarmed"),TEXT("melee")}.Contains(Role))return;
    const double Now=FPlatformTime::Seconds();
    for(auto It=LastBuffRequest.CreateIterator();It;++It)if(Now-It.Value()>300)It.RemoveCurrent();
    for(int32 Index=0;Index<BuffRequests.Num();++Index)if(uint32(BuffRequests[Index]->AsObject()->GetNumberField(TEXT("player")))==uint32(SenderId))
    {ReplyBuffRequest(SenderId,Sender,FString::Printf(TEXT("UCM: you already have a buff request at #%d in the queue (%d ahead of you)."),Index+1,Index),true);return;}
    if(BuffRequests.Num()>=8||LastBuffRequest.Num()>=256){ReplyBuffRequest(SenderId,Sender,TEXT("UCM: the buff queue is full. Please try again shortly."),true);return;}
    if(const auto* Last=LastBuffRequest.Find(SenderId);Last&&Now-*Last<120){ReplyBuffRequest(SenderId,Sender,TEXT("UCM: please wait two minutes between buff requests."),true);return;}
    auto O=MakeShared<FJsonObject>();O->SetNumberField(TEXT("id"),++BuffRequestSerial);
    O->SetNumberField(TEXT("player"),uint32(SenderId));O->SetStringField(TEXT("name"),Sender.Left(128));O->SetStringField(TEXT("role"),Role);
    O->SetNumberField(TEXT("expires"),Now+900);BuffRequests.Add(MakeShared<FJsonValueObject>(O));LastBuffRequest.Add(SenderId,Now);
    BuffRequestStatus=FString::Printf(TEXT("Queued %s buffs for %s (%d waiting)"),*Role,*Sender,BuffRequests.Num());
    ReplyBuffRequest(SenderId,Sender,FString::Printf(TEXT("UCM: %s buffs queued at #%d (%d ahead of you). I will tell you when it is your turn."),*Role,BuffRequests.Num(),BuffRequests.Num()-1));
}

void UACEPluginSubsystem::QueueFellowshipTell(const FString& Text,const FString& Sender,int32 SenderId,bool Local)
{
    auto P=PolicyPlugin();auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();bool Enabled=false;
    if(!P||!P->Enabled||!P->Running||C->GetSessionState()!=EACESessionState::InWorld)return;
    (UCMServicesProfile?UCMServicesProfile:P->Profile)->TryGetBoolField(TEXT("auto_fellowship"),Enabled);
    if(!Enabled||!P->Permissions.Contains(TEXT("fellowship"))||!P->Permissions.Contains(TEXT("say"))||!SenderId||Sender.IsEmpty()||Sender.Len()>128||Text.Len()>256)return;
    if(uint32(SenderId)<0x50000001u||uint32(SenderId)>0x6fffffffu||(!Local&&SenderId==C->GetPlayerGuid()))return;
    const FString Command=Text.TrimStartAndEnd().ToLower();
    if(!TArray<FString>{TEXT("xp"),TEXT("remove"),TEXT("line"),TEXT("list"),TEXT("status"),TEXT("leader"),TEXT("location"),TEXT("help")}.Contains(Command)
        &&!Command.StartsWith(TEXT("help "))&&!Command.StartsWith(TEXT("startvote "))&&!Command.StartsWith(TEXT("vote "))
        &&!(Local&&(Command==TEXT("clear")||Command.StartsWith(TEXT("unban ")))))return;
    const double Now=FPlatformTime::Seconds();
    FellowshipTells.RemoveAll([&](const auto& V){return Now-V->AsObject()->GetNumberField(TEXT("time"))>180;});
    if(FellowshipTells.Num()>=64)FellowshipTells.RemoveAt(0);
    auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("serial"),++FellowshipTellSerial);E->SetNumberField(TEXT("time"),Now);
    E->SetNumberField(TEXT("id"),uint32(SenderId));E->SetStringField(TEXT("name"),Sender);E->SetStringField(TEXT("text"),Text.TrimStartAndEnd());E->SetBoolField(TEXT("local"),Local);
    FellowshipTells.Add(MakeShared<FJsonValueObject>(E));ScheduleDecisionWake(Now);
}
