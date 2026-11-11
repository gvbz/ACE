#include "ACEPlayerController.h"
#include "ACEOrbitCameraBoom.h"
#include "Mobile/ACEMobileWidget.h"
#include "Mobile/ACEMobileSettings.h"
#include "ACEInventoryRules.h"
#include "ACEProfiling.h"
#include "ACERetailTurn.h"
#include "UI/ACERadarVisuals.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEActionDeckSubsystem.h"
#include "VR/ACEVRLocomotion.h"
#include "ACEKeyboardRouter.h"
#include "ACEDesktopDisplay.h"
#include "Widgets/SWindow.h"
#include "ACEDesktopPointer.h"
#include "HAL/IConsoleManager.h"
#include "VR/ACEVRComponent.h"
#include "ACEClientBuild.h"
#include "ACERuntimeOptions.h"
#include "ACEInputBindings.h"
#include "ACERetailPortalAnimation.h"
#include "ACERetailUseRange.h"
#include "ACEClientSubsystem.h"
#include "ACEPlaySessionRedirect.h"
#include "Engine/WorldComposition.h"
#include "ACEOpcodes.h"
#include "ACESession.h"
#include "ACEDatSubsystem.h"
#include "ACEScriptComponent.h"
#include "Dat/ACEEnvCellMeshBuilder.h"
#include "Dat/ACECellTransit.h"
#include "Dat/ACEDatFileTypes.h"
#include "ACELoginWidget.h"
#include "ACEHoverTooltipWidget.h"
#include "ACEMouseCursorWidget.h"
#include "UI/ACEGameHUDWidget.h"
#include "UI/ACEFrameRateWidget.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUICharSelectBinder.h"
#include "UI/ACEUICharGenBinder.h"
#include "UI/ACEUIFlow.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUIElement.h"
#include "UI/ACEUITypes.h"
#include "UI/ACEUIResourceResolver.h"
#include "ACELoadingScreenActor.h"
#include "ACEWcTerrainRuntime.h"
#include "ACESkyDomeActor.h"
#include "ACEMovementComponent.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACECombatTargeting.h"
#include "ACECombatStance.h"
#include "ACEWorldPresenterComponent.h"
#include "ACETerrainPresenterComponent.h"
#include "ACEWorldEntityActor.h"
#include "ACEEnvCellActor.h"
#include "ACELandblockActor.h"
#include "ACETerrainChunkActor.h"
#include "ProceduralMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/GameViewportSubsystem.h"
#include "Blueprint/SlateBlueprintLibrary.h"
#include "Slate/SGameLayerManager.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "SceneView.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "UObject/GarbageCollection.h"
#include "RenderCommandFence.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/GameModeBase.h"
#include "Components/CapsuleComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "ACEBodySweep.h"
#include "ACEVisibleObjectPick.h"
#include "WorldCollision.h"
#include "Engine/OverlapResult.h"
#include "TimerManager.h"
#include "Engine/HitResult.h"
#include "Camera/PlayerCameraManager.h"
#include "ACECameraRetail.h"
#include "ACECameraSettings.h"
#include "Personal/PersonalCamera.h" // personal: camera
#include "ACELedgeSlide.h"
#include "ACELandingMotion.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

namespace
{
EMouseLockMode DesktopMouseLockMode()
{
    // Both windowed modes allow the visible pointer onto the desktop. Camera
    // turning has its own temporary capture, released when mouse look ends.
    const auto Window=GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWindow() : nullptr;
    return Window && FACEDesktopPointer::ShouldConfine(Window->GetWindowMode(), true, true)
        ? EMouseLockMode::LockAlways : EMouseLockMode::DoNotLock;
}
// CTransition::transitional_insert/edge_slide retains the previous supported
// position when a grounded step loses contact. A sloping face alone is not
// support: its convex edge can touch the side of the foot over an open drop.
// Center support within StepDown still permits normal stair/ramp descent.
bool HasLedgeSupport(UWorld& World, UACEDatSubsystem* Dat, const FVector& Feet,
    float Radius, float StepDown, float Scale, bool bOutdoor, const FCollisionQueryParams& Params, int32 SphereCount)
{
    float SupportZ;
    if (ACEBodySweep::FindFootSupport(World, Feet, Radius, StepDown, Params,
        SupportZ, 1.f, false, true, nullptr, true, SphereCount)) return true;
    // A failed polygon footprint must not be overridden by a center ray
    // spanning both retail step-down attempts. CheckWalkable probes only one
    // half-step from the rejected rim contact.
    const float WalkableDown=ACEBodySweep::StepDownProbeDistance(Radius,SphereCount,StepDown);
    TArray<FHitResult> Hits;
    ACEBodySweep::TraceGround(World, Hits, Feet + FVector(0,0,1.f),
        Feet - FVector(0,0,WalkableDown + .5f), Params);
    for (const FHitResult& Hit : Hits)
        if (Hit.bBlockingHit && !ACEBodySweep::IsCreatureBody(Hit)
            && Hit.ImpactNormal.Z >= .6641741f && Hit.ImpactPoint.Z >= Feet.Z-WalkableDown)
            return true;
    FVector Normal;
    return bOutdoor && Dat && Dat->SampleOutdoorGroundZ(Feet.X,Feet.Y,Scale,SupportZ,&Normal)
        && Normal.Z >= .6641741f && SupportZ <= Feet.Z+1.f && SupportZ >= Feet.Z-WalkableDown;
}

// The rendered water sheet remains at the heightfield plane. Retail body
// support is below it by DAT waterDepth; a Chaos sphere/capsule against that
// sheet would undo the already depth-correct ground sample. While wading,
// retain scenery/interior collision and solve terrain using the DAT path.
void FilterWadingTerrain(UWorld& World, UACEDatSubsystem* Dat, const FVector& From,
    const FVector& To, float Scale, FCollisionQueryParams& Params)
{
    if (!Dat || (Dat->GetOutdoorWaterDepthCm(From.X,From.Y,Scale)<=0.f
        && Dat->GetOutdoorWaterDepthCm(To.X,To.Y,Scale)<=0.f)) return;
    for (TActorIterator<AACELandblockActor> It(&World); It; ++It)
        if (It->TerrainMesh) Params.AddIgnoredComponent(It->TerrainMesh.Get());
    for (TActorIterator<AACETerrainChunkActor> It(&World); It; ++It)
        if (It->TerrainMesh) Params.AddIgnoredComponent(It->TerrainMesh.Get());
}
}

AACEPlayerController::AACEPlayerController()
{
	bShowMouseCursor = true;
}

void AACEPlayerController::UpdateKeyboardAutoRun(float& Forward, bool bToggleDown)
{
	// Retail cancels autorun on a NEW forward/backward command. Releasing the
	// key that was already held when autorun started must not stop the player.
	const bool bTogglePressed = bToggleDown && !bNumLockWasDown;
	if (bTogglePressed) bAutoRun = !bAutoRun;
	else if (!FMath::IsNearlyZero(Forward) && !FMath::IsNearlyEqual(Forward, PreviousManualForward)) bAutoRun = false;
	PreviousManualForward = Forward;
	bNumLockWasDown = bToggleDown;
	if (bAutoRun) Forward = 1.f;
}

void AACEPlayerController::BeginPlay()
{
	ACERuntimeOptions::Apply();
	Super::BeginPlay();
    if(IsMobileActive())ActivateTouchInterface(nullptr);
	if (IsLocalController()) DesktopDisplay=FACEDesktopDisplay::Create(this);
	if (IsLocalController()) KeyboardRouter=FACEKeyboardRouter::Create(this,[this]()
	{
		return Client && Client->GetSessionState()==EACESessionState::InWorld && !IsVRActive()
			&& !ACEInputBindings::IsEditing() && !ACEInputBindings::IsTextEntryFocused()
			&& !(DatGameplayBinder && DatGameplayBinder->IsChatEntryFocused())
			&& !(GameHUDWidget && GameHUDWidget->IsChatEntryFocused());
	});
	if (IsLocalController()) ACEClientBuild::UpdateWindowTitle(GetWorld(), IsVRActive());
#if WITH_EDITOR
	// Standalone/PIE is a network client even while the editor window lacks focus.
	// UEditorEngine otherwise clamps it to 3 fps. Scope the exemption to this client
	// lifetime; do not change the user's global editor performance preferences.
	if (GEditor && IsLocalController())
	{
		auto Delegate = UEditorEngine::FShouldDisableCPUThrottling::CreateWeakLambda(this, [this]()
		{
			return IsVRActive() || (Client && (Client->GetSessionState() == EACESessionState::InWorld
				|| Client->GetSessionState() == EACESessionState::EnteringWorld));
		});
		EditorThrottleDelegate = Delegate.GetHandle();
		GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));
	}
#endif
	UE_LOG(LogTemp, Warning, TEXT("ACE: AACEPlayerController::BeginPlay — IsLocalController=%d"), IsLocalController());
	if (UGameInstance* GI = GetGameInstance())
	{
		Client = GI->GetSubsystem<UACEClientSubsystem>();
		if (Client)
		{
			Client->OnEnteredWorld.AddDynamic(this, &AACEPlayerController::HandleEnteredWorld);
			Client->OnPlayerTeleportStarted.AddDynamic(this, &AACEPlayerController::HandlePlayerTeleportStarted);
			Client->OnPositionUpdate.AddDynamic(this, &AACEPlayerController::HandlePositionUpdate);
			Client->OnMotionUpdate.AddDynamic(this, &AACEPlayerController::HandleMotionUpdate);
			Client->OnPhysicsStateUpdate.AddDynamic(this, &AACEPlayerController::HandlePhysicsStateUpdate);
			Client->OnCharacterList.AddDynamic(this, &AACEPlayerController::HandleCharacterList);
			Client->OnSessionStateChanged.AddDynamic(this, &AACEPlayerController::HandleSessionStateChanged);
			Client->OnObjectCreated.AddDynamic(this, &AACEPlayerController::HandleObjectCreated);
			Client->OnVitalsUpdated.AddDynamic(this, &AACEPlayerController::HandleVitalsUpdated);
			if (TSharedPtr<FACESession> Sess = Client->GetSession())
			{
				Sess->OnUseDone.RemoveAll(this);
				Sess->OnUseDone.AddUObject(this, &AACEPlayerController::HandleUseDone);
				Sess->OnMoveToFailed.RemoveAll(this);
				Sess->OnMoveToFailed.AddUObject(this, &AACEPlayerController::HandleMoveToFailed);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("ACE: AACEPlayerController::BeginPlay — UACEClientSubsystem NOT found on GameInstance"));
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("ACE: AACEPlayerController::BeginPlay — IsGameWorld=%d HasLocalPlayer=%d"),
		GetWorld() ? GetWorld()->IsGameWorld() : -1, GetLocalPlayer() ? 1 : 0);

	if (IsLocalController())
	{
		if (ACEPlaySessionRedirect::GPendingEnteredWorldAfterTravel.IsSet())
		{
			const ACEPlaySessionRedirect::FPendingEnteredWorldAfterTravel Pending =
				ACEPlaySessionRedirect::GPendingEnteredWorldAfterTravel.GetValue();
			ACEPlaySessionRedirect::GPendingEnteredWorldAfterTravel.Reset();
			GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, Pending]()
			{
				HandleEnteredWorld(Pending.PlayerGuid, Pending.SpawnPosition);
			}));
		}

		if (ULocalPlayer* LP = GetLocalPlayer())
		{
			if (LP->ViewportClient)
			{
				LP->ViewportClient->EngineShowFlags.SetFog(true);
				LP->ViewportClient->EngineShowFlags.SetAtmosphere(false);
				LP->ViewportClient->EngineShowFlags.SetDynamicShadows(false);
			}
		}
		DeferredShowLoginUI();
	}
}

void AACEPlayerController::EnsureDatIntroCanvas()
{
	if (!IsLocalController() || !bUseDatDrivenHud || !Client)
	{
		return;
	}

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			if (!Dat->IsDatReady())
			{
				Dat->BeginBackgroundLoad();
			}
		}
	}

	if (!DatCanvasWidget)
	{
		DatCanvasWidget = CreateWidget<UACEUICanvasWidget>(this, UACEUICanvasWidget::StaticClass());
		if (DatCanvasWidget)
		{
			DatCanvasWidget->InitializeCanvas(Client->GetUIElementManager());
			DatCanvasWidget->SetResourceResolver(Client->GetUIResourceResolver());
		}
	}
	if (!DatCanvasWidget)
	{
		return;
	}

	if (UACEUIFlow* Flow = Client->GetUIFlow())
	{
		Flow->SetMode(ACEUI::EACEUIFlowMode::Intro);
	}
	if (UACEUILayoutResolver* Layout = Client->GetUILayoutResolver())
	{
		Layout->LoadLayout(ACEUI::LayoutId::Intro);
	}
	if (UACEUIResourceResolver* Res = Client->GetUIResourceResolver())
	{
		Res->ResolveTexture(0x0600610Fu);
	}
	if (!IsVRActive() && !DatCanvasWidget->IsInViewport())
	{
		DatCanvasWidget->AddToPlayerScreen(8000);
	}
	DatCanvasWidget->SetVisibility(ESlateVisibility::Visible);
	UE_LOG(LogTemp, Log, TEXT("ACE: UIFlow Intro layout 0x21000001 (splash under credentials)"));
}

void AACEPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UpdateObjectHighlights(false);
	KeyboardRouter.Reset();
	DesktopDisplay.Reset();
	DesktopPointer.Reset();
	if (IsLocalController()) ACERuntimeOptions::ApplyDesktopUIScale(FIntPoint::ZeroValue,true);
#if WITH_EDITOR
	if (GEditor)
	{
		GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll([this](const auto& Delegate)
		{
			return Delegate.GetHandle() == EditorThrottleDelegate;
		});
	}
#endif
	SetSkyWeatherEnabled(true);
	if (LoadingScreenActor)
	{
		LoadingScreenActor->HideAllVisuals();
		LoadingScreenActor->Destroy();
		LoadingScreenActor = nullptr;
	}
	bEnterWorldLoading = false;
	bWorldRevealActive = false;

	// Prefer a clean CharacterLogOff so the account is not left orphaned online.
	if (Client)
	{
		Client->Logout();
		Client->OnEnteredWorld.RemoveAll(this);
		Client->OnPlayerTeleportStarted.RemoveAll(this);
		Client->OnPositionUpdate.RemoveAll(this);
		Client->OnMotionUpdate.RemoveAll(this);
		Client->OnPhysicsStateUpdate.RemoveAll(this);
		Client->OnCharacterList.RemoveAll(this);
		Client->OnSessionStateChanged.RemoveAll(this);
		Client->OnObjectCreated.RemoveAll(this);
		Client->OnVitalsUpdated.RemoveAll(this);
		if (TSharedPtr<FACESession> Sess = Client->GetSession())
		{
			Sess->OnUseDone.RemoveAll(this);
			Sess->OnMoveToFailed.RemoveAll(this);
		}
	}

	DestroyGameHUD();
	if(FrameRateWidget){FrameRateWidget->RemoveFromParent();FrameRateWidget=nullptr;}
	Super::EndPlay(EndPlayReason);
}

void AACEPlayerController::DeferredShowLoginUI()
{
	// Defer to next frame so PIE can finish its first Slate/viewport draw before building login widgets.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(this, &AACEPlayerController::ShowLoginUI);
	}
}

bool AACEPlayerController::IsLoginUIVisible() const
{
	if (const auto* VR = GetPawn() ? GetPawn()->FindComponentByClass<UACEVRComponent>() : nullptr;
		VR && VR->IsActive()) return VR->IsLoginPanelReady();
	if (!LoginWidget || !LoginWidget->IsInViewport())
	{
		return false;
	}

	// IsInViewport() only means the widget is COMPOSITED into the viewport's widget tree — it
	// says nothing about whether its allocated slot actually has a non-zero size. Right after
	// AddToPlayerScreen, the slot can still be sitting at a zero-sized rect (viewport pixel size
	// wasn't known yet), in which case the widget is technically "in the viewport" but 100%
	// invisible. Require a real, non-zero size before calling it "visible" so callers (e.g. the
	// diagnostic HUD) don't hide themselves prematurely.
	const FVector2D CachedGeomSize = LoginWidget->GetCachedGeometry().GetLocalSize();
	if (CachedGeomSize.X > 1.f && CachedGeomSize.Y > 1.f)
	{
		return true;
	}

	const FVector2D Desired = LoginWidget->GetDesiredSize();
	return Desired.X > 1.f && Desired.Y > 1.f;
}

bool AACEPlayerController::IsSessionDisconnected() const
{
	return !Client || Client->GetSessionState() == EACESessionState::Disconnected;
}

void AACEPlayerController::ShowLoginUI()
{
	// Deferred viewport retries can arrive after the character list. They must
	// not put the account launcher back over the screen that replaced it.
	if (DatCharSelectBinder || DatCharGenBinder || (Client &&
		(Client->GetSessionState()==EACESessionState::EnteringWorld || Client->GetSessionState()==EACESessionState::InWorld))) return;
	UWorld* World = GetWorld();
	const bool bHasGameViewport = World && World->GetGameViewport() != nullptr;

	// Never stream landblocks / world entities on the login screen.
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			Dat->SetWorldStreamingAllowed(false);
		}
	}

	// Hide default pawn / sky so DAT Intro splash + credentials card are the only UI.
	if (APawn* P = GetPawn())
	{
		P->SetActorHiddenInGame(true);
		P->SetActorEnableCollision(false);
		// Keep WC from streaming Dereth landscape tiles at the real PlayerStart while login UI runs.
		P->SetActorLocation(FVector::ZeroVector, false, nullptr, ETeleportType::TeleportPhysics);
	}
	if (World)
	{
		for (TActorIterator<AACESkyDomeActor> It(World); It; ++It)
		{
			if (AACESkyDomeActor* Sky = *It)
			{
				Sky->SetActorHiddenInGame(true);
				Sky->SetWeatherEnabled(false);
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("ACE: ShowLoginUI called (existing LoginWidget=%s, InViewport=%d, HasLocalPlayer=%d, HasGameViewport=%d)"),
		LoginWidget ? TEXT("valid") : TEXT("NULL"),
		LoginWidget ? LoginWidget->IsInViewport() : 0,
		GetLocalPlayer() ? 1 : 0,
		bHasGameViewport ? 1 : 0);

	EnsureDatIntroCanvas();

	if (!LoginWidget)
	{
		TSubclassOf<UACELoginWidget> ClassToUse = LoginWidgetClass;
		if (!ClassToUse)
		{
			ClassToUse = UACELoginWidget::StaticClass();
		}
		UE_LOG(LogTemp, Warning, TEXT("ACE: ShowLoginUI — creating widget of class %s"), *ClassToUse->GetName());

		LoginWidget = CreateWidget<UACELoginWidget>(this, ClassToUse);
		if (!LoginWidget)
		{
			UE_LOG(LogTemp, Error, TEXT("ACE: ShowLoginUI — CreateWidget FAILED for class %s"), *ClassToUse->GetName());
			if (bShowACEDebug && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(6002, 8.f, FColor::Red,
					TEXT("ACE: ShowLoginUI - CreateWidget FAILED"));
			}
			return;
		}
	}
	LoginWidget->SetUseDatIntroBackdrop(true);
	if (IsVRActive())
	{
		// The VR component owns this widget's world-space presentation. Login retries
		// must not attach a fullscreen copy over the headset's hands and keyboard.
		LoginWidget->SetVisibility(ESlateVisibility::Visible);
		ApplyLoginUIInputMode();
		return;
	}

	if (LoginWidget->IsInViewport())
	{
		// Widget is already actually on screen — re-assert visibility/input and viewport slot.
		UE_LOG(LogTemp, Warning, TEXT("ACE: ShowLoginUI — widget already in viewport, re-asserting visibility/input"));
		LoginWidget->SetVisibility(ESlateVisibility::Visible);
		LoginWidget->SetUseDatIntroBackdrop(true);
		DesiredSizeRetryAttempts = 0;
		ApplyViewportSizeToLoginWidget();
		ApplyLoginUIInputMode();
		return;
	}

	// The UObject exists but was never (successfully) composited into the viewport yet.
	// AddToPlayerScreen/AddToViewport silently return false (no crash, no visible error in
	// Standalone) if World->GetGameViewport() isn't ready — this is extremely common right
	// after BeginPlay in "Play Standalone" while the game window is still being created.
	if (!bHasGameViewport)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE: ShowLoginUI — no GameViewport yet, will retry"));
		ScheduleShowLoginUIRetry();
		return;
	}

	if (AttachLoginWidgetToScreen())
	{
		ApplyLoginUIInputMode();
		ShowLoginUIRetryAttempts = 0;
		UE_LOG(LogTemp, Warning, TEXT("ACE: ShowLoginUI — widget attached to player screen (ZOrder=9999) — IsInViewport=%d DesiredSize=%s"),
			LoginWidget->IsInViewport() ? 1 : 0, *LoginWidget->GetDesiredSize().ToString());

		if (bShowACEDebug && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(6001, 8.f, FColor::Yellow,
				TEXT("ACE: Login widget CREATED + AddToPlayerScreen (confirmed IsInViewport)"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ACE: ShowLoginUI — AddToPlayerScreen returned FALSE (GetGameViewport/OwningLocalPlayer not ready), will retry"));
		if (bShowACEDebug && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(6003, 3.f, FColor::Red,
				TEXT("ACE: AddToPlayerScreen FAILED — retrying..."));
		}
		ScheduleShowLoginUIRetry();
	}
}

bool AACEPlayerController::AttachLoginWidgetToScreen()
{
	if (!LoginWidget)
	{
		return false;
	}

	// AddToPlayerScreen (rather than AddToViewport) ties the widget to this controller's
	// LocalPlayer/viewport explicitly — more reliable than AddToViewport when the game
	// viewport window is still being created (e.g. "Play In New Window"). It returns false
	// (without crashing or logging anything visible) if the viewport isn't ready yet, so the
	// return value MUST be checked.
	if (!LoginWidget->AddToPlayerScreen(9999))
	{
		return false;
	}

	LoginWidget->SetVisibility(ESlateVisibility::Visible);
	LoginWidget->SetIsFocusable(true);

	DesiredSizeRetryAttempts = 0;
	ApplyViewportSizeToLoginWidget();

	return LoginWidget->IsInViewport();
}

void AACEPlayerController::ApplyViewportSizeToLoginWidget()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DesiredSizeRetryTimerHandle);
	}

	if (!LoginWidget || !LoginWidget->IsInViewport())
	{
		return;
	}

	FVector2D ViewportSizePx = FVector2D::ZeroVector;
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewportSizePx);
	}
	if (ViewportSizePx.X <= 1.f || ViewportSizePx.Y <= 1.f)
	{
		if (const ULocalPlayer* LP = GetLocalPlayer())
		{
			if (LP->ViewportClient && LP->ViewportClient->Viewport)
			{
				ViewportSizePx = FVector2D(LP->ViewportClient->Viewport->GetSizeXY());
			}
		}
	}
	if (ViewportSizePx.X <= 1.f || ViewportSizePx.Y <= 1.f)
	{
		if (UWorld* World = GetWorld())
		{
			if (UGameViewportClient* GVC = World->GetGameViewport())
			{
				if (GVC->Viewport)
				{
					ViewportSizePx = FVector2D(GVC->Viewport->GetSizeXY());
				}
			}
		}
	}

	const bool bViewportSizeValid = ViewportSizePx.X > 1.f && ViewportSizePx.Y > 1.f;
	if (!bViewportSizeValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE: ApplyViewportSizeToLoginWidget — viewport size not ready yet"));
		ScheduleDesiredSizeRetry();
		return;
	}

	// Set the complete stretch slot together. SetPositionInViewport resets anchors
	// to (0,0), silently turning this into an auto-sized top-left widget. The lobby's
	// anchored canvas needs the allocated viewport, not its tiny desired size.
	if (UGameViewportSubsystem* Viewport = UGameViewportSubsystem::Get())
	{
		FGameViewportWidgetSlot Slot = Viewport->GetWidgetSlot(LoginWidget);
		Slot.Anchors = FAnchors(0.f, 0.f, 1.f, 1.f);
		Slot.Alignment = FVector2D::ZeroVector;
		Slot.Offsets = FMargin(0.f);
		Viewport->SetWidgetSlot(LoginWidget, Slot);
	}
	LoginWidget->ForceLayoutPrepass();

	const FVector2D ContentDesiredSize = LoginWidget->GetDesiredSize();
	const FVector2D CachedGeomSize = LoginWidget->GetCachedGeometry().GetLocalSize();
	UE_LOG(LogTemp, Warning,
		TEXT("ACE: ApplyViewportSizeToLoginWidget — ViewportSizePx=%s ContentDesiredSize=%s CachedGeom=%s"),
		*ViewportSizePx.ToString(),
		*ContentDesiredSize.ToString(),
		*CachedGeomSize.ToString());

	if (ContentDesiredSize.IsNearlyZero() || CachedGeomSize.X <= 1.f || CachedGeomSize.Y <= 1.f)
	{
		ScheduleDesiredSizeRetry();
	}
}

void AACEPlayerController::ScheduleDesiredSizeRetry()
{
	++DesiredSizeRetryAttempts;

	// ~15s of retries at 0.15s intervals — same budget as ScheduleShowLoginUIRetry.
	constexpr int32 MaxDesiredSizeRetryAttempts = 100;
	if (DesiredSizeRetryAttempts > MaxDesiredSizeRetryAttempts)
	{
		UE_LOG(LogTemp, Error, TEXT("ACE: ScheduleDesiredSizeRetry — giving up after %d attempts, viewport size never became valid"),
			DesiredSizeRetryAttempts);
		return;
	}

	if (DesiredSizeRetryAttempts <= 3)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimerForNextTick(this, &AACEPlayerController::ApplyViewportSizeToLoginWidget);
			return;
		}
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(DesiredSizeRetryTimerHandle, this, &AACEPlayerController::ApplyViewportSizeToLoginWidget, 0.15f, false);
	}
}

void AACEPlayerController::RefreshDesktopDisplayInputMode()
{
	if (IsVRActive()) return;
	if (FSlateApplication::IsInitialized()) FSlateApplication::Get().ReleaseAllPointerCapture();
	if (LoginWidget) ApplyLoginUIInputMode();
	else if (bMouseLookActive) SetMouseLookActive(true);
	else ApplyInWorldInputMode();
}

void AACEPlayerController::ApplyLoginUIInputMode()
{
	if (IsVRActive()) { SetInputMode(FInputModeGameOnly()); bShowMouseCursor = false; return; }
	if (!LoginWidget)
	{
		return;
	}
	bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	InputMode.SetLockMouseToViewportBehavior(DesktopMouseLockMode());
	InputMode.SetWidgetToFocus(LoginWidget->TakeWidget());
	SetInputMode(InputMode);
}

void AACEPlayerController::ScheduleShowLoginUIRetry()
{
	++ShowLoginUIRetryAttempts;

	// ~15s of retries at 0.15s intervals — generous enough for even a slow window/viewport
	// startup, but bounded so we don't retry forever if something is fundamentally broken.
	constexpr int32 MaxRetryAttempts = 100;
	if (ShowLoginUIRetryAttempts > MaxRetryAttempts)
	{
		UE_LOG(LogTemp, Error, TEXT("ACE: ScheduleShowLoginUIRetry — giving up after %d attempts, GameViewport never became ready"), ShowLoginUIRetryAttempts);
		if (bShowACEDebug && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(6004, 10.f, FColor::Red,
				TEXT("ACE: Login UI FAILED — GameViewport never became ready"));
		}
		return;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(ShowLoginUIRetryTimerHandle, this, &AACEPlayerController::ShowLoginUI, 0.15f, false);
	}
}

void AACEPlayerController::HideLoginUI()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ShowLoginUIRetryTimerHandle);
		World->GetTimerManager().ClearTimer(DesiredSizeRetryTimerHandle);
		// ShowLoginUI hides the sky actor — restore it when credentials leave.
		for (TActorIterator<AACESkyDomeActor> It(World); It; ++It)
		{
			if (AACESkyDomeActor* Sky = *It)
			{
				Sky->SetActorHiddenInGame(false);
			}
		}
	}
	if (LoginWidget)
	{
		LoginWidget->RemoveFromParent();
		LoginWidget = nullptr;
	}
	ApplyInWorldInputMode();
}

UACEVRComponent* AACEPlayerController::GetVRComponent() const
{
	return GetPawn() ? GetPawn()->FindComponentByClass<UACEVRComponent>() : nullptr;
}

void AACEPlayerController::ApplyInWorldInputMode()
{
	if (IsVRActive()) { SetInputMode(FInputModeGameOnly()); bShowMouseCursor = false; return; }
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	Mode.SetLockMouseToViewportBehavior(DesktopMouseLockMode());
	SetInputMode(Mode);
}

void AACEPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	// Development packages inherit Engine debug bindings too. Reserve the game
	// keys even when an older Saved/Input.ini still contains those defaults.
	if (PlayerInput) PlayerInput->DebugExecBindings.Reset();
	GetMutableDefault<UInputSettings>()->ConsoleKeys.Reset();
	// WASD is polled in PlayerTick so no DefaultInput.ini mappings are required.
}

EMouseCursor::Type AACEPlayerController::GetMouseCursor() const
{
	// The game viewport can query the controller instead of the Slate HUD
	// before mouse capture (notably on Linux). Use the same hover hit test.
	if (bShowMouseCursor && bUseDatDrivenHud && Client && DatCanvasWidget
		&& DatCanvasWidget->IsVisible() && FSlateApplication::IsInitialized())
	{
		if (const auto* Manager = Client->GetUIElementManager())
		{
			const FGeometry& Geometry = DatCanvasWidget->GetCachedGeometry();
			const EMouseCursor::Type Cursor = Manager->GetWindowCursor(
				Geometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos()), Geometry.GetLocalSize());
			if (Cursor != EMouseCursor::Default) return Cursor;
		}
	}
	return Super::GetMouseCursor();
}

void AACEPlayerController::SetInputMode(const FInputModeDataBase& InData)
{
	Super::SetInputMode(InData);
#if PLATFORM_LINUX
	if (IsVRActive())
	{
		DesktopPointer.Reset();
		return;
	}
	if (auto* LP = GetLocalPlayer(); LP && LP->ViewportClient)
	{
		if (!DesktopPointer) DesktopPointer = FACEDesktopPointer::Create(LP->ViewportClient);
		if (DesktopPointer)
		{
			FACEDesktopPointer::PrepareInputMode(LP->GetSlateOperations(), *LP->ViewportClient);
			DesktopPointer->Update(true);
		}
	}
#endif
}

bool AACEPlayerController::InputKey(const FInputKeyEventArgs& Params)
{
 if(IsMobileActive() && MobileWidget && (Params.Key==EKeys::Android_Back || Params.Key==EKeys::Escape))
 {
  if(Params.Event==IE_Pressed)
  {
   if(Client && Client->GetSessionState()==EACESessionState::CharacterSelect)Client->Logout();
   else if(!MobileWidget->Panel.IsNone())MobileWidget->ClosePanel();
   else {MobileWidget->bWheel=false;MobileWidget->bEditing=false;MobileWidget->OpenPanel("Menu");}
  }
  return true;
 }
 // Text controls may leave key-down unhandled while consuming the character.
 // Do not let that key reach gameplay. Releases still clear previously held keys.
 if(ACEInputBindings::IsTextEntryFocused() && !Params.Key.IsMouseButton() && Params.Event!=IE_Released)
 {
  // Preserve controller modifier state so a chord can leave chat as well as
  // enter it. Gameplay polling is blocked while the text field owns focus.
  if(Params.Key.IsGamepadKey())Super::InputKey(Params);
  return true;
 }
 // Record controller buttons even when an immediate UI action consumes them:
 // they may also be held as modifiers for another button in this same frame.
 const bool GamepadHandled=Params.Key.IsGamepadKey()?Super::InputKey(Params):false;
 const auto Mods=FSlateApplication::Get().GetModifierKeys();
 const FInputChord Chord(Params.Key,Mods.IsShiftDown(),Mods.IsControlDown(),Mods.IsAltDown(),Mods.IsCommandDown());
 // Not every input source goes through Slate (e.g. viewport/simulated events).
 // Claim deck chords before Super records them for the native hotbar poll.
 // Releases always reach Super so switching override cannot leave keys held.
 if((Params.Event==IE_Pressed||Params.Event==IE_Repeat)&&GetGameInstance())
  if(auto* Deck=GetGameInstance()->GetSubsystem<UACEActionDeckSubsystem>())
   if(Deck->HandleKey(Chord,Params.Event==IE_Repeat)||Deck->ReservesKey(Chord))return true;
 if(Params.Event==IE_Pressed)MovementKeyPressOrder.Add(Params.Key,++MovementKeySequence);
 if(Params.Event==IE_Pressed && ACEInputBindings::Matches(ACEInputBindings::Action(TEXT("ToggleUCM")),Chord,this)
  && !ACEInputBindings::IsEditing() && Client && Client->GetSessionState()==EACESessionState::InWorld
  && !(DatGameplayBinder && DatGameplayBinder->IsChatEntryFocused()) && !(GameHUDWidget && GameHUDWidget->IsChatEntryFocused()))
 {
  if(auto* Plugins=GetGameInstance()->GetSubsystem<UACEPluginSubsystem>())Plugins->ToggleUCM();
  return true;
 }
 if (Params.Event==IE_Pressed && ACEInputBindings::Matches(ACEInputBindings::Action(TEXT("ToggleChat")),Chord,this)
  && !ACEInputBindings::IsEditing() && Client && Client->GetSessionState()==EACESessionState::InWorld
  && !IsDesktopInterfaceHidden() && DatGameplayBinder && DatCanvasWidget && DatCanvasWidget->IsVisible())
 {
  DatGameplayBinder->ToggleChatEntryFocus(); return true;
 }
 if(Params.Event==IE_Pressed && ACEInputBindings::Matches(ACEInputBindings::Action(TEXT("Chat")),Chord,this)
  && Client && Client->GetSessionState()==EACESessionState::InWorld && !IsDesktopInterfaceHidden())
 {
  if(DatGameplayBinder && DatCanvasWidget && DatCanvasWidget->IsVisible() && !DatGameplayBinder->IsChatEntryFocused())
  {
   DatGameplayBinder->FocusChatEntry();return true;
  }
  if(GameHUDWidget && !GameHUDWidget->IsChatEntryFocused())
  {
   GameHUDWidget->FocusChatEntry();return true;
  }
 }
 const bool Handled=Params.Key.IsGamepadKey()?GamepadHandled:Super::InputKey(Params);
 // Some desktop backends report stick axes without directional button events.
 // Feed those directions through the ordinary rebindable action path, retaining
 // analog magnitude separately for movement/camera and releasing on recenter.
 if (Params.Event==IE_Axis && !IsVRActive() && ACEInputBindings::StickDirectionKey(Params.Key,1.f).IsValid())
 {
  for(float Sign:{-1.f,1.f})
  {
   const FKey Direction=ACEInputBindings::StickDirectionKey(Params.Key,Sign);
   const bool Down=Params.AmountDepressed*Sign>.2f;
   if(Down!=IsInputKeyDown(Direction))
   {
    auto Event=Params;Event.Key=Direction;Event.Event=Down?IE_Pressed:IE_Released;Event.AmountDepressed=Down?1.f:0.f;
    InputKey(Event);
   }
  }
  return true;
 }
 // A hidden HUD cannot receive NativeOnMouseWheel. Handle wheel events that
 // reach the viewport through the same binding-aware zoom path. Slate consumes
 // scrolling over visible panels before it gets here. Use only the axis event:
 // the viewport also sends a pressed/released pair for each wheel movement.
 if (Params.Key==EKeys::MouseWheelAxis && Params.Event==IE_Axis
  && Client && Client->GetSessionState()==EACESessionState::InWorld && !IsVRActive())
 {
  ApplyCameraWheelZoom(Params.AmountDepressed);
  return true;
 }
 // These actions are polled, so PlayerInput has no bound delegate to mark the
 // event handled. Letting Tab escape to Slate navigates focus, releasing mouse
 // capture and held movement keys even though targeting itself is non-modal.
 if (!Params.Key.IsMouseButton() && Client && Client->GetSessionState()==EACESessionState::InWorld
  && !ACEInputBindings::IsEditing() && !(DatGameplayBinder && DatGameplayBinder->IsChatEntryFocused())
  && !(GameHUDWidget && GameHUDWidget->IsChatEntryFocused()))
  for (const auto& Action:ACEInputBindings::Actions()) if (ACEInputBindings::Matches(Action.Key,Chord,this)) return true;
 return Handled;
}


void AACEPlayerController::UpdateFrameRateOverlay()
{
	const bool Enabled=Client && Client->GetSessionState()==EACESessionState::InWorld
		&& ACERuntimeOptions::Get(TEXT("ShowFrameRate"))>.5f && !IsDesktopInterfaceHidden() && !IsMobileActive();
	if(!Enabled)
	{
		if(FrameRateWidget && FrameRateWidget->GetVisibility()!=ESlateVisibility::Collapsed)
		{
			FrameRateWidget->SetVisibility(ESlateVisibility::Collapsed);
			FrameRateWidget->RemoveFromParent();FrameRateWidget->ResetSample();
		}
		return;
	}
	if(!FrameRateWidget)FrameRateWidget=CreateWidget<UACEFrameRateWidget>(this);
	if(!FrameRateWidget)return;
	FrameRateWidget->Sample(FPlatformTime::Seconds());
	const bool Desktop=!IsVRActive();
	bool Added=false;
	if(!Desktop) { if(FrameRateWidget->IsInViewport())FrameRateWidget->RemoveFromParent(); }
	else if(!FrameRateWidget->IsInViewport())
	{
		FrameRateWidget->AddToViewport(10000);Added=true;
	}
	const float Dpi=FMath::Max(.01f,UWidgetLayoutLibrary::GetViewportScale(this));
	FrameRateWidget->UpdatePresentation(Client?Client->GetUIElementManager():nullptr,Desktop,
		UWidgetLayoutLibrary::GetViewportSize(this)/Dpi,Added);
}

void AACEPlayerController::PlayerTick(float DeltaTime)
{
	ACE_PROFILE_SCOPE(Player);
	Super::PlayerTick(DeltaTime);
	auto* VR = GetPawn() ? GetPawn()->FindComponentByClass<UACEVRComponent>() : nullptr;
	const bool bVR = VR && VR->IsActive();
    const bool bMobile = IsMobileActive();
    UpdateMobileInterface();
	if (DesktopPointer) DesktopPointer->Update(!bVR && !bMobile);
	if (Client) if (auto* Manager=Client->GetUIElementManager())
		Manager->SetQuickShowUi(!bVR && Client->GetSessionState()==EACESessionState::InWorld
			&& FSlateApplication::IsInitialized() && FSlateApplication::Get().IsActive()
			&& ACEInputBindings::Down(this,ACEInputBindings::Action(TEXT("QuickShowUI"))));
	if (IsLocalController())
	{
		int32 Width=0,Height=0; GetViewportSize(Width,Height);
		ACERuntimeOptions::ApplyDesktopUIScale(FIntPoint(Width,Height),bVR || bMobile);
		UpdateFrameRateOverlay();
	}
	if (bVR) VR->PrepareMovement(DeltaTime);
	if (bVR) PortalWorldRevealElapsed = -1.f;
	if (PortalWorldRevealElapsed >= 0.f && PlayerCameraManager)
	{
		PortalWorldRevealElapsed += FMath::Max(0.f, DeltaTime);
		int32 W = 1280, H = 720; GetViewportSize(W, H);
		const float Aspect = static_cast<float>(W) / FMath::Max(1, H);
		PlayerCameraManager->SetFOV(ACERetailPortalAnimation::StretchFov(
			ACECameraRetail::HorizontalFovDegrees(Aspect, GetCameraFieldOfViewDegrees()), Aspect, PortalWorldRevealElapsed, true));
		if (PortalWorldRevealElapsed < 1.f) return;
		PortalWorldRevealElapsed = -1.f;
		PlayerCameraManager->UnlockFOV();
	}

	// Per-tick diagnostic message — gated behind bShowACEDebug (default false) since the UMG
	// login widget is now the primary UI and this was previously spamming the screen every frame.
	if (bShowACEDebug && GEngine && IsLocalController())
	{
		FVector2D DebugViewportSize = FVector2D::ZeroVector;
		if (const ULocalPlayer* LP = GetLocalPlayer())
		{
			if (LP->ViewportClient && LP->ViewportClient->Viewport)
			{
				DebugViewportSize = FVector2D(LP->ViewportClient->Viewport->GetSizeXY());
			}
		}

		GEngine->AddOnScreenDebugMessage(6000, 0.f, FColor::Yellow,
			FString::Printf(TEXT("ACE: PC alive — Session=%s LoginWidget=%s InViewport=%s LoginUIVisible=%s DesiredSize=%s CachedGeomSize=%s ViewportSize=%s"),
				Client ? *UEnum::GetValueAsString(Client->GetSessionState()) : TEXT("NoClient"),
				LoginWidget ? TEXT("valid") : TEXT("NULL"),
				LoginWidget ? (LoginWidget->IsInViewport() ? TEXT("YES") : TEXT("no")) : TEXT("n/a"),
				IsLoginUIVisible() ? TEXT("YES") : TEXT("no"),
				LoginWidget ? *LoginWidget->GetDesiredSize().ToString() : TEXT("n/a"),
				LoginWidget ? *LoginWidget->GetCachedGeometry().GetLocalSize().ToString() : TEXT("n/a"),
				*DebugViewportSize.ToString()));
	}

	if (bPendingEnterWorldTransition)
	{
		bPendingEnterWorldTransition = false;
		if (Client && Client->GetSessionState() == EACESessionState::EnteringWorld)
		{
			DestroyCharacterSelectUI();
			BeginEnterWorldLoadScreen();
		}
	}
	const bool bLoggingOff = TickLogOffTransition();
	if (bLoggingOff || ((bEnterWorldLoading || bWorldRevealActive) && Client
		&& (Client->GetSessionState() == EACESessionState::EnteringWorld
			|| Client->GetSessionState() == EACESessionState::InWorld)))
	{
		if (APawn* P = GetPawn())
			if (auto* App = P->FindComponentByClass<UACECharacterAppearanceComponent>()) App->ResetWalkingMotion();
		UpdateObjectHighlights(false);
		// Retail replaces the SmartBox view, not the gameplay interface. Keep
		// panel/chat controls available without polling movement or world actions.
		if (!IsVRActive() && !ACEInputBindings::IsEditing() && !ACEInputBindings::IsTextEntryFocused()
			&& !(DatGameplayBinder && DatGameplayBinder->IsChatEntryFocused()))
		{
			if (ACEInputBindings::Pressed(this, ACEInputBindings::Action(TEXT("ToggleInterface"))))
				SetDesktopInterfaceHidden(!bDesktopInterfaceHidden);
			if (DatGameplayBinder && !IsDesktopInterfaceHidden())
			{
				DatGameplayBinder->PollInterfaceKeyboardActions(this);
				if (ACEInputBindings::Pressed(this, EKeys::Escape)) DatGameplayBinder->HandleEscape();
			}
		}
		// Logout waits for the server's roster. It must never stream a new
		// destination, reveal the old world, or send an arrival LoginComplete.
		if (!bLoggingOff) TickWorldTransition();
		return;
	}
	if (!Client || Client->GetSessionState() != EACESessionState::InWorld)
	{
		if (APawn* P = GetPawn())
			if (auto* App = P->FindComponentByClass<UACECharacterAppearanceComponent>()) App->ResetWalkingMotion();
		UpdateObjectHighlights(false);
		if (GameHUDWidget)
		{
			DestroyGameHUD();
		}
		TickPendingCharacterSelect();
		if (Client && Client->GetSessionState() == EACESessionState::CharacterSelect && bUseEnterWorldLoadScreen)
			PreparePortalScreen();
		return;
	}

	if (!bVR) { EnsureRetailMouseCursor(); UpdateOrbitCamera(DeltaTime); }

	if (UWorld* World = GetWorld())
	{
		if (World->WorldComposition && World->WorldComposition->GetTilesList().Num() > 0 && Client)
		{
			static constexpr int32 WcGameplayRingTiles = 4;
			static constexpr float WcStreamPollInterval = 0.15f;
			WcTerrainStreamAccum += DeltaTime;
			const FACEPosition Pose = Client->GetPlayerPosition();
			if (Pose.IsValid())
			{
				const FVector Loc = Pose.ToUnrealLocation(WorldScale);
				const float TileCoordX = Loc.X / 612000.f;
				const float TileCoordY = Loc.Y / 612000.f;
				const int32 TileX = (TileCoordX < 0.f)
					? FMath::CeilToInt(TileCoordX - 0.5f)
					: FMath::FloorToInt(TileCoordX + 0.5f);
				const int32 TileY = (TileCoordY < 0.f)
					? FMath::CeilToInt(TileCoordY - 0.5f)
					: FMath::FloorToInt(TileCoordY + 0.5f);
				const bool bTileChanged = TileX != WcLastStreamTileX || TileY != WcLastStreamTileY;
				const bool bFirstStream = WcLastStreamTileX == INT32_MIN;
				if (bTileChanged || bFirstStream)
				{
					if (bTileChanged)
					{
						WcLastStreamTileX = TileX;
						WcLastStreamTileY = TileY;
					}
					else
					{
						WcLastStreamTileX = TileX;
						WcLastStreamTileY = TileY;
					}
					ACEWcTerrainRuntime::ShowWcLandscapesAfterLogin(World);
					ACEWcTerrainRuntime::RequestTerrainAroundBlocking(World, Loc, WcGameplayRingTiles);
				}
				else if (WcTerrainStreamAccum >= WcStreamPollInterval)
				{
					WcTerrainStreamAccum = 0.f;
					ACEWcTerrainRuntime::RequestTerrainAround(World, Loc, WcGameplayRingTiles);
				}
			}
		}
	}

	if (!bVR && !bMobile) { PollObjectHover(); PollObjectClick(); }
	UpdateObjectHighlights(true,!bVR);

	// Poll logical actions through the retail/rebound keymap: W/X forward/back,
	// A/D turn, Z/C sidestep by default. Opposing held commands form a stack.
	// Run is the default hold-key; Shift = walk (retail AC).
	const bool bChatFocused = bVR || (bMobile && MobileWidget && MobileWidget->IsBlockingMovement()) || (DatGameplayBinder && DatGameplayBinder->IsChatEntryFocused())
		|| (GameHUDWidget && GameHUDWidget->IsChatEntryFocused()) || ACEInputBindings::IsEditing() || ACEInputBindings::IsTextEntryFocused();
	if (!bVR && (!bChatFocused || IsDesktopInterfaceHidden())
		&& ACEInputBindings::Pressed(this, ACEInputBindings::Action(TEXT("ToggleInterface"))))
		SetDesktopInterfaceHidden(!bDesktopInterfaceHidden);
	float F = 0.f, R = 0.f, T = 0.f;
	const uint64 MovementGeneration = FMath::Max(MovementSuppressionSequence, ReadingMovementSequence);
	F = ACEInputBindings::MovementAxis(this,EKeys::W,EKeys::S,MovementKeyPressOrder,FKey(),FKey(),MovementGeneration);
	R = ACEInputBindings::MovementAxis(this,EKeys::E,EKeys::Q,MovementKeyPressOrder,FKey(),FKey(),MovementGeneration);
	if (bMouseForwardActive && !bChatFocused) F = 1.f;
	// Retail mouse turning temporarily converts turn commands into sidesteps.
	const bool bMouseFacing = bMouseLookActive && (bInstantMouseLookHeld || bMouseLookToggled || (IsUseMouseTurning() && IsInputKeyDown(EKeys::RightMouseButton)));
	const float TurnKeys=ACEInputBindings::MovementAxis(this,EKeys::D,EKeys::A,MovementKeyPressOrder,FKey(),FKey(),MovementGeneration);
	if(bMouseFacing)R=ACEInputBindings::MovementAxis(this,EKeys::E,EKeys::Q,MovementKeyPressOrder,EKeys::D,EKeys::A,MovementGeneration);else T=TurnKeys;
	F = FMath::Clamp(F, -1.f, 1.f);
	R = FMath::Clamp(R, -1.f, 1.f);
	T = FMath::Clamp(T, -1.f, 1.f);
	bRunning = !ACEInputBindings::Down(this, EKeys::LeftShift);

	UpdateKeyboardAutoRun(F, ACEInputBindings::Down(this, EKeys::NumLock));
    if(bMobile && MobileWidget)
    {
        const auto Stick=MobileWidget->Movement();F=-Stick.Y;R=Stick.X;T=MobileWidget->Turning();
        bRunning=!MobileWidget->IsWalkHeld();bAutoRun=false;
        MobileLook(MobileWidget->ConsumeLook());
    }

	// Retail number row: magic = spell bank slots; peace = inventory shortcuts.
	// DAT HUD owns this when GameHUDWidget is inactive.

	// Retail Space: hold to charge jump bar, release to Jump (0xF61B).
	const bool bSpaceDown = bVR ? VR->IsJumpHeld() : bMobile ? (MobileWidget && MobileWidget->IsJumpHeld()) : ACEInputBindings::Down(this, EKeys::SpaceBar) && !bChatFocused;
	if (!bVR && bSpaceDown && !bSpaceWasDown)
	{
		BeginJumpCharge();
	}
	if (!bVR && bJumpCharging)
	{
		// Retail fills the bar in ~1 second.
		JumpChargeExtent = FMath::Clamp(JumpChargeExtent + DeltaTime / 1.0f, 0.f, 1.f);
		if (DatGameplayBinder)
		{
			DatGameplayBinder->SetJumpChargeFraction(JumpChargeExtent);
		}
	}
	if (!bVR && !bSpaceDown && bSpaceWasDown && bJumpCharging)
	{
		ReleaseJump(F, R);
	}
	bSpaceWasDown = bSpaceDown;
	if (bJumpAirborne)
	{
		JumpAirborneSeconds += DeltaTime;
	}

	if (!bChatFocused && Client && Client->GetSessionState() == EACESessionState::InWorld)
	{
        if(DatGameplayBinder) DatGameplayBinder->PollKeyboardActions(this);
        else if(GameHUDWidget)for(int32 Slot=0;Slot<9;++Slot)
            if(ACEInputBindings::Pressed(this,ACEInputBindings::Shortcut(Slot)))GameHUDWidget->ActivateHotbarSlot(Slot);
	}
	// A hotkey above may have started an attack/cast after these axes were sampled.
	if (MovementGeneration != FMath::Max(MovementSuppressionSequence, ReadingMovementSequence)) F=R=T=0.f;
	if (!bVR && DatGameplayBinder)
	{
		DatGameplayBinder->SetCombatMovementBlocked(bJumpAirborne
			|| (!bChatFocused && (!FMath::IsNearlyZero(F) || !FMath::IsNearlyZero(R))));
	}

	// Retail pickup is distinct from Use: a portable pack can be opened or collected.
	if (!bChatFocused && ACEInputBindings::Pressed(this, ACEInputBindings::Action(TEXT("Pickup"))) && Client
		&& Client->GetSessionState() == EACESessionState::InWorld)
	{
		PickupSelectedObject();
	}

	if (!bChatFocused && DatGameplayBinder
		&& Client && Client->GetSessionState() == EACESessionState::InWorld)
	{
		// ToD Keyboard mapping defaults (Emotes + Movement + UI tabs).
		if (ACEInputBindings::Pressed(this, EKeys::G))
		{
			DatGameplayBinder->PlayEmoteHotkey(ACEMotion::Sitting, true);
		}
		else if (ACEInputBindings::Pressed(this, EKeys::B))
		{
			DatGameplayBinder->PlayEmoteHotkey(ACEMotion::Sleeping, true);
		}
		else if (ACEInputBindings::Pressed(this, EKeys::C))
		{
			DatGameplayBinder->PlayEmoteHotkey(ACEMotion::Crouch, true);
		}
		else if (ACEInputBindings::Pressed(this, EKeys::K))
		{
			DatGameplayBinder->PlayEmoteHotkey(0x430000f0u, true); // PointState (retail held pose)
		}
		else if (ACEInputBindings::Pressed(this, EKeys::J))
		{
			DatGameplayBinder->PlayEmoteHotkey(0x13000087u, false); // Wave
		}
		else if (ACEInputBindings::Pressed(this, EKeys::Tilde))
		{
			DatGameplayBinder->ToggleCombatModeHotkey();
		}
		else if (ACEInputBindings::Pressed(this, EKeys::Home))
		{
			SelectLastAttacker();
		}
		else if (ACEInputBindings::Pressed(this, EKeys::Escape))
		{
			DatGameplayBinder->HandleEscape();
			if (APawn* P = GetPawn())
			{
				if (UACECharacterAppearanceComponent* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
				{
					App->CancelHeldActionMotion();
				}
			}
		}
		else if (ACEInputBindings::Pressed(this, ACEInputBindings::Action(TEXT("ClosestMonster")))) CycleNearbyTarget(true, 0);
		else if (ACEInputBindings::Pressed(this, ACEInputBindings::Action(TEXT("ClosestItem")))) CycleNearbyTarget(false, 0);
		else if (ACEInputBindings::Pressed(this, EKeys::LeftBracket)) CycleNearbyTarget(false, -1);
		else if (ACEInputBindings::Pressed(this, EKeys::RightBracket)) CycleNearbyTarget(false, 1);
		else if (ACEInputBindings::Pressed(this, EKeys::Semicolon)) CycleNearbyTarget(true, -1);
		else if (ACEInputBindings::Pressed(this, EKeys::Apostrophe)) CycleNearbyTarget(true, 1);
	}

	if (!bChatFocused && ACEInputBindings::Down(this, ACEInputBindings::Action(TEXT("Stop"))))
	{
		F=R=T=0.f; bAutoRun=false;
		if (Client && Client->GetSession())
		{
			auto Session = Client->GetSession();
			bForceMovementResend |= ACEMotion::IsHeldPoseSubstate(Session->GetCurrentForwardCommand());
			Session->ClearHeldForwardCommand();
		}
		if (APawn* P = GetPawn())
			if (auto* App = P->FindComponentByClass<UACECharacterAppearanceComponent>()) App->FinishEmoteMotion();
	}
	if (bVR) { VR->GetMovement(F, R, bRunning); T = 0.f; bAutoRun = false; }
	if (bVR && ReadingMovementSequence) F=R=T=0.f;
	bool bUniformVRMovement = bVR && Client->GetSession() && Client->GetSession()->SupportsUniformVRMovement();
	if (bUniformVRMovement)
	{
		const FVector2D Stick = ACEVRLocomotion::Input(F, R);
		F = Stick.Y; R = Stick.X;
	}
	const FVector VRRoomDelta = bVR ? VR->GetRoomScaleDelta() : FVector::ZeroVector;
	// Losing tracking or opening an interface must stop automatic VR translation.
	if (bVR && VR->IsInputBlocked() && bServerMoveToActive)
	{
		HandleMoveToFailed(0);
		if (const auto Session = Client->GetSession()) Session->CancelWorldUseApproach();
	}
	// Manual input cancels server-directed MoveTo approach.
	const bool bManualKeys = !FMath::IsNearlyZero(F) || !FMath::IsNearlyZero(R) || !FMath::IsNearlyZero(T);
	if (bManualKeys && ReadingMovementSequence)
	{
		// A new press may interrupt server control (CommandInterpreter::TakeControlFromServer).
		// Stop the reading pose too; never slide forward while still displaying it.
		ReadingMovementSequence = 0;
		if (Client->GetSession()) Client->GetSession()->ClearHeldForwardCommand();
		if (GetPawn()) if (auto* App=GetPawn()->FindComponentByClass<UACECharacterAppearanceComponent>()) App->FinishEmoteMotion();
	}
	if (bManualKeys) ReclaimManualMovement();
	if (bManualKeys && (bServerMoveToActive || bAwaitingUseDone || ApproachUseSendCount > 0))
	{
		const bool bCancelUse = bAwaitingUseDone;
		ClearServerMoveTo();
		if (bCancelUse)
			if (const auto Session = Client->GetSession()) Session->CancelWorldUseApproach();
	}

	// Manual input may have ended an approach above; choose its negotiated
	// movement model immediately rather than spending one frame at legacy speed.
	bUniformVRMovement &= !bServerMoveToActive;
	// Follow Use approach: face the target first (direct heading), then walk straight in.
	if (bServerMoveToActive && !bManualKeys && ServerMoveToTargetGuid != 0)
	{
		FACEWorldObject Target;
		if (Client->GetWorldObject(ServerMoveToTargetGuid, Target) && Target.bHasPosition)
		{
			FACEPosition PoseForFacing = bHavePredictedPose ? PredictedPose : Client->GetPlayerPosition();
			// ACE Position origins are cylinder bottoms. The pawn actor is at the Unreal
			// capsule center, so use the predicted protocol pose for server-exact range.
			const FVector SelfLoc = PoseForFacing.ToUnrealLocation(WorldScale);
			const FVector TargetLoc = Target.Position.ToUnrealLocation(WorldScale);
			FVector To = TargetLoc - SelfLoc;
			To.Z = 0.f;
			const float DistCm = To.Size();
			const float CylinderDistCm = GetUseCylinderDistanceCm(Target, SelfLoc, TargetLoc);
			// Preserve authored UseRadius (portals often use negative = must overlap cylinders).
			// Never coerce negative/zero → 0.6 — that made Use fire early and fail server-side.
			const float UseRadiusAc = bServerMoveToActive ? ServerMoveToDistance : Target.UseRadius;
			const float UseRadiusCm = UseRadiusAc * WorldScale;
			// Match server IsWithinUseRadiusOf (cylinder gap ≤ UseRadius). No DistCm shortcut.

			// MoveToManager::HandleMoveToPosition checks full 3D displacement, after arrival.
			// Preserve the server's limit (15 AC for charge); zero denotes its unlimited default.
			if (CylinderDistCm > UseRadiusCm
				&& bHaveApproachStartPose && ServerMoveToFailDistance > 0.f)
			{
				const FVector StartLoc = ApproachStartPose.ToUnrealLocation(WorldScale);
				if (FVector::Distance(SelfLoc, StartLoc) / WorldScale > ServerMoveToFailDistance)
				{
					HandleMoveToFailed(0x003D);
					if (TSharedPtr<FACESession> Sess = Client->GetSession())
					{
						Sess->ReportMoveToFailure(0x003D);
					}
				}
			}

			if (bServerMoveToActive)
			{
			// Retail MoveToManager recomputes the heading while approaching. A locked
			// ray misses the target after sliding around collision or target movement.
			if (DistCm > 1.f)
			{
				ApproachTargetDir = To.GetSafeNormal();
			}

			const float Dot = ApproachTargetDir.IsNearlyZero()
				? 1.f
				: PoseForFacing.GetFacingDotUnreal2D(ApproachTargetDir);
			constexpr float FaceDotMin = 0.999999f;
			const bool bFacing = bVR || Dot >= FaceDotMin;

			if (UseApproachPhase == EACEUseApproachPhase::None)
			{
				UseApproachPhase = EACEUseApproachPhase::Turning;
			}
			if (UseApproachPhase == EACEUseApproachPhase::Turning)
			{
				ApproachTurnSeconds += DeltaTime;
			}
			if (UseApproachPhase == EACEUseApproachPhase::Turning && bFacing)
			{
				// Finish at the exact door heading, then lock that heading for the run.
				PredictedPose = PoseForFacing;
				if (!bVR) PredictedPose.SetAceFacingFromUnrealDir2D(ApproachTargetDir);
				if (APawn* P = GetPawn())
				{
					P->SetActorRotation(PredictedPose.ToUnrealQuat());
				}
				Client->SetReportedPosition(PredictedPose);
				UseApproachPhase = EACEUseApproachPhase::Walking;
				ApproachStallSeconds = 0.f;
				ApproachStallLastLoc = SelfLoc;
				ApproachTurnSeconds = 0.f;
			}
			const bool bInUseRange = CylinderDistCm <= UseRadiusCm;
			// Retail checks progress in distance/time. A fixed displacement per frame
			// classified normal walking at high FPS as a stall, then invented arrival
			// up to three AC units outside the server's actual use radius.
			if (UseApproachPhase == EACEUseApproachPhase::Walking && !bInUseRange)
			{
				const float MovedCm = GetUseCylinderDistanceCm(Target, ApproachStallLastLoc, TargetLoc) - CylinderDistCm;
				ApproachStallLastLoc = SelfLoc;
				if (MovedCm < 0.25f * WorldScale * DeltaTime)
				{
					ApproachStallSeconds += DeltaTime;
				}
				else
				{
					ApproachStallSeconds = 0.f;
				}
				// Blocked far from the target (solid furniture/walls) — retail MoveToManager
				// cancels on failed progress. Without this the approach latched
				// bServerMoveToActive forever and later gives failed with "You're too busy!".
				if (ApproachStallSeconds >= 2.5f)
				{
					UE_LOG(LogTemp, Log, TEXT("ACE: Use approach stalled out of range guid=0x%08X cyl=%.1f — cancel"),
						ServerMoveToTargetGuid, CylinderDistCm);
					HandleMoveToFailed(0);
					if (const auto Session = Client->GetSession()) Session->CancelWorldUseApproach();
					// Phase is now None — the rest of this block is inert this tick.
				}
			}
			auto SendApproachUse = [&](bool bForce = false)
			{
				// Give / server MoveOnly: never Use — CreateMoveToChain completes from AutoPos.
				// Sending Use runs StopExistingMoveToChains and cancels the pending give.
				if (bApproachMoveOnly || (!bResendUseWhenInRange && !bApproachPickupIntoInventory))
				{
					return;
				}
				if (!bResendUseWhenInRange && !bForce)
				{
					return;
				}
				FACEWorldObject ApproachTarget;
				const bool bHaveApproachTarget =
					Client->GetWorldObject(ServerMoveToTargetGuid, ApproachTarget);
				if (bHaveApproachTarget
					&& (Client->GetOpenVendorGuid() == ServerMoveToTargetGuid
						|| Client->GetOpenExternalContainerGuid() == ServerMoveToTargetGuid))
				{
					ClearServerMoveTo();
					return;
				}
				// Every world interaction is one request. Repeating it can toggle a
				// container/door closed or restart the server's move-to chain.
				if (!bApproachPickupIntoInventory)
				{
					if (ApproachUseSendCount > 0)
					{
						if (ApproachTarget.IsVendor())
						{
							VendorOpenWaitSeconds += DeltaTime;
							if (VendorOpenWaitSeconds >= 3.f)
							{
								ClearServerMoveTo();
							}
						}
						// Do not ClearServerMoveTo / StopMovement here — GameActionMoveToState
						// cancels the server's CreateMoveToChain. Wait for UseDone.
						return;
					}
				}
				const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
				// Throttle a deferred pickup/interaction, without replaying a sent Use.
				if (!bForce && LastApproachUseTime > 0.0 && (Now - LastApproachUseTime) < 0.35)
				{
					return;
				}
				LastApproachUseTime = Now;
				if (!bApproachMoveOnly)
				{
					bResendUseWhenInRange = true;
				}
				if (bHavePredictedPose)
				{
					Client->SetReportedPosition(PredictedPose);
				}
				Client->FlushAutonomousPosition(true);
				Client->SetForcePositionReporting(true);
				if (bApproachPickupIntoInventory)
				{
					SendInventoryPickup(ServerMoveToTargetGuid, ApproachPickupAmount);
					if (APawn* Possessed = GetPawn())
					{
						if (UACECharacterAppearanceComponent* App =
							Possessed->FindComponentByClass<UACECharacterAppearanceComponent>())
						{
							// MotionCommand.Pickup — play immediately; server broadcast can lag.
							App->PlayActionMotion(static_cast<int32>(0x40000018));
						}
					}
					UE_LOG(LogTemp, Log, TEXT("ACE: PutInContainer at range guid=0x%08X cyl=%.1f use=%.1f"),
						ServerMoveToTargetGuid, CylinderDistCm, UseRadiusCm);
					// One shot only — StartPickup sets IsBusy until the anim finishes;
					// Holding retries spam WeenieError.YoureTooBusy.
					ClearServerMoveTo();
					return;
				}
				else
				{
					Client->SendUseItem(ServerMoveToTargetGuid);
					++ApproachUseSendCount;
					bAwaitingUseDone = true;
					UE_LOG(LogTemp, Log, TEXT("ACE: Use at range guid=0x%08X cyl=%.1f use=%.1f"),
						ServerMoveToTargetGuid, CylinderDistCm, UseRadiusCm);
				}
			};
			if (UseApproachPhase == EACEUseApproachPhase::Walking && bInUseRange)
			{
				// Reaching use/charge distance stops translation. Facing continues
				// at the retail turn rate; arrival cannot snap onto a moving target.
				UseApproachPhase = EACEUseApproachPhase::Holding;
				if (!bApproachMoveOnly && ApproachUseSendCount == 0 && Client)
				{
					// Stop the client run BEFORE Use. Any MoveToState after Use (including
					// axes-0) cancels server CreateMoveToChain.
					Client->SendMovementEx(0.f, 0.f, 0.f, bRunning, false, true);
					ForwardSent = 0.f;
					RightSent = 0.f;
					TurnSent = 0.f;
					bWasMoving = false;
				}
				if (!bResendUseWhenInRange && !bApproachPickupIntoInventory)
				{
					// Give hand-off: flush final pose so server CreateMoveToChain succeeds; do not Use.
					ApproachStallSeconds = 0.f;
					if (bHavePredictedPose)
					{
						Client->SetReportedPosition(PredictedPose);
					}
					Client->FlushAutonomousPosition(true);
					Client->SetForcePositionReporting(true);
					UE_LOG(LogTemp, Log, TEXT("ACE: Give/MoveOnly at range guid=0x%08X cyl=%.1f use=%.1f"),
						ServerMoveToTargetGuid, CylinderDistCm, UseRadiusCm);
				}
				else
				{
					SendApproachUse(true);
				}
			}
			// Keep reporting the arrived pose until the server finishes Use/Give.
			// Opening a container is never retried: Use is a toggle on many servers.
			if (UseApproachPhase == EACEUseApproachPhase::Holding && bInUseRange)
			{
				if (!bResendUseWhenInRange && !bApproachPickupIntoInventory)
				{
					const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
					if (LastApproachUseTime <= 0.0 || (Now - LastApproachUseTime) >= 0.35)
					{
						LastApproachUseTime = Now;
						if (bHavePredictedPose)
						{
							Client->SetReportedPosition(PredictedPose);
						}
						Client->FlushAutonomousPosition(true);
						Client->SetForcePositionReporting(true);
					}
					ApproachStallSeconds += DeltaTime;
					if (ApproachStallSeconds >= 1.25f && !(Client && Client->IsUseBusy()))
					{
						ClearServerMoveTo();
					}
				}
				else
				{
					FACEWorldObject HoldTarget;
					if (Client->GetWorldObject(ServerMoveToTargetGuid, HoldTarget)
						&& ((HoldTarget.IsVendor()
								&& Client->GetOpenVendorGuid() == ServerMoveToTargetGuid)
							|| Client->GetOpenExternalContainerGuid() == ServerMoveToTargetGuid))
					{
						ClearServerMoveTo();
					}
					else
					{
						SendApproachUse(false);
					}
				}
			}
			if (UseApproachPhase == EACEUseApproachPhase::Holding
				&& !bInUseRange)
			{
				// A delayed server MoveTo can tighten the radius after we started
				// holding (ACE does this for magic stance). Resume toward the new
				// distance even after a long wait; holding time is not failed walking.
				// Give / MoveOnly approaches must stay MoveOnly: sending Use runs
				// StopExistingMoveToChains on the server and cancels the pending give.
				UseApproachPhase = bFacing ? EACEUseApproachPhase::Walking : EACEUseApproachPhase::Turning;
				ApproachStallSeconds = 0.f;
				ApproachStallLastLoc = SelfLoc;
				ApproachTurnSeconds = 0.f;
				bResendUseWhenInRange = !bApproachMoveOnly && ApproachUseSendCount == 0;
			}

			F = 0.f;
			R = 0.f;
			T = 0.f;
			if (UseApproachPhase == EACEUseApproachPhase::Walking
				|| (UseApproachPhase == EACEUseApproachPhase::Holding && !bInUseRange && bResendUseWhenInRange))
			{
				// Already facing — run straight forward only (no strafe).
				// Keep F=1 until Use fires at true arrive; Holding may still close if Use pending.
				F = 1.f;
				if (bVR)
				{
					// Approach in world space without rotating the player's head/body frame.
					const FVector BodyForward = VR->GetBodyForward();
					F = FVector::DotProduct(ApproachTargetDir, BodyForward);
					R = FVector::DotProduct(ApproachTargetDir, FVector::CrossProduct(FVector::UpVector, BodyForward));
				}
			}
			// Turning/Holding: axes stay 0; heading is applied directly on PredictedPose below.
			} // bServerMoveToActive (after fail-distance check)
		}
	}

	bool bPluginDrivingMovement=false;
	bool bPluginFastBuffMovement=false;
	if (auto* Plugins = GetGameInstance()->GetSubsystem<UACEPluginSubsystem>())
    {
        const FVector Facing = bVR ? VR->GetBodyForward() : (GetPawn() ? GetPawn()->GetActorForwardVector() : FVector::ForwardVector);
        bPluginFastBuffMovement=Plugins->IsFastBuffMovementActive();
        Plugins->ApplyMovement(this, F, R, T, bManualKeys || bSpaceDown,
            (!bVR && bChatFocused) || (bVR && VR->IsMovementBlocked()) || bJumpAirborne || bServerMoveToActive || ReadingMovementSequence!=0, bVR, Facing,
            Client->GetLocomotionSpeed(bRunning) * GetLocalCreatureScale() * WorldScale * DeltaTime);
        bPluginDrivingMovement=Plugins->IsDrivingMovement();
    }

    if(bPluginJumpQueued)
    {
        if(bManualKeys || bSpaceDown)CancelPluginJump();
        else if(!bJumpAirborne && GetPawn() && !bServerMoveToActive && (bVR?!VR->IsMovementBlocked():!bChatFocused))
        {
            const float Angle=FMath::FindDeltaAngleDegrees(GetPawn()->GetActorRotation().Yaw,PluginJumpHeading);
            // Angle is Unreal yaw; AC-space turn integration reflects its sign.
            F=R=0;T=bVR?0:FMath::Clamp(Angle/45.f,-1.f,1.f);
            if(bVR || FMath::Abs(Angle)<3)
            {
                if(!PluginJumpStarted){BeginJumpCharge();PluginJumpStarted=FPlatformTime::Seconds();}
                JumpChargeExtent=FMath::Clamp(float(FPlatformTime::Seconds()-PluginJumpStarted),0.f,1.f);
                if(JumpChargeExtent>=PluginJumpCharge)
                {
                    const float Rad=FMath::DegreesToRadians(Angle);
                    bRunning=!bPluginWalkJump;ReleaseJump(bVR?PluginJumpForward*FMath::Cos(Rad)-PluginJumpStrafe*FMath::Sin(Rad):PluginJumpForward,bVR?PluginJumpForward*FMath::Sin(Rad)+PluginJumpStrafe*FMath::Cos(Rad):PluginJumpStrafe);
                    bPluginJumpQueued=false;PluginJumpStarted=0;
                }
            }
        }
    }

	if (!bVR && GetPawn())
		if (auto* App=GetPawn()->FindComponentByClass<UACECharacterAppearanceComponent>(); App && App->IsMovementLockedByAction())
		{ F=0.f; R=0.f; }

	// Retail: no air control of the arc — lock JumpWorldAceVelocity. A/D may still yaw the
	// mesh while airborne (visual only); charge keeps turn for aim.
	const float RawF = F;
	const float RawR = R;
	if (bJumpCharging && bStandingJumpLocked)
	{
		// Snapshot aim for release (W/S/Q/E) without allowing loco prediction mid-charge.
		StandingJumpAimF = RawF;
		StandingJumpAimR = RawR;
	}
	if (bStandingJumpLocked && (bJumpCharging || bJumpAirborne))
	{
		F = 0.f;
		R = 0.f;
	}
	if (bJumpAirborne)
	{
		F = 0.f;
		R = 0.f;
		// Keep T — rotate the model; ballistic velocity stays JumpWorldAceVelocity.
	}

    PendingMouseTurnDegrees = 0.f;
    // Explicit keyboard turning takes precedence over camera-follow steering.
	// Automatic approach supplies F too; its heading belongs to the target, not the camera.
	if (!bServerMoveToActive && !bPluginDrivingMovement && bMouseFacing && FMath::IsNearlyZero(T) && GetPawn())
    {
        if (auto* Boom = GetPawn()->FindComponentByClass<USpringArmComponent>())
            PendingMouseTurnDegrees = ACECameraRetail::FollowTurnDegrees(Boom->GetRelativeRotation().Yaw,
                DeltaTime, true, true);
    }
	if (!FMath::IsNearlyZero(PendingMouseTurnDegrees))
	{
		ReclaimManualMovement();
	}

	ForwardAxis = F;
	RightAxis = R;
	TurnAxis = T;

	const bool bMouseTurnSend = bMouseFacing
		&& FMath::Abs(PendingMouseTurnDegrees) > 0.01f;
	// Mouse commands explicitly apply HoldRun; keyboard commands inherit the
	// current hold key. The same raw speed drives prediction and the packet.
	const float TurnRateDegPerSec=FMath::RadiansToDegrees(1.5f)*((bMouseFacing||bRunning)?1.5f:1.f);
	const float ReportedTurn = bMouseTurnSend
		? FMath::Clamp(PendingMouseTurnDegrees / FMath::Max(TurnRateDegPerSec*DeltaTime, KINDA_SMALL_NUMBER),-1.5f,1.5f) : T;
	MouseTurnReportSeconds += DeltaTime;
	const bool bMouseTurnReport = (bMouseTurnSend || bMouseTurnSent) && MouseTurnReportSeconds >= .5f;
	// Mouse events are sparse at low speeds. Bridge short gaps in footwork only;
	// heading prediction and outgoing raw input still stop immediately.
	if (bMouseFacing && !bVR && !bJumpAirborne)
	{
		if (!FMath::IsNearlyZero(PendingMouseTurnDegrees))
		{
			MouseTurnAnimationIdle=0.f;
			MouseTurnAnimationRate=FMath::Clamp(PendingMouseTurnDegrees/FMath::Max(TurnRateDegPerSec*DeltaTime,KINDA_SMALL_NUMBER),-1.5f,1.5f)*1.5f;
		}
		else if ((MouseTurnAnimationIdle+=DeltaTime)>.1f) MouseTurnAnimationRate=0.f;
	}
	else {MouseTurnAnimationIdle=0.f;MouseTurnAnimationRate=0.f;}
	auto LocalAnimationRate = [&](float Forward, float Strafe)
	{
		if (bUniformVRMovement)
			return ACEVRLocomotion::AnimationRate(Forward, Strafe, bRunning, Client->GetRunRate());
		// CMotionInterp::adjust_motion scales SideStep by 0.5 * 3.12 / 1.25,
		// then Run multiplies/clamps it to 3. Match the physical sidestep rate;
		// passing 1 here left local feet at one third of the high-skill cadence.
		if (FMath::Abs(Strafe) > FMath::Abs(Forward) + KINDA_SMALL_NUMBER)
			return FMath::Abs(Strafe) * Client->GetSidestepSpeed(bRunning) / 1.25f;
		// Route arrival (and analog input) can request less than a full stride.
		// CMotionInterp scales cycle speed by that input before the run modifier;
		// use the same multiplier as prediction and RawMotionState.ForwardSpeed.
		return !FMath::IsNearlyZero(Forward)
			? FMath::Abs(Forward) * (bRunning ? Client->GetRunRate() : 1.f) : 1.f;
	};
	// Drive walk/run anim on the local player's appearance from the same axes we send to the server.
	FTransform WalkingRootDelta = FTransform::Identity;
	float WalkingCycleSeconds = DeltaTime;
	bool bWalkingTransitionThisFrame = false, bWalkingTransitionPending = false;
	if (APawn* PossessedPawn = GetPawn())
	{
		if (UACECharacterAppearanceComponent* App = PossessedPawn->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			App->SetWalkingTransitionsEnabled(!bVR && !bJumpAirborne && !bJumpCharging
				&& !bServerMoveToActive && !bPluginDrivingMovement && !bPluginFastBuffMovement);
			// Retail StopObjectCompletely and ordinary key-up both return through
			// the motion table. Stop/chat suppress input, not the unfinished step.
			App->SetTurnInput(bVR || bJumpAirborne ? 0.f : bMouseFacing ? MouseTurnAnimationRate : ReportedTurn*(bRunning?1.5f:1.f));
			if (!bJumpAirborne && !FMath::IsNearlyZero(F)
				&& (FMath::Sign(F) != FMath::Sign(ForwardSent) || bForceMovementResend))
			{
				App->InterruptCastWithMovement();
				App->FinishMissileMotion();
			}
			App->UpdateCellLighting((bHavePredictedPose ? PredictedPose : Client->GetPlayerPosition()).CellId);
			if (bJumpAirborne)
			{
				App->SetSuppressLocoIdleBlend(true);
				// Standing jump: no loco axes. Run jump: keep Raw axes for land resume.
				if (!bStandingJumpLocked && (!FMath::IsNearlyZero(RawF) || !FMath::IsNearlyZero(RawR)))
				{
					App->SetLocomotionInput(RawF, RawR, bRunning,
						LocalAnimationRate(RawF, RawR), bUniformVRMovement);
				}
				else
				{
					App->SetLocomotionInput(0.f, 0.f, false, 1.f);
				}
			}
			else if (bJumpCharging)
			{
				// Retail keeps the current run/walk or Ready cycle while charging.
				if (!bStandingJumpLocked && (!FMath::IsNearlyZero(RawF) || !FMath::IsNearlyZero(RawR)))
				{
					// Discard an old airborne pose before continuing grounded movement.
					App->ClearJumpMotionIfAny();
					App->SetSuppressLocoIdleBlend(false);
					App->SetLocomotionInput(RawF, RawR, bRunning,
						LocalAnimationRate(RawF, RawR), bUniformVRMovement);
				}
				else
				{
					// Retail charge_jump keeps Ready while standing; JumpCharging has
					// no player motion entry and must not latch an unevaluated held pose.
					App->ClearJumpMotionIfAny();
					App->SetLocomotionInput(0.f, 0.f, false, 1.f);
				}
			}
			else
			{
				App->ClearJumpMotionIfAny();
				App->SetSuppressLocoIdleBlend(false);
				App->SetLocomotionInput(F, R, bRunning,
					LocalAnimationRate(F, R), bUniformVRMovement);
			}
			bWalkingTransitionThisFrame = App->HasWalkingTransition();
			WalkingRootDelta = App->AdvanceWalkingMotion(DeltaTime, WalkingCycleSeconds);
			bWalkingTransitionPending = App->HasWalkingTransition();
			// Report the requested raw movement to the server, but don't predict
			// a full-speed walk/run cycle while a missile/physical combat link
			// owns the forward channel. Casting stays mobile on desktop and touch.
			if (!bVR && App->HasPendingCombatForwardMotion()) WalkingCycleSeconds = 0.f;
		}
	}

	const bool bMoving = !FMath::IsNearlyZero(F) || !FMath::IsNearlyZero(R) || !FMath::IsNearlyZero(T) || !VRRoomDelta.IsNearlyZero();
	// Keep local prediction while Use approach is active, and for the full jump arc
	// (otherwise SoftReconcile snaps idle airborne poses back to the ground).
	// The network send threshold must not discard small local mouse turns.
	bLocalPredicting = bMoving || !FMath::IsNearlyZero(PendingMouseTurnDegrees)
		|| bServerMoveToActive || bServerTurnActive || bJumpAirborne || bJumpCharging || bWalkingTransitionThisFrame || !LandingWorldAceVelocity.IsNearlyZero() || LandingRootTrack.IsActive()
		|| (bVR && !VR->IsMovementBlocked());
	const bool bChanged =
		bForceMovementResend ||
		!FMath::IsNearlyEqual(F, ForwardSent) ||
		!FMath::IsNearlyEqual(R, RightSent) ||
		(!(bMouseTurnSend || bMouseTurnSent) && !FMath::IsNearlyEqual(ReportedTurn, TurnSent)) ||
		(bMoving != bWasMoving) ||
		((bMoving || bJumpCharging) && bRunning != bRunningSent) ||
		(bUniformVRMovement != bUniformVRMovementSent) ||
		(bJumpCharging != bJumpChargeSent) ||
		(bJumpAirborne != bJumpAirborneSent) ||
		bMouseTurnReport;
	// After Use is in flight, MoveToState (F=1 leftover or axes-0) cancels CreateMoveToChain.
	// Inventory requests do not set IsUseBusy. Their server-directed approach
	// still reports progress through AutonomousPosition, like retail's
	// MoveToManager / CommandInterpreter::SendPositionEvent. Translating its
	// walk/arrival into a new input command cancels the pending pickup/give.
	// Fast buffs send a synchronous press/release in TryFastBuffTap. They never
	// leave a held axis whose release could be swallowed by this use freeze.
	const bool bFreezeMoveToState = !bManualKeys && !bMouseTurnSend && !bMouseTurnSent && !bJumpAirborne && !bJumpCharging
		&& !bManualMovementCommandSent
		&& (bServerMoveToActive || bServerTurnActive || bAwaitingUseDone || ApproachUseSendCount > 0 || Client->IsUseBusy());

	if (bFreezeMoveToState)
	{
		if (bHavePredictedPose)
		{
			Client->SetReportedPosition(PredictedPose);
		}
		// Session's retail position cadence reports progress. Per-frame AutoPos
		// here made a 240 Hz client send four times as often as a 60 Hz client.
		Client->SetForcePositionReporting(true);
		if (bChanged)
		{
			bForceMovementResend = false;
			ForwardSent = F;
			RightSent = R;
			TurnSent = ReportedTurn;
			bRunningSent = bRunning;
			bWasMoving = bMoving;
			bJumpChargeSent = bJumpCharging;
			bJumpAirborneSent = bJumpAirborne;
		}
	}
	else if (bChanged)
	{
		if (bHavePredictedPose)
		{
			Client->SetReportedPosition(PredictedPose);
		}
		bForceMovementResend = false;
		const bool bStandingLongJump = bJumpCharging && bStandingJumpLocked
			&& JumpChargeExtent > 0.05f;
		if (bJumpAirborne)
		{
			// Contact=0 preserves JumpPack translation. Retail contact_allows_move
			// still permits TurnRight/TurnLeft in the air, so observers must receive
			// the same turn that local prediction applies during the jump.
			Client->SendMovementEx(0.f, 0.f, ReportedTurn, bRunning, false, false, false, bMouseTurnSend);
			Client->SetForcePositionReporting(true);
		}
		else if (bMoving || bJumpCharging || bMouseTurnSend)
		{
			// Standing charge: axes 0 + StandingLongJump bit. Run charge: keep loco Contact=1.
			const float SendF = bStandingJumpLocked ? 0.f : RawF;
			const float SendR = bStandingJumpLocked ? 0.f : RawR;
			float SendT = T;
			if (bMouseTurnSend)
			{
				SendT = ReportedTurn;
			}
			Client->SendMovementEx(SendF, SendR, SendT, bRunning, bStandingLongJump, true, bUniformVRMovement, bMouseTurnSend);
		}
		else if (bServerMoveToActive || (Client && Client->IsUseBusy()))
		{
			// Holding at a door/portal: zero axes with Contact=1 and keep AutoPos flowing.
			// StopMovement() cancels the server CreateMoveToChain that ActOnUse still needs.
			Client->SendMovementEx(0.f, 0.f, 0.f, bRunning, false, true);
			Client->SetForcePositionReporting(true);
		}
		else
		{
			if (bHavePredictedPose)
			{
				Client->SetReportedPosition(PredictedPose);
			}
			// StopCompletely carries the final pose in MoveToState and preserves
			// RawState's hold key. A separate AutoPos duplicates that report.
			Client->SendMovementEx(0.f, 0.f, 0.f, bRunning, false, true);
			Client->SetForcePositionReporting(bWalkingTransitionThisFrame);
		}
		ForwardSent = F;
		RightSent = R;
		TurnSent = ReportedTurn;
		bManualMovementCommandSent = bManualKeys || bMouseTurnSend;
		bRunningSent = bRunning;
		bUniformVRMovementSent = bUniformVRMovement;
		bMouseTurnSent = bMouseTurnSend;
		MouseTurnReportSeconds = 0.f;
		bWasMoving = bMoving;
		bJumpChargeSent = bJumpCharging;
		bJumpAirborneSent = bJumpAirborne;
	}

	// Resolve support, cell transitions and body sweeps together at a bounded
	// physics interval. Subdividing only the lateral sweep still sampled stairs
	// at the far end of a 50-70ms headset frame, lifting the body onto furniture
	// or dropping its floor before it actually reached the doorway.
	const float MovementFrameTime = DeltaTime;
	int32 MovementSteps = bVR && (bMoving || bJumpAirborne)
		? FMath::Clamp(FMath::CeilToInt(MovementFrameTime * 90.f), 1, 24) : 1;
	// CTransition::calc_num_steps limits ordinary travel to one lower-sphere
	// radius per insertion. Support must be sampled at those intermediate poses,
	// not just at two supported endpoints on opposite sides of a cliff/gap.
	if(!bVR && bMoving && !bJumpAirborne)
	{
		const float ForwardSpeed=(F<0.f ? Client->GetLocomotionSpeed(false)
			* (bRunning?Client->GetRunRate():1.f)*.65f : Client->GetLocomotionSpeed(bRunning));
		const FVector Travel(FMath::Abs(R)*Client->GetSidestepSpeed(bRunning)*MovementFrameTime,
			FMath::Abs(F)*ForwardSpeed*WalkingCycleSeconds,0.f);
		const float Distance=(Travel.Size()+WalkingRootDelta.GetTranslation().Size())*GetLocalCreatureScale()*WorldScale;
		MovementSteps=FMath::Max(1,FMath::CeilToInt(Distance/FMath::Max(1.f,GetPlayerCollisionBody().Radii[0])));
	}
	DeltaTime = MovementFrameTime / MovementSteps;
	// Residency can change between substeps, but the world cannot stream new
	// components in the middle of this synchronous movement loop. Reuse the
	// collision filter when the admitted cells remain the same.
	TOptional<FCollisionQueryParams> MovementFilterCache;
	TSet<uint32> MovementFilterCells;
	bool bMovementFilterOutdoor = false;
	bool bMovementFilterIndoor = false;
	for (int32 MovementStep = 0; MovementStep < MovementSteps; ++MovementStep)
	{
	if (bSyncPossessedPawn)
	{
		if (APawn* P = GetPawn())
		{
			// Own a stable predicted pose while moving (and briefly after). Re-seeding every
			// tick from GetPlayerPosition() (server F748) caused stop/stutter rubber-banding.
			FACEPosition Pred;
			if (bHavePredictedPose)
			{
				Pred = PredictedPose;
			}
			else
			{
				Pred = Client->GetPlayerPosition();
				if (!Pred.IsValid())
				{
					return;
				}
			}

			const uint32 SupportedStartCell = Pred.CellId;
			AdvanceServerTurn(Pred, DeltaTime);
			if (bVR && !VR->IsMovementBlocked())
			{
				Pred.SetAceFacingFromUnrealDir2D(VR->GetBodyForward());
				Pred.Location += FACEPosition::AceVectorToUnreal(VRRoomDelta / MovementSteps, 1.f / WorldScale);
				Pred.NormalizeOutdoorLandblock();
			}
			if (!FMath::IsNearlyZero(T) || !FMath::IsNearlyZero(PendingMouseTurnDegrees))
			{
				// Turn about AC +Z. Allowed mid-jump for visuals — arc uses JumpWorldAceVelocity.
				float TurnInput = T;
                const bool bFollowCamera = bMouseFacing && !FMath::IsNearlyZero(PendingMouseTurnDegrees);
                if (bFollowCamera)
                {
                    TurnInput = PendingMouseTurnDegrees / FMath::Max(TurnRateDegPerSec * DeltaTime, KINDA_SMALL_NUMBER);
                    if (auto* Boom = P->FindComponentByClass<USpringArmComponent>())
                    {
                        FRotator Relative = Boom->GetRelativeRotation();
                        // Preserve the world-space view while the character
                        // catches up to a heading chosen with free orbit. Only
                        // consume the angle actually turned this frame.
                        Relative.Yaw = FMath::UnwindDegrees(Relative.Yaw - PendingMouseTurnDegrees);
                        Boom->SetRelativeRotation(Relative);
                    }
                }
				const FQuat Ac(Pred.RotationXYZ.X, Pred.RotationXYZ.Y, Pred.RotationXYZ.Z, Pred.RotationW);
				const FQuat TurnDelta(FVector::UpVector, FMath::DegreesToRadians(-TurnRateDegPerSec * TurnInput * DeltaTime));
				const FQuat NewAc = (TurnDelta * Ac).GetNormalized();
				Pred.RotationW = NewAc.W;
				Pred.RotationXYZ = FVector(NewAc.X, NewAc.Y, NewAc.Z);
				PendingMouseTurnDegrees = 0.f;
			}
			// Face before advancing, then keep steering toward a moving target or
			// back toward it after a collision slide changes our line of approach.
			if (!bVR && bServerMoveToActive && !ApproachTargetDir.IsNearlyZero()
				&& (UseApproachPhase == EACEUseApproachPhase::Turning
					|| UseApproachPhase == EACEUseApproachPhase::Holding
					|| UseApproachPhase == EACEUseApproachPhase::Walking))
			{
				FACEPosition FaceTarget = Pred;
				FaceTarget.SetAceFacingFromUnrealDir2D(ApproachTargetDir);
				const FQuat Blended = ACERetailTurn::Advance(Pred.GetAcQuat(), FaceTarget.GetAcQuat(),
					ServerMoveToSpeed * (bRunning ? 1.5f : 1.f), DeltaTime);
				Pred.RotationW = Blended.W;
				Pred.RotationXYZ = FVector(Blended.X, Blended.Y, Blended.Z);
			}
			if (!bJumpAirborne && (!FMath::IsNearlyZero(F) || !FMath::IsNearlyZero(R)))
			{
				// Retail CPhysicsObj::UpdatePositionInternal scales grounded animation
				// displacement by the creature's size. Airborne velocity is separate.
				const float GroundScale = GetLocalCreatureScale();
				// Backward motion remains WalkForward played negatively, even
				// with Run held (CMotionInterp::apply_run_to_command).
				const float ForwardSpeed = (F<0.f ? Client->GetLocomotionSpeed(false)
					* (bRunning?Client->GetRunRate():1.f) : Client->GetLocomotionSpeed(bRunning)) * GroundScale;
				const float SideSpeed = Client->GetSidestepSpeed(bRunning) * GroundScale;
				// ACE MotionInterp: WalkBackwards → WalkForward with speed *= -0.65.
				const float BackFactor = (F < 0.f) ? 0.65f : 1.f;
				const FVector LocalVelocity = bVR && bServerMoveToActive
					? FVector(R, F, 0.f) * Client->GetLocomotionSpeed(bRunning) * GroundScale
					: bUniformVRMovement
					? ACEVRLocomotion::Velocity(F, R, bRunning, Client->GetRunRate()) * GroundScale
					: FVector(R * SideSpeed, F * BackFactor * ForwardSpeed, 0.f);
				// Entry/exit position frames replace forward cycle displacement;
				// strafe remains an immediate additive modifier in retail.
				Pred.Location += Pred.GetAceForwardInAcSpace() * LocalVelocity.Y * (WalkingCycleSeconds / MovementSteps)
					+ Pred.GetAceRightInAcSpace() * LocalVelocity.X * DeltaTime;
				Pred.NormalizeOutdoorLandblock();
			}
			if (bWalkingTransitionThisFrame && !bJumpAirborne)
			{
				Pred.Location += Pred.GetAcQuat().RotateVector(WalkingRootDelta.GetTranslation()) * GetLocalCreatureScale() / MovementSteps;
				const FQuat Facing = (Pred.GetAcQuat() * WalkingRootDelta.GetRotation()).GetNormalized();
				Pred.RotationW = Facing.W;
				Pred.RotationXYZ = FVector(Facing.X, Facing.Y, Facing.Z);
				Pred.NormalizeOutdoorLandblock();
			}
			// Retail applies authored grounded position frames in addition to both
			// cycle velocity and physical landing coast. Resolve through the same
			// collision path below; never move only the visible character mesh.
			const bool bHadLandingMotion = !bJumpAirborne
				&& (!LandingWorldAceVelocity.IsNearlyZero() || LandingRootTrack.IsActive());
			if (!bJumpAirborne && LandingRootTrack.IsActive())
			{
				const FTransform RootDelta = LandingRootTrack.Advance(DeltaTime);
				const FQuat Facing = Pred.GetAcQuat();
				Pred.Location += Facing.RotateVector(RootDelta.GetTranslation()) * GetLocalCreatureScale();
				const FQuat Rotation = (Facing * RootDelta.GetRotation()).GetNormalized();
				Pred.RotationW = Rotation.W;
				Pred.RotationXYZ = FVector(Rotation.X,Rotation.Y,Rotation.Z);
				Pred.NormalizeOutdoorLandblock();
			}
			// Ballistic arc from leave-ground velocity (world ACE); no mid-air redirect.
			if (bHadLandingMotion)
			{
				FACEWorldObject Self;
				const float Friction = Client->GetWorldObject(Client->GetPlayerGuid(),Self) ? Self.Friction : .95f;
				LandingWorldAceVelocity = ACELandingMotion::Step(LandingWorldAceVelocity,Friction,DeltaTime);
				Pred.Location += LandingWorldAceVelocity * DeltaTime;
				Pred.NormalizeOutdoorLandblock();
				Client->SetForcePositionReporting(!LandingWorldAceVelocity.IsNearlyZero() || LandingRootTrack.IsActive());
			}
			// Always integrate while airborne — zeroing XY at a wall used to make the full
			// vector NearlyZero at apex and stop gravity (hang midair).
			if (bJumpAirborne)
			{
				Pred.Location += JumpWorldAceVelocity * DeltaTime + FVector(0,0,-4.9f * DeltaTime * DeltaTime);
				JumpWorldAceVelocity.Z += -9.8f * DeltaTime;
				Pred.NormalizeOutdoorLandblock();
			}

			if (bLocalPredicting)
			{
				bHavePredictedPose = true;
			}

			// Soft-correct toward the server only while idle. Reconciling XY while predicting
			// pulls AutoPos backward against local run (slow movement + rubber-banding).
			if (bHavePredictedPose)
			{
				PredictedPose = Pred;
				if (bHaveLastServerPose
					&& !bEnterWorldLoading && !bWorldRevealActive
					&& !bJumpAirborne)
				{
					if (bLocalPredicting)
					{
						// Catastrophic desync only (lag spike / admin move).
						const FVector PredUe = PredictedPose.ToUnrealLocation(WorldScale);
						const FVector SrvUe = LastServerPose.ToUnrealLocation(WorldScale);
						if (FVector::Dist2D(PredUe, SrvUe) >= ServerSnapErrorCm)
						{
							SoftReconcilePredictedTowardServer(LastServerPose, DeltaTime, /*bAllowHeading*/ false);
							Pred = PredictedPose;
						}
					}
					else
					{
						SoftReconcilePredictedTowardServer(LastServerPose, DeltaTime, /*bAllowHeading*/ false);
						Pred = PredictedPose;
					}
				}
			}

			FVector Desired = Pred.ToUnrealLocation(WorldScale);
			const FVector BeforeCollision = Desired;
			const FVector Current = P->GetActorLocation();
			const uint32 MovementStartCell = Pred.CellId;
			FName MovementBlocker = NAME_None;
			FVector MovementContactNormal = FVector::ZeroVector;
			float MovementPenetration = 0.f;
			bool bMovementLedge = false;
			TOptional<FCollisionQueryParams> LedgeSupportParams;

			float CapsuleHalfHeight = 88.f;
			float CapsuleRadius = 34.f;
			if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
			{
				CapsuleHalfHeight = Cap->GetScaledCapsuleHalfHeight();
				CapsuleRadius = Cap->GetScaledCapsuleRadius();
			}

			constexpr float FloorZ = 0.66417414618662751f;
			bool bLandedThisStep = false;
			auto FinishJumpLanding = [&]()
			{
				bLandedThisStep = true;
				LandingRootTrack.Reset();
				LandingWorldAceVelocity = FVector(JumpWorldAceVelocity.X,JumpWorldAceVelocity.Y,0.f);
				// Poses received before touchdown describe the old airborne height.
				// Wait for a fresh server anchor rather than reconciling back up.
				bHaveLastServerPose = false;
				bJumpAirborne = false;
				bJumpAirborneSent = false;
				bStandingJumpLocked = false;
				bHaveJumpLaunchFacing = false;
				StandingJumpAimF = 0.f;
				StandingJumpAimR = 0.f;
				JumpLocalAceVelocity = FVector::ZeroVector;
				JumpWorldAceVelocity = FVector::ZeroVector;
				JumpAirborneSeconds = 0.f;
				if (UACECharacterAppearanceComponent* App =
					P->FindComponentByClass<UACECharacterAppearanceComponent>())
				{
					App->ClearJumpMotionIfAny(true, Client->GetSession()->GetCurrentForwardCommand());
					App->BuildLandingRootTrack(LandingRootTrack);
					App->SetSuppressLocoIdleBlend(false);
				}
				if (Client)
				{
				Client->SetForcePositionReporting(!LandingWorldAceVelocity.IsNearlyZero() || LandingRootTrack.IsActive());
					// Send contact only after the resolved feet/cell are committed below.
					// The previous pose can still be above the floor at this point.
					bForceMovementResend = true;
				}
			};


			// Collision + stair climb. Dual thresholds match PhysicsGlobals:
			//   LandingZ (~0.087) — wall vs landable (step-down / jump land)
			//   FloorZ   (~0.664) — ON_WALKABLE / slope cling (≈48.4°)
			if (UWorld* World = GetWorld())
			{
				constexpr float WallSkinCm = 2.f;
				constexpr float LandingZ = 0.0871557f;
				const auto Shape = GetPlayerCollisionBody();
				FCollisionQueryParams Params(SCENE_QUERY_STAT(ACEWallBlock), /*bTraceComplex*/ true, P);
				if (bVR) { TArray<AActor*> AttachedPresentation; P->GetAttachedActors(AttachedPresentation, true, true); Params.AddIgnoredActors(AttachedPresentation); }
				// Channel sweep (ECC_Pawn): ObjectType-only queries miss WorldDynamic meshes and
				// any proxy that isn't ObjectType=WorldStatic. Solid props Block Pawn.
				constexpr ECollisionChannel MoveChannel = ECC_Pawn;

				UACEDatSubsystem* WaterDat = GetGameInstance()
					? GetGameInstance()->GetSubsystem<UACEDatSubsystem>() : nullptr;

				const bool bIndoorPred = bHavePredictedPose
					&& (static_cast<uint32>(PredictedPose.CellId) & 0xFFFFu) >= 0x0100u;
				const float StepUpCmRetail = GetStepUpHeightCm();
				// Do NOT inflate indoor step-up to a full meter — that lets the capsule
				// land on low second-story floors / climb through shop ceilings.
				// Stairs use walkable collision + lateral probes; keep retail StepUpHeight.
				const float StepUpCm = StepUpCmRetail;

				// P0: retail ObjCell::find_cell_list — only these cells participate in sweeps.
				TArray<uint32> TransitCells;
				TSet<uint32> TransitCellSet;
				TSet<uint32> TransitEnvCells;
				bool bTransitHitsInterior = false;
				bool bTransitHasOutdoorLand = !bIndoorPred;
				if (WaterDat)
				{
					const uint32 HintCell = bHavePredictedPose
						? static_cast<uint32>(PredictedPose.CellId)
						: static_cast<uint32>(Pred.CellId);
					// Center + CapsuleRadius missed walls above the waist; jump arcs into
					// neighbor EnvCells that were transit-ignored → pass-through walls.
					const float CapsuleBoundR = ACECellTransit::ComputeCapsuleTransitRadiusCm(
						CapsuleRadius, CapsuleHalfHeight);
					float TransitSphereR = CapsuleBoundR;
					const FVector DestCenterUe(
						Desired.X, Desired.Y, Desired.Z + CapsuleHalfHeight);
					if (bJumpAirborne)
					{
						const float ArcLen = FVector::Dist(
							FVector(Current.X, Current.Y, Current.Z), DestCenterUe);
						TransitSphereR = FMath::Max(
							TransitSphereR, ArcLen + CapsuleBoundR + 16.f);
					}
					else
					{
						TransitSphereR = FMath::Max(
							TransitSphereR,
							FVector::Dist2D(Current, DestCenterUe) + CapsuleBoundR);
					}
					auto AbsorbTransitAt = [&](const FVector& Center)
					{
						TArray<uint32> Local;
						bool bLocalInterior = false;
						ACECellTransit::FindCellList(
							*WaterDat, HintCell, Center, TransitSphereR, WorldScale, Local, &bLocalInterior);
						if (bLocalInterior)
						{
							bTransitHitsInterior = true;
						}
						for (uint32 CellId : Local)
						{
							if (TransitCellSet.Contains(CellId))
							{
								continue;
							}
							TransitCellSet.Add(CellId);
							TransitCells.Add(CellId);
						}
					};
					AbsorbTransitAt(FVector(Current.X, Current.Y, Current.Z));
					// Include the destination even while outdoor-resident. A walking
					// capsule can cross a doorway before its current cell changes.
					if (bJumpAirborne || !Current.Equals(DestCenterUe, .01f))
					{
						AbsorbTransitAt(DestCenterUe);
						if (bJumpAirborne)
						{
							AbsorbTransitAt(
								(FVector(Current.X, Current.Y, Current.Z) + DestCenterUe) * 0.5f);
							if (JumpWorldAceVelocity.Z > 0.f)
							{
								const float ApexZ = FMath::Max(Current.Z, DestCenterUe.Z)
									+ JumpWorldAceVelocity.Z * WorldScale * DeltaTime;
								AbsorbTransitAt(FVector(DestCenterUe.X, DestCenterUe.Y, ApexZ));
							}
						}
					}
					bTransitHasOutdoorLand = false;
					for (uint32 CellId : TransitCells)
					{
						if (ACECellTransit::IsIndoorCell(CellId))
						{
							TransitEnvCells.Add(CellId);
						}
						else
						{
							bTransitHasOutdoorLand = true;
						}
					}
					// Retail CObjCell::find_cell_list: an outdoor seed *always* runs
					// add_all_outside_cells — land cells are unconditionally in the
					// collision set while outdoor-resident. Never let a transit miss
					// leave an outdoor player with zero land collision.
					if (!bIndoorPred)
					{
						bTransitHasOutdoorLand = true;
					}
				}

				const bool bUseLandSupport = bTransitHasOutdoorLand && (!bIndoorPred || (WaterDat
					&& ACECellTransit::UsesOutdoorGround(*WaterDat, PredictedPose.CellId, Desired, WorldScale)));

				auto IsOutdoorTerrainHit = [](const FHitResult& Hit) -> bool
				{
					return Hit.Component.IsValid()
						&& Hit.Component->ComponentTags.Contains(FName(TEXT("ACEOutdoorTerrain")));
				};
				auto IsBuildingShellHit = [](const FHitResult& Hit) -> bool
				{
					return Hit.Component.IsValid()
						&& Hit.Component->ComponentTags.Contains(FName(TEXT("ACEBuildingShell")));
				};

				// Retail: land terrain always collides while the object is outdoor-resident
				// (CLandCell::find_env_collisions runs unconditionally; hits_interior_cell
				// only relaxes the *building shell* solid test, never land). Land drops out
				// of the set solely via residency: once the point is inside an EnvCell's
				// cell BSP, the seed is indoor and land is omitted unless an outside portal
				// straddle re-adds it.
				auto IsBlockingOutdoorTerrain = [&](const FHitResult& Hit, double X, double Y) -> bool
				{
					return IsOutdoorTerrainHit(Hit) && bUseLandSupport;
				};

				// Retail OBJECTINFO::validate_walkable: contact embeds waterDepth along the plane
				// (zDist = dist / N.z). Flat land → SurfaceZ - depth; slopes scale by 1/Nz.
				auto ApplyRetailWaterDepth = [WaterDat, this](double X, double Y, float SurfaceZ, float NormalZ) -> float
				{
					if (!WaterDat)
					{
						return SurfaceZ;
					}
					const float DepthCm = WaterDat->GetOutdoorWaterDepthCm(X, Y, WorldScale);
					if (DepthCm <= KINDA_SMALL_NUMBER)
					{
						return SurfaceZ;
					}
					const float Nz = FMath::Max(0.15f, NormalZ);
					return SurfaceZ - DepthCm / Nz;
				};

				// Cell-partitioned sweeps: ignore outdoor land when not in transit; ignore
				// EnvCells outside the transit set (StabList peek stay draw-only).
				FCollisionQueryParams SweepParams = Params;
				const bool bReuseMovementFilter = bVR && MovementFilterCache.IsSet()
					&& bMovementFilterIndoor == bIndoorPred
					&& bMovementFilterOutdoor == bTransitHasOutdoorLand
					&& MovementFilterCells.Num() == TransitEnvCells.Num()
					&& MovementFilterCells.Includes(TransitEnvCells);
				if (bReuseMovementFilter) SweepParams = MovementFilterCache.GetValue();
				if (!bReuseMovementFilter && !bTransitHasOutdoorLand)
				{
					for (TActorIterator<AACELandblockActor> It(World); It; ++It)
					{
						if (It->TerrainMesh)
						{
							SweepParams.AddIgnoredComponent(
								static_cast<const UPrimitiveComponent*>(It->TerrainMesh.Get()));
						}
					}
					for (TActorIterator<AACETerrainChunkActor> It(World); It; ++It)
					{
						if (It->TerrainMesh)
						{
							SweepParams.AddIgnoredComponent(
								static_cast<const UPrimitiveComponent*>(It->TerrainMesh.Get()));
						}
					}
				}
				if (!bReuseMovementFilter && bIndoorPred && (TransitEnvCells.Num() > 0 || WaterDat))
				{
					ACECellTransit::RestrictRoomCollision(*World, TransitEnvCells, SweepParams);
				}
				if (bVR && !bReuseMovementFilter)
				{
					MovementFilterCache = SweepParams;
					MovementFilterCells = TransitEnvCells;
					bMovementFilterOutdoor = bTransitHasOutdoorLand;
					bMovementFilterIndoor = bIndoorPred;
				}
				if (bUseLandSupport)
					FilterWadingTerrain(*World,WaterDat,P->GetActorLocation(),Desired,WorldScale,SweepParams);

				// OBJECTINFO::get_walkable_z / CTransition::step_up use the
				// walking threshold for a grounded body. LandingZ alone allowed
				// walking through the steep sloped side of a statue pedestal.
				const float BlockingNormalZ = bJumpAirborne ? LandingZ : FloorZ;
				auto IsWallHit = [BlockingNormalZ](const FHitResult& Hit) -> bool
				{
					// An upward contact against the upper body is a roof/ledge,
					// never foot support. Treating it as a floor let steps enter walls.
					if (Hit.bBlockingHit && ACEBodySweep::IsUpperBodyContact(Hit)) return true;
					if (Hit.bBlockingHit && ACEBodySweep::IsCreatureBody(Hit)) return true;
					if (!Hit.bBlockingHit || Hit.Normal.Z >= BlockingNormalZ)
					{
						return false;
					}
					// Steep heightfield faces are walls (retail slide along precipices).
					// Treating land as never-a-wall let the capsule jitter on the lip.
					return true;
				};

				auto SweepCapsule = [&](FHitResult& OutHit, const FVector& From, const FVector& To) -> bool
				{
					const bool Hit = ACEBodySweep::Sweep(*World, OutHit, From, To, Shape, SweepParams);
					if (Hit && IsWallHit(OutHit))
					{
						MovementBlocker = OutHit.GetComponent() ? OutHit.GetComponent()->GetFName() : NAME_None;
						MovementContactNormal = OutHit.Normal; MovementPenetration = OutHit.PenetrationDepth;
					}
					return Hit;
				};
				auto RecoverPenetration = [&](const FVector& From, const FVector& Push, const FHitResult& Original)
				{
					return ACEBodySweep::Recover(*World, From, Push, Original, Shape, SweepParams);
				};

				if (bServerMoveToActive && ServerMoveToTargetGuid != 0)
				{
					for (TActorIterator<AACEWorldEntityActor> It(World); It; ++It)
					{
						if (It->GetACEGuid() == ServerMoveToTargetGuid)
						{
							Params.AddIgnoredActor(*It);
							break;
						}
					}
					// Do NOT ignore landblock terrain here — SampleFeetZ needs outdoor
					// walkables or precipice blocking freezes Use-approach on grade.
				}

				const float FeetNow = Current.Z - CapsuleHalfHeight;
				const float MoveDist2D = FVector::Dist2D(Current, Desired);
				// Preserve distance at low frame rates. Subdivide collision below;
				// capping distance here made run speed proportional to frame rate.
				const float MaxHorizStep = FMath::Max(22.f, CapsuleRadius * 0.65f);
				const bool bIndoorNow = (static_cast<uint32>(Pred.CellId) & 0xFFFFu) >= 0x0100u;
				const float RampClimbCm = bIndoorNow
					? (StepUpCm + 12.f)
					: (StepUpCm + 28.f);
				// Retail CheckWalkable / Setup StepDownHeight. Add waterDepth only when wet.
				const float StepDownCm = GetStepDownHeightCm();
				const float WaterDepthDestCm = WaterDat
					? WaterDat->GetOutdoorWaterDepthCm(Desired.X, Desired.Y, WorldScale) : 0.f;
				const float RampDropCm = StepDownCm + WaterDepthDestCm;
				if (!bJumpAirborne && HasLedgeSupport(*World, WaterDat,
					Current-FVector(0,0,CapsuleHalfHeight), CapsuleRadius,
					StepDownCm, WorldScale, bUseLandSupport, SweepParams,Shape.Count))
					LedgeSupportParams = SweepParams;

				// Desktop's step grace can hold feet on a nonexistent floor for
				// almost a second after sliding off furniture. Tracked locomotion
				// must use the actual lower-sphere support at the resolved position.
				const float StepHoldDuration = bVR ? 0.f : bIndoorNow ? 0.85f : 0.35f;

				// Retail CLandCell::find_env_collisions: Collided only when the *landblock*
				// is EntirelyWater (get_block_water_type). Coastal cells that are fully
				// flooded on a PartiallyWater block are walkable at 0.9 AC depth.
				const bool bDestEntirelyWater = bUseLandSupport && WaterDat
					&& WaterDat->GetOutdoorBlockWaterType(Desired.X, Desired.Y, WorldScale) == 2;

				auto CollectWalkableFeet = [&](double X, double Y, float NearFeetZ, float MaxUp, float MaxDown,
					float& BestIndoorZ, bool& bHaveIndoorHit, float& BestOutdoorZ, bool& bHaveOutdoorHit)
				{
					const float TraceTop = NearFeetZ + MaxUp + .5f;
					const FVector TraceStart(X, Y, TraceTop);
					const FVector TraceEnd(X, Y, NearFeetZ - MaxDown - 20.f);
					TArray<FHitResult> Hits;
					if (!ACEBodySweep::TraceGround(*World, Hits, TraceStart, TraceEnd, Params))
					{
						return;
					}
					for (const FHitResult& Hit : Hits)
					{
						// Accept landable slopes (retail LandingZ). FloorZ gates ON_WALKABLE cling.
						const float MinWalkZ = bJumpAirborne ? LandingZ : FloorZ;
						if (!Hit.bBlockingHit || ACEBodySweep::IsCreatureBody(Hit) || Hit.ImpactNormal.Z < MinWalkZ)
						{
							continue;
						}
						if (Hit.ImpactPoint.Z > NearFeetZ + MaxUp)
						{
							continue;
						}
						if (Hit.ImpactPoint.Z < NearFeetZ - MaxDown)
						{
							continue;
						}
						if (IsBlockingOutdoorTerrain(Hit, X, Y))
						{
							const float WalkZ = ApplyRetailWaterDepth(
								X, Y, Hit.ImpactPoint.Z, Hit.ImpactNormal.Z);
							if (!bHaveOutdoorHit
								|| FMath::Abs(WalkZ - NearFeetZ) < FMath::Abs(BestOutdoorZ - NearFeetZ))
							{
								BestOutdoorZ = WalkZ;
								bHaveOutdoorHit = true;
							}
						}
						else if (!IsOutdoorTerrainHit(Hit))
						{
							// ACEBuildingShell is walkable (Setup stairs/stoops). Skipping it
							// left only the heightfield in front of the riser — TryStepUp died.
							if (!IsBuildingShellHit(Hit))
							{
								// Stab scenery (tent roofs, awnings) only within StepUp —
								// MaxUp/RampClimbCm is for outdoor grade + indoor stairs.
								const float SceneryMaxUp = bIndoorNow ? MaxUp : (StepUpCm + 12.f);
								if (Hit.ImpactPoint.Z > NearFeetZ + SceneryMaxUp)
								{
									continue;
								}
							}
							// Prefer the walkable nearest the feet — max-Z snapped to loft
							// ceilings / second stories and shoved the capsule upward.
							if (!bHaveIndoorHit
								|| FMath::Abs(Hit.ImpactPoint.Z - NearFeetZ)
									< FMath::Abs(BestIndoorZ - NearFeetZ))
							{
								BestIndoorZ = Hit.ImpactPoint.Z;
								bHaveIndoorHit = true;
							}
						}
					}
				};

				auto SampleFeetZ = [&](double X, double Y, float NearFeetZ, float MaxUp, float MaxDown, float& OutFeetZ,
					bool bUseRing = true) -> bool
				{
					float BestIndoorZ = -TNumericLimits<float>::Max();
					float BestOutdoorZ = -TNumericLimits<float>::Max();
					bool bHaveIndoorHit = false;
					bool bHaveOutdoorHit = false;
					CollectWalkableFeet(X, Y, NearFeetZ, MaxUp, MaxDown,
						BestIndoorZ, bHaveIndoorHit, BestOutdoorZ, bHaveOutdoorHit);

					const bool bNeedRing = bIndoorNow
						? !bHaveIndoorHit
						: (!bHaveIndoorHit && (!bHaveOutdoorHit
							|| BestOutdoorZ < NearFeetZ + 2.f));
					if (bUseRing && bNeedRing)
					{
						const float Ring = FMath::Max(6.f, CapsuleRadius * 0.45f);
						const FVector2D Offs[] = {
							FVector2D(Ring, 0.f), FVector2D(-Ring, 0.f),
							FVector2D(0.f, Ring), FVector2D(0.f, -Ring),
							FVector2D(Ring * 0.7f, Ring * 0.7f), FVector2D(-Ring * 0.7f, Ring * 0.7f),
							FVector2D(Ring * 0.7f, -Ring * 0.7f), FVector2D(-Ring * 0.7f, -Ring * 0.7f)
						};
						for (const FVector2D& O : Offs)
						{
							CollectWalkableFeet(X + O.X, Y + O.Y, NearFeetZ, MaxUp, MaxDown,
								BestIndoorZ, bHaveIndoorHit, BestOutdoorZ, bHaveOutdoorHit);
						}
					}

					// Outdoor DAT land+water — retail keeps the heightfield as real support
				// wherever land is in the transit set, including terrain exposed by
				// an indoor cell's horizontal outside portal beneath a walkway.
					// Raised walkables (stoops/stairs) still win below via the step preference.
					if (bUseLandSupport && WaterDat)
					{
						float DatFeetZ = NearFeetZ;
						FVector DatNormal;
						if (WaterDat->SampleOutdoorGroundZ(X, Y, WorldScale, DatFeetZ, &DatNormal)
							&& DatNormal.Z >= FloorZ
							&& DatFeetZ <= NearFeetZ + MaxUp
							&& DatFeetZ >= NearFeetZ - MaxDown)
						{
							if (!bHaveOutdoorHit
								|| FMath::Abs(DatFeetZ - NearFeetZ) < FMath::Abs(BestOutdoorZ - NearFeetZ))
							{
								BestOutdoorZ = DatFeetZ;
								bHaveOutdoorHit = true;
							}
						}
					}

					if (bIndoorNow && !bUseLandSupport)
					{
						if (bHaveIndoorHit)
						{
							OutFeetZ = BestIndoorZ;
							return true;
						}
						return false;
					}
					// Outdoors: raised Stab scenery (stoops/chests/stairs) only within StepUp —
					// RampClimbCm (≤160) was treating low tent roofs as floors and snapping
					// the capsule onto the canvas when walking through the doorway.
					const float RaisedMaxUp = StepUpCm + 12.f;
					if (bHaveIndoorHit
						&& BestIndoorZ <= NearFeetZ + RaisedMaxUp
						&& BestIndoorZ >= NearFeetZ - MaxDown
						&& (!bHaveOutdoorHit || BestIndoorZ >= BestOutdoorZ + 1.5f))
					{
						OutFeetZ = BestIndoorZ;
						return true;
					}
					if (bHaveOutdoorHit)
					{
						OutFeetZ = BestOutdoorZ;
						return true;
					}
					// Outdoors: never fall back to high Stab hits (tent roofs). Same-level
					// scenery only — raised walkables already handled above via RaisedMaxUp.
					if (bHaveIndoorHit
						&& (bIndoorNow || BestIndoorZ <= NearFeetZ + 8.f))
					{
						OutFeetZ = BestIndoorZ;
						return true;
					}
					return false;
				};

				// Use one support predicate for the destination and ledge tangent.
				// Offset rays alone can see floor behind an unsupported foot; the
				// lower sphere validates edge contact without blocking narrow treads.
				auto SampleWalkingFeet = [&](double X, double Y, float& OutFeetZ)
				{
					const float WalkableDown=ACEBodySweep::StepDownProbeDistance(Shape.Radii[0],Shape.Count,RampDropCm);
					bool bFound = SampleFeetZ(X,Y,FeetNow,RampClimbCm,WalkableDown,OutFeetZ,false);
					float SphereZ = FeetNow;
					if (ACEBodySweep::FindBodyFootSupport(*World,FVector(X,Y,FeetNow),Shape,
						RampDropCm,SweepParams,SphereZ,RampClimbCm,bFound && OutFeetZ<=FeetNow,true)
						&& (!bFound || SphereZ>OutFeetZ))
					{
						OutFeetZ=SphereZ;
						bFound=true;
					}
					return bFound;
				};

				auto TryStepUp = [&](const FVector& From, const FVector& DesiredEnd, FVector& OutLanded) -> bool
				{
					// Retail Transition.StepUp only while OnWalkable — never mid-jump.
					if (bJumpAirborne || StepUpCm < 1.f)
					{
						return false;
					}
					FVector MoveDir = DesiredEnd - From;
					MoveDir.Z = 0.f;
					if (MoveDir.SizeSquared() < KINDA_SMALL_NUMBER)
					{
						MoveDir = Pred.GetAceForwardVector().GetSafeNormal2D();
						if (MoveDir.IsNearlyZero())
						{
							return false;
						}
					}
					else
					{
						MoveDir.Normalize();
					}

					// CTransition::step_up uses StepUpHeight for step_down, but
					// CPolygon::adjust_sphere_to_plane accepts walk_interp down to
					// -0.5: the landing can rise 1.5 times that height. The previous
					// fixed +12 cm cap rejected retail's ordinary tabletop step.
					// Keep this allowance inside the collision-validated step path,
					// not the general floor snap (which could pull bodies onto roofs).
					const float MaxStepRise = StepUpCm * 1.5f;
					// Short reaches first — pool rims / curb lips are narrower than the capsule.
					// Long hops land the center in the water void and SampleFeetZ returns grade (Rise≈0).
					const float ReachCandidates[] = {
						CapsuleRadius * 0.22f,
						CapsuleRadius * 0.38f,
						CapsuleRadius * 0.55f,
						CapsuleRadius * 0.75f,
						CapsuleRadius * 1.05f
					};
					const float LiftCandidates[] = {
						12.f,
						FMath::Min(36.f, StepUpCm * 0.35f),
						StepUpCm * 0.50f,
						StepUpCm * 0.70f,
						StepUpCm,
						FMath::Min(StepUpCm, CapsuleRadius * 1.1f),
						MaxStepRise
					};
                    struct FLiftProbe { FVector Position; bool bBlocked=false; };
                    TArray<FLiftProbe,TInlineAllocator<7>> LiftProbes;
                    for (const float LiftCm : LiftCandidates)
                    {
                        auto& Probe=LiftProbes.AddDefaulted_GetRef();
                        Probe.Position=From+FVector(0,0,LiftCm);
                        if (LiftCm<3.f) { Probe.bBlocked=true; continue; }
                        FHitResult UpHit;
                        bool bLiftHit=SweepCapsule(UpHit,From,Probe.Position);
                        // Retail BSP step-up adjusts the lower sphere onto a
                        // walkable plane; it does not sweep that sphere through
                        // the riser. A table's apron must not become a ceiling
                        // for the foot. Preserve the upper sphere's clearance,
                        // then validate the full body across and onto the tread.
                        if(bLiftHit && Shape.Count>1 && !ACEBodySweep::IsUpperBodyContact(UpHit)
                            && !ACEBodySweep::IsCreatureBody(UpHit)
                            && UpHit.ImpactPoint.Z<=From.Z-CapsuleHalfHeight+MaxStepRise)
                        {
                            FACECollisionBody Upper=Shape;
                            Upper.Centers[0]=Shape.Centers[1];Upper.Radii[0]=Shape.Radii[1];Upper.Count=1;
                            bLiftHit=ACEBodySweep::Sweep(*World,UpHit,From,Probe.Position,Upper,SweepParams);
                            if(bLiftHit)UpHit.MyBoneName=FName(TEXT("ACEUpperBodySphere"));
                            if(bLiftHit && UpHit.bStartPenetrating){Probe.bBlocked=true;continue;}
                        }
                        if (bLiftHit && IsWallHit(UpHit) && !UpHit.bStartPenetrating)
                        {
                            if (UpHit.ImpactNormal.Z<-.15f)
                            {
                                Probe.Position.Z=UpHit.Location.Z-2.f;
                                Probe.bBlocked=Probe.Position.Z<=From.Z+3.f;
                            }
                            else Probe.bBlocked=true;
                        }
                    }
					const float FeetAtFrom = From.Z - CapsuleHalfHeight;

					// Indoor L/quarter-turn stairs: next tread is offset sideways — forward-only
					// reach lands past the pie and misses. Probe ±45°/±90° with short reaches.
					TArray<FVector, TInlineAllocator<5>> ReachDirs;
					ReachDirs.Add(MoveDir);
					if (bIndoorNow)
					{
						const FVector Right(-MoveDir.Y, MoveDir.X, 0.f);
						ReachDirs.Add((MoveDir + Right * 0.7f).GetSafeNormal2D());
						ReachDirs.Add((MoveDir - Right * 0.7f).GetSafeNormal2D());
					}

					for (const FVector& Dir : ReachDirs)
					{
					for (const float ReachCm : ReachCandidates)
					{
						if (ReachCm < 4.f)
						{
							continue;
						}
						// Lateral dirs: keep reaches short so we don't hop into walls.
						const bool bLateral = FVector::DotProduct(Dir, MoveDir) < 0.95f;
						if (bLateral && ReachCm > CapsuleRadius * 0.55f)
						{
							continue;
						}
                        // Every reach/direction uses the same vertical path. Resolve it
                        // once per height instead of repeating it up to eleven times.
                        for (const auto& Probe : LiftProbes)
                        {
                            if (Probe.bBlocked) continue;
                            const FVector Lifted=Probe.Position;
							const FVector LiftedEnd = Lifted + Dir * ReachCm;
							FHitResult HorizHit;
							FVector AfterHoriz = LiftedEnd;
							if (SweepCapsule(HorizHit, Lifted, LiftedEnd) && IsWallHit(HorizHit))
							{
								if (HorizHit.bStartPenetrating)
								{
									continue;
								}
								const float Progress = FVector::Dist2D(Lifted, HorizHit.Location);
								if (Progress < 2.f)
								{
									continue;
								}
								AfterHoriz = FVector(HorizHit.Location.X, HorizHit.Location.Y, Lifted.Z)
									+ HorizHit.ImpactNormal.GetSafeNormal2D() * WallSkinCm;
							}

							float LandFeetZ = 0.f;
							bool bHaveLand = false;
							const float LandMaxUp = MaxStepRise;
							const float LandMaxDown = CapsuleHalfHeight + StepUpCm + 16.f;
							auto TryLandAt = [&](double X, double Y) -> bool
							{
								float Z = 0.f;
								if (!bIndoorNow)
								{
									// SampleFeetZ prefers land (nearest to feet) at the XY in
									// front of a riser. For outdoor Setup stairs, take the
									// highest building-shell tread in StepUp — same as indoor.
									const float TreadMaxUp = MaxStepRise;
									const float TraceTop = FeetAtFrom + LandMaxUp + .5f;
									TArray<FHitResult> StairHits;
									if (World->LineTraceMultiByChannel(
										StairHits, FVector(X, Y, TraceTop),
										FVector(X, Y, FeetAtFrom - LandMaxDown - 20.f),
										MoveChannel, SweepParams))
									{
										float BestZ = -TNumericLimits<float>::Max();
										bool bFound = false;
										for (const FHitResult& Hit : StairHits)
										{
											if (!Hit.bBlockingHit || Hit.ImpactNormal.Z < FloorZ)
											{
												continue;
											}
											if (Hit.ImpactPoint.Z > FeetAtFrom + TreadMaxUp
												|| Hit.ImpactPoint.Z < FeetAtFrom - LandMaxDown)
											{
												continue;
											}
											if (!IsBuildingShellHit(Hit))
											{
												continue;
											}
											if (Hit.ImpactPoint.Z > BestZ)
											{
												BestZ = Hit.ImpactPoint.Z;
												bFound = true;
											}
										}
										if (bFound)
										{
											LandFeetZ = BestZ;
											return true;
										}
									}
								}
								if (bIndoorNow)
								{
									// Nearest-to-feet SampleFeetZ prefers the floor in front of
									// a riser. For step-up, take the highest walkable in range
									// (the tread) instead of the floor.
									const float TraceTop = FeetAtFrom + LandMaxUp + .5f;
									TArray<FHitResult> Hits;
									if (World->LineTraceMultiByChannel(
										Hits, FVector(X, Y, TraceTop),
										FVector(X, Y, FeetAtFrom - LandMaxDown - 20.f),
										MoveChannel, Params))
									{
										float BestZ = -TNumericLimits<float>::Max();
										bool bFound = false;
										for (const FHitResult& Hit : Hits)
										{
											if (!Hit.bBlockingHit || Hit.ImpactNormal.Z < FloorZ)
											{
												continue;
											}
											if (Hit.ImpactPoint.Z > FeetAtFrom + LandMaxUp
												|| Hit.ImpactPoint.Z < FeetAtFrom - LandMaxDown)
											{
												continue;
											}
											if (IsOutdoorTerrainHit(Hit))
											{
												continue;
											}
											if (Hit.ImpactPoint.Z > BestZ)
											{
												BestZ = Hit.ImpactPoint.Z;
												bFound = true;
											}
										}
										if (bFound)
										{
											LandFeetZ = BestZ;
											return true;
										}
									}
								}
								if (SampleFeetZ(X, Y, FeetAtFrom, LandMaxUp, LandMaxDown, Z))
								{
									LandFeetZ = Z;
									return true;
								}
								const FVector DownEnd(X, Y, FeetAtFrom - 8.f);
								FHitResult DownHit;
								if (SweepCapsule(DownHit, FVector(X, Y, AfterHoriz.Z), DownEnd) && DownHit.bBlockingHit
									&& DownHit.ImpactNormal.Z >= FloorZ)
								{
									if (!bIndoorNow || !IsOutdoorTerrainHit(DownHit))
									{
										LandFeetZ = DownHit.ImpactPoint.Z;
										return true;
									}
								}
								return false;
							};
							if (TryLandAt(AfterHoriz.X, AfterHoriz.Y))
							{
								bHaveLand = true;
							}
							else
							{
								// Indoor L/turns and outdoor Setup stairs: next tread can sit
								// beside the capsule center (town-hall / cellar).
								const float R = CapsuleRadius * 0.55f;
								const float Offsets[][2] = {
									{ R, 0.f }, { -R, 0.f }, { 0.f, R }, { 0.f, -R },
									{ R * 0.7f, R * 0.7f }, { -R * 0.7f, R * 0.7f },
									{ R * 0.7f, -R * 0.7f }, { -R * 0.7f, -R * 0.7f }
								};
								for (const auto& O : Offsets)
								{
									if (TryLandAt(AfterHoriz.X + O[0], AfterHoriz.Y + O[1]))
									{
										AfterHoriz.X += O[0];
										AfterHoriz.Y += O[1];
										bHaveLand = true;
										break;
									}
								}
							}
							// CPolygon::check_walkable admits an edge within the lower
							// sphere's footprint. A center ray can still see the floor
							// beside a thin tabletop and miss that valid raised tread.
							float FootSupportZ=FeetAtFrom;
							if(ACEBodySweep::FindBodyFootSupport(*World,FVector(AfterHoriz.X,AfterHoriz.Y,FeetAtFrom),
								Shape,LandMaxDown,SweepParams,FootSupportZ,MaxStepRise,false,true)
								&& (!bHaveLand || FootSupportZ>LandFeetZ))
							{
								LandFeetZ=FootSupportZ;bHaveLand=true;
							}
							if (!bHaveLand)
							{
								continue;
							}
							// Must actually climb — landing back on grade through a hollow (pool)
							// is not a step-up.
							const float RiseFeet = LandFeetZ - FeetAtFrom;
							if (RiseFeet < 0.5f || RiseFeet > MaxStepRise)
							{
								continue;
							}
							const float LandCenterZ = LandFeetZ + CapsuleHalfHeight;
							FVector Landed(AfterHoriz.X, AfterHoriz.Y, LandCenterZ);
                            // Side probes may find the tread behind us when
                            // leaving a bench. A step must advance in the input
                            // direction, never pull the player back onto it.
                            if (FVector::DotProduct((Landed-From).GetSafeNormal2D(),MoveDir)<=0.f) continue;
							if (!ACEBodySweep::CanTraverseStep(*World, Lifted, AfterHoriz, Landed,
								Shape, SweepParams, FloorZ, &Landed))
							{
								continue;
							}
							if (Landed.Z-From.Z>MaxStepRise) continue;
							// Block second-floor / low-ceiling clips: only reject true overhead
							// ceilings near the head. Do not reject stair climbs when a distant
							// ceiling is in the sweep (previous check killed all indoor steps).
							{
								FHitResult CeilHit;
								const FVector HeadEnd = Landed + FVector(0.f, 0.f, CapsuleHalfHeight + 6.f);
								if (SweepCapsule(CeilHit, Landed, HeadEnd) && CeilHit.bBlockingHit
									&& !CeilHit.bStartPenetrating
									&& CeilHit.ImpactNormal.Z < -0.55f)
								{
									const float HeadClear = CeilHit.ImpactPoint.Z - LandFeetZ;
									if (HeadClear < CapsuleHalfHeight * 2.f + 4.f)
									{
										continue;
									}
								}
								if (SweepCapsule(CeilHit, Landed, Landed + FVector(0.f, 0.f, 1.f))
									&& CeilHit.bStartPenetrating
									&& CeilHit.ImpactNormal.Z < -0.55f)
								{
									continue;
								}
							}
							OutLanded = Landed;
							return true;
						}
					}
					} // ReachDirs
					return false;
				};

				float DestFeetZ = FeetNow;
				bool bHaveRampGround = bJumpAirborne
					? SampleFeetZ(Desired.X,Desired.Y,FeetNow,RampClimbCm,RampDropCm,DestFeetZ)
					: SampleWalkingFeet(Desired.X,Desired.Y,DestFeetZ);
				FVector Start = Current;
				// ACE Position / Pred.Location is the cylinder bottom (feet). The pawn actor
				// is at the capsule center — always add CapsuleHalfHeight when placing End.Z.
				const float DesiredFeetZ = Desired.Z;
				const float DesiredCenterZ = DesiredFeetZ + CapsuleHalfHeight;
				// Retail EdgeSlide / precipice: if destination has no support within StepDown,
				// try sliding along the ledge tangent instead of hard XY freeze.
				// All grounded input paths share this check, including Use-approach
				// and sub-centimeter room-scale corrections.
				// Preserve FVector precision through support queries and sweeps;
				// float world XY rounds away small movement at distant landblocks.
				double EndX = Desired.X;
				double EndY = Desired.Y;
				// Retail CLandCell::find_env_collisions → Collided on EntirelyWater *landblocks*.
				if (bDestEntirelyWater)
				{
					EndX = Current.X;
					EndY = Current.Y;
					bHaveRampGround = SampleFeetZ(
						EndX, EndY, FeetNow, RampClimbCm, RampDropCm, DestFeetZ);
				}
				else if (!bJumpAirborne && !bHaveRampGround && MoveDist2D > UE_SMALL_NUMBER)
				{
					bMovementLedge = true;
                    const FVector2D Slide = ACELedgeSlide::Resolve(FVector2D(Current.X,Current.Y),
                        FVector2D(Desired.X-Current.X,Desired.Y-Current.Y), [&](const FVector2D& At)
                        {
                            float Z=FeetNow;
                            return SampleWalkingFeet(At.X,At.Y,Z);
                        }, true);
                    EndX=Slide.X; EndY=Slide.Y;
                    bHaveRampGround=SampleWalkingFeet(EndX,EndY,DestFeetZ);
				}
				FVector End(EndX, EndY,
					bJumpAirborne
						? DesiredCenterZ
						: (bHaveRampGround ? (DestFeetZ + CapsuleHalfHeight) : Current.Z));

				// Climbing onto higher indoor tread — refresh step-hold so plank gaps don't drop us.
				if (!bJumpAirborne && bHaveRampGround && bIndoorNow && DestFeetZ > FeetNow + 1.f)
				{
					StepHoldMinFeetZ = DestFeetZ;
					StepHoldSeconds = StepHoldDuration;
				}



				if (!bJumpAirborne)
				{
					FHitResult PenHit;
					const bool bStartHit = SweepCapsule(PenHit, Start, Start + FVector(0.f, 0.f, 0.1f));
					// A server/streaming correction can start inside terrain, including
					// a walkable floor. Do not roll a successful recovery back into it.
					if (bStartHit && PenHit.bStartPenetrating && PenHit.PenetrationDepth > .5f
						&& !ACEBodySweep::IsCreatureBody(PenHit)) LedgeSupportParams.Reset();
					if (bStartHit && PenHit.bStartPenetrating && IsWallHit(PenHit))
					{
						if (bVR)
						{
							static double LastVRPenetrationLog = 0;
							if (FPlatformTime::Seconds() - LastVRPenetrationLog > 1.)
							{
								LastVRPenetrationLog = FPlatformTime::Seconds();
								UE_LOG(LogTemp, Log, TEXT("ACE VR penetration: actor=%s component=%s cell=%08X normal=%s depth=%.2f feet=%.2f"),
									*GetNameSafe(PenHit.GetActor()), *GetNameSafe(PenHit.GetComponent()), Pred.CellId,
									*PenHit.ImpactNormal.ToString(), PenHit.PenetrationDepth, Start.Z - CapsuleHalfHeight);
							}
						}
						// Ceiling / soffit penetration: never TryStepUp — that shoves the
						// capsule through the floor above. Push along the 3D normal instead.
						if (PenHit.ImpactNormal.Z < -0.15f && !ACEBodySweep::IsCreatureBody(PenHit))
						{
							FVector Push = PenHit.ImpactNormal.GetSafeNormal();
							if (Push.IsNearlyZero())
							{
								Push = FVector(0.f, 0.f, -1.f);
							}
							Start = RecoverPenetration(Start, Push * (PenHit.PenetrationDepth + WallSkinCm), PenHit);
							// Drop a bogus "raised walkable" that was the soffit/roof above us.
							if (bHaveRampGround && DestFeetZ > FeetNow + StepUpCm + 12.f)
							{
								bHaveRampGround = false;
								End.Z = Start.Z;
							}
							else if (!bHaveRampGround)
							{
								End.Z = Start.Z;
							}
						}
						else
						{
							FVector Stepped;
							if (!ACEBodySweep::IsCreatureBody(PenHit) && TryStepUp(Start, End, Stepped))
							{
								Start = Stepped;
								End = FVector(EndX, EndY, Stepped.Z);
								StepHoldMinFeetZ = Stepped.Z - CapsuleHalfHeight;
								StepHoldSeconds = StepHoldDuration;
							}
							else
							{
								Start = RecoverPenetration(Start, PenHit.Normal.GetSafeNormal2D()
									* (PenHit.PenetrationDepth + WallSkinCm), PenHit);
								if (!bHaveRampGround)
								{
									End.Z = Start.Z;
								}
							}
						}
					}
				}

				FVector BounceNormalUe = FVector::ZeroVector;
				bool bHaveBounceNormal = false;
				if (!bJumpAirborne) End.Z=FMath::Max(End.Z,Start.Z);
				// A turn-only step can skip every lateral sweep. Retain the
				// clamped start height, not the pre-clamp terrain point, until the
				// body-aware ground snap below resolves its vertical support.
				FVector Resolved = End;
				const FVector PathGoal = End;
				if (bJumpAirborne)
				{
					// A steep contact redirects falling velocity; it is not a stable
					// landing. Ending/restarting the fall each frame loses momentum
					// and turns a roof slide into a long, slow crawl.
					const auto AirMove = ACEBodySweep::MoveAirborne(*World,Start,End,Shape,SweepParams,JumpWorldAceVelocity.Z<=0.f,FloorZ);
					Resolved=AirMove.Position; BounceNormalUe=AirMove.ContactNormal;
					bHaveBounceNormal=!BounceNormalUe.IsNearlyZero();
					if (AirMove.bLanded) FinishJumpLanding();
				}
				else
				{
				FVector PathStart = Start;
				// Move laterally before stepping down. A diagonal chord between
                // two valid sphere supports cuts through a narrow seat edge.
                // The final lower-sphere ground snap resolves descent at the
                // reached XY, after the body has cleared the obstruction.
                if (!bJumpAirborne) End.Z=FMath::Max(End.Z,Start.Z);
				const int32 SweepSteps = FMath::Clamp(FMath::CeilToInt(
					FVector::Dist2D(PathStart, PathGoal) / MaxHorizStep), 1, 32);
				const FVector StepDelta = (PathGoal - PathStart) / SweepSteps;
				for (int32 SweepStep = 0; SweepStep < SweepSteps; ++SweepStep)
				{
					// Continue from the resolved contact position. Re-aiming at the
					// original endpoint would accumulate blocked distance into the
					// next sweep and push through walls or skip narrow stair treads.
					End = PathStart + StepDelta;
					Start = PathStart;
					if (End.Equals(Start, 0.1f))
					{
						continue;
					}
					FHitResult Hit;
					bool bPathContact = SweepCapsule(Hit, Start, End);
                    // A ramp hit can hide a wall later in the same query.
                    // Follow its walkable plane and sweep the remaining path
                    // again; accepting End outright tunnels through the wall.
                    for (int32 RampPass=0; bPathContact && !IsWallHit(Hit)
                        && !Hit.bStartPenetrating && RampPass<3; ++RampPass)
                    {
                        Start=Hit.Location+Hit.Normal*.1f;
                        const FVector Remaining=End-Start;
                        End.Z=FMath::Max(End.Z, Start.Z
                            -(Hit.Normal.X*Remaining.X+Hit.Normal.Y*Remaining.Y)/Hit.Normal.Z);
                        bPathContact=SweepCapsule(Hit,Start,End);
                    }
					if (bPathContact && IsWallHit(Hit))
					{
						{
							static double LastWallLogSec = 0.0;
							const double NowSec = FPlatformTime::Seconds();
							if (NowSec - LastWallLogSec > 0.75)
							{
								LastWallLogSec = NowSec;
								const AActor* HitActor = Hit.GetActor();
								const UPrimitiveComponent* HitComp = Hit.Component.Get();
								UE_LOG(LogTemp, Verbose,
									TEXT("ACEWallBlock: actor=%s comp=%s cell=0x%08X n=(%.2f,%.2f,%.2f) pen=%d"),
									HitActor ? *HitActor->GetName() : TEXT("null"),
									HitComp ? *HitComp->GetName() : TEXT("null"),
									Pred.CellId,
									Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z,
									Hit.bStartPenetrating ? 1 : 0);
							}
						}
						FVector Stepped;
						if (!bJumpAirborne && !ACEBodySweep::IsCreatureBody(Hit) && Hit.ImpactNormal.Z>=-.15f && !Hit.bStartPenetrating && TryStepUp(Start, End, Stepped))
						{
							Resolved = Stepped;
							StepHoldMinFeetZ = Stepped.Z - CapsuleHalfHeight;
							StepHoldSeconds = StepHoldDuration;
						}
						else if (Hit.bStartPenetrating)
						{
							if (!bJumpAirborne && !ACEBodySweep::IsCreatureBody(Hit) && Hit.ImpactNormal.Z>=-.15f && TryStepUp(Start, End, Stepped))
							{
								Resolved = Stepped;
								StepHoldMinFeetZ = Stepped.Z - CapsuleHalfHeight;
								StepHoldSeconds = StepHoldDuration;
							}
							else
							{
								Resolved = ACEBodySweep::SlideFromPenetration(*World, Start, End, Hit, Shape, SweepParams);
								BounceNormalUe = Hit.ImpactNormal.GetSafeNormal();
								bHaveBounceNormal = true;
							}
						}
						else
						{
							BounceNormalUe = Hit.ImpactNormal.GetSafeNormal();
							bHaveBounceNormal = true;
							const FVector HitLoc(Hit.Location.X, Hit.Location.Y, Hit.Location.Z);
							FVector SlideOrigin = HitLoc;
							const FVector GroundNormal=ACEBodySweep::GroundContactNormal(*World,Start,Shape,SweepParams);
							const FVector Remainder=ACEBodySweep::GroundSlideDelta(End-SlideOrigin,Hit.Normal.GetSafeNormal(),GroundNormal);
							// Keep airborne Z from End (ballistic); ground slides snap feet.
							FVector SlideEnd = SlideOrigin + Remainder;
							if (bJumpAirborne) SlideEnd.Z=End.Z;
							if (!bJumpAirborne)
							{
								float SlideFeet = SlideOrigin.Z - CapsuleHalfHeight;
								if (SampleFeetZ(SlideEnd.X, SlideEnd.Y, FeetNow, RampClimbCm, RampDropCm, SlideFeet))
								{
									SlideEnd.Z = FMath::Max(SlideEnd.Z, SlideFeet + CapsuleHalfHeight);
								}
							}
							FHitResult SlideHit;
							const bool bSlideContact=SweepCapsule(SlideHit, SlideOrigin, SlideEnd);
							if (bSlideContact && IsWallHit(SlideHit))
							{
								if (!bJumpAirborne && !ACEBodySweep::IsCreatureBody(SlideHit) && SlideHit.ImpactNormal.Z>=-.15f && !SlideHit.bStartPenetrating
									&& TryStepUp(SlideOrigin, SlideEnd, Stepped))
								{
									Resolved = Stepped;
									StepHoldMinFeetZ = Stepped.Z - CapsuleHalfHeight;
									StepHoldSeconds = StepHoldDuration;
								}
								else if (SlideHit.bStartPenetrating)
								{
									Resolved = ACEBodySweep::SlideFromPenetration(*World, SlideOrigin, SlideEnd, SlideHit, Shape, SweepParams);
									BounceNormalUe = SlideHit.ImpactNormal.GetSafeNormal();
									bHaveBounceNormal = true;
								}
								else
								{
									Resolved = ACEBodySweep::SlideGrounded(*World, SlideOrigin, SlideEnd, SlideHit, Shape, SweepParams);
									BounceNormalUe = SlideHit.ImpactNormal.GetSafeNormal();
									bHaveBounceNormal = true;
								}
							}
							else
							{
								Resolved = bSlideContact ? (SlideHit.bStartPenetrating ? SlideOrigin : SlideHit.Location) : SlideEnd;
							}
						}
					}
					else
					{
						Resolved = bPathContact ? (Hit.bStartPenetrating ? Start : Hit.Location) : End;
					}
					PathStart = Resolved;
				}

				}

				Desired.X = Resolved.X;
				Desired.Y = Resolved.Y;
				Desired.Z = Resolved.Z - CapsuleHalfHeight; // feet; capsule center after ground snap
				Pred.SetLocationFromUnreal(Desired, WorldScale);

				// Airborne: retail PhysicsObj.handle_all_collisions uses the object's elasticity.
				if (bJumpAirborne)
				{
					const float BlockedZ = Resolved.Z - PathGoal.Z;
					if (!bHaveBounceNormal && BlockedZ < -2.f && JumpWorldAceVelocity.Z > 0.f)
					{
						// Fallback only when the support query supplied no contact normal.
						// A real ceiling contact takes the same retail bounce as a wall.
						JumpWorldAceVelocity.Z = 0.f;
					}
					if (bHaveBounceNormal)
					{
						// AceVectorToUnreal flips X — convert velocity to UE, bounce, convert back.
						FVector VelUe(-JumpWorldAceVelocity.X, JumpWorldAceVelocity.Y, JumpWorldAceVelocity.Z);
						const FVector N = BounceNormalUe.GetSafeNormal();
						const float CollisionAngle = FVector::DotProduct(VelUe, N);
						const auto PhysicsSession = Client->GetSession();
						const auto* PhysicsSelf = PhysicsSession ? PhysicsSession->GetWorldObjects().Find(Client->GetPlayerGuid()) : nullptr;
						if (PhysicsSelf && (PhysicsSelf->PhysicsState & ACEPhysicsState::Inelastic))
							JumpWorldAceVelocity = FVector::ZeroVector;
						else if (CollisionAngle < 0.f)
						{
							const float Elasticity = PhysicsSelf ? PhysicsSelf->GetValidElasticity() : .05f;
							VelUe += N * (-CollisionAngle * (Elasticity + 1.f));
							JumpWorldAceVelocity = FVector(-VelUe.X, VelUe.Y, VelUe.Z);
						}
					}

				}
			}

			// CTransition-lite: CellId from portal / building transit + point_in_cell
			// (not only shallow CellBSP BFS). Feet Z after step-up for containment.
			if (UGameInstance* GI = GetGameInstance())
			{
				if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
				{
					// Use capsule center (not feet) so thin doorway CellBSP doesn't flicker
					// outdoor↔indoor every tick and fight shell collision.
					const FVector TestPos(Desired.X, Desired.Y, Desired.Z + CapsuleHalfHeight);
					const FVector PathStart = Current; // already the capsule center
					uint32 ResolvedCell = static_cast<uint32>(Pred.CellId);
					if (ACECellTransit::ResolvePathCellId(
							*Dat, ResolvedCell, PathStart, TestPos,
							FMath::Max(8.f, CapsuleRadius), WorldScale, ResolvedCell))
					{
						// Retail CTransition::validate_transition commits the accepted cell
						// in the air too. Deferring until landing loses thin window/door
						// entry cells, leaving a player deep inside a room outdoor-resident.
						// The terrain presenter prepares occupied PhysicsBSP on the same
						// frame; collision readiness must not discard a valid transit.
						Pred.CellId = static_cast<int32>(ResolvedCell);
						Pred.SetLocationFromUnreal(Desired, WorldScale);
					}
				}
			}

			Pred.NormalizeOutdoorLandblock();
			bool bIndoor = (static_cast<uint32>(Pred.CellId) & 0xFFFFu) >= 0x0100u;

			// Snap feet to walkable mesh — skipped while airborne so jump arcs aren't crushed.
			float GroundZ = Desired.Z;
			bool bHaveGround = false;
			bool bSupportHasLand = !bIndoor;
			const float RampClimbCm = bIndoor
				? (GetStepUpHeightCm() + 12.f)
				: (GetStepUpHeightCm() + 28.f);
			float SnapStepDownCm = GetStepDownHeightCm();
			if (UGameInstance* SnapGI = GetGameInstance())
			{
				if (UACEDatSubsystem* SnapDat = SnapGI->GetSubsystem<UACEDatSubsystem>())
				{
					SnapStepDownCm += SnapDat->GetOutdoorWaterDepthCm(Desired.X, Desired.Y, WorldScale);
				}
			}
			const float FeetZ = Desired.Z;

			if (!bJumpAirborne)
			{
			if (UWorld* World = GetWorld())
			{
				FCollisionQueryParams Params(SCENE_QUERY_STAT(ACEGroundSnap), /*bTraceComplex*/ true, P);
				if (bVR) { TArray<AActor*> AttachedPresentation; P->GetAttachedActors(AttachedPresentation, true, true); Params.AddIgnoredActors(AttachedPresentation); }
				// Match sweep transit: ignore EnvCells outside candidates; ignore land when
				// outdoor land is not in the transit set (indoors / building entry).
				if (UGameInstance* SnapGI = GetGameInstance())
				{
					if (UACEDatSubsystem* SnapDat = SnapGI->GetSubsystem<UACEDatSubsystem>())
					{
						TArray<uint32> SnapTransit;
						bool bSnapHitsInterior = false;
						const float SnapBoundR = FMath::Sqrt(
							FMath::Square(CapsuleRadius) + FMath::Square(CapsuleHalfHeight));
						ACECellTransit::FindCellList(*SnapDat, static_cast<uint32>(Pred.CellId),
							FVector(Desired.X, Desired.Y, FeetZ + CapsuleHalfHeight),
							FMath::Max(8.f, SnapBoundR), WorldScale, SnapTransit, &bSnapHitsInterior);
						TSet<uint32> SnapEnv;
						bool bSnapHasLand = false;
						for (uint32 Id : SnapTransit)
						{
							if (ACECellTransit::IsIndoorCell(Id))
							{
								SnapEnv.Add(Id);
							}
							else
							{
								bSnapHasLand = true;
							}
						}
						bSupportHasLand = bSnapHasLand && ACECellTransit::UsesOutdoorGround(
							*SnapDat, Pred.CellId, Desired, WorldScale);
						if (!bSupportHasLand)
						{
							for (TActorIterator<AACELandblockActor> It(World); It; ++It)
							{
								if (It->TerrainMesh)
								{
									Params.AddIgnoredComponent(
										static_cast<const UPrimitiveComponent*>(It->TerrainMesh.Get()));
								}
							}
							for (TActorIterator<AACETerrainChunkActor> It(World); It; ++It)
							{
								if (It->TerrainMesh)
								{
									Params.AddIgnoredComponent(
										static_cast<const UPrimitiveComponent*>(It->TerrainMesh.Get()));
								}
							}
						}
						if (bIndoor) ACECellTransit::RestrictRoomCollision(*World, SnapEnv, Params);
						(void)bSnapHitsInterior;
					}
				}
				if (bSupportHasLand)
					FilterWadingTerrain(*World,GetGameInstance()->GetSubsystem<UACEDatSubsystem>(),
						P->GetActorLocation(),Desired,WorldScale,Params);

                // Use the same lower-sphere support as the movement sweep. A
                // second center-only snap must not undo a successful edge step.
                float SphereGroundZ=FeetZ;
                const bool bHaveSphereGround=ACEBodySweep::FindBodyFootSupport(*World,
                    FVector(Desired.X,Desired.Y,FeetZ),GetPlayerCollisionBody(),SnapStepDownCm,Params,SphereGroundZ);
				const float TraceTop = FeetZ + RampClimbCm + .5f;
				TArray<FHitResult> Hits;
				auto CollectGroundHits = [&](double X, double Y)
				{
					const FVector S(X, Y, TraceTop);
					// Cap depth to StepDownHeight (+ waterDepth at dest) — unlimited traces
					// caused cliff blink-to-ground.
					const FVector E(X, Y, FeetZ - SnapStepDownCm - 40.f);
					TArray<FHitResult> LocalHits;
					if (!ACEBodySweep::TraceGround(*World, LocalHits, S, E, Params))
					{
						return;
					}
					Hits.Append(LocalHits);
				};
				CollectGroundHits(Desired.X, Desired.Y);
				if (bIndoor)
				{
					const float Ring = 18.f;
					CollectGroundHits(Desired.X + Ring, Desired.Y);
					CollectGroundHits(Desired.X - Ring, Desired.Y);
					CollectGroundHits(Desired.X, Desired.Y + Ring);
					CollectGroundHits(Desired.X, Desired.Y - Ring);
				}
				if (Hits.Num() > 0)
				{
					float BestIndoorZ = -TNumericLimits<float>::Max();
					float BestOutdoorZ = -TNumericLimits<float>::Max();
					bool bHaveIndoor = false;
					bool bHaveOutdoor = false;
					UACEDatSubsystem* SnapDat = nullptr;
					if (UGameInstance* GI = GetGameInstance())
					{
						SnapDat = GI->GetSubsystem<UACEDatSubsystem>();
					}
					for (const FHitResult& Hit : Hits)
					{
						if (!Hit.bBlockingHit || ACEBodySweep::IsCreatureBody(Hit) || Hit.ImpactNormal.Z < 0.6641741f)
						{
							continue;
						}
						if (Hit.ImpactPoint.Z > FeetZ + RampClimbCm)
						{
							continue;
						}
						if (Hit.ImpactPoint.Z < FeetZ - SnapStepDownCm)
						{
							continue;
						}
						const bool bOutdoor = Hit.Component.IsValid()
							&& Hit.Component->ComponentTags.Contains(FName(TEXT("ACEOutdoorTerrain")));
						const bool bBuildingShell = Hit.Component.IsValid()
							&& Hit.Component->ComponentTags.Contains(FName(TEXT("ACEBuildingShell")));
						(void)bBuildingShell;
						if (bOutdoor)
						{
							float WalkZ = Hit.ImpactPoint.Z;
							if (SnapDat)
							{
								const float DepthCm = SnapDat->GetOutdoorWaterDepthCm(
									Desired.X, Desired.Y, WorldScale);
								if (DepthCm > KINDA_SMALL_NUMBER)
								{
									WalkZ -= DepthCm / FMath::Max(0.15f, Hit.ImpactNormal.Z);
								}
							}
							if (!bHaveOutdoor
								|| FMath::Abs(WalkZ - FeetZ) < FMath::Abs(BestOutdoorZ - FeetZ))
							{
								BestOutdoorZ = WalkZ;
								bHaveOutdoor = true;
							}
						}
						else if (!bHaveIndoor
							|| FMath::Abs(Hit.ImpactPoint.Z - FeetZ) < FMath::Abs(BestIndoorZ - FeetZ))
						{
							// Outdoors: ignore Stab roofs / awnings above StepUp (same as SampleFeetZ).
							if (!bIndoor && Hit.ImpactPoint.Z > FeetZ + GetStepUpHeightCm() + 12.f)
							{
								continue;
							}
							BestIndoorZ = Hit.ImpactPoint.Z;
							bHaveIndoor = true;
						}
					}

					if (bSupportHasLand && SnapDat)
					{
						// Retail: heightfield support applies to admitted land cells, even
						// when an interior passage has no floor. Raised walkables win via the step
						// preference below when clearly above grade.
						float DatZ = FeetZ;
						FVector DatNormal;
						if (SnapDat->SampleOutdoorGroundZ(Desired.X, Desired.Y, WorldScale, DatZ, &DatNormal)
							&& DatNormal.Z >= FloorZ
							&& DatZ <= FeetZ + RampClimbCm
							&& DatZ >= FeetZ - SnapStepDownCm)
						{
							if (!bHaveOutdoor
								|| FMath::Abs(DatZ - FeetZ) < FMath::Abs(BestOutdoorZ - FeetZ))
							{
								BestOutdoorZ = DatZ;
								bHaveOutdoor = true;
							}
						}
					}

					if (!bHaveGround && bIndoor && bHaveIndoor)
					{
						GroundZ = BestIndoorZ;
						bHaveGround = true;
					}
					else if (!bHaveGround && bIndoor && !bSupportHasLand)
					{
						if (StepHoldSeconds > 0.f)
						{
							GroundZ = StepHoldMinFeetZ;
							bHaveGround = true;
						}
					}
					else if (!bHaveGround && bHaveIndoor
						&& BestIndoorZ <= FeetZ + GetStepUpHeightCm() + 12.f
						&& BestIndoorZ >= FeetZ - SnapStepDownCm
						&& (!bHaveOutdoor || BestIndoorZ >= BestOutdoorZ + 1.5f))
					{
						// Prefer raised walkables (stairs / stoops / chests) within StepUp —
						// not RampClimbCm (that snapped players onto low tent roofs).
						GroundZ = BestIndoorZ;
						bHaveGround = true;
					}
					else if (!bHaveGround && bHaveOutdoor)
					{
						GroundZ = BestOutdoorZ;
						bHaveGround = true;
					}
					else if (!bHaveGround && bHaveIndoor && bIndoor)
					{
						// Indoor-only fallback. Outdoors never snap onto leftover Stab roofs.
						GroundZ = BestIndoorZ;
						bHaveGround = true;
					}
					if (bHaveGround)
					{
						Pred.Location.Z = GroundZ / WorldScale;
					}
				}
                // A narrow edge can support the lower sphere even when every
                // vertical ray misses. Keep that contact until the body clears it.
                if (bHaveSphereGround && (!bHaveGround || SphereGroundZ > GroundZ))
                {
                    GroundZ = SphereGroundZ;
                    bHaveGround = true;
                    Pred.Location.Z = GroundZ / WorldScale;
                }
				// Large-world contact precision can alternate the sphere/ray height
				// by a few millimetres on slopes. A stationary tracked player should
				// not feel that numerical correction as repeated camera bobbing.
				if (bVR && !bJumpAirborne && !bMoving && VRRoomDelta.IsNearlyZero(.1f)
					&& FVector::DistSquared2D(Desired,Current)<.01 && bHaveGround
					&& FMath::Abs(GroundZ-FeetZ)<1.f)
				{
					GroundZ=FeetZ; Pred.Location.Z=FeetZ/WorldScale;
				}
                // Rays can see the next tread before the body clears the lip
                // of this one. The final downward adjustment is movement too:
                // stop at the full capsule's first contact instead of pulling
                // its lower sphere through that edge.
                if (bHaveGround && GroundZ > FeetZ)
                {
                    // A support ray/sphere does not test headroom. Never apply
                    // its upward correction through the floor above a stair.
                    const FVector Center(Desired.X,Desired.Y,FeetZ+CapsuleHalfHeight);
                    FHitResult RiseHit;
                    if (ACEBodySweep::Sweep(*World,RiseHit,Center,
                        FVector(Center.X,Center.Y,GroundZ+CapsuleHalfHeight),
                        GetPlayerCollisionBody(),Params)
                        && RiseHit.ImpactNormal.Z<.6641741f)
                    {
                        GroundZ=RiseHit.bStartPenetrating ? FeetZ : RiseHit.Location.Z-CapsuleHalfHeight;
                        // The clamped crown may leave the foot inside the ramp.
                        // Recover downhill when upward recovery cannot clear
                        // the ceiling, rather than keeping that wedged pose.
                        FVector Clamped(Desired.X,Desired.Y,GroundZ+CapsuleHalfHeight);
                        const auto BodyShape=GetPlayerCollisionBody();
                        const FVector FootCenter=Clamped+BodyShape.Centers[0];
                        FHitResult Contact;
                        // Query the foot separately: a time-zero crown hit can
                        // hide its simultaneous ramp contact in a body sweep.
                        if (World->SweepSingleByChannel(Contact,FootCenter,FootCenter-FVector(0,0,.1f),
                            FQuat::Identity,ECC_Pawn,FCollisionShape::MakeSphere(BodyShape.Radii[0]),Params)
                            && !ACEBodySweep::IsCreatureBody(Contact)
                            && Contact.bStartPenetrating && Contact.PenetrationDepth>.05f && Contact.Normal.Z>=.6641741f)
                        {
                            Clamped=ACEBodySweep::Recover(*World,Clamped,Contact.Normal*(Contact.PenetrationDepth+.5f),Contact,BodyShape,Params);
                        }
                        Desired.X=Clamped.X;Desired.Y=Clamped.Y;GroundZ=Clamped.Z-CapsuleHalfHeight;
                        Pred.SetLocationFromUnreal(FVector(Desired.X,Desired.Y,GroundZ),WorldScale);
                    }
                }
                if (bHaveGround && GroundZ < FeetZ)
                {
                    const FVector Center(Desired.X, Desired.Y, FeetZ + CapsuleHalfHeight);
                    {
                        // A side contact is not a floor. The old single sweep
                        // treated any initial wall overlap as support forever.
                        const FVector SupportCenter(Center.X,Center.Y,GroundZ+CapsuleHalfHeight);
                        const auto Snap = ACEBodySweep::MoveAirborne(*World, Center,
                            SupportCenter,
                            GetPlayerCollisionBody(),Params,true,FloorZ,true);
                        Desired.X=Snap.Position.X; Desired.Y=Snap.Position.Y;
                        GroundZ=Snap.Position.Z-CapsuleHalfHeight;
                        // An unobstructed sweep reaching the already validated
                        // support is grounded too. Requiring an earlier impact
                        // toggled falling on exact end-of-sweep stair contacts.
                        bHaveGround=Snap.bLanded || Snap.Position.Equals(SupportCenter,.01f);
                        if (!bHaveGround && Snap.Position.Equals(Center,.01f))
                        {
                            // At a ramp/flat seam, Chaos can report microscopic
                            // penetration of the lower sphere into the tread
                            // edge. Recovery cannot resolve that rounded contact
                            // and the ledge guard would undo all forward travel.
                            // Accept only contact skin, with independently proven
                            // walkable support within retail StepDown. Walls,
                            // crowns, creatures and real overlaps still block.
                            FHitResult Contact;
                            if (ACEBodySweep::Sweep(*World,Contact,Center,SupportCenter,
                                GetPlayerCollisionBody(),Params,false)
                                && Contact.bStartPenetrating && Contact.PenetrationDepth<=.05f
                                && Contact.Normal.Z>.0871557f && !ACEBodySweep::IsUpperBodyContact(Contact)
                                && !ACEBodySweep::IsCreatureBody(Contact)
                                && HasLedgeSupport(*World,GetGameInstance()->GetSubsystem<UACEDatSubsystem>(),
                                    FVector(Desired.X,Desired.Y,GroundZ),CapsuleRadius,SnapStepDownCm,
                                    WorldScale,bSupportHasLand,Params,GetPlayerCollisionBody().Count)) bHaveGround=true;
                        }
                        for (int32 Retry=0; !bHaveGround && Retry<3; ++Retry)
                        {
                            // A side contact can shift XY while descending off
                            // a tread. Re-evaluate its floor, then sweep to it;
                            // the original support point is no longer beneath us.
                            // Keep the original step-down budget so a recovery
                            // cannot attach the feet to a floor across a drop.
                            float ResolvedSupportZ=GroundZ;
                            const float RemainingDown=FMath::Max(0.f,GroundZ-(FeetZ-SnapStepDownCm));
                            if (!ACEBodySweep::FindBodyFootSupport(*World,
                                FVector(Desired.X,Desired.Y,GroundZ),GetPlayerCollisionBody(),RemainingDown,
                                Params,ResolvedSupportZ,CapsuleRadius)) break;
                            if (FMath::Abs(ResolvedSupportZ-GroundZ)<1.f)
                            {
                                bHaveGround=true;
                                break;
                            }
                            if (ResolvedSupportZ>GroundZ) break;
                            const FVector ResolvedCenter(Desired.X,Desired.Y,GroundZ+CapsuleHalfHeight);
                            const FVector ResolvedTarget(Desired.X,Desired.Y,ResolvedSupportZ+CapsuleHalfHeight);
                            const auto Contact=ACEBodySweep::MoveAirborne(*World,ResolvedCenter,ResolvedTarget,
                                GetPlayerCollisionBody(),Params,true,FloorZ,true);
                            Desired.X=Contact.Position.X; Desired.Y=Contact.Position.Y;
                            GroundZ=Contact.Position.Z-CapsuleHalfHeight;
                            bHaveGround=Contact.bLanded || Contact.Position.Equals(ResolvedTarget,.01f);
                            if (Contact.Position.Equals(ResolvedCenter,.01f)) break;
                        }
                        Pred.SetLocationFromUnreal(FVector(Desired.X,Desired.Y,GroundZ),WorldScale);
                    }
                }
			}
			}

			// Outdoor heightfield fallback when complex mesh traces miss.
			// Retail: land support follows the collision cell list, not just residency.
			if (!bJumpAirborne && !bHaveGround && bSupportHasLand)
			{
				if (UGameInstance* GI = GetGameInstance())
				{
					if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
					{
						float SampledZ = Desired.Z;
						FVector DatNormal;
						if (Dat->SampleOutdoorGroundZ(Desired.X, Desired.Y, WorldScale, SampledZ, &DatNormal)
							&& DatNormal.Z >= FloorZ)
						{
							const bool bAcceptOutdoor = SampledZ <= FeetZ + RampClimbCm
								&& SampledZ >= FeetZ - SnapStepDownCm;
							if (bAcceptOutdoor)
							{
								GroundZ = SampledZ;
								bHaveGround = true;
								Pred.Location.Z = SampledZ / WorldScale;
							}
						}
					}
				}
			}

			// Wall sliding, penetration recovery and the downward sweep can all
			// change XY after the first ledge test. Commit only a supported result,
			// as retail edge_slide does, instead of beginning an accidental fall.
			// Unsupported portal drops and explicit jumps never enter this guard.
			if (LedgeSupportParams.IsSet() && !bJumpAirborne
				&& (!bHaveGround || !HasLedgeSupport(*GetWorld(),
					GetGameInstance()->GetSubsystem<UACEDatSubsystem>(),
					FVector(Desired.X,Desired.Y,GroundZ), CapsuleRadius, SnapStepDownCm,
					WorldScale, bSupportHasLand, LedgeSupportParams.GetValue(),GetPlayerCollisionBody().Count)))
			{
				Desired = Current-FVector(0,0,CapsuleHalfHeight);
				GroundZ = Desired.Z;
				Pred.CellId = SupportedStartCell;
				bIndoor = (SupportedStartCell & 0xFFFFu) >= 0x100u;
				Pred.SetLocationFromUnreal(Desired,WorldScale);
				StepHoldSeconds = 0.f;
				bHaveGround = true;
				bMovementLedge = true;
			}

			if (bJumpAirborne)
			{
				// Keep predicted feet Z; convert to capsule center below.
				GroundZ = Desired.Z;
				bHaveGround = true;
			}
			else if (!bHaveGround && StepHoldSeconds > 0.f)
			{
				// Just stepped onto a tread — trust capsule step-up for a beat so line-trace
				// misses don't fall through stairs.
				GroundZ = FMath::Max(Desired.Z, StepHoldMinFeetZ);
				bHaveGround = true;
			}
			else if (!bHaveGround)
			{
				bool bHoldForFloor = false;
				if (bIndoor)
				{
					if (AGameModeBase* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
					{
						if (UACETerrainPresenterComponent* Terrain = GM->FindComponentByClass<UACETerrainPresenterComponent>())
						{
							bHoldForFloor = !Terrain->IsPlayerCellCollisionReady();
						}
					}
				}
				if (bHoldForFloor)
				{
					// Hold only while collision is streaming. A loaded indoor cell
					// can contain stairs, balconies and open drops with no support.
					GroundZ = Desired.Z;
					bHaveGround = true;
				}
				else
				{
					// Walking off an edge is airborne even without a Jump action. Keep
					// integrating and reporting this pose when the movement keys are released.
					bJumpAirborne = true;
					bLocalPredicting = true;
					bHavePredictedPose = true;
					JumpAirborneSeconds = 0.f;
					JumpWorldAceVelocity = Pred.GetAceForwardInAcSpace() * F
						* (F < 0.f ? .65f : 1.f) * Client->GetLocomotionSpeed(bRunning)
						+ Pred.GetAceRightInAcSpace() * R * Client->GetSidestepSpeed(bRunning);
					if (bUniformVRMovement)
						JumpWorldAceVelocity = Pred.GetAcQuat().RotateVector(ACEVRLocomotion::JumpVelocity(F, R, bRunning, Client->GetRunRate()));
					JumpWorldAceVelocity.Z = -9.8f * DeltaTime;
					GroundZ = Desired.Z - .5f * 9.8f * WorldScale * DeltaTime * DeltaTime;
					bForceMovementResend = true;
					Client->SetForcePositionReporting(true);
					if (auto* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
					{
						App->SetSuppressLocoIdleBlend(true);
						App->SetHeldActionMotion(static_cast<int32>(0x40000015));
					}
				}
			}

			// Falling bypasses the normal ground snap. A streamed terrain body or
			// a one-sided triangle missed on a steep descent must not leave the
			// feet underneath the known heightfield, including the submerged DAT
			// support while the rendered water sheet is ignored. This is a floor bound,
			// not a snap-down: jumps keep their arc until they reach the ground.
			if (!bIndoor)
			{
				if (auto* Dat = GetGameInstance()->GetSubsystem<UACEDatSubsystem>())
				{
					float TerrainZ;
					FVector TerrainNormal;
					if (Dat->SampleOutdoorGroundZ(Desired.X, Desired.Y, WorldScale, TerrainZ, &TerrainNormal))
					{
						if (GroundZ < TerrainZ)
						{
							GroundZ = TerrainZ;
							bHaveGround = true;
							if (bJumpAirborne && TerrainNormal.Z >= FloorZ) FinishJumpLanding();
						}
						// A heightfield point is not the lower body's contact point on
						// an incline. The fallback above can put the sphere into the
						// slope even though its feet are above the sampled triangle.
						// Resolve that actual overlap against all solids; do not raise
						// the player from an infinite extension of the terrain plane.
						if (GroundZ<=TerrainZ+CapsuleRadius && TerrainNormal.Z<.999f)
						{
							FCollisionQueryParams TerrainParams(SCENE_QUERY_STAT(TerrainBodyRecovery),true,P);
							const FVector Center(Desired.X,Desired.Y,GroundZ+CapsuleHalfHeight);
							FilterWadingTerrain(*GetWorld(),Dat,Center,Center,WorldScale,TerrainParams);
							FHitResult TerrainHit;
							const auto Body=GetPlayerCollisionBody();
							FVector Recovered=Center;
							for(int32 Contact=0;Contact<4;++Contact)
							{
								if (!ACEBodySweep::SweepBody(*GetWorld(),TerrainHit,Recovered,Recovered+FVector(0,0,.01f),Body,TerrainParams)
									|| !TerrainHit.bStartPenetrating || TerrainHit.PenetrationDepth<=.05f
									|| TerrainHit.Normal.Z<=.0871557f || !TerrainHit.GetComponent()
									|| !TerrainHit.GetComponent()->ComponentHasTag(TEXT("ACEOutdoorTerrain"))) break;
								const FVector Push=TerrainHit.Normal*(TerrainHit.PenetrationDepth+.2f);
								const FVector Next=bJumpAirborne
									? ACEBodySweep::Recover(*GetWorld(),Recovered,Push,TerrainHit,Body,TerrainParams)
									: ACEBodySweep::RecoverGrounded(*GetWorld(),Recovered,Push,TerrainHit,Body,TerrainParams);
								if (Next.Equals(Recovered,.01f)) break;
								Recovered=Next;
							}
							if (!Recovered.Equals(Center,.01f))
							{
								const FVector RecoveredFeet=Recovered-FVector(0,0,CapsuleHalfHeight);
								if (!bJumpAirborne && LedgeSupportParams.IsSet()
									&& !HasLedgeSupport(*GetWorld(),Dat,RecoveredFeet,CapsuleRadius,
										SnapStepDownCm,WorldScale,true,LedgeSupportParams.GetValue(),Body.Count))
								{
									Desired.X=Current.X;Desired.Y=Current.Y;
									GroundZ=Current.Z-CapsuleHalfHeight;
									bMovementLedge=true;
								}
								else
								{
									Desired.X=RecoveredFeet.X;Desired.Y=RecoveredFeet.Y;
									GroundZ=RecoveredFeet.Z;
								}
							}
						}
					}
				}
			}

			if (StepHoldSeconds > 0.f)
			{
				StepHoldSeconds = FMath::Max(0.f, StepHoldSeconds - DeltaTime);
			}

			// AC positions and the visual origin are at the surface, without a UE skin offset.
			constexpr float GroundSkinCm = 0.f;
			Desired.Z = GroundZ + CapsuleHalfHeight + GroundSkinCm;

			if (bVR && !VRRoomDelta.IsNearlyZero()) VR->CompensateRoomScale(Desired - P->GetActorLocation());
			if (const auto* Debug = IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Movement.Debug")); Debug && Debug->GetInt() && bMoving)
            {
                static double LastMoveLog = 0.;
                const double Now = FPlatformTime::Seconds();
                const FVector Requested(BeforeCollision.X-Current.X,BeforeCollision.Y-Current.Y,0);
                const float Along = FVector::DotProduct(Desired-Current,Requested.GetSafeNormal());
                const bool bDetail = Debug->GetInt() >= 2 && (MovementStartCell != uint32(Pred.CellId)
                    || (Requested.Size() > .5f && Along < Requested.Size()*.9f));
                if (Now - LastMoveLog >= (bDetail ? .1 : 1.))
                {
                    LastMoveLog = Now;
                    UE_LOG(LogTemp, Log, TEXT("ACE MoveStep: dt=%.4f forward=%.3f side=%.3f run=%d vr=%d predicted=%d expected=%.2f beforeCollision=%.2f afterCollision=%.2f airborne=%d"),
                        DeltaTime, F, R, bRunning, bVR, bLocalPredicting, F * Client->GetLocomotionSpeed(bRunning) * GetLocalCreatureScale() * DeltaTime * WorldScale,
                        FVector::Dist2D(BeforeCollision, P->GetActorLocation()), FVector::Dist2D(Desired, P->GetActorLocation()), bJumpAirborne);
                    if (Debug->GetInt() >= 2)
                        UE_LOG(LogTemp, Log, TEXT("ACE MoveContact: cell=%08X->%08X feet=%s requested=%s reached=%s along=%.2f blocker=%s normal=%s penetration=%.3f ledge=%d support=%d"),
                            MovementStartCell, uint32(Pred.CellId), *(Current-FVector(0,0,CapsuleHalfHeight)).ToString(),
                            *Requested.ToString(), *(Desired-Current).ToString(), Along, *MovementBlocker.ToString(),
                            *MovementContactNormal.ToString(), MovementPenetration, bMovementLedge, bHaveGround);
                }
            }
            P->SetActorLocation(Desired, false);
			if (!bJumpAirborne && (bHadLandingMotion || bLandedThisStep))
			{
				if (bMovementLedge) { LandingWorldAceVelocity = FVector::ZeroVector; LandingRootTrack.Reset(); }
				else if (!MovementContactNormal.IsNearlyZero())
				{
					const FVector N = FACEPosition::AceVectorToUnreal(MovementContactNormal.GetSafeNormal2D());
					const float Into = FVector::DotProduct(LandingWorldAceVelocity,N);
					if (Into < 0) LandingWorldAceVelocity -= Into*N;
				}
				Client->SetForcePositionReporting(!LandingWorldAceVelocity.IsNearlyZero() || LandingRootTrack.IsActive());
			}
			P->SetActorRotation(Pred.ToUnrealQuat());

			// Desired.Z is capsule center; AutoPos must report feet so WithinUseRadius matches.
			Pred.SetLocationFromUnreal(
				FVector(Desired.X, Desired.Y, Desired.Z - CapsuleHalfHeight - GroundSkinCm),
				WorldScale);
			if (auto Session = Client->GetSession()) Session->SetReportedContact(!bJumpAirborne);
			if (bLandedThisStep)
			{
				Client->SetReportedPosition(Pred);
				Client->FlushAutonomousPosition(true);
			}
			if (bLocalPredicting || bHavePredictedPose)
			{
				PredictedPose = Pred;
				bHavePredictedPose = true;
				if (bLocalPredicting)
				{
					Client->SetReportedPosition(Pred);
					if (bWalkingTransitionThisFrame && !bMoving && !bJumpAirborne)
					{
						// Raw keys are already released. Continue AutoPos through the
						// final collision-resolved footstep, then relinquish ownership.
						Client->SetForcePositionReporting(true);
						if (!bWalkingTransitionPending)
						{
							Client->FlushAutonomousPosition(true);
							Client->SetForcePositionReporting(false);
						}
					}
				}
				else if (bHaveLastServerPose)
				{
					// Idle: stop owning prediction once we are close enough to the server anchor.
					const FVector PredUe = Pred.ToUnrealLocation(WorldScale);
					const FVector SrvUe = LastServerPose.ToUnrealLocation(WorldScale);
					if (FVector::Dist(PredUe, SrvUe) < 25.f)
					{
						bHavePredictedPose = false;
					}
				}
			}
		}
	}
	} // Movement substeps
}

float AACEPlayerController::GetLocalCreatureScale() const
{
	if (!Client)
	{
		return 1.f;
	}
	const auto Session = Client->GetSession();
	const auto* Self = Session ? Session->GetWorldObjects().Find(Client->GetPlayerGuid()) : nullptr;
	if (Self && FMath::IsFinite(Self->Scale) && Self->Scale > KINDA_SMALL_NUMBER)
	{
		return Self->Scale;
	}
	return 1.f;
}

float AACEPlayerController::GetCameraScaleCm() const
{
	return WorldScale * GetLocalCreatureScale();
}

float AACEPlayerController::GetCameraFieldOfViewDegrees() const
{
	return DatGameplayBinder ? DatGameplayBinder->GetCameraFieldOfViewDegrees()
		: ACERuntimeOptions::Get(TEXT("FieldOfViewDegrees"));
}

void AACEPlayerController::ApplyRetailCameraFov()
{
	APawn* P = GetPawn();
	UCameraComponent* Cam = P ? P->FindComponentByClass<UCameraComponent>() : nullptr;
	int32 SizeX = 0, SizeY = 0;
	GetViewportSize(SizeX, SizeY);
	ACECameraRetail::ApplyFovToCamera(Cam, SizeX, SizeY, GetCameraFieldOfViewDegrees(), bCameraMapMode);
}

void AACEPlayerController::ApplyRetailCameraPivot(USpringArmComponent* Boom, UCapsuleComponent* Capsule) const
{
	if (!Boom || !Capsule)
	{
		return;
	}
	const float PivotZ = PersonalCamera::PivotHeightAc(bCameraInHead) * GetCameraScaleCm(); // personal: camera
	Boom->ProbeSize = ACECameraRetail::ViewerRadiusAc * WorldScale;
	Boom->SetRelativeLocation(FVector(0.f, 0.f, PivotZ - Capsule->GetScaledCapsuleHalfHeight()));
}

void AACEPlayerController::ApplyCameraOffset(USpringArmComponent* Boom, float OffsetYAc, float OffsetZAc,
	float PitchDegrees, bool bKeepYaw)
{
	if (!Boom)
	{
		return;
	}
	const float ScaleCm = GetCameraScaleCm();
	const float ArmLen = ACECameraRetail::OffsetLengthCm(OffsetYAc, OffsetZAc, ScaleCm);
	Boom->TargetArmLength = ArmLen;
	if (!bCameraInHead)
	{
		UserCameraArmLength = ArmLen;
	}
	Boom->SocketOffset = FVector::ZeroVector;
	Boom->TargetOffset = FVector::ZeroVector;
	FRotator Rotation = Boom->GetRelativeRotation();
	Rotation.Pitch = PitchDegrees;
	if (!bKeepYaw)
	{
		Rotation.Yaw = ACECameraRetail::BoomYawToFaceAcForward;
	}
	Rotation.Roll = 0.f;
	Boom->SetRelativeRotation(Rotation);
}

void AACEPlayerController::SetCameraInHead(USpringArmComponent* Boom, bool bInHead)
{
	if (!Boom)
	{
		return;
	}
	if (bInHead && bCameraLookDown)
	{
		SetCameraLookDown(Boom, false);
	}
	if (bInHead == bCameraInHead)
	{
		return;
	}
	bCameraInHead = bInHead;
	Boom->bDoCollisionTest = !bInHead;
	Boom->bEnableCameraLag = !bInHead;
	Boom->bEnableCameraRotationLag = !bInHead;
	if (APawn* P = GetPawn())
	{
		if (UACECharacterAppearanceComponent* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			App->SetAppearanceVisible(!bInHead);
		}
	}
	if (bInHead)
	{
		Boom->TargetArmLength = 0.f;
		Boom->SocketOffset = FVector(ACECameraRetail::InHeadForwardCm(GetCameraScaleCm()), 0.f, 0.f);
		Boom->TargetOffset = FVector::ZeroVector;
		FRotator Rotation = Boom->GetRelativeRotation();
		Rotation.Pitch = 0.f;
		Rotation.Roll = 0.f;
		Boom->SetRelativeRotation(Rotation);
	}
	else
	{
		Boom->SocketOffset = FVector::ZeroVector;
		ApplyCameraOffset(Boom,
			ACECameraRetail::LeaveHeadYAc,
			ACECameraRetail::LeaveHeadZAc,
			ACECameraRetail::PitchFromOffsetDegrees(
				ACECameraRetail::LeaveHeadYAc, ACECameraRetail::LeaveHeadZAc),
			true);
	}
}

void AACEPlayerController::SetCameraLookDown(USpringArmComponent* Boom, bool bLookDown)
{
	if (!bLookDown && bCameraMapMode)
	{
		bCameraMapMode = false;
		ApplyRetailCameraFov();
		if (Boom) Boom->bDoCollisionTest = true;
		if (auto* LP = GetLocalPlayer(); LP && LP->ViewportClient)
			LP->ViewportClient->EngineShowFlags.SetFog(bMapSavedFog);
	}
	if (!Boom)
	{
		return;
	}
	if (bLookDown == bCameraLookDown)
	{
		return;
	}
	if (bLookDown)
	{
		LookDownSavedRotation = Boom->GetRelativeRotation();
		LookDownSavedArm = UserCameraArmLength >= 0.f ? UserCameraArmLength : Boom->TargetArmLength;
		bLookDownHasSaved = true;
		if (bCameraInHead)
		{
			SetCameraInHead(Boom, false);
		}
		bCameraLookDown = true;
		ApplyCameraOffset(Boom,
			ACECameraRetail::LookDownOffsetYAc,
			ACECameraRetail::LookDownOffsetZAc,
			ACECameraRetail::LookDownPitchDegrees(),
			true);
		Boom->bEnableCameraLag = false;
		Boom->bEnableCameraRotationLag = false;
	}
	else
	{
		bCameraLookDown = false;
		Boom->bEnableCameraLag = true;
		Boom->bEnableCameraRotationLag = true;
		if (bLookDownHasSaved)
		{
			Boom->SetRelativeRotation(LookDownSavedRotation);
			UserCameraArmLength = LookDownSavedArm;
			Boom->TargetArmLength = LookDownSavedArm;
			Boom->SocketOffset = FVector::ZeroVector;
		}
		bLookDownHasSaved = false;
	}
}

void AACEPlayerController::SetCameraMapMode(USpringArmComponent* Boom, bool bMapMode)
{
	if (bMapMode && !ACECameraRetail::CanUseMapView(GetEffectiveCellId())) return;
	if (!Boom || bMapMode == bCameraMapMode) return;
	if (!bMapMode) { SetCameraLookDown(Boom, false); return; }
	SetCameraLookDown(Boom, true);
	bCameraMapMode = true;
	ApplyRetailCameraFov();
	ApplyCameraOffset(Boom, ACECameraRetail::MapOffsetYAc,
		ACECameraRetail::LookDownOffsetZAc, ACECameraRetail::LookDownPitchDegrees(), true);
	UserCameraArmLength = Boom->TargetArmLength = -ACECameraRetail::MapOffsetYAc * GetCameraScaleCm();
	Boom->SocketOffset.Z = ACECameraRetail::LookDownOffsetZAc * GetCameraScaleCm();
	// Retail's map camera is above the scene and does not retract against trees/roofs.
	Boom->bDoCollisionTest = false;
	if (auto* LP = GetLocalPlayer(); LP && LP->ViewportClient)
	{
		bMapSavedFog = LP->ViewportClient->EngineShowFlags.Fog;
		LP->ViewportClient->EngineShowFlags.SetFog(false);
	}
}

void AACEPlayerController::ResetCameraToRetailDefaults(USpringArmComponent* Boom)
{
	if (!Boom)
	{
		return;
	}
	if (bCameraMapMode) SetCameraMapMode(Boom, false);
	if (bCameraLookDown)
	{
		bCameraLookDown = false;
		bLookDownHasSaved = false;
	}
	if (bCameraInHead)
	{
		SetCameraInHead(Boom, false);
	}
	Boom->bDoCollisionTest = true;
	Boom->bEnableCameraLag = true;
	Boom->bEnableCameraRotationLag = true;
	Boom->SocketOffset = FVector::ZeroVector;
	ApplyCameraOffset(Boom,
		ACECameraRetail::DefaultOffsetYAc,
		ACECameraRetail::DefaultOffsetZAc,
		ACECameraRetail::DefaultPitchDegrees(),
		false);
	if (APawn* P = GetPawn())
	{
		if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
		{
			ApplyRetailCameraPivot(Boom, Cap);
		}
	}
	DefaultCameraRotation = Boom->GetRelativeRotation();
	DefaultCameraDistance = Boom->TargetArmLength;
	UserCameraArmLength = DefaultCameraDistance;
	bCameraDefaultsCaptured = true;
}

void AACEPlayerController::SetMouseLookActive(bool bActive)
{
	if (bActive && !IsUseMouseTurning() && !bInstantMouseLookHeld) return;
	if (bMouseLookActive == bActive && !bActive)
	{
		return;
	}
	const bool bEntering = bActive && !bMouseLookActive;
	bMouseLookActive = bActive;
	if (!bActive) bMouseLookToggled=false;
	if (bActive)
	{
		if (bEntering) MouseLookTravelPixels = 0.f;
		bMouseLookUsesCapture = IsUseMouseTurning() || bInstantMouseLookHeld;
		if (bMouseLookUsesCapture)
		{
			// Relative input must remain with the viewport while orbiting; otherwise
			// Slate can consume the second mouse button or stop deltas at window edges.
			struct FLookInputMode : FInputModeGameOnly
			{ virtual bool ShouldFlushInputOnViewportFocus() const override { return false; } } Mode;
			Mode.SetConsumeCaptureMouseDown(false);
			SetInputMode(Mode);
		}
		bShowMouseCursor = false;
		if (HoverTooltipWidget)
		{
			HoverTooltipWidget->SetVisibility(ESlateVisibility::Collapsed);
		}
		int32 SizeX = 0, SizeY = 0;
		GetViewportSize(SizeX, SizeY);
		if (bEntering && SizeX > 0 && SizeY > 0)
		{
			SetMouseLocation(SizeX / 2, SizeY / 2);
		}
	}
	else
	{
		if (bMouseLookUsesCapture)
		{
			struct FLookReleaseMode : FInputModeGameAndUI
			{ virtual bool ShouldFlushInputOnViewportFocus() const override { return false; } } Mode;
			Mode.SetHideCursorDuringCapture(false).SetLockMouseToViewportBehavior(DesktopMouseLockMode());
			SetInputMode(Mode);
			bMouseLookUsesCapture = false;
		}
		bShowMouseCursor = true;
		EnsureRetailMouseCursor();
		SetMouseLocation(FMath::RoundToInt(MouseLookPressX), FMath::RoundToInt(MouseLookPressY));
	}
}

bool AACEPlayerController::UpdateMouseButtons(bool bRightDown, bool bLeftDown,
    bool bInputFocused, bool bOverUI)
{
    const bool bToggle=ACECameraSettings::GetToggleMouseLook() && !bInstantMouseLookHeld;
    if (!bToggle) bMouseLookToggled=false;
    // A focus/option change cancels the gesture. Re-enabling it while RMB is
    // still held must not recapture the viewport or turn the release into a click.
    if (bMouseLookActive && ((!IsUseMouseTurning() && !bInstantMouseLookHeld) || bInputFocused))
    {
        bRightClickEligible = false;
        bLeftOrbitEligible = false;
        SetMouseLookActive(false);
    }
    if (bLeftDown && !bLeftMouseWasDown)
        bLeftOrbitEligible = IsUseMouseTurning() && !bInputFocused && !bOverUI;
    if (bRightDown && !bRightMouseWasDown)
    {
        bRightClickEligible = !bInputFocused && (!bOverUI || bMouseLookActive);
        bMouseLookMovedPlayer = false;
        MouseLookTravelPixels = 0.f;
        if (bRightClickEligible && (IsUseMouseTurning() || bInstantMouseLookHeld))
        {
            if (bToggle) { bMouseLookToggled=!bMouseLookToggled; SetMouseLookActive(bMouseLookToggled); }
            else SetMouseLookActive(true);
        }
    }
    if (bInputFocused) bRightClickEligible = false;
    bMouseForwardActive = bMouseLookActive && bRightDown && bLeftDown;
    bMouseLookMovedPlayer |= bMouseForwardActive;
    const bool bIdentify = !bToggle && !bRightDown && bRightMouseWasDown && bRightClickEligible
        && !bMouseLookMovedPlayer && MouseLookTravelPixels <= 4.f
        && (bMouseLookActive || !bOverUI);
    if (!bMouseLookToggled && !bRightDown && !(bLeftDown && bLeftOrbitEligible))
    {
        SetMouseLookActive(false);
        bRightClickEligible = false;
        bMouseLookMovedPlayer = false;
    }
    bRightMouseWasDown = bRightDown;
    bLeftMouseWasDown = bLeftDown;
    if (!bLeftDown) bLeftOrbitEligible = false;
    return bIdentify;
}

void AACEPlayerController::UpdateMouseLook(float DeltaTime, USpringArmComponent* Boom)
{
    (void)DeltaTime;
    const bool bInputFocused = (DatGameplayBinder && DatGameplayBinder->IsChatEntryFocused())
        || (GameHUDWidget && GameHUDWidget->IsChatEntryFocused()) || ACEInputBindings::IsEditing() || ACEInputBindings::IsTextEntryFocused();
    const bool bWasInstant = bInstantMouseLookHeld;
    bInstantMouseLookHeld = !bInputFocused && ACEInputBindings::Down(this,ACEInputBindings::Action(TEXT("CameraInstantMouseLook")));
    const bool bRightDown = IsInputKeyDown(EKeys::RightMouseButton) || bInstantMouseLookHeld;
    const bool bLeftDown = IsInputKeyDown(EKeys::LeftMouseButton);
    float MX = 0.f, MY = 0.f;
    const bool bHaveMouse = GetMousePosition(MX, MY);
    if (bHaveMouse && !bMouseLookActive && ((bRightDown && !bRightMouseWasDown)
        || (bLeftDown && !bLeftMouseWasDown && !bRightDown)))
    {
        MouseLookPressX = MX;
        MouseLookPressY = MY;
    }
    else if (bRightMouseWasDown && !bMouseLookActive && bHaveMouse)
    {
        // With camera controls disabled RMB remains an ordinary inspect click.
        MouseLookTravelPixels = FMath::Max(MouseLookTravelPixels,
            FMath::Abs(MX - MouseLookPressX) + FMath::Abs(MY - MouseLookPressY));
    }
    if (UpdateMouseButtons(bRightDown, bLeftDown,
        bInputFocused, IsMouseOverBlockingUI() || (DatGameplayBinder
            && (DatGameplayBinder->IsInventoryInteractionActive() || DatGameplayBinder->IsSpellDragActive()
                || DatGameplayBinder->IsScrollbarDragActive()))))
    {
        if(!bWasInstant)IdentifyAtScreenPosition(MouseLookPressX, MouseLookPressY);
    }
    // Do not capture an ordinary left click: it still selects objects. Begin
    // free orbit only after a world press turns into a drag.
    if (DatGameplayBinder && DatGameplayBinder->IsInventoryInteractionActive()) bLeftOrbitEligible = false;
    if (!bMouseLookActive && bLeftDown && bLeftOrbitEligible && bHaveMouse
        && FMath::Abs(MX - MouseLookPressX) + FMath::Abs(MY - MouseLookPressY) > 4.f)
        SetMouseLookActive(true);
    if (!bMouseLookActive || !Boom) return;
    bShowMouseCursor = false;
    float DeltaX = 0.f, DeltaY = 0.f;
    GetInputMouseDelta(DeltaX, DeltaY);
    // Unreal's MouseY is up-positive; desktop cursor Y is down-positive.
    ApplyMouseLookDelta(DeltaX, -DeltaY, Boom);
}

void AACEPlayerController::ApplyMouseLookDelta(float DeltaX, float DeltaY, USpringArmComponent* Boom)
{
    if (!Boom || (!IsUseMouseTurning() && !bInstantMouseLookHeld) || !bMouseLookActive) return;
    MouseLookTravelPixels += FMath::Abs(DeltaX) + FMath::Abs(DeltaY);
    const float DegreesPerPixel = ACECameraSettings::GetMouseDegreesPerPixel();
    if (ACECameraSettings::GetInvertMouseX()) DeltaX=-DeltaX;
    if (ACECameraSettings::GetInvertMouseY()) DeltaY=-DeltaY;
    float YawDegrees = DeltaX * DegreesPerPixel;
    if (bInstantMouseLookHeld || bMouseLookToggled || bRightMouseWasDown)
    {
        // CameraSet::Rotate caps new mouse-facing input at the retail turn
        // speed. Do this before adding it to the camera's existing orbit so
        // excess mouse travel is discarded without losing a free-orbit view.
        const float Limit = FMath::RadiansToDegrees(ACECameraRetail::MouseTurnRadiansPerSecond)
            * FMath::Max(0.f, CameraInputFrameSeconds);
        YawDegrees = FMath::Clamp(YawDegrees, -Limit, Limit);
    }
    ApplyCameraOrbitDelta(Boom, YawDegrees, -DeltaY * DegreesPerPixel);
}

void AACEPlayerController::ApplyCameraOrbitDelta(USpringArmComponent* Boom, float YawDegrees, float PitchDegrees)
{
    if (!Boom || !FMath::IsFinite(YawDegrees) || !FMath::IsFinite(PitchDegrees)) return;
    // Retail CameraSet::Rotate leaves LookDown (including MapMode) before
    // rotating. Restore the ordinary arm and collision before applying input.
    if (bCameraLookDown && !FMath::IsNearlyZero(YawDegrees)) SetCameraLookDown(Boom, false);
    FRotator Rotation = Boom->GetRelativeRotation();
    if (bCameraLookDown)
    {
        // CameraSet::Raise/Lower translate the viewer offset without changing
        // its downward direction. Pitching the 450-unit map boom instead swings
        // the camera below the world with collision disabled.
        const float ScaleCm = GetCameraScaleCm();
        const float DistanceDelta = -PitchDegrees * ACECameraRetail::LookDownUnitsPerOrbitDegree * ScaleCm;
        const float RaiseLimit = ACECameraRetail::FartherMaxXYAc * ScaleCm;
        if (DistanceDelta < 0.f)
            Boom->TargetArmLength = FMath::Max(ACECameraRetail::CloserMinLengthAc * ScaleCm, Boom->TargetArmLength + DistanceDelta);
        else if (Boom->TargetArmLength < RaiseLimit)
            Boom->TargetArmLength = FMath::Min(RaiseLimit, Boom->TargetArmLength + DistanceDelta);
        UserCameraArmLength = Boom->TargetArmLength;
        Rotation.Pitch = ACECameraRetail::LookDownPitchDegrees();
        Rotation.Roll = 0.f;
        Boom->SetRelativeRotation(Rotation);
        return;
    }
    Rotation.Yaw += YawDegrees;
    Rotation.Pitch = FMath::Clamp(Rotation.Pitch + PitchDegrees,
        bCameraInHead ? -53.f : -89.f, bCameraInHead ? 53.f : 20.f);
    Rotation.Roll = 0.f;
    Boom->SetRelativeRotation(Rotation);
}

void AACEPlayerController::UpdateOrbitCamera(float DeltaTime)
{
	CameraInputFrameSeconds = FMath::Max(0.f, DeltaTime);
	APawn* P = GetPawn();
	USpringArmComponent* Boom = P ? P->FindComponentByClass<USpringArmComponent>() : nullptr;
	if (!Boom)
	{
		return;
	}

	Boom->CameraLagSpeed = ACECameraRetail::TranslationLagSpeed * ACERuntimeOptions::Get(TEXT("CameraStiffness"));
	Boom->CameraRotationLagSpeed = Boom->CameraLagSpeed;
	ApplyRetailCameraFov();
	if (UCapsuleComponent* Cap = P ? P->FindComponentByClass<UCapsuleComponent>() : nullptr)
	{
		ApplyRetailCameraPivot(Boom, Cap);
	}

	if (!bCameraDefaultsCaptured)
	{
		ResetCameraToRetailDefaults(Boom);
		PersonalCamera::Restore(Boom, UserCameraArmLength, GetCameraScaleCm()); // personal: camera
	}
	// Follow predicted cell ownership immediately, including walking/window
	// entry and portals while the server's position update is still in flight.
	if (bCameraMapMode && !ACECameraRetail::CanUseMapView(GetEffectiveCellId()))
		SetCameraMapMode(Boom, false);

	UpdateMouseLook(DeltaTime, Boom);

	const float bOrbitLeft = ACEInputBindings::Value(this, EKeys::NumPadFour);
	const float bOrbitRight = ACEInputBindings::Value(this, EKeys::NumPadSix);
	const float bOrbitUp = ACEInputBindings::Value(this, EKeys::NumPadEight);
	const float bOrbitDown = ACEInputBindings::Value(this, EKeys::NumPadTwo);
	const float OrbitStep = ACECameraRetail::NumpadOrbitDegreesPerSecond * DeltaTime
		* ACERuntimeOptions::Get(TEXT("CameraAdjustment"));
	const float PitchStep = bCameraLookDown
		? ACECameraRetail::NumpadOrbitDegreesPerSecond * DeltaTime
		: ACECameraRetail::KeyboardPitchRate(ACERuntimeOptions::Get(TEXT("CameraAdjustment")),IsUseMouseTurning()) * DeltaTime;
	ApplyCameraOrbitDelta(Boom, (bOrbitLeft - bOrbitRight) * OrbitStep,
		(bOrbitDown - bOrbitUp) * PitchStep);

	if (ACEInputBindings::Down(this, EKeys::Add)) StepCameraZoom(Boom, true, DeltaTime);
	else if (ACEInputBindings::Down(this, EKeys::Subtract)) StepCameraZoom(Boom, false, DeltaTime);
	if (!bCameraInHead)
	{
		SyncUserCameraArmLength(Boom, DeltaTime);
	}

	const bool bZeroDown = ACEInputBindings::Down(this, EKeys::NumPadZero);
	if (bZeroDown && !bNumPadZeroWasDown)
	{
		ResetCameraToRetailDefaults(Boom);
	}
	bNumPadZeroWasDown = bZeroDown;

	const bool bFiveDown = ACEInputBindings::Down(this, EKeys::NumPadFive);
	if (bFiveDown && !bNumPadFiveWasDown)
	{
		SetCameraInHead(Boom, !bCameraInHead);
	}
	bNumPadFiveWasDown = bFiveDown;

	const bool bThreeDown = ACEInputBindings::Down(this, EKeys::NumPadThree);
	if (bThreeDown && !bNumPadThreeWasDown)
	{
		SetCameraLookDown(Boom, !bCameraLookDown);
	}
	bNumPadThreeWasDown = bThreeDown;
	const bool bMapDown = ACEInputBindings::Down(this, ACEInputBindings::Action(TEXT("CameraViewMapMode")));
	if (bMapDown && !bCameraMapWasDown) SetCameraMapMode(Boom, !bCameraMapMode);
	bCameraMapWasDown = bMapDown;

	UpdateCombatTargetCameraAssist(DeltaTime, Boom);
	UpdateCameraSlope(Boom, DeltaTime);
	PersonalCamera::Update(Boom, UserCameraArmLength, GetCameraScaleCm(), bCameraInHead || bCameraLookDown || bCameraMapMode, DeltaTime); // personal: camera
}

void AACEPlayerController::UpdateCameraSlope(USpringArmComponent* Boom, float DeltaTime)
{
	auto* Orbit = Cast<UACEOrbitCameraBoom>(Boom);
	APawn* CameraPawn = GetPawn();
	if (!Orbit || !CameraPawn) return;
	Orbit->SlopePitch = 0.f;
	const FVector Position = CameraPawn->GetActorLocation();
	const FVector Delta = Position - CameraSlopeLastPosition;
	const bool Valid = bHaveCameraSlopePosition && DeltaTime > SMALL_NUMBER && Delta.SizeSquared() < FMath::Square(2000.);
	CameraSlopeLastPosition = Position; bHaveCameraSlopePosition = true;
	if (!Valid) CameraSlopeVelocities.Reset();
	CameraSlopeVelocities.Add(Valid ? Delta / DeltaTime : FVector::ZeroVector);
	if (CameraSlopeVelocities.Num()>5) CameraSlopeVelocities.RemoveAt(0,1,EAllowShrinking::No);
	FVector Velocity = FVector::ZeroVector; for (const auto& V : CameraSlopeVelocities) Velocity += V;
	Velocity /= CameraSlopeVelocities.Num();
	// CameraSet's ordinary rear/in-head views align to the contact plane;
	// overhead, front and side views retain their explicitly chosen orientation.
	if (IsVRActive() || bCameraLookDown || bCameraMapMode || ACERuntimeOptions::Get(TEXT("CameraAlignToSlope"))<.5f
		|| (!bCameraInHead && FMath::Abs(FMath::UnwindDegrees(Boom->GetRelativeRotation().Yaw-ACECameraRetail::BoomYawToFaceAcForward))>22.f)
		|| Velocity.SizeSquared2D()<.01f || !GetWorld()->GetPhysicsScene()) return;
	FVector Normal = FVector::UpVector;
	if (bJumpAirborne)
	{
		Velocity.Z *= .1f;
		Normal = FRotationMatrix::MakeFromX(Velocity).GetUnitAxis(EAxis::Z);
	}
	else if (const auto* Capsule=CameraPawn->FindComponentByClass<UCapsuleComponent>())
	{
		const FVector Feet = Position-FVector(0,0,Capsule->GetScaledCapsuleHalfHeight());
		FCollisionQueryParams Query(SCENE_QUERY_STAT(ACECameraSlope),false,CameraPawn);
		TArray<FHitResult> Hits;
		if (ACEBodySweep::TraceGround(*GetWorld(),Hits,Feet+FVector(0,0,40),Feet-FVector(0,0,80),Query))
			for (const auto& Hit : Hits) if (Hit.bBlockingHit && Hit.ImpactNormal.Z>.664f && !ACEBodySweep::IsCreatureBody(Hit))
			{ Normal=Hit.ImpactNormal; break; }
	}
	// AC's forward axis is local +Y, rather than Unreal's +X.
	Orbit->SlopePitch=FVector::VectorPlaneProject(CameraPawn->GetActorRightVector(),Normal).Rotation().Pitch;
}

void AACEPlayerController::SyncUserCameraArmLength(USpringArmComponent* Boom, float DeltaSeconds)
{
	if (!Boom || bCameraInHead || bCameraMapMode)
	{
		return;
	}
	if (UserCameraArmLength < 0.f)
	{
		UserCameraArmLength = Boom->TargetArmLength;
	}
	const float ScaleCm = GetCameraScaleCm();
	const float MinThirdPerson = ACECameraRetail::CloserMinLengthAc * ScaleCm;
	// CameraSet::Raise/Lower rotate the existing offset without changing its
	// length. The per-axis Farther limits apply only to a requested zoom step;
	// reapplying them here silently zoomed in when lowering a distant camera.
	UserCameraArmLength = FMath::Max(UserCameraArmLength, MinThirdPerson);
	// SpringArm camera lag smooths the pivot, but not changes in arm length.
	// Retail CameraManager interpolates the viewer origin too, so blend zoom
	// with its translation stiffness before SpringArm performs obstruction tests.
	const float Alpha = DeltaSeconds < 0.f ? 1.f
		: ACECameraRetail::TranslationAlpha(DeltaSeconds, ACERuntimeOptions::Get(TEXT("CameraStiffness")));
	Boom->TargetArmLength = FMath::Lerp(Boom->TargetArmLength, UserCameraArmLength, Alpha);
}

void AACEPlayerController::StepCameraZoom(USpringArmComponent* Boom, bool bCloser, float DeltaSeconds, float Steps)
{
	if (!Boom || DeltaSeconds < .0002f || Steps <= 0.f) return;
	if (bCameraLookDown) SetCameraLookDown(Boom, false);
	if (bCameraInHead)
	{
		if (!bCloser) SetCameraInHead(Boom, false);
		return;
	}
	if (UserCameraArmLength < 0.f) UserCameraArmLength = Boom->TargetArmLength;
	const float ScaleCm = GetCameraScaleCm();
	// Wheel detents take the same elapsed-frame step as retail's Closer/Farther
	// input events. Preserve individual steps when the platform coalesces them.
	for (float Left=FMath::Min(Steps,120.f); Left>0.f; Left-=1.f)
	{
		const float Next = UserCameraArmLength * ACECameraRetail::ZoomMultiplier(bCloser, DeltaSeconds,
			ACERuntimeOptions::Get(TEXT("CameraAdjustment")), FMath::Min(Left,1.f));
		// Retail rejects offsets outside the bounds instead of changing modes or
		// flattening the camera pitch to make them fit. Numpad 5 owns first person.
		if (Next < ACECameraRetail::CloserMinLengthAc*ScaleCm
			|| (!bCloser && Next > PersonalCamera::MaxArmLength(ScaleCm))) break; // personal: camera
		UserCameraArmLength=Next;
	}
}

bool AACEPlayerController::IsUseMouseTurning() const
{
	return Client && Client->IsCharacterOptionSet(0x31);
}

void AACEPlayerController::ApplyCameraWheelZoom(float WheelDelta, bool bForwardHotkeys)
{
	if (FMath::IsNearlyZero(WheelDelta) || IsMouseOverBlockingUI()
		|| ACEInputBindings::IsEditing() || ACEInputBindings::IsTextEntryFocused()
		|| (DatGameplayBinder && DatGameplayBinder->IsChatEntryFocused())
		|| (GameHUDWidget && GameHUDWidget->IsChatEntryFocused()))
	{
		return;
	}
    const auto Mods=FSlateApplication::Get().GetModifierKeys();
    const FInputChord Chord(WheelDelta>0?EKeys::MouseScrollUp:EKeys::MouseScrollDown,Mods.IsShiftDown(),Mods.IsControlDown(),Mods.IsAltDown(),Mods.IsCommandDown());
    if(ACEInputBindings::Matches(EKeys::Add,Chord,this))AdjustMouseCameraDistance(FMath::Abs(WheelDelta));
    else if(ACEInputBindings::Matches(EKeys::Subtract,Chord,this))AdjustMouseCameraDistance(-FMath::Abs(WheelDelta));
    else if(bForwardHotkeys)
    {
        // The visible canvas consumes this Slate wheel event. Deliver its pulse
        // to normal action polling; the hidden-HUD viewport already supplies
        // this key pair itself. Never synthesize a held wheel key or zoom twice.
        InputKey(FInputKeyEventArgs::CreateSimulated(Chord.Key,IE_Pressed,1.f));
        InputKey(FInputKeyEventArgs::CreateSimulated(Chord.Key,IE_Released,0.f));
    }
}

void AACEPlayerController::AdjustMouseCameraDistance(float WheelDelta)
{
    if (!FMath::IsFinite(WheelDelta) || FMath::IsNearlyZero(WheelDelta)) return;
	APawn* P = GetPawn();
	USpringArmComponent* Boom = P ? P->FindComponentByClass<USpringArmComponent>() : nullptr;
	if (!Boom)
	{
		return;
	}
	StepCameraZoom(Boom, WheelDelta>0.f, CameraInputFrameSeconds, FMath::Abs(WheelDelta));
}

void AACEPlayerController::UpdateCombatTargetCameraAssist(float DeltaTime, USpringArmComponent* Boom)
{
	if (!Client || !Boom || bMouseLookActive || bCameraInHead || bCameraLookDown)
	{
		return;
	}
	if (!Client->IsCharacterOptionSet(0x07))
	{
		return;
	}
	const FACESelectedObject Sel = Client->GetSelectedObject();
	if (Sel.Guid == 0)
	{
		return;
	}
	FACEWorldObject Target;
	if (!Client->GetWorldObject(Sel.Guid, Target) || !Target.IsAttackable() || !Target.bHasPosition)
	{
		return;
	}
	const FACEPosition PlayerPos = bHavePredictedPose ? PredictedPose : Client->GetPlayerPosition();
	if (!PlayerPos.IsValid())
	{
		return;
	}
	const FVector PlayerUe = PlayerPos.ToUnrealLocation(WorldScale);
	const FVector TargetUe = Target.Position.ToUnrealLocation(WorldScale);
	const FVector ToTarget = (TargetUe - PlayerUe).GetSafeNormal2D();
	if (ToTarget.IsNearlyZero())
	{
		return;
	}
	const FVector CharForward = PlayerPos.GetAceForwardVector().GetSafeNormal2D();
	const FVector CharRight = FVector::CrossProduct(FVector::UpVector, CharForward).GetSafeNormal();
	const float TargetOrbitYaw = FMath::RadiansToDegrees(FMath::Atan2(
		FVector::DotProduct(ToTarget, CharRight),
		FVector::DotProduct(ToTarget, CharForward)));
	FRotator RelRot = Boom->GetRelativeRotation();
	const float CurrentOrbitYaw = RelRot.Yaw - ACECameraRetail::BoomYawToFaceAcForward;
	const float Error = FMath::FindDeltaAngleDegrees(CurrentOrbitYaw, TargetOrbitYaw);
	if (FMath::Abs(Error) <= 1.5f)
	{
		return;
	}
	RelRot.Yaw += FMath::Clamp(Error, -75.f * DeltaTime, 75.f * DeltaTime);
	RelRot.Roll = 0.f;
	Boom->SetRelativeRotation(RelRot);
}

void AACEPlayerController::MoveForward(float Value) { ForwardAxis = Value; }
void AACEPlayerController::MoveRight(float Value) { RightAxis = Value; }
void AACEPlayerController::Turn(float Value) { TurnAxis = Value; }
void AACEPlayerController::JumpPressed() { BeginJumpCharge(); }
void AACEPlayerController::JumpReleased()
{
	if (bJumpCharging)
	{
		// Live keys at release (standing charge zeros loco axes each tick).
		float AimF = 0.f, AimR = 0.f;
		if (ACEInputBindings::Down(this, EKeys::W)) AimF += 1.f;
		if (ACEInputBindings::Down(this, EKeys::S)) AimF -= 1.f;
		if (ACEInputBindings::Down(this, EKeys::E)) AimR += 1.f;
		if (ACEInputBindings::Down(this, EKeys::Q)) AimR -= 1.f;
		if (bStandingJumpLocked)
		{
			AimF = StandingJumpAimF;
			AimR = StandingJumpAimR;
			if (ACEInputBindings::Down(this, EKeys::W)) AimF = 1.f;
			if (ACEInputBindings::Down(this, EKeys::S)) AimF = -1.f;
			if (ACEInputBindings::Down(this, EKeys::E)) AimR = 1.f;
			if (ACEInputBindings::Down(this, EKeys::Q)) AimR = -1.f;
		}
		ReleaseJump(AimF, AimR);
	}
}
void AACEPlayerController::SprintPressed() { bRunning = false; } // Shift = walk
void AACEPlayerController::SprintReleased() { bRunning = true; }

bool AACEPlayerController::IsJumpBlockedByMotion(bool bCharging) const
{
	if (Client && Client->GetSession())
	{
		const uint32 Command = Client->GetSession()->GetCurrentForwardCommand();
		if (bCharging ? ACEMotion::BlocksJumpCharge(Command) : ACEMotion::BlocksJumpRelease(Command)) return true;
	}
	const APawn* P = GetPawn();
	const auto* App = P ? P->FindComponentByClass<UACECharacterAppearanceComponent>() : nullptr;
	return App && App->IsJumpBlockedByMotion(bCharging);
}

void AACEPlayerController::BeginJumpCharge()
{
	if (!Client || Client->GetSessionState() != EACESessionState::InWorld || bJumpCharging)
	{
		return;
	}
	// CACQualities::CanJump is checked both when charging and at release.
	// GetJumpHeight's minimum height is not permission to jump at 200% load.
	if (Client->GetMovementBurden() >= 2.f)
	{
		Client->ReceiveChatMessage(TEXT("You are too encumbered to jump!"), FString(), ACEChatMessageType::ChatError);
		return;
	}
	if (IsJumpBlockedByMotion(true))
	{
		Client->ReceiveChatMessage(TEXT("You can't jump from this position"), FString(), ACEChatMessageType::ChatError);
		return;
	}
	bJumpCharging = true;
	bJumpChargeSent = false; // force MoveToState with StandingLongJump
	// charge_jump preserves the current forward substate. It neither stands a
	// resting player up nor removes an emote while building the jump power bar.
	JumpChargeExtent = 0.f;
	StandingJumpAimF = 0.f;
	StandingJumpAimR = 0.f;
	// Capture at charge start — pressing WASD mid-charge must not convert a standing jump
	// into a run jump (retail StandingLongJump ignores loco until leave-ground).
	// Retail charge_jump allows charging in flight; standing_longjump is only
	// entered on walkable contact. Never change the current ballistic arc.
	if (!bJumpAirborne)
		bStandingJumpLocked = Client->GetSession()->GetCurrentForwardCommand() == ACEMotion::Ready
			&& FMath::IsNearlyZero(ForwardAxis) && FMath::IsNearlyZero(RightAxis) && FMath::IsNearlyZero(TurnAxis);
	if (DatGameplayBinder)
	{
		DatGameplayBinder->SetJumpChargeFraction(0.f);
	}
}

void AACEPlayerController::ReleaseJump(float Forward, float Right)
{
	if (bJumpAirborne)
	{
		// jump_is_allowed requires contact. Holding through touchdown retains
		// the charge; releasing before touchdown cannot launch a second arc.
		bJumpCharging = false;
		JumpChargeExtent = 0.f;
		if (DatGameplayBinder) DatGameplayBinder->SetJumpChargeFraction(0.f);
		return;
	}
	if (!Client || !bJumpCharging)
	{
		bJumpCharging = false;
		bStandingJumpLocked = false;
		StandingJumpAimF = 0.f;
		StandingJumpAimR = 0.f;
		if (DatGameplayBinder)
		{
			DatGameplayBinder->SetJumpChargeFraction(0.f);
		}
		return;
	}
	bJumpCharging = false;
	bJumpChargeSent = false;
	const float Extent = FMath::Clamp(JumpChargeExtent, 0.f, 1.f);
	JumpChargeExtent = 0.f;
	if (DatGameplayBinder)
	{
		DatGameplayBinder->SetJumpChargeFraction(0.f);
	}

	if (Client->GetMovementBurden() >= 2.f)
	{
		bStandingJumpLocked = false;
		StandingJumpAimF = StandingJumpAimR = 0.f;
		Client->ReceiveChatMessage(TEXT("You are too encumbered to jump!"), FString(), ACEChatMessageType::ChatError);
		return;
	}

	if (IsJumpBlockedByMotion(false))
	{
		bStandingJumpLocked = false;
		StandingJumpAimF = StandingJumpAimR = 0.f;
		Client->ReceiveChatMessage(TEXT("You can't jump from this position"), FString(), ACEChatMessageType::ChatError);
		return;
	}

	// Standing jump: no aim → vertical only. W/S/Q/E at release with run (no Shift) uses
	// run-scale forward jump — retail standing charge + W is a full forward jump, not walk/shift.
	const bool bStandingAim = bStandingJumpLocked
		&& ((FMath::IsNearlyZero(Forward) && FMath::IsNearlyZero(Right)) || !bRunning);

	// MotionInterp.get_state_velocity — local ACE space (X=strafe, Y=forward, Z=up).
	constexpr float WalkAnimSpeed = 3.12f;
	constexpr float RunAnimSpeed = 4.f;
	FVector LocalVel = FVector::ZeroVector;
	if (!FMath::IsNearlyZero(Right))
	{
		LocalVel.X = Client->GetSidestepSpeed(bStandingAim ? false : bRunning)
			* FMath::Clamp(Right, -1.f, 1.f);
	}
	if (Forward > KINDA_SMALL_NUMBER)
	{
		LocalVel.Y = (bStandingAim || !bRunning)
			? (WalkAnimSpeed * FMath::Clamp(Forward, 0.f, 1.f))
			: (RunAnimSpeed * Client->GetRunRate() * FMath::Clamp(Forward, 0.f, 1.f));
	}
	else if (Forward < -KINDA_SMALL_NUMBER)
	{
		// Retail adjust_motion applies Run to negative WalkForward too; the
		// jump inherits that interpreted velocity just like grounded movement.
		LocalVel.Y = -WalkAnimSpeed * 0.65f * FMath::Clamp(-Forward, 0.f, 1.f)
			* (!bStandingAim && bRunning ? Client->GetRunRate() : 1.f);
	}
	const float MaxSpeed = bStandingAim
		? WalkAnimSpeed
		: (RunAnimSpeed * Client->GetRunRate());
	if (LocalVel.SizeSquared2D() > FMath::Square(MaxSpeed) && MaxSpeed > KINDA_SMALL_NUMBER)
	{
		LocalVel = LocalVel.GetSafeNormal2D() * MaxSpeed;
	}
	const float Height = Client->GetJumpHeight(Extent);
	if (IsVRActive() && Client->GetSession() && Client->GetSession()->SupportsUniformVRMovement())
		LocalVel = ACEVRLocomotion::JumpVelocity(Forward, Right, !bStandingAim && bRunning, Client->GetRunRate());
	LocalVel.Z = FMath::Sqrt(FMath::Max(0.f, Height * 19.6f));
	JumpLocalAceVelocity = LocalVel;
	LandingWorldAceVelocity = FVector::ZeroVector;
	LandingRootTrack.Reset();

	FACEPosition LaunchPose;
	if (bHavePredictedPose)
	{
		LaunchPose = PredictedPose;
	}
	else
	{
		LaunchPose = Client->GetPlayerPosition();
	}
	// Freeze world-ACE velocity for the full arc (MotionInterp leave-ground).
	JumpWorldAceVelocity = LaunchPose.IsValid()
		? LaunchPose.GetAcQuat().RotateVector(LocalVel)
		: LocalVel;
	if (LaunchPose.IsValid())
	{
		JumpLaunchAcQuat = LaunchPose.GetAcQuat();
		bHaveJumpLaunchFacing = true;
	}
	else
	{
		bHaveJumpLaunchFacing = false;
	}

	Client->SendJump(Extent, LocalVel);
	// Leave-ground MoveToState carries Contact=0. Retail leaves the airborne
	// arc to JumpPack physics, resuming autonomous position reports on landing.
	Client->SendMovementEx(0.f, 0.f, 0.f, bRunning, false, false);
	Client->SetForcePositionReporting(true);
	bJumpAirborne = true;
	bJumpAirborneSent = true;
	JumpAirborneSeconds = 0.f;
	bLocalPredicting = true;
	bHavePredictedPose = true;
	if (LaunchPose.IsValid())
	{
		PredictedPose = LaunchPose;
	}

	if (APawn* P = GetPawn())
	{
		if (UACECharacterAppearanceComponent* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			// CMotionInterp::LeaveGround removes old links and applies Falling.
			// Jumpup is not a player takeoff link: queuing a held Falling after it
			// skipped the authored airborne transition entirely, especially on re-jump.
			App->ClearJumpMotionIfAny();
			App->SetSuppressLocoIdleBlend(true);
			App->PlayActionMotion(static_cast<int32>(0x40000015), 1.f, 0);
		}
	}
}

void AACEPlayerController::ShowGameHUD()
{
	if (!IsLocalController())
	{
		return;
	}

	DestroyCharacterSelectUI();

	if (bUseDatDrivenHud)
	{
		if (!Client) return; // Wait for initialization; never substitute the development HUD.
		// Portal transit retains the HUD. Reinitializing its binder here resets
		// the selected panel, inventory position, and chat buffers.
		if (DatCanvasWidget && DatGameplayBinder)
		{
			if (!IsVRActive() && !DatCanvasWidget->IsInViewport()) DatCanvasWidget->AddToPlayerScreen(9000);
			DatCanvasWidget->SetVisibility(IsDesktopInterfaceHidden() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
			EnsureRetailMouseCursor();
			return;
		}
		if (!DatCanvasWidget)
		{
			DatCanvasWidget = CreateWidget<UACEUICanvasWidget>(this, UACEUICanvasWidget::StaticClass());
			if (DatCanvasWidget)
			{
				DatCanvasWidget->InitializeCanvas(Client->GetUIElementManager());
				DatCanvasWidget->SetResourceResolver(Client->GetUIResourceResolver());
			}
		}
		bool bDatHudReady = false;
		if (DatCanvasWidget && Client->GetUIFlow())
		{
			Client->GetUIFlow()->SetMode(ACEUI::EACEUIFlowMode::Gameplay);
			// SetMode loads on a mode change. When portal prefetch already loaded
			// gameplay, reuse that tree rather than rebuilding it at reveal.
			if (UACEUIElementManager* UiMgr = Client->GetUIElementManager())
			{
				const TSharedPtr<FACEUIElement> Root = UiMgr->GetSyntheticRoot();
				bDatHudReady = Root.IsValid() && Root->Children.Num() > 0;
			}
		}
		if (bDatHudReady)
		{
			DatCanvasWidget->TakeWidget();
			if (!DatGameplayBinder)
			{
				DatGameplayBinder = NewObject<UACEUIGameplayBinder>(this);
			}
			if (DatGameplayBinder)
			{
				DatGameplayBinder->Initialize(Client, Client->GetUIElementManager(), DatCanvasWidget, this);
				DatCanvasWidget->SetGameplayBinder(DatGameplayBinder);
			}
			if (!IsVRActive() && !DatCanvasWidget->IsInViewport())
			{
				DatCanvasWidget->AddToPlayerScreen(9000);
			}
			DatCanvasWidget->SetVisibility(IsDesktopInterfaceHidden() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
			if (GameHUDWidget)
			{
				GameHUDWidget->SetVisibility(ESlateVisibility::Collapsed);
			}
			EnsureRetailMouseCursor();
			return;
		}
		// A failed retail layout must not silently substitute a different UI.
		FailWorldEntry(TEXT("Cannot load the retail gameplay interface. Check Game files and repair or reinstall the client, then log in again."));
		return;
	}

	if (!GameHUDWidget)
	{
		TSubclassOf<UACEGameHUDWidget> HudClass = GameHUDWidgetClass;
		if (!HudClass)
		{
			HudClass = UACEGameHUDWidget::StaticClass();
		}
		GameHUDWidget = CreateWidget<UACEGameHUDWidget>(this, HudClass);
	}
	if (GameHUDWidget && !GameHUDWidget->IsInViewport())
	{
		// Below the hover tooltip (10050), above the world.
		GameHUDWidget->AddToPlayerScreen(9000);
	}
	if (GameHUDWidget)
	{
		// Containers pass through; buttons / chat entry stay clickable.
		GameHUDWidget->SetVisibility(IsDesktopInterfaceHidden() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
	if (DatCanvasWidget)
	{
		DatCanvasWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
	EnsureRetailMouseCursor();
}

void AACEPlayerController::EnableGameplayViewportShadows()
{
	if (ULocalPlayer* LP = GetLocalPlayer())
	{
		if (LP->ViewportClient)
		{
			LP->ViewportClient->EngineShowFlags.SetDynamicShadows(true);
			LP->ViewportClient->EngineShowFlags.SetLighting(true);
		}
	}
}

void AACEPlayerController::EnsureRetailMouseCursor()
{
	if (IsVRActive() || IsMobileActive()) { bShowMouseCursor = false; return; }
	const float CursorScale = ACERuntimeOptions::Get(TEXT("CursorScale"));
	if (MouseCursorWidget) MouseCursorWidget->SetCursorScale(CursorScale);
	for (const auto& Cursor : RetailWindowCursorWidgets) if (Cursor) Cursor->SetCursorScale(CursorScale);
	if (!IsLocalController() || bRetailCursorInstalled)
	{
		return;
	}
	FACEDatTextureResolver* Resolver = nullptr;
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			if (!Dat->IsDatReady())
			{
				Dat->BeginBackgroundLoad();
				return;
			}
			Resolver = Dat->GetTextureResolver();
		}
	}
	if (!Resolver)
	{
		return;
	}

	RetailCursorDefaultTex = Resolver->GetOrCreateUiTexture(UACEMouseCursorWidget::DefaultCursorDid);
	if (auto* Resources = Client ? Client->GetUIResourceResolver() : nullptr)
	{
		RetailCursorBusyTex = Resources->ResolveTexture(Resources->ResolveCursorId(14));
		for (int32 Cursor : {3,4,5,6,10,11,12,13,15})
			RetailActionCursorTextures.Add(Cursor,Resources->ResolveTexture(Resources->ResolveCursorId(Cursor)));
	}
	RetailCursorInteractableTex = Resolver->GetOrCreateUiTexture(UACEMouseCursorWidget::InteractableCursorDid);
	RetailCursorTargetTex = Resolver->GetOrCreateUiTexture(UACEMouseCursorWidget::TargetCursorDid);
	RetailCursorTargetValidTex = Resolver->GetOrCreateUiTexture(UACEMouseCursorWidget::TargetValidCursorDid);
	RetailCursorTargetInvalidTex = Resolver->GetOrCreateUiTexture(UACEMouseCursorWidget::TargetInvalidCursorDid);
	if (!RetailCursorDefaultTex)
	{
		return;
	}
	if (!MouseCursorWidget)
	{
		MouseCursorWidget = CreateWidget<UACEMouseCursorWidget>(this, UACEMouseCursorWidget::StaticClass());
	}
	if (!MouseCursorWidget)
	{
		return;
	}

	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;
	MouseCursorWidget->SetCursorScale(CursorScale);
	CurrentMouseCursor = EMouseCursor::Default;
	SetMouseCursorWidget(EMouseCursor::Default, MouseCursorWidget);
	// Let Slate's cursor query select the native window affordance independently
	// of the world-selection cursor, including while a drag retains capture.
	const TPair<EMouseCursor::Type, uint32> WindowCursors[] = {
		{EMouseCursor::CardinalCross, UACEMouseCursorWidget::MoveCursorDid},
		{EMouseCursor::ResizeUpDown, UACEMouseCursorWidget::ResizeVerticalCursorDid},
		{EMouseCursor::ResizeLeftRight, UACEMouseCursorWidget::ResizeHorizontalCursorDid},
		{EMouseCursor::ResizeSouthEast, UACEMouseCursorWidget::ResizeNWSECursorDid},
		{EMouseCursor::ResizeSouthWest, UACEMouseCursorWidget::ResizeNESWCursorDid}
	};
	for (const auto& Cursor : WindowCursors)
	{
		if (auto* Texture = Resolver->GetOrCreateUiTexture(Cursor.Value))
		{
			auto* Widget = CreateWidget<UACEMouseCursorWidget>(this, UACEMouseCursorWidget::StaticClass());
			if (!Widget) continue;
			Widget->SetCursorScale(CursorScale);
			Widget->SetCursorTexture(Texture, UACEMouseCursorWidget::WindowHotspot, UACEMouseCursorWidget::WindowHotspot);
			SetMouseCursorWidget(Cursor.Key, Widget);
			RetailWindowCursorWidgets.Add(Widget);
		}
	}
	bRetailCursorInstalled = true;
	bRetailCursorInteractable = false;
	ApplyRetailMouseCursor(false);
}

void AACEPlayerController::ApplyRetailMouseCursor(bool bInteractable, bool bCompatible)
{
	if (!MouseCursorWidget)
	{
		return;
	}
	const auto Session=Client?Client->GetSession():nullptr;
	const bool Busy=IsWorldTransitionActive() || (Session&&Session->IsCursorBusy());
	const int32 Mode=Client&&Client->GetSessionState()==EACESessionState::InWorld ? Client->GetPlayerVitalsView().CombatMode : 1;
	const int32 TargetMode=Client&&Client->GetSessionState()==EACESessionState::InWorld ? (bPendingUseTargeting ? 3 : ObjectCursorTargetMode) : 0;
	const int32 Cursor=UACEMouseCursorWidget::RetailCursorEnum(Busy,TargetMode,Mode,bInteractable,bCompatible);
	UTexture2D* Tex=nullptr;
	switch (Cursor)
	{
	case 1: Tex=RetailCursorDefaultTex;break;
	case 2: Tex=RetailCursorInteractableTex;break;
	case 14: Tex=RetailCursorBusyTex;break;
	case 39: Tex=RetailCursorTargetTex;break;
	case 40: Tex=RetailCursorTargetValidTex;break;
	case 41: Tex=RetailCursorTargetInvalidTex;break;
	default: Tex=RetailActionCursorTextures.FindRef(Cursor);break;
	}
	if (!Tex)
	{
		return;
	}
	const int32 Hotspot=!Busy&&(TargetMode==1||TargetMode==3)?UACEMouseCursorWidget::TargetHotspot:0;
	MouseCursorWidget->SetCursorTexture(Tex,Hotspot,Hotspot);
	bRetailCursorInteractable = bInteractable;
}

void AACEPlayerController::SetPendingUseTargeting(bool bPending)
{
	bPendingUseTargeting = bPending;
	if (bPending) ObjectCursorTargetMode=0;
	if (bRetailCursorInstalled)
	{
		ApplyRetailMouseCursor(false);
	}
}

void AACEPlayerController::SetObjectCursorTargeting(int32 Mode)
{
	ObjectCursorTargetMode=Mode==1||Mode==2 ? Mode : 0;
	if (ObjectCursorTargetMode) bPendingUseTargeting=false;
	if (bRetailCursorInstalled) ApplyRetailMouseCursor(false);
}

void AACEPlayerController::SetDesktopInterfaceHidden(bool bHideInterface)
{
	if (IsVRActive() || !Client || Client->GetSessionState()!=EACESessionState::InWorld) return;
	bDesktopInterfaceHidden=bHideInterface;
	if (bHideInterface)
	{
		if (DatGameplayBinder)
		{
			DatGameplayBinder->CancelPointerGestures();
			DatGameplayBinder->CancelPendingChatRefocus();
		}
		HideGameHUD();
		bHoverTooltipVisible=false;
		HoverTooltipText.Reset();
		if (HoverTooltipWidget) HoverTooltipWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
	else
	{
		// Restore the existing canvas, keeping open windows, positions and chat.
		if (DatCanvasWidget && DatGameplayBinder) DatCanvasWidget->SetVisibility(ESlateVisibility::Visible);
		else if (GameHUDWidget) GameHUDWidget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
	UpdateFrameRateOverlay();
}

void AACEPlayerController::HideGameHUD()
{
	if (GameHUDWidget)
	{
		// The user's interface toggle preserves chat history and panel state.
		GameHUDWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (DatCanvasWidget)
	{
		DatCanvasWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void AACEPlayerController::DestroyGameHUD()
{
	bDesktopInterfaceHidden=false;
	DestroyCharacterSelectUI();
	if (DatGameplayBinder)
	{
		DatGameplayBinder->Shutdown();
		DatGameplayBinder = nullptr;
	}
	if (GameHUDWidget)
	{
		GameHUDWidget->RemoveFromParent();
		GameHUDWidget = nullptr;
	}
	if (DatCanvasWidget)
	{
		DatCanvasWidget->SetGameplayBinder(nullptr);
		DatCanvasWidget->SetCharSelectBinder(nullptr);
		DatCanvasWidget->RemoveFromParent();
		DatCanvasWidget = nullptr;
	}
}

void AACEPlayerController::DestroyCharacterSelectUI()
{
	if(DatCharGenBinder){DatCharGenBinder->Shutdown();DatCharGenBinder=nullptr;}
	if(DatCanvasWidget)DatCanvasWidget->SetCharGenBinder(nullptr);
	bPendingCharSelect = false;
	PendingCharSelectCharacters.Reset();
	PendingCharSelectServerName.Reset();
	if (DatCharSelectBinder)
	{
		DatCharSelectBinder->Shutdown();
		DatCharSelectBinder = nullptr;
	}
	if (DatCanvasWidget)
	{
		DatCanvasWidget->SetCharSelectBinder(nullptr);
		// Tear down the canvas when it only hosted character select — leftover overlay
		// text must not paint over the world / portal tunnel.
		if (!DatGameplayBinder)
		{
			DatCanvasWidget->RemoveFromParent();
			DatCanvasWidget = nullptr;
			if (Client && Client->GetUIElementManager())
			{
				Client->GetUIElementManager()->ClearRoots();
			}
			if (Client && Client->GetUIFlow())
			{
				Client->GetUIFlow()->SetMode(ACEUI::EACEUIFlowMode::None);
			}
		}
	}
}

void AACEPlayerController::CollectLayoutTextureIds(const TSharedPtr<FACEUIElement>& Node, TArray<uint32>& OutIds)
{
	if (!Node.IsValid())
	{
		return;
	}
	if (Node->ImageFileId != 0)
	{
		OutIds.AddUnique(Node->ImageFileId);
	}
	if (Node->AlphaFileId != 0)
	{
		OutIds.AddUnique(Node->AlphaFileId);
	}
	for (const TSharedPtr<FACEUIElement>& Child : Node->Children)
	{
		CollectLayoutTextureIds(Child, OutIds);
	}
}

bool AACEPlayerController::TryPrefetchCharacterSelectAssets()
{
	CharacterSelectLoadError.Reset();
	if (!Client)
	{
		return false;
	}
	UGameInstance* GI = GetGameInstance();
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat || !Dat->IsDatReady())
	{
		if (Dat)
		{
			Dat->BeginBackgroundLoad();
			if (!Dat->GetDatLoadError().IsEmpty())
				CharacterSelectLoadError = FString::Printf(TEXT("Cannot load character selection: %s. Check your retail DAT installation, then click Login to retry."), *Dat->GetDatLoadError());
		}
		return false;
	}

	UACEUIResourceResolver* Res = Client->GetUIResourceResolver();
	if (!Res)
	{
		CharacterSelectLoadError = TEXT("Cannot load character selection: UI resources are unavailable.");
		return false;
	}

	if (Client->GetUIFlow())
	{
		Client->GetUIFlow()->SetMode(ACEUI::EACEUIFlowMode::CharacterManagement);
	}
	if (UACEUILayoutResolver* Layout = Client->GetUILayoutResolver())
	{
		if (!Layout->LoadLayout(ACEUI::LayoutId::CharacterManagement))
		{
			CharacterSelectLoadError = TEXT("Cannot load character selection: packaged UI layouts are missing or unreadable. Extract the complete Windows package, then restart the game.");
			return false;
		}
	}

	// Critical chrome — backdrop + Enter + primary buttons (must not show blank first).
	static const uint32 CriticalDids[] = {
		0x06007576u, // CharacterManagementField JPEG backdrop
		0x06004CB2u, 0x06004CB1u, // Enter idle + alpha
		0x06004C9Eu, 0x06004C9Du, // red large idle + alpha
		0x06004CA3u, 0x06004CA2u, // red huge idle + alpha
		0x06004CC3u, 0x06004CC4u, 0x06004CC5u, 0x06004CC6u, 0x06004CC7u, 0x06004CB8u, // list frame
		0x06005EA7u, 0x06005EA9u, 0x06005EABu, // character slot idle
	};
	int32 Resolved = 0;
	for (uint32 Did : CriticalDids)
	{
		if (Res->ResolveTexture(Did))
		{
			++Resolved;
		}
	}
	// Backdrop + Enter are mandatory; allow a couple of optional chrome misses.
	const bool bBackdrop = Res->ResolveTexture(0x06007576u) != nullptr;
	const bool bEnter = Res->ResolveTexture(0x06004CB2u, 0x06004CB1u) != nullptr
		|| Res->ResolveTexture(0x06004CB2u) != nullptr;
	if (!bBackdrop || !bEnter || Resolved < 8)
	{
		CharacterSelectLoadError = TEXT("Cannot load character selection: required retail DAT images are missing or unreadable. Check client_portal.dat, then click Login to retry.");
		return false;
	}

	// Warm fonts used by char-select overlays so names don't pop in a frame later.
	Res->RenderDatText(0x4000000Bu, TEXT("World"), FColor(216, 175, 91));
	Res->RenderDatText(0x40000009u, TEXT("Character"), FColor::White);
	Res->RenderDatText(0x40000012u, TEXT("ENTER"), FColor::White, true);
	return true;
}

void AACEPlayerController::TickPendingCharacterSelect()
{
	if (!bPendingCharSelect || !IsLocalController())
	{
		return;
	}
	if (!TryPrefetchCharacterSelectAssets())
	{
		if (LoginWidget)
		{
			LoginWidget->SetVisibility(ESlateVisibility::Visible);
			LoginWidget->SetStatus(CharacterSelectLoadError.IsEmpty()
				? TEXT("Loading character select...") : CharacterSelectLoadError);
		}
		if (!CharacterSelectLoadError.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("ACE: %s"), *CharacterSelectLoadError);
			bPendingCharSelect = false;
		}
		return;
	}

	const TArray<FACECharacterInfo> Characters = MoveTemp(PendingCharSelectCharacters);
	const FString ServerName = MoveTemp(PendingCharSelectServerName);
	bPendingCharSelect = false;
	PendingCharSelectCharacters.Reset();
	PendingCharSelectServerName.Reset();
	ShowCharacterSelectUI(Characters, ServerName);
}

bool AACEPlayerController::TryPrefetchGameplayHudAssets()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_PrefetchGameplayHud);
	if (bGameplayUiAssetsReady)
	{
		return true;
	}
	if (!Client || !bUseDatDrivenHud)
	{
		bGameplayUiAssetsReady = true;
		return true;
	}
	UGameInstance* GI = GetGameInstance();
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat || !Dat->IsDatReady())
	{
		return false;
	}
	UACEUIResourceResolver* Res = Client->GetUIResourceResolver();
	if (!Res)
	{
		return false;
	}

	static const uint32 MustHave[] = {
		0x06004CC0u, 0x06004CC1u,
		0x06001938u, 0x0600193Au, 0x0600193Cu,
		0x060011FBu,
	};
	static const uint32 CornerDids[] = {
		0x06004C40u, 0x06004C41u, 0x06004C42u, 0x06004C43u,
	};
	if (GameplayUiPrefetchIds.IsEmpty())
	{
		// Preserve an existing gameplay tree across recalls. Load a new one only
		// once at initial entry, then yield before decoding its texture queue.
		if (!DatGameplayBinder)
		{
			if (auto* Layout = Client->GetUILayoutResolver())
				if (!Layout->PrepareLayout(ACEUI::LayoutId::ClassicGameplay)) return false;
			if (auto* Flow = Client->GetUIFlow()) Flow->SetMode(ACEUI::EACEUIFlowMode::Gameplay);
			else if (auto* Layout = Client->GetUILayoutResolver())
			{
				if (!Layout->LoadLayout(ACEUI::LayoutId::ClassicGameplay)) return false;
			}
		}
		if (auto* Manager = Client->GetUIElementManager())
			CollectLayoutTextureIds(Manager->GetSyntheticRoot(), GameplayUiPrefetchIds);
		GameplayUiLayoutTextureCount = GameplayUiPrefetchIds.Num();
		if (GameplayUiLayoutTextureCount == 0) return false;
		GameplayUiPrefetchIds.Append(MustHave, UE_ARRAY_COUNT(MustHave));
		GameplayUiPrefetchIds.Append(CornerDids, UE_ARRAY_COUNT(CornerDids));
		return false;
	}
	// ResolveTexture can decode and upload a DAT image. A cold gameplay layout
	// contains hundreds: draining it in one tick freezes the VR view at Play.
	const double Deadline = FPlatformTime::Seconds() + .002;
	int32 ThisFrame = 0;
	while (GameplayUiPrefetchIndex < GameplayUiPrefetchIds.Num()
		&& ThisFrame < 8 && (ThisFrame == 0 || FPlatformTime::Seconds() < Deadline))
	{
		const int32 Index = GameplayUiPrefetchIndex++;
		const bool bReady = Res->ResolveTexture(GameplayUiPrefetchIds[Index]) != nullptr;
		if (Index < GameplayUiLayoutTextureCount) GameplayUiResolvedCount += bReady ? 1 : 0;
		else if (Index < GameplayUiLayoutTextureCount + UE_ARRAY_COUNT(MustHave)) bGameplayUiRequiredReady &= bReady;
		++ThisFrame;
	}
	if (GameplayUiPrefetchIndex < GameplayUiPrefetchIds.Num()) return false;
	bGameplayUiAssetsReady = bGameplayUiRequiredReady
		&& GameplayUiResolvedCount >= FMath::Max(12, GameplayUiLayoutTextureCount * 3 / 4);
	if (bGameplayUiAssetsReady)
	{
		UE_LOG(LogTemp, Log, TEXT("ACE: gameplay UI textures ready (%d/%d layout DIDs)"), GameplayUiResolvedCount, GameplayUiLayoutTextureCount);
	}
	else
	{
		GameplayUiPrefetchIndex = GameplayUiResolvedCount = 0;
		bGameplayUiRequiredReady = true;
	}
	return bGameplayUiAssetsReady;
}

void AACEPlayerController::ShowCharacterSelectUI(const TArray<FACECharacterInfo>& Characters, const FString& ServerName)
{
	if(DatCharGenBinder){DatCharGenBinder->Shutdown();DatCharGenBinder=nullptr;}
	if(DatCanvasWidget)DatCanvasWidget->SetCharGenBinder(nullptr);
	if (!IsLocalController() || !Client)
	{
		return;
	}

	// Character select must stay light — no landblock / entity streaming until Enter World.
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			Dat->SetWorldStreamingAllowed(false);
		}
	}

	// Credentials UI hands off to retail character select — remove so its text can't leak.
	if (UWorld* World=GetWorld())
	{
		World->GetTimerManager().ClearTimer(ShowLoginUIRetryTimerHandle);
		World->GetTimerManager().ClearTimer(DesiredSizeRetryTimerHandle);
	}
	if (LoginWidget)
	{
		LoginWidget->SetVisibility(ESlateVisibility::Collapsed);
		LoginWidget->RemoveFromParent();
		LoginWidget = nullptr;
	}

	if (DatGameplayBinder)
	{
		DatGameplayBinder->Shutdown();
		DatGameplayBinder = nullptr;
	}
	if (DatCanvasWidget)
	{
		DatCanvasWidget->SetGameplayBinder(nullptr);
	}

	if (!DatCanvasWidget)
	{
		DatCanvasWidget = CreateWidget<UACEUICanvasWidget>(this, UACEUICanvasWidget::StaticClass());
		if (DatCanvasWidget)
		{
			DatCanvasWidget->InitializeCanvas(Client->GetUIElementManager());
			DatCanvasWidget->SetResourceResolver(Client->GetUIResourceResolver());
		}
	}
	if (!DatCanvasWidget)
	{
		ShowLoginUI();
		if (LoginWidget)
		{
			LoginWidget->SetVisibility(ESlateVisibility::Visible);
		}
		return;
	}

	bool bLayoutReady = false;
	if (Client->GetUIFlow())
	{
		Client->GetUIFlow()->SetMode(ACEUI::EACEUIFlowMode::CharacterManagement);
	}
	if (UACEUILayoutResolver* Resolver = Client->GetUILayoutResolver())
	{
		bLayoutReady = Resolver->LoadLayout(ACEUI::LayoutId::CharacterManagement);
	}
	if (UACEUIElementManager* UiMgr = Client->GetUIElementManager())
	{
		const TSharedPtr<FACEUIElement> Root = UiMgr->GetSyntheticRoot();
		bLayoutReady = bLayoutReady && Root.IsValid() && Root->Children.Num() > 0;
	}

	if (!bLayoutReady)
	{
		UE_LOG(LogTemp, Warning, TEXT("ACE: CharacterManagement layout failed — falling back to LoginWidget list"));
		ShowLoginUI();
		if (LoginWidget)
		{
			LoginWidget->SetVisibility(ESlateVisibility::Visible);
		}
		return;
	}

	if (!DatCharSelectBinder)
	{
		DatCharSelectBinder = NewObject<UACEUICharSelectBinder>(this);
	}
	// Construct the layer before the binder installs character names and buttons.
	DatCanvasWidget->TakeWidget();
	if (DatCharSelectBinder)
	{
		DatCharSelectBinder->Initialize(Client, Client->GetUIElementManager(), DatCanvasWidget, this,
			Characters, ServerName);
		DatCanvasWidget->SetCharSelectBinder(DatCharSelectBinder);
	}
	if (!IsVRActive() && !DatCanvasWidget->IsInViewport())
	{
		DatCanvasWidget->AddToPlayerScreen(9000);
	}
	DatCanvasWidget->SetVisibility(ESlateVisibility::Visible);
	bShowMouseCursor = true;
	EnsureRetailMouseCursor();
	SetInputMode(FInputModeGameAndUI().SetHideCursorDuringCapture(false).SetLockMouseToViewportBehavior(DesktopMouseLockMode()));
}

void AACEPlayerController::HandleEnteredWorld(int32 PlayerGuid, const FACEPosition& SpawnPosition)
{
	if (UWorld* World = GetWorld())
	{
		const bool bOnWcMap = World->WorldComposition && World->WorldComposition->GetTilesList().Num() > 0;
		if (!bOnWcMap && !ACEPlaySessionRedirect::GPendingWcTravelMapAfterLogin.IsEmpty())
		{
			ACEPlaySessionRedirect::FPendingEnteredWorldAfterTravel Pending;
			Pending.PlayerGuid = PlayerGuid;
			Pending.SpawnPosition = SpawnPosition;
			ACEPlaySessionRedirect::GPendingEnteredWorldAfterTravel = Pending;

			const FString TravelMap = ACEPlaySessionRedirect::GPendingWcTravelMapAfterLogin;
			ACEPlaySessionRedirect::GPendingWcTravelMapAfterLogin.Empty();
			UE_LOG(LogTemp, Warning, TEXT("ACE: Enter World — ClientTravel to WC map '%s'"), *TravelMap);
			ClientTravel(TravelMap, TRAVEL_Absolute);
			return;
		}
	}

	HideLoginUI();
	DestroyCharacterSelectUI();
	// Initial entry warms the HUD incrementally; an existing HUD stays visible.
	if (!bUseEnterWorldLoadScreen)
	{
		ShowGameHUD();
		EnableGameplayViewportShadows();
	}
	bLocalPredicting = false;
	bHavePredictedPose = false;
	LastAppliedTeleportSeq = Client ? static_cast<uint16>(Client->GetTeleportSeq()) : 0;
	LastAppliedForcePositionSeq = Client && Client->GetSession() ? Client->GetSession()->GetForcePositionSeq() : 0;
	bHaveLastServerPose = SpawnPosition.IsValid();
	if (bHaveLastServerPose)
	{
		LastServerPose = SpawnPosition;
	}
	if (bSyncPossessedPawn)
	{
		if (APawn* P = GetPawn())
		{
			FVector SpawnLoc = SpawnPosition.ToUnrealLocation(WorldScale);
			float CapsuleHalfHeight = 88.f;
			if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
			{
				CapsuleHalfHeight = Cap->GetScaledCapsuleHalfHeight();
			}
			// ACE Z is feet; capsule root is center.
			SpawnLoc.Z += CapsuleHalfHeight;
			P->SetActorLocation(SpawnLoc);
			P->SetActorRotation(SpawnPosition.ToUnrealQuat());
			ApplyLocalPawnPkCollision();
		}
	}

	if (bUseEnterWorldLoadScreen)
	{
		bPendingEnterWorldTransition = false;
		if (!bEnterWorldLoading) BeginEnterWorldLoadScreen();
	}
	else if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			// Prefetch portal-space art only once a character is entering the world.
			Dat->PrefetchPortalSpaceSetup(0x02000306, WorldScale);
			Dat->SetWorldStreamingAllowed(true);
		}
	}

	// Defer DAT appearance off the ObjectCreate packet drain — building the local Setup mesh
	// synchronously here used to freeze the game thread before landblocks could stream.
	PendingSelfAppearanceGuid = PlayerGuid;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(this, &AACEPlayerController::ApplyPendingSelfAppearance);
	}
}

void AACEPlayerController::PreparePortalScreen()
{
	if (!GetWorld()) return;
	if (!IsValid(LoadingScreenActor))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		LoadingScreenActor = GetWorld()->SpawnActor<AACELoadingScreenActor>(
			AACELoadingScreenActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	}
	if (!LoadingScreenActor) return;
	LoadingScreenActor->WorldScale = WorldScale;
	LoadingScreenActor->TryBuildMesh();
	LoadingScreenActor->HideAllVisuals();
	LoadingScreenActor->SetActorTickEnabled(false);
}

bool AACEPlayerController::TickLogOffTransition()
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	if (!Session || !Session->IsLogOffPending()) return false;
	if (!bLogOffPresentationActive)
	{
		bLogOffPresentationActive = true;
		bLocalPredicting = bAutoRun = bMouseForwardActive = false;
		ClearServerMoveTo();
		ForwardAxis = RightAxis = TurnAxis = 0.f;
		PortalWorldRevealElapsed = -1.f;
		if (PlayerCameraManager) PlayerCameraManager->UnlockFOV();
		if (DatGameplayBinder)
		{
			DatGameplayBinder->CancelPointerGestures();
			DatGameplayBinder->CancelPendingUseWith();
			DatGameplayBinder->CancelCombatAttack();
		}
		SetIgnoreMoveInput(true);
		SetIgnoreLookInput(true);
	}
	if (!bUseEnterWorldLoadScreen) return true;
	const float Elapsed = float(FPlatformTime::Seconds() - Session->GetLogOffPortalStartTime());
	const bool bVR = IsVRActive();
	if (bEnterWorldLoading || bWorldRevealActive)
	{
		// Already in portal space: keep that tunnel rather than restarting it.
		// Only our own world-fade entry needs the retail one-second tunnel fade-in.
		if (bLogOffWorldFadeStarted && PlayerCameraManager && !bVR)
		{
			if (Elapsed < 2.f)
			{
				int32 W=1280,H=720; GetViewportSize(W,H);
				const float Aspect=float(W)/FMath::Max(1,H);
				PlayerCameraManager->SetFOV(ACERetailPortalAnimation::StretchFov(
					ACECameraRetail::HorizontalFovDegrees(Aspect,GetCameraFieldOfViewDegrees()),Aspect,Elapsed-1.f,true));
			}
			else PlayerCameraManager->UnlockFOV();
		}
		return true;
	}
	if (Elapsed < 0.f) return true; // Let the server's logout motion play in the world.
	PreparePortalScreen();
	if (!bLogOffWorldFadeStarted)
	{
		bLogOffWorldFadeStarted = true;
		PlayPortalEnterSound();
	}
	if (PlayerCameraManager && !bVR)
	{
		int32 W=1280,H=720; GetViewportSize(W,H);
		const float Aspect=float(W)/FMath::Max(1,H);
		PlayerCameraManager->SetFOV(ACERetailPortalAnimation::StretchFov(
			ACECameraRetail::HorizontalFovDegrees(Aspect,GetCameraFieldOfViewDegrees()),Aspect,Elapsed,false));
	}
	// Do not substitute a blank view if the portal art still needs a frame to load.
	if (Elapsed >= 1.f && LoadingScreenActor && LoadingScreenActor->bMeshReady)
	{
		BeginWorldTransition(TEXT("logout"),false);
		if (PlayerCameraManager && !bVR) PlayerCameraManager->SetFOV(179.f);
	}
	return true;
}

void AACEPlayerController::BeginWorldTransition(const TCHAR* Reason, bool bPlayEnterSound)
{
	PortalWorldRevealElapsed = -1.f;
	if (PlayerCameraManager) PlayerCameraManager->UnlockFOV();
	bWorldEntryRecoveryAttempted = false;
	bAwaitingRecoveryDestination = false;
	WorldEntryRecoveryElapsed = 0.f;
	WorldEntryFailureReason.Reset();
	bEnterWorldLoading = true;
	bWorldRevealActive = false;
	bGameplayUiAssetsReady = false;
	GameplayUiPrefetchIds.Reset();
	GameplayUiPrefetchIndex = GameplayUiResolvedCount = GameplayUiLayoutTextureCount = 0;
	bGameplayUiRequiredReady = true;
	bDestinationStreamingStarted = false;
	PortalRetirement = EPortalRetirement::ClearScene;
	PortalFirstVisibleFrame = MAX_uint64;
	EnterWorldLoadElapsed = 0.f;
	TunnelVisibleElapsed = 0.f;
	bLoggedTransitionTimeout = false;
	bPortalExitNotified = false;
	bLocalPredicting = false;
	ClearServerMoveTo();

	// Keep ViewTarget on the single LoadingScreenActor — pawn auto-camera steals the view
	// and makes the tunnel look frozen / absent.
	bAutoManageActiveCameraTarget = false;
	PortalSavedControlRotation = GetControlRotation();
	bHavePortalSavedControlRotation = true;

	// gmSmartBoxUI::UseTime hides only the world viewport. Chat and the other
	// gameplay panels remain usable; cancel gestures that started in the world.
	if (DatGameplayBinder)
	{
		DatGameplayBinder->CancelPointerGestures();
		DatGameplayBinder->CancelPendingUseWith();
		DatGameplayBinder->CancelCombatAttack();
		DatGameplayBinder->ShowTransientInfo(TEXT("In Portal Space - Please Wait..."));
	}
	if (FSlateApplication::IsInitialized()) FSlateApplication::Get().ReleaseAllPointerCapture();
	ApplyInWorldInputMode();
	ApplyRetailMouseCursor(false);
	if (HoverTooltipWidget)
	{
		HoverTooltipWidget->SetVisibility(ESlateVisibility::Collapsed);
	}
	bHoverTooltipVisible = false;

	UWorld* World = GetWorld();

	// Camera follows the tunnel actor — suppress rain sheets / weather PES over that view.
	// Retail SmartBox::Hide: the 3D world (including GameSky) is not drawn in portal space.
	SetSkyWeatherEnabled(false);
	if (World)
	{
		for (TActorIterator<AACESkyDomeActor> It(World); It; ++It)
		{
			if (AACESkyDomeActor* Sky = *It)
			{
				Sky->SetActorHiddenInGame(true);
				Sky->SetWeatherEnabled(false);
			}
		}
	}

	if (Client)
	{
		Client->StopMovement();
		Client->SetForcePositionReporting(false);
	}
	ForwardSent = -999.f;
	RightSent = -999.f;
	TurnSent = -999.f;
	bWasMoving = false;
	bForceMovementResend = true;

	if (APawn* P = GetPawn())
	{
		P->SetActorHiddenInGame(true);
		P->SetActorEnableCollision(false);
		if (auto* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
			App->ResetMotionForTeleport();
		if (UACEScriptComponent* Scripts = P->FindComponentByClass<UACEScriptComponent>())
		{
			Scripts->StopAllEffects();
			Scripts->SetPhysicsHidden(true);
		}
		if (Client)
		{
			const FACEPosition Pose = Client->GetPlayerPosition();
			if (Pose.IsValid())
			{
				FVector Feet = Pose.ToUnrealLocation(WorldScale);
				if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
				{
					Feet.Z += Cap->GetScaledCapsuleHalfHeight();
				}
				P->SetActorLocation(Feet);
			}
		}
	}

	if (!World)
	{
		return;
	}

	// Stream destination landblocks under the portal at a lightweight budget so the
	// tunnel animation stays smooth. Full-rate streaming resumes at reveal.
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			Dat->SetInPortalSpace(true);
			Dat->SetLightweightStreaming(true);
			// Submit a tracked portal frame before installing destination meshes.
			Dat->SetWorldStreamingAllowed(false);
			if (!Dat->IsDatReady())
			{
				Dat->BeginBackgroundLoad();
			}
		}
	}

	// Reset landblock fail marks / force a fresh ring sync around the session player cell.
	if (AGameModeBase* GM = World->GetAuthGameMode())
	{
		if (UACETerrainPresenterComponent* Terrain = GM->FindComponentByClass<UACETerrainPresenterComponent>())
		{
			// Retail CellManager keeps prefetching destination landblocks while the
			// portal viewport is showing. LoadRadius=0 made cell-collision wait 45s.
			Terrain->RestoreProceduralStreamingAfterLogin(5, 6);
		}
	}

	// Exactly one tunnel actor — never spawn a second while one exists.
	if (LoadingScreenActor && !IsValid(LoadingScreenActor))
	{
		LoadingScreenActor = nullptr;
	}
	if (!LoadingScreenActor && World)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		LoadingScreenActor = World->SpawnActor<AACELoadingScreenActor>(
			AACELoadingScreenActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
		UE_LOG(LogTemp, Log, TEXT("ACE: spawned portal-space tunnel actor"));
	}
	if (LoadingScreenActor)
	{
		LoadingScreenActor->WorldScale = WorldScale;
		LoadingScreenActor->SetActorTickEnabled(true);
		LoadingScreenActor->BeginTunnel();
		if (!IsVRActive() && LoadingScreenActor->Camera)
		{
			SetViewTarget(LoadingScreenActor);
		}
	}

	if (bPlayEnterSound) PlayPortalEnterSound();
	SetIgnoreMoveInput(true);
	SetIgnoreLookInput(true);
	UE_LOG(LogTemp, Log, TEXT("ACE: portal-space transition started (%s)"), Reason ? Reason : TEXT("unknown"));
}

void AACEPlayerController::PlayPortalEnterSound()
{
	// Retail portal audio: UI_EnterPortal (SoundTable 0x2000004B type 0x6A) once.
	// Animation 0x030005AC SoundTweaked @ frame 2 starts the portalspace ambient.
	AActor* SoundHost = LoadingScreenActor ? static_cast<AActor*>(LoadingScreenActor.Get()) : GetPawn();
	if (SoundHost)
	{
		UACEScriptComponent* Scripts = SoundHost->FindComponentByClass<UACEScriptComponent>();
		if (!Scripts)
		{
			Scripts = NewObject<UACEScriptComponent>(SoundHost, TEXT("ACEPortalScripts"));
			Scripts->RegisterComponent();
		}
		if (Scripts)
		{
			Scripts->StopAllSounds();
			constexpr int32 UiSoundTable = 0x2000004B;
			constexpr int32 UiEnterPortal = 0x6A;
			Scripts->PlayCenteredSoundFromTable(UiSoundTable, UiEnterPortal, 1.f);
		}
	}

}

bool AACEPlayerController::IsWorldEntryGeometryReady(uint32 CandidateCell) const
{
	UWorld* World = GetWorld();
	if (!World || !Client || !Client->GetPlayerPosition().IsValid()) return false;
	if (World->WorldComposition && World->WorldComposition->GetTilesList().Num() > 0)
		return ACEWcTerrainRuntime::IsTerrainReadyAround(World, Client->GetPlayerPosition().ToUnrealLocation(WorldScale), 4);
	const auto* GM = World->GetAuthGameMode();
	const auto* Terrain = GM ? GM->FindComponentByClass<UACETerrainPresenterComponent>() : nullptr;
	return Terrain && Terrain->IsPortalPlacementGeometryReady(Client->GetPlayerPosition().CellId, CandidateCell);
}

bool AACEPlayerController::FindWorldEntryPlacement(FVector& OutCapsuleCenter, EWorldEntryPlacement* OutState, uint32* OutCellId) const
{
	if (OutState) *OutState = EWorldEntryPlacement::Unavailable;
	if (OutCellId) *OutCellId = 0;
	const APawn* EntryPawn = GetPawn();
	const UCapsuleComponent* Capsule = EntryPawn ? EntryPawn->FindComponentByClass<UCapsuleComponent>() : nullptr;
	if (!Client || !GetWorld() || !Capsule || !Client->GetPlayerPosition().IsValid()) return false;
	const FACEPosition Pose = Client->GetPlayerPosition();
	const FVector Feet = Pose.ToUnrealLocation(WorldScale);
	UACEDatSubsystem* Dat = GetGameInstance() ? GetGameInstance()->GetSubsystem<UACEDatSubsystem>() : nullptr;
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	// Placement uses the same authored lower sphere as normal movement,
	// including its offset from the object's origin.
	const auto Body = GetPlayerCollisionBody();
	const float FloorRadius = Body.Radii[0];
	const FVector LowerCenter = Body.Centers[0]+FVector(0,0,HalfHeight);
	const float StepUp = FMath::Max(2.f, GetStepUpHeightCm());
	const float StepDown = GetStepDownHeightCm();
	const bool bGeometryReady = IsWorldEntryGeometryReady();
	bool bSawSolidBlocker = false;
	bool bUnresolvedClearSpace = false;
	const FCollisionQueryParams BaseParams(SCENE_QUERY_STAT(ACEWorldEntryPlacement), false, EntryPawn);
	// Portal/login coordinates are authoritative, and the server suppresses
	// creature contacts while materializing. A mob at the exit must neither
	// become our floor nor make valid architecture look like an unsafe landing.
	// Exclude only nearby creature bodies for this placement query; normal
	// movement immediately resumes their authored collision and escape rules.
	const float Limit = 4.f * WorldScale;
	TArray<FOverlapResult> Nearby;
	const auto Shape = Body.WithClearance(0.f,.5f);
	auto TryPosition = [&](FVector CandidateFeet)
	{
		// Outdoor portal coordinates can predate the current land height (the
		// Khayyaban arrival is below its DAT terrain). Seed from that height,
		// then still require cooked geometry and a clear full body.
		float TerrainZ = 0.f;
		FVector TerrainNormal=FVector::UpVector;
		const bool bOutdoorSupport=!ACECellTransit::IsIndoorCell(Pose.CellId) && Dat
			&& Dat->SampleOutdoorGroundZ(CandidateFeet.X, CandidateFeet.Y, WorldScale, TerrainZ,&TerrainNormal);
		if (bOutdoorSupport)
			CandidateFeet.Z = FMath::Max(CandidateFeet.Z, double(TerrainZ));
		FCollisionQueryParams Params=BaseParams;
		const bool bWading=bOutdoorSupport && Dat->GetOutdoorWaterDepthCm(CandidateFeet.X,CandidateFeet.Y,WorldScale)>0.f;
		if(bWading)
		{
			// CLandCell::find_env_collisions rejects entirely-water blocks, but
			// coastal water supports feet at the depth used by validate_walkable.
			if(Dat->GetOutdoorBlockWaterType(CandidateFeet.X,CandidateFeet.Y,WorldScale)==2)
			{bSawSolidBlocker=true;return false;}
			// The rendered sheet is above that support. Do not interpret wading
			// below it as penetrating a solid floor; scenery remains collidable.
			FilterWadingTerrain(*GetWorld(),Dat,CandidateFeet,CandidateFeet,WorldScale,Params);
		}
		Nearby.Reset();
		// Query at the corrected candidate height as well as its XY: old outdoor
		// portal coordinates can lie below today's terrain and its creatures.
		GetWorld()->OverlapMultiByChannel(Nearby, CandidateFeet + FVector(0,0,HalfHeight-Limit*.5f),
			FQuat::Identity, ECC_Pawn, FCollisionShape::MakeBox(FVector(Radius,Radius,
				HalfHeight+Limit*.5f+StepUp)), Params);
		for (const FOverlapResult& Overlap : Nearby)
		{
			const UPrimitiveComponent* Component=Overlap.GetComponent();
			if (Component && Component->ComponentTags.Contains(TEXT("ACECreatureBody")))
				Params.AddIgnoredComponent(Component);
		}
		FHitResult Floor;
		// Retail CTransition::find_placement_position keeps the placement and clears
		// contact when its normal step-down finds no floor. Four units is the lateral
		// escape search, not a mandatory floor search or a license to skip a portal drop.
		bool bSupported = GetWorld()->SweepSingleByChannel(Floor,
			CandidateFeet + LowerCenter + FVector(0, 0, StepUp),
			CandidateFeet + LowerCenter - FVector(0, 0, StepDown), FQuat::Identity,
			ECC_Pawn, FCollisionShape::MakeSphere(FloorRadius), Params)
			&& !Floor.bStartPenetrating && Floor.ImpactNormal.Z >= .5f;
		float SupportZ=bSupported ? Floor.Location.Z-LowerCenter.Z : -MAX_flt;
		if(bWading && bGeometryReady && TerrainNormal.Z>=.5f
			&& TerrainZ>=CandidateFeet.Z-StepDown && TerrainZ<=CandidateFeet.Z+StepUp)
		{
			SupportZ=FMath::Max(SupportZ,TerrainZ);
			bSupported=true;
		}
		if (bSupported) CandidateFeet.Z = SupportZ + .5f;
		const FVector Center = CandidateFeet + FVector(0, 0, HalfHeight);
		if (ACEBodySweep::OverlapsBody(*GetWorld(),Center,Shape,Params))
		{
			bSawSolidBlocker = true;
			return false;
		}
		uint32 CandidateCell = Pose.CellId;
		const FVector FootCenter = CandidateFeet + LowerCenter;
		if (ACECellTransit::IsIndoorCell(Pose.CellId) && (!Dat
			|| !ACECellTransit::SphereIntersectsEnvCell(*Dat, CandidateCell, FootCenter, 0.f, WorldScale)))
		{
			// Retail resolves placement through the cell's transit/visible neighbors.
			// The portal's cell is a hint, not proof that the authored point is inside it.
			if (!Dat || !ACECellTransit::ResolveTransitCellId(*Dat, Pose.CellId, FootCenter, Radius, WorldScale, CandidateCell)
				|| !ACECellTransit::IsIndoorCell(CandidateCell)
				|| !ACECellTransit::SphereIntersectsEnvCell(*Dat, CandidateCell, FootCenter, 0.f, WorldScale)
				|| !IsWorldEntryGeometryReady(CandidateCell))
			{
				bUnresolvedClearSpace = true;
				return false;
			}
		}
		// Empty space qualifies only after the destination collision is resident.
		// A delayed cook or missing room must not look like an intentional drop.
		if (!bSupported && !bGeometryReady) return false;
		OutCapsuleCenter = Center;
		if (OutCellId) *OutCellId = CandidateCell;
		if (OutState) *OutState = bSupported ? EWorldEntryPlacement::Grounded : EWorldEntryPlacement::Airborne;
		return true;
	};
	if (TryPosition(Feet)) return true;
	// Transition::FindPlacementPos searches outward up to four AC units. Validate
	// cell and full body for each alternative, allowing valid airborne placements.
	const float RingStep = FMath::Max(Radius, 48.f);
	const int32 Rings = FMath::Max(1, FMath::CeilToInt(Limit / RingStep));
	for (int32 Ring = 1; Ring <= Rings; ++Ring)
	{
		const float Distance = Limit * Ring / Rings;
		const int32 Steps = FMath::Max(8, FMath::CeilToInt(2.f * PI * Distance / RingStep));
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const float Angle = 2.f * PI * Step / Steps;
			if (TryPosition(Feet + FVector(FMath::Sin(Angle) * Distance, FMath::Cos(Angle) * Distance, 0))) return true;
		}
	}
	if (OutState && bGeometryReady && bSawSolidBlocker && !bUnresolvedClearSpace) *OutState = EWorldEntryPlacement::Blocked;
	return false;
}

void AACEPlayerController::ResetWorldEntryDestination()
{
	// F751 precedes the position. A recall can replace the destination while the
	// tunnel is already visible; the old LoginComplete must not acknowledge it.
	bEnterWorldLoading = true;
	bWorldRevealActive = false;
	bPortalExitNotified = false;
	bWaitingForServerUnhide = true;
	bAwaitingRecoveryDestination = false;
	WorldEntryRecoveryElapsed = 0.f;
	EnterWorldLoadElapsed = 0.f;
	bLoggedTransitionTimeout = false;
	bLocalPredicting = false;
	bHavePredictedPose = false;
	ClearServerMoveTo();
	// Arrival has not completed. Resume movement reporting only at the safe cut,
	// otherwise AutoPos can overwrite the server's destination during portal space.
	if (Client) Client->SetForcePositionReporting(false);
}

void AACEPlayerController::FailWorldEntry(const FString& Reason)
{
	WorldEntryFailureReason = Reason;
	bEnterWorldLoading = false;
	bWorldRevealActive = false;
	bAwaitingRecoveryDestination = false;
	if (LoadingScreenActor)
	{
		LoadingScreenActor->Destroy();
		LoadingScreenActor = nullptr;
	}
	UE_LOG(LogTemp, Warning, TEXT("ACE: world entry failed: %s"), *Reason);
	if (Client && Client->GetSession()) Client->GetSession()->Disconnect();
	// Disconnect broadcasts state/log messages synchronously; show the useful reason last.
	if (LoginWidget) LoginWidget->SetStatus(Reason);
}

bool AACEPlayerController::TickWorldEntryRecovery(float DeltaTime, const FString& BlockingGate)
{
	if (UACEDatSubsystem* Dat = GetGameInstance() ? GetGameInstance()->GetSubsystem<UACEDatSubsystem>() : nullptr)
	{
		if (!Dat->GetDatLoadError().IsEmpty())
		{
			// A recall cannot repair missing world files. Leave the tunnel with
			// the actionable error instead of waiting for nonexistent collision.
			FailWorldEntry(Dat->GetDatLoadError());
			return true;
		}
	}
	if (bAwaitingRecoveryDestination)
	{
		WorldEntryRecoveryElapsed += DeltaTime;
		if (WorldEntryRecoveryElapsed >= WorldTransitionMaxSeconds)
		{
			FailWorldEntry(TEXT("The spawn location is blocked and the server did not complete lifestone recall. Check that this character is attuned and allowed to recall, then reconnect."));
		}
		return true;
	}
	const bool bUnsafe = BlockingGate == TEXT("cell-collision") || BlockingGate == TEXT("wc-terrain")
		|| BlockingGate == TEXT("spawn-placement");
	if (!bUnsafe || EnterWorldLoadElapsed < WorldTransitionMaxSeconds) return false;
	FVector Placement;
	EWorldEntryPlacement PlacementState;
	if (FindWorldEntryPlacement(Placement, &PlacementState) && IsWorldEntryGeometryReady()) return false;
	if (PlacementState != EWorldEntryPlacement::Blocked)
	{
		FailWorldEntry(TEXT("The destination geometry could not finish loading. Please reconnect and check the client DAT files. No lifestone recall was requested."));
		return true;
	}
	if (bWorldEntryRecoveryAttempted)
	{
		FailWorldEntry(TEXT("The lifestone destination could not load a safe spawn. Reconnect after checking the client DAT files. The character was not placed in missing or blocked terrain."));
		return true;
	}
	if (!Client || Client->GetSessionState() != EACESessionState::InWorld) return false;
	bWorldEntryRecoveryAttempted = true;
	bAwaitingRecoveryDestination = true;
	WorldEntryRecoveryElapsed = 0.f;
	// Existing TeleToLifestone (0x63): server owns Sanctuary and recall eligibility.
	// Do not spoof an outbound position, force an unsafe reveal, or spam recalls.
	Client->SendTeleToLifestone();
	UE_LOG(LogTemp, Warning, TEXT("ACE: unsafe world entry (%s) -- requesting one lifestone recall"), *BlockingGate);
	return true;
}

bool AACEPlayerController::IsWorldTransitionReady(FString* OutBlockingGate, bool bIgnoreServerUnhide) const
{
	auto Block = [&](const TCHAR* Gate) -> bool
	{
		if (OutBlockingGate)
		{
			*OutBlockingGate = Gate;
		}
		return false;
	};
	if (bAwaitingRecoveryDestination) return Block(TEXT("lifestone-recall"));
	if (!Client || Client->GetSessionState() != EACESessionState::InWorld)
		return Block(TEXT("server-entry"));

	// Prefer the textured tunnel, but permit destination loading if its art fails.
	const bool bTunnelMeshReady = LoadingScreenActor && LoadingScreenActor->bMeshReady;
	const bool bTunnelMeshTimedOut = EnterWorldLoadElapsed >= FMath::Min(WorldTransitionMaxSeconds, 4.f);
	if (!bTunnelMeshReady && !bTunnelMeshTimedOut)
	{
		return Block(TEXT("portal-tunnel-mesh"));
	}
	const float VisibleElapsed = bTunnelMeshReady ? TunnelVisibleElapsed : EnterWorldLoadElapsed;
	if (VisibleElapsed < EnterWorldLoadMinSeconds)
	{
		return Block(TEXT("min-tunnel-time"));
	}

	UGameInstance* GI = GetGameInstance();
	UACEDatSubsystem* Dat = GI ? GI->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (!Dat || !Dat->IsDatReady())
	{
		return Block(TEXT("dat-ready"));
	}
	if (!Dat->IsWorldStreamingAllowed())
	{
		return Block(TEXT("streaming-held"));
	}


	FACEPosition AreaCenter;
	if (Client)
	{
		AreaCenter = Client->GetPlayerPosition();
	}

	if (AGameModeBase* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
	{
		const bool bWcBakedTerrain = GetWorld() && GetWorld()->WorldComposition
			&& GetWorld()->WorldComposition->GetTilesList().Num() > 0;
		if (!bWcBakedTerrain)
		{
			if (UACETerrainPresenterComponent* Terrain = GM->FindComponentByClass<UACETerrainPresenterComponent>())
			{
				// Resident center collision required; the arrival itself may be airborne.
				if (!Terrain->IsPlayerCellCollisionReady())
				{
					return Block(TEXT("cell-collision"));
				}
				FVector Placement;
				if (!FindWorldEntryPlacement(Placement)) return Block(TEXT("spawn-placement"));
				const bool bIndoor = (static_cast<uint32>(AreaCenter.CellId) & 0xFFFFu) >= 0x0100u;
				if (bIndoor && !bIgnoreServerUnhide)
				{
					// Portal exit: current cell + portal/VisibleCells neighborhood only.
					// Waiting on IsEnvCellSyncComplete loads every EnvCell in the 1-ring LB
					// shell (Town Network / marketplace = thousands) and never leaves portal space.
					if (!Terrain->IsPlayerIndoorNeighborhoodReady())
					{
						return Block(TEXT("envcell-neighborhood"));
					}
				}
				if (!bIgnoreServerUnhide && Terrain->NeedsExteriorTerrain(static_cast<uint32>(AreaCenter.CellId)))
				{
					// Surface interiors also need the landscape visible through their exits.
					if (!Terrain->IsLoadRadiusTerrainReady()) return Block(TEXT("visible-terrain"));
					if (!Terrain->IsLoadRadiusBuildingsReadyForRadius(Terrain->GetFullDetailRadius()))
					{
						return Block(TEXT("load-radius-terrain"));
					}
				}
			}
		}
		else if (UWorld* World = GetWorld())
		{
			FVector SpawnLoc = FVector::ZeroVector;
			if (Client)
			{
				const FACEPosition Pose = Client->GetPlayerPosition();
				if (Pose.IsValid())
				{
					SpawnLoc = Pose.ToUnrealLocation(WorldScale);
				}
			}
			if (!ACEWcTerrainRuntime::IsTerrainReadyAround(World, SpawnLoc, 4))
			{
				return Block(TEXT("wc-terrain"));
			}
		}
	}

	FVector Placement;
	if (!FindWorldEntryPlacement(Placement)) return Block(TEXT("spawn-placement"));
	// Allow nearby objects to arrive while distant terrain and HUD textures warm.
	// A clear placement is mandatory; a valid portal drop need not have floor contact.
	if (bIgnoreServerUnhide) return true;

	// Deadlock guard: ACE only clears Hidden after LoginComplete (OnTeleportComplete).
	// Callers that still need to wait for unhide leave bIgnoreServerUnhide=false; arrival
	// notification uses true so we can send LoginComplete while waiting for SetState.
	if (!bIgnoreServerUnhide && bWaitingForServerUnhide && EnterWorldLoadElapsed < WorldTransitionMaxSeconds)
	{
		return Block(TEXT("server-unhide"));
	}

	// Gameplay HUD textures must be warm before reveal — otherwise chrome pops in after exit.
	if (!bGameplayUiAssetsReady)
	{
		return Block(TEXT("gameplay-ui-assets"));
	}

	// Nearby weenie meshes + outdoor flora beside the pawn. Requires LoginComplete first
	// for ObjectCreate, so skip when bIgnoreServerUnhide — that path only sends arrival
	// once terrain/buildings are ready so appearances can cook under the tunnel.
	if (!bIgnoreServerUnhide)
	{
		if (AGameModeBase* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
		{
			const bool bWcBakedTerrain = GetWorld() && GetWorld()->WorldComposition
				&& GetWorld()->WorldComposition->GetTilesList().Num() > 0;
			if (!bWcBakedTerrain)
			{
				if (UACETerrainPresenterComponent* Terrain = GM->FindComponentByClass<UACETerrainPresenterComponent>())
				{
					if (Terrain->NeedsExteriorTerrain(static_cast<uint32>(AreaCenter.CellId))
						&& !Terrain->IsLoadRadiusSceneryCompleteForRadius(1))
					{
						return Block(TEXT("near-scenery"));
					}
				}
			}
			if (UACEWorldPresenterComponent* WorldPres = GM->FindComponentByClass<UACEWorldPresenterComponent>())
			{
				if (!WorldPres->IsAreaAppearancesReady(AreaCenter, 48.f))
				{
					return Block(TEXT("area-appearances"));
				}
			}
		}
	}

	if (OutBlockingGate)
	{
		OutBlockingGate->Reset();
	}
	return true;
}

void AACEPlayerController::SetSkyWeatherEnabled(bool bEnabled)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AACESkyDomeActor> It(World); It; ++It)
	{
		if (AACESkyDomeActor* Sky = *It)
		{
			Sky->SetWeatherEnabled(bEnabled);
		}
	}
}

void AACEPlayerController::NotifyPortalArrivalIfNeeded()
{
	if (bPortalExitNotified || !Client)
	{
		return;
	}
	Client->NotifyExitedPortalSpace();
	bPortalExitNotified = true;
	UE_LOG(LogTemp, Log, TEXT("ACE: LoginComplete sent (exited portal space)"));
}

bool AACEPlayerController::RetirePortalScene()
{
	auto* Dat = GetGameInstance() ? GetGameInstance()->GetSubsystem<UACEDatSubsystem>() : nullptr;
	if (PortalRetirement == EPortalRetirement::ClearScene)
	{
		PortalRetirementStarted = FPlatformTime::Seconds();
		if (Dat) Dat->LogLandTextureMemory(TEXT("before retirement"));
		if (auto* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
			if (auto* Terrain = GM->FindComponentByClass<UACETerrainPresenterComponent>())
				Terrain->ResetStreamingForTransition();
		if (Dat) Dat->TrimMemoryCaches();
		PortalRetirement = EPortalRetirement::WaitPreviousGC;
	}
	if (PortalRetirement == EPortalRetirement::WaitPreviousGC)
	{
		// A collection already in progress may have marked the old scene reachable.
		// Finish it before requesting a fresh pass over the now-destroyed actors.
		if (IsIncrementalReachabilityAnalysisPending() || IsIncrementalPurgePending()) return false;
		PortalRetirementLastGC = GetLastGCTime();
		GEngine->ForceGarbageCollection(false);
		PortalRetirement = EPortalRetirement::WaitCollection;
		return false;
	}
	if (PortalRetirement == EPortalRetirement::WaitCollection)
	{
		// Engine-end-of-frame GC destroys unreachable resources with its normal time
		// budget. Continue rendering tracked portal frames throughout the purge.
		if (GetLastGCTime() <= PortalRetirementLastGC || IsIncrementalReachabilityAnalysisPending()
			|| IsIncrementalPurgePending()) return false;
		PortalRetirementFence = MakeShared<FRenderCommandFence>();
		PortalRetirementFence->BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
		PortalRetirement = EPortalRetirement::WaitRender;
		return false;
	}
	if (PortalRetirement == EPortalRetirement::WaitRender)
	{
		if (!PortalRetirementFence->IsFenceComplete()) return false;
		PortalRetirementFence.Reset();
		PortalRetirement = EPortalRetirement::Complete;
		if (Dat) Dat->LogLandTextureMemory(TEXT("after retirement"));
		UE_LOG(LogTemp, Log, TEXT("ACE: portal resource retirement completed in %.3fs"),
			FPlatformTime::Seconds() - PortalRetirementStarted);
	}
	return PortalRetirement == EPortalRetirement::Complete;
}

void AACEPlayerController::TickWorldTransition()
{
	const float Dt = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.016f;
	EnterWorldLoadElapsed += Dt;
	SetSkyWeatherEnabled(false);
	if ((!Client || Client->GetSessionState() != EACESessionState::InWorld)
		&& EnterWorldLoadElapsed >= WorldTransitionMaxSeconds)
	{
		FailWorldEntry(TEXT("The server did not finish entering the world. Please reconnect."));
		return;
	}

	if (LoadingScreenActor)
	{
		LoadingScreenActor->TryBuildMesh();
		// Keep the camera locked on the tunnel the entire wait — never peek at the world.
		if (!IsVRActive() && LoadingScreenActor->Camera
			&& (LoadingScreenActor->GetPhase() == EACEPortalTransitionPhase::Tunnel
				|| LoadingScreenActor->GetPhase() == EACEPortalTransitionPhase::Reveal)
			&& GetViewTarget() != LoadingScreenActor)
		{
			SetViewTarget(LoadingScreenActor);
		}
	}

	// Start budgeted destination work once the tunnel is on-camera. Keeping lightweight
	// mode until reveal would prevent the very appearances the readiness gate waits on.
	const bool bTunnelMeshReady = LoadingScreenActor && LoadingScreenActor->bMeshReady;
	const bool bTunnelMeshTimedOut = EnterWorldLoadElapsed >= FMath::Min(WorldTransitionMaxSeconds, 4.f);
	if (bTunnelMeshReady || bTunnelMeshTimedOut)
	{
		if (PortalFirstVisibleFrame == MAX_uint64) PortalFirstVisibleFrame = GFrameCounter;
		if (bTunnelMeshReady) TunnelVisibleElapsed += Dt;
		// Submit a tracked portal frame before the first expensive destination batch.
		if (GFrameCounter <= PortalFirstVisibleFrame) return;
		if (!Client || Client->GetSessionState() != EACESessionState::InWorld)
		{
			return;
		}
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			{
				if (!bDestinationStreamingStarted)
				{
					// Keep submitting tracked portal frames while cooked parents load.
					if (!Dat->PrepareRuntimeMaterials()) return;
					if (!RetirePortalScene()) return;
					// Destination scenery / weenie meshes cook under the tunnel so reveal
					// isn't a pop-in. Keep the camera on the portal until those gates pass.
					bDestinationStreamingStarted = true;
					Dat->SetLightweightStreaming(false);
					Dat->SetWorldStreamingAllowed(true);
					if (AGameModeBase* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
					{
						if (UACEWorldPresenterComponent* WorldPres =
							GM->FindComponentByClass<UACEWorldPresenterComponent>())
						{
							WorldPres->BeginPostPortalAppearanceBoost(16, 40.f);
						}
						if (UACETerrainPresenterComponent* Terrain =
							GM->FindComponentByClass<UACETerrainPresenterComponent>())
						{
							Terrain->RestoreProceduralStreamingAfterLogin(5, 6);
							Terrain->KickLandblockLoginBurst();
						}
					}
					UE_LOG(LogTemp, Log, TEXT("ACE: portal destination streaming started (tunnel ready=%d)"), bTunnelMeshReady);
				}
			}
		}
		if (UWorld* World = GetWorld())
		{
			if (World->WorldComposition && World->WorldComposition->GetTilesList().Num() > 0 && Client)
			{
				const FACEPosition Pose = Client->GetPlayerPosition();
				if (Pose.IsValid())
				{
					const FVector SpawnLoc = Pose.ToUnrealLocation(WorldScale);
					ACEWcTerrainRuntime::ShowWcLandscapesAfterLogin(World);
						ACEWcTerrainRuntime::RequestTerrainAround(
						World, SpawnLoc, /*RingTiles*/ 4);
				}
			}
		}
	}
	else if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			if (!Dat->IsDatReady())
			{
				Dat->BeginBackgroundLoad();
			}
		}
	}

	if (!Client || Client->GetSessionState() != EACESessionState::InWorld) return;
	if (PendingSelfAppearanceGuid != 0 && bDestinationStreamingStarted)
	{
		ApplyPendingSelfAppearance();
	}

	// Prefetch ClassicGameplay textures under the tunnel so ShowGameHUD does not pop art later.
	TryPrefetchGameplayHudAssets();
	if (bGameplayUiAssetsReady && !DatGameplayBinder && !GameHUDWidget)
	{
		ShowGameHUD();
		if (DatGameplayBinder) DatGameplayBinder->ShowTransientInfo(TEXT("In Portal Space - Please Wait..."));
	}

	if (bWorldRevealActive)
	{
		APawn* P = GetPawn();
		const FVector PawnLoc = P ? P->GetActorLocation() : FVector::ZeroVector;
		const bool bRevealDone = LoadingScreenActor
			? LoadingScreenActor->TickReveal(Dt, PawnLoc)
			: true;
		if (bRevealDone)
		{
			FinishWorldTransition();
		}
		return;
	}

	FString BlockingGate;
	const bool bReady = IsWorldTransitionReady(&BlockingGate);
	if (TickWorldEntryRecovery(Dt, BlockingGate)) return;
	if (!bReady)
	{
		static double LastPortalGateLog = 0.0;
		const double Now = FPlatformTime::Seconds();
		if (Now - LastPortalGateLog >= 2.0)
		{
			LastPortalGateLog = Now;
			const FACEPosition Destination = Client ? Client->GetPlayerPosition() : FACEPosition();
			UE_LOG(LogTemp, Warning,
				TEXT("ACE: portal-space holding (%.1fs, %s, cell=0x%08X pos=%s)"),
				EnterWorldLoadElapsed, BlockingGate.IsEmpty() ? TEXT("unknown") : *BlockingGate,
				static_cast<uint32>(Destination.CellId), *Destination.Location.ToString());
		}
	}
	// Once destination content is ready, tell the server we exited portal space so it can
	// clear Teleporting / Hidden. Waiting for unhide without LoginComplete is a deadlock.
	if (!bPortalExitNotified && IsWorldTransitionReady(nullptr, /*bIgnoreServerUnhide*/ true))
	{
		NotifyPortalArrivalIfNeeded();
	}
	if (!bReady && EnterWorldLoadElapsed >= WorldTransitionMaxSeconds)
	{
		// Force past server-unhide and indoor neighborhood stalls (Town Network).
		// Outdoor landblocks / nearby flora stay held — force-reveal is the pop-in.
		if (BlockingGate == TEXT("server-unhide"))
		{
			if (!bLoggedTransitionTimeout)
			{
				bLoggedTransitionTimeout = true;
				UE_LOG(LogTemp, Warning, TEXT("ACE: portal-space server-unhide stalled after %.1fs — clearing wait"),
					EnterWorldLoadElapsed);
			}
			bWaitingForServerUnhide = false;
		}
		else if (BlockingGate == TEXT("envcell-visual")
			|| BlockingGate == TEXT("envcell-neighborhood")
			|| BlockingGate == TEXT("envcell-landblock")
			|| BlockingGate == TEXT("area-appearances"))
		{
			if (!bLoggedTransitionTimeout)
			{
				bLoggedTransitionTimeout = true;
				UE_LOG(LogTemp, Warning,
					TEXT("ACE: portal-space indoor/appearance ready stalled after %.1fs (%s) — forcing LoginComplete + reveal"),
					EnterWorldLoadElapsed, *BlockingGate);
			}
			NotifyPortalArrivalIfNeeded();
			BeginWorldReveal();
			return;
		}
		else if (!bLoggedTransitionTimeout)
		{
			bLoggedTransitionTimeout = true;
			UE_LOG(LogTemp, Warning, TEXT("ACE: portal-space still loading after %.1fs (blocked by %s) — holding"),
				EnterWorldLoadElapsed, BlockingGate.IsEmpty() ? TEXT("unknown") : *BlockingGate);
		}
	}

	if (IsWorldTransitionReady(&BlockingGate))
	{
		BeginWorldReveal();
	}
}

void AACEPlayerController::BeginWorldReveal()
{
	if (bWorldRevealActive)
	{
		return;
	}
	// Even the visual-load timeout must pass the physical placement check.
	FVector SafePlacement;
	if (!FindWorldEntryPlacement(SafePlacement)) return;

	NotifyPortalArrivalIfNeeded();

	bEnterWorldLoading = false;
	bWorldRevealActive = true;
	bWaitingForServerUnhide = false;
	EnableGameplayViewportShadows();

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			Dat->SetLightweightStreaming(false);
			Dat->SetWorldStreamingAllowed(true);
		}
	}

	if (APawn* P = GetPawn())
	{
		// Re-snap to the server feet pose now that floor collision should exist.
		if (Client)
		{
			const FACEPosition Pose = Client->GetPlayerPosition();
			FVector Feet = SafePlacement;
			float CapsuleHalfHeight = 88.f;
			if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
			{
				CapsuleHalfHeight = Cap->GetScaledCapsuleHalfHeight();
			}
			P->SetActorLocation(Feet);
			P->SetActorRotation(Pose.ToUnrealQuat());
			PredictedPose = Pose;
			PredictedPose.SetLocationFromUnreal(Feet - FVector(0, 0, CapsuleHalfHeight), WorldScale);
			Client->SetReportedPosition(PredictedPose);
			bHavePredictedPose = true;
		}
		// Keep the pawn hidden until FinishWorldTransition cuts to the destination.
		P->SetActorHiddenInGame(true);
		P->SetActorEnableCollision(false);
		if (UACEScriptComponent* Scripts = P->FindComponentByClass<UACEScriptComponent>())
		{
			// Preserve the Hidden particle cloud through the camera cut.
			Scripts->SetPhysicsHidden(true);
		}
		if (!IsVRActive() && LoadingScreenActor && LoadingScreenActor->Camera)
		{
			SetViewTarget(LoadingScreenActor);
		}
	}

	if (AGameModeBase* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
	{
		if (UACEWorldPresenterComponent* WorldPres = GM->FindComponentByClass<UACEWorldPresenterComponent>())
		{
			WorldPres->RehomePlayerChildren(Client ? Client->GetPlayerGuid() : 0);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ACE: portal-space reveal started (%.2fs in tunnel)"), EnterWorldLoadElapsed);

	// If the bubble actor failed to spawn, finish immediately.
	if (!LoadingScreenActor)
	{
		FinishWorldTransition();
	}
}

void AACEPlayerController::InvalidateMovementAfterTeleport(bool bAirborneArrival)
{
	// CommandInterpreter::PlayerTeleported cancels autorun. Physical held keys
	// are sampled normally next tick; cached pre-portal axes must not survive.
	bAutoRun = false;
	bManualMovementCommandSent = false;
	ForwardAxis = RightAxis = TurnAxis = 0.f;
	// Re-seed MotionStance from CombatMode before StopMovement so MoveToState does not
	// echo NonCombat and wipe HandCombat/Magic after portal arrival.
	if (Client && Client->GetPlayerVitals().bValid)
	{
		const int32 Mode = Client->GetPlayerVitals().CombatMode;
		const TArray<FACEWorldObject> Equipped = Client->GetEquippedItems();
		const uint32 Stance = ACECombatStance::ResolveForCombatMode(Equipped, static_cast<uint32>(Mode));
		if (TSharedPtr<FACESession> Session = Client->GetSession())
		{
			Session->SetCurrentStance(Stance);
		}
		if (APawn* P = GetPawn())
		{
			if (UACECharacterAppearanceComponent* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
			{
				App->SetPreferredStyle(static_cast<int32>(Stance));
			}
		}
	}
	if (Client)
	{
		if (bAirborneArrival) Client->SendMovementEx(0.f, 0.f, 0.f, bRunning, false, false);
		else Client->StopMovement();
		if (auto Session = Client->GetSession()) Session->SetReportedContact(!bAirborneArrival);
		Client->SetForcePositionReporting(true);
	}
	ForwardSent = -999.f;
	RightSent = -999.f;
	TurnSent = -999.f;
	bWasMoving = false;
	bRunningSent = !bRunning;
	bForceMovementResend = true;
	bLocalPredicting = bAirborneArrival;
	bJumpCharging = false;
	bJumpAirborne = bAirborneArrival;
	bJumpAirborneSent = bAirborneArrival;
	bStandingJumpLocked = false;
	bHaveJumpLaunchFacing = false;
	JumpAirborneSeconds = 0.f;
	StepHoldSeconds = 0.f;
	JumpWorldAceVelocity = FVector::ZeroVector;
	LandingWorldAceVelocity = FVector::ZeroVector;
	LandingRootTrack.Reset();
	JumpLocalAceVelocity = FVector::ZeroVector;
	ClearServerMoveTo();
	// Clearing an old approach disables forced reports; a falling arrival needs
	// them immediately, including the interval before the next movement tick.
	if (Client) Client->SetForcePositionReporting(true);
	if (GetPawn())
		if (auto* App = GetPawn()->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			// Also clear anything received while loading, after restoring the combat
			// stance. This covers both tunnel exits and instant server teleports.
			App->ResetMotionForTeleport();
			if (bAirborneArrival)
			{
				App->SetSuppressLocoIdleBlend(true);
				App->SetHeldActionMotion(0x40000015); // Falling, without a Jump action/impulse.
			}
			// The appearance tick may already have run this frame. Seat the arrival
			// pose before the camera cut instead of displaying one last running frame.
			App->TickComponent(0.f, LEVELTICK_All, nullptr);
		}
}

void AACEPlayerController::FinishWorldTransition()
{
	FVector SafePlacement;
	EWorldEntryPlacement PlacementState;
	uint32 PlacementCell = 0;
	if (!FindWorldEntryPlacement(SafePlacement, &PlacementState, &PlacementCell))
	{
		bEnterWorldLoading = true;
		bWorldRevealActive = false;
		if (LoadingScreenActor) LoadingScreenActor->BeginTunnel();
		return;
	}
	bEnterWorldLoading = false;
	bWorldRevealActive = false;
	bWaitingForServerUnhide = false;
	bAutoManageActiveCameraTarget = true;
	SetIgnoreMoveInput(false);
	SetIgnoreLookInput(false);
	ApplyInWorldInputMode();

	if (APawn* P = GetPawn())
	{
		if (UACEScriptComponent* Scripts = P->FindComponentByClass<UACEScriptComponent>())
		{
			Scripts->StopAllSounds();
		}

		// Exit whoosh from the tunnel host (still in-portal; only ForceCentered SFX allowed).
		AActor* SoundHost = LoadingScreenActor ? static_cast<AActor*>(LoadingScreenActor.Get()) : static_cast<AActor*>(P);
		if (UACEScriptComponent* PortalScripts = SoundHost->FindComponentByClass<UACEScriptComponent>())
		{
			PortalScripts->StopAllSounds();
			constexpr int32 UiSoundTable = 0x2000004B;
			constexpr int32 UiExitPortal = 0x6B;
			PortalScripts->PlayCenteredSoundFromTable(UiSoundTable, UiExitPortal, 1.f);
		}

		// Seat pawn + ControlRotation from server pose BEFORE view-target switch so the first
		// gameplay frame isn't a tumbled camera cut that then snaps upright.
		if (Client)
		{
			const FACEPosition Pose = Client->GetPlayerPosition();
			if (Pose.IsValid())
			{
				FVector Feet = SafePlacement;
				float CapsuleHalfHeight = 88.f;
				if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
				{
					CapsuleHalfHeight = Cap->GetScaledCapsuleHalfHeight();
				}
				P->SetActorLocation(Feet);
				P->SetActorRotation(Pose.ToUnrealQuat());
				PredictedPose = Pose;
				PredictedPose.CellId = PlacementCell;
				PredictedPose.SetLocationFromUnreal(Feet - FVector(0, 0, CapsuleHalfHeight), WorldScale);
				Client->SetReportedPosition(PredictedPose);
				bHavePredictedPose = true;
			}
		}
		P->SetActorHiddenInGame(false);
		P->SetActorEnableCollision(true);
		if (UACECharacterAppearanceComponent* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			App->SetAppearanceVisible(true);
		}
		if (UACEScriptComponent* Scripts = P->FindComponentByClass<UACEScriptComponent>())
		{
			// Retail UnHide stops new bubbles and fades the mesh in over 0.75s.
			Scripts->SetPhysicsHidden(false);
		}
		if (AGameModeBase* DropGM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
		{
			if (UACEWorldPresenterComponent* WorldPres = DropGM->FindComponentByClass<UACEWorldPresenterComponent>())
			{
				WorldPres->DropPendingOneShotEffectsForGuid(Client ? Client->GetPlayerGuid() : 0);
			}
		}
		{
			FRotator Face = P->GetActorRotation();
			Face.Pitch = 0.f;
			Face.Roll = 0.f;
			SetControlRotation(Face);
			bHavePortalSavedControlRotation = false;
		}
		if (PlayerCameraManager)
		{
			PlayerCameraManager->SetGameCameraCutThisFrame();
		}
		SetViewTarget(P);
		if (!IsVRActive() && LoadingScreenActor && LoadingScreenActor->IsRevealFinished() && PlayerCameraManager)
		{
			PortalWorldRevealElapsed = 0.f;
			PlayerCameraManager->SetFOV(179.f);
		}
	}

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			Dat->SetInPortalSpace(false);
			Dat->SetLightweightStreaming(false);
			Dat->SetWorldStreamingAllowed(true);
		}
	}
	// Resume deferred streaming. The presenter's first world-camera update restores
	// actor visibility even when the destination finished loading inside the tunnel.
	if (UWorld* RevealWorld = GetWorld())
	{
		if (AGameModeBase* GM = RevealWorld->GetAuthGameMode())
		{
			if (UACETerrainPresenterComponent* Terrain =
				GM->FindComponentByClass<UACETerrainPresenterComponent>())
			{
				Terrain->KickLandblockLoginBurst();
			}
		}
	}

	if (LoadingScreenActor)
	{
		LoadingScreenActor->HideAllVisuals();
		LoadingScreenActor->Destroy();
		LoadingScreenActor = nullptr;
	}

	bool bIndoorDest = false;
	if (Client)
	{
		const FACEPosition Pose = Client->GetPlayerPosition();
		bIndoorDest = (static_cast<uint32>(Pose.CellId) & 0xFFFFu) >= 0x0100u;
	}
	if (UWorld* World = GetWorld())
	{
		SetSkyWeatherEnabled(!bIndoorDest);
		for (TActorIterator<AACESkyDomeActor> It(World); It; ++It)
		{
			if (AACESkyDomeActor* Sky = *It)
			{
				Sky->SetActorHiddenInGame(false);
				Sky->InvalidateAndRebuild();
			}
		}
		if (World->WorldComposition && World->WorldComposition->GetTilesList().Num() > 0)
		{
			WcLastStreamTileX = INT32_MIN;
			WcLastStreamTileY = INT32_MIN;
			ACEWcTerrainRuntime::ShowWcLandscapesAfterLogin(World);
			ACEWcTerrainRuntime::MaintainLandscapePresentation(World);
		}
	}

	EnableGameplayViewportShadows();

	ShowGameHUD();
	bShowMouseCursor = true;
	EnsureRetailMouseCursor();
	ApplyRetailMouseCursor(false);
	// The loading screen releases viewport capture. Preserve the user's hold/
	// toggle intent and restore relative input without replacing the saved cursor.
	if (bMouseLookActive && !IsVRActive()) SetMouseLookActive(true);

	// Re-seed prediction from the post-teleport server pose and force a fresh MoveToState
	// so held WASD keys don't silently reuse pre-portal movement sequences.
	NotifyPortalArrivalIfNeeded();
	if (Client && bHavePredictedPose)
	{
		// Keep the validated placement; an earlier server anchor may be buried in
		// terrain or predate the new destination. Airborne arrivals keep their height.
		LastServerPose = PredictedPose;
		Client->SetReportedPosition(PredictedPose);
	}
	const bool bAirborneArrival = PlacementState == EWorldEntryPlacement::Airborne;
	InvalidateMovementAfterTeleport(bAirborneArrival);
	if (Client) Client->FlushAutonomousPosition(!bAirborneArrival);
	if (DatGameplayBinder && Client && Client->GetPlayerVitals().bValid)
	{
		// Stance alone is not enough — CombatMode drives spell hotbar visibility / 1–9 cast.
		DatGameplayBinder->SyncCombatModeFromServer(Client->GetPlayerVitals().CombatMode);
	}
	if (AGameModeBase* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
	{
		if (UACEWorldPresenterComponent* WorldPres = GM->FindComponentByClass<UACEWorldPresenterComponent>())
		{
			WorldPres->BeginPostPortalAppearanceBoost(4, 2.f);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ACE: portal-space transition finished (%.2fs)"), EnterWorldLoadElapsed);
}

void AACEPlayerController::ApplyPendingSelfAppearance()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ACE_ApplySelfAppearance);
	// The PlayerCreate timer can run before a tracked portal frame is submitted.
	// Leave appearance work queued for TickWorldTransition in that case.
	if (bEnterWorldLoading && !bDestinationStreamingStarted) return;
	if (!bApplyLocalAppearance || !Client || PendingSelfAppearanceGuid == 0)
	{
		return;
	}

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			if (!Dat->IsDatReady())
			{
				Dat->BeginBackgroundLoad();
				if (UWorld* World = GetWorld())
				{
					FTimerHandle Handle;
					World->GetTimerManager().SetTimer(Handle, this, &AACEPlayerController::ApplyPendingSelfAppearance, 0.25f, false);
				}
				return;
			}
		}
	}

	FACEWorldObject Self;
	if (Client->GetWorldObject(PendingSelfAppearanceGuid, Self))
	{
		Self.bIsSelf = true;
		UACECharacterAppearanceComponent* Comp = EnsurePawnAppearance();
		const bool bHadAppearance = Comp && Comp->HasAppearance();
		const uint64 AppearanceBefore = Comp ? Comp->GetAppliedAppearanceHash() : 0;
		ApplyPlayerCapsuleFromSetup(Self.SetupId);
		if (Comp)
		{
			Comp->ApplyWorldObject(Self, WorldScale);
			// Query-only Visibility so the local player can be hovered / selected / identified.
			Comp->ConfigurePartCollision(/*bBlocking*/ false, /*bQueryVisibilityOnly*/ false);
			ApplyPlayerCapsuleFromSetup(Self.SetupId);
		}
		const bool bAppearanceNoOp = bHadAppearance && Comp
			&& Comp->GetAppliedAppearanceHash() == AppearanceBefore;
		bool bScriptsJustCreated = false;
		if (APawn* P = GetPawn())
		{
			// The pawn is a plain engine pawn — the FX runtime must be created here, or every
			// PlayEffect/PlayScript for the local player guid queues forever (no cast particles).
			UACEScriptComponent* Scripts = P->FindComponentByClass<UACEScriptComponent>();
			if (!Scripts)
			{
				Scripts = NewObject<UACEScriptComponent>(P, TEXT("ACEScripts"));
				Scripts->RegisterComponent();
				Scripts->InitializeFromObject(Self, WorldScale);
				Scripts->NotifyAppearanceReady(/*bIsDoor*/ false);
				bScriptsJustCreated = true;
				UE_LOG(LogTemp, Warning,
					TEXT("ACE: local ScriptComponent ready peTable=0x%08X setup=0x%08X"),
					Self.PhysicsEffectTableId, Self.SetupId);
			}
			else if (Scripts->GetSetupId() != static_cast<uint32>(Self.SetupId) || !Scripts->IsEffectReady())
			{
				Scripts->InitializeFromObject(Self, WorldScale);
				Scripts->NotifyAppearanceReady(/*bIsDoor*/ false);
				bScriptsJustCreated = true;
			}
		}
		if (AGameModeBase* GM = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr)
		{
			if (UACEWorldPresenterComponent* WorldPres = GM->FindComponentByClass<UACEWorldPresenterComponent>())
			{
				if (!bAppearanceNoOp)
				{
					WorldPres->RehomePlayerChildren(PendingSelfAppearanceGuid);
				}
				if (bScriptsJustCreated || !bAppearanceNoOp)
				{
					WorldPres->DropPendingOneShotEffectsForGuid(PendingSelfAppearanceGuid);
					WorldPres->FlushPendingEffectsForGuid(PendingSelfAppearanceGuid);
				}
			}
		}
	}
	PendingSelfAppearanceGuid = 0;

	// Keep pawn hidden until portal-space finishes so clothing/weapon pop-in isn't visible.
	if (bEnterWorldLoading || bWorldRevealActive)
	{
		if (APawn* P = GetPawn())
		{
			P->SetActorHiddenInGame(true);
			P->SetActorEnableCollision(false);
		}
	}
	else if (APawn* P = GetPawn())
	{
		P->SetActorHiddenInGame(false);
		P->SetActorEnableCollision(true);
		if (UACECharacterAppearanceComponent* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			App->SetAppearanceVisible(true);
		}
		if (UACEScriptComponent* Scripts = P->FindComponentByClass<UACEScriptComponent>())
		{
			Scripts->RestoreMeshVisuals();
		}
	}
}

void AACEPlayerController::SoftReconcilePredictedTowardServer(const FACEPosition& Server, float DeltaTime, bool bAllowHeading)
{
	if (!Server.IsValid())
	{
		return;
	}
	if (!bHavePredictedPose)
	{
		PredictedPose = Server;
		bHavePredictedPose = true;
		return;
	}

	FVector PredUe = PredictedPose.ToUnrealLocation(WorldScale);
	const FVector SrvUe = Server.ToUnrealLocation(WorldScale);
	// A settled server pose also owns altitude. The old XY-only deadzone could
	// leave a stationary client above the bottom of a cliff indefinitely.
	if (!bLocalPredicting && !bJumpAirborne && FMath::Abs(SrvUe.Z-PredUe.Z) > 25.f)
	{
		PredUe.Z = SrvUe.Z;
		PredictedPose.SetLocationFromUnreal(PredUe, WorldScale);
	}
	const float Err2D = FVector::Dist2D(PredUe, SrvUe);

	// Catastrophic desync (lag spike / admin move) — snap XY, keep local Z for ground.
	if (Err2D >= ServerSnapErrorCm)
	{
		FVector Snapped = SrvUe;
		Snapped.Z = PredUe.Z;
		PredictedPose.SetLocationFromUnreal(Snapped, WorldScale);
		if (bAllowHeading)
		{
			PredictedPose.RotationW = Server.RotationW;
			PredictedPose.RotationXYZ = Server.RotationXYZ;
		}
		return;
	}

	// Small lag is expected while predicting — don't fight the local run.
	const float Deadzone = FMath::Max(0.f, ServerReconcileDeadzoneCm);
	if (Err2D <= Deadzone)
	{
		return;
	}

	const float Rate = FMath::Max(0.f, ServerReconcileRate);
	const float Alpha = (Rate <= KINDA_SMALL_NUMBER)
		? 0.f
		: (1.f - FMath::Exp(-Rate * FMath::Max(DeltaTime, 0.f)));
	if (Alpha <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// Only correct the excess beyond the deadzone so motion stays continuous.
	const FVector Delta2D(SrvUe.X - PredUe.X, SrvUe.Y - PredUe.Y, 0.f);
	const float Excess = Err2D - Deadzone;
	const FVector Correction = Delta2D.GetSafeNormal() * (Excess * Alpha);
	FVector Blended = PredUe + Correction;
	Blended.Z = PredUe.Z;
	PredictedPose.SetLocationFromUnreal(Blended, WorldScale);

	if (bAllowHeading)
	{
		const FQuat BlendedQ = FQuat::Slerp(
			PredictedPose.GetAcQuat(), Server.GetAcQuat(), Alpha).GetNormalized();
		PredictedPose.RotationW = BlendedQ.W;
		PredictedPose.RotationXYZ = FVector(BlendedQ.X, BlendedQ.Y, BlendedQ.Z);
	}
}

void AACEPlayerController::HandlePlayerTeleportStarted()
{
	// Death respawns reuse this pawn. An ordinary Ready echo is not evidence
	// of revival, but the server's teleport is: stop both the falling animation
	// and its held final frame before presenting the lifestone arrival.
	if (APawn* PlayerPawn = GetPawn())
		if (auto* Appearance = PlayerPawn->FindComponentByClass<UACECharacterAppearanceComponent>())
			Appearance->ClearDeathMotion();
	// Retail: SmartBox::HandlePlayerTeleport raises waiting_for_teleport on 0xF751 and the
	// tunnel goes up immediately — the destination UpdatePosition follows. Entering here
	// (instead of on the TeleportSeq bump) means the old world never flashes at the new
	// position before the portal covers it.
	if (bUsePortalTransitionOnTeleport && bUseEnterWorldLoadScreen
		&& !bEnterWorldLoading && !bWorldRevealActive
		&& Client && Client->GetSessionState() == EACESessionState::InWorld)
	{
		BeginWorldTransition(TEXT("teleport"));
	}
}

void AACEPlayerController::HandlePositionUpdate(int32 ObjectGuid, const FACEPosition& Position)
{
	if (!Client || ObjectGuid != Client->GetPlayerGuid() || !bSyncPossessedPawn)
	{
		return;
	}


	LastServerPose = Position;
	bHaveLastServerPose = true;

	const uint16 NewTeleportSeq = Client->GetTeleportSeq();
	const uint16 NewForcePositionSeq = Client->GetSession() ? Client->GetSession()->GetForcePositionSeq() : 0;
	const bool bServerForcedPosition = NewForcePositionSeq != LastAppliedForcePositionSeq;
	LastAppliedForcePositionSeq = NewForcePositionSeq;
	const uint32 NewCell = static_cast<uint32>(Position.CellId);
	const uint32 PredCell = bHavePredictedPose ? static_cast<uint32>(PredictedPose.CellId) : NewCell;
	const bool bTeleportSeqChanged = (NewTeleportSeq != LastAppliedTeleportSeq);
	// Some servers deliver the destination without a preceding teleport event.
	if (bTeleportSeqChanged)
		if (APawn* PlayerPawn = GetPawn())
			if (auto* Appearance = PlayerPawn->FindComponentByClass<UACECharacterAppearanceComponent>())
				Appearance->ClearDeathMotion();
	const bool bCellChanged = (NewCell != PredCell);
	// A delayed room/doorway cell is an ordinary movement acknowledgement.
	// Treating it as an arrival ran the 4m placement search against adjacent
	// room geometry and repeatedly pulled a moving VR body back across portals.
	// Only actual teleport/load transitions use arrival placement. Explicit
	// force-position messages retain their separate authoritative branch below.
	const bool bForceSnap = bTeleportSeqChanged
		|| bEnterWorldLoading || bWorldRevealActive;
	if (bCellChanged)
	{
		if (const auto* Debug = IConsoleManager::Get().FindConsoleVariable(TEXT("ace.Movement.Debug")); Debug && Debug->GetInt())
		{
			static double LastCorrectionLog = 0.;
			const double Now = FPlatformTime::Seconds();
			if (Now - LastCorrectionLog >= 1.)
			{
				LastCorrectionLog = Now;
				UE_LOG(LogTemp, Log, TEXT("ACE MoveCorrection: localCell=%08X serverCell=%08X arrival=%d forced=%d predicting=%d errorXY=%.2f"),
					PredCell, NewCell, bForceSnap, bServerForcedPosition, bLocalPredicting,
					bHavePredictedPose ? FVector::Dist2D(PredictedPose.ToUnrealLocation(WorldScale), Position.ToUnrealLocation(WorldScale)) : 0.);
			}
		}
	}

	if (bTeleportSeqChanged && bUsePortalTransitionOnTeleport && bUseEnterWorldLoadScreen
		&& !bEnterWorldLoading && !bWorldRevealActive
		&& Client->GetSessionState() == EACESessionState::InWorld)
	{
		BeginWorldTransition(TEXT("teleport"));
	}
	else if (bTeleportSeqChanged && (bEnterWorldLoading || bWorldRevealActive))
	{
		ResetWorldEntryDestination();
	}
	else if (bTeleportSeqChanged)
	{
		// Instant teleport (no tunnel): arrive on the server, then resume MoveToState / AutoPos.
		bPortalExitNotified = false;
		NotifyPortalArrivalIfNeeded();
		InvalidateMovementAfterTeleport();
	}

	APawn* P = GetPawn();
	if (!P)
	{
		LastAppliedTeleportSeq = NewTeleportSeq;
		return;
	}

	const FVector TargetFeet = Position.ToUnrealLocation(WorldScale);
	const FQuat TargetQ = Position.ToUnrealQuat();
	FVector Target = TargetFeet;
	float CapsuleHalfHeight = 88.f;
	if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
	{
		CapsuleHalfHeight = Cap->GetScaledCapsuleHalfHeight();
	}
	Target.Z += CapsuleHalfHeight;

	FACEPosition Corrected = Position;
	if (!bForceSnap)
	{
		// Retail SmartBox::HandleReceivedPosition retains the player's heading even
		// for FORCE_POSITION_TS corrections. XYZ is authoritative, but the facing
		// in an acknowledgement predates any input during the network roundtrip.
		if (bHavePredictedPose && PredictedPose.IsValid())
		{
			Corrected.RotationW = PredictedPose.RotationW;
			Corrected.RotationXYZ = PredictedPose.RotationXYZ;
		}
		else
		{
			// The packet parser has already replaced Client->GetPlayerPosition().
			// After prediction settles, the pawn is the surviving local heading.
			const FQuat LocalRotation = P->GetActorQuat();
			Corrected.RotationW = LocalRotation.W;
			Corrected.RotationXYZ = FVector(LocalRotation.X, -LocalRotation.Y, -LocalRotation.Z);
		}
	}

	if (bServerForcedPosition && !bForceSnap)
	{
		// Forced corrections are authoritative, including vertical position. They
		// are not portal arrivals and must not run recall placement searches.
		P->SetActorLocation(Target);
		PredictedPose = Corrected;
		Client->SetReportedPosition(Corrected);
		bHavePredictedPose = true;
		bJumpAirborne = false;
		bJumpAirborneSent = false;
		bJumpCharging = false;
		bStandingJumpLocked = false;
		bHaveJumpLaunchFacing = false;
		StandingJumpAimF = StandingJumpAimR = 0.f;
		JumpAirborneSeconds = JumpChargeExtent = 0.f;
		JumpWorldAceVelocity = FVector::ZeroVector;
		LandingWorldAceVelocity = FVector::ZeroVector;
		LandingRootTrack.Reset();
		JumpLocalAceVelocity = FVector::ZeroVector;
		StepHoldSeconds = 0.f;
		Client->SetForcePositionReporting(false);
		bForceMovementResend = true;
		if (auto* App = P->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			App->ResetWalkingMotion();
			App->ClearJumpMotionIfAny();
			App->SetSuppressLocoIdleBlend(false);
		}
		// SmartBox::HandleReceivedPosition acknowledges a force-position blip
		// immediately, with the corrected XYZ and the player's retained heading.
		// Waiting for input or the periodic report leaves the server correcting
		// against an obsolete position. Airborne reports remain suppressed.
		Client->FlushAutonomousPosition(Corrected.bIsGrounded);
		return;
	}

	if (bForceSnap)
	{
		if (auto* App = P->FindComponentByClass<UACECharacterAppearanceComponent>()) App->ResetWalkingMotion();
		// Lifestone / portal recalls often land on the weenie origin (crystal center).
		// If the capsule overlaps world geometry at the snap point, push out along facing.
		{
			float CapsuleRadius = 34.f;
			if (UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>())
			{
				CapsuleRadius = Cap->GetScaledCapsuleRadius();
			}
			FCollisionQueryParams Params(SCENE_QUERY_STAT(ACERecallDepenetrate), false, P);
			const auto CapsuleShape = GetPlayerCollisionBody();
			auto Occupied = [&](const FVector& At) -> bool
			{
				if (!GetWorld())
				{
					return false;
				}
				return ACEBodySweep::OverlapsBody(*GetWorld(),At,CapsuleShape,Params);
			};
			if (Occupied(Target))
			{
				// Retail Transition::FindPlacementPos: ring of heading steps, adjustDist = 4 AC.
				const float SphereRadCm = FMath::Max(48.f, CapsuleRadius);
				const float AdjustCm = 4.f * WorldScale;
				const int32 NumRings = FMath::Max(2, FMath::CeilToInt(AdjustCm / SphereRadCm));
				const float DistPerRing = AdjustCm / static_cast<float>(NumRings);
				bool bFound = false;
				for (int32 Ring = 1; Ring <= NumRings && !bFound; ++Ring)
				{
					const float Dist = DistPerRing * static_cast<float>(Ring);
					const float RadiansPerStep = PI * DistPerRing / SphereRadCm;
					const int32 Steps = FMath::Max(4, FMath::CeilToInt(RadiansPerStep * static_cast<float>(Ring) * 2.f));
					const float AngleStep = 360.f / static_cast<float>(Steps);
					for (int32 J = 0; J < Steps; ++J)
					{
						const float Rad = FMath::DegreesToRadians(AngleStep * static_cast<float>(J));
						FVector Candidate = Target + FVector(FMath::Sin(Rad) * Dist, FMath::Cos(Rad) * Dist, 0.f);
						Candidate.Z = Target.Z;
						if (!Occupied(Candidate))
						{
							Target = Candidate;
							bFound = true;
							break;
						}
					}
				}
			}
		}
		P->SetActorLocation(Target);
		P->SetActorRotation(TargetQ);
		Client->SetReportedPosition(Position);
		PredictedPose = Position;
		PredictedPose.SetLocationFromUnreal(
			FVector(Target.X, Target.Y, Target.Z - CapsuleHalfHeight), WorldScale);
		bHavePredictedPose = bLocalPredicting && !bEnterWorldLoading && !bWorldRevealActive;
		LastAppliedTeleportSeq = NewTeleportSeq;

		if (bEnterWorldLoading || bWorldRevealActive)
		{
			P->SetActorHiddenInGame(true);
			P->SetActorEnableCollision(false);
		}
		return;
	}

	LastAppliedTeleportSeq = NewTeleportSeq;

	// While predicting (or settling): TickLocalMovement owns the transform and soft-blends
	// toward LastServerPose — never hard-reset the capsule here.
	if (bLocalPredicting || bHavePredictedPose)
	{
		// The packet parser stores the server pose before broadcasting it. Keep
		// the presentation/reporting pose consistent with the body until the next
		// movement tick; LastServerPose above preserves the authoritative anchor.
		FACEPosition LocalPosition;
		if (TryGetLocallyPredictedPosition(LocalPosition)) Client->SetReportedPosition(LocalPosition);
		return;
	}

	// Fully idle: gently ease the capsule toward the server pose (no hard check/reset).
	const FVector Current = P->GetActorLocation();
	const float ErrCm = FVector::Dist(Current, Target);
	const float Alpha = 1.f - FMath::Exp(-ServerReconcileRate * (GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.016f));
	FVector Adjusted = ErrCm >= ServerSnapErrorCm ? Target : FMath::Lerp(Current, Target, Alpha);
	// A normal position acknowledgement is still movement through the world.
	// Only the explicit teleport/force branches above may bypass solids. This
	// also covers the frame after releasing movement, when prediction goes idle.
	if (auto* Cap = P->FindComponentByClass<UCapsuleComponent>(); Cap && GetWorld())
	{
		const auto Shape = GetPlayerCollisionBody();
		const FCollisionQueryParams Query(SCENE_QUERY_STAT(ACEIdleCorrection), true, P);
		FHitResult Hit;
		if (ACEBodySweep::Sweep(*GetWorld(), Hit, Current, Adjusted, Shape, Query))
			Adjusted = ACEBodySweep::SlideGrounded(*GetWorld(), Current, Adjusted, Hit, Shape, Query);
	}
	P->SetActorLocation(Adjusted);
	Corrected.SetLocationFromUnreal(Adjusted-FVector(0,0,CapsuleHalfHeight), WorldScale);
	Client->SetReportedPosition(Corrected);
}

void AACEPlayerController::HandleCharacterList(const TArray<FACECharacterInfo>& Characters, const FString& ServerName)
{
	bShowMouseCursor = true;

	// Returning from in-world logoff (or first auth): tear down gameplay HUD.
	if (GameHUDWidget || DatGameplayBinder)
	{
		DestroyGameHUD();
	}

	// Do not flip to CharacterManagement until DAT chrome textures resolve — keep login
	// on-screen so the player never sees an empty / half-drawn char-select flash.
	PendingCharSelectCharacters = Characters;
	PendingCharSelectServerName = ServerName;
	bPendingCharSelect = true;
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
		{
			Dat->SetWorldStreamingAllowed(false);
			if (!Dat->IsDatReady())
			{
				Dat->BeginBackgroundLoad();
			}
		}
	}
	if (!LoginWidget || !LoginWidget->IsInViewport())
	{
		ShowLoginUI();
	}
	if (LoginWidget)
	{
		LoginWidget->SetVisibility(ESlateVisibility::Visible);
		LoginWidget->SetStatus(TEXT("Loading character select..."));
	}
	TickPendingCharacterSelect();
}

void AACEPlayerController::HandleSessionStateChanged(EACESessionState NewState)
{
	// A saved FPS preference belongs to the gameplay HUD, not the login/lobby.
	if (NewState != EACESessionState::InWorld) UpdateFrameRateOverlay();
	if (NewState == EACESessionState::Connecting) WorldEntryFailureReason.Reset();
	if (NewState == EACESessionState::EnteringWorld && bUseEnterWorldLoadScreen)
		bPendingEnterWorldTransition = true; // Outside the Play button's Slate callback.
	if (NewState == EACESessionState::Disconnected || NewState == EACESessionState::Failed
		|| NewState == EACESessionState::CharacterSelect
		|| NewState == EACESessionState::AwaitCharacterList)
	{
		bPendingEnterWorldTransition = false;
		bDestinationStreamingStarted = false;
		if (bEnterWorldLoading || bWorldRevealActive || bLogOffPresentationActive)
		{
			bEnterWorldLoading = bWorldRevealActive = false;
			bLogOffPresentationActive = bLogOffWorldFadeStarted = false;
			if (LoadingScreenActor) { LoadingScreenActor->Destroy(); LoadingScreenActor = nullptr; }
			ResetIgnoreMoveInput();
			ResetIgnoreLookInput();
			if (GetPawn()) SetViewTarget(GetPawn());
		}
		PendingSelfAppearanceGuid = 0;
		PortalWorldRevealElapsed = -1.f;
		if (GetGameInstance())
			if (auto* Dat = GetGameInstance()->GetSubsystem<UACEDatSubsystem>()) Dat->SetInPortalSpace(false);
		if (PlayerCameraManager) PlayerCameraManager->UnlockFOV();
		if (APawn* P = GetPawn())
		{
			if (auto* Scripts = P->FindComponentByClass<UACEScriptComponent>())
			{
				Scripts->StopAllSounds();
				Scripts->StopAllEffects();
				Scripts->DestroyComponent();
			}
			if (auto* Appearance = P->FindComponentByClass<UACECharacterAppearanceComponent>()) Appearance->ClearAppearance();
			P->SetActorHiddenInGame(true);
		}
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			{
				Dat->SetWorldStreamingAllowed(false);
			}
		}
	}
	if (NewState == EACESessionState::Disconnected || NewState == EACESessionState::Failed)
	{
		bEnterWorldLoading = false;
		bWorldRevealActive = false;
		bAwaitingRecoveryDestination = false;
		if (LoadingScreenActor)
		{
			LoadingScreenActor->Destroy();
			LoadingScreenActor = nullptr;
		}
		bPendingCharSelect = false;
		PendingCharSelectCharacters.Reset();
		PendingCharSelectServerName.Reset();
		DestroyGameHUD();
		DestroyCharacterSelectUI();
		ShowLoginUI();
		if (LoginWidget && !WorldEntryFailureReason.IsEmpty()) LoginWidget->SetStatus(WorldEntryFailureReason);
	}
}

UACECharacterAppearanceComponent* AACEPlayerController::EnsurePawnAppearance()
{
	APawn* P = GetPawn();
	if (!P)
	{
		return nullptr;
	}
	UACECharacterAppearanceComponent* Comp = P->FindComponentByClass<UACECharacterAppearanceComponent>();
	if (!Comp)
	{
		Comp = NewObject<UACECharacterAppearanceComponent>(P, TEXT("ACEAppearance"));
		Comp->bUseWorldLighting = true;
		Comp->RegisterComponent();
		Comp->bHideOwnerMeshes = true;
	}
	return Comp;
}

void AACEPlayerController::HandleObjectCreated(const FACEWorldObject& Object)
{
	if (!bApplyLocalAppearance || !Object.bIsSelf)
	{
		return;
	}
	// Queue for next tick — never build DAT meshes inside PollSockets.
	PendingSelfAppearanceGuid = Object.Guid;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(this, &AACEPlayerController::ApplyPendingSelfAppearance);
	}
}

float AACEPlayerController::GetStepUpHeightCm() const
{
	// Retail PartArray.GetStepUpHeight = Setup.StepUpHeight * Scale.Z (no invented inflation).
	float StepAc = 0.5f;
	if (Client)
	{
		FACEWorldObject Self;
		if (Client->GetWorldObject(Client->GetPlayerGuid(), Self) && Self.SetupId != 0)
		{
			if (UGameInstance* GI = GetGameInstance())
			{
				if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
				{
					float IgnoredH = 0.f, IgnoredR = 0.f;
					uint32 IgnoredAnim = 0;
					float SetupStep = 0.5f;
					if (Dat->TryGetSetupPhysics(static_cast<uint32>(Self.SetupId), SetupStep, IgnoredH, IgnoredR, IgnoredAnim))
					{
						const float Scale = Self.Scale > KINDA_SMALL_NUMBER ? Self.Scale : 1.f;
						StepAc = FMath::Max(0.05f, SetupStep * Scale);
					}
				}
			}
		}
	}
	return FMath::Max(5.f, StepAc * WorldScale);
}

float AACEPlayerController::GetStepDownHeightCm() const
{
	// Retail PartArray.GetStepDownHeight = Setup.StepDownHeight * Scale.Z.
	// Used for CheckWalkable / precipice — do not inflate like client StepUp.
	float StepAc = 0.5f;
	if (Client)
	{
		FACEWorldObject Self;
		if (Client->GetWorldObject(Client->GetPlayerGuid(), Self) && Self.SetupId != 0)
		{
			if (UGameInstance* GI = GetGameInstance())
			{
				if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
				{
					float IgnoredUp = 0.f, IgnoredH = 0.f, IgnoredR = 0.f;
					uint32 IgnoredAnim = 0;
					float SetupStepDown = 0.5f;
					if (Dat->TryGetSetupPhysics(static_cast<uint32>(Self.SetupId), IgnoredUp, IgnoredH, IgnoredR, IgnoredAnim, &SetupStepDown))
					{
						const float Scale = Self.Scale > KINDA_SMALL_NUMBER ? Self.Scale : 1.f;
						StepAc = FMath::Max(0.f, SetupStepDown * Scale);
					}
				}
			}
		}
	}
	return StepAc * WorldScale;
}

float AACEPlayerController::GetUseCylinderDistanceCm(
	const FACEWorldObject& Target, const FVector& SelfOriginCm, const FVector& TargetOriginCm) const
{
	// Match retail MoveToManager::GetCurrentDistance and server
	// WorldObject.GetCylinderDistance, using the authored Setup dimensions.
	float SelfScale = 1.f;
	if (Client)
	{
		FACEWorldObject Self;
		if (Client->GetWorldObject(Client->GetPlayerGuid(), Self))
		{
			SelfScale = Self.Scale > KINDA_SMALL_NUMBER ? Self.Scale : 1.f;
		}
	}
	const float SelfRadiusAc = FMath::Max(0.f, PlayerSetupRadiusAc * SelfScale);
	const float SelfHeightAc = FMath::Max(0.f, PlayerSetupHeightAc * SelfScale);

	const float TargetScale = Target.Scale > KINDA_SMALL_NUMBER ? Target.Scale : 1.f;
	float TargetRadiusAc = 0.25f;
	float TargetHeightAc = 0.f;
	if (Target.SetupId != 0)
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			{
				float Step = 0.f, Height = 0.f, Radius = 0.25f;
				uint32 Anim = 0;
				if (Dat->TryGetSetupPhysics(static_cast<uint32>(Target.SetupId), Step, Height, Radius, Anim))
				{
					TargetRadiusAc = FMath::Max(0.f, Radius * TargetScale);
					TargetHeightAc = FMath::Max(0.f, Height * TargetScale);
				}
			}
		}
	}

	return ACERetailUseRange::CylinderDistance(SelfOriginCm, SelfRadiusAc * WorldScale, SelfHeightAc * WorldScale,
		TargetOriginCm, TargetRadiusAc * WorldScale, TargetHeightAc * WorldScale);
}

FACECollisionBody AACEPlayerController::GetPlayerCollisionBody() const
{
	const APawn* BodyPawn=GetPawn();
	const auto* Capsule=BodyPawn?BodyPawn->FindComponentByClass<UCapsuleComponent>():nullptr;
	FACECollisionBody Body(FCollisionShape::MakeCapsule(Capsule?Capsule->GetScaledCapsuleRadius():48.f,
		Capsule?Capsule->GetScaledCapsuleHalfHeight():91.75f));
	if (PlayerMovementSpheresAc.IsEmpty())return Body;
	const float Scale=GetLocalCreatureScale()*WorldScale;
	const FQuat Rotation=BodyPawn?BodyPawn->GetActorQuat():FQuat::Identity;
	Body.Count=FMath::Min(2,PlayerMovementSpheresAc.Num());
	for(int32 I=0;I<Body.Count;++I)
	{
		const auto& S=PlayerMovementSpheresAc[I];
		Body.Centers[I]=Rotation.RotateVector(FACEPosition::AceVectorToUnreal(FVector(S.X,S.Y,S.Z),Scale))
			-FVector(0,0,Body.HalfHeight);
		Body.Radii[I]=S.W*Scale;
	}
	return Body;
}

void AACEPlayerController::ApplyPlayerCapsuleFromSetup(int32 SetupId)
{
	APawn* P = GetPawn();
	if (!P)
	{
		return;
	}
	UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>();
	if (!Cap)
	{
		return;
	}

	float StepAc = 0.5f;
	float HeightAc = PlayerSetupHeightAc;
	float RadiusAc = PlayerSetupRadiusAc;
	uint32 DefaultAnim = 0;
	if (SetupId != 0)
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UACEDatSubsystem* Dat = GI->GetSubsystem<UACEDatSubsystem>())
			{
				Dat->TryGetSetupPhysics(static_cast<uint32>(SetupId), StepAc, HeightAc, RadiusAc, DefaultAnim);
			}
		}
	}

	float ObjScale = 1.f;
	if (Client)
	{
		FACEWorldObject Self;
		if (Client->GetWorldObject(Client->GetPlayerGuid(), Self) && Self.Scale > KINDA_SMALL_NUMBER)
		{
			ObjScale = Self.GetValidObjectScale();
		}
	}

	// Full Setup size for Use-radius math only.
	PlayerSetupHeightAc = FMath::Max(0.f, HeightAc);
	PlayerSetupRadiusAc = FMath::Max(0.f, RadiusAc);

	// Retail applies object scale to the authored body without human-size clamps.
	// Keep Setup.Height for the feet-to-pawn coordinate frame; actual movement
	// queries use the individual spheres (including their small foot offsets).
	float CollisionRadiusAc=FMath::Max(.001f,RadiusAc);
	PlayerMovementSpheresAc.Reset();
	if (auto* Dat=GetGameInstance()?GetGameInstance()->GetSubsystem<UACEDatSubsystem>():nullptr)
	{
		TArray<FACEDatCollisionShape> Spheres;
		if (Dat->GetSetupMovementSpheres(SetupId,Spheres))
			for(int32 I=0;I<FMath::Min(2,Spheres.Num());++I)
			{
				const auto& S=Spheres[I];
				if (!FMath::IsFinite(S.Radius) || S.Radius<=0 || FVector(S.Origin).ContainsNaN())continue;
				PlayerMovementSpheresAc.Add(FVector4f(S.Origin,S.Radius));
			}
	}
	if (!PlayerMovementSpheresAc.IsEmpty())CollisionRadiusAc=PlayerMovementSpheresAc[0].W;
	const float RadiusCm=CollisionRadiusAc*ObjScale*WorldScale;
	const float HalfHeightCm=FMath::Max(RadiusCm,FMath::Max(.001f,HeightAc)*ObjScale*WorldScale*.5f);

	const float OldHalf = Cap->GetScaledCapsuleHalfHeight();
	const FVector Loc = P->GetActorLocation();
	const float FeetZ = Loc.Z - OldHalf;

	// SetCapsuleSize takes unscaled dimensions. Object.Scale already contributes to
	// the world dimensions above and also scales the owning pawn.
	const float ShapeScale = FMath::Max(KINDA_SMALL_NUMBER, Cap->GetShapeScale());
	Cap->SetCapsuleSize(RadiusCm / ShapeScale, HalfHeightCm / ShapeScale, true);
	Cap->SetUseCCD(true);
	// World hover/select uses Visibility on capsules (not mesh parts).
	Cap->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	ApplyLocalPawnPkCollision();

	FVector NewLoc = Loc;
	NewLoc.Z = FeetZ + Cap->GetScaledCapsuleHalfHeight();
	P->SetActorLocation(NewLoc, false);

	if (UACECharacterAppearanceComponent* AppearanceComp =
		P->FindComponentByClass<UACECharacterAppearanceComponent>())
	{
		AppearanceComp->bAlignMeshToCapsuleBottom = true;
		AppearanceComp->RefreshMeshAlignment();
	}
	if (USpringArmComponent* Boom = P->FindComponentByClass<USpringArmComponent>())
	{
		ApplyRetailCameraPivot(Boom, Cap);
	}
}

void AACEPlayerController::ApplyLocalPawnPkCollision()
{
	APawn* P = GetPawn();
	if (!P)
	{
		return;
	}
	UCapsuleComponent* Cap = P->FindComponentByClass<UCapsuleComponent>();
	if (!Cap)
	{
		return;
	}
	bool bPk = false;
	if (Client)
	{
		bPk = ACEPlayerKillerStatus::BlocksPlayers(Client->GetPlayerVitals().PlayerKillerStatus);
		if (!bPk)
		{
			FACEWorldObject Self;
			if (Client->GetWorldObject(Client->GetPlayerGuid(), Self))
			{
				bPk = Self.IsPkOrPkLite();
			}
		}
	}
	Cap->SetCollisionResponseToChannel(ECC_Pawn, bPk ? ECR_Block : ECR_Ignore);
}

void AACEPlayerController::HandleVitalsUpdated(const FACEPlayerVitals& /*Vitals*/)
{
	ApplyLocalPawnPkCollision();
}

void AACEPlayerController::ClearServerMoveTo()
{
	ReadingMovementSequence = 0;
	bServerMoveToActive = false;
	bServerTurnActive = false;
	bServerControlledMotion = false;
	ServerMoveToTargetGuid = 0;
	ServerMoveToDistance = 0.6f;
	ServerMoveToSpeed = 1.f;
	ServerMoveToFailDistance = 0.f;
	bHaveApproachStartPose = false;
	UseApproachPhase = EACEUseApproachPhase::None;
	ApproachTargetDir = FVector::ZeroVector;
	bResendUseWhenInRange = false;
	LastApproachUseTime = 0.0;
	bApproachPickupIntoInventory = false;
	ApproachPickupAmount = 0;
	bApproachMoveOnly = false;
	bAwaitingUseDone = false;
	ApproachUseSendCount = 0;
	VendorOpenWaitSeconds = 0.f;
	ApproachStallSeconds = 0.f;
	ApproachStallLastLoc = FVector::ZeroVector;
	ApproachTurnSeconds = 0.f;
	if (Client)
	{
		Client->SetForcePositionReporting(false);
	}
}

void AACEPlayerController::HandleUseDone(uint32 /*Error*/)
{
	if (ReadingMovementSequence && GetPawn())
		if (auto* App=GetPawn()->FindComponentByClass<UACECharacterAppearanceComponent>()) App->FinishEmoteMotion();
	ReadingMovementSequence = 0;
	bAwaitingUseDone = false;
	if (bServerMoveToActive && !bApproachPickupIntoInventory)
	{
		ClearServerMoveTo();
	}
}

void AACEPlayerController::EndUseApproach()
{
	ClearServerMoveTo();
}

bool AACEPlayerController::IsInventoryInteractionActive() const
{
    return DatGameplayBinder&&DatGameplayBinder->IsInventoryInteractionActive();
}
void AACEPlayerController::EndCombatApproachTo(int32 TargetGuid)
{
	// Server combat MoveTo shares the use-approach controller. A dying target
	// can remain in awareness until its death animation finishes; it is no
	// longer a destination. Do not cancel a real Use/Give/pickup acknowledgment.
	if (TargetGuid && bServerMoveToActive && ServerMoveToTargetGuid == TargetGuid
		&& bApproachMoveOnly && !bResendUseWhenInRange && !bApproachPickupIntoInventory
		&& !bAwaitingUseDone && ApproachUseSendCount == 0 && Client && !Client->IsUseBusy())
		ClearServerMoveTo();
}

void AACEPlayerController::HandleMoveToFailed(uint32 /*Error*/)
{
	if (!bServerMoveToActive) return;
	ClearServerMoveTo();
	ForwardSent = RightSent = TurnSent = 0.f;
	bWasMoving = false;
	if (Client) Client->StopMovement();
}

void AACEPlayerController::SendInventoryPickup(int32 Guid, int32 Amount)
{
	if (!Client) return;
	if (DatGameplayBinder)
		DatGameplayBinder->PickupInventoryAmount(Guid, Amount);
	else Client->SendPutItemInContainer(Guid,
		Client->ResolvePickupContainer(Guid, GameHUDWidget ? GameHUDWidget->GetOpenPackGuid() : 0), 0);
}

void AACEPlayerController::BeginUseApproach(int32 TargetGuid, float DistanceAc, bool bPickupIntoInventory,
	bool bFireActionWhenInRange, int32 PickupAmount)
{
	if (IsWorldTransitionActive()) return;
	ApproachPickupAmount = bPickupIntoInventory
		? (PickupAmount > 0 ? PickupAmount : (DatGameplayBinder ? DatGameplayBinder->GetSelectedItemAmount(TargetGuid) : 0)) : 0;
	if (TargetGuid == 0)
	{
		return;
	}
	bManualMovementCommandSent = false; // The use approach now owns the pending stop.
	// The interaction takes over the previous mouse turn. Its delayed half-
	// second release must not become a new MoveToState after Use/Give: ACE treats
	// that as manual cancellation, which can strand a custom reward's callback.
	bMouseTurnSent = false;
	MouseTurnReportSeconds = 0.f;
	PendingMouseTurnDegrees = 0.f;
	if (bServerMoveToActive && ServerMoveToTargetGuid == TargetGuid)
	{
		// A repeated double-click must not toggle the object or reset its charge
		// origin while the server is still servicing the first Use request.
		if (bAwaitingUseDone) return;
		bResendUseWhenInRange = bFireActionWhenInRange;
		bApproachPickupIntoInventory = bPickupIntoInventory;
		bApproachMoveOnly = !bFireActionWhenInRange;
		// Fresh player-initiated interaction on the same target: allow one more Use send.
		ApproachUseSendCount = 0;
		VendorOpenWaitSeconds = 0.f;
		ServerMoveToDistance = DistanceAc;
		// Re-Use while Holding: resume Walking so we close any remaining gap then Use.
		if (UseApproachPhase == EACEUseApproachPhase::Holding
			|| UseApproachPhase == EACEUseApproachPhase::None)
		{
			UseApproachPhase = EACEUseApproachPhase::Walking;
			ApproachStallSeconds = 0.f;
		}
		if (Client)
		{
			Client->SetForcePositionReporting(true);
		}
		return;
	}
	bServerMoveToActive = true;
	ServerMoveToTargetGuid = TargetGuid;
	ServerMoveToDistance = DistanceAc;
	// The server supplies the limit, including MAX for ordinary ACE Use and
	// finite charge limits on servers that configure them. Never invent one.
	ServerMoveToFailDistance = 0.f;
	UseApproachPhase = EACEUseApproachPhase::Turning;
	ApproachTargetDir = FVector::ZeroVector;
	bResendUseWhenInRange = bFireActionWhenInRange;
	bApproachPickupIntoInventory = bPickupIntoInventory;
	bApproachMoveOnly = !bFireActionWhenInRange;
	ApproachUseSendCount = 0;
	VendorOpenWaitSeconds = 0.f;
	ApproachStallSeconds = 0.f;
	ApproachStallLastLoc = FVector::ZeroVector;
	ApproachTurnSeconds = 0.f;
	if (Client)
	{
		Client->SetForcePositionReporting(true);
		FACEPosition Seed = bHavePredictedPose ? PredictedPose : Client->GetPlayerPosition();
		if (Seed.IsValid())
		{
			PredictedPose = Seed;
			bHavePredictedPose = true;
			ApproachStartPose = Seed;
			bHaveApproachStartPose = true;
		}
		if (bFireActionWhenInRange && !bPickupIntoInventory)
		{
			// ItemHolder::UseObject sends Use before movement. This gives the
			// server a chance to reject the interaction or supply MoveTo distance,
			// charge limit and target; pre-walking used to bypass those rules.
			// MoveToState and Use share the ordered weenie queue. Install the
			// current pose before serializing that movement event; the separate
			// autonomous-position queue cannot establish ordering ahead of Use.
			if (bHavePredictedPose) Client->SetReportedPosition(PredictedPose);
			if (Client->GetSession()) Client->GetSession()->ClearHeldForwardCommand();
			Client->SendMovementEx(0.f, 0.f, 0.f, bRunning, false, !bJumpAirborne);
			ForwardSent = RightSent = TurnSent = 0.f;
			bWasMoving = false;
			Client->FlushAutonomousPosition(!bJumpAirborne);
			bAwaitingUseDone = true;
			ApproachUseSendCount = 1;
			bResendUseWhenInRange = false;
			Client->SendUseItem(TargetGuid);
		}
		FACEWorldObject Target;
		if (Client->GetWorldObject(TargetGuid, Target) && Target.bHasPosition && GetPawn())
		{
			// Protocol pose = cylinder bottom (same as tick approach); pawn location is capsule center.
			const FACEPosition SelfPose = bHavePredictedPose ? PredictedPose : Client->GetPlayerPosition();
			const FVector SelfLoc = SelfPose.IsValid()
				? SelfPose.ToUnrealLocation(WorldScale)
				: GetPawn()->GetActorLocation();
			const FVector TargetLoc = Target.Position.ToUnrealLocation(WorldScale);
			FVector To = TargetLoc - SelfLoc;
			To.Z = 0.f;
			if (!To.IsNearlyZero())
			{
				ApproachTargetDir = To.GetSafeNormal();
			}
			// Already inside UseRadius (cylinder gap) — Use immediately; never F=1 into a wall.
			const float UseRadiusCm = ServerMoveToDistance * WorldScale;
			if (GetUseCylinderDistanceCm(Target, SelfLoc, TargetLoc) <= UseRadiusCm)
			{
				UseApproachPhase = EACEUseApproachPhase::Holding;
				if (bFireActionWhenInRange && ApproachUseSendCount == 0)
				{
					if (bPickupIntoInventory)
					{
						SendInventoryPickup(TargetGuid, ApproachPickupAmount);
					}
					else
					{
						Client->SendUseItem(TargetGuid);
						bAwaitingUseDone = true;
					}
					++ApproachUseSendCount;
					bResendUseWhenInRange = Target.IsVendor();
				}
				else
				{
					// Give already in range — report pose; server MoveToChain finishes the hand-off.
					if (bHavePredictedPose)
					{
						Client->SetReportedPosition(PredictedPose);
					}
					Client->FlushAutonomousPosition(true);
				}
			}
		}
	}
}

void AACEPlayerController::PickupSelectedObject()
{
	if (!Client || IsWorldTransitionActive()) return;
	const FACESelectedObject Selected = Client->GetSelectedObject();
	FACEWorldObject Object;
	if (Selected.bValid && Client->GetWorldObject(Selected.Guid, Object) && Object.IsWorldLootable())
	{
		if (Client->IsUseBusy() || (Client->GetSession() && Client->GetSession()->IsInventoryTransferPending())) return;
		SendInventoryPickup(Object.Guid);
		return;
	}
	InteractWithSelectedObject();
}

void AACEPlayerController::InteractWithSelectedObject()
{
	if (!Client)
	{
		return;
	}
	const FACESelectedObject Sel = Client->GetSelectedObject();
	if (!Sel.bValid || Sel.Guid == 0)
	{
		if (DatGameplayBinder) DatGameplayBinder->CancelPendingUseWith();
		SetObjectCursorTargeting(1);
		return;
	}
	InteractWithObject(Sel.Guid);
}

void AACEPlayerController::InteractWithObject(int32 ObjectGuid)
{
	if (!Client || ObjectGuid == 0 || IsWorldTransitionActive())
	{
		return;
	}
	if (DatGameplayBinder && DatGameplayBinder->TryCompletePendingUseWithTarget(ObjectGuid))
	{
		return;
	}
	FACEWorldObject Obj;
	if (!Client->GetWorldObject(ObjectGuid, Obj))
	{
		if (Client->IsUseBusy())
		{
			return;
		}
		Client->SendUseItem(ObjectGuid);
		return;
	}

	if (Obj.IsDirectDoorUseBlocked())
	{
		// Shared session gate supplies the retail feedback without moving toward it.
		Client->SendUseItem(ObjectGuid);
		return;
	}
	// Authored UseRadius as-is (negative portals = overlap). Parser default is already 0.6.
	const float DistAc = Obj.UseRadius;
	const int32 PlayerGuid = Client->GetPlayerGuid();

	// Other players: open secure trade (give is via inventory drag).
	if (Obj.bIsPlayer && ObjectGuid != PlayerGuid)
	{
		Client->SendOpenTrade(ObjectGuid);
		UE_LOG(LogTemp, Log, TEXT("ACE: OpenTrade guid=0x%08X name='%s'"), ObjectGuid, *Obj.Name);
		return;
	}

	// F and the hand toolbar share the same server-confirmed inventory operation.
	if (Obj.WielderId == PlayerGuid)
	{
		// Pickup is PutItemInContainer, not Use: clothing has no ActOnUse.
		SendInventoryPickup(ObjectGuid);
		return;
	}
	if (Client->SortInventoryItem(ObjectGuid)) return;

	// F/hand on external loot shares retail pickup preference and stack handling.
	if (Obj.ContainerId != 0 && Obj.CurrentWieldedLocation == 0 && Obj.WielderId == 0
		&& Obj.ParentGuid == 0 && Obj.ContainerId != PlayerGuid)
	{
		SendInventoryPickup(ObjectGuid);
		UE_LOG(LogTemp, Log, TEXT("ACE: Loot pickup guid=0x%08X from=0x%08X"),
			ObjectGuid, Obj.ContainerId);
		return;
	}

	if (ACEInventoryRules::IsWorldPickupOnUse(Obj))
	{
		// ACCWeenieObject::UIAttemptPutInContainer sends the inventory request
		// immediately. The server supplies any MoveTo and completes the pickup.
		// Pre-walking here could stall against furniture without ever sending F's
		// request, and bypassed server use distances and inventory failure replies.
		if (Client->IsUseBusy() || (Client->GetSession() && Client->GetSession()->IsInventoryTransferPending())) return;
		SendInventoryPickup(ObjectGuid);
		UE_LOG(LogTemp, Log, TEXT("ACE: Pickup requested guid=0x%08X name='%s'"), ObjectGuid, *Obj.Name);
		return;
	}

	// Retail ItemHolder::UseObject honors useability before attackability. Cows
	// and other usable creatures must reach the server's Use/emote handler.
	if ((Obj.ItemType & ACEItemType::Creature) != 0 && Obj.IsAttackable() && !Obj.IsVendor()
		&& !ACEItemUseable::IsSourceUsable(Obj.ItemUseable))
	{
		UE_LOG(LogTemp, Log, TEXT("ACE: Select-only attackable guid=0x%08X name='%s'"), ObjectGuid, *Obj.Name);
		return;
	}

	// Stuck scenery (signs, etc.): identify/select only — no Use. Stuck + Usable (fountains,
	// levers, Font of Jojii) must still SendUseItem / approach.
	if ((Obj.ObjectDescriptionFlags & ACEObjectDescFlag::Stuck) != 0
		&& !Obj.IsDoor() && !Obj.IsOpenable() && !Obj.IsLifeStone() && !Obj.IsCorpse()
		&& !Obj.IsVendor() && !Obj.IsGameBoard()
		&& (Obj.ItemType & (ACEItemType::Creature | ACEItemType::Portal)) == 0
		&& !ACEItemUseable::IsSourceUsable(Obj.ItemUseable))
	{
		UE_LOG(LogTemp, Log, TEXT("ACE: Stuck non-usable guid=0x%08X name='%s'"), ObjectGuid, *Obj.Name);
		return;
	}

	if (Obj.ShouldClientApproachForUse())
	{
		if (Client->IsUseBusy())
		{
			return;
		}
		// Send Use once, then follow the server's approach through autonomous
		// positions. New MoveToState packets would cancel CreateMoveToChain.
		BeginUseApproach(ObjectGuid, DistAc, /*bPickupIntoInventory*/ false);
		UE_LOG(LogTemp, Log, TEXT("ACE: Use approach guid=0x%08X name='%s'"), ObjectGuid, *Obj.Name);
		return;
	}

	if (Client->IsUseBusy())
	{
		return;
	}
	Client->SendUseItem(ObjectGuid);
	UE_LOG(LogTemp, Log, TEXT("ACE: Use guid=0x%08X name='%s'"), ObjectGuid, *Obj.Name);
}

void AACEPlayerController::ReclaimManualMovement()
{
	// Retail CommandInterpreter::TakeControlFromServer stops the server's
	// forward substate and reapplies ALL held command lists, including turn
	// and sidestep. Comparing axes alone misses a cast arriving while those
	// keys are already held: its ServerControl stamp still needs acknowledging.
	if (bServerControlledMotion)
	{
		bForceMovementResend = true;
		// StopObjectCompletely removes cyclic states, not unfinished links.
		// Our cast one-shot already returns to locomotion at the link's end;
		// allow the independent sidestep/turn channels to move in the meantime.
	}
	bServerTurnActive = false;
	bServerControlledMotion = false;
}

void AACEPlayerController::PrepareManualCombat(bool bFinishJump)
{
	if (!Client || IsVRActive()) return;
	// StartAttackRequest finishes a pending jump charge, never an airborne arc.
	if (bFinishJump)
	{
		bJumpCharging = bStandingJumpLocked = false;
		JumpChargeExtent = 0.f;
		if (DatGameplayBinder) DatGameplayBinder->SetJumpChargeFraction(0.f);
	}
	// CommandInterpreter::MaybeStopCompletely preserves server-owned actions.
	if (bServerControlledMotion) return;
	ClearServerMoveTo();
	if (Client->GetSession()) Client->GetSession()->ClearHeldForwardCommand();
	MovementSuppressionSequence = ++MovementKeySequence;
	bManualMovementCommandSent = false;
	bAutoRun = bMouseForwardActive = false;
	PreviousManualForward = 0.f;
	ForwardAxis = RightAxis = TurnAxis = 0.f;
	ForwardSent = RightSent = TurnSent = 0.f;
	bWasMoving = bForceMovementResend = false;
	bMouseTurnSent = false;
	bRunningSent = bRunning;
	bJumpChargeSent = bJumpCharging;
	bJumpAirborneSent = bJumpAirborne;
	PendingMouseTurnDegrees = 0.f;
	if (bHavePredictedPose) Client->SetReportedPosition(PredictedPose);
	if (APawn* P = GetPawn())
		if (auto* Appearance = P->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			Appearance->SetWalkingTransitionsEnabled(false);
			Appearance->SetLocomotionInput(0.f, 0.f, false);
		}
	Client->SendMovementEx(0.f, 0.f, 0.f, bRunning, bStandingJumpLocked, !bJumpAirborne);
}

void AACEPlayerController::AdvanceServerTurn(FACEPosition& Pose, float Seconds)
{
	if (!bServerTurnActive || IsVRActive()) return;
	const FQuat Next = ACERetailTurn::Advance(Pose.GetAcQuat(), ServerTurnHeading, ServerTurnSpeed, Seconds);
	Pose.RotationW = Next.W;
	Pose.RotationXYZ = FVector(Next.X, Next.Y, Next.Z);
	if (Next.Equals(ServerTurnHeading, .000001f)) bServerTurnActive = false;
}

void AACEPlayerController::BeginServerMoveTo(const FACEObjectMotionState& Motion)
{
	if (IsVRActive()) return;
	if (bServerMoveToActive && ServerMoveToTargetGuid == Motion.MoveToTargetGuid)
	{
		// Repeated server MoveTo updates must not restart Turning after the straight run begins.
		ServerMoveToDistance = Motion.MoveToDistance;
		ServerMoveToSpeed = FMath::Max(0.f, Motion.MoveToSpeed);
		ServerMoveToFailDistance = Motion.MoveToFailDistance;
		return;
	}
	// Preserve client Use/pickup/give intent across a server MoveTo echo.
	// GiveObjectRequest arrives as MoveTo with neither flag — approach only (no SendUseItem).
	// Never re-arm Use while MoveOnly (give): Use runs StopExistingMoveToChains and cancels the turn-in.
	const bool bKeepPickup = bApproachPickupIntoInventory;
	const bool bKeepMoveOnly = bApproachMoveOnly && !bKeepPickup;
	const bool bFire = bKeepMoveOnly ? false : (bResendUseWhenInRange || bKeepPickup);
	// MoveToDistance can be negative (portal overlap UseRadius) — pass through unchanged.
	BeginUseApproach(Motion.MoveToTargetGuid, Motion.MoveToDistance, bKeepPickup, bFire, ApproachPickupAmount);
	bServerControlledMotion = true;
	ServerMoveToSpeed = FMath::Max(0.f, Motion.MoveToSpeed);
	ServerMoveToFailDistance = Motion.MoveToFailDistance;
}

void AACEPlayerController::HandleMotionUpdate(int32 ObjectGuid, const FACEObjectMotionState& Motion)
{
	if (!Client || ObjectGuid != Client->GetPlayerGuid())
	{
		return;
	}
	bServerControlledMotion = true;
	bManualMovementCommandSent = false;
	bAutoRun = false; // CommandInterpreter::LoseControlToServer
	if (ACEMotion::NormalizeCommand(Motion.ForwardCommand)==0x4000001cu
		|| ACEMotion::NormalizeCommand(Motion.ActionCommand)==0x4000001cu)
	{
		const uint64 PreviousReadingSequence=ReadingMovementSequence;
		ClearServerMoveTo(); // Reading supersedes approach; preserve the pending Use itself.
		bServerControlledMotion=true;
		ReadingMovementSequence=PreviousReadingSequence?PreviousReadingSequence:++MovementKeySequence;
		bMouseForwardActive=false;
		ForwardAxis=RightAxis=TurnAxis=ForwardSent=RightSent=TurnSent=0.f;
		bWasMoving=bForceMovementResend=false;
		if (GetPawn()) if (auto* App=GetPawn()->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			App->SetWalkingTransitionsEnabled(false);
			App->SetLocomotionInput(0.f,0.f,false);
		}
	}
	else if (Motion.MovementType==0 && !Client->IsUseBusy()) ReadingMovementSequence=0;
	bServerTurnActive = false;
	if (Motion.MovementType == 8 || Motion.MovementType == 9)
	{
		if (Motion.TurnToFlags & 0x10000u) ClearServerMoveTo();
		bServerControlledMotion = true;
		FACEPosition Pose = bHavePredictedPose ? PredictedPose : Client->GetPlayerPosition();
		if (Pose.IsValid())
		{
			float Heading = Motion.MoveToDesiredHeading;
			if (Motion.MovementType == 8)
			{
				FACEWorldObject Target;
				if (Client->GetWorldObject(Motion.MoveToTargetGuid, Target) && Target.bHasPosition)
				{
					const FVector Direction = Target.Position.ToUnrealLocation(WorldScale) - Pose.ToUnrealLocation(WorldScale);
					if (!Direction.IsNearlyZero()) Pose.SetAceFacingFromUnrealDir2D(Direction);
					Heading += Pose.GetAceHeadingDegrees();
				}
				else Heading = Motion.TurnToFallbackHeading;
			}
			Pose.SetAceFacingFromUnrealDir2D(FACEPosition::UnrealDirFromAceHeadingDegrees(Heading));
			ServerTurnHeading = Pose.GetAcQuat();
			ServerTurnSpeed = Motion.TurnToSpeed * (bRunningSent ? 1.5f : 1.f);
			bServerTurnActive = true;
		}
	}
	else if (Motion.MovementType == 6 && Motion.MoveToTargetGuid != 0)
	{
		BeginServerMoveTo(Motion);
	}
	else if (Motion.MovementType == 7 && bServerMoveToActive)
	{
		// MoveToPosition echo while already Use-approaching — keep target, refresh stop distance.
		// A give sends its own MoveToPosition (server CreateMoveToChain for GiveObjectRequest);
		// flipping to Use here cancelled the give chain with ActionCancelled.
		ServerMoveToDistance = Motion.MoveToDistance;
		bResendUseWhenInRange = !bApproachMoveOnly && ApproachUseSendCount == 0;
		ServerMoveToFailDistance = Motion.MoveToFailDistance;
	}
	else if (Motion.MovementType != 6 && Motion.MovementType != 7 && !Motion.bMoving)
	{
		// Charge cancelled / finished (YouChargedTooFar) — stop local run.
		// Only cancel when FailDistance was set (server MoveTo / melee charge).
		// Client Use approach keeps FailDistance=0 and must survive idle motion echoes.
		if (bServerMoveToActive && ServerMoveToFailDistance > 0.f
			&& (UseApproachPhase == EACEUseApproachPhase::Walking
				|| UseApproachPhase == EACEUseApproachPhase::Turning))
		{
			ClearServerMoveTo();
		}
	}

	// Server CurrentStyle / cast-attack Commands apply to the local pawn appearance too.
	// Ignore transient NonCombat from SwitchCombatStyles while CombatMode is still armed —
	// adopting it flashes magic→standing and MoveToState then locks peace on the server.
	if (APawn* PossessedPawn = GetPawn())
	{
		if (UACECharacterAppearanceComponent* App = PossessedPawn->FindComponentByClass<UACECharacterAppearanceComponent>())
		{
			if (Motion.CurrentStyle != 0)
			{
				const uint32 Style = static_cast<uint32>(Motion.CurrentStyle);
				const bool bStyleNonCombat = Style == ACEMotion::StanceNonCombat;
				const int32 Mode = Client->GetPlayerVitals().CombatMode;
				const bool bCombatModeActive = Client->GetPlayerVitals().bValid
					&& Mode != static_cast<int32>(ACECombatMode::NonCombat)
					&& Mode != 0;
				if (!bStyleNonCombat || !bCombatModeActive)
				{
					App->SetPreferredStyle(Motion.CurrentStyle);
				}
			}
			if (Motion.ActionCommand != 0)
			{
				const uint32 Act = static_cast<uint32>(Motion.ActionCommand);
				const uint32 Full = Act <= 0xFFFFu
					? ACEMotion::ExpandPackedCommand(static_cast<uint16>(Act))
					: Act;
				// A held forward pose survives independent sidestep/turn commands.
				const bool bHoldEmote = ACEMotion::IsHeldRestCommand(Full);
				App->PlayActionMotion(Motion.ActionCommand, Motion.ActionSpeed, Motion.CurrentStyle, bHoldEmote, Motion.bNewAction);
				if (!bHoldEmote || Motion.bNewAction)
				{
					for (int32 I=0;I<Motion.ActionFollowups.Num();++I)
					{
						const uint32 Next = ACEMotion::NormalizeCommand(static_cast<uint32>(Motion.ActionFollowups[I]));
						const bool Hold = ACEMotion::IsHeldRestCommand(Next);
						App->QueueActionMotion(Motion.ActionFollowups[I], Motion.FollowupSpeed(I), Motion.CurrentStyle, Hold);
					}
				}
			}
			else if (ACEMotion::NormalizeCommand(static_cast<uint32>(Motion.ForwardCommand)) == ACEMotion::Ready)
			{
				// Ready is carried as a substate, not an action. In particular it
				// completes Reload with the authored overhead nock/draw animation.
				App->FinishMissileMotion();
				App->FinishEmoteMotion();
			}
		}
	}
}

void AACEPlayerController::HandlePhysicsStateUpdate(int32 ObjectGuid, int32 PhysicsState)
{
	// Door opened (Ethereal) — end approach. bResendUseWhenInRange stays true while Holding
	// and retrying Use, so do not gate ClearServerMoveTo on that flag.
	if (bServerMoveToActive && ObjectGuid == ServerMoveToTargetGuid
		&& (PhysicsState & ACEPhysicsState::Ethereal) != 0)
	{
		ClearServerMoveTo();
	}

	// Retail portal completion clears the Hidden physics bit on the local player.
	if (Client && ObjectGuid == Client->GetPlayerGuid())
	{
		const bool bPhysicsHidden = (PhysicsState & ACEPhysicsState::Hidden) != 0;
		if (bPhysicsHidden && (bEnterWorldLoading || bWorldRevealActive || bUsePortalTransitionOnTeleport))
		{
			bWaitingForServerUnhide = true;
		}
		else if (!bPhysicsHidden)
		{
			bWaitingForServerUnhide = false;
		}
	}
}

bool AACEPlayerController::TryGetLocallyPredictedPosition(FACEPosition& OutPosition) const
{
	if (!bHavePredictedPose || !PredictedPose.IsValid() || bEnterWorldLoading || bWorldRevealActive
		|| !Client || Client->GetSessionState() != EACESessionState::InWorld) return false;
	const auto Session = Client->GetSession();
	// Presentation delegates may run before this controller's packet delegate.
	// Never mask a teleport/forced position that has not been applied yet.
	if (!Session || Session->GetTeleportSeq() != LastAppliedTeleportSeq
		|| Session->GetForcePositionSeq() != LastAppliedForcePositionSeq) return false;
	OutPosition = PredictedPose;
	return true;
}

int32 AACEPlayerController::GetEffectiveCellId() const
{
	if (bHavePredictedPose && PredictedPose.IsValid())
	{
		return PredictedPose.CellId;
	}
	if (Client)
	{
		const FACEPosition Pos = Client->GetPlayerPosition();
		if (Pos.IsValid())
		{
			return Pos.CellId;
		}
	}
	return 0;
}

bool AACEPlayerController::GetHoverTooltip(FString& OutText, FVector2D& OutCursorPixels) const
{
	if (!bHoverTooltipVisible || HoverTooltipText.IsEmpty())
	{
		return false;
	}
	OutText = HoverTooltipText;
	OutCursorPixels = HoverCursorPixels;
	return true;
}

void AACEPlayerController::EnsureHoverTooltipWidget()
{
	if (HoverTooltipWidget)
	{
		return;
	}
	HoverTooltipWidget = CreateWidget<UACEHoverTooltipWidget>(this, UACEHoverTooltipWidget::StaticClass());
	if (!HoverTooltipWidget)
	{
		return;
	}
	// Above login (9999) and any other Slate chrome so the blurb is never buried.
	HoverTooltipWidget->AddToPlayerScreen(10050);
	HoverTooltipWidget->SetVisibility(ESlateVisibility::Collapsed);
}

bool AACEPlayerController::IsMouseOverBlockingUI() const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}

	// Must use absolute desktop coords — GetMousePosition() is viewport-local and made
	// LocateWindowUnderMouse always hit the wrong Visible chrome (blocking all world picks).
	const FVector2D ScreenPos = FSlateApplication::Get().GetCursorPos();

	// DAT-driven HUD: full-screen canvas uses SelfHitTestInvisible image chrome, so Slate
	// path often misses SBorder. Hit-test the element tree so spell/panel clicks don't
	// fall through to PollObjectClick (which clears the selected target).
	if (bUseDatDrivenHud && DatCanvasWidget
		&& DatCanvasWidget->GetVisibility() != ESlateVisibility::Collapsed
		&& DatCanvasWidget->GetVisibility() != ESlateVisibility::Hidden
		&& Client)
	{
		if (UACEUIElementManager* Mgr = Client->GetUIElementManager())
		{
			const FGeometry& Geo = DatCanvasWidget->GetCachedGeometry();
			const FVector2D Local = Geo.AbsoluteToLocal(ScreenPos);
			int32 Cx = 0, Cy = 0;
			if (Mgr->ViewportToCanvas(Local, Geo.GetLocalSize(), Cx, Cy)
				&& Mgr->IsCanvasOverUI(Cx, Cy))
			{
				return true;
			}
		}
	}

	FWidgetPath WidgetPath = FSlateApplication::Get().LocateWindowUnderMouse(
		ScreenPos, FSlateApplication::Get().GetInteractiveTopLevelWindows(), false);
	if (!WidgetPath.IsValid())
	{
		return false;
	}

	auto IsInteractiveControl = [](const SWidget& W) -> bool
	{
		const FString Type = W.GetTypeAsString();
		// SBorder/SImage: HUD chrome (AddArt/AddPanel) binds mouse handlers — must block
		// world pick when the cursor is over those panels.
		return Type == TEXT("SButton")
			|| Type == TEXT("SBorder")
			|| Type == TEXT("SImage")
			|| Type == TEXT("SScrollBar")
			|| Type == TEXT("SScrollBox")
			|| Type.Contains(TEXT("EditableText"))
			|| Type.Contains(TEXT("RetailTextEntry"))
			|| Type.Contains(TEXT("SCheckBox"))
			|| Type.Contains(TEXT("SSlider"))
			|| Type.Contains(TEXT("SCombo"));
	};

	for (int32 i = WidgetPath.Widgets.Num() - 1; i >= 0; --i)
	{
		const TSharedRef<SWidget>& Widget = WidgetPath.Widgets[i].Widget;
		const EVisibility Vis = Widget->GetVisibility();
		if (Vis == EVisibility::Collapsed || Vis == EVisibility::Hidden
			|| Vis == EVisibility::HitTestInvisible || Vis == EVisibility::SelfHitTestInvisible)
		{
			continue;
		}
		if (Vis == EVisibility::Visible && IsInteractiveControl(*Widget))
		{
			return true;
		}
	}
	return false;
}

namespace
{
	struct FACEVisibilityPick
	{
		AACEWorldEntityActor* Entity = nullptr;
		bool bHitSelf = false;
	};

	AACEWorldEntityActor* AsWorldEntity(AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}
		if (AACEWorldEntityActor* Entity = Cast<AACEWorldEntityActor>(Actor))
		{
			return Entity;
		}
		return Cast<AACEWorldEntityActor>(Actor->GetOwner());
	}

	FACEVisibilityPick ResolveVisibilityPick(UWorld& World, const FVector& Start, const FVector& End, APawn* SelfPawn, bool IncludeSelf = true)
	{
		AActor* Actor = ACEVisibleObjectPick::Trace(World, Start, End, SelfPawn, nullptr, IncludeSelf);
		FACEVisibilityPick Out;
		Out.bHitSelf = Actor && Actor == SelfPawn;
		Out.Entity = Out.bHitSelf ? nullptr : AsWorldEntity(Actor);
		return Out;
	}
}

bool AACEPlayerController::GetWorldPointerPosition(float& X, float& Y) const
{
	// SceneViewport's cached mouse position stops updating while an inventory
	// widget captures the drag. Use the same desktop-to-viewport conversion as
	// the release/drop path so hovering cannot remain at the press location.
	auto* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	const auto LayerManager = Viewport ? Viewport->GetGameLayerManager() : nullptr;
	if (Viewport && Viewport->Viewport && FSlateApplication::IsInitialized() && LayerManager.IsValid()
		&& LayerManager->GetViewportWidgetHostGeometry().GetLocalSize().GetMin() > 0.f)
	{
		FVector2D Pixels, Local, Size=FVector2D::ZeroVector;
		USlateBlueprintLibrary::AbsoluteToViewport(this, FSlateApplication::Get().GetCursorPos(), Pixels, Local);
		Viewport->GetViewportSize(Size);
		X=Pixels.X; Y=Pixels.Y;
		return X>=0 && Y>=0 && X<Size.X && Y<Size.Y;
	}
	return !(DatGameplayBinder && DatGameplayBinder->IsInventoryDragActive()) && GetMousePosition(X,Y);
}

AACEWorldEntityActor* AACEPlayerController::PickWorldPointer(bool& bHitSelf) const
{
	bHitSelf=false;
	if (!GetWorld()) return nullptr;
	FVector Origin,Direction;
	if (bMouseLookActive)
	{
		FRotator ViewRotation; GetPlayerViewPoint(Origin,ViewRotation);
		Direction=ViewRotation.Vector();
	}
	else
	{
		float X=0,Y=0;
		if (!GetWorldPointerPosition(X,Y) || !DeprojectScreenPositionToWorld(X,Y,Origin,Direction)) return nullptr;
	}
	const auto Pick=ResolveVisibilityPick(*GetWorld(),Origin,Origin+Direction*200000.f,GetPawn(),!bMouseLookActive);
	bHitSelf=Pick.bHitSelf;
	return Pick.Entity;
}

void AACEPlayerController::UpdateObjectHighlights(bool bEnabled, bool bDesktop)
{
	AACEWorldEntityActor* Selection=nullptr;
	AACEWorldEntityActor* Hover=nullptr;
	UACECharacterAppearanceComponent* PlayerAppearance=nullptr;
	if (bEnabled && Client && Client->GetSessionState()==EACESessionState::InWorld
		&& ACERuntimeOptions::Get(TEXT("ObjectGlow"))>.5f)
	{
		const int32 SelectedGuid=Client->GetSelectedObject().Guid;
		if (SelectedGuid && SelectedGuid==Client->GetPlayerGuid())
		{
			// Self is rendered by the possessed pawn, not the world presenter.
			// Share the same glow in desktop and VR without adding a HUD marker.
			const auto Session=Client->GetSession();
			const auto* Self=Session?Session->GetWorldObjects().Find(SelectedGuid):nullptr;
			if (APawn* LocalPawn=GetPawn(); LocalPawn && !LocalPawn->IsHidden() && Self
				&& !(Self->ObjectDescriptionFlags & ACEObjectDescFlag::UiHidden) && !(Self->PhysicsState & ACEPhysicsState::NoDraw))
				PlayerAppearance=LocalPawn->FindComponentByClass<UACECharacterAppearanceComponent>();
		}
		if (bDesktop)
		{
			Hover=DesktopHover.Get();
			if (auto* Mode=GetWorld()->GetAuthGameMode())
				if (auto* Presenter=Mode->FindComponentByClass<UACEWorldPresenterComponent>())
					Selection=Presenter->FindEntityActor(SelectedGuid);
		}
	}
	auto Visible=[this](AACEWorldEntityActor* Actor)
	{
		return Actor && !Actor->IsHidden() && Actor->IsCellVisible()
			&& (!Client || Actor->GetACEGuid()!=Client->GetPlayerGuid())
			&& !(Actor->ObjectDescriptionFlags & ACEObjectDescFlag::UiHidden)
			&& !(Actor->PhysicsState & ACEPhysicsState::NoDraw);
	};
	if (!Visible(Selection)) Selection=nullptr;
	if (!Visible(Hover)) Hover=nullptr;
	for (auto Previous:{DesktopHighlightSelection,DesktopHighlightHover})
		if (auto* Actor=Previous.Get(); Actor && Actor!=Selection && Actor!=Hover) Actor->SetSelectionHighlight(0.f);
	DesktopHighlightSelection=Selection; DesktopHighlightHover=Hover;
	if (Hover && Hover!=Selection) Hover->SetSelectionHighlight(.45f);
	if (Selection) Selection->SetSelectionHighlight(1.f);
	if (auto* Previous=HighlightedPlayerAppearance.Get(); Previous && Previous!=PlayerAppearance)
		Previous->SetSelectionHighlight(0.f);
	HighlightedPlayerAppearance=PlayerAppearance;
	if (PlayerAppearance) PlayerAppearance->SetSelectionHighlight(1.f);
}

void AACEPlayerController::PollObjectHover()
{
	DesktopHover.Reset();
	bHoverTooltipVisible = false;
	HoverTooltipText.Reset();

	auto HideTip = [this]()
	{
		if (HoverTooltipWidget)
		{
			HoverTooltipWidget->SetVisibility(ESlateVisibility::Collapsed);
		}
		if (bRetailCursorInstalled)
		{
			ApplyRetailMouseCursor(false);
		}
	};

	if (!Client || Client->GetSessionState() != EACESessionState::InWorld)
	{
		ObjectCursorTargetMode=0;
		bPendingUseTargeting=false;
		HideTip();
		return;
	}

	if (bMouseLookActive)
	{
		bool bHitSelf=false; DesktopHover=PickWorldPointer(bHitSelf);
		HideTip(); return;
	}

	EnsureHoverTooltipWidget();

	float MouseX = 0.f, MouseY = 0.f;
	if (!GetWorldPointerPosition(MouseX, MouseY))
	{
		HideTip();
		return;
	}
	HoverCursorPixels = FVector2D(MouseX, MouseY);
	uint32 TooltipTemplate = UACEHoverTooltipWidget::OptionsTemplate;
	if (!IsDesktopInterfaceHidden() && DatGameplayBinder && FSlateApplication::IsInitialized()
		&& DatGameplayBinder->GetStatTooltipAt(FSlateApplication::Get().GetCursorPos(), HoverTooltipText, &TooltipTemplate))
	{
		if (bRetailCursorInstalled) ApplyRetailMouseCursor(false);
		bHoverTooltipVisible = true;
		if (HoverTooltipWidget)
		{
			HoverTooltipWidget->SetTooltipText(HoverTooltipText, TooltipTemplate);
			HoverTooltipWidget->SetTooltipScreenPosition(HoverCursorPixels);
			HoverTooltipWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		return;
	}

	if (IsMouseOverBlockingUI())
	{
		HideTip();
		if ((bPendingUseTargeting||ObjectCursorTargetMode) && DatGameplayBinder && FSlateApplication::IsInitialized())
		{
			const int32 Target=DatGameplayBinder->GetInventoryTargetAt(FSlateApplication::Get().GetCursorPos());
			ApplyRetailMouseCursor(Target!=0,DatGameplayBinder->IsPendingUseTargetCompatible(Target));
		}
		return;
	}

	bool bHitSelf=false;
	AACEWorldEntityActor* Entity=PickWorldPointer(bHitSelf);
	DesktopHover=Entity;
	if (IsDesktopInterfaceHidden()) { HideTip(); return; }
	if (!Entity && !bHitSelf)
	{
		HideTip();
		return;
	}

	FString Name;
	if (bHitSelf)
	{
		FACEWorldObject SelfObj;
		if (Client->GetWorldObject(Client->GetPlayerGuid(), SelfObj))
		{
			Name = SelfObj.Name;
		}
		if (Name.IsEmpty())
		{
			Name = TEXT("You");
		}
	}
	else
	{
		Name = Entity->ACEName;
		if (Name.IsEmpty())
		{
			FACEWorldObject Obj;
			if (Client->GetWorldObject(Entity->GetACEGuid(), Obj))
			{
				Name = Obj.Name;
			}
		}
		if (Name.IsEmpty())
		{
			Name = FString::Printf(TEXT("0x%08X"), Entity->GetACEGuid());
		}
	}

	HoverTooltipText = Name;
	bHoverTooltipVisible = true;

	if (bRetailCursorInstalled)
	{
		const int32 Target=bHitSelf ? Client->GetPlayerGuid() : Entity->GetACEGuid();
		ApplyRetailMouseCursor(true,!DatGameplayBinder || !bPendingUseTargeting
			|| DatGameplayBinder->IsPendingUseTargetCompatible(Target));
	}

	if (HoverTooltipWidget)
	{
		HoverTooltipWidget->SetTooltipText(Name);
		HoverTooltipWidget->SetTooltipScreenPosition(HoverCursorPixels);
		HoverTooltipWidget->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
}

void AACEPlayerController::CycleNearbyTarget(bool bEnemies, int32 Direction)
{
	if (const int32 Target = FindNearbyTarget(bEnemies,Direction)) Client->SelectObject(Target);
}

void AACEPlayerController::SelectLastAttacker()
{
	if (!Client || Client->GetSessionState() != EACESessionState::InWorld || !Client->GetSession()) return;
	const int32 Attacker = Client->GetSession()->GetLastAttackerGuid();
	FACEWorldObject Object;
	if (!Attacker || Attacker == Client->GetPlayerGuid() || !Client->GetWorldObject(Attacker, Object)
		|| !Object.IsSelectableWorldObject()) return;
	FACEPosition PlayerPosition = Client->GetPlayerPosition();
	TryGetLocallyPredictedPosition(PlayerPosition);
	if (!PlayerPosition.IsValid()) return;
	// CPlayerSystem's Last Attacker action has no 15-second expiry. It requires
	// a known world object strictly inside the planar indoor/outdoor radar radius.
	const double RangeSquared = FMath::Square(ACERadarVisuals::RangeAc(uint32(PlayerPosition.CellId)));
	if (FVector::DistSquaredXY(Object.Position.ToUnrealLocation(1.f), PlayerPosition.ToUnrealLocation(1.f)) < RangeSquared)
		Client->SelectObject(Attacker);
}

int32 AACEPlayerController::FindNearbyTarget(bool bEnemies, int32 Direction, int32 ExcludeGuid) const
{
	if (!Client) return 0;
	FACEPosition PlayerPosition=Client->GetPlayerPosition();
	TryGetLocallyPredictedPosition(PlayerPosition);
	if (!PlayerPosition.IsValid()) return 0;
	const FVector Origin = PlayerPosition.ToUnrealLocation(1.f);
	const double RangeSquared=FMath::Square(ACERadarVisuals::RangeAc(uint32(PlayerPosition.CellId)));
	const auto Fellowship = Client->GetFellowship();
	TArray<FACEWorldObject> Objects = Client->GetWorldObjects();
	FACEWorldObject Self;
	const FACEWorldObject* SelfObject = Client->GetWorldObject(Client->GetPlayerGuid(), Self) ? &Self : nullptr;
	Objects.RemoveAll([&](const FACEWorldObject& Obj)
	{
		return !Obj.IsSelectableWorldObject()
			|| FVector::DistSquaredXY(Obj.Position.ToUnrealLocation(1.f),Origin)>RangeSquared
			|| Obj.Guid == Client->GetPlayerGuid() || Obj.Guid == ExcludeGuid
			|| (bEnemies ? (!ACECombatTargeting::CanAttack(Obj, SelfObject) || Obj.PetOwnerId != 0 || Obj.IsVendor())
				: (Obj.bIsPlayer || (Obj.ItemType & ACEItemType::Creature) != 0))
			|| (bEnemies && Fellowship.Members.ContainsByPredicate([&](const auto& Member) { return Member.Guid == Obj.Guid; }));
	});
	// CPlayerSystem::SelectNext limits candidates by planar radar radius first,
	// then orders them by planar distance plus weighted height. Retail's object
	// visibility table is server awareness, not the camera's drawing frustum.
	auto Distance = [&](const FACEWorldObject& Obj)
	{
		const FVector Delta = Obj.Position.ToUnrealLocation(1.f)-Origin;
		return Delta.Size2D()+FMath::Abs(Delta.Z)*1.2;
	};
	Objects.Sort([&](const FACEWorldObject& A, const FACEWorldObject& B)
	{
		const double DA = Distance(A), DB = Distance(B);
		return DA == DB ? static_cast<uint32>(A.Guid) < static_cast<uint32>(B.Guid) : DA < DB;
	});
	if (Objects.IsEmpty()) return 0;
	const int32 Current = Objects.IndexOfByPredicate([&](const FACEWorldObject& O) { return O.Guid == Client->GetSelectedObject().Guid; });
	const int32 Next = Current == INDEX_NONE || Direction == 0 ? 0 : (Current + Direction + Objects.Num()) % Objects.Num();
	return Objects[Next].Guid;
}

void AACEPlayerController::IdentifyAtScreenPosition(float MouseX, float MouseY)
{
	if (!Client || Client->GetSessionState() != EACESessionState::InWorld)
	{
		return;
	}
	if (IsMouseOverBlockingUI())
	{
		return;
	}
	FVector WorldOrigin, WorldDir;
	if (!DeprojectScreenPositionToWorld(MouseX, MouseY, WorldOrigin, WorldDir))
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FVector TraceEnd = WorldOrigin + WorldDir * 200000.f;
	APawn* SelfPawn = GetPawn();
	const FACEVisibilityPick Pick = ResolveVisibilityPick(*World, WorldOrigin, TraceEnd, SelfPawn);
	const int32 SelfGuid = Client->GetPlayerGuid();
	if (Pick.Entity)
	{
		Client->SelectObject(Pick.Entity->GetACEGuid());
		Client->SendIdentifyObject(Pick.Entity->GetACEGuid());
	}
	else if (Pick.bHitSelf && SelfGuid != 0)
	{
		Client->SelectObject(SelfGuid);
		Client->SendIdentifyObject(SelfGuid);
	}
}

void AACEPlayerController::PollObjectClick()
{
	if (!Client || Client->GetSessionState() != EACESessionState::InWorld)
	{
		return;
	}

	const bool bLeft = WasInputKeyJustPressed(EKeys::LeftMouseButton);
	if (!bLeft)
	{
		return;
	}

	if (!bMouseLookActive && IsMouseOverBlockingUI())
	{
		return;
	}

	if (DatGameplayBinder)
	{
		DatGameplayBinder->ClearChatEntryFocus();
	}

	bool bHitSelf=false;
	AACEWorldEntityActor* Entity=PickWorldPointer(bHitSelf);

	const int32 SelfGuid = Client->GetPlayerGuid();
	if (!Entity && !bHitSelf)
	{
		if (bLeft && DatGameplayBinder)
		{
			DatGameplayBinder->CancelPendingUseWith();
		}
		Client->SelectObject(0);
		LastLeftClickGuid = 0;
		LastLeftClickTime = 0.0;
		return;
	}

	const int32 Guid = Entity ? Entity->GetACEGuid() : (bHitSelf ? SelfGuid : 0);
	if (Guid == 0)
	{
		return;
	}

	// Dual-use (mana stone / key): single click completes UseWithTarget on self or world object.
	if (bLeft && (bPendingUseTargeting||ObjectCursorTargetMode) && DatGameplayBinder)
	{
		Client->SelectObject(Guid);
		if (DatGameplayBinder->TryCompletePendingUseWithTarget(Guid))
		{
			LastLeftClickGuid = 0;
			LastLeftClickTime = 0.0;
			return;
		}
	}

	const FACESelectedObject PriorSel = Client->GetSelectedObject();
	const double Now = FPlatformTime::Seconds();
	// True double-click only — two presses on the same guid within the window.
	// Do NOT treat "already selected" as a free second click (that made any later click Use).
	constexpr double DoubleClickSeconds = 0.5;
	const bool bDouble = (Guid == LastLeftClickGuid)
		&& ((Now - LastLeftClickTime) < DoubleClickSeconds);
	LastLeftClickGuid = Guid;
	LastLeftClickTime = Now;

	// First click: select (QueryHealth keeps server target for the health bar).
	Client->SelectObject(Guid);
	if (bHitSelf && !Entity)
	{
		return;
	}
	if (bDouble)
	{
		InteractWithObject(Guid);
		// Consume the pair so a third click is a fresh select, not another Use.
		LastLeftClickGuid = 0;
		LastLeftClickTime = 0.0;
	}
	(void)PriorSel;
}

AACEWorldEntityActor* AACEPlayerController::PickWorldEntityUnderCursor() const
{
	float MouseX = 0.f, MouseY = 0.f;
	if (!GetWorldPointerPosition(MouseX, MouseY))
	{
		return nullptr;
	}
	return PickWorldEntityAtScreenPosition(MouseX, MouseY);
}

namespace ACEPickTargeting
{
	bool IsSameFloorPickTarget(
		const FACEPosition& PlayerPos, const FACEWorldObject& Obj, float WorldScale,
		float OutdoorZFudgeCm = 180.f)
	{
		if (!PlayerPos.IsValid() || !Obj.bHasPosition)
		{
			return true;
		}
		const uint32 PC = static_cast<uint32>(PlayerPos.CellId);
		const uint32 OC = static_cast<uint32>(Obj.Position.CellId);
		const bool bPlayerIndoor = (PC & 0xFFFFu) >= 0x0100u;
		const bool bObjIndoor = (OC & 0xFFFFu) >= 0x0100u;
		if (bPlayerIndoor && bObjIndoor)
		{
			return PC == OC;
		}
		const float Dz = FMath::Abs(
			PlayerPos.ToUnrealLocation(WorldScale).Z - Obj.Position.ToUnrealLocation(WorldScale).Z);
		return Dz <= OutdoorZFudgeCm;
	}
}

AACEWorldEntityActor* AACEPlayerController::PickWorldEntityAtScreenPosition(float ScreenX, float ScreenY) const
{
	if (IsVRActive()) { FVector Impact; return GetPawn()->FindComponentByClass<UACEVRComponent>()->AimTarget(Impact); }
	FVector WorldOrigin, WorldDir;
	if (!DeprojectScreenPositionToWorld(ScreenX, ScreenY, WorldOrigin, WorldDir))
	{
		return nullptr;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	AACEWorldEntityActor* Candidate = ResolveVisibilityPick(*World, WorldOrigin,
		WorldOrigin + WorldDir * 200000.f, GetPawn()).Entity;
	if (!Candidate || Candidate->GetParentGuid() != 0) return nullptr;
	return Candidate;
}

void AACEPlayerController::ShowCharacterCreationUI()
{
    if(!Client||!Client->GetSession()||!DatCanvasWidget||Client->GetSessionState()!=EACESessionState::CharacterSelect)return;
    if(Client->GetCharacters().Num()>=Client->GetSession()->GetCharacterSlotCount())return;
    if(DatCharSelectBinder){DatCharSelectBinder->Shutdown();DatCharSelectBinder=nullptr;}
    DatCanvasWidget->SetCharSelectBinder(nullptr);
    Client->GetUIFlow()->SetMode(ACEUI::EACEUIFlowMode::CharacterCreation);
    DatCharGenBinder=NewObject<UACEUICharGenBinder>(this);
    if(!DatCharGenBinder->Initialize(Client,DatCanvasWidget,this))
    {DatCharGenBinder->Shutdown();DatCharGenBinder=nullptr;ShowCharacterSelectUI(Client->GetCharacters(),Client->GetSession()->GetServerName());return;}
    DatCanvasWidget->SetCharGenBinder(DatCharGenBinder);
    // The existing canvas stays attached to the VR panel, so UpdatePanels will
    // not perform another input-mode handoff when character creation opens.
    if(IsVRActive()){ApplyInWorldInputMode();return;}
    FInputModeGameAndUI Mode;Mode.SetWidgetToFocus(DatCanvasWidget->TakeWidget());Mode.SetHideCursorDuringCapture(false);Mode.SetLockMouseToViewportBehavior(DesktopMouseLockMode());SetInputMode(Mode);bShowMouseCursor=true;
}

bool AACEPlayerController::TryFastBuffTap()
{
    if (!GetPawn() || IsWorldTransitionActive() || bJumpAirborne || bJumpCharging
        || bServerMoveToActive || bServerTurnActive || bAwaitingUseDone || ApproachUseSendCount > 0)
        return false;
    auto* C = Client ? Client.Get() : GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if (!C || !C->GetSession() || C->GetSession()->GetState() != EACESessionState::InWorld
        || C->GetSession()->GetCurrentStance() != ACEMotion::StanceMagic)
        return false;

    // Retail CommandInterpreter::HandleKeyboardCommand applies and reports each
    // key edge immediately. A press/release in the same update can replace the
    // cast substate without a physics step between them. Never hold Backward
    // until a later frame/result; that made native fast buffs translate the pawn.
    if (bHavePredictedPose) C->SetReportedPosition(PredictedPose);
    C->SendMovementEx(-1.f, 0.f, 0.f, bRunning, false, true);
    if (auto* Appearance = GetPawn()->FindComponentByClass<UACECharacterAppearanceComponent>())
        Appearance->InterruptCastWithMovement();
    C->SendMovementEx(0.f, 0.f, 0.f, bRunning, false, true);
    return true;
}

void AACEPlayerController::QueuePluginJump(float Charge,float Heading,float Forward,float Strafe,bool Walk)
{
    if(bPluginJumpQueued||bJumpAirborne||bJumpCharging||!FMath::IsFinite(Heading))return;
    bPluginJumpQueued=true;PluginJumpCharge=FMath::Clamp(Charge,0.f,1.f);PluginJumpHeading=Heading;PluginJumpForward=FMath::Clamp(Forward,-1.f,1.f);PluginJumpStrafe=FMath::Clamp(Strafe,-1.f,1.f);bPluginWalkJump=Walk;PluginJumpStarted=0;
}
void AACEPlayerController::CancelPluginJump()
{
    if(bPluginJumpQueued&&PluginJumpStarted){bJumpCharging=false;bStandingJumpLocked=false;JumpChargeExtent=0;}
    bPluginJumpQueued=false;PluginJumpStarted=0;
}
void AACEPlayerController::SetPluginAttackPower(float Power)
{
    if(DatGameplayBinder)DatGameplayBinder->SetRequestedAttackPower(Power);
}

void AACEPlayerController::ShowPluginAttack(int32 Target, uint32 Height, float Power)
{
    if (DatGameplayBinder) DatGameplayBinder->ShowPluginAttack(Target, Height, Power);
}
