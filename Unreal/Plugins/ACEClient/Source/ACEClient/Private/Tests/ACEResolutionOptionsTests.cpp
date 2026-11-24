#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "ACEDesktopDisplay.h"
#include "UI/ACEVideoSettingsWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameUserSettings.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ComboBoxString.h"
#include "UObject/StrongObjectPtr.h"
#if PLATFORM_LINUX
#include <SDL3/SDL.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEResolutionOptionsTest,"ACE.Rendering.ResolutionOptions",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEResolutionOptionsTest::RunTest(const FString&)
{
    const FIntPoint Desktops[]={{1280,800},{1920,1080},{2560,1440},{2560,1600},{3440,1440},{3840,2160},{3840,1080},{5120,1440},{7680,2160}};
    for(const auto Desktop:Desktops)
    {
        for(bool EmptyEnumeration:{false,true})
        {
            const auto Options=FACEDesktopDisplay::BuildResolutionOptions(EmptyEnumeration?TArray<FIntPoint>{}:TArray<FIntPoint>{Desktop},Desktop,Desktop,true);
            TestTrue(TEXT("Empty or single-mode drivers still provide multiple resolutions"),Options.Num()>2);
            TestTrue(TEXT("Desktop resolution is always offered"),Options.Contains(Desktop));
            TestTrue(TEXT("720p remains available on larger displays"),Options.Contains(FIntPoint(1280,720)));
            for(int32 I=0;I<Options.Num();++I)
            {
                TestTrue(TEXT("Fallback sizes fit this monitor"),Options[I].X<=Desktop.X && Options[I].Y<=Desktop.Y);
                if(I)TestTrue(TEXT("Resolution options are unique and numerically sorted"),Options[I-1].X<Options[I].X || (Options[I-1].X==Options[I].X && Options[I-1].Y<Options[I].Y));
            }
            if(Desktop.X>=2560 && Desktop.Y>=1440)TestTrue(TEXT("1440p is not capped to 1080p"),Options.Contains(FIntPoint(2560,1440)));
            for(const FIntPoint Size : {FIntPoint(2560,720),FIntPoint(3840,1080),FIntPoint(5120,1440),FIntPoint(7680,2160)})
                TestEqual(TEXT("32:9 fallback is offered when it fits the monitor"),Options.Contains(Size),Size.X<=Desktop.X && Size.Y<=Desktop.Y);
        }
    }
    const auto Reported=FACEDesktopDisplay::BuildResolutionOptions({{640,480},{0,0},{-1,1080},{2560,1440},{2560,1440},{3008,1692},{7680,4320}},
        {3840,2160},{1712,964},false);
    TestEqual(TEXT("Refresh-rate duplicates and invalid modes are removed"),Reported.Num(),5);
    TestTrue(TEXT("Unusual and supersampled reported modes are retained"),Reported.Contains(FIntPoint(3008,1692))&&Reported.Contains(FIntPoint(7680,4320)));
    TestTrue(TEXT("Custom saved window size survives enumeration"),Reported.Contains(FIntPoint(1712,964)));
    TestTrue(TEXT("Missing all display data still gives usable fallback sizes"),FACEDesktopDisplay::BuildResolutionOptions({},FIntPoint::ZeroValue,FIntPoint::ZeroValue,true).Num()>1);

#if PLATFORM_LINUX
    // Query the actual Linux display API even in the offscreen renderer fixture.
    const bool OwnVideo=!(SDL_WasInit(SDL_INIT_VIDEO)&SDL_INIT_VIDEO);
    if(OwnVideo && !SDL_InitSubSystem(SDL_INIT_VIDEO))
    {AddError(FString::Printf(TEXT("SDL video init failed: %s"),UTF8_TO_TCHAR(SDL_GetError())));return false;}
    ON_SCOPE_EXIT {if(OwnVideo)SDL_QuitSubSystem(SDL_INIT_VIDEO);};
    const auto Display=SDL_GetPrimaryDisplay();
    const auto* Desktop=SDL_GetDesktopDisplayMode(Display);
    if(!TestNotNull(TEXT("Linux provides its actual desktop mode"),Desktop))return false;
    const auto NativeOptions=FACEDesktopDisplay::GetAvailableResolutions(nullptr,EWindowMode::Fullscreen);
    TestTrue(TEXT("SDL desktop pixel size reaches the resolution list"),NativeOptions.Contains(FIntPoint(Desktop->w,Desktop->h)));
    int Count=0;
    if(auto** Modes=SDL_GetFullscreenDisplayModes(Display,&Count))
    {
        for(int I=0;I<Count;++I)
            if(Modes[I] && Modes[I]->w>=800 && Modes[I]->h>=600)
                TestTrue(TEXT("Every native Linux display mode reaches the picker"),NativeOptions.Contains(FIntPoint(Modes[I]->w,Modes[I]->h)));
        SDL_free(Modes);
    }
    AddInfo(FString::Printf(TEXT("Linux display: %s, desktop %dx%d, reported modes %d, offered resolutions %d"),
        UTF8_TO_TCHAR(SDL_GetCurrentVideoDriver()),Desktop->w,Desktop->h,Count,NativeOptions.Num()));
#endif

    auto* Settings=UGameUserSettings::GetGameUserSettings();
    const auto SavedSize=Settings->GetScreenResolution();const auto SavedMode=Settings->GetFullscreenMode();
    ON_SCOPE_EXIT {Settings->SetScreenResolution(SavedSize);Settings->SetFullscreenMode(SavedMode);};
    Settings->SetScreenResolution({1712,964});Settings->SetFullscreenMode(EWindowMode::Windowed);
    const auto Values=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    auto* World=UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,true,ERHIFeatureLevel::Num,&Values);
    auto* GI=NewObject<UGameInstance>(GEngine);GI->InitializeStandalone();
    GI->GetWorldContext()->SetCurrentWorld(World);World->SetGameInstance(GI);
    ON_SCOPE_EXIT {GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);};
    TStrongObjectPtr<UACEVideoSettingsWidget> Widget(CreateWidget<UACEVideoSettingsWidget>(GI));
    auto Slate=Widget->TakeWidget();
    auto* Resolution=Cast<UComboBoxString>(Widget->WidgetTree->FindWidget(TEXT("DisplayResolution")));
    auto* Quality=Cast<UComboBoxString>(Widget->WidgetTree->FindWidget(TEXT("GraphicsQuality")));
    if(!TestNotNull(TEXT("Resolution picker exists"),Resolution)||!TestNotNull(TEXT("Quality picker exists"),Quality))return false;
    TestTrue(TEXT("The real widget offers more than the saved resolution"),Resolution->GetOptionCount()>1);
    TestEqual(TEXT("The real widget selects the saved custom size"),Resolution->GetSelectedOption(),FString(TEXT("1712 x 964")));
    Quality->SetSelectedIndex(0);
    Settings->SetScreenResolution({1600,900});
    Resolution->OnOpening.Broadcast();
    TestEqual(TEXT("Opening the picker preserves an unapplied selection"),Resolution->GetSelectedOption(),FString(TEXT("1712 x 964")));
    TestTrue(TEXT("Opening the picker refreshes newly reported/current sizes"),Resolution->FindOptionIndex(TEXT("1600 x 900"))>=0);
    TestEqual(TEXT("Refreshing display modes preserves quality edits"),Quality->GetSelectedIndex(),0);
    Widget->NativeTick(FGeometry(),0);
    TestEqual(TEXT("An actual external resolution change updates the selection"),Resolution->GetSelectedOption(),FString(TEXT("1600 x 900")));
    Widget->ResetVideo();
    TestEqual(TEXT("Reopening options retains the active resolution"),Resolution->GetSelectedOption(),FString(TEXT("1600 x 900")));
    return true;
}
#endif
