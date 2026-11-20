#include "ACEClientBuild.h"
#include "Mobile/ACEMobileSettings.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SWindow.h"

const TCHAR* ACEClientBuild::ProductName(bool bVR)
{
    return UACEMobileSettings::IsEnabled() && !bVR ? TEXT("AC:Unreal Mobile") : PLATFORM_ANDROID || bVR ? VRProductName : DesktopProductName;
}

void ACEClientBuild::UpdateWindowTitle(UWorld* World, bool bVR)
{
	if (!World || (World->WorldType != EWorldType::Game && World->WorldType != EWorldType::PIE)) return;
	if (const auto* Viewport = World->GetGameViewport())
	{
		auto Window = Viewport->GetWindow();
		// UE 5.8's registered scene viewport can precede the legacy window cache.
		if (!Window && FSlateApplication::IsInitialized())
			if (const auto* SceneViewport = Viewport->GetGameViewport())
				if (const auto Widget = SceneViewport->GetViewportWidget().Pin())
					Window = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef());
		if (Window)
		{
			// Embedded PIE shares an editor window; only rename dedicated preview windows.
			if (World->WorldType == EWorldType::PIE && Window->GetTag() != FName(TEXT("PIEWindow"))) return;
			Window->SetTitle(FText::FromString(ProductName(bVR)));
		}
	}
}
