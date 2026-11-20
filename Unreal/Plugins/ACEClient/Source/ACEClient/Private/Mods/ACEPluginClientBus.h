#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Game-thread, nonblocking loopback transport. The first participant hosts the
// relay; remaining participants reconnect/elect a relay when that process exits.
// This is deliberately not a Lua socket API or a retail UB wire protocol.
class FACEPluginClientBus
{
public:
    static constexpr int32 MaxFrame = 16384;
    static constexpr int32 MaxQueue = 256 * 1024;
    FACEPluginClientBus(const FString& Namespace, uint16 Port = 37981);
    ~FACEPluginClientBus();
    void Configure(const FString& Scope, const FString& Character, uint32 Player,
        const TArray<TSharedPtr<FJsonValue>>& Subscriptions);
    void Tick(double Now);
    bool Send(const TSharedPtr<FJsonObject>& Message);
    TArray<TSharedPtr<FJsonObject>> Drain();
    TArray<TSharedPtr<FJsonValue>> Peers(const FString& Plugin) const;
    bool Connected() const { return Ready; }
    const FString& Instance() const { return Id; }
    const FString& Error() const { return LastError; }
private:
    struct FConnection
    {
        class FSocket* Socket = nullptr;
        TArray<uint8> In, Out;
        TSharedPtr<FJsonObject> Hello;
        double LastRead = 0, RateStart = 0;
        int32 RateCount = 0;
    };
    FString Space, Id, LastError;
    uint16 Port;
    class FSocket* Listener = nullptr;
#if PLATFORM_LINUX
    int32 ListenerLock = -1;
#endif
    FConnection Client;
    TArray<TUniquePtr<FConnection>> Clients;
    TMap<FString,TSharedPtr<FJsonObject>> KnownPeers;
    TArray<TSharedPtr<FJsonObject>> Inbox;
    TSharedPtr<FJsonObject> Hello;
    bool Ready = false;
    double RetryAt = 0, HeartbeatAt = 0;
    static void Close(FConnection& C);
    static bool Queue(FConnection& C, const TSharedPtr<FJsonObject>& O);
    static bool Pump(FConnection& C, double Now, TArray<TSharedPtr<FJsonObject>>& Frames);
    bool AcceptHello(FConnection& C, const TSharedPtr<FJsonObject>& O);
    bool Relay(FConnection& C, const TSharedPtr<FJsonObject>& O, double Now);
    void Drop(int32 Index);
    void Disconnect(const FString& Reason, double Now);
};
