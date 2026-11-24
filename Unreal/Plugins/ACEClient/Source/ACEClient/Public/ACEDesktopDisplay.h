#pragma once
#include "CoreMinimal.h"
#include "GenericPlatform/GenericWindow.h"

class AACEPlayerController;
/** Desktop display shortcuts and window placement, independent of gameplay focus. */
class ACECLIENT_API FACEDesktopDisplay
{
public:
    virtual ~FACEDesktopDisplay() = default;
    static TSharedPtr<FACEDesktopDisplay> Create(AACEPlayerController* Controller);
    /** A mode-only fullscreen switch uses the monitor, not the old window size. */
    static FIntPoint ResolveResolution(EWindowMode::Type Previous, EWindowMode::Type Next,
        FIntPoint Requested, FIntPoint Desktop, bool ExplicitResolution);
    static FIntPoint GetMonitorResolution(AACEPlayerController* Controller);
    /** Display modes for the monitor containing the game window, plus usable window sizes. */
    static TArray<FIntPoint> GetAvailableResolutions(AACEPlayerController* Controller, EWindowMode::Type Mode = EWindowMode::Fullscreen);
    static TArray<FIntPoint> BuildResolutionOptions(const TArray<FIntPoint>& Reported,
        FIntPoint Desktop, FIntPoint Current, bool IncludeCommonSizes);
    /** Preview display changes for 15 seconds before saving them. */
    static void ApplyModeChange(AACEPlayerController* Controller, EWindowMode::Type Mode, FIntPoint Resolution);
    static bool IsModeChangePending();
    static void FinishModeChange(bool Keep);
    /** Recover a usable desktop window and reset graphics preferences only. */
    static void ResetGraphics(AACEPlayerController* Controller);
    static EWindowMode::Type ToggleMode(EWindowMode::Type Actual, EWindowMode::Type Preferred)
    {
        return Actual == EWindowMode::Windowed
            ? (Preferred == EWindowMode::Fullscreen ? EWindowMode::Fullscreen : EWindowMode::WindowedFullscreen)
            : EWindowMode::Windowed;
    }
};
