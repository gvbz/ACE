#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginClientBus.h"
#include "ACEClientSubsystem.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    constexpr bool Available=PLATFORM_WINDOWS||PLATFORM_LINUX;
    FString S(const TSharedPtr<FJsonObject>& O,const TCHAR* K)
    {FString V;if(O)O->TryGetStringField(K,V);return V;}
    bool Name(const FString& V)
    {
        if(V.IsEmpty()||V.Len()>64)return false;
        for(TCHAR C:V)if(!((C>='a'&&C<='z')||(C>='A'&&C<='Z')||(C>='0'&&C<='9')||C=='_'||C=='-'||C=='.'))return false;
        return true;
    }
    bool Depth(const TSharedPtr<FJsonValue>& V,int32 Level=0)
    {
        if(!V||Level>10)return false;
        if(V->Type==EJson::Object){for(const auto& Pair:V->AsObject()->Values)if(!Depth(Pair.Value,Level+1))return false;}
        else if(V->Type==EJson::Array){for(const auto& Item:V->AsArray())if(!Depth(Item,Level+1))return false;}
        else if(V->Type==EJson::Number&&!FMath::IsFinite(V->AsNumber()))return false;
        return true;
    }
}
void UACEPluginSubsystem::ResetClientMessaging(FACEClientPlugin& P)
{
    P.ClientChannels.Reset();P.ClientChannelEpoch.Empty();P.ClientChannelError.Empty();
    P.ClientMessageWindow=0;P.ClientMessageCount=0;
    P.PluginEvents.RemoveAll([](const auto& V){return S(V->AsObject(),TEXT("type"))==TEXT("client_message");});
}
bool UACEPluginSubsystem::TickClientMessaging(float)
{
    if(!Available)return true;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    const bool InWorld=Session&&C->GetSessionState()==EACESessionState::InWorld&&C->GetPlayerGuid()!=0;
    const FString Scope=InWorld?FMD5::HashAnsiString(*Session->GetClientChannelScope()):FString();
    if(!InWorld||ClientBusSession.Pin()!=Session||ClientBusPlayer!=C->GetPlayerGuid()||ClientBusScope!=Scope)
    {
        // Never carry subscriptions or queued commands across login/character/server boundaries.
        ClientBus.Reset();for(auto& P:Plugins)ResetClientMessaging(*P);
        ClientBusSession=Session;ClientBusPlayer=InWorld?C->GetPlayerGuid():0;ClientBusScope=Scope;
    }
    if(!InWorld)return true;
    TArray<TSharedPtr<FJsonValue>> Subs;
    for(auto& P:Plugins)
    {
        if(!P->Enabled||!P->Running||P->Api<2||!P->Permissions.Contains(TEXT("client_messaging"))
            ||P->VMSession.Pin()!=Session||P->Player!=C->GetPlayerGuid()||P->Server!=C->GetServerName())
        {ResetClientMessaging(*P);continue;}
        for(const auto& Channel:P->ClientChannels)
        {
            auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("plugin"),P->Id);O->SetStringField(TEXT("channel"),Channel);
            O->SetStringField(TEXT("epoch"),P->ClientChannelEpoch);Subs.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    if(Subs.IsEmpty()){ClientBus.Reset();return true;}
    if(!ClientBus)
    {
        const FString Space=FMD5::HashAnsiString(*(FString(FPlatformProcess::UserSettingsDir())+TEXT("/ACE.ClientChannels.1/")+FPlatformProcess::UserName()));
        // Different OS accounts use separate relay ports/namespaces; this is not
        // authentication against other software running on the same computer.
        const uint16 Port=uint16(37000+FCrc::StrCrc32(*Space)%1000);
        ClientBus=MakeShared<FACEPluginClientBus>(Space,Port);
    }
    ClientBus->Configure(Scope,Session->GetPlayerName(),uint32(C->GetPlayerGuid()),Subs);
    ClientBus->Tick(FPlatformTime::Seconds());
    for(const auto& E:ClientBus->Drain())
    {
        auto P=Find(S(E,TEXT("plugin")));
        if(!P||!P->Running||!P->Enabled||!P->Permissions.Contains(TEXT("client_messaging"))
            ||P->ClientChannelEpoch!=S(E,TEXT("epoch"))||!P->ClientChannels.Contains(S(E,TEXT("channel"))))continue;
        E->RemoveField(TEXT("epoch"));QueuePluginEvent(*P,E);
        if(P->IsPolicy())ScheduleDecisionWake(FPlatformTime::Seconds());
    }
    return true;
}
TSharedPtr<FJsonObject> UACEPluginSubsystem::ClientMessagingSnapshot(const FACEClientPlugin& P) const
{
    auto O=MakeShared<FJsonObject>();O->SetBoolField(TEXT("available"),Available);
    O->SetBoolField(TEXT("connected"),ClientBus&&ClientBus->Connected());
    O->SetStringField(TEXT("instance"),ClientBus?ClientBus->Instance():FString());
    O->SetStringField(TEXT("error"),!P.ClientChannelError.IsEmpty()?P.ClientChannelError:ClientBus?ClientBus->Error():Available?FString():TEXT("Local client channels require Windows or Linux"));
    O->SetArrayField(TEXT("peers"),ClientBus?ClientBus->Peers(P.Id):TArray<TSharedPtr<FJsonValue>>());
    TArray<TSharedPtr<FJsonValue>> Channels;for(const auto& V:P.ClientChannels)Channels.Add(MakeShared<FJsonValueString>(V));
    O->SetArrayField(TEXT("channels"),Channels);return O;
}
bool UACEPluginSubsystem::ApplyClientMessaging(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& O,FString& Error)
{
    if(!O->HasField(TEXT("client_channels"))&&!O->HasField(TEXT("client_messages")))return true;
    if(P.Api<2||!P.Permissions.Contains(TEXT("client_messaging")))
    {Error=TEXT("client_channels/client_messages require API 2 and client_messaging permission");return false;}
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!P.Enabled||!P.Running||!C->GetSession()||C->GetSessionState()!=EACESessionState::InWorld
        ||P.Player!=C->GetPlayerGuid()||P.Server!=C->GetServerName()||P.VMSession.Pin()!=C->GetSession())
    {Error=TEXT("Client channels require the running plugin's current character session");return false;}
    TArray<FString> Channels=P.ClientChannels;const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
    if(O->HasField(TEXT("client_channels")))
    {
        if(!O->TryGetArrayField(TEXT("client_channels"),Values)||Values->Num()>8){Error=TEXT("client_channels must contain at most 8 channel names");return false;}
        Channels.Reset();for(const auto& V:*Values)
        {if(V->Type!=EJson::String||!Name(V->AsString())){Error=TEXT("Channel names use 1-64 ASCII letters, digits, dot, dash or underscore");return false;}Channels.AddUnique(V->AsString());}
        Channels.Sort();
    }
    int32 Total=Channels.Num();for(const auto& Peer:Plugins)if(Peer.Get()!=&P)Total+=Peer->ClientChannels.Num();
    if(Total>64){Error=TEXT("This client supports at most 64 channel subscriptions");return false;}
    const TArray<TSharedPtr<FJsonValue>>* Messages=nullptr;
    if(O->HasField(TEXT("client_messages")))
    {
        if(!O->TryGetArrayField(TEXT("client_messages"),Messages)||Messages->Num()>8){Error=TEXT("client_messages must contain at most 8 messages");return false;}
        for(const auto& V:*Messages)
        {
            const auto M=V->AsObject();const TSharedPtr<FJsonObject>* Data=nullptr;
            if(!M||!Channels.Contains(S(M,TEXT("channel")))||!M->TryGetObjectField(TEXT("data"),Data)
                ||(M->HasField(TEXT("to"))&&(M->TryGetField(TEXT("to"))->Type!=EJson::String||!Name(S(M,TEXT("to"))))))
            {Error=TEXT("Each client message needs a subscribed channel, object data, and optional peer instance in to");return false;}
            FString Json;FJsonSerializer::Serialize(Data->ToSharedRef(),TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
            if(FTCHARToUTF8(*Json).Length()>4096||!Depth(MakeShared<FJsonValueObject>(*Data)))
            {Error=TEXT("Client message data exceeds 4096 UTF-8 bytes or 10 nested levels");return false;}
        }
    }
    if(!Available){P.ClientChannelError=TEXT("Local client channels require Windows or Linux");return true;}
    // Establish/refresh context before changing subscriptions (the first tick
    // after login deliberately clears state belonging to the previous session).
    TickClientMessaging(0);
    if(Channels!=P.ClientChannels||P.ClientChannelEpoch.IsEmpty())
    {
        P.ClientChannels=Channels;P.ClientChannelEpoch=FGuid::NewGuid().ToString(EGuidFormats::Digits);
        P.PluginEvents.RemoveAll([](const auto& V){return S(V->AsObject(),TEXT("type"))==TEXT("client_message");});
    }
    TickClientMessaging(0);
    if(Messages&&!Messages->IsEmpty())
    {
        const double Now=FPlatformTime::Seconds();if(Now-P.ClientMessageWindow>=1){P.ClientMessageWindow=Now;P.ClientMessageCount=0;}
        if(Now-ClientBusSendWindow>=1){ClientBusSendWindow=Now;ClientBusSendCount=0;}
        if(P.ClientMessageCount+Messages->Num()>20){P.ClientChannelError=TEXT("Client channel rate limit: 20 messages/second; batch not sent");return true;}
        if(ClientBusSendCount+Messages->Num()>80){P.ClientChannelError=TEXT("Client channel rate limit: 80 messages/second across plugins; batch not sent");return true;}
        P.ClientMessageCount+=Messages->Num();ClientBusSendCount+=Messages->Num();P.ClientChannelError.Empty();
        for(const auto& V:*Messages)
        {
            const auto M=V->AsObject();auto Out=MakeShared<FJsonObject>();
            Out->SetStringField(TEXT("channel"),S(M,TEXT("channel")));Out->SetObjectField(TEXT("data"),M->GetObjectField(TEXT("data")));
            if(M->HasField(TEXT("to")))Out->SetStringField(TEXT("to"),S(M,TEXT("to")));
            Out->SetStringField(TEXT("type"),TEXT("send"));Out->SetStringField(TEXT("plugin"),P.Id);Out->SetStringField(TEXT("epoch"),P.ClientChannelEpoch);
            if(!ClientBus||!ClientBus->Send(Out)){P.ClientChannelError=ClientBus?ClientBus->Error():TEXT("Client channel is not connected; message not sent");break;}
        }
    }
    return true;
}
