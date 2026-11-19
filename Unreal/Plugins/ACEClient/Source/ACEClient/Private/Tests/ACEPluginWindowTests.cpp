#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "ACEClientSubsystem.h"
#include "ACEPlayerController.h"
#include "ACESession.h"
#include "Mobile/ACEMobileSettings.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginDesktop.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SVirtualWindow.h"

namespace
{
    TSharedPtr<SWidget> FindPluginControl(const TSharedRef<SWidget>& Widget,FName Tag)
    {
        if(Widget->GetTag()==Tag)return Widget;
        auto* Children=Widget->GetChildren();
        for(int32 Index=0;Index<Children->Num();++Index)
            if(auto Found=FindPluginControl(Children->GetChildAt(Index),Tag))return Found;
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginWindowTest,"ACE.Plugins.WindowLaunchAndLifecycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginWindowTest::RunTest(const FString&)
{
    if(!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Plugin window regression requires Slate."));return false;
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("ACEMobile")))
    {
        AddWarning(TEXT("Desktop plugin window regression skipped in forced mobile mode."));return true;
    }
    auto* MobileDefaults=GetMutableDefault<UACEMobileSettings>();
    const bool WasMobileEnabled=MobileDefaults->bEnabled;MobileDefaults->bEnabled=false;
    ON_SCOPE_EXIT {MobileDefaults->bEnabled=WasMobileEnabled;};

    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto* GI=NewObject<UGameInstance>(GEngine);GI->InitializeStandalone();auto* Dummy=GI->GetWorld();
    auto& Context=*GI->GetWorldContext();Context.SetCurrentWorld(World);World->SetGameInstance(GI);Dummy->DestroyWorld(false);
    auto* PC=World->SpawnActor<AACEPlayerController>();auto* LP=NewObject<ULocalPlayer>(GEngine);
    LP->PlayerController=PC;GI->AddLocalPlayer(LP,FPlatformUserId::CreateFromInternalId(0));PC->SetPlayer(LP);World->AddController(PC);
    auto* Client=GI->GetSubsystem<UACEClientSubsystem>();auto& Session=*Client->GetSession();
    auto* Host=GI->GetSubsystem<UACEPluginSubsystem>();
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation")/(TEXT("PluginWindows-")+FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    Host->StorageRoot=Root;Host->Settings=MakeShared<FJsonObject>();Host->Discover();
    for(const auto& Plugin:Host->Plugins)Plugin->Enabled=Plugin->Id==TEXT("waypoint");
    Host->SetWaypointOption(TEXT("arrow"),false);Host->SetWaypointOption(TEXT("dungeon_overlay"),false);
    auto Generic=MakeShared<FACEClientPlugin>();Generic->Id=TEXT("windowtest");Generic->Name=TEXT("Window test");Generic->Enabled=true;
    Generic->Manifest=MakeShared<FJsonObject>();Generic->Profile=MakeShared<FJsonObject>();Host->Plugins.Add(Generic);

    // Exercise the actual manager callback with a local controller and viewport;
    // a bare GameInstance instead takes the login manager's embedded-panel path.
    auto Window=SNew(SVirtualWindow).Size(FVector2D(1280,900));auto Content=SNew(SOverlay);Window->SetContent(Content);
    auto* Viewport=NewObject<UGameViewportClient>(GEngine);Viewport->Init(Context,GI,false);
    Context.GameViewport=Viewport;LP->ViewportClient=Viewport;
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    Viewport->SetViewportOverlayWidget(Window,Content);
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
    ON_SCOPE_EXIT
    {
        MobileDefaults->bEnabled=false;Session.State=EACESessionState::Disconnected;
        Host->CloseDesktopPluginManager(false);Host->RemoveDesktopDock();Viewport->RemoveAllViewportWidgets();
        LP->ViewportClient=nullptr;Context.GameViewport=nullptr;
        GI->Shutdown();World->DestroyWorld(false);IFileManager::Get().DeleteDirectory(*Root,false,true);
    };
    Session.State=EACESessionState::InWorld;
    TestTrue(TEXT("Fixture uses the real desktop launch path"),GI->GetFirstLocalPlayerController()==PC&&GI->GetGameViewportClient()==Viewport&&!PC->IsMobileActive()&&!PC->IsVRActive());

    auto Click=[&](const TSharedRef<SWidget>& Manager,const TCHAR* Id)
    {
        const auto Control=FindPluginControl(Manager,FName(*(TEXT("PluginOpen.")+FString(Id))));
        if(!TestTrue(*FString::Printf(TEXT("Manager contains Open for %s"),Id),Control.IsValid()))return false;
        if(!TestTrue(*FString::Printf(TEXT("Open for %s is enabled"),Id),Control->IsEnabled()))return false;
        StaticCastSharedPtr<SButton>(Control)->SimulateClick();return true;
    };
    auto OpenManager=[&]() -> TSharedPtr<SACEPluginDesktop>
    {
        if(!Host->DesktopPanel)Host->TogglePanel();
        TestTrue(TEXT("Plugin Manager opens in the fixture viewport"),Host->DesktopPanel.IsValid());
        return Host->DesktopPanel;
    };

    if(!TestTrue(TEXT("Map entry opens its first desktop frame"),Host->OpenPluginWindow(TEXT("waypoint.map"))))return false;
    const auto Dock=Host->DesktopDock;const auto MapWidget=Dock->Windows.FindChecked(TEXT("waypoint.map")).Widget;
    TestTrue(TEXT("Generic plugin opens in another frame"),Host->OpenPluginWindow(Generic->Id));
    const auto GenericWidget=Dock->Windows.FindChecked(Generic->Id).Widget;
    const auto Position=Dock->Windows.FindChecked(TEXT("waypoint.map")).Position;
    const auto Size=Dock->Windows.FindChecked(TEXT("waypoint.map")).Size;
    Host->SetPluginWindowPinned(TEXT("waypoint.map"),true);
    int32 ExpectedWindows=Dock->Windows.Num();
    {
        const auto Manager=OpenManager();if(!Manager)return false;
        const auto PreviousOrder=Dock->Windows.FindChecked(TEXT("waypoint.map")).Slot->GetZOrder();
        if(!Click(Manager.ToSharedRef(),TEXT("waypoint")))return false;
        TestTrue(TEXT("Manager Open raises the existing map frame"),Dock->Windows.FindChecked(TEXT("waypoint.map")).Slot->GetZOrder()>PreviousOrder);
        TestTrue(TEXT("Manager Open reuses the current map widget"),Dock->Windows.FindChecked(TEXT("waypoint.map")).Widget==MapWidget);
        TestFalse(TEXT("Manager Open does not create a second NAV frame"),Dock->Windows.Contains(TEXT("waypoint")));
        TestEqual(TEXT("Manager Open does not add a duplicate frame"),Dock->Windows.Num(),ExpectedWindows);
        TestTrue(TEXT("Manager Open keeps both aliases open"),Host->IsPluginWindowOpen(TEXT("waypoint"))&&Host->IsPluginWindowOpen(TEXT("waypoint.map")));
        TestEqual(TEXT("Manager Open preserves window position"),Dock->Windows.FindChecked(TEXT("waypoint.map")).Position,Position);
        TestEqual(TEXT("Manager Open preserves window size"),Dock->Windows.FindChecked(TEXT("waypoint.map")).Size,Size);
        TestTrue(TEXT("Manager Open preserves pin state"),Host->IsPluginWindowPinned(TEXT("waypoint.map")));
        TestFalse(TEXT("Manager launch closes the manager"),Host->DesktopPanel.IsValid());
        TestFalse(TEXT("Manager launch records the manager as closed"),Host->WasPluginWindowOpen(TEXT("__manager")));
    }
    // A retained/embedded manager has no desktop manager to close. Repeated
    // Open must never interpret closing it as a toggle that creates one.
    const auto StandaloneManager=Host->MakePanel();
    for(int32 Attempt=0;Attempt<3;++Attempt)
    {
        const auto PreviousOrder=Dock->Windows.FindChecked(TEXT("waypoint.map")).Slot->GetZOrder();
        if(!Click(StandaloneManager,TEXT("waypoint")))return false;
        TestFalse(TEXT("Standalone manager Open never creates a desktop manager"),Host->DesktopPanel.IsValid());
        TestTrue(TEXT("Repeated Open keeps the existing map visible and raises it"),Dock->IsOpen(TEXT("waypoint.map"))&&Dock->Windows.FindChecked(TEXT("waypoint.map")).Slot->GetZOrder()>PreviousOrder);
        TestTrue(TEXT("Repeated Open preserves widget identity"),Dock->Windows.FindChecked(TEXT("waypoint.map")).Widget==MapWidget);
        TestEqual(TEXT("Repeated Open keeps the same frame count"),Dock->Windows.Num(),ExpectedWindows);
    }
    Host->TogglePluginWindow(TEXT("waypoint"));
    TestFalse(TEXT("Alias toggle still hides the existing map"),Host->IsPluginWindowOpen(TEXT("waypoint.map")));
    if(!Click(StandaloneManager,TEXT("waypoint")))return false;
    TestTrue(TEXT("Manager reopens a hidden frame instead of rebuilding it"),Dock->IsOpen(TEXT("waypoint.map"))&&Dock->Windows.FindChecked(TEXT("waypoint.map")).Widget==MapWidget);
    TestEqual(TEXT("Reopening the hidden frame keeps its size"),Dock->Windows.FindChecked(TEXT("waypoint.map")).Size,Size);
    {
        const auto Manager=OpenManager();if(!Manager)return false;
        const auto PreviousOrder=Dock->Windows.FindChecked(Generic->Id).Slot->GetZOrder();
        if(!Click(Manager.ToSharedRef(),*Generic->Id))return false;
        TestTrue(TEXT("Manager raises an already-open ordinary plugin"),Dock->Windows.FindChecked(Generic->Id).Slot->GetZOrder()>PreviousOrder);
        TestTrue(TEXT("Ordinary plugin Open preserves the original widget"),Dock->Windows.FindChecked(Generic->Id).Widget==GenericWidget);
        TestFalse(TEXT("Ordinary plugin launch closes the manager"),Host->DesktopPanel.IsValid());
    }
    Host->TogglePluginWindow(Generic->Id);
    if(!Click(StandaloneManager,*Generic->Id))return false;
    TestTrue(TEXT("Ordinary plugin reopens with the same widget"),Dock->IsOpen(Generic->Id)&&Dock->Windows.FindChecked(Generic->Id).Widget==GenericWidget);

    // Temporary presentation changes suspend the desktop surfaces rather than
    // marking the user's manager/window preferences as explicitly closed.
    for(bool MobileMode:{true,false})
    {
        if(!OpenManager())return false;
        TestTrue(TEXT("Open manager records its visibility before transition"),Host->WasPluginWindowOpen(TEXT("__manager")));
        if(MobileMode)MobileDefaults->bEnabled=true;else Session.State=EACESessionState::CharacterSelect;
        Host->UpdateDesktopDock();
        TestFalse(TEXT("Leaving desktop gameplay removes its plugin dock"),Host->DesktopDock.IsValid());
        TestFalse(TEXT("Leaving desktop gameplay removes its manager surface"),Host->DesktopPanel.IsValid());
        TestTrue(TEXT("Suspending the manager preserves its reopen preference"),Host->WasPluginWindowOpen(TEXT("__manager")));
        TestTrue(TEXT("Suspending preserves the primary map visibility preference"),Host->WasPluginWindowOpen(TEXT("waypoint.map")));
        MobileDefaults->bEnabled=false;Session.State=EACESessionState::InWorld;Host->UpdateDesktopDock();
        if(!TestTrue(TEXT("Returning to desktop restores dock and manager"),Host->DesktopDock.IsValid()&&Host->DesktopPanel.IsValid()))return false;
        TestTrue(TEXT("Returning restores one shared Waypoint interface"),Host->DesktopDock->IsOpen(TEXT("waypoint"))&&Host->DesktopDock->IsOpen(TEXT("waypoint.map"))&&!Host->DesktopDock->Windows.Contains(TEXT("waypoint")));
        TestTrue(TEXT("Returning preserves the map's pin state"),Host->IsPluginWindowPinned(TEXT("waypoint.map")));
        const auto RestoredManager=Host->DesktopPanel;Host->UpdateDesktopDock();
        TestTrue(TEXT("Repeated desktop refresh reuses the restored manager"),Host->DesktopPanel==RestoredManager);
    }
    Host->CloseDesktopPluginManager();Host->UpdateDesktopDock();
    TestFalse(TEXT("Explicitly closing the manager prevents automatic reopening"),Host->DesktopPanel.IsValid()||Host->WasPluginWindowOpen(TEXT("__manager")));
    return true;
}
#endif
