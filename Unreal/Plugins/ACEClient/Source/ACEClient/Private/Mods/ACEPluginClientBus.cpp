#include "Mods/ACEPluginClientBus.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if PLATFORM_LINUX
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace
{
#if PLATFORM_LINUX
    int32 LockRelayPort(uint16 Port)
    {
        // Unreal's SetReuseAddr also enables SO_REUSEPORT on Linux. Elect
        // exactly one relay before binding, while retaining TIME_WAIT reuse.
        // The per-user lock is independent of each client's -UserDir. Kernel
        // ownership ends on process exit; spawned clients cannot inherit it.
        const FString Directory = FString(FPlatformProcess::ApplicationSettingsDir()) / TEXT("ACUnreal");
        if(!IFileManager::Get().MakeDirectory(*Directory,true))return -1;
        const FString Path = Directory / FString::Printf(TEXT("client-relay-%u.lock"),Port);
        const int32 Handle = open(TCHAR_TO_UTF8(*Path),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if(Handle>=0&&flock(Handle,LOCK_EX|LOCK_NB)==0)return Handle;
        if(Handle>=0)close(Handle);
        return -1;
    }
#endif
    FString S(const TSharedPtr<FJsonObject>& O, const TCHAR* K)
    { FString V; if(O)O->TryGetStringField(K,V); return V; }
    FString Encode(const TSharedPtr<FJsonObject>& O)
    { FString V; FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&V)); return V; }
    TSharedPtr<FJsonObject> Packet(const TCHAR* Type)
    { auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),Type);return O; }
    FString Subscription(const TSharedPtr<FJsonObject>& H,const FString& Plugin,const FString& Channel)
    {
        const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
        if(H&&H->TryGetArrayField(TEXT("subscriptions"),A))for(const auto& V:*A)
        {const auto O=V->AsObject();if(O&&S(O,TEXT("plugin"))==Plugin&&S(O,TEXT("channel"))==Channel)return S(O,TEXT("epoch"));}
        return {};
    }
    bool SafeName(const FString& V,int32 Max)
    {
        if(V.IsEmpty()||V.Len()>Max)return false;
        for(TCHAR C:V)if(!((C>='a'&&C<='z')||(C>='A'&&C<='Z')||(C>='0'&&C<='9')||C=='_'||C=='-'||C=='.'))return false;
        return true;
    }
    bool WouldBlock()
    {return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode()==SE_EWOULDBLOCK;}
    // Reject deeply nested input before invoking the recursive JSON reader.
    bool ShallowJSON(const FString& Text)
    {
        int32 Depth=0;bool Quoted=false,Escape=false;
        for(TCHAR C:Text)
        {
            if(Quoted){if(Escape)Escape=false;else if(C=='\\')Escape=true;else if(C=='"')Quoted=false;}
            else if(C=='"')Quoted=true;
            else if(C=='{'||C=='['){if(++Depth>16)return false;}
            else if(C=='}'||C==']'){if(--Depth<0)return false;}
        }
        return Depth==0&&!Quoted;
    }
}

FACEPluginClientBus::FACEPluginClientBus(const FString& Namespace,uint16 InPort)
    : Space(Namespace),Id(FGuid::NewGuid().ToString(EGuidFormats::Digits)),Port(InPort) {}
FACEPluginClientBus::~FACEPluginClientBus()
{
    Close(Client);for(auto& C:Clients)Close(*C);
    if(Listener){Listener->Close();ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Listener);}
#if PLATFORM_LINUX
    if(ListenerLock>=0)close(ListenerLock);
#endif
}
void FACEPluginClientBus::Close(FConnection& C)
{
    if(C.Socket){C.Socket->Close();ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(C.Socket);}
    C=FConnection();
}
bool FACEPluginClientBus::Queue(FConnection& C,const TSharedPtr<FJsonObject>& O)
{
    const FTCHARToUTF8 Bytes(*Encode(O));const int32 N=Bytes.Length();
    if(!C.Socket||N>MaxFrame||C.Out.Num()+N+4>MaxQueue)return false;
    for(int32 Shift=24;Shift>=0;Shift-=8)C.Out.Add(uint8(uint32(N)>>Shift));
    C.Out.Append(reinterpret_cast<const uint8*>(Bytes.Get()),N);return true;
}
bool FACEPluginClientBus::Pump(FConnection& C,double Now,TArray<TSharedPtr<FJsonObject>>& Frames)
{
    if(!C.Socket)return false;
    if(!C.Out.IsEmpty())
    {
        int32 Sent=0;
        if(!C.Socket->Send(C.Out.GetData(),FMath::Min(C.Out.Num(),64*1024),Sent)&&!WouldBlock())return false;
        if(Sent>0)C.Out.RemoveAt(0,Sent,EAllowShrinking::No);
    }
    uint8 Buffer[16384];
    for(int32 I=0;I<4;++I)
    {
        int32 Read=0;
        // Unreal's stream Recv returns true with zero bytes for EWOULDBLOCK,
        // and false for EOF/errors (unlike the underlying recv syscall).
        if(!C.Socket->Recv(Buffer,UE_ARRAY_COUNT(Buffer),Read))return false;
        if(Read==0)break;
        C.LastRead=Now;C.In.Append(Buffer,Read);if(C.In.Num()>MaxQueue)return false;
    }
    int32 Used=0;
    while(C.In.Num()-Used>=4&&Frames.Num()<64)
    {
        const uint8* B=C.In.GetData()+Used;
        const uint32 N=(uint32(B[0])<<24)|(uint32(B[1])<<16)|(uint32(B[2])<<8)|B[3];
        if(N==0||N>MaxFrame)return false;
        if(uint32(C.In.Num()-Used-4)<N)break;
        const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(B+4),N);
        const FString Json(Text.Length(),Text.Get());TSharedPtr<FJsonObject> O;
        if(!ShallowJSON(Json)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),O)||!O)return false;
        Frames.Add(O);Used+=4+N;
    }
    if(Used)C.In.RemoveAt(0,Used,EAllowShrinking::No);
    return Now-C.LastRead<30;
}
void FACEPluginClientBus::Configure(const FString& Scope,const FString& Character,uint32 Player,const TArray<TSharedPtr<FJsonValue>>& Subscriptions)
{
    auto O=Packet(TEXT("hello"));O->SetNumberField(TEXT("version"),1);O->SetStringField(TEXT("namespace"),Space);
    O->SetStringField(TEXT("instance"),Id);O->SetStringField(TEXT("scope"),Scope);O->SetStringField(TEXT("character"),Character.Left(128));
    O->SetNumberField(TEXT("player"),Player);O->SetArrayField(TEXT("subscriptions"),Subscriptions);
    if(Hello&&Encode(Hello)==Encode(O))return;
    Hello=O;
    if(Client.Socket&&!Queue(Client,Hello))Disconnect(TEXT("Client channel queue full"),FPlatformTime::Seconds());
}
bool FACEPluginClientBus::AcceptHello(FConnection& C,const TSharedPtr<FJsonObject>& O)
{
    double Version=0,Player=0;const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
    if(!O->TryGetNumberField(TEXT("version"),Version)||Version!=1||S(O,TEXT("namespace"))!=Space
        ||!SafeName(S(O,TEXT("instance")),64)||S(O,TEXT("scope")).IsEmpty()||S(O,TEXT("scope")).Len()>128
        ||S(O,TEXT("character")).Len()>128||!O->TryGetNumberField(TEXT("player"),Player)||!FMath::IsFinite(Player)
        ||!O->TryGetArrayField(TEXT("subscriptions"),A)||A->Num()>64)return false;
    for(const auto& V:*A)
    {const auto P=V->AsObject();if(!P||!SafeName(S(P,TEXT("plugin")),64)||!SafeName(S(P,TEXT("channel")),64)||!SafeName(S(P,TEXT("epoch")),64))return false;}
    if(C.Hello&&(S(C.Hello,TEXT("instance"))!=S(O,TEXT("instance"))||S(C.Hello,TEXT("scope"))!=S(O,TEXT("scope"))))return false;
    for(const auto& Other:Clients)if(Other.Get()!=&C&&Other->Hello&&S(Other->Hello,TEXT("instance"))==S(O,TEXT("instance")))return false;
    C.Hello=O;
    if(!Queue(C,Packet(TEXT("ready"))))return false;
    for(const auto& Other:Clients)if(Other->Hello&&S(Other->Hello,TEXT("scope"))==S(O,TEXT("scope")))
    {
        auto P=Packet(TEXT("peer"));P->SetObjectField(TEXT("peer"),O);
        if(!Queue(*Other,P))Other->LastRead=-1e30;
        if(Other.Get()!=&C){auto Existing=Packet(TEXT("peer"));Existing->SetObjectField(TEXT("peer"),Other->Hello);if(!Queue(C,Existing))return false;}
    }
    return true;
}
bool FACEPluginClientBus::Relay(FConnection& C,const TSharedPtr<FJsonObject>& O,double Now)
{
    const FString Type=S(O,TEXT("type"));
    if(Now-C.RateStart>=1){C.RateStart=Now;C.RateCount=0;}
    if(++C.RateCount>256)return false;
    if(Type==TEXT("hello"))return AcceptHello(C,O);
    if(!C.Hello)return false;
    if(Type==TEXT("ping"))return Queue(C,Packet(TEXT("pong")));
    if(Type!=TEXT("send"))return false;
    const FString Plugin=S(O,TEXT("plugin")),Channel=S(O,TEXT("channel")),To=S(O,TEXT("to"));
    const TSharedPtr<FJsonObject>* Data=nullptr;
    if(Subscription(C.Hello,Plugin,Channel).IsEmpty()||S(O,TEXT("epoch"))!=Subscription(C.Hello,Plugin,Channel)
        ||(!To.IsEmpty()&&!SafeName(To,64))||!O->TryGetObjectField(TEXT("data"),Data)||FTCHARToUTF8(*Encode(*Data)).Length()>4096)return false;
    for(auto& Peer:Clients)
    {
        if(Peer.Get()==&C||!Peer->Hello||S(Peer->Hello,TEXT("scope"))!=S(C.Hello,TEXT("scope")))continue;
        if(!To.IsEmpty()&&To!=S(Peer->Hello,TEXT("instance")))continue;
        const FString Epoch=Subscription(Peer->Hello,Plugin,Channel);if(Epoch.IsEmpty())continue;
        auto E=Packet(TEXT("client_message"));E->SetStringField(TEXT("sender"),S(C.Hello,TEXT("instance")));
        E->SetStringField(TEXT("character"),S(C.Hello,TEXT("character")));E->SetField(TEXT("player"),C.Hello->TryGetField(TEXT("player")));
        E->SetStringField(TEXT("plugin"),Plugin);E->SetStringField(TEXT("channel"),Channel);E->SetStringField(TEXT("epoch"),Epoch);E->SetObjectField(TEXT("data"),*Data);
        if(!Queue(*Peer,E))Peer->LastRead=-1e30;
    }
    return true;
}
void FACEPluginClientBus::Drop(int32 Index)
{
    const auto H=Clients[Index]->Hello;Close(*Clients[Index]);Clients.RemoveAt(Index);
    if(H)for(auto& Other:Clients)if(Other->Hello&&S(Other->Hello,TEXT("scope"))==S(H,TEXT("scope")))
    {auto O=Packet(TEXT("leave"));O->SetStringField(TEXT("instance"),S(H,TEXT("instance")));if(!Queue(*Other,O))Other->LastRead=-1e30;}
}
void FACEPluginClientBus::Disconnect(const FString& Reason,double Now)
{Close(Client);Ready=false;KnownPeers.Reset();Inbox.Reset();LastError=Reason;RetryAt=Now+1;}
void FACEPluginClientBus::Tick(double Now)
{
    if(!Hello)return;
    auto* SS=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    auto Address=SS->CreateInternetAddr();bool Valid=false;Address->SetIp(TEXT("127.0.0.1"),Valid);Address->SetPort(Port);
    if(!Client.Socket&&Now>=RetryAt)
    {
        if(!Listener)
        {
#if PLATFORM_LINUX
            const int32 CandidateLock = LockRelayPort(Port);
            if(CandidateLock>=0)
#endif
            {
                auto* Candidate=SS->CreateSocket(NAME_Stream,TEXT("ACE plugin loopback relay"),false);
                if(Candidate)
                {
                    Candidate->SetNonBlocking(true);
#if PLATFORM_LINUX
                    // The held file lock prevents Linux's implicit SO_REUSEPORT
                    // from splitting clients between competing relay listeners.
                    Candidate->SetReuseAddr(true);
#endif
                    if(Candidate->Bind(*Address)&&Candidate->Listen(32))Listener=Candidate;
                    else{Candidate->Close();SS->DestroySocket(Candidate);}
                }
#if PLATFORM_LINUX
                if(Listener)ListenerLock=CandidateLock;else close(CandidateLock);
#endif
            }
        }
        Client.Socket=SS->CreateSocket(NAME_Stream,TEXT("ACE plugin local channel"),false);
        if(Client.Socket)
        {
            Client.Socket->SetNonBlocking(true);Client.Socket->SetNoDelay(true);Client.LastRead=Now;
            // Connect may report in-progress on a nonblocking socket. Pump handles failure/retry.
            Client.Socket->Connect(*Address);Queue(Client,Hello);HeartbeatAt=Now+2;
        }
        else{LastError=TEXT("Local client channels unavailable");RetryAt=Now+1;}
    }
    if(Listener)
    {
        for(int32 I=0;I<4;++I)
        {
            bool Pending=false;if(!Listener->HasPendingConnection(Pending)||!Pending)break;
            FSocket* Socket=Listener->Accept(TEXT("ACE plugin local peer"));if(!Socket)break;
            if(Clients.Num()>=32){Socket->Close();SS->DestroySocket(Socket);continue;}
            Socket->SetNonBlocking(true);Socket->SetNoDelay(true);
            auto C=MakeUnique<FConnection>();C->Socket=Socket;C->LastRead=Now;Clients.Add(MoveTemp(C));
        }
        for(int32 I=Clients.Num()-1;I>=0;--I)
        {
            auto& C=*Clients[I];TArray<TSharedPtr<FJsonObject>> Frames;bool OK=Pump(C,Now,Frames);
            for(const auto& O:Frames)if(OK)OK=Relay(C,O,Now);
            if(!C.Hello&&Now-C.LastRead>5)OK=false;
            if(!OK)Drop(I);
        }
    }
    if(Client.Socket)
    {
        if(Now>=HeartbeatAt){HeartbeatAt=Now+2;if(!Queue(Client,Packet(TEXT("ping")))){Disconnect(TEXT("Client channel queue full"),Now);return;}}
        TArray<TSharedPtr<FJsonObject>> Frames;
        if(!Pump(Client,Now,Frames)){Disconnect(TEXT("Reconnecting local client channels"),Now);return;}
        for(const auto& O:Frames)
        {
            const FString Type=S(O,TEXT("type"));
            if(Type==TEXT("ready")){Ready=true;LastError.Empty();}
            else if(Type==TEXT("peer"))
            {const TSharedPtr<FJsonObject>* P=nullptr;if(O->TryGetObjectField(TEXT("peer"),P)&&(KnownPeers.Contains(S(*P,TEXT("instance")))||KnownPeers.Num()<32))KnownPeers.Add(S(*P,TEXT("instance")),*P);}
            else if(Type==TEXT("leave"))KnownPeers.Remove(S(O,TEXT("instance")));
            else if(Type==TEXT("client_message"))
            {if(Inbox.Num()<64)Inbox.Add(O);else LastError=TEXT("Client channel inbox full; message dropped");}
        }
    }
}
bool FACEPluginClientBus::Send(const TSharedPtr<FJsonObject>& Message)
{
    if(!Ready){LastError=TEXT("Local client channel is not connected; message not sent");return false;}
    if(!Queue(Client,Message)){LastError=TEXT("Client channel queue full; message not sent");return false;}
    LastError.Empty();
    return true;
}
TArray<TSharedPtr<FJsonObject>> FACEPluginClientBus::Drain()
{TArray<TSharedPtr<FJsonObject>> Result=MoveTemp(Inbox);Inbox.Reset();return Result;}
TArray<TSharedPtr<FJsonValue>> FACEPluginClientBus::Peers(const FString& Plugin) const
{
    TArray<TSharedPtr<FJsonValue>> Result;TArray<FString> Keys;KnownPeers.GetKeys(Keys);Keys.Sort();
    for(const auto& Key:Keys)
    {
        if(Key==Id)continue;const auto& H=KnownPeers[Key];const TArray<TSharedPtr<FJsonValue>>* A=nullptr;
        TArray<TSharedPtr<FJsonValue>> Channels;
        if(H->TryGetArrayField(TEXT("subscriptions"),A))for(const auto& V:*A)if(S(V->AsObject(),TEXT("plugin"))==Plugin)
            Channels.Add(MakeShared<FJsonValueString>(S(V->AsObject(),TEXT("channel"))));
        if(Channels.IsEmpty())continue;
        auto P=MakeShared<FJsonObject>();P->SetStringField(TEXT("instance"),Key);P->SetStringField(TEXT("character"),S(H,TEXT("character")));
        P->SetField(TEXT("player"),H->TryGetField(TEXT("player")));P->SetArrayField(TEXT("channels"),Channels);Result.Add(MakeShared<FJsonValueObject>(P));
    }
    return Result;
}
