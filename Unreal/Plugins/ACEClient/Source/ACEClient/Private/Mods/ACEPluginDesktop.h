#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SConstraintCanvas.h"

class UACEPluginSubsystem;
class SStackBox;
class SScrollBox;

/** Nonmodal desktop plugin launcher and independent movable plugin windows. */
class SACEPluginDesktop : public SCompoundWidget
{
    friend class FACEPluginHostTest;
    friend class FACEPluginWindowTest;
    friend class FACELedgerTest;
    friend class FACEUCMLogTest;
    friend class FACEActionDeckTest;
public:
    SLATE_BEGIN_ARGS(SACEPluginDesktop):_Host(nullptr),_ManagerOnly(false){} SLATE_ARGUMENT(UACEPluginSubsystem*, Host) SLATE_ARGUMENT(bool,ManagerOnly) SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Refresh();
    /** Show and raise an existing window, creating it only on its first open. */
    bool Open(const FString& Id);
    void Toggle(const FString& Id);
    bool IsOpen(const FString& Id) const;
    bool ContainsPoint(const FString& Id, FVector2D Absolute) const;
    void SaveOverlayLayout();
    virtual void Tick(const FGeometry& Geometry, double Time, float Delta) override;
private:
    struct FWindow
    {
        TSharedPtr<SWidget> Widget;
        SConstraintCanvas::FSlot* Slot = nullptr;
        FVector2D Position, Size;
        FString Id,LedgerId;
        bool HiddenChrome=false;
        bool Open = true;
        bool MapOverlay = false;
        bool PluginBar = false;
    };
    TWeakObjectPtr<UACEPluginSubsystem> Host;
    TSharedPtr<SConstraintCanvas> Canvas;
    TSharedPtr<SStackBox> Buttons;
    TSharedPtr<SScrollBox> BarScroll;
    TMap<FString, FWindow> Windows;
    FString Signature;
    FVector2D ViewSize = FVector2D::ZeroVector;
    int32 TopOrder = 1;
    bool ConstrainWindows = true;
    bool ManagerOnly = false;
    FString ResolveWindowId(const FString& Id) const;
    void Layout(FWindow& Window);
    FVector2D DisplaySize(const FWindow& Window) const;
    void Move(const FString& Id, FVector2D Delta, bool Save);
    void Resize(const FString& Id, FVector2D Delta, bool Save);
    void Raise(const FString& Id);
    void Hide(const FString& Id, bool Remember = true);
};
