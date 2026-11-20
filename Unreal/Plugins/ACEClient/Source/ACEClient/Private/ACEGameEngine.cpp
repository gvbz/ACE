#include "ACEGameEngine.h"
#include "ACEClientBuild.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Widgets/SWindow.h"

float ACEFrontendFrameRate::GetLimit(const UEngine& Engine,float GameplayLimit,bool Editor)
{
    // A stereo headset already has compositor pacing. Never force its tracking
    // and presentation loop down to the desktop menu rate.
    if(IsRunningDedicatedServer()||IsRunningCommandlet()||!FApp::CanEverRender()||Engine.IsStereoscopic3D())return GameplayLimit;
    bool HasPlayWorld=false;
    for(const FWorldContext& Context:Engine.GetWorldContexts())
    {
        if(Context.WorldType!=EWorldType::Game&&Context.WorldType!=EWorldType::PIE)continue;
        HasPlayWorld=true;
        const auto* World=Context.World();
        const auto* PC=World?Cast<AACEPlayerController>(World->GetFirstPlayerController()):nullptr;
        const auto* GI=World?World->GetGameInstance():nullptr;
        const auto* Client=GI?GI->GetSubsystem<UACEClientSubsystem>():nullptr;
        // In multi-PIE, a live game must not inherit another window's menu cap.
        if(PC&&(PC->IsVRActive()||(Client&&Client->GetSessionState()==EACESessionState::InWorld&&!PC->IsWorldTransitionActive())))return GameplayLimit;
    }
    if(Editor&&!HasPlayWorld)return GameplayLimit;
    // Covers startup before a world/controller exists, connection/error screens,
    // character creation/selection, loading, and the return to login after logout.
    return GameplayLimit>0?FMath::Min(GameplayLimit,30.f):30.f;
}

void UACEGameEngine::Init(IEngineLoop* InEngineLoop)
{
    Super::Init(InEngineLoop);
    // Strip Unreal's development/RHI suffix before the login world starts.
    if(const auto Window=GameViewportWindow.Pin())
        Window->SetTitle(FText::FromString(ACEClientBuild::ProductName()));
}

float UACEGameEngine::GetMaxTickRate(float DeltaTime,bool bAllowFrameRateSmoothing) const
{
    return ACEFrontendFrameRate::GetLimit(*this,Super::GetMaxTickRate(DeltaTime,bAllowFrameRateSmoothing),false);
}
