#if WITH_DEV_AUTOMATION_TESTS
#include "ACEGameEngine.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACESession.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"

namespace
{
    class FMeasureFrontendFrames : public IAutomationLatentCommand
    {
    public:
        FMeasureFrontendFrames(FAutomationTestBase* InTest,UWorld* InWorld,UGameInstance* InGI)
            :Test(InTest),World(InWorld),GI(InGI) {}
        virtual bool Update() override
        {
            // Allow the frame containing world setup/previous tests to settle.
            if(++Frames<=5){Started=FPlatformTime::Seconds();return false;}
            if(Frames<50)return false;
            const double Elapsed=FPlatformTime::Seconds()-Started;
            const double Measured=45./FMath::Max(Elapsed,.001);
            Test->AddInfo(FString::Printf(TEXT("Measured login pacing: %.2f FPS across 45 engine frames"),Measured));
            Test->TestTrue(TEXT("Actual engine loop honors the 30 FPS menu cap"),Measured<=34.);
            GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);
            return true;
        }
    private:
        FAutomationTestBase* Test;
        UWorld* World;
        UGameInstance* GI;
        int32 Frames=0;
        double Started=0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEFrontendFrameLimitTest,"ACE.Rendering.FrontendFrameLimit",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEFrontendFrameLimitTest::RunTest(const FString&)
{
    if(!FApp::CanEverRender()||GEngine->IsStereoscopic3D())
    {AddError(TEXT("Frontend frame pacing test requires a non-stereo rendering RHI"));return false;}
    AddInfo(TEXT("Frame pacing engine: ")+GEngine->GetClass()->GetPathName());
    if(!GIsEditor)TestTrue(TEXT("Standalone uses the configured game engine"),GEngine->IsA<UACEGameEngine>());
    FString EngineClass;GConfig->GetString(TEXT("/Script/Engine.Engine"),TEXT("GameEngine"),EngineClass,GEngineIni);
    TestEqual(TEXT("Packaged/standalone engine uses frontend pacing"),EngineClass,FString(TEXT("/Script/ACEClient.ACEGameEngine")));
    const auto& Startup=*GetDefault<UACEGameEngine>();
    TestEqual(TEXT("Startup before world creation is capped"),ACEFrontendFrameRate::GetLimit(Startup,0,false),30.f);
    TestEqual(TEXT("Editor outside Play keeps its own rate"),ACEFrontendFrameRate::GetLimit(Startup,144,true),144.f);
    const float SavedSetting=UGameUserSettings::GetGameUserSettings()->GetFrameRateLimit();
    const float SavedCVar=IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"))->GetFloat();
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
        .CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
    auto* W=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& Context=GEngine->CreateNewWorldContext(EWorldType::Game);Context.SetCurrentWorld(W);
    auto* GI=NewObject<UGameInstance>(GEngine);W->SetGameInstance(GI);Context.OwningGameInstance=GI;GI->OnWorldChanged(nullptr,W);GI->Init();
    auto* Client=GI->GetSubsystem<UACEClientSubsystem>();Client->Session=MakeShared<FACESession>();
    auto* PC=W->SpawnActor<AACEPlayerController>();
    // This isolated world has not initialized actors for play, so the usual
    // PostInitializeComponents controller registration has not run yet.
    W->AddController(PC);
    TestTrue(TEXT("Controller belongs to the tested frontend world"),W->GetFirstPlayerController()==PC);
    for(auto State:{EACESessionState::Disconnected,EACESessionState::Connecting,EACESessionState::AwaitConnectRequest,
        EACESessionState::AwaitCharacterList,EACESessionState::CharacterSelect,EACESessionState::EnteringWorld,EACESessionState::Failed})
    {
        Client->Session->State=State;
        TestEqual(TEXT("Every frontend/login state caps unlimited gameplay"),ACEFrontendFrameRate::GetLimit(*GEngine,0,true),30.f);
        TestEqual(TEXT("High gameplay limit still caps menus"),ACEFrontendFrameRate::GetLimit(*GEngine,144,true),30.f);
        TestEqual(TEXT("A lower chosen limit stays lower"),ACEFrontendFrameRate::GetLimit(*GEngine,20,true),20.f);
    }
    Client->Session->State=EACESessionState::InWorld;
    TestEqual(TEXT("Active game restores unlimited"),ACEFrontendFrameRate::GetLimit(*GEngine,0,true),0.f);
    TestEqual(TEXT("Active game restores chosen 144 FPS"),ACEFrontendFrameRate::GetLimit(*GEngine,144,true),144.f);
    PC->bEnterWorldLoading=true;TestEqual(TEXT("Loading before world reveal remains capped"),ACEFrontendFrameRate::GetLimit(*GEngine,144,true),30.f);PC->bEnterWorldLoading=false;
    PC->bWorldRevealActive=true;TestEqual(TEXT("World reveal remains capped"),ACEFrontendFrameRate::GetLimit(*GEngine,144,true),30.f);PC->bWorldRevealActive=false;
    PC->bLogOffPresentationActive=true;TestEqual(TEXT("Logout presentation caps immediately"),ACEFrontendFrameRate::GetLimit(*GEngine,144,true),30.f);PC->bLogOffPresentationActive=false;
    // Another login window must not throttle an active game in multi-PIE.
    auto* Menu=UWorld::CreateWorld(EWorldType::PIE,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto& MenuContext=GEngine->CreateNewWorldContext(EWorldType::PIE);MenuContext.SetCurrentWorld(Menu);
    TestEqual(TEXT("Active game wins over another login window"),ACEFrontendFrameRate::GetLimit(*GEngine,144,true),144.f);
    GEngine->DestroyWorldContext(Menu);Menu->DestroyWorld(false);
    Client->Session->State=EACESessionState::Disconnected;
    TestEqual(TEXT("Returning to login reinstates cap"),ACEFrontendFrameRate::GetLimit(*GEngine,0,true),30.f);
    TestEqual(TEXT("Saved gameplay preference is untouched"),UGameUserSettings::GetGameUserSettings()->GetFrameRateLimit(),SavedSetting);
    TestEqual(TEXT("Console gameplay limit is untouched"),IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"))->GetFloat(),SavedCVar);
    TestTrue(TEXT("Installed engine override applies the menu cap"),GEngine->GetMaxTickRate(1.f/120.f)<=30.f&&GEngine->GetMaxTickRate(1.f/120.f)>0);
    ADD_LATENT_AUTOMATION_COMMAND(FMeasureFrontendFrames(this,W,GI));
    return true;
}
#endif
